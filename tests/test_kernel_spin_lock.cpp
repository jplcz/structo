// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <reloco/lifetime.hpp>
#include <structo/sync/kernel_spin_lock.hpp>
#include <structo/sync/softlock_detector.hpp>

#include <cstdint>

using structo::sync::kernel_spin_lock;

namespace {

// Fake kernel backend: reports an adjustable "current thread" pointer-sized
// id so tests can simulate several distinct callers without real threads.
struct fake_owner_traits {
  using owner_type = std::uintptr_t;

  static inline owner_type current = 1;

  static owner_type current_owner() noexcept { return current; }
};

using test_lock = kernel_spin_lock<fake_owner_traits>;

// Fake kernel backend with a tiny softlock tick limit, to exercise the
// `softlock_detector` wired into `lock()`'s contended spin without
// having to actually spin millions of times.
struct fake_owner_traits_low_softlock_limit {
  using owner_type = std::uintptr_t;

  static constexpr structo::sync::softlock_detector::counter_type softlock_limit = 4;

  static inline owner_type current = 1;

  static owner_type current_owner() noexcept { return current; }
};

/** @brief Fixture for `kernel_spin_lock` tests; resets the fake "current owner" before each test. */
class KernelSpinLockTest : public ::testing::Test {
protected:
  void SetUp() override { fake_owner_traits::current = 1; }
};

} // namespace

TEST_F(KernelSpinLockTest, StartsUnlocked) {
  test_lock lock;
  EXPECT_FALSE(lock.is_locked());
  EXPECT_FALSE(lock.is_locked_by_current());
}

TEST_F(KernelSpinLockTest, LockAcquiresAndMarksCurrentAsOwner) {
  test_lock lock;
  lock.lock();
  EXPECT_TRUE(lock.is_locked());
  EXPECT_TRUE(lock.is_locked_by_current());
  lock.unlock();
}

TEST_F(KernelSpinLockTest, UnlockReleasesLock) {
  test_lock lock;
  lock.lock();
  lock.unlock();
  EXPECT_FALSE(lock.is_locked());
  EXPECT_FALSE(lock.is_locked_by_current());
}

TEST_F(KernelSpinLockTest, TryLockSucceedsWhenUnlocked) {
  test_lock lock;
  EXPECT_TRUE(lock.try_lock());
  EXPECT_TRUE(lock.is_locked_by_current());
  lock.unlock();
}

TEST_F(KernelSpinLockTest, TryLockFailsWhenHeldByAnotherOwner) {
  test_lock lock;
  fake_owner_traits::current = 1;
  lock.lock();

  fake_owner_traits::current = 2;
  EXPECT_FALSE(lock.try_lock());

  fake_owner_traits::current = 1;
  lock.unlock();
}

TEST_F(KernelSpinLockTest, IsLockedByCurrentReflectsWhichOwnerLocked) {
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

TEST_F(KernelSpinLockTest, UnlockByNonOwnerTraps) {
  test_lock lock;
  fake_owner_traits::current = 1;
  lock.lock();

  fake_owner_traits::current = 2;
  // gtest's death-test macro expands to libc fprintf calls outside our control.
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  EXPECT_DEATH({ lock.unlock(); }, "");
  RELOCO_END_UNSAFE_BUFFER_USAGE

  // Restore the real owner so the fixture's lock can be safely destroyed.
  fake_owner_traits::current = 1;
  lock.unlock();
}

TEST_F(KernelSpinLockTest, RecursiveLockByCurrentOwnerTraps) {
  test_lock lock;
  lock.lock();
  // gtest's death-test macro expands to libc fprintf calls outside our control.
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  EXPECT_DEATH({ lock.lock(); }, "");
  RELOCO_END_UNSAFE_BUFFER_USAGE
  lock.unlock();
}

TEST_F(KernelSpinLockTest, DoubleUnlockTraps) {
  test_lock lock;
  lock.lock();
  lock.unlock();
  // gtest's death-test macro expands to libc fprintf calls outside our control.
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  EXPECT_DEATH({ lock.unlock(); }, "");
  RELOCO_END_UNSAFE_BUFFER_USAGE
}

TEST_F(KernelSpinLockTest, DestroyingHeldLockTraps) {
  // gtest's death-test macro expands to libc fprintf calls outside our control.
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  EXPECT_DEATH(
      {
        test_lock lock;
        lock.lock();
      },
      "");
  RELOCO_END_UNSAFE_BUFFER_USAGE
}

TEST_F(KernelSpinLockTest, DestroyingUnlockedLockIsFine) {
  {
    test_lock lock;
  }
  SUCCEED();
}

TEST_F(KernelSpinLockTest, LockSpinsUntilOwnerReleases) {
  test_lock lock;
  fake_owner_traits::current = 1;
  lock.lock();
  // Simulate the holder releasing before a second `lock()` call for a
  // different owner would observe it -- single-threaded, so this just
  // exercises the "already free by the time we check again" path.
  lock.unlock();

  fake_owner_traits::current = 2;
  lock.lock();
  EXPECT_TRUE(lock.is_locked_by_current());
  lock.unlock();
}

TEST(KernelSpinLockSoftlockTest, ContendedLockTrapsAfterSoftlockLimit) {
  using lowlimit_lock = kernel_spin_lock<fake_owner_traits_low_softlock_limit>;

  lowlimit_lock lock;
  fake_owner_traits_low_softlock_limit::current = 1;
  lock.lock();

  // Owner 1 never releases, so owner 2's `lock()` spins forever against
  // the softlock_detector wired into the contended loop -- it should
  // trap well before actually spinning forever.
  fake_owner_traits_low_softlock_limit::current = 2;
  // gtest's death-test macro expands to libc fprintf calls outside our control.
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  EXPECT_DEATH({ lock.lock(); }, "");
  RELOCO_END_UNSAFE_BUFFER_USAGE

  // Restore the real owner so the lock can be safely destroyed.
  fake_owner_traits_low_softlock_limit::current = 1;
  lock.unlock();
}
