// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/callout.hpp>

namespace {

using namespace structo;

// --------------------------------------------------------------------
// A fake, single-slot "callout subsystem": `submit` just records the
// submitted object/period into a static slot instead of a real
// wheel/queue, and exposes `simulate_fire` as a test-only door into the
// otherwise-private `async_kernel_object::fire()`, standing in for
// whatever real wheel-sweep/interrupt context a genuine subsystem would
// call it from.
// --------------------------------------------------------------------
struct fake_callout_subsystem {
  static inline void *pending_obj = nullptr;
  static inline void (*pending_fire)(void *) = nullptr;
  static inline reloco::duration last_period = reloco::duration{};
  static inline int submit_calls = 0;
  static inline int cancel_calls = 0;

  static void reset_counters() {
    pending_obj = nullptr;
    pending_fire = nullptr;
    last_period = reloco::duration{};
    submit_calls = 0;
    cancel_calls = 0;
  }

  template <typename Obj> static reloco::result<void> submit(Obj &self, reloco::duration period) noexcept {
    ++submit_calls;
    pending_obj = &self;
    pending_fire = [](void *ctx) noexcept { static_cast<Obj *>(ctx)->fire(); };
    last_period = period;
    return {};
  }

  template <typename Obj> static reloco::result<void> cancel(Obj &self) noexcept {
    ++cancel_calls;
    if (pending_obj == &self)
      pending_obj = nullptr;
    return {};
  }

