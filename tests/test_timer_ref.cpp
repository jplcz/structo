// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/timer_ref.hpp>

#include <optional>

namespace {

using namespace structo;
using namespace structo::hw;

// --------------------------------------------------------------------
// A fake, fully-featured timer backend: tracks armed mode/period and a
// pending-expiry latch a test can set directly. Implements both optional
// members (remaining, capabilities) plus the optional callback pair --
// `fire()` is the test's stand-in for the backend's own interrupt,
// invoking whatever callback is currently registered, if any.
// --------------------------------------------------------------------
struct fake_timer {
  bool active = false;
  timer_mode mode = timer_mode::one_shot;
  duration period{};
  duration left{};
  bool expired = false;
  bool has_cb = false;
  std::optional<reloco::function_ref<void()>> cb;

  void fire() {
    if (has_cb && cb)
      (*cb)();
  }
};

} // namespace

template <> struct structo::hw::timer_traits<fake_timer> {
  static reloco::result<void> try_start(fake_timer &b, timer_mode mode, duration period) noexcept {
    b.active = true;
    b.mode = mode;
    b.period = period;
    b.left = period;
    b.expired = false;
    return {};
  }
  static reloco::result<void> cancel(fake_timer &b) noexcept {
    b.active = false;
    b.expired = false;
    return {};
  }
  static reloco::result<bool> is_active(fake_timer &b) noexcept { return b.active; }
  static reloco::result<void> try_wait(fake_timer &b) noexcept {
    if (!b.expired)
      return reloco::unexpected(reloco::error::try_again);
    b.expired = false;
    if (b.mode == timer_mode::one_shot)
      b.active = false;
    else
      b.left = b.period;
    return {};
  }
  static reloco::result<duration> remaining(fake_timer &b) noexcept {
    if (!b.active)
      return reloco::unexpected(reloco::error::invalid_state);
    return b.left;
  }
  static reloco::result<timer_capabilities> capabilities(fake_timer &) noexcept {
    timer_capabilities caps;
    caps.supports_one_shot = true;
    caps.supports_periodic = true;
    caps.min_period = duration::from_micros(1);
    caps.max_period = duration::from_secs(3600);
    caps.resolution = duration::from_micros(1);
    caps.clock_hz = 1'000'000;
    caps.is_per_cpu = true;
    caps.supports_callback = true;
    return caps;
  }
  static reloco::result<void> set_callback(fake_timer &b, reloco::function_ref<void()> cb) noexcept {
    b.cb = cb;
    b.has_cb = true;
    return {};
  }
  static reloco::result<void> clear_callback(fake_timer &b) noexcept {
    b.has_cb = false;
    return {};
  }
};

namespace {

// --------------------------------------------------------------------
// A minimal backend with NO optional members, exercising the
// optional-trait fallback (error::unsupported_operation).
// --------------------------------------------------------------------
struct bare_timer {
  bool active = false;
};

} // namespace

template <> struct structo::hw::timer_traits<bare_timer> {
  static reloco::result<void> try_start(bare_timer &b, timer_mode, duration) noexcept {
    b.active = true;
    return {};
  }
  static reloco::result<void> cancel(bare_timer &b) noexcept {
    b.active = false;
    return {};
  }
  static reloco::result<bool> is_active(bare_timer &b) noexcept { return b.active; }
  static reloco::result<void> try_wait(bare_timer &) noexcept { return reloco::unexpected(reloco::error::try_again); }
};

namespace {

TEST(TimerRefTest, UnboundRefFailsEveryOperation) {
  timer_ref unbound;
  EXPECT_FALSE(static_cast<bool>(unbound));

  EXPECT_FALSE(unbound.try_start(timer_mode::one_shot, duration::from_millis(10)).has_value());
  EXPECT_FALSE(unbound.cancel().has_value());
  EXPECT_FALSE(unbound.is_active().has_value());
  EXPECT_FALSE(unbound.try_wait().has_value());
  EXPECT_FALSE(unbound.remaining().has_value());
  EXPECT_FALSE(unbound.capabilities().has_value());
  EXPECT_FALSE(unbound.set_callback([]() noexcept {}).has_value());
  EXPECT_FALSE(unbound.clear_callback().has_value());

  auto r = unbound.wait();
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), error::unsupported_operation);
}

