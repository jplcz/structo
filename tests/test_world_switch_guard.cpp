// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/arch/world_switch_guard.hpp>

#include <cstdint>
#include <utility>

using structo::arch::with_world_switch;
using structo::arch::world_switch_guard;

namespace {

// Fake non-banked register group: tracks save/restore call counts and
// the live "hardware" value, letting tests assert ordering/pairing
// without any real registers.
struct fake_regs_traits {
  struct state_type {
    std::uint64_t value = 0;
  };

  static inline std::uint64_t live_value = 0;
  static inline int save_count = 0;
  static inline int restore_count = 0;

  static state_type save() noexcept {
    ++save_count;
    return state_type{live_value};
  }

  static void restore(const state_type &s) noexcept {
    ++restore_count;
    live_value = s.value;
  }

  static void reset() noexcept {
    live_value = 0;
    save_count = 0;
    restore_count = 0;
  }
};

/** @brief Fixture for `world_switch_guard` tests; resets `fake_regs_traits`'s mutable static state before each test. */
class WorldSwitchGuardTest : public ::testing::Test {
protected:
  void SetUp() override { fake_regs_traits::reset(); }
};

} // namespace

TEST_F(WorldSwitchGuardTest, ConstructionSavesAndDestructionRestores) {
  fake_regs_traits::live_value = 42;
  {
    world_switch_guard<fake_regs_traits> guard;
    EXPECT_TRUE(guard.is_armed());
    EXPECT_EQ(guard.state().value, 42u);
    EXPECT_EQ(fake_regs_traits::save_count, 1);
    EXPECT_EQ(fake_regs_traits::restore_count, 0);

    // Simulate the other world clobbering the shared register.
    fake_regs_traits::live_value = 0xDEAD;
  }
  // Destructor restored our own captured snapshot, undoing the clobber.
  EXPECT_EQ(fake_regs_traits::restore_count, 1);
  EXPECT_EQ(fake_regs_traits::live_value, 42u);
}

TEST_F(WorldSwitchGuardTest, UnlockRestoresEarlyAndDisarmsFurtherRestoreOnDestruction) {
  fake_regs_traits::live_value = 7;
  {
    world_switch_guard<fake_regs_traits> guard;
    fake_regs_traits::live_value = 99;
    guard.unlock();
    EXPECT_FALSE(guard.is_armed());
    EXPECT_EQ(fake_regs_traits::restore_count, 1);
    EXPECT_EQ(fake_regs_traits::live_value, 7u);

    // A second unlock() must be a no-op: no double-restore.
    fake_regs_traits::live_value = 123;
    guard.unlock();
    EXPECT_EQ(fake_regs_traits::restore_count, 1);
    EXPECT_EQ(fake_regs_traits::live_value, 123u);
  }
  EXPECT_EQ(fake_regs_traits::restore_count, 1);
}

TEST_F(WorldSwitchGuardTest, MoveConstructionTransfersOwnershipAndDisarmsSource) {
  fake_regs_traits::live_value = 11;
  world_switch_guard<fake_regs_traits> first;
  EXPECT_EQ(fake_regs_traits::save_count, 1);

  world_switch_guard<fake_regs_traits> second(std::move(first));
  EXPECT_FALSE(first.is_armed());
  EXPECT_TRUE(second.is_armed());
  EXPECT_EQ(second.state().value, 11u);

  // Destroying the moved-from guard must not restore; only the
  // moved-to guard restores once, on its own destruction.
  EXPECT_EQ(fake_regs_traits::restore_count, 0);
}

TEST_F(WorldSwitchGuardTest, MoveAssignmentRestoresTargetsPreviousStateBeforeTakingOver) {
  fake_regs_traits::live_value = 1;
  world_switch_guard<fake_regs_traits> a;
  fake_regs_traits::live_value = 2;
  world_switch_guard<fake_regs_traits> b;
  EXPECT_EQ(fake_regs_traits::save_count, 2);

  a = std::move(b);

  // Assigning into `a` first restores whatever `a` held (live_value
  // becomes 1 momentarily), then takes over `b`'s snapshot (value==2)
  // and disarms `b`.
  EXPECT_EQ(fake_regs_traits::restore_count, 1);
  EXPECT_TRUE(a.is_armed());
  EXPECT_EQ(a.state().value, 2u);
  EXPECT_FALSE(b.is_armed());
}

TEST_F(WorldSwitchGuardTest, WithWorldSwitchInvokesCallableWithoutArgumentsAndRestoresAfterwards) {
  fake_regs_traits::live_value = 5;
  bool called = false;
  with_world_switch<fake_regs_traits>([&] { called = true; });
  EXPECT_TRUE(called);
  EXPECT_EQ(fake_regs_traits::restore_count, 1);
}

TEST_F(WorldSwitchGuardTest, WithWorldSwitchPassesGuardReferenceWhenRequested) {
  fake_regs_traits::live_value = 9;
  bool unlocked_early = false;
  with_world_switch<fake_regs_traits>([&](auto &guard) {
    EXPECT_TRUE(guard.is_armed());
    EXPECT_EQ(guard.state().value, 9u);
    guard.unlock();
    unlocked_early = true;
  });
  EXPECT_TRUE(unlocked_early);
  // The callable itself already unlocked (restored); with_world_switch's
  // own guard destructor must not restore a second time.
  EXPECT_EQ(fake_regs_traits::restore_count, 1);
}

TEST_F(WorldSwitchGuardTest, WithWorldSwitchForwardsReturnValue) {
  const int result = with_world_switch<fake_regs_traits>([] { return 99; });
  EXPECT_EQ(result, 99);
}
