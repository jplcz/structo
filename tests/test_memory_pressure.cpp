// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <structo/memory_pressure.hpp>

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using namespace structo;

TEST(MemoryPressureWatermarks, DerivedFromTotalWithFloorAndSum) {
  constexpr auto wm = memory_watermarks::for_total(1'000'000, 5, 64);
  EXPECT_EQ(wm.min, 5000u);
  EXPECT_EQ(wm.low, 6250u);
  EXPECT_EQ(wm.high, 7500u);
  EXPECT_TRUE(wm.valid());
  EXPECT_EQ(memory_watermarks::for_total(100, 5, 64).min, 64u);
  const auto sum = wm + memory_watermarks::for_total(100, 5, 64);
  EXPECT_EQ(sum.min, 5064u);
  EXPECT_TRUE(sum.valid());
}

TEST(MemoryPressureLevels, ClassifiedByWatermarks) {
  const memory_watermarks wm{100, 125, 150};
  EXPECT_EQ(pressure_level_of(150, wm), pressure_level::none);
  EXPECT_EQ(pressure_level_of(149, wm), pressure_level::low);
  EXPECT_EQ(pressure_level_of(125, wm), pressure_level::low);
  EXPECT_EQ(pressure_level_of(124, wm), pressure_level::medium);
  EXPECT_EQ(pressure_level_of(100, wm), pressure_level::medium);
  EXPECT_EQ(pressure_level_of(99, wm), pressure_level::critical);
}

TEST(MemoryPressureUrgency, LinearAndOverflowSafe) {
  const memory_watermarks wm{100, 150, 200};
  EXPECT_EQ(pressure_urgency(200, wm), 0u);
  EXPECT_EQ(pressure_urgency(100, wm), 256u);
  EXPECT_EQ(pressure_urgency(150, wm), 128u);
  EXPECT_EQ(pressure_urgency(0, wm), 256u);
  const memory_watermarks big{0, 1ull << 62, 1ull << 63};
  EXPECT_EQ(pressure_urgency(1ull << 62, big), 128u);
  EXPECT_EQ(pressure_urgency(5, memory_watermarks{10, 10, 10}), 256u); // degenerate set
  EXPECT_EQ(effective_free_pages(100, 51), 125u);
}

TEST(MemoryPressureEfficiency, InefficiencyAndBlend) {
  EXPECT_EQ(reclaim_inefficiency(0, 0), 0u);
  EXPECT_EQ(reclaim_inefficiency(100, 100), 0u);
  EXPECT_EQ(reclaim_inefficiency(100, 0), 256u);
  EXPECT_EQ(reclaim_inefficiency(100, 75), 64u);
  EXPECT_EQ(reclaim_inefficiency(~0ull, 0), 256u);
  EXPECT_EQ(combine_pressure(0, 256), 0u);    // no watermark pressure: nothing to reclaim for
  EXPECT_EQ(combine_pressure(128, 0), 128u);  // efficient reclaim leaves it alone
  EXPECT_EQ(combine_pressure(128, 256), 256u);
  EXPECT_EQ(combine_pressure(128, 128), 192u);
}

TEST(MemoryPressureAverage, FastAttackSlowDecayReachesZero) {
  pressure_average avg;
  EXPECT_EQ(avg.update(200), 200u);
  EXPECT_EQ(avg.update(0), 150u);
  EXPECT_GT(avg.update(0), 0u);
  for (int i = 0; i < 100; ++i)
    avg.update(0);
  EXPECT_EQ(avg.value(), 0u);
  EXPECT_EQ(avg.update(999), 256u); // clamped
}

TEST(PageDaemonBackoff, IntervalFollowsUrgencyAndFutileRoundsDouble) {
  using reloco::duration;
  const page_daemon_backoff_config cfg{duration::from_millis(10), duration::from_secs(1), 3};
  EXPECT_EQ(page_daemon_interval(cfg, 0).as_millis(), 1000u);
  EXPECT_EQ(page_daemon_interval(cfg, 256).as_millis(), 10u);
  EXPECT_EQ(page_daemon_interval(cfg, 128).as_millis(), 505u);

  page_daemon_backoff b{cfg};
  EXPECT_EQ(b.next_interval(256, 50, 10).as_millis(), 10u); // progress
  EXPECT_EQ(b.next_interval(256, 50, 0).as_millis(), 20u);  // futile x1
  EXPECT_EQ(b.next_interval(256, 50, 0).as_millis(), 40u);
  EXPECT_EQ(b.next_interval(256, 50, 0).as_millis(), 80u);
  EXPECT_EQ(b.next_interval(256, 50, 0).as_millis(), 80u);  // capped at max_futile_shift
  EXPECT_EQ(b.futile_rounds(), 3u);
  EXPECT_EQ(b.next_interval(256, 0, 0).as_millis(), 10u);   // idle round clears the streak
  EXPECT_EQ(b.next_interval(0, 50, 0).as_millis(), 1000u); // saturates at max_interval
}

} // namespace
