// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/sync/guarded_rw_spin_mutex.hpp>
#include <structo/sync/irq_guard.hpp>
#include <structo/sync/queue_rw_spin_lock.hpp>
#include <structo/sync/rw_spin_lock.hpp>
#include <structo/sync/spinlock_entry_guard.hpp>

#include <cstdint>
#include <utility>

#include <reloco/lifetime.hpp>

// Test fixtures index raw buffers freely; bounds are checked by the assertions.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

using structo::sync::guarded_rw_spin_mutex;
using structo::sync::irq_guard;
using structo::sync::queue_rw_spin_lock;
using structo::sync::rw_spin_lock;
using structo::sync::spinlock_entry_guard;

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

/** @brief Fixture for `guarded_rw_spin_mutex` tests; resets all fake backends' mutable static state before each test.
 */
class GuardedRwSpinMutexTest : public ::testing::Test {
protected:
  void SetUp() override {
    fake_owner_traits::current = 1;
    fake_irq_traits::reset();
    fake_spinlock_entry_traits::reset();
  }
};

} // namespace

TEST_F(GuardedRwSpinMutexTest, DefaultConstructsTheProtectedValue) {
  guarded_rw_spin_mutex<int, rw_spin_lock<fake_owner_traits>> m;
  auto g = m.read_lock();
  EXPECT_EQ(*g, 0);
}

TEST_F(GuardedRwSpinMutexTest, ConstructsFromAValue) {
  guarded_rw_spin_mutex<int, rw_spin_lock<fake_owner_traits>> m(7);
  auto g = m.read_lock();
  EXPECT_EQ(*g, 7);
}

TEST_F(GuardedRwSpinMutexTest, PlainLocksWithNoIrqLockerDefaultDoNotTouchAnyBackend) {
  guarded_rw_spin_mutex<int, rw_spin_lock<fake_owner_traits>> m(1);
  {
    auto g = m.write_lock();
    *g += 1;
  }
  {
    auto g = m.read_lock();
    EXPECT_EQ(*g, 2);
  }
  EXPECT_EQ(fake_irq_traits::save_count, 0);
}

TEST_F(GuardedRwSpinMutexTest, ReadLockEngagesIrqGuardAndAllowsMultipleReaders) {
  guarded_rw_spin_mutex<int, rw_spin_lock<fake_owner_traits>, irq_guard<fake_irq_traits>> m(5);
  auto g1 = m.read_lock();
  auto g2 = m.read_lock();
  EXPECT_EQ(*g1, 5);
  EXPECT_EQ(*g2, 5);
  EXPECT_EQ(m.reader_count(), 2u);
  EXPECT_EQ(fake_irq_traits::save_count, 2);

  g1.unlock();
  EXPECT_EQ(m.reader_count(), 1u);
  EXPECT_EQ(fake_irq_traits::restore_count, 1);

  g2.unlock();
  EXPECT_EQ(m.reader_count(), 0u);
  EXPECT_EQ(fake_irq_traits::restore_count, 2);
}

TEST_F(GuardedRwSpinMutexTest, WriteLockEngagesIrqGuardBeforeTakingTheWriterSide) {
  guarded_rw_spin_mutex<int, rw_spin_lock<fake_owner_traits>, irq_guard<fake_irq_traits>> m(0);
  {
    auto g = m.write_lock();
    EXPECT_TRUE(m.is_write_locked());
    EXPECT_TRUE(m.is_write_locked_by_current());
    EXPECT_EQ(fake_irq_traits::save_count, 1);
    *g = 42;
  }
  EXPECT_FALSE(m.is_write_locked());
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
  EXPECT_EQ(m.unsafe_get_mut(), 42);
}

TEST_F(GuardedRwSpinMutexTest, ReadGuardUnlockIsIdempotent) {
  guarded_rw_spin_mutex<int, rw_spin_lock<fake_owner_traits>, irq_guard<fake_irq_traits>> m(0);
  auto g = m.read_lock();
  g.unlock();
  EXPECT_FALSE(g.is_locked());
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
  g.unlock();
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
}

TEST_F(GuardedRwSpinMutexTest, WriteGuardUnlockIsIdempotent) {
  guarded_rw_spin_mutex<int, rw_spin_lock<fake_owner_traits>, irq_guard<fake_irq_traits>> m(0);
  auto g = m.write_lock();
  g.unlock();
  EXPECT_FALSE(g.is_locked());
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
  g.unlock();
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
}

TEST_F(GuardedRwSpinMutexTest, TryReadLockFailureReleasesIrqGuardImmediately) {
  guarded_rw_spin_mutex<int, rw_spin_lock<fake_owner_traits>, irq_guard<fake_irq_traits>> m(0);
  auto held = m.write_lock();

  auto maybe = m.try_read_lock();
  EXPECT_FALSE(maybe.has_value());
  EXPECT_EQ(fake_irq_traits::save_count, 2);
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
}

TEST_F(GuardedRwSpinMutexTest, TryWriteLockFailureReleasesIrqGuardImmediately) {
  guarded_rw_spin_mutex<int, rw_spin_lock<fake_owner_traits>, irq_guard<fake_irq_traits>> m(0);
  auto held = m.read_lock();

  auto maybe = m.try_write_lock();
  EXPECT_FALSE(maybe.has_value());
  EXPECT_EQ(fake_irq_traits::save_count, 2);
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
}

