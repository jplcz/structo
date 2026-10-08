// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/arch/cpu_mask.hpp>
#include <structo/arch/ipi_dispatcher.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <reloco/lifetime.hpp>

// Test fixtures index raw buffers freely; bounds are checked by the assertions.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

using structo::arch::cpu_mask;
using structo::arch::ipi_dispatcher;
using structo::arch::ipi_percpu_state;
using structo::arch::physical_cpu_tag;

namespace {

constexpr std::size_t kMaxCpus = 8;
using mask_type = cpu_mask<physical_cpu_tag, kMaxCpus>;

/** @brief Records every `Handlers::invoke()` call; cleared in
 * `IpiDispatcherTest::SetUp()` before each test. Shared across the
 * per-test dispatcher instantiations below (each keyed by its own local
 * `PerCpu` tag, so their *static* IPI state never aliases), so clearing
 * it per test is enough to keep tests independent. */
std::vector<std::pair<std::uint32_t, std::size_t>> g_call_log; // NOLINT

struct recording_handlers {
  static void invoke(std::uint32_t reason, std::size_t executing_cpu) noexcept {
    g_call_log.emplace_back(reason, executing_cpu);
  }
};

/** @brief Records every `Sender::send_ipi()` call; cleared alongside
 * `g_call_log`. */
struct sent_ipi {
  std::uint32_t ipi_id;
  mask_type targets;
};
std::vector<sent_ipi> g_sent_ipis; // NOLINT

struct recording_sender {
  static reloco::result<void> send_ipi(reloco::span<const std::uint64_t> target_mask, std::uint32_t ipi_id) noexcept {
    mask_type targets;
    for (std::size_t cpu = 0; cpu < kMaxCpus; ++cpu) {
      if ((target_mask[cpu / 64] & (std::uint64_t{1} << (cpu % 64))) != 0) {
        targets.set(cpu);
      }
    }
    g_sent_ipis.push_back({ipi_id, targets});
    return reloco::result<void>{};
  }
};

/** @brief Per-CPU state storage for one `ipi_dispatcher` instantiation,
 * keyed by @p Tag so every `TEST_F` below gets its own, independent
 * static multicast queue/lock/generation counter (those live on
 * `ipi_dispatcher` itself, keyed by the full template arg list) as well
 * as its own per-CPU array. */
template <typename Tag, std::size_t MaxReasons> struct per_cpu_provider {
  static inline ipi_percpu_state<mask_type, MaxReasons> states[kMaxCpus]{}; // NOLINT
  static ipi_percpu_state<mask_type, MaxReasons> *get(std::size_t cpu) noexcept { return &states[cpu]; }
};

class IpiDispatcherTest : public ::testing::Test {
protected:
  void SetUp() override {
    g_call_log.clear();
    g_sent_ipis.clear();
  }
};

} // namespace

TEST_F(IpiDispatcherTest, NotifyAndPollReasonsInvokesHandlerOnce) {
  struct tag {};
  using dispatcher = ipi_dispatcher<per_cpu_provider<tag, 2>, recording_handlers, recording_sender, mask_type, 2>;

  dispatcher::on_cpu_online(1, mask_type{});
  ASSERT_TRUE(dispatcher::notify(mask_type::single(1), 0).has_value());

  dispatcher::poll_reasons(1);
  EXPECT_EQ(g_call_log.size(), 1u);
  EXPECT_EQ(g_call_log[0], (std::pair<std::uint32_t, std::size_t>{0u, 1u}));

  // Nothing new happened since the last poll: a second poll is a no-op.
  dispatcher::poll_reasons(1);
  EXPECT_EQ(g_call_log.size(), 1u);
}

TEST_F(IpiDispatcherTest, MultipleNotifiesCoalesceIntoOneInvoke) {
  struct tag {};
  using dispatcher = ipi_dispatcher<per_cpu_provider<tag, 2>, recording_handlers, recording_sender, mask_type, 2>;

  dispatcher::on_cpu_online(2, mask_type{});
  ASSERT_TRUE(dispatcher::notify(mask_type::single(2), 0).has_value());
  ASSERT_TRUE(dispatcher::notify(mask_type::single(2), 0).has_value());
  ASSERT_TRUE(dispatcher::notify(mask_type::single(2), 0).has_value());

  dispatcher::poll_reasons(2);
  EXPECT_EQ(g_call_log.size(), 1u);
}

