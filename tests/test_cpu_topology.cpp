// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/arch/cpu_topology.hpp>

using structo::arch::cpu_topology;
using structo::arch::flat_cpu_topology_decoder;
using structo::arch::passive_cpu_topology_decoder;

namespace {

/** @brief Fixture for `cpu_topology`/`flat_cpu_topology_decoder` tests. */
class CpuTopologyTest : public ::testing::Test {};

} // namespace

TEST_F(CpuTopologyTest, DefaultConstructedIsEmpty) {
  cpu_topology<8, 4> topo;
  EXPECT_EQ(topo.level_count(), 0u);
}

TEST_F(CpuTopologyTest, SetAndGetLevelIdRoundTrips) {
  cpu_topology<8, 4> topo;
  topo.set_level_count(2);
  topo.set_level_id(0, 0, 5);
  topo.set_level_id(0, 1, 9);
  EXPECT_EQ(topo.level_id(0, 0), 5u);
  EXPECT_EQ(topo.level_id(0, 1), 9u);
  EXPECT_EQ(topo.level_count(), 2u);
}

TEST_F(CpuTopologyTest, TryAccessorsReportOutOfRange) {
  cpu_topology<4, 2> topo;
  EXPECT_FALSE(topo.try_set_level_id(4, 0, 1).has_value());
  EXPECT_FALSE(topo.try_set_level_id(0, 2, 1).has_value());
  EXPECT_FALSE(topo.try_level_id(4, 0).has_value());
  EXPECT_FALSE(topo.try_level_id(0, 2).has_value());
  EXPECT_FALSE(topo.try_set_level_count(3).has_value());
  ASSERT_TRUE(topo.try_set_level_id(0, 0, 7).has_value());
  auto got = topo.try_level_id(0, 0);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(*got, 7u);
}

TEST_F(CpuTopologyTest, ClearResetsEverything) {
  cpu_topology<4, 2> topo;
  topo.set_level_count(1);
  topo.set_level_id(0, 0, 3);
  topo.clear();
  EXPECT_EQ(topo.level_count(), 0u);
  EXPECT_EQ(topo.level_id(0, 0), 0u);
}

TEST_F(CpuTopologyTest, SharesLevelComparesRecordedIds) {
  cpu_topology<4, 2> topo;
  topo.set_level_count(2);
  topo.set_level_id(0, 0, 1);
  topo.set_level_id(1, 0, 1);
  topo.set_level_id(2, 0, 2);
  topo.set_level_id(0, 1, 10);
  topo.set_level_id(1, 1, 10);
  topo.set_level_id(2, 1, 10);

  EXPECT_TRUE(topo.shares_level(0, 1, 0));  // same core-level group
  EXPECT_FALSE(topo.shares_level(0, 2, 0)); // different core-level group
  EXPECT_TRUE(topo.shares_level(0, 2, 1));  // same coarser (e.g. cluster) group
}

TEST_F(CpuTopologyTest, LowestSharedLevelFindsFinestMatchOrNone) {
  cpu_topology<4, 2> topo;
  topo.set_level_count(2);
  topo.set_level_id(0, 0, 1);
  topo.set_level_id(1, 0, 1);
  topo.set_level_id(2, 0, 2);
  topo.set_level_id(0, 1, 10);
  topo.set_level_id(1, 1, 10);
  topo.set_level_id(2, 1, 20);

  auto l01 = topo.lowest_shared_level(0, 1);
  ASSERT_TRUE(l01.has_value());
  EXPECT_EQ(*l01, 0u);

  auto l02 = topo.lowest_shared_level(0, 2);
  EXPECT_FALSE(l02.has_value());
}

TEST_F(CpuTopologyTest, FlatDecoderGivesEveryCpuItsOwnSingletonGroup) {
  cpu_topology<8, 4> topo;
  auto count = flat_cpu_topology_decoder::decode(topo, 4);
  EXPECT_EQ(count, 4u);
  EXPECT_EQ(topo.level_count(), 1u);
  for (std::size_t a = 0; a < count; ++a) {
    for (std::size_t b = 0; b < count; ++b) {
      EXPECT_EQ(topo.shares_level(a, b, 0), a == b);
    }
  }
}

TEST_F(CpuTopologyTest, FlatDecoderClampsCpuCountToCapacity) {
  cpu_topology<2, 4> topo;
  auto count = flat_cpu_topology_decoder::decode(topo, 10);
  EXPECT_EQ(count, 2u);
}

TEST_F(CpuTopologyTest, PassiveDecoderPutsEveryCpuInOneSharedGroup) {
  cpu_topology<8, 4> topo;
  auto count = passive_cpu_topology_decoder::decode(topo, 4);
  EXPECT_EQ(count, 4u);
  EXPECT_EQ(topo.level_count(), 1u);
  for (std::size_t a = 0; a < count; ++a) {
    for (std::size_t b = 0; b < count; ++b) {
      EXPECT_TRUE(topo.shares_level(a, b, 0));
    }
  }
}

TEST_F(CpuTopologyTest, PassiveDecoderClampsCpuCountToCapacity) {
  cpu_topology<2, 4> topo;
  auto count = passive_cpu_topology_decoder::decode(topo, 10);
  EXPECT_EQ(count, 2u);
}
