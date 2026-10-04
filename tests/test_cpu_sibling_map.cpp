// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/arch/cpu_sibling_map.hpp>

using structo::arch::cpu_sibling_map;
using structo::arch::cpu_topology;
using structo::arch::flat_cpu_topology_decoder;
using structo::arch::passive_cpu_topology_decoder;

namespace {

/** @brief Fixture for `cpu_sibling_map` tests. */
class CpuSiblingMapTest : public ::testing::Test {};

/**
 * @brief Builds a 4-CPU, 3-level topology: {0,1} share core (level 0) and
 * cluster (level 1); {2,3} likewise; all four share one package (level 2).
 */
cpu_topology<4, 3> make_two_cluster_topology() {
  cpu_topology<4, 3> topo;
  topo.set_level_count(3);
  topo.set_level_id(0, 0, 10);
  topo.set_level_id(0, 1, 100);
  topo.set_level_id(0, 2, 1000);
  topo.set_level_id(1, 0, 10);
  topo.set_level_id(1, 1, 100);
  topo.set_level_id(1, 2, 1000);
  topo.set_level_id(2, 0, 20);
  topo.set_level_id(2, 1, 200);
  topo.set_level_id(2, 2, 1000);
  topo.set_level_id(3, 0, 20);
  topo.set_level_id(3, 1, 200);
  topo.set_level_id(3, 2, 1000);
  return topo;
}

} // namespace

TEST_F(CpuSiblingMapTest, DefaultConstructedIsEmpty) {
  cpu_sibling_map<4, 3> siblings;
  for (std::size_t cpu = 0; cpu < 4; ++cpu)
    EXPECT_EQ(siblings.sibling_count(cpu), 0u);
}

TEST_F(CpuSiblingMapTest, OrdersClosestClusterSiblingFirst) {
  auto topo = make_two_cluster_topology();
  cpu_sibling_map<4, 3> siblings;
  siblings.build(topo, 4);

  ASSERT_EQ(siblings.sibling_count(0), 3u);
  EXPECT_EQ(siblings.sibling(0, 0), 1u); // same core/cluster: closest
  EXPECT_EQ(siblings.sibling(0, 1), 2u); // only shares the package level
  EXPECT_EQ(siblings.sibling(0, 2), 3u);

  ASSERT_EQ(siblings.sibling_count(2), 3u);
  EXPECT_EQ(siblings.sibling(2, 0), 3u);
}

TEST_F(CpuSiblingMapTest, FlatTopologyHasNoDefinedOrderingPreference) {
  cpu_topology<4, 4> topo;
  flat_cpu_topology_decoder::decode(topo, 4);
  cpu_sibling_map<4, 4> siblings;
  siblings.build(topo, 4);
  // Every CPU is its own singleton group -- no pair shares any level, so
  // ties break by ascending CPU index.
  ASSERT_EQ(siblings.sibling_count(0), 3u);
  EXPECT_EQ(siblings.sibling(0, 0), 1u);
  EXPECT_EQ(siblings.sibling(0, 1), 2u);
  EXPECT_EQ(siblings.sibling(0, 2), 3u);
}

TEST_F(CpuSiblingMapTest, PassiveTopologyTreatsEveryCpuAsEquallyClose) {
  cpu_topology<4, 1> topo;
  passive_cpu_topology_decoder::decode(topo, 4);
  cpu_sibling_map<4, 1> siblings;
  siblings.build(topo, 4);
  // All CPUs share the single group -- level 0 for every pair, so order
  // is purely the deterministic ascending-index tie-break.
  ASSERT_EQ(siblings.sibling_count(0), 3u);
  EXPECT_EQ(siblings.sibling(0, 0), 1u);
  EXPECT_EQ(siblings.sibling(0, 1), 2u);
  EXPECT_EQ(siblings.sibling(0, 2), 3u);
}

TEST_F(CpuSiblingMapTest, BuildClampsCpuCountToCapacity) {
  auto topo = make_two_cluster_topology();
  cpu_sibling_map<4, 3> siblings;
  siblings.build(topo, 100); // clamped to MaxCpus == 4
  EXPECT_EQ(siblings.sibling_count(0), 3u);
}

TEST_F(CpuSiblingMapTest, TrySiblingReportsOutOfRange) {
  auto topo = make_two_cluster_topology();
  cpu_sibling_map<4, 3> siblings;
  siblings.build(topo, 4);
  EXPECT_FALSE(siblings.try_sibling(0, 3).has_value());
  EXPECT_FALSE(siblings.try_sibling(10, 0).has_value());
  EXPECT_TRUE(siblings.try_sibling(0, 0).has_value());
}

TEST_F(CpuSiblingMapTest, ClearResetsEverySiblingCount) {
  auto topo = make_two_cluster_topology();
  cpu_sibling_map<4, 3> siblings;
  siblings.build(topo, 4);
  siblings.clear();
  for (std::size_t cpu = 0; cpu < 4; ++cpu)
    EXPECT_EQ(siblings.sibling_count(cpu), 0u);
}
