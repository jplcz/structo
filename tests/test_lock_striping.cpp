// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/sync/lock_striping.hpp>

#include <reloco/mutex.hpp>
#include <thread>
#include <vector>

namespace {

using structo::sync::lock_striping;

class LockStripingTest : public ::testing::Test {};

} // namespace

TEST_F(LockStripingTest, LockForLocksAndUnlocksOnScopeExit) {
  lock_striping<4> table;
  int key = 0;
  {
    auto guard = table.lock_for(&key);
    (void)guard;
  }
  // Re-locking the same key after the guard dropped must not deadlock.
  auto guard2 = table.lock_for(&key);
  (void)guard2;
}

TEST_F(LockStripingTest, TryLockForSameStripeFailsWhileHeld) {
  lock_striping<1> table; // force a collision: only one stripe exists
  int key_a = 0;
  int key_b = 0;

  auto guard_a = table.lock_for(&key_a);
  auto result = table.try_lock_for(&key_b);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), reloco::error::busy);
}

TEST_F(LockStripingTest, TryLockForSucceedsOnceReleased) {
  lock_striping<1> table;
  int key_a = 0;
  int key_b = 0;

  {
    auto guard_a = table.lock_for(&key_a);
  }
  auto result = table.try_lock_for(&key_b);
  ASSERT_TRUE(result.has_value());
}

TEST_F(LockStripingTest, ExplicitResetUnlocksBeforeScopeExit) {
  lock_striping<1> table;
  int key_a = 0;
  int key_b = 0;

  auto guard_a = table.lock_for(&key_a);
  guard_a.reset();
  auto result = table.try_lock_for(&key_b);
  EXPECT_TRUE(result.has_value());
}

TEST_F(LockStripingTest, MoveTransfersOwnershipOfTheHeldStripe) {
  lock_striping<1> table;
  int key_a = 0;
  int key_b = 0;

  auto guard_a = table.lock_for(&key_a);
  auto moved = std::move(guard_a);
  // guard_a is now empty; destroying it must be a no-op, and the stripe
  // must still be held via `moved` until it is itself destroyed.
  auto result = table.try_lock_for(&key_b);
  EXPECT_FALSE(result.has_value());
}

TEST_F(LockStripingTest, SharedLockForAllowsConcurrentReaders) {
  lock_striping<1, reloco::shared_mutex> table;
  int key_a = 0;
  int key_b = 0;

  auto reader1 = table.shared_lock_for(&key_a);
  auto reader2 = table.shared_lock_for(&key_b); // same stripe, still shared
  (void)reader1;
  (void)reader2;
}

TEST_F(LockStripingTest, TryLockForFailsWhileSharedLockIsHeld) {
  lock_striping<1, reloco::shared_mutex> table;
  int key_a = 0;
  int key_b = 0;

  auto reader = table.shared_lock_for(&key_a);
  auto result = table.try_lock_for(&key_b);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), reloco::error::busy);
}

TEST_F(LockStripingTest, TrySharedLockForFailsWhileExclusivelyHeld) {
  lock_striping<1, reloco::shared_mutex> table;
  int key_a = 0;
  int key_b = 0;

  auto writer = table.lock_for(&key_a);
  auto result = table.try_shared_lock_for(&key_b);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error(), reloco::error::busy);
}

TEST_F(LockStripingTest, SequenceForIsEvenWhenUnlockedAndOddWhileLockedExclusively) {
  lock_striping<1> table;
  int key = 0;

  EXPECT_EQ(table.sequence_for(&key) % 2, 0u);
  {
    auto guard = table.lock_for(&key);
    EXPECT_EQ(table.sequence_for(&key) % 2, 1u);
  }
  EXPECT_EQ(table.sequence_for(&key) % 2, 0u);
}

TEST_F(LockStripingTest, ValidateForSucceedsWhenNoWriterIntervenes) {
  lock_striping<1> table;
  int key = 0;

  const std::uint32_t start = table.sequence_for(&key);
  EXPECT_TRUE(table.validate_for(&key, start));
}

TEST_F(LockStripingTest, ValidateForFailsWhenAWriterIntervenes) {
  lock_striping<1> table;
  int key = 0;

  const std::uint32_t start = table.sequence_for(&key);
  {
    auto guard = table.lock_for(&key);
  }
  EXPECT_FALSE(table.validate_for(&key, start));
}

TEST_F(LockStripingTest, ValidateForFailsWhileAWriterIsStillActive) {
  lock_striping<1> table;
  int key = 0;

  auto guard = table.lock_for(&key);
  const std::uint32_t start = table.sequence_for(&key);
  EXPECT_FALSE(table.validate_for(&key, start));
}

TEST_F(LockStripingTest, DistinctKeysCanHashToDifferentStripesAndLockConcurrently) {
  // With enough stripes, two distinct addresses are very likely to land
  // on different ones; run both lock acquisitions on separate threads
  // and simply assert the program doesn't deadlock/hang (bounded by
  // GoogleTest's own test timeout).
  lock_striping<64> table;
  int key_a = 0;
  int key_b = 0;

  std::thread t([&] {
    auto guard = table.lock_for(&key_b);
    std::this_thread::yield();
  });
  {
    auto guard = table.lock_for(&key_a);
    std::this_thread::yield();
  }
  t.join();
}
