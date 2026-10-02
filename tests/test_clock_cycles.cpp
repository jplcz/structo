// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/clock_cycles.hpp>

#include <limits>

namespace {

using namespace structo;
using namespace structo::hw;

TEST(ClockCyclesTest, DefaultIsZero) {
  cycles c;
  EXPECT_EQ(c.raw(), 0u);
}

TEST(ClockCyclesTest, ComparisonOperators) {
  cycles a{10};
  cycles b{20};
  cycles a_again{10};

  EXPECT_TRUE(a == a_again);
  EXPECT_FALSE(a == b);
  EXPECT_TRUE(a != b);
  EXPECT_FALSE(a != a_again);
  EXPECT_TRUE(a < b);
  EXPECT_TRUE(a <= b);
  EXPECT_TRUE(a <= a_again);
  EXPECT_TRUE(b > a);
  EXPECT_TRUE(b >= a);
  EXPECT_TRUE(a >= a_again);
}

TEST(ClockCyclesTest, CheckedArithmeticSucceedsAndFails) {
  cycles max_cycles{std::numeric_limits<std::uint64_t>::max()};

  auto ok = cycles{100}.checked_add(cycles{50});
  ASSERT_TRUE(ok.has_value());
  EXPECT_EQ(ok.value().raw(), 150u);

  auto overflow = max_cycles.checked_add(cycles{1});
  ASSERT_FALSE(overflow.has_value());
  EXPECT_EQ(overflow.error(), error::integer_overflow);

  auto sub_ok = cycles{100}.checked_sub(cycles{40});
  ASSERT_TRUE(sub_ok.has_value());
  EXPECT_EQ(sub_ok.value().raw(), 60u);

  auto underflow = cycles{10}.checked_sub(cycles{20});
  ASSERT_FALSE(underflow.has_value());
  EXPECT_EQ(underflow.error(), error::integer_overflow);

  auto mul_ok = cycles{100}.checked_mul(3);
  ASSERT_TRUE(mul_ok.has_value());
  EXPECT_EQ(mul_ok.value().raw(), 300u);

  auto mul_overflow = max_cycles.checked_mul(2);
  ASSERT_FALSE(mul_overflow.has_value());
  EXPECT_EQ(mul_overflow.error(), error::integer_overflow);
}

TEST(ClockCyclesTest, WrappingArithmeticNeverFails) {
  cycles max_cycles{std::numeric_limits<std::uint64_t>::max()};

  EXPECT_EQ(max_cycles.wrapping_add(cycles{1}).raw(), 0u);
  EXPECT_EQ(cycles{0}.wrapping_sub(cycles{1}).raw(), std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(cycles{10}.wrapping_mul(std::numeric_limits<std::uint64_t>::max()).raw(),
            static_cast<std::uint64_t>(10 * std::numeric_limits<std::uint64_t>::max()));
}

TEST(ClockCyclesTest, SaturatingArithmeticClampsInsteadOfFailing) {
  cycles max_cycles{std::numeric_limits<std::uint64_t>::max()};

  EXPECT_EQ(max_cycles.saturating_add(cycles{1}).raw(), std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(cycles{0}.saturating_sub(cycles{1}).raw(), 0u);
  EXPECT_EQ(max_cycles.saturating_mul(2).raw(), std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(cycles{100}.saturating_add(cycles{50}).raw(), 150u);
}

TEST(ClockCyclesTest, DurationToCyclesAndBackRoundTrips) {
  constexpr std::uint64_t clock_hz = 1'000'000; // 1 MHz

  auto c = checked_duration_to_cycles(duration::from_micros(5), clock_hz);
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c.value().raw(), 5u);

  auto back = checked_cycles_to_duration(c.value(), clock_hz);
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(back.value().as_nanos(), duration::from_micros(5).as_nanos());
}

TEST(ClockCyclesTest, DurationToCyclesFloorsSubCycleRemainder) {
  constexpr std::uint64_t clock_hz = 1'000; // 1 kHz -> 1ms per cycle

  auto c = checked_duration_to_cycles(duration::from_micros(500), clock_hz);
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c.value().raw(), 0u); // half a cycle floors to zero
}

TEST(ClockCyclesTest, ZeroClockHzIsInvalidArgument) {
  auto to_cycles = checked_duration_to_cycles(duration::from_secs(1), 0);
  ASSERT_FALSE(to_cycles.has_value());
  EXPECT_EQ(to_cycles.error(), error::invalid_argument);

  auto to_duration = checked_cycles_to_duration(cycles{1}, 0);
  ASSERT_FALSE(to_duration.has_value());
  EXPECT_EQ(to_duration.error(), error::invalid_argument);
}

TEST(ClockCyclesTest, DurationToCyclesReportsOverflow) {
  // A huge duration at a high clock frequency overflows the uint64_t
  // intermediate product.
  auto overflow =
      checked_duration_to_cycles(duration::from_secs(std::numeric_limits<std::uint64_t>::max()), 1'000'000'000);
  ASSERT_FALSE(overflow.has_value());
  EXPECT_EQ(overflow.error(), error::integer_overflow);
}

} // namespace
