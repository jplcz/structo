// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/arch/per_cpu_ptr.hpp>
#include <structo/sched.hpp>

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

// --------------------------------------------------------------------
// noop_sched
// --------------------------------------------------------------------

struct noop_task {
  struct {
    noop_task *next = nullptr;
    noop_task **prev = nullptr;
  } link;
  int id = 0;
};

using noop_state = noop_sched_state<noop_task, &noop_task::link>;
using noop_percpu = test_percpu<noop_state>;
using noop = noop_sched<noop_task, &noop_task::link, noop_percpu>;

class NoopSchedTest : public ::testing::Test {
protected:
  void SetUp() override { noop_percpu::set(&state_); }
  void TearDown() override { noop_percpu::set(nullptr); }
  noop_state state_;
};

TEST_F(NoopSchedTest, StartsEmpty) {
  EXPECT_TRUE(noop::empty());
  EXPECT_EQ(noop::size(), 0u);
  EXPECT_EQ(noop::pick_next(), nullptr);
}

TEST_F(NoopSchedTest, DispatchesInFifoOrder) {
  noop_task a{{}, 1};
  noop_task b{{}, 2};
  noop_task c{{}, 3};
  noop::enqueue(a);
  noop::enqueue(b);
  noop::enqueue(c);
  EXPECT_EQ(noop::size(), 3u);

  EXPECT_EQ(noop::pick_next()->id, 1);
  EXPECT_EQ(noop::pick_next()->id, 2);
  EXPECT_EQ(noop::pick_next()->id, 3);
  EXPECT_EQ(noop::pick_next(), nullptr);
}

TEST_F(NoopSchedTest, IsLinkedAndRemove) {
  noop_task a{{}, 1};
  EXPECT_FALSE(noop::is_linked(a));
  noop::enqueue(a);
  EXPECT_TRUE(noop::is_linked(a));
  noop::remove(a);
  EXPECT_FALSE(noop::is_linked(a));
  EXPECT_TRUE(noop::empty());
}

TEST_F(NoopSchedTest, UniformInterfaceMethodsAllBehaveAsEnqueue) {
  noop_task a{{}, 1};
  noop_task b{{}, 2};
  noop::enqueue(a);
  noop_task *dispatched = noop::pick_next();
  ASSERT_EQ(dispatched, &a);
  noop::on_block(*dispatched); // no-op: already removed from the queue
  EXPECT_FALSE(noop::is_linked(a));
  noop::on_wake(*dispatched); // becomes runnable again, same as enqueue
  EXPECT_TRUE(noop::is_linked(a));

  dispatched = noop::pick_next();
  ASSERT_EQ(dispatched, &a);
  noop::enqueue(b);
  noop::requeue(*dispatched); // quantum expired while still runnable -- back to the tail
  EXPECT_EQ(noop::pick_next()->id, 2);
  EXPECT_EQ(noop::pick_next()->id, 1);

  noop::enqueue(a);
  dispatched = noop::pick_next();
  noop::on_yield(*dispatched); // voluntary yield -- same as requeue for this policy
  EXPECT_TRUE(noop::is_linked(a));
}

// --------------------------------------------------------------------
// fixed_priority_sched
// --------------------------------------------------------------------

struct fixed_task {
  struct {
    fixed_task *next = nullptr;
    fixed_task **prev = nullptr;
  } link;
  unsigned priority = 0;
  int id = 0;
};

constexpr std::size_t kFixedPriorities = 8;
using fixed_state = fixed_priority_sched_state<fixed_task, &fixed_task::link, &fixed_task::priority, kFixedPriorities>;
using fixed_percpu = test_percpu<fixed_state>;
using fixed =
    fixed_priority_sched<fixed_task, &fixed_task::link, &fixed_task::priority, kFixedPriorities, fixed_percpu>;

class FixedPrioritySchedTest : public ::testing::Test {
protected:
  void SetUp() override { fixed_percpu::set(&state_); }
  void TearDown() override { fixed_percpu::set(nullptr); }
  fixed_state state_;
};

