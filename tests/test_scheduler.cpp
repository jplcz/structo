// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/bootldr/scheduler.hpp>

using namespace structo::bootldr;

namespace {

std::uint64_t clock_now(void *ctx) noexcept { return *static_cast<std::uint64_t *>(ctx); }

class Scheduler : public ::testing::Test {
protected:
  std::uint64_t now = 0;
  scheduler sched;
  Scheduler() { sched.set_clock(clock_now, &now); }
};

struct log {
  int items[64]{};
  int n = 0;
  void add(int v) { items[n++] = v; }
};

struct fault_log {
  int count = 0;
  reloco::error err{};
};

reloco::task<void> worker(scheduler &s, log &l, int tag, int rounds) {
  for (int i = 0; i < rounds; ++i) {
    l.add(tag * 10 + i);
    co_await s.yield();
  }
}

reloco::task<void> sleeper(scheduler &s, log &l) {
  l.add(1);
  co_await co_await s.sleep_for(100);
  l.add(2);
}

reloco::task<void> sleep_once(scheduler &s) { co_await co_await s.sleep_for(5); }

reloco::task<void> waiter(scheduler::event &e, log &l, int tag) {
  co_await e.wait();
  l.add(tag);
}

reloco::task<void> failing(scheduler &s) {
  co_await s.yield();
  co_await reloco::unexpected(reloco::error::out_of_range);
}

reloco::task<void> joiner(scheduler &s, task_id t, reloco::error &out, log &l) {
  auto r = co_await s.join(t);
  if (!r)
    out = r.error();
  l.add(1);
}

reloco::task<void> forever(scheduler &s) {
  for (;;)
    co_await s.yield();
}

reloco::task<void> with_allocator(reloco::allocator_arg_t, reloco::allocator_ref, scheduler &s, log &l) {
  co_await s.yield();
  l.add(7);
}

} // namespace

TEST_F(Scheduler, RoundRobinInterleavesYieldingTasks) {
  log l;
  ASSERT_TRUE(sched.spawn(worker(sched, l, 1, 3)).has_value());
  ASSERT_TRUE(sched.spawn(worker(sched, l, 2, 2)).has_value());
  EXPECT_EQ(sched.live(), 2u);
  sched.run();
  EXPECT_EQ(sched.live(), 0u);
  const int expect[] = {10, 20, 11, 21, 12};
  ASSERT_EQ(l.n, 5);
  for (int i = 0; i < 5; ++i)
    EXPECT_EQ(l.items[i], expect[i]);
}

TEST_F(Scheduler, SleepWakesAtDeadlineOnly) {
  log l;
  ASSERT_TRUE(sched.spawn(sleeper(sched, l)).has_value());
  EXPECT_EQ(sched.run_once(), 1u);
  EXPECT_EQ(l.n, 1);
  now = 99;
  EXPECT_EQ(sched.run_once(), 1u);
  EXPECT_EQ(l.n, 1);
  now = 100;
  sched.run_once(); // timer moves to ready; runs next round at the latest
  sched.run_once();
  EXPECT_EQ(l.n, 2);
  EXPECT_EQ(sched.live(), 0u);
}

TEST_F(Scheduler, SleepWithoutClockFailsTask) {
  scheduler noclock;
  fault_log f;
  noclock.set_fault_handler(
      [](void *c, task_id, reloco::error e) noexcept {
        auto *p = static_cast<fault_log *>(c);
        ++p->count;
        p->err = e;
      },
      &f);
  ASSERT_TRUE(noclock.spawn(sleep_once(noclock)).has_value());
  noclock.run();
  EXPECT_EQ(f.count, 1);
  EXPECT_EQ(f.err, reloco::error::invalid_state);
}

TEST_F(Scheduler, EventWakesAllWaiters) {
  scheduler::event ev{sched};
  log l;
  ASSERT_TRUE(sched.spawn(waiter(ev, l, 1)).has_value());
  ASSERT_TRUE(sched.spawn(waiter(ev, l, 2)).has_value());
  sched.run_once();
  sched.run_once();
  EXPECT_EQ(l.n, 0);
  ev.set();
  sched.run();
  EXPECT_EQ(l.n, 2);
  // Already set: waiting completes immediately.
  ASSERT_TRUE(sched.spawn(waiter(ev, l, 3)).has_value());
  sched.run();
  EXPECT_EQ(l.n, 3);
}

