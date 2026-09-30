// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/region_set.hpp>

using structo::error;
using structo::region_set;

TEST(RegionSetTest, SortsAndMergesAdjacentOrOverlappingRegions) {
  region_set<4> regions;

  ASSERT_TRUE(regions.try_add(0x3000, 0x1000));
  ASSERT_TRUE(regions.try_add(0x1000, 0x1000));
  ASSERT_TRUE(regions.try_add(0x1800, 0x2000));

  ASSERT_EQ(regions.size(), 1u);
  EXPECT_EQ(regions[0].base, 0x1000u);
  EXPECT_EQ(regions[0].size, 0x3000u);
  EXPECT_EQ(regions[0].end(), 0x4000u);
}

TEST(RegionSetTest, SubtractsWholeEdgesAndMiddleOfRegions) {
  region_set<6> regions;
  ASSERT_TRUE(regions.try_add(0x1000, 0x5000));
  ASSERT_TRUE(regions.try_subtract(0x1000, 0x1000));
  ASSERT_TRUE(regions.try_subtract(0x5000, 0x1000));
  ASSERT_TRUE(regions.try_subtract(0x3000, 0x1000));

  ASSERT_EQ(regions.size(), 2u);
  EXPECT_EQ(regions[0].base, 0x2000u);
  EXPECT_EQ(regions[0].size, 0x1000u);
  EXPECT_EQ(regions[1].base, 0x4000u);
  EXPECT_EQ(regions[1].size, 0x1000u);

  const auto largest = regions.largest_region();
  EXPECT_EQ(largest.base, 0x2000u);
  EXPECT_EQ(largest.size, 0x1000u);
}

TEST(RegionSetTest, EmptySubtractionAndZeroLengthInsertionAreNoOps) {
  region_set<2> regions;
  ASSERT_TRUE(regions.try_add(0x1000, 0x1000));

  ASSERT_TRUE(regions.try_add(0x2000, 0));
  ASSERT_TRUE(regions.try_subtract(0x1000, 0));
  ASSERT_TRUE(regions.try_subtract(0x3000, 0x1000));

  ASSERT_EQ(regions.size(), 1u);
  EXPECT_EQ(regions[0].base, 0x1000u);
  EXPECT_EQ(regions[0].size, 0x1000u);
}

TEST(RegionSetTest, ReportsCapacityFailureWhenARegionCannotBeSplit) {
  region_set<1> regions;
  ASSERT_TRUE(regions.try_add(0x1000, 0x3000));

  auto result = regions.try_subtract(0x2000, 0x1000);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), error::capacity_exceeded);
  ASSERT_EQ(regions.size(), 1u);
  EXPECT_EQ(regions[0].base, 0x1000u);
  EXPECT_EQ(regions[0].size, 0x3000u);
}
