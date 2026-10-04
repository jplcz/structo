// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hypervisor/vm_exit_dispatcher.hpp>

#include <cstdint>

using structo::hypervisor::vm_exit_dispatcher;

namespace {

struct fake_exit_context {
  int last_reason = -1;
  int invoke_count = 0;
};

struct fake_vm_exit_handlers {
  using context_type = fake_exit_context;

  static reloco::result<void> invoke(std::uint32_t reason, context_type &ctx) noexcept {
    ++ctx.invoke_count;
    ctx.last_reason = static_cast<int>(reason);
    if (reason == unsupported_reason) {
      return reloco::unexpected(reloco::error::unsupported_operation);
    }
    return {};
  }

  static inline constexpr std::uint32_t unsupported_reason = 999;
};

using test_dispatcher = vm_exit_dispatcher<fake_vm_exit_handlers, 8>;

} // namespace

class VmExitDispatcherTest : public ::testing::Test {};

TEST_F(VmExitDispatcherTest, DispatchForwardsReasonAndContextToHandlers) {
  fake_exit_context ctx;
  const reloco::result<void> outcome = test_dispatcher::dispatch(3, ctx);
  EXPECT_TRUE(outcome.has_value());
  EXPECT_EQ(ctx.last_reason, 3);
  EXPECT_EQ(ctx.invoke_count, 1);
}

TEST_F(VmExitDispatcherTest, DispatchPropagatesHandlersError) {
  fake_exit_context ctx;
  const reloco::result<void> outcome = test_dispatcher::dispatch(fake_vm_exit_handlers::unsupported_reason, ctx);
  EXPECT_FALSE(outcome.has_value());
  EXPECT_EQ(outcome.error(), reloco::error::unsupported_operation);
}

TEST_F(VmExitDispatcherTest, CountTracksInRangeReasonsIndependently) {
  fake_exit_context ctx;
  (void)test_dispatcher::dispatch(2, ctx);
  (void)test_dispatcher::dispatch(2, ctx);
  (void)test_dispatcher::dispatch(5, ctx);

  EXPECT_GE(test_dispatcher::count(2), 2u);
  EXPECT_GE(test_dispatcher::count(5), 1u);
}

TEST_F(VmExitDispatcherTest, DispatchStillForwardsOutOfRangeReasonWithoutCounting) {
  fake_exit_context ctx;
  const reloco::result<void> outcome = test_dispatcher::dispatch(100, ctx);
  EXPECT_TRUE(outcome.has_value());
  EXPECT_EQ(ctx.last_reason, 100);
}

TEST_F(VmExitDispatcherTest, TryCountFailsForOutOfRangeReason) {
  const reloco::result<std::uint64_t> outcome = test_dispatcher::try_count(100);
  EXPECT_FALSE(outcome.has_value());
  EXPECT_EQ(outcome.error(), reloco::error::out_of_bounds);
}

TEST_F(VmExitDispatcherTest, TryCountSucceedsForInRangeReason) {
  fake_exit_context ctx;
  (void)test_dispatcher::dispatch(1, ctx);
  const reloco::result<std::uint64_t> outcome = test_dispatcher::try_count(1);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_GE(outcome.value(), 1u);
}
