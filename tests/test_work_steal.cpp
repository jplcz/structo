// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/work_steal.hpp>

using structo::always_steal_policy;
using structo::find_steal_candidate;
using structo::performance_steal_policy;
using structo::power_save_steal_policy;
using structo::arch::cpu_mask;
using structo::arch::cpu_sibling_map;
using structo::arch::cpu_topology;

namespace {

struct work_steal_test_tag {};

using mask4 = cpu_mask<work_steal_test_tag, 4>;

/**
 * @brief 4-CPU, 3-level topology: {0,1} share core (level 0) and cluster
 * (level 1); {2,3} likewise; all four share one package (level 2).
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

cpu_sibling_map<4, 3> make_siblings(const cpu_topology<4, 3> &topo) {
  cpu_sibling_map<4, 3> siblings;
  siblings.build(topo, 4);
  return siblings;
}

/** @brief Fixture for `find_steal_candidate`/steal-policy tests. */
class WorkStealTest : public ::testing::Test {};

} // namespace

TEST_F(WorkStealTest, PerformancePolicyPrefersClosestCandidateOverBusiestOverall) {
  auto topo = make_two_cluster_topology();
  auto siblings = make_siblings(topo);
  // cpu1 (same cluster as cpu0) is lightly busier; cpu3 (cross-package) is the busiest overall.
  std::size_t load[4] = {0, 2, 0, 10};
  auto eligible = mask4::filled();

  auto victim =
      find_steal_candidate<performance_steal_policy>(siblings, 0, eligible, [&](std::size_t cpu) { return load[cpu]; });
  ASSERT_TRUE(victim.has_value());
  EXPECT_EQ(*victim, 1u); // closest acceptable candidate, not the globally busiest one
}

TEST_F(WorkStealTest, PerformancePolicyRequiresMoreThanOneExtraTask) {
  auto topo = make_two_cluster_topology();
  auto siblings = make_siblings(topo);
  std::size_t load[4] = {0, 1, 0, 0}; // only one extra task anywhere -- not worth it
  auto eligible = mask4::filled();

  auto victim =
      find_steal_candidate<performance_steal_policy>(siblings, 0, eligible, [&](std::size_t cpu) { return load[cpu]; });
  EXPECT_FALSE(victim.has_value());
}

TEST_F(WorkStealTest, PowerSavePolicyOnlyStealsWhenLocallyIdle) {
  auto topo = make_two_cluster_topology();
  auto siblings = make_siblings(topo);
  std::size_t load[4] = {1, 0, 0, 5}; // cpu0 itself has one task -- not idle
  auto eligible = mask4::filled();

  auto victim =
      find_steal_candidate<power_save_steal_policy>(siblings, 0, eligible, [&](std::size_t cpu) { return load[cpu]; });
  EXPECT_FALSE(victim.has_value());
}

TEST_F(WorkStealTest, PowerSavePolicyStealsFromAnyBusyNeighborWhenIdle) {
  auto topo = make_two_cluster_topology();
  auto siblings = make_siblings(topo);
  std::size_t load[4] = {0, 0, 0, 3};
  auto eligible = mask4::filled();

  auto victim =
      find_steal_candidate<power_save_steal_policy>(siblings, 0, eligible, [&](std::size_t cpu) { return load[cpu]; });
  ASSERT_TRUE(victim.has_value());
  EXPECT_EQ(*victim, 3u);
}

TEST_F(WorkStealTest, EligibleMaskExcludesOfflineOrAlreadyTriedCpus) {
  auto topo = make_two_cluster_topology();
  auto siblings = make_siblings(topo);
  std::size_t load[4] = {0, 0, 0, 3};
  auto eligible = mask4::filled();
  eligible.clear(3); // e.g. offline, or already tried and failed this round

  auto victim =
      find_steal_candidate<power_save_steal_policy>(siblings, 0, eligible, [&](std::size_t cpu) { return load[cpu]; });
  EXPECT_FALSE(victim.has_value());
}

TEST_F(WorkStealTest, RetryByClearingFailedCandidateFindsNextClosest) {
  auto topo = make_two_cluster_topology();
  auto siblings = make_siblings(topo);
  std::size_t load[4] = {0, 4, 0, 4}; // cpu1 (closer) and cpu3 both qualify for performance_steal_policy
  auto eligible = mask4::filled();

  auto first =
      find_steal_candidate<performance_steal_policy>(siblings, 0, eligible, [&](std::size_t cpu) { return load[cpu]; });
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first, 1u);

  eligible.clear(*first); // simulate: stealing from cpu1 failed (queue emptied concurrently)
  auto second =
      find_steal_candidate<performance_steal_policy>(siblings, 0, eligible, [&](std::size_t cpu) { return load[cpu]; });
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*second, 3u);
}

TEST_F(WorkStealTest, AlwaysStealPolicyTakesAnyNonemptyCandidateRegardlessOfLocalLoad) {
  auto topo = make_two_cluster_topology();
  auto siblings = make_siblings(topo);
  std::size_t load[4] = {7, 0, 0, 1}; // local cpu already has plenty of work
  auto eligible = mask4::filled();

  auto victim =
      find_steal_candidate<always_steal_policy>(siblings, 0, eligible, [&](std::size_t cpu) { return load[cpu]; });
  ASSERT_TRUE(victim.has_value());
  EXPECT_EQ(*victim, 3u);
}

TEST_F(WorkStealTest, PerformancePolicyAttemptsWhenLocalLoadLow) {
  EXPECT_TRUE(performance_steal_policy::should_attempt_steal(0));
  EXPECT_TRUE(performance_steal_policy::should_attempt_steal(1));
  EXPECT_FALSE(performance_steal_policy::should_attempt_steal(2));
}

TEST_F(WorkStealTest, PowerSavePolicyOnlyAttemptsWhenFullyIdle) {
  EXPECT_TRUE(power_save_steal_policy::should_attempt_steal(0));
  EXPECT_FALSE(power_save_steal_policy::should_attempt_steal(1));
}

TEST_F(WorkStealTest, AlwaysStealPolicyAlwaysAttemptsRegardlessOfLocalLoad) {
  EXPECT_TRUE(always_steal_policy::should_attempt_steal(0));
  EXPECT_TRUE(always_steal_policy::should_attempt_steal(42));
}

TEST_F(WorkStealTest, FindStealCandidateSkipsSearchWhenPolicyDeclinesToAttempt) {
  auto topo = make_two_cluster_topology();
  auto siblings = make_siblings(topo);
  std::size_t load[4] = {1, 0, 0, 5}; // local isn't idle; power_save shouldn't even look
  auto eligible = mask4::filled();
  bool candidate_load_queried = false;

  auto victim = find_steal_candidate<power_save_steal_policy>(siblings, 0, eligible, [&](std::size_t cpu) {
    if (cpu != 0)
      candidate_load_queried = true;
    return load[cpu];
  });
  EXPECT_FALSE(victim.has_value());
  EXPECT_FALSE(candidate_load_queried); // confirms the early-exit, not merely "no candidate qualified"
}

TEST_F(WorkStealTest, NoCandidateQualifiesReturnsEmpty) {
  auto topo = make_two_cluster_topology();
  auto siblings = make_siblings(topo);
  std::size_t load[4] = {0, 0, 0, 0};
  auto eligible = mask4::filled();

  auto victim =
      find_steal_candidate<always_steal_policy>(siblings, 0, eligible, [&](std::size_t cpu) { return load[cpu]; });
  EXPECT_FALSE(victim.has_value());
}
