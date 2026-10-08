// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <reloco/lifetime.hpp>
#include <structo/sync/rw_spin_lock.hpp>
#include <structo/sync/softlock_detector.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using structo::sync::rw_spin_lock;

namespace {

// Fake kernel backend: reports an adjustable "current thread" pointer-sized
// id so tests can simulate several distinct callers without real threads.
struct fake_owner_traits {
  using owner_type = std::uintptr_t;

  static inline owner_type current = 1;

  static owner_type current_owner() noexcept { return current; }
};

using test_lock = rw_spin_lock<fake_owner_traits>;

// Fake kernel backend with a tiny softlock tick limit, to exercise the
// `softlock_detector` wired into `write_lock()`'s contended spin without
// having to actually spin millions of times.
struct fake_owner_traits_low_softlock_limit {
  using owner_type = std::uintptr_t;

  static constexpr structo::sync::softlock_detector::counter_type softlock_limit = 4;

  static inline owner_type current = 1;

  static owner_type current_owner() noexcept { return current; }
};

/** @brief Fixture for `rw_spin_lock` tests; resets the fake "current owner" before each test. */
class RwSpinLockTest : public ::testing::Test {
protected:
  void SetUp() override { fake_owner_traits::current = 1; }
};

} // namespace

TEST_F(RwSpinLockTest, StartsUnlocked) {
  test_lock lock;
  EXPECT_FALSE(lock.is_locked());
  EXPECT_FALSE(lock.is_read_locked());
  EXPECT_FALSE(lock.is_write_locked());
  EXPECT_EQ(lock.reader_count(), 0u);
}