TEST_F(FixedPrioritySchedTest, DispatchesHighestPriorityFirst) {
  fixed_task low{{}, 5, 1};
  fixed_task high{{}, 0, 2};
  fixed_task mid{{}, 2, 3};
  fixed::enqueue(low);
  fixed::enqueue(high);
  fixed::enqueue(mid);

  EXPECT_EQ(fixed::pick_next()->id, 2);
  EXPECT_EQ(fixed::pick_next()->id, 3);
  EXPECT_EQ(fixed::pick_next()->id, 1);
  EXPECT_EQ(fixed::pick_next(), nullptr);
}

TEST_F(FixedPrioritySchedTest, SamePriorityIsFifo) {
  fixed_task a{{}, 3, 1};
  fixed_task b{{}, 3, 2};
  fixed::enqueue(a);
  fixed::enqueue(b);
  EXPECT_EQ(fixed::pick_next()->id, 1);
  EXPECT_EQ(fixed::pick_next()->id, 2);
}

TEST_F(FixedPrioritySchedTest, RequeueGivesRoundRobinBehavior) {
  fixed_task a{{}, 1, 1};
  fixed_task b{{}, 1, 2};
  fixed::enqueue(a);
  fixed::enqueue(b);

  fixed_task *first = fixed::pick_next();
  ASSERT_EQ(first->id, 1);
  // Quantum expired while `a` was still runnable: requeue it instead of
  // leaving it dispatched, for SCHED_RR-like behavior.
  fixed::requeue(*first);

  EXPECT_EQ(fixed::pick_next()->id, 2);
  EXPECT_EQ(fixed::pick_next()->id, 1);
}

TEST_F(FixedPrioritySchedTest, IsLinkedAndRemove) {
  fixed_task a{{}, 0, 1};
  EXPECT_FALSE(fixed::is_linked(a));
  fixed::enqueue(a);
  EXPECT_TRUE(fixed::is_linked(a));
  fixed::remove(a);
  EXPECT_FALSE(fixed::is_linked(a));
}

TEST_F(FixedPrioritySchedTest, UniformInterfaceMethodsAllBehaveAsEnqueue) {
  fixed_task a{{}, 2, 1};
  fixed::enqueue(a);
  fixed_task *dispatched = fixed::pick_next();
  ASSERT_EQ(dispatched, &a);
  fixed::on_block(*dispatched); // no-op: already removed from the queue
  EXPECT_FALSE(fixed::is_linked(a));
  fixed::on_wake(*dispatched); // becomes runnable again, same as enqueue
  EXPECT_TRUE(fixed::is_linked(a));

  dispatched = fixed::pick_next();
  fixed::on_yield(*dispatched); // voluntary yield -- same as requeue for this policy
  EXPECT_TRUE(fixed::is_linked(a));
}

// --------------------------------------------------------------------
// edf_sched
// --------------------------------------------------------------------

struct edf_task {
  struct {
    edf_task *next = nullptr;
    edf_task **prev = nullptr;
  } link;
  instant deadline{};
  int id = 0;
};

using edf_state = edf_sched_state<edf_task, &edf_task::link, &edf_task::deadline>;
using edf_percpu = test_percpu<edf_state>;
using edf = edf_sched<edf_task, &edf_task::link, &edf_task::deadline, edf_percpu>;

class EdfSchedTest : public ::testing::Test {
protected:
  void SetUp() override { edf_percpu::set(&state_); }
  void TearDown() override { edf_percpu::set(nullptr); }
  edf_state state_;
};

TEST_F(EdfSchedTest, StartsEmpty) {
  EXPECT_TRUE(edf::empty());
  EXPECT_EQ(edf::size(), 0u);
  EXPECT_EQ(edf::pick_next(), nullptr);
}

TEST_F(EdfSchedTest, DispatchesSoonestDeadlineFirstRegardlessOfEnqueueOrder) {
  edf_task far{{}, at(1000), 1};
  edf_task near{{}, at(10), 2};
  edf_task mid{{}, at(100), 3};
  edf::enqueue(far);
  edf::enqueue(near);
  edf::enqueue(mid);
  EXPECT_EQ(edf::size(), 3u);

  EXPECT_EQ(edf::pick_next()->id, 2);
  EXPECT_EQ(edf::pick_next()->id, 3);
  EXPECT_EQ(edf::pick_next()->id, 1);
  EXPECT_EQ(edf::pick_next(), nullptr);
}

