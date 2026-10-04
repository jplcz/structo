// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/arch/per_cpu_ptr.hpp>
#include <structo/sched.hpp>
#include <structo/sched_world_handoff.hpp>

namespace {

using namespace structo;
using reloco::duration;
using reloco::instant;

/** @brief Single-slot `per_cpu_ptr` `Tag`, one instantiation per state type, for use in tests. */
template <typename StateType> struct test_percpu_tag {
  static inline constexpr std::size_t max_cpus = 1;
  static inline void *slot = nullptr;
  static void *get_ptr(std::size_t) noexcept { return slot; }
  static void set_ptr(std::size_t, void *ptr) noexcept { slot = ptr; }
  static std::size_t current() noexcept { return 0; }
};

template <typename StateType> using test_percpu = arch::per_cpu_ptr<test_percpu_tag<StateType>, StateType>;

instant at(std::uint64_t millis) { return instant{} + duration::from_millis(millis); }

struct task {
  struct {
    task *next = nullptr;
    task **prev = nullptr;
  } link;
  unsigned priority = 0;
  int id = 0;
};

constexpr std::size_t kNumPriorities = 8;
using sched_state = fixed_priority_sched_state<task, &task::link, &task::priority, kNumPriorities>;
using sched_percpu = test_percpu<sched_state>;
using sched = fixed_priority_sched<task, &task::link, &task::priority, kNumPriorities, sched_percpu>;

struct test_traits {
  static constexpr duration max_dwell() noexcept { return duration::from_millis(100); }
  static constexpr duration stuck_warning_threshold() noexcept { return duration::from_millis(1'000); }
};

using handoff = sched_world_handoff<task, sched, test_traits>;

class SchedWorldHandoffTest : public ::testing::Test {
protected:
  void SetUp() override { sched_percpu::set(&state_); }
  void TearDown() override { sched_percpu::set(nullptr); }
  sched_state state_;
  task world_thread{{}, 0, 100};
};

TEST_F(SchedWorldHandoffTest, StartsNotInWorldAndNoSwitchNeeded) {
  handoff h(world_thread);
  EXPECT_FALSE(h.in_world());
  EXPECT_FALSE(h.force_switch_needed());
  EXPECT_FALSE(h.should_switch_to_world(at(0)));
  EXPECT_FALSE(h.stuck_warning_due(at(0)));
  EXPECT_EQ(h.dwell_time(at(1'000)), duration{});
}

TEST_F(SchedWorldHandoffTest, OnWorldEnteredTracksKindAndStartsDwellClock) {
  handoff h(world_thread);
  h.on_world_entered(at(0), world_entry_kind::cooperative);
  EXPECT_TRUE(h.in_world());
  EXPECT_EQ(h.entry_kind(), world_entry_kind::cooperative);
  EXPECT_EQ(h.dwell_time(at(30)), duration::from_millis(30));

  h.on_world_entered(at(0), world_entry_kind::critical);
  EXPECT_EQ(h.entry_kind(), world_entry_kind::critical);
}

TEST_F(SchedWorldHandoffTest, OnWorldExitedClearsInWorldAndForceSwitchNeeded) {
  handoff h(world_thread);
  h.on_world_entered(at(0), world_entry_kind::cooperative);
  h.request_force_switch(at(10));
  ASSERT_TRUE(h.force_switch_needed());

  h.on_world_exited(at(20));
  EXPECT_FALSE(h.in_world());
  EXPECT_FALSE(h.force_switch_needed());
  EXPECT_EQ(h.dwell_time(at(1'000)), duration{});
}

TEST_F(SchedWorldHandoffTest, RequestForceSwitchForcesWorldThreadRegardlessOfPriority) {
  handoff h(world_thread);
  task high_priority_task{{}, 0, 1}; // numerically lowest priority value = highest priority
  sched::enqueue(high_priority_task);
  h.on_world_entered(at(0), world_entry_kind::cooperative);

  h.request_force_switch(at(10));
  EXPECT_TRUE(h.force_switch_needed());
  // world_thread must win despite high_priority_task's superior priority
  EXPECT_EQ(sched::pick_next()->id, world_thread.id);
  EXPECT_EQ(sched::pick_next()->id, 1);
}

TEST_F(SchedWorldHandoffTest, ShouldSwitchToWorldIsFalseWhileNotInWorld) {
  handoff h(world_thread);
  h.request_force_switch(at(0)); // nonsensical outside on_world_entered, but must not misbehave
  EXPECT_FALSE(h.should_switch_to_world(at(0)));
}

TEST_F(SchedWorldHandoffTest, ShouldSwitchToWorldIsTrueWhenForceSwitchNeeded) {
  handoff h(world_thread);
  h.on_world_entered(at(0), world_entry_kind::cooperative);
  h.request_force_switch(at(10));
  EXPECT_TRUE(h.should_switch_to_world(at(10)));
}

TEST_F(SchedWorldHandoffTest, ShouldSwitchToWorldForcesReentryOnceDwellBudgetExceeded) {
  handoff h(world_thread);
  h.on_world_entered(at(0), world_entry_kind::cooperative);
  EXPECT_FALSE(h.should_switch_to_world(at(50))); // within budget (max_dwell == 100ms)
  EXPECT_FALSE(h.force_switch_needed());

  EXPECT_TRUE(h.should_switch_to_world(at(150))); // budget exceeded
  EXPECT_TRUE(h.force_switch_needed());           // sticky: marked needed, not just reported once
  EXPECT_EQ(sched::pick_next()->id, world_thread.id);
}

TEST_F(SchedWorldHandoffTest, ShouldSwitchToWorldIgnoresDwellBudgetDuringCriticalEntry) {
  handoff h(world_thread);
  h.on_world_entered(at(0), world_entry_kind::critical);
  // A critical (forced, uncooperative) entry has no cooperative dwell budget to exceed.
  EXPECT_FALSE(h.should_switch_to_world(at(10'000)));
  EXPECT_FALSE(h.force_switch_needed());
}

TEST_F(SchedWorldHandoffTest, StuckWarningDueOnlyAfterThresholdWhileInWorld) {
  handoff h(world_thread);
  h.on_world_entered(at(0), world_entry_kind::cooperative);
  EXPECT_FALSE(h.stuck_warning_due(at(500)));
  EXPECT_TRUE(h.stuck_warning_due(at(1'500)));

  h.on_world_exited(at(1'500));
  EXPECT_FALSE(h.stuck_warning_due(at(10'000))); // no longer in world -- never stuck
}

TEST_F(SchedWorldHandoffTest, OnWorldEnteredForcesResumeHintIntoPlace) {
  handoff h(world_thread);
  task resumed{{}, 0, 2};
  task other{{}, 0, 3};
  sched::enqueue(other);

  h.on_world_entered(at(0), world_entry_kind::cooperative, &resumed);
  EXPECT_EQ(sched::pick_next()->id, 2); // resume hint wins over other, never-enqueued and all
  EXPECT_EQ(sched::pick_next()->id, 3);
}

TEST_F(SchedWorldHandoffTest, OnBlockClearsAStaleResumeHintBeforeItsEverDispatched) {
  handoff h(world_thread);
  task resumed{{}, 0, 2};
  h.on_world_entered(at(0), world_entry_kind::cooperative, &resumed);

  // resumed blocks (e.g. needs a lock) before pick_next ever consumed its force_next pin.
  sched::on_block(resumed);
  h.on_block(resumed);

  // A later, unrelated wake must not re-force it into place anymore.
  sched::on_wake(resumed);
  h.on_wake(resumed);
  EXPECT_EQ(sched::pick_next()->id, 2); // still runnable (ordinary enqueue), just not force-pinned
  EXPECT_TRUE(sched::empty());
}

TEST_F(SchedWorldHandoffTest, OnWakeReForcesWorldThreadIfForceSwitchStillNeededAfterItBlocks) {
  handoff h(world_thread);
  h.on_world_entered(at(0), world_entry_kind::cooperative);
  h.request_force_switch(at(10)); // pins world_thread
  ASSERT_FALSE(sched::is_linked(world_thread));

  // world_thread blocks partway through its forced return path (e.g. needs a lock).
  sched::on_block(world_thread); // clears sched.hpp's own internal pin automatically
  h.on_block(world_thread);      // no-op: world_thread is never a `resume_hint`

  task other{{}, 0, 9};
  sched::enqueue(other);

  // world_thread wakes back up -- must be re-forced into place since the switch is still owed.
  sched::on_wake(world_thread);
  h.on_wake(world_thread);
  EXPECT_EQ(sched::pick_next()->id, world_thread.id);
  EXPECT_EQ(sched::pick_next()->id, 9);
}

TEST_F(SchedWorldHandoffTest, OnWakeDoesNotReForceWorldThreadWhenNoSwitchIsOwed) {
  handoff h(world_thread);
  h.on_world_entered(at(0), world_entry_kind::cooperative);
  // Give world_thread a worse priority than usual so ordinary (non-forced) dispatch order is observable.
  world_thread.priority = 7;
  // No request_force_switch -- world_thread blocking/waking is an ordinary, ungoverned event.
  sched::enqueue(world_thread);
  sched::on_block(world_thread);
  h.on_block(world_thread);

  task other{{}, 0, 9}; // best priority
  sched::enqueue(other);

  sched::on_wake(world_thread);
  h.on_wake(world_thread);
  // Ordinary priority order applies -- not force-pinned.
  EXPECT_EQ(sched::pick_next()->id, 9);
  EXPECT_EQ(sched::pick_next()->id, world_thread.id);
}

} // namespace
