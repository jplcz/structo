// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/sync/queue_rw_spin_lock.hpp>
#include <structo/sync/softlock_detector.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using structo::sync::queue_rw_spin_lock;

namespace {

// Fake kernel backend: reports an adjustable "current thread" pointer-sized
// id so tests can simulate several distinct callers without real threads.
struct fake_owner_traits {
  using owner_type = std::uintptr_t;

  static inline owner_type current = 1;

  static owner_type current_owner() noexcept { return current; }
};

using test_lock = queue_rw_spin_lock<fake_owner_traits>;

// Fake kernel backend with a tiny softlock tick limit, to exercise the
// `softlock_detector` wired into `write_lock()`'s contended drain-spin
// without having to actually spin millions of times.
struct fake_owner_traits_low_softlock_limit {
  using owner_type = std::uintptr_t;

  static constexpr structo::sync::softlock_detector::counter_type softlock_limit = 4;

  static inline owner_type current = 1;

  static owner_type current_owner() noexcept { return current; }
};

/** @brief Fixture for `queue_rw_spin_lock` tests; resets the fake "current owner" before each test. */
class QueueRwSpinLockTest : public ::testing::Test {
protected:
  void SetUp() override { fake_owner_traits::current = 1; }
};

} // namespace

TEST_F(QueueRwSpinLockTest, StartsUnlocked) {
  test_lock lock;
  EXPECT_FALSE(lock.is_locked());
  EXPECT_FALSE(lock.is_read_locked());
  EXPECT_FALSE(lock.is_write_locked());
  EXPECT_EQ(lock.reader_count(), 0u);
}

TEST_F(QueueRwSpinLockTest, ReadLockAllowsMultipleConcurrentReaders) {
  test_lock lock;
  lock.read_lock();
  lock.read_lock();
  EXPECT_TRUE(lock.is_read_locked());
  EXPECT_EQ(lock.reader_count(), 2u);
  lock.read_unlock();
  EXPECT_EQ(lock.reader_count(), 1u);
  lock.read_unlock();
  EXPECT_FALSE(lock.is_read_locked());
}

TEST_F(QueueRwSpinLockTest, TryReadLockSucceedsWhenUnlocked) {
  test_lock lock;
  EXPECT_TRUE(lock.try_read_lock());
  EXPECT_EQ(lock.reader_count(), 1u);
  lock.read_unlock();
}

TEST_F(QueueRwSpinLockTest, TryReadLockFailsWhenWriteLocked) {
  test_lock lock;
  test_lock::node n;
  lock.write_lock(n);
  EXPECT_FALSE(lock.try_read_lock());
  lock.write_unlock(n);
}

TEST_F(QueueRwSpinLockTest, WriteLockAcquiresAndMarksCurrentAsOwner) {
  test_lock lock;
  test_lock::node n;
  lock.write_lock(n);
  EXPECT_TRUE(lock.is_write_locked());
  EXPECT_TRUE(lock.is_write_locked_by_current());
  EXPECT_TRUE(lock.is_locked());
  lock.write_unlock(n);
}

TEST_F(QueueRwSpinLockTest, WriteUnlockReleasesLock) {
  test_lock lock;
  test_lock::node n;
  lock.write_lock(n);
  lock.write_unlock(n);
  EXPECT_FALSE(lock.is_write_locked());
  EXPECT_FALSE(lock.is_locked());
}

TEST_F(QueueRwSpinLockTest, TryWriteLockSucceedsWhenUnlocked) {
  test_lock lock;
  test_lock::node n;
  EXPECT_TRUE(lock.try_write_lock(n));
  EXPECT_TRUE(lock.is_write_locked_by_current());
  lock.write_unlock(n);
}

TEST_F(QueueRwSpinLockTest, TryWriteLockFailsWhenReadLocked) {
  test_lock lock;
  test_lock::node n;
  lock.read_lock();
  EXPECT_FALSE(lock.try_write_lock(n));
  lock.read_unlock();
}

