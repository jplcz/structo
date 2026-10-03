// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/sync/backoff.hpp>

#include <cstdint>

using structo::sync::backoff;

TEST(BackoffTest, DefaultConstructedStartsAtOnePendingSpin) {
  backoff bo;
  EXPECT_EQ(bo.pending_spins(), 1u);
}

TEST(BackoffTest, SpinDoublesPendingSpinsEachCall) {
  backoff bo;
  EXPECT_EQ(bo.pending_spins(), 1u);
  bo.spin();
  EXPECT_EQ(bo.pending_spins(), 2u);
  bo.spin();
  EXPECT_EQ(bo.pending_spins(), 4u);
  bo.spin();
  EXPECT_EQ(bo.pending_spins(), 8u);
}

TEST(BackoffTest, SpinCapsPendingSpinsAtMax) {
  backoff bo(1, 16);
  for (int i = 0; i < 10; ++i) {
    bo.spin();
  }
  EXPECT_EQ(bo.pending_spins(), 16u);
}

TEST(BackoffTest, SpinNeverExceedsMaxEvenWhenDoublingWouldOverflowIt) {
  // max_spins (100) is not a power of two multiple of initial_spins (1),
  // so naive doubling would overshoot it (64 -> 128) before saturating;
  // it must clamp to exactly max_spins instead.
  backoff bo(1, 100);
  for (int i = 0; i < 20; ++i) {
    bo.spin();
  }
  EXPECT_EQ(bo.pending_spins(), 100u);
}

TEST(BackoffTest, ResetRestoresInitialPendingSpins) {
  backoff bo(3, 1024);
  bo.spin();
  bo.spin();
  EXPECT_NE(bo.pending_spins(), 3u);

  bo.reset();
  EXPECT_EQ(bo.pending_spins(), 3u);
}

TEST(BackoffTest, CustomInitialAndMaxSpinsAreHonored) {
  backoff bo(5, 20);
  EXPECT_EQ(bo.pending_spins(), 5u);
  bo.spin();
  EXPECT_EQ(bo.pending_spins(), 10u);
  bo.spin();
  EXPECT_EQ(bo.pending_spins(), 20u);
  bo.spin();
  EXPECT_EQ(bo.pending_spins(), 20u);
}

TEST(BackoffTest, SpinUntilReturnsAssoonAsPredicateIsTrue) {
  int calls = 0;
  backoff::spin_until([&calls] {
    ++calls;
    return calls >= 3;
  });
  EXPECT_EQ(calls, 3);
}

TEST(BackoffTest, SpinUntilReturnsImmediatelyWhenAlreadyTrue) {
  int calls = 0;
  backoff::spin_until([&calls] {
    ++calls;
    return true;
  });
  EXPECT_EQ(calls, 1);
}
