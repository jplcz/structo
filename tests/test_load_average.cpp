// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/load_average.hpp>

#include <cstdint>

namespace {

using namespace structo;

std::uint32_t abs_diff(std::uint32_t a, std::uint32_t b) noexcept { return a > b ? a - b : b - a; }

} // namespace

TEST(LoadAverageTest, DefaultConstructedValueIsZero) {
  load_average<> avg(reloco::duration::from_secs(60));
  EXPECT_EQ(avg.value().raw(), 0U);
}

TEST(LoadAverageTest, ZeroElapsedLeavesValueUnchanged) {
  load_average<> avg(reloco::duration::from_secs(60));
  avg.sample(reloco::duration::from_secs(60), 4U);
  auto before = avg.value().raw();
  avg.sample(reloco::duration::from_secs(0), 7U); // decay == 1 (exp(0)), active_count ignored
  EXPECT_EQ(avg.value().raw(), before);
}

TEST(LoadAverageTest, OneTimeConstantElapsedDecaysByInverseE) {
  // decay after exactly one time constant is always 1/e, matching exp()'s own definition
  // (see reloco's test_fixed_point.cpp's ExpOfNegativeOneApproximatesInverseE: raw ~753 at Q21.11).
  load_average<> seeded(reloco::duration::from_secs(60));
  seeded.sample(reloco::duration::from_secs(1'000'000), 100U); // >> time constant -> decay clamped to 0 -> value == 100
  EXPECT_EQ(seeded.value().to_int(), 100U);

  seeded.sample(reloco::duration::from_secs(60), 0U); // exactly one time constant, dropping to 0 active
  // new_value == 100 * (1/e) + 0 * (1 - 1/e) == 100/e ~= 36.7879
  EXPECT_LE(abs_diff(seeded.value().raw(), 75270U), 400U); // 753 (1/e at Q21.11) * 100, +/- tolerance
}

TEST(LoadAverageTest, FarExceedingTimeConstantClampsToActiveCountExactly) {
  load_average<> avg(reloco::duration::from_secs(60));
  avg.sample(reloco::duration::from_secs(1), 5U);
  avg.sample(reloco::duration::from_secs(1'000'000), 9U); // a huge elapsed, clamping decay to 0
  EXPECT_EQ(avg.value().to_int(), 9U);
}

TEST(LoadAverageTest, RepeatedSamplingConvergesTowardSteadyActiveCount) {
  load_average<> avg(reloco::duration::from_secs(60));
  for (int i = 0; i < 200; ++i)
    avg.sample(reloco::duration::from_secs(10), 3U);
  // Converges toward exactly 3 * one_raw, but repeated quantized multiply/divide each step accumulates a
  // small rounding drift -- not `to_int() == 3` exactly, see fixed_point.hpp's own silent-truncation caveats.
  EXPECT_LE(abs_diff(avg.value().raw(), 3U * decltype(avg)::fixed::one_raw), 16U);
}

TEST(LoadAverageTest, ConstexprUsable) {
  constexpr auto make = []() constexpr {
    load_average<> avg(reloco::duration::from_secs(60));
    avg.sample(reloco::duration::from_secs(3600), 5U);
    return avg.value();
  };
  constexpr auto v = make();
  static_assert(v.to_int() == 5U);
  SUCCEED();
}

TEST(UnixLoadAverageTest, DefaultConstructedAllZero) {
  unix_load_average<> avg;
  EXPECT_EQ(avg.one_minute().raw(), 0U);
  EXPECT_EQ(avg.five_minute().raw(), 0U);
  EXPECT_EQ(avg.fifteen_minute().raw(), 0U);
}

TEST(UnixLoadAverageTest, SampleUpdatesAllThreeWindows) {
  unix_load_average<> avg;
  avg.sample(reloco::duration::from_secs(1'000'000), 4U); // clamp all three to exactly 4
  EXPECT_EQ(avg.one_minute().to_int(), 4U);
  EXPECT_EQ(avg.five_minute().to_int(), 4U);
  EXPECT_EQ(avg.fifteen_minute().to_int(), 4U);
}

TEST(UnixLoadAverageTest, ShorterWindowDecaysFasterThanLonger) {
  unix_load_average<> avg;
  avg.sample(reloco::duration::from_secs(1'000'000), 10U); // seed all three at 10
  avg.sample(reloco::duration::from_secs(60), 0U);         // one time constant for the 1-minute window only
  // The 1-minute window has decayed a full time constant (down toward ~10/e); the 5- and 15-minute windows have
  // barely moved (60s is only 1/5 and 1/15 of their own time constants) -- so ordering is one < five < fifteen.
  EXPECT_LT(avg.one_minute().raw(), avg.five_minute().raw());
  EXPECT_LT(avg.five_minute().raw(), avg.fifteen_minute().raw());
}
