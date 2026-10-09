// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/input_device_ref.hpp>

#include <array>
#include <deque>
#include <optional>

namespace {

using namespace structo;
using namespace structo::hw;

// Fully-featured fake: an event queue plus every optional operation.
struct fake_input {
  std::deque<input_event> events;
  std::deque<std::uint8_t> report;
  std::uint8_t leds = 0;
  std::optional<reloco::function_ref<void()>> cb;
  // Simulates the device interrupt: queues an event and notifies the callback.
  void inject(const input_event &e) {
    events.push_back(e);
    if (cb)
      (*cb)();
  }
};

// Minimal fake with only the mandatory operations.
struct bare_input {
  std::deque<input_event> events;
};

} // namespace

template <> struct structo::hw::input_traits<fake_input> {
  static input_capabilities capabilities(const fake_input &) noexcept {
    return {input_class::keyboard | input_class::mouse | input_class::hid_reports, 0x1234, 0x5678, true};
  }
  static reloco::result<bool> event_ready(fake_input &b) noexcept { return !b.events.empty(); }
  static reloco::result<input_event> try_read_event(fake_input &b) noexcept {
    if (b.events.empty())
      return reloco::unexpected(reloco::error::try_again);
    auto e = b.events.front();
    b.events.pop_front();
    return e;
  }
  static reloco::result<void> set_leds(fake_input &b, std::uint8_t mask) noexcept {
    b.leds = mask;
    return {};
  }
  static reloco::result<void> set_callback(fake_input &b, reloco::function_ref<void()> cb) noexcept {
    b.cb = cb;
    return {};
  }
  static reloco::result<void> clear_callback(fake_input &b) noexcept {
    b.cb.reset();
    return {};
  }
  static reloco::result<input_abs_info> abs_info(fake_input &, input_axis a) noexcept {
    if (a != input_axis::x)
      return reloco::unexpected(reloco::error::invalid_argument);
    return input_abs_info{0, 4095, 10};
  }
  static reloco::result<std::size_t> try_read_report(fake_input &b, reloco::span<std::uint8_t> dst) noexcept {
    if (b.report.empty())
      return reloco::unexpected(reloco::error::try_again);
    std::size_t n = 0;
    while (n < dst.size() && !b.report.empty()) {
      dst[n++] = b.report.front();
      b.report.pop_front();
    }
    return n;
  }
};

template <> struct structo::hw::input_traits<bare_input> {
  static input_capabilities capabilities(const bare_input &) noexcept {
    return {static_cast<std::uint32_t>(input_class::gamepad)};
  }
  static reloco::result<bool> event_ready(bare_input &b) noexcept { return !b.events.empty(); }
  static reloco::result<input_event> try_read_event(bare_input &b) noexcept {
    if (b.events.empty())
      return reloco::unexpected(reloco::error::try_again);
    auto e = b.events.front();
    b.events.pop_front();
    return e;
  }
};

