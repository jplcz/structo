// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/cpu_load.hpp>

namespace {

using namespace structo;

} // namespace

TEST(CpuLoadTest, DefaultConstructedIsZero) {
  cpu_load<> load;
  EXPECT_EQ(load.current(), 0U);
  EXPECT_EQ(load.average().raw(), 0U);
}

TEST(CpuLoadTest, SampleRecordsExactCurrentCount) {
  cpu_load<> load(reloco::duration::from_secs(60));
  load.sample(reloco::duration::from_secs(1), 5U);
  EXPECT_EQ(load.current(), 5U);
  load.sample(reloco::duration::from_secs(1), 0U);
  EXPECT_EQ(load.current(), 0U);
}

TEST(CpuLoadTest, AverageDecaysLikeLoadAverage) {
  cpu_load<> load(reloco::duration::from_secs(60));
  load.sample(reloco::duration::from_secs(1'000'000), 100U); // >> time constant -> decay clamped to 0
  EXPECT_EQ(load.average().to_int(), 100U);
  EXPECT_EQ(load.current(), 100U);
}

TEST(CpuLoadTest, CurrentAndAverageAnswerDifferentQuestions) {
  cpu_load<> load(reloco::duration::from_secs(60));
  // Steady-state busy for a long time, then one transient idle sample:
  for (int i = 0; i < 200; ++i)
    load.sample(reloco::duration::from_secs(10), 4U);
  load.sample(reloco::duration::from_secs(1), 0U); // a brief, transient dip
  EXPECT_EQ(load.current(), 0U);                   // "nothing stealable right now"
  EXPECT_GT(load.average().to_int(), 0U);           // "but it's been consistently busy"
}