TEST_F(IpiDispatcherTest, NotifyMultipleReasonsInvokesHandlerPerReason) {
  struct tag {};
  using dispatcher = ipi_dispatcher<per_cpu_provider<tag, 3>, recording_handlers, recording_sender, mask_type, 3>;

  dispatcher::on_cpu_online(0, mask_type{});
  ASSERT_TRUE(dispatcher::notify(mask_type::single(0), 2).has_value());
  ASSERT_TRUE(dispatcher::notify(mask_type::single(0), 0).has_value());

  dispatcher::poll_reasons(0);
  ASSERT_EQ(g_call_log.size(), 2u);
  EXPECT_EQ(g_call_log[0], (std::pair<std::uint32_t, std::size_t>{0u, 0u}));
  EXPECT_EQ(g_call_log[1], (std::pair<std::uint32_t, std::size_t>{2u, 0u}));
}

TEST_F(IpiDispatcherTest, OnCpuOnlineResetsReasonStateSoStalePollsAreNoOps) {
  struct tag {};
  using dispatcher = ipi_dispatcher<per_cpu_provider<tag, 1>, recording_handlers, recording_sender, mask_type, 1>;

  ASSERT_TRUE(dispatcher::notify(mask_type::single(3), 0).has_value());
  dispatcher::on_cpu_online(3, mask_type{});

  dispatcher::poll_reasons(3);
  EXPECT_TRUE(g_call_log.empty());
}

TEST_F(IpiDispatcherTest, PushMessageUnicastRunsCallbackAndCompletes) {
  struct tag {};
  using dispatcher = ipi_dispatcher<per_cpu_provider<tag, 1>, recording_handlers, recording_sender, mask_type, 1>;

  dispatcher::on_cpu_online(4, mask_type{});
  dispatcher::message_type msg;
  std::vector<std::size_t> ran_on;
  auto cb = [&ran_on](std::size_t cpu) noexcept { ran_on.push_back(cpu); };
  msg.bind(cb);

  dispatcher::push_message(mask_type::single(4), msg);
  EXPECT_FALSE(msg.is_done());

  dispatcher::poll_messages(4);
  EXPECT_TRUE(msg.is_done());
  ASSERT_EQ(ran_on.size(), 1u);
  EXPECT_EQ(ran_on[0], 4u);
}

TEST_F(IpiDispatcherTest, PushMessageMulticastRunsOnEachTargetAndCompletesWhenAllClaim) {
  struct tag {};
  using dispatcher = ipi_dispatcher<per_cpu_provider<tag, 1>, recording_handlers, recording_sender, mask_type, 1>;

  dispatcher::on_cpu_online(1, mask_type{});
  dispatcher::on_cpu_online(2, mask_type{});
  dispatcher::on_cpu_online(3, mask_type{});

  mask_type targets;
  targets.set(1);
  targets.set(2);
  targets.set(3);

  dispatcher::message_type msg;
  std::vector<std::size_t> ran_on;
  auto cb = [&ran_on](std::size_t cpu) noexcept { ran_on.push_back(cpu); };
  msg.bind(cb);
  dispatcher::push_message(targets, msg);

  dispatcher::poll_messages(1);
  EXPECT_FALSE(msg.is_done());
  dispatcher::poll_messages(2);
  EXPECT_FALSE(msg.is_done());
  dispatcher::poll_messages(3);
  EXPECT_TRUE(msg.is_done());

  ASSERT_EQ(ran_on.size(), 3u);
  EXPECT_EQ(ran_on[0], 1u);
  EXPECT_EQ(ran_on[1], 2u);
  EXPECT_EQ(ran_on[2], 3u);

  // Re-polling an uninvolved CPU (or any target again) is a cheap,
  // lock-free no-op: the shared generation counter hasn't moved.
  dispatcher::poll_messages(1);
  EXPECT_EQ(ran_on.size(), 3u);
}

TEST_F(IpiDispatcherTest, EnqueueSendsHardwareIpiCoveringEveryTarget) {
  struct tag {};
  using dispatcher = ipi_dispatcher<per_cpu_provider<tag, 1>, recording_handlers, recording_sender, mask_type, 1>;

  dispatcher::on_cpu_online(5, mask_type{});
  dispatcher::on_cpu_online(6, mask_type{});

  mask_type targets;
  targets.set(5);
  targets.set(6);

  dispatcher::message_type msg;
  auto no_op = [](std::size_t) noexcept {};
  msg.bind(no_op);
  ASSERT_TRUE(dispatcher::enqueue(targets, msg, 42).has_value());

  ASSERT_EQ(g_sent_ipis.size(), 1u);
  EXPECT_EQ(g_sent_ipis[0].ipi_id, 42u);
  EXPECT_EQ(g_sent_ipis[0].targets, targets);

  dispatcher::poll_messages(5);
  dispatcher::poll_messages(6);
  EXPECT_TRUE(msg.is_done());
}

