// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file input_device_ref.hpp
 * @brief `structo::hw::input_device_ref`: a type-erased, non-owning handle
 * over a polled human-input device -- keyboard, mouse, touchpad/touchscreen,
 * gamepad, any other HID -- plus the `input_traits<Backend>` customization
 * point a concrete backend specializes to be bindable through it.
 *
 * ## Event model
 *
 * Every device, whatever its transport (PS/2, USB HID, virtio-input, a
 * board's GPIO matrix keypad), is reduced to a stream of small, fixed-size
 * @ref input_event records, in the style of Linux evdev:
 *
 * | `type`                    | `code`                                  | `value`                         |
 * |---------------------------|-----------------------------------------|---------------------------------|
 * | `input_event_type::key`   | a USB HID keyboard usage (`hid_key`)    | `released` / `pressed` / `repeat` |
 * | `input_event_type::button`| an `input_button` (mouse, gamepad, ...) | `released` / `pressed`          |
 * | `input_event_type::rel`   | an `input_axis` (movement, wheel)       | signed delta since the last event |
 * | `input_event_type::abs`   | an `input_axis` (touch, tablet, stick)  | absolute position               |
 * | `input_event_type::sync`  | `0`                                     | `0`; ends one atomic update     |
 *
 * Keyboard `code`s are the USB HID *Keyboard/Keypad page (0x07)* usage IDs
 * (`hid_key`), a layout-independent identification of the physical key
 * (`a` is `0x04` whatever the layout). Turning that into characters is up
 * to the consumer; @ref hid_key_to_ascii does it for the US layout.
 * A backend for hardware that reports other codes (PS/2 scan codes, matrix
 * positions) translates them.
 *
 * A device may emit `sync` after the events that belong together (a mouse
 * report with both `dx` and `dy`); consumers that care about atomicity
 * apply events until the next `sync`.
 *
 * ## Raw HID reports
 *
 * A HID-class device may additionally expose its raw input reports through
 * the optional `input_traits::try_read_report`, for consumers that parse
 * report descriptors themselves (gamepads with exotic layouts, sensors).
 * A backend may offer the event stream, the raw reports, or both.
 *
 * ## Customizing: `input_traits<Backend>`
 *
 * A specialization must supply three mandatory functions:
 * @code
 * template <> struct structo::hw::input_traits<my_backend> {
 *   // What the device is/has (called often; must be cheap).
 *   static structo::hw::input_capabilities capabilities(const my_backend &) noexcept;
 *   // Whether `try_read_event` would succeed right now.
 *   static reloco::result<bool> event_ready(my_backend &) noexcept;
 *   // Removes and returns the oldest pending event, or fails with error::try_again.
 *   static reloco::result<structo::hw::input_event> try_read_event(my_backend &) noexcept;
 * };
 * @endcode
 * Optionally, also:
 *  - `set_leds(Backend &, std::uint8_t mask)` (`input_led` bits: Caps/Num/Scroll Lock);
 *  - `abs_info(Backend &, input_axis)` returning `result<input_abs_info>` (range of an absolute axis);
 *  - `try_read_report(Backend &, span<std::uint8_t>)` returning `result<std::size_t>` (raw HID report,
 *    `error::try_again` when none is pending);
 *  - `flush(Backend &)` (discard everything pending, e.g. before a "press any key" prompt).
 * Detected via SFINAE, the same optional-member idiom `uart_traits::current_config` uses; an absent optional
 * operation fails with `error::unsupported_operation` (`flush` falls back to draining via `try_read_event`).
 *
 * ## Polled and interrupt-driven access
 *
 * Polling (`event_ready`/`try_read_event`/`read_event`/`read_available`) always works. A backend that can also
 * raise an interrupt when input arrives may additionally supply the optional pair
 * @code
 * static reloco::result<void> set_callback(my_backend &, reloco::function_ref<void()> cb) noexcept;
 * static reloco::result<void> clear_callback(my_backend &) noexcept;
 * @endcode
 * exactly like `timer_traits`' completion callback:
 *  - `set_callback` registers `cb` (replacing any previous one) to be invoked -- typically from interrupt
 *    context -- whenever the device's queue goes from empty to non-empty (it may fire more often, never less).
 *    `cb` is a non-owning `function_ref`: the callable must stay valid until `clear_callback` or the device's
 *    destruction. It takes no arguments and must be interrupt-safe (short, non-blocking); the usual body just
 *    wakes a task or calls `read_available` to drain the queue. Events are not passed to the callback, so
 *    polling and callbacks can be mixed freely: the events stay queued in the backend until read.
 *  - `clear_callback` unregisters it; clearing when none is set is not an error.
 * A backend providing the pair should report `input_capabilities::supports_callback = true`. Without the
 * pair, `set_callback`/`clear_callback` fail with `error::unsupported_operation` and the device is polled only.
 */

#include <reloco/detail/assert.hpp>
#include <reloco/detail/compat.hpp>
#include <reloco/error.hpp>
#include <reloco/function_ref.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/span.hpp>
#include <reloco/string_view.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace structo {

using namespace reloco;

namespace hw {

// ============================================================================
// Events
// ============================================================================

/** @brief What an @ref input_event describes. */
enum class input_event_type : std::uint8_t {
  sync = 0, ///< end of one atomic group of events
  key,      ///< keyboard key: `code` is a @ref hid_key, `value` an @ref input_key_state
  button,   ///< pointer/gamepad button: `code` is an @ref input_button, `value` 0 or 1
  rel,      ///< relative axis (mouse motion, wheel): `code` is an @ref input_axis, `value` a signed delta
  abs,      ///< absolute axis (touch, tablet, stick): `code` is an @ref input_axis, `value` the position
};

/** @brief `input_event::value` of an `input_event_type::key` event. */
enum class input_key_state : std::int32_t { released = 0, pressed = 1, repeat = 2 };

/** @brief Buttons of pointers and gamepads (`input_event_type::button`). */
enum class input_button : std::uint16_t {
  left = 0,
  right,
  middle,
  back,
  forward,
  touch, ///< finger/pen in contact with a touch surface
  // Gamepad face/shoulder/system buttons.
  gamepad_a = 0x100,
  gamepad_b,
  gamepad_x,
  gamepad_y,
  gamepad_l1,
  gamepad_r1,
  gamepad_select,
  gamepad_start,
  gamepad_mode,
  gamepad_thumb_l,
  gamepad_thumb_r,
  dpad_up,
  dpad_down,
  dpad_left,
  dpad_right,
};

/** @brief Axes (`input_event_type::rel` / `input_event_type::abs`). */
enum class input_axis : std::uint16_t {
  x = 0,
  y,
  z,
  wheel,  ///< vertical scroll wheel (`rel`)
  hwheel, ///< horizontal scroll wheel (`rel`)
  rx,     ///< second stick / rotation around x (`abs`)
  ry,
  rz,
  pressure, ///< touch/pen pressure (`abs`)
  throttle_left,
  throttle_right,
};

/** @brief One input event. */
struct input_event {
  input_event_type type = input_event_type::sync;
  std::uint16_t code = 0; ///< meaning depends on `type`, see the table in the @file docs
  std::int32_t value = 0; ///< meaning depends on `type`
  std::uint64_t time = 0; ///< backend timestamp (its own unit, typically microseconds); 0 if it has none

  [[nodiscard]] constexpr bool is_key(std::uint16_t c) const noexcept {
    return type == input_event_type::key && code == c;
  }
  [[nodiscard]] constexpr bool is_pressed() const noexcept {
    return (type == input_event_type::key || type == input_event_type::button) && value != 0;
  }
  [[nodiscard]] friend constexpr bool operator==(const input_event &a, const input_event &b) noexcept {
    return a.type == b.type && a.code == b.code && a.value == b.value && a.time == b.time;
  }
  [[nodiscard]] friend constexpr bool operator!=(const input_event &a, const input_event &b) noexcept {
    return !(a == b);
  }
};

/** @brief Builds a key event. */
[[nodiscard]] constexpr input_event make_key_event(std::uint16_t hid_usage, input_key_state s,
                                                   std::uint64_t time = 0) noexcept {
  return {input_event_type::key, hid_usage, static_cast<std::int32_t>(s), time};
}
/** @brief Builds a button event. */
[[nodiscard]] constexpr input_event make_button_event(input_button b, bool pressed, std::uint64_t time = 0) noexcept {
  return {input_event_type::button, static_cast<std::uint16_t>(b), pressed ? 1 : 0, time};
}
/** @brief Builds a relative-axis event. */
[[nodiscard]] constexpr input_event make_rel_event(input_axis a, std::int32_t delta, std::uint64_t time = 0) noexcept {
  return {input_event_type::rel, static_cast<std::uint16_t>(a), delta, time};
}
/** @brief Builds an absolute-axis event. */
[[nodiscard]] constexpr input_event make_abs_event(input_axis a, std::int32_t pos, std::uint64_t time = 0) noexcept {
  return {input_event_type::abs, static_cast<std::uint16_t>(a), pos, time};
}
/** @brief Builds a sync event. */
[[nodiscard]] constexpr input_event make_sync_event(std::uint64_t time = 0) noexcept {
  return {input_event_type::sync, 0, 0, time};
}

// ============================================================================
// Keyboard codes
// ============================================================================

/** @brief USB HID Keyboard/Keypad page (0x07) usage IDs of the keys a boot-time console needs. */
enum class hid_key : std::uint16_t {
  a = 0x04, // a..z are 0x04..0x1D
  z = 0x1D,
  n1 = 0x1E, // 1..9 are 0x1E..0x26
  n9 = 0x26,
  n0 = 0x27,
  enter = 0x28,
  escape = 0x29,
  backspace = 0x2A,
  tab = 0x2B,
  space = 0x2C,
  minus = 0x2D,
  equal = 0x2E,
  left_bracket = 0x2F,
  right_bracket = 0x30,
  backslash = 0x31,
  semicolon = 0x33,
  apostrophe = 0x34,
  grave = 0x35,
  comma = 0x36,
  period = 0x37,
  slash = 0x38,
  caps_lock = 0x39,
  f1 = 0x3A, // f1..f12 are 0x3A..0x45
  f12 = 0x45,
  print_screen = 0x46,
  scroll_lock = 0x47,
  pause = 0x48,
  insert = 0x49,
  home = 0x4A,
  page_up = 0x4B,
  delete_key = 0x4C,
  end = 0x4D,
  page_down = 0x4E,
  right = 0x4F,
  left = 0x50,
  down = 0x51,
  up = 0x52,
  num_lock = 0x53,
  left_ctrl = 0xE0,
  left_shift = 0xE1,
  left_alt = 0xE2,
  left_gui = 0xE3,
  right_ctrl = 0xE4,
  right_shift = 0xE5,
  right_alt = 0xE6,
  right_gui = 0xE7,
};

/** @brief Whether `usage` is Left/Right Shift. */
[[nodiscard]] constexpr bool hid_key_is_shift(std::uint16_t usage) noexcept {
  return usage == static_cast<std::uint16_t>(hid_key::left_shift) ||
         usage == static_cast<std::uint16_t>(hid_key::right_shift);
}

/**
 * @brief Translates a HID keyboard usage to ASCII for the US layout.
 * @param usage HID Keyboard page usage ID (`input_event::code` of a key event).
 * @param shift Whether a Shift key is held.
 * @return The character, or `0` for keys with no printable ASCII meaning (modifiers, arrows, F-keys). Enter yields
 * `'\\n'`, Tab `'\\t'`, Backspace `'\\b'`, Escape `0x1B`.
 */
[[nodiscard]] constexpr char hid_key_to_ascii(std::uint16_t usage, bool shift = false) noexcept {
  constexpr reloco::string_view digits_shifted("!@#$%^&*(");
  if (usage >= 0x04 && usage <= 0x1D) {
    const char base = static_cast<char>('a' + (usage - 0x04));
    return shift ? static_cast<char>(base - 'a' + 'A') : base;
  }
  if (usage >= 0x1E && usage <= 0x26)
    return shift ? digits_shifted[usage - 0x1E] : static_cast<char>('1' + (usage - 0x1E));
  switch (usage) {
  case 0x27:
    return shift ? ')' : '0';
  case 0x28:
    return '\n';
  case 0x29:
    return '\x1b';
  case 0x2A:
    return '\b';
  case 0x2B:
    return '\t';
  case 0x2C:
    return ' ';
  case 0x2D:
    return shift ? '_' : '-';
  case 0x2E:
    return shift ? '+' : '=';
  case 0x2F:
    return shift ? '{' : '[';
  case 0x30:
    return shift ? '}' : ']';
  case 0x31:
    return shift ? '|' : '\\';
  case 0x33:
    return shift ? ':' : ';';
  case 0x34:
    return shift ? '"' : '\'';
  case 0x35:
    return shift ? '~' : '`';
  case 0x36:
    return shift ? '<' : ',';
  case 0x37:
    return shift ? '>' : '.';
  case 0x38:
    return shift ? '?' : '/';
  default:
    return 0;
  }
}

// ============================================================================
// Capabilities
// ============================================================================

/** @brief Device classes a device can belong to; combined in @ref input_capabilities::classes. */
enum class input_class : std::uint32_t {
  keyboard = 1u << 0,
  mouse = 1u << 1,  ///< relative pointer
  tablet = 1u << 2, ///< absolute pointer (tablet, touchscreen, touchpad)
  gamepad = 1u << 3,
  hid_reports = 1u << 4, ///< raw HID reports are available through `try_read_report`
};

[[nodiscard]] constexpr std::uint32_t operator|(input_class a, input_class b) noexcept {
  return static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b);
}
[[nodiscard]] constexpr std::uint32_t operator|(std::uint32_t a, input_class b) noexcept {
  return a | static_cast<std::uint32_t>(b);
}

/** @brief Keyboard LED bits for `set_leds`. */
enum class input_led : std::uint8_t { num_lock = 1u << 0, caps_lock = 1u << 1, scroll_lock = 1u << 2 };

/** @brief What a device is. */
struct input_capabilities {
  std::uint32_t classes = 0;   ///< bitwise OR of @ref input_class
  std::uint16_t vendor_id = 0; ///< USB-style identification; 0 if unknown
  std::uint16_t product_id = 0;
  /// Whether the backend supplies `input_traits::set_callback`/`clear_callback` (interrupt-driven notification).
  bool supports_callback = false;

  [[nodiscard]] constexpr bool has(input_class c) const noexcept {
    return (classes & static_cast<std::uint32_t>(c)) != 0;
  }
};

/** @brief Range of an absolute axis, for scaling to a screen. */
struct input_abs_info {
  std::int32_t minimum = 0;
  std::int32_t maximum = 0;
  std::int32_t resolution = 0; ///< units per mm (0 = unknown)
};

// ============================================================================
// Customization Point
// ============================================================================

/**
 * @brief Opt-in customization point describing how to poll a concrete
 * input device, through @ref input_device_ref. Intentionally left undefined
 * for any `Backend` that hasn't been adapted. See the @file-level docs above
 * for the required/optional member list.
 */
template <typename Backend> struct input_traits;

namespace detail {

template <typename Backend, typename = void> struct has_input_traits : std::false_type {};

template <typename Backend>
struct has_input_traits<
    Backend, std::void_t<decltype(input_traits<Backend>::capabilities), decltype(input_traits<Backend>::event_ready),
                         decltype(input_traits<Backend>::try_read_event)>> : std::true_type {};

template <typename Traits, typename = void> struct input_has_set_leds : std::false_type {};
template <typename Traits>
struct input_has_set_leds<Traits, std::void_t<decltype(Traits::set_leds)>> : std::true_type {};

template <typename Traits, typename = void> struct input_has_abs_info : std::false_type {};
template <typename Traits>
struct input_has_abs_info<Traits, std::void_t<decltype(Traits::abs_info)>> : std::true_type {};

template <typename Traits, typename = void> struct input_has_report : std::false_type {};
template <typename Traits>
struct input_has_report<Traits, std::void_t<decltype(Traits::try_read_report)>> : std::true_type {};

// Detects the optional set_callback/clear_callback pair -- both or neither.
template <typename Traits, typename = void> struct input_has_callback : std::false_type {};
template <typename Traits>
struct input_has_callback<Traits, std::void_t<decltype(Traits::set_callback), decltype(Traits::clear_callback)>>
    : std::true_type {};

template <typename Traits, typename = void> struct input_has_flush : std::false_type {};
template <typename Traits> struct input_has_flush<Traits, std::void_t<decltype(Traits::flush)>> : std::true_type {};

} // namespace detail

// ============================================================================
// Type-Erased Input Device Handle
// ============================================================================

/**
 * @brief Type-erased, non-owning handle over a polled input device
 * (keyboard, mouse, HID, ...). A pure two-pointer (ctx + vtable) forwarder,
 * like `uart_ref`.
 *
 * Default-constructed refs are *unbound*: every operation fails with
 * `error::unsupported_operation` rather than trapping.
 */
class RELOCO_POINTER input_device_ref {
public:
  /** @brief Spin-loop iteration bound of the blocking `read_event` overload. */
  static constexpr std::uint32_t default_max_spins = 1'000'000;

  /** @brief Fixed, per-bound-backend-type dispatch table. */
  struct vtable {
    input_capabilities (*capabilities)(void *ctx) noexcept;
    result<bool> (*event_ready)(void *ctx) noexcept;
    result<input_event> (*try_read_event)(void *ctx) noexcept;
    result<void> (*set_leds)(void *ctx, std::uint8_t mask) noexcept;
    result<input_abs_info> (*abs_info)(void *ctx, input_axis axis) noexcept;
    result<std::size_t> (*try_read_report)(void *ctx, span<std::uint8_t> dst) noexcept;
    result<void> (*flush)(void *ctx) noexcept;
    result<void> (*set_callback)(void *ctx, function_ref<void()> cb) noexcept;
    result<void> (*clear_callback)(void *ctx) noexcept;
  };

  /** @brief Constructs an unbound ref. */
  constexpr input_device_ref() noexcept = default;

  /**
   * @brief Binds this ref to an existing, adapted backend.
   * @tparam Backend Concrete backend type, deduced. Must have an
   * @ref input_traits specialization.
   * @param b Backend to bind. Must outlive this handle and every copy of it.
   */
  template <typename Backend, std::enable_if_t<detail::has_input_traits<Backend>::value, int> = 0>
  constexpr explicit input_device_ref(Backend &b RELOCO_LIFETIMEBOUND RELOCO_LIFETIME_CAPTURE_BY_THIS) noexcept
      : ctx_(std::addressof(b)), vtbl_(&s_vtbl<Backend>) {}

  /** @brief Rejects rvalue/temporary backend bindings. */
  template <typename Backend, std::enable_if_t<!std::is_lvalue_reference_v<Backend>, int> = 0>
  input_device_ref(Backend &&) = delete;

  /** @brief Whether this ref is bound to a backend. */
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return vtbl_ != nullptr; }

  /** @brief What the device is; all-zero when unbound. */
  [[nodiscard]] input_capabilities capabilities() const noexcept {
    return vtbl_ ? vtbl_->capabilities(ctx_) : input_capabilities{};
  }

  /** @brief Whether an event can be read without blocking. */
  [[nodiscard]] result<bool> event_ready() const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->event_ready(ctx_);
  }

  /** @brief Removes and returns the oldest pending event; fails with `error::try_again` if there is none. */
  [[nodiscard]] result<input_event> try_read_event() const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->try_read_event(ctx_);
  }

  /** @brief Sets the keyboard LEDs (`input_led` bits); `error::unsupported_operation` if the backend cannot. */
  [[nodiscard]] result<void> set_leds(std::uint8_t mask) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->set_leds(ctx_, mask);
  }

  /** @brief Range of an absolute axis; `error::unsupported_operation` if the backend does not report it. */
  [[nodiscard]] result<input_abs_info> abs_info(input_axis axis) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->abs_info(ctx_, axis);
  }

  /**
   * @brief Reads one raw HID input report into `dst` (see the @file docs).
   * @return The report length; `error::try_again` if none is pending, `error::unsupported_operation` if the
   * backend has no raw reports.
   */
  [[nodiscard]] result<std::size_t> try_read_report(span<std::uint8_t> dst) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->try_read_report(ctx_, dst);
  }

  /** @brief Discards everything pending (e.g. before a "press any key" prompt). */
  [[nodiscard]] result<void> flush() const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->flush(ctx_);
  }

  /**
   * @brief Registers `cb` to be invoked (typically from interrupt context) whenever input becomes available;
   * see "Polled and interrupt-driven access" in the @file docs. `cb` must stay valid until `clear_callback()`.
   * Fails with `error::unsupported_operation` if unbound or the backend has no interrupt support
   * (`capabilities().supports_callback`); polling remains available either way.
   */
  [[nodiscard]] result<void> set_callback(function_ref<void()> cb) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->set_callback(ctx_, cb);
  }

  /** @brief Unregisters the callback set by `set_callback()`; a no-op if none is set. */
  [[nodiscard]] result<void> clear_callback() const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->clear_callback(ctx_);
  }

  // --------------------------------------------------------------------
  // Generic conveniences, synthesized from the mandatory operations.
  // --------------------------------------------------------------------

  /**
   * @brief Waits for the next event, spinning on `event_ready` for up to `max_spins` iterations.
   * Fails with `error::timed_out` if none arrives, or whatever the backend reports.
   */
  [[nodiscard]] result<input_event> read_event(std::uint32_t max_spins = default_max_spins) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    for (std::uint32_t i = 0; i < max_spins; ++i) {
      auto ready = vtbl_->event_ready(ctx_);
      if (!ready)
        return unexpected(ready.error());
      if (ready.value())
        return vtbl_->try_read_event(ctx_);
    }
    return unexpected(error::timed_out);
  }

  /**
   * @brief Drains whatever is *currently* pending (never blocks) into `dst`, stopping early when `dst` is full.
   * @return The number of events read (may be `0`).
   */
  [[nodiscard]] result<std::size_t> read_available(span<input_event> dst) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    std::size_t n = 0;
    while (n < dst.size()) {
      auto ready = vtbl_->event_ready(ctx_);
      if (!ready)
        return unexpected(ready.error());
      if (!ready.value())
        break;
      auto ev = vtbl_->try_read_event(ctx_);
      if (!ev)
        return unexpected(ev.error());
      dst[n++] = ev.value();
    }
    return n;
  }

  /**
   * @brief Waits for the next key *press* (or auto-repeat) and returns its HID usage, skipping every other
   * event (releases, pointer motion, sync). Convenient for menus and "press any key".
   * Fails with `error::timed_out` after `max_spins` consecutive empty polls.
   */
  [[nodiscard]] result<std::uint16_t> read_key(std::uint32_t max_spins = default_max_spins) const noexcept {
    for (;;) {
      auto ev = read_event(max_spins);
      if (!ev)
        return unexpected(ev.error());
      if (ev.value().type == input_event_type::key && ev.value().value != 0)
        return ev.value().code;
    }
  }

