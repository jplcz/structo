// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/sync/guarded_spin_mutex.hpp>
#include <structo/sync/irq_guard.hpp>
#include <structo/sync/kernel_spin_lock.hpp>
#include <structo/sync/queue_spin_lock.hpp>
#include <structo/sync/spinlock_entry_guard.hpp>
#include <structo/sync/ticket_spin_lock.hpp>

#include <cstdint>
#include <utility>

#include <reloco/lifetime.hpp>

// Test fixtures index raw buffers freely; bounds are checked by the assertions.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

using structo::sync::guarded_spin_mutex;
using structo::sync::irq_guard;
using structo::sync::kernel_spin_lock;
using structo::sync::queue_spin_lock;
using structo::sync::spinlock_entry_guard;
using structo::sync::ticket_spin_lock;

namespace {

// Fake kernel backend: reports an adjustable "current thread" pointer-sized
// id so tests can simulate several distinct callers without real threads.
struct fake_owner_traits {
  using owner_type = std::uintptr_t;

  static inline owner_type current = 1;

  static owner_type current_owner() noexcept { return current; }
};

// Fake architecture backend: tracks nesting via an incrementing "flags"
// counter and records every save/restore so tests can assert ordering
// without touching real CPSID/MSR-style instructions.
struct fake_irq_traits {
  using flags_type = int;

  static inline int depth = 0;
  static inline int save_count = 0;
  static inline int restore_count = 0;

  static flags_type hw_save_irqs() noexcept {
    ++save_count;
    return depth++;
  }

  static void hw_restore_irqs(flags_type flags) noexcept {
    ++restore_count;
    depth = flags;
  }

  static void reset() noexcept {
    depth = 0;
    save_count = 0;
    restore_count = 0;
  }
};

// Fake kernel backend for `spinlock_entry_guard`-flavored tests: tracks
// nesting via a plain counter instead of saved/restored flags.
struct fake_spinlock_entry_traits {
  static inline int depth = 0;

  static void spinlock_enter() noexcept { ++depth; }
  static void spinlock_exit() noexcept { --depth; }

  static void reset() noexcept { depth = 0; }
};

/** @brief Fixture for `guarded_spin_mutex` tests; resets all fake backends' mutable static state before each test. */
class GuardedSpinMutexTest : public ::testing::Test {
protected:
  void SetUp() override {
    fake_owner_traits::current = 1;
    fake_irq_traits::reset();
    fake_spinlock_entry_traits::reset();
  }
};

} // namespace

TEST_F(GuardedSpinMutexTest, DefaultConstructsTheProtectedValue) {
  guarded_spin_mutex<int, kernel_spin_lock<fake_owner_traits>> m;
  auto g = m.lock();
  EXPECT_EQ(*g, 0);
}

TEST_F(GuardedSpinMutexTest, ConstructsFromAValue) {
  guarded_spin_mutex<int, kernel_spin_lock<fake_owner_traits>> m(7);
  auto g = m.lock();
  EXPECT_EQ(*g, 7);
}

TEST_F(GuardedSpinMutexTest, PlainLockWithNoIrqLockerDefaultDoesNotTouchAnyBackend) {
  guarded_spin_mutex<int, kernel_spin_lock<fake_owner_traits>> m(1);
  {
    auto g = m.lock();
    *g += 1;
  }
  EXPECT_EQ(m.unsafe_get_mut(), 2);
  EXPECT_EQ(fake_irq_traits::save_count, 0);
}

TEST_F(GuardedSpinMutexTest, LockEngagesIrqGuardBeforeTakingTheSpinLock) {
  guarded_spin_mutex<int, kernel_spin_lock<fake_owner_traits>, irq_guard<fake_irq_traits>> m(0);
  {
    auto g = m.lock();
    EXPECT_TRUE(g.is_locked());
    EXPECT_EQ(fake_irq_traits::save_count, 1);
    EXPECT_EQ(fake_irq_traits::restore_count, 0);
    *g = 42;
  }
  // Guard destruction must release the spinlock first, then the irq_guard.
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
  EXPECT_EQ(m.unsafe_get_mut(), 42);
}

TEST_F(GuardedSpinMutexTest, UnlockIsIdempotent) {
  guarded_spin_mutex<int, kernel_spin_lock<fake_owner_traits>, irq_guard<fake_irq_traits>> m(0);
  auto g = m.lock();
  g.unlock();
  EXPECT_FALSE(g.is_locked());
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
  g.unlock();
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
}

