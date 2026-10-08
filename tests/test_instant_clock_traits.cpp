// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <reloco/lifetime.hpp>
#include <structo/hw/instant_clock_traits.hpp>

using structo::hw::cycles;
using structo::hw::kernel_monotonic_clock_tag;
using structo::hw::kernel_realtime_clock_tag;
using structo::hw::time_manager;
using structo::hw::time_source_capabilities;
using structo::hw::time_source_ref;
using structo::hw::vdso_clock_source;
using reloco::duration;
using reloco::error;
using reloco::result;

namespace {

// A fully scriptable free-running counter backend, mirroring test_time_manager.cpp's fake_backend.
struct fake_backend {
  std::uint64_t value = 0;
  std::uint64_t clock_hz = 1'000'000'000ULL;
};

} // namespace

template <> struct structo::hw::time_source_traits<fake_backend> {
  static result<cycles> try_now(fake_backend &b) noexcept { return cycles{b.value}; }
  static time_source_capabilities capabilities(fake_backend &b) noexcept {
    time_source_capabilities caps;
    caps.clock_hz = b.clock_hz;
    caps.max_value = cycles{UINT64_MAX};
    caps.is_monotonic = true;
    caps.is_per_cpu = false;
    return caps;
  }
};

namespace {

// --------------------------------------------------------------------
// kernel_time_manager(): the one definition this test binary supplies, reading from a
// test-settable slot so each test can point it at its own fixture's time_manager (or leave it
// unset, for the not-yet-started path).
// --------------------------------------------------------------------
time_manager *&kernel_time_manager_slot() {
  static time_manager *slot = nullptr;
  return slot;
}

} // namespace

namespace structo::hw {
time_manager &kernel_time_manager() noexcept { return *kernel_time_manager_slot(); }
} // namespace structo::hw

// A test-local failure policy tag, kept separate from kernel_monotonic_clock_tag/kernel_realtime_clock_tag
// so this file can exercise time_manager_clock_failure_policy's undefined-by-default contract without
// colliding with whatever any other translation unit might specialize those two for.
namespace {
struct test_spin_tag {};
} // namespace

// This test binary acts as the "kernel" for the real kernel_monotonic_clock_tag/kernel_realtime_clock_tag
// specializations exercised below, so it must supply their failure policies too (time_manager_clock_failure_policy
// is left undefined by design until a kernel opts in -- see instant_clock_traits.hpp's file-level docs).
template <> struct structo::hw::time_manager_clock_failure_policy<kernel_monotonic_clock_tag>
    : structo::hw::trap_time_failure_policy<kernel_monotonic_clock_tag> {};
template <> struct structo::hw::time_manager_clock_failure_policy<kernel_realtime_clock_tag>
    : structo::hw::trap_time_failure_policy<kernel_realtime_clock_tag> {};

namespace {

class InstantClockTraitsTest : public ::testing::Test {
protected:
  void SetUp() override { kernel_time_manager_slot() = nullptr; }
  void TearDown() override { kernel_time_manager_slot() = nullptr; }

  fake_backend backend_;
  time_source_ref ref_{backend_};
};

} // namespace

#if !defined(NDEBUG) || defined(RELOCO_ENABLE_ASSERTS)
TEST_F(InstantClockTraitsTest, TrapTimeFailurePolicyTrapsOnRecover) {
  struct isolated_tag {}; // dedicated tag so this trap can't be confused with another test's policy
  using policy = structo::hw::trap_time_failure_policy<isolated_tag>;

  time_manager dummy_mgr; // never dereferenced by recover(); only passed through
  // gtest's death-test macro expands to libc fprintf calls outside our control.
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  EXPECT_DEATH({ (void)policy::recover(dummy_mgr, error::not_initialized); }, "");
  RELOCO_END_UNSAFE_BUFFER_USAGE
}
#endif

TEST_F(InstantClockTraitsTest, MonotonicClockTraitsReadsThroughKernelTimeManager) {
  auto mgr = time_manager::try_create(ref_, vdso_clock_source::x86_tsc);
  ASSERT_TRUE(mgr.has_value());
  ASSERT_TRUE(mgr->start());
  kernel_time_manager_slot() = &mgr.value();

  backend_.value = 2'000'000'000ULL; // +2s
  ASSERT_TRUE(mgr->resync());

  auto now = reloco::instant_clock_traits<kernel_monotonic_clock_tag>::now();
  EXPECT_EQ(now.as_secs(), 2u);
}

TEST_F(InstantClockTraitsTest, RealtimeClockTraitsReadsThroughKernelTimeManager) {
  auto mgr = time_manager::try_create(ref_, vdso_clock_source::x86_tsc);
  ASSERT_TRUE(mgr.has_value());
  ASSERT_TRUE(mgr->start());
  ASSERT_TRUE(mgr->set_realtime(duration::from_secs(1'700'000'000ULL)));
  kernel_time_manager_slot() = &mgr.value();

  auto now = reloco::instant_clock_traits<kernel_realtime_clock_tag>::now();
  EXPECT_EQ(now.as_secs(), 1'700'000'000ULL);
}

#if !defined(NDEBUG) || defined(RELOCO_ENABLE_ASSERTS)
TEST_F(InstantClockTraitsTest, TrapTimeFailurePolicyIsAppliedOnFailure) {
  auto mgr = time_manager::try_create(ref_, vdso_clock_source::x86_tsc);
  ASSERT_TRUE(mgr.has_value());
  // start() deliberately not called: try_monotonic_now() fails with error::not_initialized.
  kernel_time_manager_slot() = &mgr.value();

  // gtest's death-test macro expands to libc fprintf calls outside our control.
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  EXPECT_DEATH({ (void)reloco::instant_clock_traits<kernel_monotonic_clock_tag>::now(); }, "");
  RELOCO_END_UNSAFE_BUFFER_USAGE
}
#endif

TEST(TimeManagerClockFailurePolicyTest, UndefinedPolicyIsACompileErrorNotASilentDefault) {
  // There is no runtime behavior to assert here -- time_manager_clock_failure_policy<Tag> is left
  // undefined (an incomplete type) for any Tag that hasn't specialized it, by design (see
  // instant_clock_traits.hpp's file-level docs). This test exists purely to document that contract
  // alongside the two policies actually exercised above.
  SUCCEED();
}

TEST_F(InstantClockTraitsTest, RetrySpinTimerPolicyRetriesUntilTimeManagerSucceeds) {
  auto mgr = time_manager::try_create(ref_, vdso_clock_source::x86_tsc);
  ASSERT_TRUE(mgr.has_value());
  ASSERT_TRUE(mgr->start());

  // Exercise the ready-made retry_spin_timer_policy directly, rather than through
  // reloco::instant_clock_traits (which is only specialized for kernel_monotonic_clock_tag/
  // kernel_realtime_clock_tag in this header) -- confirms its recover() contract works. mgr is
  // already started/resynced, so the very first retry succeeds; this cannot hang the suite.
  backend_.value = 3'000'000'000ULL;
  ASSERT_TRUE(mgr->resync());

  auto recovered = structo::hw::retry_spin_timer_policy<test_spin_tag>::recover(mgr.value(), error::not_initialized);
  EXPECT_EQ(recovered.as_secs(), 3u);
}