TEST_F(GuardedRwSpinMutexTest, TryReadLockAndTryWriteLockSucceedWhenFreeAndGrantAccess) {
  guarded_rw_spin_mutex<int, rw_spin_lock<fake_owner_traits>, irq_guard<fake_irq_traits>> m(3);

  auto maybe_r = m.try_read_lock();
  ASSERT_TRUE(maybe_r.has_value());
  EXPECT_EQ(**maybe_r, 3);
  maybe_r->unlock();
  EXPECT_FALSE(m.is_read_locked());

  auto maybe_w = m.try_write_lock();
  ASSERT_TRUE(maybe_w.has_value());
  **maybe_w = 4;
  maybe_w->unlock();
  EXPECT_FALSE(m.is_write_locked());
  EXPECT_EQ(m.unsafe_get_mut(), 4);
}

TEST_F(GuardedRwSpinMutexTest, WriteGuardIsMovable) {
  guarded_rw_spin_mutex<int, rw_spin_lock<fake_owner_traits>, irq_guard<fake_irq_traits>> m(1);
  auto g = m.write_lock();
  auto moved = std::move(g);
  EXPECT_TRUE(moved.is_locked());
  EXPECT_FALSE(g.is_locked());
  *moved += 1;
  moved.unlock();
  EXPECT_FALSE(m.is_write_locked());
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
  EXPECT_EQ(m.unsafe_get_mut(), 2);
}

TEST_F(GuardedRwSpinMutexTest, ReadGuardIsMovable) {
  guarded_rw_spin_mutex<int, rw_spin_lock<fake_owner_traits>, irq_guard<fake_irq_traits>> m(1);
  auto g = m.read_lock();
  auto moved = std::move(g);
  EXPECT_TRUE(moved.is_locked());
  EXPECT_FALSE(g.is_locked());
  EXPECT_EQ(*moved, 1);
  moved.unlock();
  EXPECT_FALSE(m.is_read_locked());
}

TEST_F(GuardedRwSpinMutexTest, QueueRwSpinLockWriteSideTakesACallerSuppliedNode) {
  guarded_rw_spin_mutex<int, queue_rw_spin_lock<fake_owner_traits>, irq_guard<fake_irq_traits>> m(10);
  EXPECT_TRUE(decltype(m)::write_uses_node);

  decltype(m)::node n;
  {
    auto g = m.write_lock(n);
    EXPECT_EQ(fake_irq_traits::save_count, 1);
    *g += 5;
  }
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
  EXPECT_EQ(m.unsafe_get_mut(), 15);

  decltype(m)::node n2;
  auto maybe = m.try_write_lock(n2);
  ASSERT_TRUE(maybe.has_value());
  **maybe += 1;
  maybe->unlock();
  EXPECT_EQ(m.unsafe_get_mut(), 16);
}

TEST_F(GuardedRwSpinMutexTest, QueueRwSpinLockReadSideTakesNoNode) {
  guarded_rw_spin_mutex<int, queue_rw_spin_lock<fake_owner_traits>, irq_guard<fake_irq_traits>> m(7);
  auto g = m.read_lock();
  EXPECT_TRUE(m.is_read_locked());
  EXPECT_EQ(*g, 7);
  g.unlock();
  EXPECT_FALSE(m.is_read_locked());
}

TEST_F(GuardedRwSpinMutexTest, PlainRwLocksDoNotUseANode) {
  EXPECT_FALSE(decltype(guarded_rw_spin_mutex<int, rw_spin_lock<fake_owner_traits>>{})::write_uses_node);
}

TEST_F(GuardedRwSpinMutexTest, SpinlockEntryGuardFlavorEntersAndExitsTheSection) {
  guarded_rw_spin_mutex<int, rw_spin_lock<fake_owner_traits>, spinlock_entry_guard<fake_spinlock_entry_traits>> m(0);
  {
    auto g = m.write_lock();
    EXPECT_EQ(fake_spinlock_entry_traits::depth, 1);
    *g = 3;
  }
  EXPECT_EQ(fake_spinlock_entry_traits::depth, 0);
  EXPECT_EQ(m.unsafe_get_mut(), 3);
}

TEST_F(GuardedRwSpinMutexTest, UnsafeGetMutGivesDirectAccess) {
  guarded_rw_spin_mutex<int, rw_spin_lock<fake_owner_traits>> m(1);
  m.unsafe_get_mut() = 100;
  auto g = m.read_lock();
  EXPECT_EQ(*g, 100);
}

TEST_F(GuardedRwSpinMutexTest, ReadGuardGetMatchesOperatorStar) {
  guarded_rw_spin_mutex<int, rw_spin_lock<fake_owner_traits>> m(5);
  auto g = m.read_lock();
  EXPECT_EQ(g.get(), 5);
  EXPECT_EQ(*g, 5);
}

TEST_F(GuardedRwSpinMutexTest, WriteGuardGetAndGetMutMatchOperatorStar) {
  guarded_rw_spin_mutex<int, rw_spin_lock<fake_owner_traits>> m(5);
  auto g = m.write_lock();
  EXPECT_EQ(g.get(), 5);
  g.get_mut() += 1;
  EXPECT_EQ(g.get(), 6);
  EXPECT_EQ(*g, 6);
}

RELOCO_END_UNSAFE_BUFFER_USAGE