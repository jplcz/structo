// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/sync/spinlock_entry_guard.hpp>

#include <utility>

using structo::sync::spinlock_entered_locked;
using structo::sync::spinlock_entered_token;
using structo::sync::spinlock_entry_guard;
using structo::sync::with_spinlock_entered;

namespace {

// Fake kernel backend: tracks a simple nesting counter so tests can
// assert ordering/pairing without touching real interrupt-disable or
// critical-section entry.
struct fake_spinlock_entry_traits {
  static inline int depth = 0;
  static inline int enter_count = 0;
  static inline int exit_count = 0;

  static void spinlock_enter() noexcept {
    ++enter_count;
    ++depth;
  }

  static void spinlock_exit() noexcept {
    ++exit_count;
    --depth;
  }

  static void reset() noexcept {
    depth = 0;
    enter_count = 0;
    exit_count = 0;
  }
};

/** @brief Fixture for `spinlock_entry_guard`/`spinlock_entered_locked` tests; resets
 * `fake_spinlock_entry_traits`'s mutable static state before each test. */
class SpinlockEntryGuardTest : public ::testing::Test {
protected:
  void SetUp() override { fake_spinlock_entry_traits::reset(); }
};

} // namespace

TEST_F(SpinlockEntryGuardTest, ConstructionEntersAndDestructionExits) {
  {
    spinlock_entry_guard<fake_spinlock_entry_traits> guard;
    EXPECT_TRUE(guard.is_armed());
    EXPECT_EQ(fake_spinlock_entry_traits::enter_count, 1);
    EXPECT_EQ(fake_spinlock_entry_traits::exit_count, 0);
    EXPECT_EQ(fake_spinlock_entry_traits::depth, 1);
  }
  EXPECT_EQ(fake_spinlock_entry_traits::exit_count, 1);
  EXPECT_EQ(fake_spinlock_entry_traits::depth, 0);
}

TEST_F(SpinlockEntryGuardTest, UnlockExitsEarlyAndDisarmsFurtherExitOnDestruction) {
  {
    spinlock_entry_guard<fake_spinlock_entry_traits> guard;
    guard.unlock();
    EXPECT_FALSE(guard.is_armed());
    EXPECT_EQ(fake_spinlock_entry_traits::exit_count, 1);

    // A second unlock() must be a no-op: no double-exit.
    guard.unlock();
    EXPECT_EQ(fake_spinlock_entry_traits::exit_count, 1);
  }
  EXPECT_EQ(fake_spinlock_entry_traits::exit_count, 1);
}

TEST_F(SpinlockEntryGuardTest, MoveConstructionTransfersOwnershipAndDisarmsSource) {
  spinlock_entry_guard<fake_spinlock_entry_traits> first;
  EXPECT_EQ(fake_spinlock_entry_traits::enter_count, 1);

  spinlock_entry_guard<fake_spinlock_entry_traits> second(std::move(first));
  EXPECT_FALSE(first.is_armed());
  EXPECT_TRUE(second.is_armed());

  // Destroying the moved-from guard must not exit; only the moved-to
  // guard exits once, on its own destruction.
  EXPECT_EQ(fake_spinlock_entry_traits::exit_count, 0);
}

TEST_F(SpinlockEntryGuardTest, MoveAssignmentExitsTargetsPreviousStateBeforeTakingOver) {
  spinlock_entry_guard<fake_spinlock_entry_traits> a;
  spinlock_entry_guard<fake_spinlock_entry_traits> b;
  EXPECT_EQ(fake_spinlock_entry_traits::enter_count, 2);

  a = std::move(b);

  // Assigning into `a` first exits whatever `a` held, then takes over
  // `b`'s state and disarms `b`.
  EXPECT_EQ(fake_spinlock_entry_traits::exit_count, 1);
  EXPECT_TRUE(a.is_armed());
  EXPECT_FALSE(b.is_armed());
}

TEST_F(SpinlockEntryGuardTest, TokenIsOnlyObtainableFromAnActiveGuard) {
  spinlock_entry_guard<fake_spinlock_entry_traits> guard;
  auto token = guard.token(); // must compile: only spinlock_entry_guard/with_spinlock_entered can mint one
  (void)token;
}

TEST_F(SpinlockEntryGuardTest, WithSpinlockEnteredInvokesCallableWithoutArgumentsAndExitsAfterwards) {
  bool called = false;
  with_spinlock_entered<fake_spinlock_entry_traits>([&] { called = true; });
  EXPECT_TRUE(called);
  EXPECT_EQ(fake_spinlock_entry_traits::exit_count, 1);
}

TEST_F(SpinlockEntryGuardTest, WithSpinlockEnteredPassesProofTokenWhenRequested) {
  bool called = false;
  with_spinlock_entered<fake_spinlock_entry_traits>([&](spinlock_entered_token) { called = true; });
  EXPECT_TRUE(called);
  EXPECT_EQ(fake_spinlock_entry_traits::exit_count, 1);
}

TEST_F(SpinlockEntryGuardTest, WithSpinlockEnteredPassesGuardReferenceWhenRequested) {
  bool exited_early = false;
  with_spinlock_entered<fake_spinlock_entry_traits>([&](spinlock_entry_guard<fake_spinlock_entry_traits> &guard) {
    EXPECT_TRUE(guard.is_armed());
    guard.unlock();
    exited_early = true;
  });
  EXPECT_TRUE(exited_early);
  // The callable itself already exited via unlock(); with_spinlock_entered's
  // own guard destructor must not exit a second time.
  EXPECT_EQ(fake_spinlock_entry_traits::exit_count, 1);
}

TEST_F(SpinlockEntryGuardTest, WithSpinlockEnteredForwardsReturnValue) {
  const int result = with_spinlock_entered<fake_spinlock_entry_traits>([] { return 42; });
  EXPECT_EQ(result, 42);
}

TEST_F(SpinlockEntryGuardTest, SpinlockEnteredLockedLockGrantsExclusiveAccessAndExitsOnGuardDestruction) {
  spinlock_entered_locked<int, fake_spinlock_entry_traits> locked(7);

  {
    auto guard = locked.lock();
    EXPECT_EQ(*guard, 7);
    *guard = 9;
  }
  EXPECT_EQ(fake_spinlock_entry_traits::exit_count, 1);

  auto guard2 = locked.lock();
  EXPECT_EQ(*guard2, 9);
}

TEST_F(SpinlockEntryGuardTest, SpinlockEnteredLockedBorrowIsZeroCostGivenAnExistingToken) {
  spinlock_entered_locked<int, fake_spinlock_entry_traits> locked(5);

  spinlock_entry_guard<fake_spinlock_entry_traits> guard;
  const int enter_count_before = fake_spinlock_entry_traits::enter_count;

  int &value = locked.borrow(guard.token());
  EXPECT_EQ(value, 5);
  value = 11;

  // borrow() must not perform any additional enter/exit of its own.
  EXPECT_EQ(fake_spinlock_entry_traits::enter_count, enter_count_before);
  EXPECT_EQ(locked.borrow(guard.token()), 11);
}

TEST_F(SpinlockEntryGuardTest, SpinlockEnteredLockedWithLockPassesValueAndOptionalToken) {
  spinlock_entered_locked<int, fake_spinlock_entry_traits> locked(1);

  const int doubled = locked.with_lock([](int &value) {
    value += 1;
    return value * 2;
  });
  EXPECT_EQ(doubled, 4);

  bool saw_token = false;
  locked.with_lock([&](int &value, spinlock_entered_token) {
    saw_token = true;
    EXPECT_EQ(value, 2);
  });
  EXPECT_TRUE(saw_token);
}