TEST_F(Scheduler, JoinReturnsOutcomeOfJoinableTask) {
  log l;
  auto id = sched.spawn(failing(sched), spawn_mode::joinable);
  ASSERT_TRUE(id.has_value());
  reloco::error seen{};
  ASSERT_TRUE(sched.spawn(joiner(sched, *id, seen, l)).has_value());
  sched.run();
  EXPECT_EQ(l.n, 1);
  EXPECT_EQ(seen, reloco::error::out_of_range);
  // Joined once: the id is gone now.
  ASSERT_TRUE(sched.spawn(joiner(sched, *id, seen, l)).has_value());
  sched.run();
  EXPECT_EQ(seen, reloco::error::invalid_argument);
}

TEST_F(Scheduler, PollersRunEveryRoundAndCancelStopsTask) {
  int polls = 0;
  ASSERT_TRUE(sched.add_poller([](void *p) noexcept { ++*static_cast<int *>(p); }, &polls).has_value());
  log l;
  auto id = sched.spawn(worker(sched, l, 1, 1000), spawn_mode::joinable);
  ASSERT_TRUE(id.has_value());
  sched.run_once();
  sched.run_once();
  EXPECT_EQ(polls, 2);
  ASSERT_TRUE(sched.cancel(*id).has_value());
  EXPECT_EQ(sched.live(), 0u);
  const int before = l.n;
  sched.run_once();
  EXPECT_EQ(l.n, before); // never resumed again
  auto again = sched.cancel(*id);
  ASSERT_FALSE(again.has_value());
  EXPECT_EQ(again.error(), reloco::error::invalid_argument);
}

TEST_F(Scheduler, FramesCanUseSchedulerAllocator) {
  log l;
  ASSERT_TRUE(sched.spawn(with_allocator(reloco::allocator_arg, sched.allocator(), sched, l)).has_value());
  sched.run();
  EXPECT_EQ(l.n, 1);
}

TEST_F(Scheduler, DestroyingSchedulerWithParkedTasksIsSafe) {
  {
    scheduler local;
    ASSERT_TRUE(local.spawn(forever(local)).has_value());
    local.run_once();
    local.run_once();
  } // parked frame destroyed with the scheduler
  SUCCEED();
}

namespace {

reloco::task<void> timed_waiter(scheduler::event &e, log &l, int ok_tag, int timeout_tag) {
  auto r = co_await e.wait_for(100);
  l.add(r ? ok_tag : (r.error() == reloco::error::timed_out ? timeout_tag : -1));
}

} // namespace

TEST_F(Scheduler, EventWaitForTimesOut) {
  scheduler::event ev{sched};
  log l;
  ASSERT_TRUE(sched.spawn(timed_waiter(ev, l, 1, 2)).has_value());
  sched.run_once();
  now = 50;
  sched.run_once();
  EXPECT_EQ(l.n, 0);
  now = 100;
  sched.run_once();
  sched.run_once();
  ASSERT_EQ(l.n, 1);
  EXPECT_EQ(l.items[0], 2);
  // The abandoned wait must not resume the (finished) task when the event is set later.
  ev.set();
  sched.run_once();
  EXPECT_EQ(l.n, 1);
}

TEST_F(Scheduler, EventWaitForCompletesWhenSetAndIgnoresLateTimer) {
  scheduler::event ev{sched};
  log l;
  ASSERT_TRUE(sched.spawn(timed_waiter(ev, l, 1, 2)).has_value());
  sched.run_once();
  ev.set();
  sched.run_once();
  ASSERT_EQ(l.n, 1);
  EXPECT_EQ(l.items[0], 1);
  now = 500;
  sched.run_once();
  sched.run_once();
  EXPECT_EQ(l.n, 1);
}