TEST_F(QueueRwSpinLockTest, TryWriteLockFailsWhenWriteLockedByAnotherOwner) {
  test_lock lock;
  test_lock::node n1;
  test_lock::node n2;
  fake_owner_traits::current = 1;
  lock.write_lock(n1);

  fake_owner_traits::current = 2;
  EXPECT_FALSE(lock.try_write_lock(n2));

  fake_owner_traits::current = 1;
  lock.write_unlock(n1);
}

TEST_F(QueueRwSpinLockTest, IsWriteLockedByCurrentReflectsWhichOwnerLocked) {
  test_lock lock;
  test_lock::node n;
  fake_owner_traits::current = 1;
  lock.write_lock(n);

  fake_owner_traits::current = 2;
  EXPECT_TRUE(lock.is_write_locked());
  EXPECT_FALSE(lock.is_write_locked_by_current());

  fake_owner_traits::current = 1;
  EXPECT_TRUE(lock.is_write_locked_by_current());
  lock.write_unlock(n);
}

TEST_F(QueueRwSpinLockTest, WriteUnlockByNonOwnerTraps) {
  test_lock lock;
  test_lock::node n;
  fake_owner_traits::current = 1;
  lock.write_lock(n);

  fake_owner_traits::current = 2;
  EXPECT_DEATH({ lock.write_unlock(n); }, "");

  // Restore the real owner so the fixture's lock can be safely destroyed.
  fake_owner_traits::current = 1;
  lock.write_unlock(n);
}

TEST_F(QueueRwSpinLockTest, RecursiveWriteLockByCurrentOwnerTraps) {
  test_lock lock;
  test_lock::node n1;
  test_lock::node n2;
  lock.write_lock(n1);
  EXPECT_DEATH({ lock.write_lock(n2); }, "");
  lock.write_unlock(n1);
}

TEST_F(QueueRwSpinLockTest, DoubleWriteUnlockTraps) {
  test_lock lock;
  test_lock::node n;
  lock.write_lock(n);
  lock.write_unlock(n);
  EXPECT_DEATH({ lock.write_unlock(n); }, "");
}

TEST_F(QueueRwSpinLockTest, ReadUnlockWithNoActiveReadersTraps) {
  test_lock lock;
  EXPECT_DEATH({ lock.read_unlock(); }, "");
}

TEST_F(QueueRwSpinLockTest, DestroyingWriteHeldLockTraps) {
  EXPECT_DEATH(
      {
        test_lock lock;
        test_lock::node n;
        lock.write_lock(n);
      },
      "");
}

TEST_F(QueueRwSpinLockTest, DestroyingReadHeldLockTraps) {
  EXPECT_DEATH(
      {
        test_lock lock;
        lock.read_lock();
      },
      "");
}

TEST_F(QueueRwSpinLockTest, DestroyingUnlockedLockIsFine) {
  {
    test_lock lock;
  }
  SUCCEED();
}

TEST(QueueRwSpinLockConcurrencyTest, WriterWaitingBlocksNewReadersUntilDrained) {
  struct thread_owner_traits {
    using owner_type = std::uintptr_t;

    static owner_type current_owner() noexcept {
      thread_local int marker = 0;
      return reinterpret_cast<std::uintptr_t>(&marker);
    }
  };

  queue_rw_spin_lock<thread_owner_traits> lock;
  lock.read_lock();

  std::atomic<bool> writer_done{false};
  std::thread writer([&] {
    queue_rw_spin_lock<thread_owner_traits>::node n;
    lock.write_lock(n);
    writer_done.store(true, std::memory_order_release);
    lock.write_unlock(n);
  });

  // Give the writer a moment to win admission and announce intent (set
  // the waiting bit); not required for correctness below, just makes the
  // "blocks new readers" assertion meaningful rather than a lucky race.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  EXPECT_FALSE(writer_done.load(std::memory_order_acquire));
  EXPECT_FALSE(lock.try_read_lock()) << "a new reader must be refused once a writer is waiting";

  lock.read_unlock();
  writer.join();
  EXPECT_TRUE(writer_done.load(std::memory_order_acquire));
}