TEST_F(EdfSchedTest, SameDeadlineIsFifo) {
  edf_task a{{}, at(50), 1};
  edf_task b{{}, at(50), 2};
  edf::enqueue(a);
  edf::enqueue(b);
  EXPECT_EQ(edf::pick_next()->id, 1);
  EXPECT_EQ(edf::pick_next()->id, 2);
}

TEST_F(EdfSchedTest, IsLinkedAndRemove) {
  edf_task a{{}, at(10), 1};
  EXPECT_FALSE(edf::is_linked(a));
  edf::enqueue(a);
  EXPECT_TRUE(edf::is_linked(a));
  edf::remove(a);
  EXPECT_FALSE(edf::is_linked(a));
}

TEST_F(EdfSchedTest, UniformInterfaceMethodsAllBehaveAsEnqueue) {
  edf_task a{{}, at(10), 1};
  edf::enqueue(a);
  edf_task *dispatched = edf::pick_next();
  ASSERT_EQ(dispatched, &a);
  edf::on_block(*dispatched); // no-op: already removed from the queue
  EXPECT_FALSE(edf::is_linked(a));
  edf::on_wake(*dispatched); // becomes runnable again, same as enqueue
  EXPECT_TRUE(edf::is_linked(a));

  dispatched = edf::pick_next();
  edf::requeue(*dispatched); // quantum expired while still runnable
  EXPECT_TRUE(edf::is_linked(a));
}

// --------------------------------------------------------------------
// sched_ule
// --------------------------------------------------------------------

struct ule_task {
  struct {
    ule_task *next = nullptr;
    ule_task **prev = nullptr;
  } link;
  unsigned priority = 0;
  ule_task_state ule{};
  int id = 0;
};

constexpr std::size_t kUlePriorities = 32;
using ule_state_t = sched_ule_state<ule_task, &ule_task::link, &ule_task::priority, kUlePriorities>;
using ule_percpu = test_percpu<ule_state_t>;
using ule = sched_ule<ule_task, &ule_task::link, &ule_task::priority, &ule_task::ule, kUlePriorities, ule_percpu>;

class SchedUleTest : public ::testing::Test {
protected:
  void SetUp() override { ule_percpu::set(&state_); }
  void TearDown() override { ule_percpu::set(nullptr); }
  ule_state_t state_;
};

TEST_F(SchedUleTest, NewTasksAreFullyInteractiveAndDispatchable) {
  ule_task a{{}, 0, {}, 1};
  ule::enqueue(a, at(0));
  EXPECT_TRUE(ule::is_linked(a));
  EXPECT_EQ(ule::size(), 1u);

  ule_task *picked = ule::pick_next(at(1));
  ASSERT_NE(picked, nullptr);
  EXPECT_EQ(picked->id, 1);
}

TEST_F(SchedUleTest, SleepDominantTaskStaysMoreUrgentThanRunDominantTask) {
  ule_task sleeper{{}, 0, {}, 1};
  ule_task hog{{}, 0, {}, 2};

  ule::enqueue(sleeper, at(0));
  ule::enqueue(hog, at(0));

  // Drain the initial `next` batch into `curr`.
  ule_task *first = ule::pick_next(at(1));
  ule_task *second = ule::pick_next(at(1));
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  // `hog` runs for a long time then yields (run-dominant).
  ule_task *runner = first->id == 2 ? first : second;
  ule_task *sleepy = first->id == 2 ? second : first;
  ule::on_yield(*runner, at(1000));
  // `sleepy` blocks immediately then wakes after a long sleep (sleep-dominant).
  ule::on_block(*sleepy, at(1));
  ule::on_wake(*sleepy, at(2000));

  EXPECT_LT(sleepy->priority, runner->priority);
}

TEST_F(SchedUleTest, RemoveWorksFromEitherQueue) {
  ule_task a{{}, 0, {}, 1};
  ule::enqueue(a, at(0)); // lands in `next`
  EXPECT_TRUE(ule::is_linked(a));
  ule::remove(a);
  EXPECT_FALSE(ule::is_linked(a));
  EXPECT_TRUE(ule::empty());
}