TEST_F(RwSpinLockTest, ReadLockAllowsMultipleConcurrentReaders) {
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

TEST_F(RwSpinLockTest, TryReadLockSucceedsWhenUnlocked) {
  test_lock lock;
  EXPECT_TRUE(lock.try_read_lock());
  EXPECT_EQ(lock.reader_count(), 1u);
  lock.read_unlock();
}

TEST_F(RwSpinLockTest, TryReadLockFailsWhenWriteLocked) {
  test_lock lock;
  lock.write_lock();
  EXPECT_FALSE(lock.try_read_lock());
  lock.write_unlock();
}

TEST_F(RwSpinLockTest, WriteLockAcquiresAndMarksCurrentAsOwner) {
  test_lock lock;
  lock.write_lock();
  EXPECT_TRUE(lock.is_write_locked());
  EXPECT_TRUE(lock.is_write_locked_by_current());
  EXPECT_TRUE(lock.is_locked());
  lock.write_unlock();
}

TEST_F(RwSpinLockTest, WriteUnlockReleasesLock) {
  test_lock lock;
  lock.write_lock();
  lock.write_unlock();
  EXPECT_FALSE(lock.is_write_locked());
  EXPECT_FALSE(lock.is_locked());
}

TEST_F(RwSpinLockTest, TryWriteLockSucceedsWhenUnlocked) {
  test_lock lock;
  EXPECT_TRUE(lock.try_write_lock());
  EXPECT_TRUE(lock.is_write_locked_by_current());
  lock.write_unlock();
}

TEST_F(RwSpinLockTest, TryWriteLockFailsWhenReadLocked) {
  test_lock lock;
  lock.read_lock();
  EXPECT_FALSE(lock.try_write_lock());
  lock.read_unlock();
}

TEST_F(RwSpinLockTest, TryWriteLockFailsWhenWriteLockedByAnotherOwner) {
  test_lock lock;
  fake_owner_traits::current = 1;
  lock.write_lock();

  fake_owner_traits::current = 2;
  EXPECT_FALSE(lock.try_write_lock());

  fake_owner_traits::current = 1;
  lock.write_unlock();
}

TEST_F(RwSpinLockTest, IsWriteLockedByCurrentReflectsWhichOwnerLocked) {
  test_lock lock;
  fake_owner_traits::current = 1;
  lock.write_lock();

  fake_owner_traits::current = 2;
  EXPECT_TRUE(lock.is_write_locked());
  EXPECT_FALSE(lock.is_write_locked_by_current());

  fake_owner_traits::current = 1;
  EXPECT_TRUE(lock.is_write_locked_by_current());
  lock.write_unlock();
}

TEST_F(RwSpinLockTest, WriteUnlockByNonOwnerTraps) {
  test_lock lock;
  fake_owner_traits::current = 1;
  lock.write_lock();

  fake_owner_traits::current = 2;
  // gtest's death-test macro expands to libc fprintf calls outside our control.
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  EXPECT_DEATH({ lock.write_unlock(); }, "");
  RELOCO_END_UNSAFE_BUFFER_USAGE

  // Restore the real owner so the fixture's lock can be safely destroyed.
  fake_owner_traits::current = 1;
  lock.write_unlock();
}

TEST_F(RwSpinLockTest, RecursiveWriteLockByCurrentOwnerTraps) {
  test_lock lock;
  lock.write_lock();
  // gtest's death-test macro expands to libc fprintf calls outside our control.
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  EXPECT_DEATH({ lock.write_lock(); }, "");
  RELOCO_END_UNSAFE_BUFFER_USAGE
  lock.write_unlock();
}

TEST_F(RwSpinLockTest, DoubleWriteUnlockTraps) {
  test_lock lock;
  lock.write_lock();
  lock.write_unlock();
  // gtest's death-test macro expands to libc fprintf calls outside our control.
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  EXPECT_DEATH({ lock.write_unlock(); }, "");
  RELOCO_END_UNSAFE_BUFFER_USAGE
}

TEST_F(RwSpinLockTest, ReadUnlockWithNoActiveReadersTraps) {
  test_lock lock;
  // gtest's death-test macro expands to libc fprintf calls outside our control.
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  EXPECT_DEATH({ lock.read_unlock(); }, "");
  RELOCO_END_UNSAFE_BUFFER_USAGE
}

TEST_F(RwSpinLockTest, DestroyingWriteHeldLockTraps) {
  // gtest's death-test macro expands to libc fprintf calls outside our control.
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  EXPECT_DEATH(
      {
        test_lock lock;
        lock.write_lock();
      },
      "");
  RELOCO_END_UNSAFE_BUFFER_USAGE
}

TEST_F(RwSpinLockTest, DestroyingReadHeldLockTraps) {
  // gtest's death-test macro expands to libc fprintf calls outside our control.
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  EXPECT_DEATH(
      {
        test_lock lock;
        lock.read_lock();
      },
      "");
  RELOCO_END_UNSAFE_BUFFER_USAGE
}

TEST_F(RwSpinLockTest, DestroyingUnlockedLockIsFine) {
  {
    test_lock lock;
  }
  SUCCEED();
}

TEST(RwSpinLockConcurrencyTest, WriterWaitingBlocksNewReadersUntilDrained) {
  struct thread_owner_traits {
    using owner_type = std::uintptr_t;

    static owner_type current_owner() noexcept {
      thread_local int marker = 0;
      return reinterpret_cast<std::uintptr_t>(&marker);
    }
  };

  rw_spin_lock<thread_owner_traits> lock;
  lock.read_lock();

  std::atomic<bool> writer_done{false};
  std::thread writer([&] {
    lock.write_lock();
    writer_done.store(true, std::memory_order_release);
    lock.write_unlock();
  });

  // Give the writer a moment to announce intent (set the waiting bit);
  // not required for correctness below, just makes the "blocks new
  // readers" assertion meaningful rather than a lucky race.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  EXPECT_FALSE(writer_done.load(std::memory_order_acquire));
  EXPECT_FALSE(lock.try_read_lock()) << "a new reader must be refused once a writer is waiting";

  lock.read_unlock();
  writer.join();
  EXPECT_TRUE(writer_done.load(std::memory_order_acquire));
}

TEST(RwSpinLockConcurrencyTest, MutualExclusionHoldsAcrossConcurrentReadersAndWriters) {
  struct thread_owner_traits {
    using owner_type = std::uintptr_t;

    static owner_type current_owner() noexcept {
      thread_local int marker = 0;
      return reinterpret_cast<std::uintptr_t>(&marker);
    }
  };

  rw_spin_lock<thread_owner_traits> lock;
  // Two halves of a value that must always be observed equal by any
  // reader: a writer-side bug (missing exclusion) would eventually let a
  // reader observe them out of sync.
  int first = 0;
  int second = 0;

  constexpr int num_writers = 2;
  constexpr int num_readers = 4;
  constexpr int writes_per_thread = 1000;
  std::atomic<bool> stop{false};
  std::atomic<int> mismatches{0};

  std::vector<std::thread> threads;
  threads.reserve(num_writers + num_readers);
  for (int i = 0; i < num_writers; ++i) {
    threads.emplace_back([&] {
      for (int j = 0; j < writes_per_thread; ++j) {
        lock.write_lock();
        ++first;
        ++second;
        lock.write_unlock();
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
  EXPECT_EQ(first, num_writers * writes_per_thread);
  EXPECT_EQ(second, num_writers * writes_per_thread);
}

TEST(RwSpinLockSoftlockTest, ContendedWriteLockTrapsAfterSoftlockLimit) {
  using lowlimit_lock = rw_spin_lock<fake_owner_traits_low_softlock_limit>;

  lowlimit_lock lock;
  fake_owner_traits_low_softlock_limit::current = 1;
  lock.write_lock();

  // Owner 1 never releases, so owner 2's `write_lock()` spins forever
  // against the softlock_detector wired into the contended loop -- it
  // should trap well before actually spinning forever.
  fake_owner_traits_low_softlock_limit::current = 2;
  // gtest's death-test macro expands to libc fprintf calls outside our control.
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  EXPECT_DEATH({ lock.write_lock(); }, "");
  RELOCO_END_UNSAFE_BUFFER_USAGE

  // Restore the real owner so the lock can be safely destroyed.
  fake_owner_traits_low_softlock_limit::current = 1;
  lock.write_unlock();
}
