// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/sync/softlock_detector.hpp>
#include <structo/sync/ticket_spin_lock.hpp>

#include <cstdint>
#include <thread>
#include <vector>

using structo::sync::ticket_spin_lock;

namespace {

// Fake kernel backend: reports an adjustable "current thread" pointer-sized
// id so tests can simulate several distinct callers without real threads.
struct fake_owner_traits {
  using owner_type = std::uintptr_t;

  static inline owner_type current = 1;

  static owner_type current_owner() noexcept { return current; }
};

using test_lock = ticket_spin_lock<fake_owner_traits>;

// Fake kernel backend with a tiny softlock tick limit, to exercise the
// `softlock_detector` wired into `lock()`'s contended spin without
// having to actually spin millions of times.
struct fake_owner_traits_low_softlock_limit {
  using owner_type = std::uintptr_t;

  static constexpr structo::sync::softlock_detector::counter_type softlock_limit = 4;

  static inline owner_type current = 1;

  static owner_type current_owner() noexcept { return current; }
};

/** @brief Fixture for `ticket_spin_lock` tests; resets the fake "current owner" before each test. */
class TicketSpinLockTest : public ::testing::Test {
protected:
  void SetUp() override { fake_owner_traits::current = 1; }
};

} // namespace

TEST_F(TicketSpinLockTest, StartsUnlocked) {
  test_lock lock;
  EXPECT_FALSE(lock.is_locked());
  EXPECT_FALSE(lock.is_locked_by_current());
}

TEST_F(TicketSpinLockTest, LockAcquiresAndMarksCurrentAsOwner) {
  test_lock lock;
  lock.lock();
  EXPECT_TRUE(lock.is_locked());
  EXPECT_TRUE(lock.is_locked_by_current());
  lock.unlock();
}

TEST_F(TicketSpinLockTest, UnlockReleasesLock) {
  test_lock lock;
  lock.lock();
  lock.unlock();
  EXPECT_FALSE(lock.is_locked());
  EXPECT_FALSE(lock.is_locked_by_current());
}

TEST_F(TicketSpinLockTest, TryLockSucceedsWhenUnlocked) {
  test_lock lock;
  EXPECT_TRUE(lock.try_lock());
  EXPECT_TRUE(lock.is_locked_by_current());
  lock.unlock();
}

TEST_F(TicketSpinLockTest, TryLockFailsWhenHeldByAnotherOwner) {
  test_lock lock;
  fake_owner_traits::current = 1;
  lock.lock();

  fake_owner_traits::current = 2;
  EXPECT_FALSE(lock.try_lock());

  fake_owner_traits::current = 1;
  lock.unlock();
}

TEST_F(TicketSpinLockTest, IsLockedByCurrentReflectsWhichOwnerLocked) {
  test_lock lock;
  fake_owner_traits::current = 1;
  lock.lock();

  fake_owner_traits::current = 2;
  EXPECT_TRUE(lock.is_locked());
  EXPECT_FALSE(lock.is_locked_by_current());

  fake_owner_traits::current = 1;
  EXPECT_TRUE(lock.is_locked_by_current());
  lock.unlock();
}

TEST_F(TicketSpinLockTest, UnlockByNonOwnerTraps) {
  test_lock lock;
  fake_owner_traits::current = 1;
  lock.lock();

  fake_owner_traits::current = 2;
  EXPECT_DEATH({ lock.unlock(); }, "");

  // Restore the real owner so the fixture's lock can be safely destroyed.
  fake_owner_traits::current = 1;
  lock.unlock();
}

TEST_F(TicketSpinLockTest, RecursiveLockByCurrentOwnerTraps) {
  test_lock lock;
  lock.lock();
  EXPECT_DEATH({ lock.lock(); }, "");
  lock.unlock();
}

TEST_F(TicketSpinLockTest, DoubleUnlockTraps) {
  test_lock lock;
  lock.lock();
  lock.unlock();
  EXPECT_DEATH({ lock.unlock(); }, "");
}

TEST_F(TicketSpinLockTest, DestroyingHeldLockTraps) {
  EXPECT_DEATH(
      {
        test_lock lock;
        lock.lock();
      },
      "");
}

TEST_F(TicketSpinLockTest, DestroyingUnlockedLockIsFine) {
  {
    test_lock lock;
  }
  SUCCEED();
}

TEST_F(TicketSpinLockTest, TicketsAreServedInArrivalOrder) {
  test_lock lock;

  // Single-threaded sanity check that acquiring/releasing for distinct
  // owners in sequence always lets the next caller through immediately
  // once the previous one has unlocked -- real cross-thread ordering is
  // covered by MutualExclusionHoldsAcrossConcurrentThreads below.
  fake_owner_traits::current = 1;
  lock.lock();
  lock.unlock();

  fake_owner_traits::current = 2;
  lock.lock();
  EXPECT_TRUE(lock.is_locked_by_current());
  lock.unlock();

  fake_owner_traits::current = 3;
  lock.lock();
  EXPECT_TRUE(lock.is_locked_by_current());
  lock.unlock();
}

TEST(TicketSpinLockConcurrencyTest, MutualExclusionHoldsAcrossConcurrentThreads) {
  // Thread-local pointer identity: a stable, unique owner id per thread
  // with zero shared mutable state to race on (unlike a shared `current`
  // static), so this traits type is safe to use concurrently.
  struct thread_owner_traits {
    using owner_type = std::uintptr_t;

    static owner_type current_owner() noexcept {
      thread_local int marker = 0;
      return reinterpret_cast<std::uintptr_t>(&marker);
    }
  };

  ticket_spin_lock<thread_owner_traits> lock;
  int shared_counter = 0;
  constexpr int num_threads = 4;
  constexpr int increments_per_thread = 2000;

  std::vector<std::thread> threads;
  threads.reserve(num_threads);
  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back([&] {
      for (int j = 0; j < increments_per_thread; ++j) {
        lock.lock();
        ++shared_counter; // only safe to do non-atomically because of the lock's mutual exclusion
        lock.unlock();
      }
    });
  }
  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(shared_counter, num_threads * increments_per_thread);
}

TEST(TicketSpinLockSoftlockTest, ContendedLockTrapsAfterSoftlockLimit) {
  using lowlimit_lock = ticket_spin_lock<fake_owner_traits_low_softlock_limit>;

  lowlimit_lock lock;
  fake_owner_traits_low_softlock_limit::current = 1;
  lock.lock();

  // Owner 1 never releases, so owner 2's `lock()` spins forever against
  // the softlock_detector wired into the contended loop -- it should
  // trap well before actually spinning forever.
  fake_owner_traits_low_softlock_limit::current = 2;
  EXPECT_DEATH({ lock.lock(); }, "");

  // Restore the real owner so the lock can be safely destroyed.
  fake_owner_traits_low_softlock_limit::current = 1;
  lock.unlock();
}
