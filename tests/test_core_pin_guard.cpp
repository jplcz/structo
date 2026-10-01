// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/sync/core_pin_guard.hpp>

#include <cstddef>
#include <type_traits>
#include <utility>

using structo::sync::core_pin_guard;
using structo::sync::with_cpu_pinned;

namespace {

// Fake scheduler backend: tracks pin/unpin nesting and a fixed "current
// CPU" so tests can assert ordering/pairing without a real scheduler.
struct fake_pin_traits {
  using cpu_id_type = std::size_t;

  static inline std::size_t current_cpu = 3;
  static inline int pin_count = 0;
  static inline int unpin_count = 0;
  static inline int nest_depth = 0;

  static cpu_id_type pin() noexcept {
    ++pin_count;
    ++nest_depth;
    return current_cpu;
  }

  static void unpin() noexcept {
    ++unpin_count;
    --nest_depth;
  }

  static void reset() noexcept {
    current_cpu = 3;
    pin_count = 0;
    unpin_count = 0;
    nest_depth = 0;
  }
};

/** @brief Fixture for `core_pin_guard` tests; resets `fake_pin_traits`'s mutable static state before each test. */
class CorePinGuardTest : public ::testing::Test {
protected:
  void SetUp() override { fake_pin_traits::reset(); }
};

} // namespace

TEST_F(CorePinGuardTest, ConstructionPinsAndDestructionUnpins) {
  {
    core_pin_guard<fake_pin_traits> guard;
    EXPECT_TRUE(guard.is_armed());
    EXPECT_EQ(guard.pinned_cpu(), 3);
    EXPECT_EQ(fake_pin_traits::pin_count, 1);
    EXPECT_EQ(fake_pin_traits::unpin_count, 0);
    EXPECT_EQ(fake_pin_traits::nest_depth, 1);
  }
  EXPECT_EQ(fake_pin_traits::unpin_count, 1);
  EXPECT_EQ(fake_pin_traits::nest_depth, 0);
}

TEST_F(CorePinGuardTest, UnlockUnpinsEarlyAndDisarmsFurtherUnpinOnDestruction) {
  {
    core_pin_guard<fake_pin_traits> guard;
    guard.unlock();
    EXPECT_FALSE(guard.is_armed());
    EXPECT_EQ(fake_pin_traits::unpin_count, 1);

    // A second unlock() must be a no-op: no double-unpin.
    guard.unlock();
    EXPECT_EQ(fake_pin_traits::unpin_count, 1);
  }
  EXPECT_EQ(fake_pin_traits::unpin_count, 1);
}

TEST_F(CorePinGuardTest, MoveConstructionTransfersOwnershipAndDisarmsSource) {
  core_pin_guard<fake_pin_traits> first;
  EXPECT_EQ(fake_pin_traits::pin_count, 1);

  core_pin_guard<fake_pin_traits> second(std::move(first));
  EXPECT_FALSE(first.is_armed());
  EXPECT_TRUE(second.is_armed());
  EXPECT_EQ(second.pinned_cpu(), 3);

  // Destroying the moved-from guard must not unpin; only the moved-to
  // guard unpins once, on its own destruction.
  EXPECT_EQ(fake_pin_traits::unpin_count, 0);
}

TEST_F(CorePinGuardTest, MoveAssignmentUnpinsTargetsPreviousStateBeforeTakingOver) {
  core_pin_guard<fake_pin_traits> a;
  fake_pin_traits::current_cpu = 5;
  core_pin_guard<fake_pin_traits> b;
  EXPECT_EQ(fake_pin_traits::pin_count, 2);

  a = std::move(b);

  // Assigning into `a` first unpins whatever `a` held, then takes over
  // `b`'s state (pinned_cpu()==5) and disarms `b`.
  EXPECT_EQ(fake_pin_traits::unpin_count, 1);
  EXPECT_TRUE(a.is_armed());
  EXPECT_EQ(a.pinned_cpu(), 5);
  EXPECT_FALSE(b.is_armed());
}

TEST_F(CorePinGuardTest, WithCpuPinnedInvokesCallableWithoutArgumentsAndUnpinsAfterwards) {
  bool called = false;
  with_cpu_pinned<fake_pin_traits>([&] { called = true; });
  EXPECT_TRUE(called);
  EXPECT_EQ(fake_pin_traits::unpin_count, 1);
}

TEST_F(CorePinGuardTest, WithCpuPinnedPassesPinnedCpuWhenRequested) {
  std::size_t seen_cpu = 0;
  with_cpu_pinned<fake_pin_traits>([&](std::size_t cpu) { seen_cpu = cpu; });
  EXPECT_EQ(seen_cpu, 3);
  EXPECT_EQ(fake_pin_traits::unpin_count, 1);
}

TEST_F(CorePinGuardTest, WithCpuPinnedPassesGuardReferenceWhenRequested) {
  bool unlocked_early = false;
  with_cpu_pinned<fake_pin_traits>([&](core_pin_guard<fake_pin_traits> &guard) {
    EXPECT_TRUE(guard.is_armed());
    EXPECT_EQ(guard.pinned_cpu(), 3);
    guard.unlock();
    unlocked_early = true;
  });
  EXPECT_TRUE(unlocked_early);
  // The callable itself already unpinned via unlock(); with_cpu_pinned's
  // own guard destructor must not unpin a second time.
  EXPECT_EQ(fake_pin_traits::unpin_count, 1);
}

TEST_F(CorePinGuardTest, WithCpuPinnedForwardsReturnValue) {
  const int result = with_cpu_pinned<fake_pin_traits>([] { return 42; });
  EXPECT_EQ(result, 42);
}

TEST_F(CorePinGuardTest, DefaultCpuIdTypeIsSizeTWhenTraitsOmitsIt) {
  struct minimal_pin_traits {
    static std::size_t pin() noexcept { return 0; }
    static void unpin() noexcept {}
  };
  static_assert(std::is_same_v<core_pin_guard<minimal_pin_traits>::cpu_id_type, std::size_t>);

  core_pin_guard<minimal_pin_traits> guard;
  EXPECT_EQ(guard.pinned_cpu(), 0);
}