TEST_F(GuardedSpinMutexTest, TryLockSucceedsWhenUnlockedAndEngagesIrqGuard) {
  guarded_spin_mutex<int, kernel_spin_lock<fake_owner_traits>, irq_guard<fake_irq_traits>> m(5);
  auto maybe = m.try_lock();
  ASSERT_TRUE(maybe.has_value());
  EXPECT_EQ(fake_irq_traits::save_count, 1);
  **maybe += 1;
  maybe->unlock();
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
  EXPECT_EQ(m.unsafe_get_mut(), 6);
}

TEST_F(GuardedSpinMutexTest, TryLockFailureReleasesIrqGuardImmediately) {
  guarded_spin_mutex<int, kernel_spin_lock<fake_owner_traits>, irq_guard<fake_irq_traits>> m(0);
  auto held = m.lock();

  fake_owner_traits::current = 2;
  auto maybe = m.try_lock();
  EXPECT_FALSE(maybe.has_value());
  // The failed attempt's own irq_guard must already have been released,
  // even though the outer `held` guard's irq_guard is still engaged.
  EXPECT_EQ(fake_irq_traits::save_count, 2);
  EXPECT_EQ(fake_irq_traits::restore_count, 1);

  fake_owner_traits::current = 1;
}

TEST_F(GuardedSpinMutexTest, GuardIsMovable) {
  guarded_spin_mutex<int, kernel_spin_lock<fake_owner_traits>, irq_guard<fake_irq_traits>> m(1);
  auto g = m.lock();
  auto moved = std::move(g);
  EXPECT_TRUE(moved.is_locked());
  EXPECT_FALSE(g.is_locked());
  *moved += 1;
  // The moved-from guard must be a no-op on destruction.
  moved.unlock();
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
  EXPECT_EQ(m.unsafe_get_mut(), 2);
}

TEST_F(GuardedSpinMutexTest, TicketSpinLockWithIrqGuard) {
  guarded_spin_mutex<int, ticket_spin_lock<fake_owner_traits>, irq_guard<fake_irq_traits>> m(0);
  {
    auto g = m.lock();
    EXPECT_EQ(fake_irq_traits::save_count, 1);
    *g = 9;
  }
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
  EXPECT_EQ(m.unsafe_get_mut(), 9);
}

TEST_F(GuardedSpinMutexTest, QueueSpinLockTakesACallerSuppliedNode) {
  guarded_spin_mutex<int, queue_spin_lock<fake_owner_traits>, irq_guard<fake_irq_traits>> m(10);
  EXPECT_TRUE(decltype(m)::uses_node);

  decltype(m)::node n;
  {
    auto g = m.lock(n);
    EXPECT_EQ(fake_irq_traits::save_count, 1);
    *g += 5;
  }
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
  EXPECT_EQ(m.unsafe_get_mut(), 15);

  decltype(m)::node n2;
  auto maybe = m.try_lock(n2);
  ASSERT_TRUE(maybe.has_value());
  **maybe += 1;
  maybe->unlock();
  EXPECT_EQ(m.unsafe_get_mut(), 16);
}

TEST_F(GuardedSpinMutexTest, PlainSpinLocksDoNotUseANode) {
  EXPECT_FALSE(decltype(guarded_spin_mutex<int, kernel_spin_lock<fake_owner_traits>>{})::uses_node);
  EXPECT_FALSE(decltype(guarded_spin_mutex<int, ticket_spin_lock<fake_owner_traits>>{})::uses_node);
}

TEST_F(GuardedSpinMutexTest, SpinlockEntryGuardFlavorEntersAndExitsTheSection) {
  guarded_spin_mutex<int, kernel_spin_lock<fake_owner_traits>, spinlock_entry_guard<fake_spinlock_entry_traits>> m(0);
  {
    auto g = m.lock();
    EXPECT_EQ(fake_spinlock_entry_traits::depth, 1);
    *g = 3;
  }
  EXPECT_EQ(fake_spinlock_entry_traits::depth, 0);
  EXPECT_EQ(m.unsafe_get_mut(), 3);
}

TEST_F(GuardedSpinMutexTest, UnsafeGetMutGivesDirectAccess) {
  guarded_spin_mutex<int, kernel_spin_lock<fake_owner_traits>> m(1);
  m.unsafe_get_mut() = 100;
  auto g = m.lock();
  EXPECT_EQ(*g, 100);
}

TEST_F(GuardedSpinMutexTest, GuardGetAndGetMutMatchOperatorStar) {
  guarded_spin_mutex<int, kernel_spin_lock<fake_owner_traits>> m(5);
  auto g = m.lock();
  EXPECT_EQ(g.get(), 5);
  g.get_mut() += 1;
  EXPECT_EQ(g.get(), 6);
  EXPECT_EQ(*g, 6);
}

RELOCO_END_UNSAFE_BUFFER_USAGE