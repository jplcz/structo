// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hypervisor/hypercall_dispatcher.hpp>

#include <cstdint>

using structo::hypervisor::hypercall_dispatcher;

namespace {

struct fake_hypercall_context {
  std::uint64_t last_call_number = 0;
  int invoke_count = 0;
};

struct fake_hypercall_handlers {
  using context_type = fake_hypercall_context;

  static inline constexpr std::uint64_t unsupported_call = 0xDEAD;

  static reloco::result<void> invoke(std::uint64_t call_number, context_type &ctx) noexcept {
    ++ctx.invoke_count;
    ctx.last_call_number = call_number;
    if (call_number == unsupported_call) {
      return reloco::unexpected(reloco::error::unsupported_operation);
    }
    return {};
  }
};

using test_dispatcher = hypercall_dispatcher<fake_hypercall_handlers, 8>;

} // namespace

class HypercallDispatcherTest : public ::testing::Test {};

TEST_F(HypercallDispatcherTest, DispatchForwardsCallNumberAndContextToHandlers) {
  fake_hypercall_context ctx;
  const reloco::result<void> outcome = test_dispatcher::dispatch(3, ctx);
  EXPECT_TRUE(outcome.has_value());
  EXPECT_EQ(ctx.last_call_number, 3u);
  EXPECT_EQ(ctx.invoke_count, 1);
}

TEST_F(HypercallDispatcherTest, DispatchPropagatesHandlersError) {
  fake_hypercall_context ctx;
  const reloco::result<void> outcome = test_dispatcher::dispatch(fake_hypercall_handlers::unsupported_call, ctx);
  EXPECT_FALSE(outcome.has_value());
  EXPECT_EQ(outcome.error(), reloco::error::unsupported_operation);
}

TEST_F(HypercallDispatcherTest, CountTracksInRangeCallNumbersIndependently) {
  fake_hypercall_context ctx;
  (void)test_dispatcher::dispatch(2, ctx);
  (void)test_dispatcher::dispatch(2, ctx);
  (void)test_dispatcher::dispatch(5, ctx);

  EXPECT_GE(test_dispatcher::count(2), 2u);
  EXPECT_GE(test_dispatcher::count(5), 1u);
}

TEST_F(HypercallDispatcherTest, DispatchStillForwardsOutOfRangeCallNumberWithoutCounting) {
  fake_hypercall_context ctx;
  // Larger than max_calls, and larger than std::size_t on no supported
  // target -- exercises the explicit std::uint64_t->std::size_t
  // narrowing guard in dispatch()/count()/try_count().
  const reloco::result<void> outcome = test_dispatcher::dispatch(0xFFFFFFFFFFFFULL, ctx);
  EXPECT_TRUE(outcome.has_value());
  EXPECT_EQ(ctx.last_call_number, 0xFFFFFFFFFFFFULL);
}

TEST_F(HypercallDispatcherTest, TryCountFailsForOutOfRangeCallNumber) {
  const reloco::result<std::uint64_t> outcome = test_dispatcher::try_count(0xFFFFFFFFFFFFULL);
  EXPECT_FALSE(outcome.has_value());
  EXPECT_EQ(outcome.error(), reloco::error::out_of_bounds);
}

TEST_F(HypercallDispatcherTest, TryCountSucceedsForInRangeCallNumber) {
  fake_hypercall_context ctx;
  (void)test_dispatcher::dispatch(1, ctx);
  const reloco::result<std::uint64_t> outcome = test_dispatcher::try_count(1);
  ASSERT_TRUE(outcome.has_value());
  EXPECT_GE(outcome.value(), 1u);
}