TEST(TimerRefTest, BoundRefReportsTrue) {
  fake_timer dev;
  timer_ref ref(dev);
  EXPECT_TRUE(static_cast<bool>(ref));
}

TEST(TimerRefTest, StartArmsOneShotAndIsActive) {
  fake_timer dev;
  timer_ref ref(dev);

  ASSERT_TRUE(ref.start(duration::from_millis(5)).has_value());
  auto active = ref.is_active();
  ASSERT_TRUE(active.has_value());
  EXPECT_TRUE(active.value());
  EXPECT_EQ(dev.mode, timer_mode::one_shot);
  EXPECT_EQ(dev.period, duration::from_millis(5));
}

TEST(TimerRefTest, StartPeriodicArmsPeriodicMode) {
  fake_timer dev;
  timer_ref ref(dev);

  ASSERT_TRUE(ref.start_periodic(duration::from_millis(1)).has_value());
  EXPECT_EQ(dev.mode, timer_mode::periodic);
}

TEST(TimerRefTest, CancelDisarms) {
  fake_timer dev;
  timer_ref ref(dev);

  ASSERT_TRUE(ref.start(duration::from_millis(5)).has_value());
  ASSERT_TRUE(ref.cancel().has_value());
  auto active = ref.is_active();
  ASSERT_TRUE(active.has_value());
  EXPECT_FALSE(active.value());
}

TEST(TimerRefTest, TryWaitReportsTryAgainUntilExpired) {
  fake_timer dev;
  timer_ref ref(dev);
  ASSERT_TRUE(ref.start(duration::from_millis(5)).has_value());

  auto r1 = ref.try_wait();
  ASSERT_FALSE(r1.has_value());
  EXPECT_EQ(r1.error(), error::try_again);

  dev.expired = true;
  auto r2 = ref.try_wait();
  ASSERT_TRUE(r2.has_value());

  // One-shot: inactive after firing.
  auto active = ref.is_active();
  ASSERT_TRUE(active.has_value());
  EXPECT_FALSE(active.value());
}

TEST(TimerRefTest, PeriodicRemainsActiveAfterTryWait) {
  fake_timer dev;
  timer_ref ref(dev);
  ASSERT_TRUE(ref.start_periodic(duration::from_millis(1)).has_value());

  dev.expired = true;
  ASSERT_TRUE(ref.try_wait().has_value());

  auto active = ref.is_active();
  ASSERT_TRUE(active.has_value());
  EXPECT_TRUE(active.value());
}

TEST(TimerRefTest, WaitSpinsThenSucceedsOnceExpired) {
  fake_timer dev;
  timer_ref ref(dev);
  ASSERT_TRUE(ref.start(duration::from_millis(5)).has_value());
  dev.expired = true;

  auto r = ref.wait();
  EXPECT_TRUE(r.has_value());
}

TEST(TimerRefTest, WaitTimesOutWhenNeverExpired) {
  fake_timer dev;
  timer_ref ref(dev);
  ASSERT_TRUE(ref.start(duration::from_millis(5)).has_value());

  auto r = ref.wait(10);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), error::timed_out);
}

TEST(TimerRefTest, RestartCancelsThenStartsFresh) {
  fake_timer dev;
  timer_ref ref(dev);
  ASSERT_TRUE(ref.start(duration::from_millis(5)).has_value());
  dev.expired = true;

  ASSERT_TRUE(ref.restart(timer_mode::periodic, duration::from_millis(2)).has_value());
  EXPECT_EQ(dev.mode, timer_mode::periodic);
  EXPECT_EQ(dev.period, duration::from_millis(2));
  EXPECT_FALSE(dev.expired);
}

TEST(TimerRefTest, RemainingReportsTimeLeft) {
  fake_timer dev;
  timer_ref ref(dev);
  ASSERT_TRUE(ref.start(duration::from_millis(5)).has_value());

  auto left = ref.remaining();
  ASSERT_TRUE(left.has_value());
  EXPECT_EQ(left.value(), duration::from_millis(5));
}

