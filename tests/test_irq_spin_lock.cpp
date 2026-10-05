// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/sync/irq_guard.hpp>
#include <structo/sync/irq_spin_lock.hpp>
#include <structo/sync/kernel_spin_lock.hpp>
#include <structo/sync/queue_spin_lock.hpp>
#include <structo/sync/spinlock_entry_guard.hpp>
#include <structo/sync/ticket_spin_lock.hpp>

#include <cstdint>
#include <utility>

using structo::sync::irq_guard;
using structo::sync::irq_spin_lock;
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

using irq_guard_lock = irq_spin_lock<kernel_spin_lock<fake_owner_traits>, irq_guard<fake_irq_traits>>;
using ticket_irq_guard_lock = irq_spin_lock<ticket_spin_lock<fake_owner_traits>, irq_guard<fake_irq_traits>>;
using queue_irq_guard_lock = irq_spin_lock<queue_spin_lock<fake_owner_traits>, irq_guard<fake_irq_traits>>;
using entry_guard_lock = irq_spin_lock<kernel_spin_lock<fake_owner_traits>, spinlock_entry_guard<fake_spinlock_entry_traits>>;

/** @brief Fixture for `irq_spin_lock` tests; resets both fake backends' mutable static state before each test. */
class IrqSpinLockTest : public ::testing::Test {
protected:
  void SetUp() override {
    fake_owner_traits::current = 1;
    fake_irq_traits::reset();
    fake_spinlock_entry_traits::reset();
  }
};

} // namespace

TEST_F(IrqSpinLockTest, StartsUnlockedWithIrqsNotEngaged) {
  irq_guard_lock lock;
  EXPECT_FALSE(lock.is_locked());
  EXPECT_EQ(fake_irq_traits::save_count, 0);
}

TEST_F(IrqSpinLockTest, LockEngagesIrqGuardBeforeTakingTheSpinLock) {
  irq_guard_lock lock;
  {
    auto g = lock.lock();
    EXPECT_TRUE(lock.is_locked());
    EXPECT_TRUE(lock.is_locked_by_current());
    EXPECT_EQ(fake_irq_traits::save_count, 1);
    EXPECT_EQ(fake_irq_traits::restore_count, 0);
  }
  // Guard destruction must release the spinlock first, then the irq_guard.
  EXPECT_FALSE(lock.is_locked());
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
}

TEST_F(IrqSpinLockTest, UnlockIsIdempotent) {
  irq_guard_lock lock;
  auto g = lock.lock();
  g.unlock();
  EXPECT_FALSE(lock.is_locked());
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
  g.unlock();
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
}

TEST_F(IrqSpinLockTest, TryLockSucceedsWhenUnlockedAndEngagesIrqGuard) {
  irq_guard_lock lock;
  auto maybe = lock.try_lock();
  ASSERT_TRUE(maybe.has_value());
  EXPECT_TRUE(lock.is_locked());
  EXPECT_EQ(fake_irq_traits::save_count, 1);
  maybe->unlock();
  EXPECT_FALSE(lock.is_locked());
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
}

TEST_F(IrqSpinLockTest, TryLockFailureReleasesIrqGuardImmediately) {
  irq_guard_lock lock;
  auto held = lock.lock();

  fake_owner_traits::current = 2;
  auto maybe = lock.try_lock();
  EXPECT_FALSE(maybe.has_value());
  // The failed attempt's own irq_guard must already have been released,
  // even though the outer `held` guard's irq_guard is still engaged.
  EXPECT_EQ(fake_irq_traits::save_count, 2);
  EXPECT_EQ(fake_irq_traits::restore_count, 1);

  fake_owner_traits::current = 1;
}

TEST_F(IrqSpinLockTest, GuardIsMovable) {
  irq_guard_lock lock;
  auto g = lock.lock();
  auto moved = std::move(g);
  EXPECT_TRUE(moved.is_locked());
  EXPECT_TRUE(lock.is_locked());
  // The moved-from guard must be a no-op on destruction.
  moved.unlock();
  EXPECT_FALSE(lock.is_locked());
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
}

TEST_F(IrqSpinLockTest, TicketSpinLockWithIrqGuard) {
  ticket_irq_guard_lock lock;
  {
    auto g = lock.lock();
    EXPECT_TRUE(lock.is_locked());
    EXPECT_EQ(fake_irq_traits::save_count, 1);
  }
  EXPECT_FALSE(lock.is_locked());
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
}

TEST_F(IrqSpinLockTest, QueueSpinLockWithIrqGuardTakesACallerSuppliedNode) {
  queue_irq_guard_lock lock;
  queue_irq_guard_lock::node n;
  {
    auto g = lock.lock(n);
    EXPECT_TRUE(lock.is_locked());
    EXPECT_EQ(fake_irq_traits::save_count, 1);
  }
  EXPECT_FALSE(lock.is_locked());
  EXPECT_EQ(fake_irq_traits::restore_count, 1);

  queue_irq_guard_lock::node n2;
  auto maybe = lock.try_lock(n2);
  ASSERT_TRUE(maybe.has_value());
  maybe->unlock();
  EXPECT_FALSE(lock.is_locked());
}

TEST_F(IrqSpinLockTest, SpinlockEntryGuardFlavorEntersAndExitsTheSection) {
  entry_guard_lock lock;
  {
    auto g = lock.lock();
    EXPECT_TRUE(lock.is_locked());
    EXPECT_EQ(fake_spinlock_entry_traits::depth, 1);
  }
  EXPECT_FALSE(lock.is_locked());
  EXPECT_EQ(fake_spinlock_entry_traits::depth, 0);
}
