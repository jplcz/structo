// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/sync/preemption_guard.hpp>

#include <utility>

using structo::sync::preempt_locked;
using structo::sync::preemption_disabled_token;
using structo::sync::preemption_guard;
using structo::sync::with_preemption_disabled;

namespace {

// Fake scheduler backend: tracks a simple nesting counter so tests can
// assert ordering/pairing without touching a real scheduler.
struct fake_preempt_traits {
  static inline int depth = 0;
  static inline int disable_count = 0;
  static inline int enable_count = 0;

  static void disable_preemption() noexcept {
    ++disable_count;
    ++depth;
  }

  static void enable_preemption() noexcept {
    ++enable_count;
    --depth;
  }

  static void reset() noexcept {
    depth = 0;
    disable_count = 0;
    enable_count = 0;
  }
};

/** @brief Fixture for `preemption_guard`/`preempt_locked` tests; resets `fake_preempt_traits`'s mutable static state before each test. */
class PreemptionGuardTest : public ::testing::Test {
protected:
  void SetUp() override { fake_preempt_traits::reset(); }
};

} // namespace

TEST_F(PreemptionGuardTest, ConstructionDisablesAndDestructionReenables) {
  {
    preemption_guard<fake_preempt_traits> guard;
    EXPECT_TRUE(guard.is_armed());
    EXPECT_EQ(fake_preempt_traits::disable_count, 1);
    EXPECT_EQ(fake_preempt_traits::enable_count, 0);
    EXPECT_EQ(fake_preempt_traits::depth, 1);
  }
  EXPECT_EQ(fake_preempt_traits::enable_count, 1);
  EXPECT_EQ(fake_preempt_traits::depth, 0);
}

TEST_F(PreemptionGuardTest, UnlockReenablesEarlyAndDisarmsFurtherReenableOnDestruction) {
  {
    preemption_guard<fake_preempt_traits> guard;
    guard.unlock();
    EXPECT_FALSE(guard.is_armed());
    EXPECT_EQ(fake_preempt_traits::enable_count, 1);

    // A second unlock() must be a no-op: no double-reenable.
    guard.unlock();
    EXPECT_EQ(fake_preempt_traits::enable_count, 1);
  }
  EXPECT_EQ(fake_preempt_traits::enable_count, 1);
}

TEST_F(PreemptionGuardTest, MoveConstructionTransfersOwnershipAndDisarmsSource) {
  preemption_guard<fake_preempt_traits> first;
  EXPECT_EQ(fake_preempt_traits::disable_count, 1);

  preemption_guard<fake_preempt_traits> second(std::move(first));
  EXPECT_FALSE(first.is_armed());
  EXPECT_TRUE(second.is_armed());

  // Destroying the moved-from guard must not reenable; only the
  // moved-to guard reenables once, on its own destruction.
  EXPECT_EQ(fake_preempt_traits::enable_count, 0);
}

TEST_F(PreemptionGuardTest, MoveAssignmentReenablesTargetsPreviousStateBeforeTakingOver) {
  preemption_guard<fake_preempt_traits> a;
  preemption_guard<fake_preempt_traits> b;
  EXPECT_EQ(fake_preempt_traits::disable_count, 2);

  a = std::move(b);

  // Assigning into `a` first re-enables whatever `a` held, then takes
  // over `b`'s state and disarms `b`.
  EXPECT_EQ(fake_preempt_traits::enable_count, 1);
  EXPECT_TRUE(a.is_armed());
  EXPECT_FALSE(b.is_armed());
}

TEST_F(PreemptionGuardTest, TokenIsOnlyObtainableFromAnActiveGuard) {
  preemption_guard<fake_preempt_traits> guard;
  auto token = guard.token(); // must compile: only preemption_guard/with_preemption_disabled can mint one
  (void)token;
}

TEST_F(PreemptionGuardTest, WithPreemptionDisabledInvokesCallableWithoutArgumentsAndReenablesAfterwards) {
  bool called = false;
  with_preemption_disabled<fake_preempt_traits>([&] { called = true; });
  EXPECT_TRUE(called);
  EXPECT_EQ(fake_preempt_traits::enable_count, 1);
}

TEST_F(PreemptionGuardTest, WithPreemptionDisabledPassesProofTokenWhenRequested) {
  bool called = false;
  with_preemption_disabled<fake_preempt_traits>([&](preemption_disabled_token) { called = true; });
  EXPECT_TRUE(called);
  EXPECT_EQ(fake_preempt_traits::enable_count, 1);
}

TEST_F(PreemptionGuardTest, WithPreemptionDisabledPassesGuardReferenceWhenRequested) {
  bool unlocked_early = false;
  with_preemption_disabled<fake_preempt_traits>([&](preemption_guard<fake_preempt_traits> &guard) {
    EXPECT_TRUE(guard.is_armed());
    guard.unlock();
    unlocked_early = true;
  });
  EXPECT_TRUE(unlocked_early);
  // The callable itself already re-enabled via unlock(); with_preemption_disabled's
  // own guard destructor must not reenable a second time.
  EXPECT_EQ(fake_preempt_traits::enable_count, 1);
}

TEST_F(PreemptionGuardTest, WithPreemptionDisabledForwardsReturnValue) {
  const int result = with_preemption_disabled<fake_preempt_traits>([] { return 42; });
  EXPECT_EQ(result, 42);
}

TEST_F(PreemptionGuardTest, PreemptLockedLockGrantsExclusiveAccessAndReenablesOnGuardDestruction) {
  preempt_locked<int, fake_preempt_traits> locked(7);

  {
    auto guard = locked.lock();
    EXPECT_EQ(*guard, 7);
    *guard = 9;
  }
  EXPECT_EQ(fake_preempt_traits::enable_count, 1);

  auto guard2 = locked.lock();
  EXPECT_EQ(*guard2, 9);
}

TEST_F(PreemptionGuardTest, PreemptLockedBorrowIsZeroCostGivenAnExistingToken) {
  preempt_locked<int, fake_preempt_traits> locked(5);

  preemption_guard<fake_preempt_traits> guard;
  const int disable_count_before = fake_preempt_traits::disable_count;

  int &value = locked.borrow(guard.token());
  EXPECT_EQ(value, 5);
  value = 11;

  // borrow() must not perform any additional disable/enable of its own.
  EXPECT_EQ(fake_preempt_traits::disable_count, disable_count_before);
  EXPECT_EQ(locked.borrow(guard.token()), 11);
}

TEST_F(PreemptionGuardTest, PreemptLockedWithLockPassesValueAndOptionalToken) {
  preempt_locked<int, fake_preempt_traits> locked(1);

  const int doubled = locked.with_lock([](int &value) {
    value += 1;
    return value * 2;
  });
  EXPECT_EQ(doubled, 4);

  bool saw_token = false;
  locked.with_lock([&](int &value, preemption_disabled_token) {
    saw_token = true;
    EXPECT_EQ(value, 2);
  });
  EXPECT_TRUE(saw_token);
}