  // Test-only door into the private `async_kernel_object::fire()`,
  // standing in for whatever real wheel-sweep/interrupt context a
  // genuine subsystem would call it from.
  static void simulate_fire() noexcept {
    if (pending_fire != nullptr)
      pending_fire(pending_obj);
  }
};

using fake_callout = callout<fake_callout_subsystem>;

void fire(fake_callout &) { fake_callout_subsystem::simulate_fire(); }

class CalloutTest : public ::testing::Test {
protected:
  void SetUp() override { fake_callout_subsystem::reset_counters(); }
};

TEST_F(CalloutTest, DefaultConstructedIsInactiveAndNotPending) {
  fake_callout co;
  EXPECT_FALSE(co.active());
  EXPECT_FALSE(co.pending());
  EXPECT_FALSE(co.firing());
}

TEST_F(CalloutTest, ResetCallsSubsystemSubmitAndSetsActivePending) {
  fake_callout co;
  int calls = 0;

  auto r = co.reset(reloco::duration::from_millis(10), [&calls](fake_callout &) noexcept { ++calls; });
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(co.active());
  EXPECT_TRUE(co.pending());
  EXPECT_EQ(fake_callout_subsystem::submit_calls, 1);
  EXPECT_EQ(fake_callout_subsystem::last_period.as_millis(), 10u);
  EXPECT_EQ(calls, 0);

  ASSERT_TRUE(co.drain().has_value());
}

TEST_F(CalloutTest, FireInvokesCallbackWithCalloutReference) {
  fake_callout co;
  fake_callout *seen = nullptr;
  ASSERT_TRUE(
      co.reset(reloco::duration::from_millis(1), [&seen](fake_callout &self) noexcept { seen = &self; }).has_value());

  fire(co);

  EXPECT_EQ(seen, &co);
  EXPECT_FALSE(co.pending());
  EXPECT_FALSE(co.firing());
}

TEST_F(CalloutTest, DeactivateSkipsCallbackButStillClearsPendingOnFire) {
  fake_callout co;
  int calls = 0;
  ASSERT_TRUE(co.reset(reloco::duration::from_millis(1), [&calls](fake_callout &) noexcept { ++calls; }).has_value());

  co.deactivate();
  EXPECT_FALSE(co.active());
  EXPECT_TRUE(co.pending());

  fire(co);

  EXPECT_EQ(calls, 0);
  EXPECT_FALSE(co.pending());
}

TEST_F(CalloutTest, StopReturnsTrueAndCallsSubsystemCancelWhenPending) {
  fake_callout co;
  ASSERT_TRUE(co.reset(reloco::duration::from_millis(1), [](fake_callout &) noexcept {}).has_value());

  auto r = co.stop();
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(r.value());
  EXPECT_EQ(fake_callout_subsystem::cancel_calls, 1);
  EXPECT_FALSE(co.pending());
  EXPECT_FALSE(co.active());
}

TEST_F(CalloutTest, StopReturnsFalseWithoutCallingSubsystemWhenNotPending) {
  fake_callout co;

  auto r = co.stop();
  ASSERT_TRUE(r.has_value());
  EXPECT_FALSE(r.value());
  EXPECT_EQ(fake_callout_subsystem::cancel_calls, 0);
}

TEST_F(CalloutTest, DrainSucceedsImmediatelyWhenIdle) {
  fake_callout co;
  EXPECT_TRUE(co.drain().has_value());
}

TEST_F(CalloutTest, DrainCancelsPendingCallout) {
  fake_callout co;
  ASSERT_TRUE(co.reset(reloco::duration::from_millis(1), [](fake_callout &) noexcept {}).has_value());

  EXPECT_TRUE(co.drain().has_value());
  EXPECT_EQ(fake_callout_subsystem::cancel_calls, 1);
  EXPECT_FALSE(co.pending());
}

TEST_F(CalloutTest, ResetReplacesPreviousPendingCallout) {
  fake_callout co;
  ASSERT_TRUE(co.reset(reloco::duration::from_millis(1), [](fake_callout &) noexcept {}).has_value());
  EXPECT_EQ(fake_callout_subsystem::submit_calls, 1);

  ASSERT_TRUE(co.reset(reloco::duration::from_millis(2), [](fake_callout &) noexcept {}).has_value());
  EXPECT_EQ(fake_callout_subsystem::cancel_calls, 1);
  EXPECT_EQ(fake_callout_subsystem::submit_calls, 2);

  ASSERT_TRUE(co.drain().has_value());
}

TEST_F(CalloutTest, ResetPeriodicReArmsItselfAfterEachFire) {
  fake_callout co;
  int fire_count = 0;

  ASSERT_TRUE(co.reset_periodic(reloco::duration::from_millis(5), [&fire_count](fake_callout &) noexcept {
                  ++fire_count;
                }).has_value());

  fire(co);
  EXPECT_EQ(fire_count, 1);
  EXPECT_TRUE(co.pending()); // re-armed itself
  EXPECT_EQ(fake_callout_subsystem::submit_calls, 2);

  fire(co);
  EXPECT_EQ(fire_count, 2);
  EXPECT_TRUE(co.pending());

  ASSERT_TRUE(co.drain().has_value());
}

TEST_F(CalloutTest, ResetPeriodicStopsReArmingAfterCallbackDeactivates) {
  fake_callout co;
  int fire_count = 0;

  ASSERT_TRUE(co.reset_periodic(reloco::duration::from_millis(5), [&fire_count](fake_callout &self) noexcept {
                  ++fire_count;
                  if (fire_count == 2)
                    self.deactivate();
                }).has_value());

  fire(co); // fire 1: re-arms
  EXPECT_TRUE(co.pending());
  fire(co); // fire 2: calls deactivate(), then re-arm is skipped
  EXPECT_EQ(fire_count, 2);
  EXPECT_FALSE(co.pending());
  EXPECT_FALSE(co.active());
}

#if !defined(NDEBUG) || defined(RELOCO_ENABLE_ASSERTS)
TEST(CalloutDeathTest, DestructorTrapsIfStillPending) {
  EXPECT_DEATH(
      {
        fake_callout co;
        (void)co.reset(reloco::duration::from_millis(1), [](fake_callout &) noexcept {});
      },
      "");
}
#endif

} // namespace
