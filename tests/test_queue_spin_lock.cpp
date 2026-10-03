// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/sync/queue_spin_lock.hpp>
#include <structo/sync/softlock_detector.hpp>

#include <cstdint>
#include <thread>
#include <vector>

using structo::sync::queue_spin_lock;

namespace {

// Fake kernel backend: reports an adjustable "current thread" pointer-sized
// id so tests can simulate several distinct callers without real threads.
struct fake_owner_traits {
  using owner_type = std::uintptr_t;

  static inline owner_type current = 1;

  static owner_type current_owner() noexcept { return current; }
};

using test_lock = queue_spin_lock<fake_owner_traits>;

// Fake kernel backend with a tiny softlock tick limit, to exercise the
// `softlock_detector` wired into `lock()`'s contended spin without
// having to actually spin millions of times.
struct fake_owner_traits_low_softlock_limit {
  using owner_type = std::uintptr_t;

  static constexpr structo::sync::softlock_detector::counter_type softlock_limit = 4;

  static inline owner_type current = 1;

  static owner_type current_owner() noexcept { return current; }
};

/** @brief Fixture for `queue_spin_lock` tests; resets the fake "current owner" before each test. */
class QueueSpinLockTest : public ::testing::Test {
protected:
  void SetUp() override { fake_owner_traits::current = 1; }
};

} // namespace

TEST_F(QueueSpinLockTest, StartsUnlocked) {
  test_lock lock;
  EXPECT_FALSE(lock.is_locked());
  EXPECT_FALSE(lock.is_locked_by_current());
}

TEST_F(QueueSpinLockTest, LockAcquiresAndMarksCurrentAsOwner) {
  test_lock lock;
  test_lock::node n;
  lock.lock(n);
  EXPECT_TRUE(lock.is_locked());
  EXPECT_TRUE(lock.is_locked_by_current());
  lock.unlock(n);
}

TEST_F(QueueSpinLockTest, UnlockReleasesLock) {
  test_lock lock;
  test_lock::node n;
  lock.lock(n);
  lock.unlock(n);
  EXPECT_FALSE(lock.is_locked());
  EXPECT_FALSE(lock.is_locked_by_current());
}

TEST_F(QueueSpinLockTest, TryLockSucceedsWhenUnlocked) {
  test_lock lock;
  test_lock::node n;
  EXPECT_TRUE(lock.try_lock(n));
  EXPECT_TRUE(lock.is_locked_by_current());
  lock.unlock(n);
}

TEST_F(QueueSpinLockTest, TryLockFailsWhenHeldByAnotherOwner) {
  test_lock lock;
  test_lock::node n1;
  test_lock::node n2;
  fake_owner_traits::current = 1;
  lock.lock(n1);

  fake_owner_traits::current = 2;
  EXPECT_FALSE(lock.try_lock(n2));

  fake_owner_traits::current = 1;
  lock.unlock(n1);
}

TEST_F(QueueSpinLockTest, IsLockedByCurrentReflectsWhichOwnerLocked) {
  test_lock lock;
  test_lock::node n;
  fake_owner_traits::current = 1;
  lock.lock(n);

  fake_owner_traits::current = 2;
  EXPECT_TRUE(lock.is_locked());
  EXPECT_FALSE(lock.is_locked_by_current());

  fake_owner_traits::current = 1;
  EXPECT_TRUE(lock.is_locked_by_current());
  lock.unlock(n);
}

TEST_F(QueueSpinLockTest, UnlockByNonOwnerTraps) {
  test_lock lock;
  test_lock::node n;
  fake_owner_traits::current = 1;
  lock.lock(n);

  fake_owner_traits::current = 2;
  EXPECT_DEATH({ lock.unlock(n); }, "");

  // Restore the real owner so the fixture's lock can be safely destroyed.
  fake_owner_traits::current = 1;
  lock.unlock(n);
}

TEST_F(QueueSpinLockTest, RecursiveLockByCurrentOwnerTraps) {
  test_lock lock;
  test_lock::node n1;
  test_lock::node n2;
  lock.lock(n1);
  EXPECT_DEATH({ lock.lock(n2); }, "");
  lock.unlock(n1);
}

TEST_F(QueueSpinLockTest, DoubleUnlockTraps) {
  test_lock lock;
  test_lock::node n;
  lock.lock(n);
  lock.unlock(n);
  EXPECT_DEATH({ lock.unlock(n); }, "");
}

TEST_F(QueueSpinLockTest, DestroyingHeldLockTraps) {
  EXPECT_DEATH(
      {
        test_lock lock;
        test_lock::node n;
        lock.lock(n);
      },
      "");
}

TEST_F(QueueSpinLockTest, DestroyingUnlockedLockIsFine) {
  { test_lock lock; }
  SUCCEED();
}

TEST_F(QueueSpinLockTest, HandOffServesQueuedWaiterInOrderAcrossSequentialOwners) {
  test_lock lock;

  // Single-threaded sanity check mirroring ticket_spin_lock's own
  // sequential-ownership test: acquiring/releasing for distinct owners in
  // sequence always lets the next caller through immediately once the
  // previous one has unlocked -- real cross-thread hand-off is covered by
  // MutualExclusionHoldsAcrossConcurrentThreads below.
  test_lock::node n;

  fake_owner_traits::current = 1;
  lock.lock(n);
  lock.unlock(n);

  fake_owner_traits::current = 2;
  lock.lock(n);
  EXPECT_TRUE(lock.is_locked_by_current());
  lock.unlock(n);

  fake_owner_traits::current = 3;
  lock.lock(n);
  EXPECT_TRUE(lock.is_locked_by_current());
  lock.unlock(n);
}

TEST(QueueSpinLockConcurrencyTest, MutualExclusionHoldsAcrossConcurrentThreads) {
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

  queue_spin_lock<thread_owner_traits> lock;
  int shared_counter = 0;
  constexpr int num_threads = 4;
  constexpr int increments_per_thread = 2000;

  std::vector<std::thread> threads;
  threads.reserve(num_threads);
  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back([&] {
      // Each thread's node is its own stack-local -- the whole point of an
      // MCS-style lock is that no two waiters ever share one.
      queue_spin_lock<thread_owner_traits>::node n;
      for (int j = 0; j < increments_per_thread; ++j) {
        lock.lock(n);
        ++shared_counter; // only safe to do non-atomically because of the lock's mutual exclusion
        lock.unlock(n);
      }
    });
  }
  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(shared_counter, num_threads * increments_per_thread);
}

TEST(QueueSpinLockSoftlockTest, ContendedLockTrapsAfterSoftlockLimit) {
  using lowlimit_lock = queue_spin_lock<fake_owner_traits_low_softlock_limit>;

  lowlimit_lock lock;
  lowlimit_lock::node n1;
  fake_owner_traits_low_softlock_limit::current = 1;
  lock.lock(n1);

  // Owner 1 never releases, so owner 2's `lock()` spins forever on its own
  // node's local flag, against the softlock_detector wired into that
  // contended loop -- it should trap well before actually spinning forever.
  EXPECT_DEATH(
      {
        lowlimit_lock::node n2;
        fake_owner_traits_low_softlock_limit::current = 2;
        lock.lock(n2);
      },
      "");

  // Restore the real owner so the lock can be safely destroyed.
  fake_owner_traits_low_softlock_limit::current = 1;
  lock.unlock(n1);
}