TEST(QueueRwSpinLockConcurrencyTest, MutualExclusionHoldsAcrossConcurrentReadersAndWriters) {
  struct thread_owner_traits {
    using owner_type = std::uintptr_t;

    static owner_type current_owner() noexcept {
      thread_local int marker = 0;
      return reinterpret_cast<std::uintptr_t>(&marker);
    }
  };

  queue_rw_spin_lock<thread_owner_traits> lock;
  // Two halves of a value that must always be observed equal by any
  // reader: a writer-side bug (missing exclusion) would eventually let a
  // reader observe them out of sync.
  int first = 0;
  int second = 0;

  constexpr int num_writers = 4;
  constexpr int num_readers = 4;
  constexpr int writes_per_thread = 1000;
  std::atomic<bool> stop{false};
  std::atomic<int> mismatches{0};
  std::atomic<int> total_writes{0};

  std::vector<std::thread> threads;
  threads.reserve(num_writers + num_readers);
  for (int i = 0; i < num_writers; ++i) {
    threads.emplace_back([&] {
      queue_rw_spin_lock<thread_owner_traits>::node n;
      for (int j = 0; j < writes_per_thread; ++j) {
        lock.write_lock(n);
        ++first;
        ++second;
        lock.write_unlock(n);
        total_writes.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (int i = 0; i < num_readers; ++i) {
    threads.emplace_back([&] {
      while (!stop.load(std::memory_order_acquire)) {
        lock.read_lock();
        if (first != second) {
          mismatches.fetch_add(1, std::memory_order_relaxed);
        }
        lock.read_unlock();
      }
    });
  }

  for (int i = 0; i < num_writers; ++i) {
    threads[static_cast<std::size_t>(i)].join();
  }
  stop.store(true, std::memory_order_release);
  for (int i = num_writers; i < num_writers + num_readers; ++i) {
    threads[static_cast<std::size_t>(i)].join();
  }

  EXPECT_EQ(mismatches.load(std::memory_order_relaxed), 0);
  EXPECT_EQ(total_writes.load(std::memory_order_relaxed), num_writers * writes_per_thread);
  EXPECT_EQ(first, num_writers * writes_per_thread);
  EXPECT_EQ(second, num_writers * writes_per_thread);
}

TEST(QueueRwSpinLockSoftlockTest, ContendedWriteLockTrapsAfterSoftlockLimitWhileReaderHeld) {
  using lowlimit_lock = queue_rw_spin_lock<fake_owner_traits_low_softlock_limit>;

  lowlimit_lock lock;
  lock.read_lock();

  // The reader never releases, so the writer is admitted immediately
  // (no other writer is queued) but then spins forever in the drain
  // loop waiting for the reader count to reach zero -- against this
  // lock's own softlock_detector wired into that loop. It should trap
  // well before actually spinning forever.
  EXPECT_DEATH(
      {
        lowlimit_lock::node n;
        fake_owner_traits_low_softlock_limit::current = 1;
        lock.write_lock(n);
      },
      "");

  lock.read_unlock();
}

TEST(QueueRwSpinLockSoftlockTest, ContendedWriteLockTrapsAfterSoftlockLimitInAdmissionQueue) {
  using lowlimit_lock = queue_rw_spin_lock<fake_owner_traits_low_softlock_limit>;

  lowlimit_lock lock;
  lowlimit_lock::node n1;
  fake_owner_traits_low_softlock_limit::current = 1;
  lock.write_lock(n1);

  // Owner 1 never releases, so owner 2's `write_lock()` spins forever
  // inside the writer admission queue itself (owner 1 still holds it) --
  // against the `softlock_detector` that `queue_spin_lock::lock()`
  // already wires into its own contended spin. It should trap well
  // before actually spinning forever.
  EXPECT_DEATH(
      {
        lowlimit_lock::node n2;
        fake_owner_traits_low_softlock_limit::current = 2;
        lock.write_lock(n2);
      },
      "");

  // Restore the real owner so the lock can be safely destroyed.
  fake_owner_traits_low_softlock_limit::current = 1;
  lock.write_unlock(n1);
}
