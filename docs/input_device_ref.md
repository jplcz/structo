<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::input_device_ref`

`include/structo/hw/input_device_ref.hpp`

A type-erased, non-owning handle over a human-input device -- keyboard, mouse,
touchpad/touchscreen, gamepad or any other HID -- plus the
`input_traits<Backend>` customization point a backend specializes to be
bindable through it. Like [`uart_ref`](uart_ref.md) it is abstraction-only:
no USB/PS/2/virtio-input protocol code, just the uniform event stream.

Every device is reduced to fixed-size `input_event` records (evdev-style):

| `type`   | `code`                                   | `value`                           |
|----------|------------------------------------------|-----------------------------------|
| `key`    | USB HID keyboard usage (`hid_key`)       | `released` / `pressed` / `repeat` |
| `button` | `input_button` (mouse, gamepad, touch)   | 0 / 1                             |
| `rel`    | `input_axis` (motion, wheel)             | signed delta                      |
| `abs`    | `input_axis` (touch, tablet, stick)      | absolute position                 |
| `sync`   | 0                                        | 0 -- ends one atomic update       |

## Polled access

```cpp
#include <structo/hw/input_device_ref.hpp>

using namespace structo::hw;

// Bind an adapted backend (it needs an input_traits<my_keyboard> specialization). It must outlive `kbd`.
input_device_ref kbd(my_keyboard);

if (kbd.capabilities().has(input_class::keyboard)) { /* ... */ }

// Non-blocking: fails with error::try_again when nothing is pending.
auto ev = kbd.try_read_event();

// Blocking spin; the argument is the spin budget, expiry = error::timed_out.
// read_key() skips releases/motion/sync and returns the HID usage of the next key press.
auto key = kbd.read_key(1'000'000);
if (key) {
  bool shift = false;                      // track hid_key_is_shift() presses/releases yourself
  char c = hid_key_to_ascii(*key, shift);  // US layout; 0 = not printable (arrows, F-keys, ...)
}

// Drain everything currently queued into a caller buffer, never blocks.
std::array<input_event, 16> buf;
auto n = kbd.read_available(buf);          // number of events stored
```

## Interrupt-driven access

A backend that can raise an interrupt when input arrives supplies
`set_callback`/`clear_callback` (same shape as `timer_ref`'s completion
callback) and reports `capabilities().supports_callback`. Polling keeps working
alongside it: events are not passed to the callback, they stay queued in the
backend until read.

```cpp
// Called from interrupt context whenever the queue goes from empty to non-empty
// (it may fire more often, never less). Must be short and non-blocking.
auto on_input = [&] {
  std::array<input_event, 8> buf;
  auto n = kbd.read_available(buf);   // drain the queue; or just wake a task and return
  // ... push *n events into your own ring / signal a task ...
};

// function_ref is non-owning: `on_input` must stay alive until clear_callback().
if (kbd.capabilities().supports_callback)
  (void)kbd.set_callback(reloco::function_ref<void()>(on_input));
// else: no interrupt support (set_callback fails with error::unsupported_operation) -- keep polling.

(void)kbd.clear_callback();   // unregister; harmless if none was set
```

## Writing a backend

```cpp
// Specialize input_traits for your device type. The three mandatory members:
template <> struct structo::hw::input_traits<my_keyboard> {
  // What the device is; also tells consumers whether callbacks exist.
  static input_capabilities capabilities(const my_keyboard &) noexcept {
    return {static_cast<std::uint32_t>(input_class::keyboard), 0x046d, 0xc31c, /*supports_callback=*/true};
  }
  // Would try_read_event succeed right now?
  static reloco::result<bool> event_ready(my_keyboard &k) noexcept { return !k.queue.empty(); }
  // Pop the oldest event, or fail with error::try_again.
  static reloco::result<input_event> try_read_event(my_keyboard &k) noexcept { /* ... */ }

  // Optional (each detected independently; absent = error::unsupported_operation):
  static reloco::result<void> set_callback(my_keyboard &k, reloco::function_ref<void()> cb) noexcept; // + clear_callback
  static reloco::result<void> set_leds(my_keyboard &k, std::uint8_t mask) noexcept;                   // input_led bits
  static reloco::result<input_abs_info> abs_info(my_keyboard &k, input_axis a) noexcept;             // absolute axis range
  static reloco::result<std::size_t> try_read_report(my_keyboard &k, reloco::span<std::uint8_t> dst) noexcept; // raw HID report
  static reloco::result<void> flush(my_keyboard &k) noexcept;      // default: drain via try_read_event
};
```

An unbound (default-constructed) ref fails every operation with
`error::unsupported_operation`.