namespace {

TEST(InputDeviceRef, UnboundFailsGracefully) {
  input_device_ref d;
  EXPECT_FALSE(static_cast<bool>(d));
  EXPECT_EQ(d.capabilities().classes, 0u);
  EXPECT_EQ(d.event_ready().error(), reloco::error::unsupported_operation);
  EXPECT_EQ(d.try_read_event().error(), reloco::error::unsupported_operation);
  EXPECT_EQ(d.read_event().error(), reloco::error::unsupported_operation);
  EXPECT_EQ(d.flush().error(), reloco::error::unsupported_operation);
}

TEST(InputDeviceRef, ReadsEventsInOrder) {
  fake_input f;
  f.events = {make_key_event(0x04, input_key_state::pressed, 7), make_rel_event(input_axis::x, -3),
              make_button_event(input_button::left, true), make_sync_event()};
  input_device_ref d(f);
  ASSERT_TRUE(d);
  EXPECT_TRUE(d.capabilities().has(input_class::keyboard));
  EXPECT_TRUE(d.capabilities().has(input_class::mouse));
  EXPECT_FALSE(d.capabilities().has(input_class::gamepad));
  EXPECT_EQ(d.capabilities().vendor_id, 0x1234);

  auto e = d.try_read_event();
  ASSERT_TRUE(e);
  EXPECT_TRUE(e.value().is_key(0x04));
  EXPECT_TRUE(e.value().is_pressed());
  EXPECT_EQ(e.value().time, 7u);
  e = d.try_read_event();
  EXPECT_EQ(e.value().type, input_event_type::rel);
  EXPECT_EQ(e.value().value, -3);
  e = d.try_read_event();
  EXPECT_EQ(e.value().type, input_event_type::button);
  e = d.try_read_event();
  EXPECT_EQ(e.value().type, input_event_type::sync);
  EXPECT_EQ(d.try_read_event().error(), reloco::error::try_again);
}

TEST(InputDeviceRef, ReadEventTimesOutWhenIdle) {
  fake_input f;
  input_device_ref d(f);
  EXPECT_EQ(d.read_event(10).error(), reloco::error::timed_out);
  f.events.push_back(make_sync_event());
  EXPECT_TRUE(d.read_event(10));
}

TEST(InputDeviceRef, ReadAvailableDrainsUpToCapacity) {
  fake_input f;
  for (int i = 0; i < 5; ++i)
    f.events.push_back(make_rel_event(input_axis::y, i));
  input_device_ref d(f);
  std::array<input_event, 3> buf{};
  auto n = d.read_available(buf);
  ASSERT_TRUE(n);
  EXPECT_EQ(n.value(), 3u);
  EXPECT_EQ(buf[2].value, 2);
  n = d.read_available(buf);
  EXPECT_EQ(n.value(), 2u);
  n = d.read_available(buf);
  EXPECT_EQ(n.value(), 0u);
}

TEST(InputDeviceRef, ReadKeySkipsReleasesAndOtherEvents) {
  fake_input f;
  f.events = {make_rel_event(input_axis::x, 1), make_key_event(0x04, input_key_state::released), make_sync_event(),
              make_key_event(0x05, input_key_state::pressed)};
  input_device_ref d(f);
  auto k = d.read_key(10);
  ASSERT_TRUE(k);
  EXPECT_EQ(k.value(), 0x05);
  EXPECT_EQ(d.read_key(10).error(), reloco::error::timed_out);
}

TEST(InputDeviceRef, OptionalOperations) {
  fake_input f;
  input_device_ref d(f);
  EXPECT_TRUE(d.set_leds(static_cast<std::uint8_t>(input_led::caps_lock)));
  EXPECT_EQ(f.leds, 2);
  auto info = d.abs_info(input_axis::x);
  ASSERT_TRUE(info);
  EXPECT_EQ(info.value().maximum, 4095);
  EXPECT_FALSE(d.abs_info(input_axis::y));

  f.report = {1, 2, 3};
  std::array<std::uint8_t, 8> buf{};
  auto n = d.try_read_report(buf);
  ASSERT_TRUE(n);
  EXPECT_EQ(n.value(), 3u);
  EXPECT_EQ(buf[2], 3);
  EXPECT_EQ(d.try_read_report(buf).error(), reloco::error::try_again);

  f.events = {make_sync_event(), make_sync_event()};
  EXPECT_TRUE(d.flush());
  EXPECT_TRUE(f.events.empty());
}

TEST(InputDeviceRef, CallbackFiresOnInputAndPollingStillWorks) {
  fake_input f;
  input_device_ref d(f);
  EXPECT_TRUE(d.capabilities().supports_callback);

  int calls = 0;
  std::size_t drained = 0;
  auto handler = [&] {
    ++calls;
    std::array<input_event, 4> buf{};
    auto n = d.read_available(buf); // the typical interrupt-context body: drain the queue
    if (n)
      drained += n.value();
  };
  ASSERT_TRUE(d.set_callback(reloco::function_ref<void()>(handler)));
  f.inject(make_key_event(0x04, input_key_state::pressed));
  f.inject(make_sync_event());
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(drained, 2u);

  ASSERT_TRUE(d.clear_callback());
  f.inject(make_sync_event());
  EXPECT_EQ(calls, 2);             // no longer notified...
  EXPECT_TRUE(d.try_read_event()); // ...but the event is still pollable
}

TEST(InputDeviceRef, MissingOptionalOperationsAreUnsupported) {
  bare_input b;
  b.events = {make_sync_event(), make_sync_event()};
  input_device_ref d(b);
  EXPECT_FALSE(d.capabilities().supports_callback);
  auto noop = [] {};
  EXPECT_EQ(d.set_callback(reloco::function_ref<void()>(noop)).error(), reloco::error::unsupported_operation);
  EXPECT_EQ(d.clear_callback().error(), reloco::error::unsupported_operation);
  EXPECT_EQ(d.set_leds(1).error(), reloco::error::unsupported_operation);
  EXPECT_EQ(d.abs_info(input_axis::x).error(), reloco::error::unsupported_operation);
  std::array<std::uint8_t, 4> buf{};
  EXPECT_EQ(d.try_read_report(buf).error(), reloco::error::unsupported_operation);
  EXPECT_TRUE(d.flush()); // falls back to draining via try_read_event
  EXPECT_TRUE(b.events.empty());
}

TEST(HidKeyToAscii, UsLayout) {
  EXPECT_EQ(hid_key_to_ascii(static_cast<std::uint16_t>(hid_key::a)), 'a');
  EXPECT_EQ(hid_key_to_ascii(static_cast<std::uint16_t>(hid_key::z), true), 'Z');
  EXPECT_EQ(hid_key_to_ascii(static_cast<std::uint16_t>(hid_key::n1)), '1');
  EXPECT_EQ(hid_key_to_ascii(static_cast<std::uint16_t>(hid_key::n1), true), '!');
  EXPECT_EQ(hid_key_to_ascii(static_cast<std::uint16_t>(hid_key::n9), true), '(');
  EXPECT_EQ(hid_key_to_ascii(static_cast<std::uint16_t>(hid_key::n0), true), ')');
  EXPECT_EQ(hid_key_to_ascii(static_cast<std::uint16_t>(hid_key::enter)), '\n');
  EXPECT_EQ(hid_key_to_ascii(static_cast<std::uint16_t>(hid_key::space)), ' ');
  EXPECT_EQ(hid_key_to_ascii(static_cast<std::uint16_t>(hid_key::slash), true), '?');
  EXPECT_EQ(hid_key_to_ascii(static_cast<std::uint16_t>(hid_key::left_shift)), 0);
  EXPECT_EQ(hid_key_to_ascii(static_cast<std::uint16_t>(hid_key::up)), 0);
  EXPECT_TRUE(hid_key_is_shift(static_cast<std::uint16_t>(hid_key::right_shift)));
  EXPECT_FALSE(hid_key_is_shift(static_cast<std::uint16_t>(hid_key::a)));
}

} // namespace
