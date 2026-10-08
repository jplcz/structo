// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <reloco/lifetime.hpp>
#include <structo/sync/softlock_detector.hpp>

using structo::sync::softlock_detector;

TEST(SoftlockDetectorTest, StartsAtZeroCount) {
  softlock_detector detector;
  EXPECT_EQ(detector.count(), 0u);
  EXPECT_EQ(detector.limit(), softlock_detector::default_limit());
}

TEST(SoftlockDetectorTest, SetDefaultLimitChangesConstructorDefault) {
  const auto original = softlock_detector::default_limit();
  softlock_detector::set_default_limit(42);
  softlock_detector detector;
  EXPECT_EQ(detector.limit(), 42u);
  softlock_detector::set_default_limit(original);
}

TEST(SoftlockDetectorTest, CustomLimitIsHonored) {
  softlock_detector detector(5);
  EXPECT_EQ(detector.limit(), 5u);
}

TEST(SoftlockDetectorTest, TickIncrementsCount) {
  softlock_detector detector(5);
  detector.tick();
  detector.tick();
  EXPECT_EQ(detector.count(), 2u);
}

TEST(SoftlockDetectorTest, TicksWithinLimitDoNotTrap) {
  softlock_detector detector(3);
  detector.tick();
  detector.tick();
  detector.tick();
  SUCCEED();
}

TEST(SoftlockDetectorTest, ExceedingLimitTraps) {
  softlock_detector detector(2);
  detector.tick();
  detector.tick();
  // gtest's death-test macro expands to libc fprintf calls outside our control.
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  EXPECT_DEATH({ detector.tick(); }, "");
  RELOCO_END_UNSAFE_BUFFER_USAGE
}

TEST(SoftlockDetectorTest, ResetClearsCount) {
  softlock_detector detector(2);
  detector.tick();
  detector.tick();
  detector.reset();
  EXPECT_EQ(detector.count(), 0u);
  detector.tick();
  detector.tick();
  SUCCEED();
}
