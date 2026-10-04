// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hypervisor/vcpu_state_guard.hpp>

#include <cstdint>
#include <utility>

using structo::hypervisor::vcpu_entry_guard;
using structo::hypervisor::with_vcpu_entry;

namespace {

// Fake GPR group: tracks save/restore call counts and the live
// "hardware" value, letting tests assert ordering/pairing without any
// real registers.
struct fake_gpr_traits {
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

/** @brief Fixture for `vcpu_entry_guard` tests; resets `fake_gpr_traits`'s mutable static state before each test. */
class VcpuStateGuardTest : public ::testing::Test {
protected:
  void SetUp() override { fake_gpr_traits::reset(); }
};

} // namespace

TEST_F(VcpuStateGuardTest, ConstructionCapturesHostThenLoadsGuestState) {
  fake_gpr_traits::state_type guest_state{0xAAA};
  fake_gpr_traits::live_value = 0x111; // host's own live value before entry

  {
    vcpu_entry_guard<fake_gpr_traits> guard(guest_state);
    EXPECT_TRUE(guard.is_armed());
    EXPECT_EQ(guard.host_state().value, 0x111u);
    // Guest's snapshot was loaded live.
    EXPECT_EQ(fake_gpr_traits::live_value, 0xAAAu);
    EXPECT_EQ(fake_gpr_traits::save_count, 1);
    EXPECT_EQ(fake_gpr_traits::restore_count, 1);

    // Simulate the guest modifying its own registers while "running".
    fake_gpr_traits::live_value = 0xBBB;
  }

  // Destructor saved the guest's modified registers back into the
  // persistent snapshot, then restored the host's own.
  EXPECT_EQ(guest_state.value, 0xBBBu);
  EXPECT_EQ(fake_gpr_traits::live_value, 0x111u);
  EXPECT_EQ(fake_gpr_traits::save_count, 2);
  EXPECT_EQ(fake_gpr_traits::restore_count, 2);
}

TEST_F(VcpuStateGuardTest, UnlockSavesGuestAndRestoresHostEarlyThenDisarms) {
  fake_gpr_traits::state_type guest_state{0x7};
  fake_gpr_traits::live_value = 0x1;

  vcpu_entry_guard<fake_gpr_traits> guard(guest_state);
  fake_gpr_traits::live_value = 0x99; // guest "modifies" its registers
  guard.unlock();

  EXPECT_FALSE(guard.is_armed());
  EXPECT_EQ(guest_state.value, 0x99u);
  EXPECT_EQ(fake_gpr_traits::live_value, 0x1u);
  const int save_count_after_unlock = fake_gpr_traits::save_count;
  const int restore_count_after_unlock = fake_gpr_traits::restore_count;

  // A second unlock() must be a no-op: no double save/restore.
  fake_gpr_traits::live_value = 0x42;
  guard.unlock();
  EXPECT_EQ(fake_gpr_traits::save_count, save_count_after_unlock);
  EXPECT_EQ(fake_gpr_traits::restore_count, restore_count_after_unlock);
  EXPECT_EQ(fake_gpr_traits::live_value, 0x42u);
}

TEST_F(VcpuStateGuardTest, DestructorIsNoOpAfterUnlock) {
  fake_gpr_traits::state_type guest_state{0x5};
  fake_gpr_traits::live_value = 0x2;
  {
    vcpu_entry_guard<fake_gpr_traits> guard(guest_state);
    guard.unlock();
  }
  // Constructor's own guest-load restore(), plus unlock()'s host-restore
  // -- exactly two, with the destructor contributing none since `unlock()`
  // already disarmed the guard.
  EXPECT_EQ(fake_gpr_traits::restore_count, 2);
}

TEST_F(VcpuStateGuardTest, MoveConstructionTransfersOwnershipAndDisarmsSource) {
  fake_gpr_traits::state_type guest_state{0x3};
  fake_gpr_traits::live_value = 0x4;

  vcpu_entry_guard<fake_gpr_traits> first(guest_state);
  EXPECT_EQ(fake_gpr_traits::save_count, 1);

  vcpu_entry_guard<fake_gpr_traits> second(std::move(first));
  EXPECT_FALSE(first.is_armed());
  EXPECT_TRUE(second.is_armed());
  EXPECT_EQ(second.host_state().value, 0x4u);

  // Destroying the moved-from guard must not save/restore; only the
  // moved-to guard does so, on its own destruction.
  EXPECT_EQ(fake_gpr_traits::restore_count, 1);
}

TEST_F(VcpuStateGuardTest, WithVcpuEntryInvokesCallableAndSavesRestoresAfterwards) {
  fake_gpr_traits::state_type guest_state{0x9};
  fake_gpr_traits::live_value = 0x10;
  bool called = false;

  with_vcpu_entry<fake_gpr_traits>(guest_state, [&] {
    called = true;
    EXPECT_EQ(fake_gpr_traits::live_value, 0x9u);
  });

  EXPECT_TRUE(called);
  EXPECT_EQ(fake_gpr_traits::live_value, 0x10u);
}

TEST_F(VcpuStateGuardTest, WithVcpuEntryPassesGuardReferenceWhenRequested) {
  fake_gpr_traits::state_type guest_state{0x20};
  fake_gpr_traits::live_value = 0x30;
  bool unlocked_early = false;

  with_vcpu_entry<fake_gpr_traits>(guest_state, [&](auto &guard) {
    EXPECT_TRUE(guard.is_armed());
    guard.unlock();
    unlocked_early = true;
  });

  EXPECT_TRUE(unlocked_early);
  EXPECT_EQ(fake_gpr_traits::live_value, 0x30u);
}

TEST_F(VcpuStateGuardTest, WithVcpuEntryForwardsReturnValue) {
  fake_gpr_traits::state_type guest_state{0x1};
  const int result = with_vcpu_entry<fake_gpr_traits>(guest_state, [] { return 123; });
  EXPECT_EQ(result, 123);
}