TEST_F(IpiDispatcherTest, CallSyncLocalOnlyRunsInlineWithoutTouchingQueue) {
  struct tag {};
  using dispatcher = ipi_dispatcher<per_cpu_provider<tag, 1>, recording_handlers, recording_sender, mask_type, 1>;

  dispatcher::on_cpu_online(0, mask_type{});
  dispatcher::message_type msg;
  bool ran = false;
  auto result = dispatcher::call_sync(0, mask_type::single(0), msg, 7, [&ran](std::size_t cpu) noexcept {
    ran = true;
    EXPECT_EQ(cpu, 0u);
  });

  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(ran);
  EXPECT_TRUE(g_sent_ipis.empty()); // purely local: no remote targets, no hardware IPI needed
  EXPECT_TRUE(msg.is_done());       // never pushed anywhere, so its ref-count was never touched
}

TEST_F(IpiDispatcherTest, OnCpuOfflineAbortsUnicastMessageWithoutRunningCallback) {
  struct tag {};
  using dispatcher = ipi_dispatcher<per_cpu_provider<tag, 1>, recording_handlers, recording_sender, mask_type, 1>;

  dispatcher::on_cpu_online(4, mask_type{});
  dispatcher::message_type msg;
  bool ran = false;
  auto cb = [&ran](std::size_t) noexcept { ran = true; };
  msg.bind(cb);
  dispatcher::push_message(mask_type::single(4), msg);

  dispatcher::on_cpu_offline(4, mask_type{});

  EXPECT_TRUE(msg.is_done());
  EXPECT_FALSE(ran);
}

TEST_F(IpiDispatcherTest, OnCpuOfflineAbortsOnlyThatCpusClaimInMulticast) {
  struct tag {};
  using dispatcher = ipi_dispatcher<per_cpu_provider<tag, 1>, recording_handlers, recording_sender, mask_type, 1>;

  dispatcher::on_cpu_online(1, mask_type{});
  dispatcher::on_cpu_online(2, mask_type{});

  mask_type targets;
  targets.set(1);
  targets.set(2);

  dispatcher::message_type msg;
  std::vector<std::size_t> ran_on;
  auto cb = [&ran_on](std::size_t cpu) noexcept { ran_on.push_back(cpu); };
  msg.bind(cb);
  dispatcher::push_message(targets, msg);

  dispatcher::on_cpu_offline(1, mask_type{});
  EXPECT_FALSE(msg.is_done()); // CPU 2 hasn't claimed its share yet
  EXPECT_TRUE(ran_on.empty());

  dispatcher::poll_messages(2);
  EXPECT_TRUE(msg.is_done());
  ASSERT_EQ(ran_on.size(), 1u);
  EXPECT_EQ(ran_on[0], 2u);
}

TEST_F(IpiDispatcherTest, PollRunsBothReasonsAndMessagesInOneCall) {
  struct tag {};
  using dispatcher = ipi_dispatcher<per_cpu_provider<tag, 1>, recording_handlers, recording_sender, mask_type, 1>;

  dispatcher::on_cpu_online(0, mask_type{});
  ASSERT_TRUE(dispatcher::notify(mask_type::single(0), 0).has_value());

  dispatcher::message_type msg;
  bool ran = false;
  auto cb = [&ran](std::size_t) noexcept { ran = true; };
  msg.bind(cb);
  dispatcher::push_message(mask_type::single(0), msg);

  dispatcher::poll(0);

  EXPECT_EQ(g_call_log.size(), 1u);
  EXPECT_TRUE(ran);
  EXPECT_TRUE(msg.is_done());
}

TEST_F(IpiDispatcherTest, MessageWaitReturnsImmediatelyWhenAlreadyDone) {
  struct tag {};
  using dispatcher = ipi_dispatcher<per_cpu_provider<tag, 1>, recording_handlers, recording_sender, mask_type, 1>;

  dispatcher::on_cpu_online(0, mask_type{});
  dispatcher::message_type msg;
  auto no_op = [](std::size_t) noexcept {};
  msg.bind(no_op);
  dispatcher::push_message(mask_type::single(0), msg);
  dispatcher::poll_messages(0);

  ASSERT_TRUE(msg.is_done());
  msg.wait(); // must not spin: is_done() is already true
}

TEST_F(IpiDispatcherTest, SendIssuesHardwareIpiWithGivenMaskAndId) {
  struct tag {};
  using dispatcher = ipi_dispatcher<per_cpu_provider<tag, 1>, recording_handlers, recording_sender, mask_type, 1>;

  mask_type targets;
  targets.set(0);
  targets.set(7);
  ASSERT_TRUE(dispatcher::send(targets, 99).has_value());

  ASSERT_EQ(g_sent_ipis.size(), 1u);
  EXPECT_EQ(g_sent_ipis[0].ipi_id, 99u);
  EXPECT_EQ(g_sent_ipis[0].targets, targets);
}

RELOCO_END_UNSAFE_BUFFER_USAGE