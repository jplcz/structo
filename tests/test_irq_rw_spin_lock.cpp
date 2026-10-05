// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/sync/irq_guard.hpp>
#include <structo/sync/irq_rw_spin_lock.hpp>
#include <structo/sync/queue_rw_spin_lock.hpp>
#include <structo/sync/rw_spin_lock.hpp>

#include <cstdint>
#include <utility>

using structo::sync::irq_guard;
using structo::sync::irq_rw_spin_lock;
using structo::sync::queue_rw_spin_lock;
using structo::sync::rw_spin_lock;

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

using rw_irq_guard_lock = irq_rw_spin_lock<rw_spin_lock<fake_owner_traits>, irq_guard<fake_irq_traits>>;
using queue_rw_irq_guard_lock = irq_rw_spin_lock<queue_rw_spin_lock<fake_owner_traits>, irq_guard<fake_irq_traits>>;

/** @brief Fixture for `irq_rw_spin_lock` tests; resets the fake backends' mutable static state before each test. */
class IrqRwSpinLockTest : public ::testing::Test {
protected:
  void SetUp() override {
    fake_owner_traits::current = 1;
    fake_irq_traits::reset();
  }
};

} // namespace

TEST_F(IrqRwSpinLockTest, StartsUnlockedWithIrqsNotEngaged) {
  rw_irq_guard_lock lock;
  EXPECT_FALSE(lock.is_locked());
  EXPECT_EQ(fake_irq_traits::save_count, 0);
}

TEST_F(IrqRwSpinLockTest, ReadLockEngagesIrqGuardAndAllowsMultipleReaders) {
  rw_irq_guard_lock lock;
  auto g1 = lock.read_lock();
  auto g2 = lock.read_lock();
  EXPECT_EQ(lock.reader_count(), 2u);
  EXPECT_EQ(fake_irq_traits::save_count, 2);

  g1.unlock();
  EXPECT_EQ(lock.reader_count(), 1u);
  EXPECT_EQ(fake_irq_traits::restore_count, 1);

  g2.unlock();
  EXPECT_EQ(lock.reader_count(), 0u);
  EXPECT_EQ(fake_irq_traits::restore_count, 2);
}

TEST_F(IrqRwSpinLockTest, WriteLockEngagesIrqGuardBeforeTakingTheWriterSide) {
  rw_irq_guard_lock lock;
  {
    auto g = lock.write_lock();
    EXPECT_TRUE(lock.is_write_locked());
    EXPECT_TRUE(lock.is_write_locked_by_current());
    EXPECT_EQ(fake_irq_traits::save_count, 1);
    EXPECT_EQ(fake_irq_traits::restore_count, 0);
  }
  EXPECT_FALSE(lock.is_write_locked());
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
}

TEST_F(IrqRwSpinLockTest, TryReadLockFailureReleasesIrqGuardImmediately) {
  rw_irq_guard_lock lock;
  auto held = lock.write_lock();

  auto maybe = lock.try_read_lock();
  EXPECT_FALSE(maybe.has_value());
  EXPECT_EQ(fake_irq_traits::save_count, 2);
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
}

TEST_F(IrqRwSpinLockTest, TryWriteLockFailureReleasesIrqGuardImmediately) {
  rw_irq_guard_lock lock;
  auto held = lock.read_lock();

  auto maybe = lock.try_write_lock();
  EXPECT_FALSE(maybe.has_value());
  EXPECT_EQ(fake_irq_traits::save_count, 2);
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
}

TEST_F(IrqRwSpinLockTest, TryReadLockAndTryWriteLockSucceedWhenFree) {
  rw_irq_guard_lock lock;

  auto maybe_r = lock.try_read_lock();
  ASSERT_TRUE(maybe_r.has_value());
  maybe_r->unlock();
  EXPECT_FALSE(lock.is_read_locked());

  auto maybe_w = lock.try_write_lock();
  ASSERT_TRUE(maybe_w.has_value());
  maybe_w->unlock();
  EXPECT_FALSE(lock.is_write_locked());
}

TEST_F(IrqRwSpinLockTest, WriteGuardIsMovable) {
  rw_irq_guard_lock lock;
  auto g = lock.write_lock();
  auto moved = std::move(g);
  EXPECT_TRUE(moved.is_locked());
  EXPECT_TRUE(lock.is_write_locked());
  moved.unlock();
  EXPECT_FALSE(lock.is_write_locked());
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
}

TEST_F(IrqRwSpinLockTest, QueueRwSpinLockWriteSideTakesACallerSuppliedNode) {
  queue_rw_irq_guard_lock lock;
  queue_rw_irq_guard_lock::node n;
  {
    auto g = lock.write_lock(n);
    EXPECT_TRUE(lock.is_write_locked());
    EXPECT_EQ(fake_irq_traits::save_count, 1);
  }
  EXPECT_FALSE(lock.is_write_locked());
  EXPECT_EQ(fake_irq_traits::restore_count, 1);

  queue_rw_irq_guard_lock::node n2;
  auto maybe = lock.try_write_lock(n2);
  ASSERT_TRUE(maybe.has_value());
  maybe->unlock();
  EXPECT_FALSE(lock.is_write_locked());
}

TEST_F(IrqRwSpinLockTest, QueueRwSpinLockReadSideTakesNoNode) {
  queue_rw_irq_guard_lock lock;
  auto g = lock.read_lock();
  EXPECT_TRUE(lock.is_read_locked());
  g.unlock();
  EXPECT_FALSE(lock.is_read_locked());
}