private:
  template <typename Backend> static input_capabilities capabilities_entry(void *ctx) noexcept {
    return input_traits<Backend>::capabilities(*static_cast<const Backend *>(ctx));
  }

  template <typename Backend> static result<bool> event_ready_entry(void *ctx) noexcept {
    return input_traits<Backend>::event_ready(*static_cast<Backend *>(ctx));
  }

  template <typename Backend> static result<input_event> try_read_event_entry(void *ctx) noexcept {
    return input_traits<Backend>::try_read_event(*static_cast<Backend *>(ctx));
  }

  template <typename Backend> static result<void> set_leds_entry(void *ctx, std::uint8_t mask) noexcept {
    if constexpr (detail::input_has_set_leds<input_traits<Backend>>::value) {
      return input_traits<Backend>::set_leds(*static_cast<Backend *>(ctx), mask);
    } else {
      (void)ctx;
      (void)mask;
      return unexpected(error::unsupported_operation);
    }
  }

  template <typename Backend> static result<input_abs_info> abs_info_entry(void *ctx, input_axis axis) noexcept {
    if constexpr (detail::input_has_abs_info<input_traits<Backend>>::value) {
      return input_traits<Backend>::abs_info(*static_cast<Backend *>(ctx), axis);
    } else {
      (void)ctx;
      (void)axis;
      return unexpected(error::unsupported_operation);
    }
  }

  template <typename Backend>
  static result<std::size_t> try_read_report_entry(void *ctx, span<std::uint8_t> dst) noexcept {
    if constexpr (detail::input_has_report<input_traits<Backend>>::value) {
      return input_traits<Backend>::try_read_report(*static_cast<Backend *>(ctx), dst);
    } else {
      (void)ctx;
      (void)dst;
      return unexpected(error::unsupported_operation);
    }
  }

  template <typename Backend> static result<void> flush_entry(void *ctx) noexcept {
    if constexpr (detail::input_has_flush<input_traits<Backend>>::value) {
      return input_traits<Backend>::flush(*static_cast<Backend *>(ctx));
    } else {
      // Fallback: drain through the mandatory operations.
      for (;;) {
        auto ready = input_traits<Backend>::event_ready(*static_cast<Backend *>(ctx));
        if (!ready)
          return unexpected(ready.error());
        if (!ready.value())
          return {};
        auto ev = input_traits<Backend>::try_read_event(*static_cast<Backend *>(ctx));
        if (!ev)
          return unexpected(ev.error());
      }
    }
  }

  template <typename Backend> static result<void> set_callback_entry(void *ctx, function_ref<void()> cb) noexcept {
    if constexpr (detail::input_has_callback<input_traits<Backend>>::value) {
      return input_traits<Backend>::set_callback(*static_cast<Backend *>(ctx), cb);
    } else {
      (void)ctx;
      (void)cb;
      return unexpected(error::unsupported_operation);
    }
  }

  template <typename Backend> static result<void> clear_callback_entry(void *ctx) noexcept {
    if constexpr (detail::input_has_callback<input_traits<Backend>>::value) {
      return input_traits<Backend>::clear_callback(*static_cast<Backend *>(ctx));
    } else {
      (void)ctx;
      return unexpected(error::unsupported_operation);
    }
  }

  template <typename Backend>
  static constexpr vtable s_vtbl{
      &capabilities_entry<Backend>, &event_ready_entry<Backend>,  &try_read_event_entry<Backend>,
      &set_leds_entry<Backend>,     &abs_info_entry<Backend>,     &try_read_report_entry<Backend>,
      &flush_entry<Backend>,        &set_callback_entry<Backend>, &clear_callback_entry<Backend>};

  void *ctx_ = nullptr;
  const vtable *vtbl_ = nullptr;
};

} // namespace hw
} // namespace structo
