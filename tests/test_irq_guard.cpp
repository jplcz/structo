// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/sync/irq_guard.hpp>

#include <utility>

using structo::sync::irq_guard;
using structo::sync::irq_locked;
using structo::sync::with_irq_disabled;

namespace {

// Fake architecture backend: tracks nesting via an incrementing "flags"
// counter and records every save/restore so tests can assert ordering
// without touching real CPSID/MSR-style instructions.
struct fake_irq_traits {
  using flags_type = int;

  static inline int depth = 0;
  static inline int save_count = 0;
  static inline int restore_count = 0;
  static inline int last_restored_flags = -1;

  static flags_type hw_save_irqs() noexcept {
    ++save_count;
    return depth++;
  }

  static void hw_restore_irqs(flags_type flags) noexcept {
    ++restore_count;
    last_restored_flags = flags;
    depth = flags;
  }

  static void reset() noexcept {
    depth = 0;
    save_count = 0;
    restore_count = 0;
    last_restored_flags = -1;
  }
};

/** @brief Fixture for `irq_guard`/`irq_locked` tests; resets `fake_irq_traits`'s mutable static state before each test.
 */
class IrqGuardTest : public ::testing::Test {
protected:
  void SetUp() override { fake_irq_traits::reset(); }
};

} // namespace

TEST_F(IrqGuardTest, ConstructionSavesAndDestructionRestoresFlags) {
  {
    irq_guard<fake_irq_traits> guard;
    EXPECT_TRUE(guard.is_armed());
    EXPECT_EQ(guard.saved_flags(), 0);
    EXPECT_EQ(fake_irq_traits::save_count, 1);
    EXPECT_EQ(fake_irq_traits::restore_count, 0);
  }
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
  EXPECT_EQ(fake_irq_traits::last_restored_flags, 0);
}

TEST_F(IrqGuardTest, UnlockRestoresEarlyAndDisarmsFurtherRestoreOnDestruction) {
  {
    irq_guard<fake_irq_traits> guard;
    guard.unlock();
    EXPECT_FALSE(guard.is_armed());
    EXPECT_EQ(fake_irq_traits::restore_count, 1);

    // A second unlock() must be a no-op: no double-restore.
    guard.unlock();
    EXPECT_EQ(fake_irq_traits::restore_count, 1);
  }
  // Destruction of an already-unlocked guard does not restore again.
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
}

TEST_F(IrqGuardTest, MoveConstructionTransfersOwnershipAndDisarmsSource) {
  irq_guard<fake_irq_traits> first;
  EXPECT_EQ(fake_irq_traits::save_count, 1);

  irq_guard<fake_irq_traits> second(std::move(first));
  EXPECT_FALSE(first.is_armed());
  EXPECT_TRUE(second.is_armed());
  EXPECT_EQ(second.saved_flags(), first.saved_flags());

  // Destroying the moved-from guard must not restore (it is disarmed); only
  // the moved-to guard restores once, on its own destruction.
  EXPECT_EQ(fake_irq_traits::restore_count, 0);
}

TEST_F(IrqGuardTest, MoveAssignmentRestoresTargetsPreviousStateBeforeTakingOver) {
  irq_guard<fake_irq_traits> a; // saves flags=0, depth becomes 1
  irq_guard<fake_irq_traits> b; // saves flags=1, depth becomes 2

  a = std::move(b);

  // Assigning into `a` first restores whatever `a` held (flags=0), then
  // takes over `b`'s state and disarms `b`.
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
  EXPECT_EQ(fake_irq_traits::last_restored_flags, 0);
  EXPECT_TRUE(a.is_armed());
  EXPECT_EQ(a.saved_flags(), 1);
}

TEST_F(IrqGuardTest, TokenIsOnlyObtainableFromAnActiveGuard) {
  irq_guard<fake_irq_traits> guard;
  auto token = guard.token(); // must compile: only irq_guard/with_irq_disabled can mint one
  (void)token;
}

TEST_F(IrqGuardTest, WithIrqDisabledInvokesCallableWithoutArgumentsAndRestoresAfterwards) {
  bool called = false;
  with_irq_disabled<fake_irq_traits>([&] { called = true; });
  EXPECT_TRUE(called);
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
}

TEST_F(IrqGuardTest, WithIrqDisabledPassesProofTokenWhenRequested) {
  bool called = false;
  with_irq_disabled<fake_irq_traits>([&](structo::sync::critical_section_token) { called = true; });
  EXPECT_TRUE(called);
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
}

TEST_F(IrqGuardTest, WithIrqDisabledPassesGuardReferenceWhenRequested) {
  bool unlocked_early = false;
  with_irq_disabled<fake_irq_traits>([&](irq_guard<fake_irq_traits> &guard) {
    EXPECT_TRUE(guard.is_armed());
    guard.unlock();
    unlocked_early = true;
  });
  EXPECT_TRUE(unlocked_early);
  // The callable itself already restored via unlock(); with_irq_disabled's
  // own guard destructor must not restore a second time.
  EXPECT_EQ(fake_irq_traits::restore_count, 1);
}

TEST_F(IrqGuardTest, WithIrqDisabledForwardsReturnValue) {
  const int result = with_irq_disabled<fake_irq_traits>([] { return 42; });
  EXPECT_EQ(result, 42);
}

TEST_F(IrqGuardTest, IrqLockedLockGrantsExclusiveAccessAndRestoresOnGuardDestruction) {
  irq_locked<int, fake_irq_traits> locked(7);

  {
    auto guard = locked.lock();
    EXPECT_EQ(*guard, 7);
    *guard = 9;
  }
  EXPECT_EQ(fake_irq_traits::restore_count, 1);

  auto guard2 = locked.lock();
  EXPECT_EQ(*guard2, 9);
}

TEST_F(IrqGuardTest, IrqLockedBorrowIsZeroCostGivenAnExistingToken) {
  irq_locked<int, fake_irq_traits> locked(5);

  irq_guard<fake_irq_traits> guard;
  const int save_count_before = fake_irq_traits::save_count;

  int &value = locked.borrow(guard.token());
  EXPECT_EQ(value, 5);
  value = 11;

  // borrow() must not perform any additional save/restore of its own.
  EXPECT_EQ(fake_irq_traits::save_count, save_count_before);
  EXPECT_EQ(locked.borrow(guard.token()), 11);
}

TEST_F(IrqGuardTest, IrqLockedWithLockPassesValueAndOptionalToken) {
  irq_locked<int, fake_irq_traits> locked(1);

  const int doubled = locked.with_lock([](int &value) {
    value += 1;
    return value * 2;
  });
  EXPECT_EQ(doubled, 4);

  bool saw_token = false;
  locked.with_lock([&](int &value, structo::sync::critical_section_token) {
    saw_token = true;
    EXPECT_EQ(value, 2);
  });
  EXPECT_TRUE(saw_token);
}