TEST_F(SchedUleTest, RequeueIsSameOperationAsOnYield) {
  ule_task a{{}, 0, {}, 1};
  ule::enqueue(a, at(0));
  ule_task *dispatched = ule::pick_next(at(1));
  ASSERT_EQ(dispatched, &a);
  ule::requeue(*dispatched, at(5000)); // quantum expired while still runnable
  EXPECT_TRUE(ule::is_linked(a));
  EXPECT_EQ(ule::size(), 1u);
}

// --------------------------------------------------------------------
// sched_4bsd
// --------------------------------------------------------------------

struct bsd_task {
  struct {
    bsd_task *next = nullptr;
    bsd_task **prev = nullptr;
  } link;
  unsigned priority = 0;
  bsd_task_state bsd{};
  int id = 0;
};

constexpr std::size_t kBsdPriorities = 64;
using bsd_state_t = sched_4bsd_state<bsd_task, &bsd_task::link, &bsd_task::priority, kBsdPriorities>;
using bsd_percpu = test_percpu<bsd_state_t>;
using bsd = sched_4bsd<bsd_task, &bsd_task::link, &bsd_task::priority, &bsd_task::bsd, kBsdPriorities, bsd_percpu>;

class Sched4BsdTest : public ::testing::Test {
protected:
  void SetUp() override { bsd_percpu::set(&state_); }
  void TearDown() override { bsd_percpu::set(nullptr); }
  bsd_state_t state_;
};

TEST_F(Sched4BsdTest, NewTaskIsDispatchable) {
  bsd_task a{{}, 0, {}, 1};
  bsd::enqueue(a, at(0));
  bsd_task *picked = bsd::pick_next(at(0));
  ASSERT_NE(picked, nullptr);
  EXPECT_EQ(picked->id, 1);
}

TEST_F(Sched4BsdTest, CpuHogGetsLowerPriorityThanFreshTask) {
  bsd_task hog{{}, 0, {}, 1};
  bsd::enqueue(hog, at(0));
  bsd_task *picked = bsd::pick_next(at(0));
  ASSERT_EQ(picked, &hog);
  // Runs for a long time, accumulating estcpu, then yields.
  bsd::on_yield(hog, at(5000));
  const unsigned hog_priority_after_run = hog.priority;

  bsd_task fresh{{}, 0, {}, 2};
  bsd::enqueue(fresh, at(5000));

  EXPECT_GT(hog_priority_after_run, fresh.priority);
}

TEST_F(Sched4BsdTest, DecayLowersPriorityNumberOverElapsedTime) {
  bsd_task hog{{}, 0, {}, 1};
  bsd::enqueue(hog, at(0));
  bsd::pick_next(at(0));
  bsd::on_yield(hog, at(5000));
  const unsigned priority_just_after_run = hog.priority;

  // A long time passes (many decay half-lives) before the task is
  // touched again; its estcpu should have decayed toward zero.
  bsd::enqueue(hog, at(60'000));
  EXPECT_LT(hog.priority, priority_just_after_run);
}

TEST_F(Sched4BsdTest, NiceShiftsPriority) {
  bsd_task a{{}, 0, {}, 1};
  bsd::enqueue(a, at(0));
  const unsigned base_priority = a.priority;
  bsd::remove(a);

  bsd::set_nice(a, 10);
  bsd::enqueue(a, at(0));
  EXPECT_GT(a.priority, base_priority);
}

TEST_F(Sched4BsdTest, IsLinkedAndRemove) {
  bsd_task a{{}, 0, {}, 1};
  EXPECT_FALSE(bsd::is_linked(a));
  bsd::enqueue(a, at(0));
  EXPECT_TRUE(bsd::is_linked(a));
  bsd::remove(a);
  EXPECT_FALSE(bsd::is_linked(a));
}

TEST_F(Sched4BsdTest, RequeueIsSameOperationAsOnYield) {
  bsd_task a{{}, 0, {}, 1};
  bsd::enqueue(a, at(0));
  bsd_task *dispatched = bsd::pick_next(at(0));
  ASSERT_EQ(dispatched, &a);
  bsd::requeue(*dispatched, at(5000)); // quantum expired while still runnable
  EXPECT_TRUE(bsd::is_linked(a));
}

} // namespace