TEST(TimerRefTest, RemainingFailsWhenNotActive) {
  fake_timer dev;
  timer_ref ref(dev);

  auto left = ref.remaining();
  ASSERT_FALSE(left.has_value());
  EXPECT_EQ(left.error(), error::invalid_state);
}

TEST(TimerRefTest, CapabilitiesReportsBackendLimits) {
  fake_timer dev;
  timer_ref ref(dev);

  auto caps = ref.capabilities();
  ASSERT_TRUE(caps.has_value());
  EXPECT_TRUE(caps.value().supports_one_shot);
  EXPECT_TRUE(caps.value().supports_periodic);
  EXPECT_EQ(caps.value().min_period, duration::from_micros(1));
  EXPECT_EQ(caps.value().max_period, duration::from_secs(3600));
  EXPECT_EQ(caps.value().clock_hz, 1'000'000u);
  EXPECT_TRUE(caps.value().is_per_cpu);
  EXPECT_TRUE(caps.value().supports_callback);
  EXPECT_TRUE(caps.value() == caps.value());
  EXPECT_FALSE(caps.value() != caps.value());
}

TEST(TimerRefTest, SetCallbackInvokedOnFire) {
  fake_timer dev;
  timer_ref ref(dev);
  ASSERT_TRUE(ref.start(duration::from_millis(5)).has_value());

  int calls = 0;
  ASSERT_TRUE(ref.set_callback([&calls]() noexcept { ++calls; }).has_value());
  EXPECT_EQ(calls, 0);

  dev.fire();
  EXPECT_EQ(calls, 1);

  dev.fire();
  EXPECT_EQ(calls, 2);
}

TEST(TimerRefTest, SetCallbackReplacesPreviousCallback) {
  fake_timer dev;
  timer_ref ref(dev);

  int first_calls = 0;
  int second_calls = 0;
  ASSERT_TRUE(ref.set_callback([&first_calls]() noexcept { ++first_calls; }).has_value());
  ASSERT_TRUE(ref.set_callback([&second_calls]() noexcept { ++second_calls; }).has_value());

  dev.fire();
  EXPECT_EQ(first_calls, 0);
  EXPECT_EQ(second_calls, 1);
}

TEST(TimerRefTest, ClearCallbackStopsFurtherInvocations) {
  fake_timer dev;
  timer_ref ref(dev);

  int calls = 0;
  ASSERT_TRUE(ref.set_callback([&calls]() noexcept { ++calls; }).has_value());
  dev.fire();
  EXPECT_EQ(calls, 1);

  ASSERT_TRUE(ref.clear_callback().has_value());
  dev.fire();
  EXPECT_EQ(calls, 1);
}

TEST(TimerRefTest, SetCallbackUnsupportedOnBareBackend) {
  bare_timer bare;
  timer_ref ref(bare);

  auto r = ref.set_callback([]() noexcept {});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), error::unsupported_operation);

  auto c = ref.clear_callback();
  ASSERT_FALSE(c.has_value());
  EXPECT_EQ(c.error(), error::unsupported_operation);
}

TEST(TimerRefTest, RemainingUnsupportedOnBareBackend) {
  bare_timer bare;
  timer_ref ref(bare);

  auto left = ref.remaining();
  ASSERT_FALSE(left.has_value());
  EXPECT_EQ(left.error(), error::unsupported_operation);
}

TEST(TimerRefTest, CapabilitiesUnsupportedOnBareBackend) {
  bare_timer bare;
  timer_ref ref(bare);

  auto caps = ref.capabilities();
  ASSERT_FALSE(caps.has_value());
  EXPECT_EQ(caps.error(), error::unsupported_operation);
}

TEST(TimerRefTest, TryWaitReportsTryAgainOnBareBackend) {
  bare_timer bare;
  timer_ref ref(bare);

  auto r = ref.try_wait();
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), error::try_again);
}

} // namespace
