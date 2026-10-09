// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file console_uart.hpp
 * @brief `structo::hw::console_uart`: a bridge that presents a keyboard
 * (@ref input_device_ref) plus a VT100 text console (@ref console_ref via
 * @ref vt100_terminal) as a plain polled UART, so anything written against
 * `uart_ref` -- the bootloader shell, the debug menu, XMODEM-less text
 * tools, the text editor -- runs unchanged on a machine with only a
 * keyboard and a screen.
 *
 * The caller supplies both adapters (an `input_device_ref` bound to a
 * keyboard backend and a `console_ref` bound to a `vga_text_console`,
 * `framebuffer_console<...>` or fake); the bridge owns only the translation
 * state and is itself a `uart_traits` backend, bound with `uart_ref`:
 *
 * @code
 * structo::hw::input_device_ref kbd(my_keyboard);          // keyboard adapter (event stream)
 * structo::hw::console_ref screen(my_vga);                 // text console adapter
 * structo::hw::console_uart bridge(kbd, screen);           // must outlive `uart` (not copyable/movable)
 * structo::hw::uart_ref uart(bridge);                      // drop-in UART for shell / prompts
 * @endcode
 *
 * ## Output (TX): bytes -> screen
 *
 * Every byte sent with `put_byte`/`write`/`write_string` is fed to the
 * `vt100_terminal`, so ANSI escape sequences (colors, cursor movement,
 * erase) work as on a serial terminal. VT100 `'\n'` only moves down a row
 * and does not return the carriage, while UART users write plain `'\n'`;
 * the bridge therefore expands a lone `'\n'` to `"\r\n"`. A `'\n'` that
 * directly follows a `'\r'` (what `uart_ref::write_string` already emits) is
 * passed through unchanged, so a line is never doubled to `"\r\r\n"`.
 *
 * ## Input (RX): key events -> bytes
 *
 * Key presses (and auto-repeats) are translated the way a serial terminal
 * would send them; releases only update the modifier state. Tracked state:
 * Shift (either), Ctrl (either) and Caps Lock (toggled on press, mirrored on
 * the keyboard LED when the device supports `set_leds`).
 *
 * | Key                         | Bytes                                         |
 * |-----------------------------|-----------------------------------------------|
 * | printable (US layout)       | the character; Shift/Caps applied              |
 * | Ctrl + letter               | `0x01`..`0x1A` (Ctrl-C = `0x03`, Ctrl-U = `0x15`) |
 * | Enter                       | `'\r'` (as a serial terminal sends it)        |
 * | Backspace                   | `0x7F`                                         |
 * | Tab / Escape                | `'\t'` / `0x1B`                                |
 * | arrows, Home, End           | `ESC [ A/B/C/D`, `ESC [ H`, `ESC [ F`          |
 * | Insert/Delete/PgUp/PgDn     | `ESC [ 2~`, `ESC [ 3~`, `ESC [ 5~`, `ESC [ 6~` |
 * | F1..F4                      | `ESC O P`, `ESC O Q`, `ESC O R`, `ESC O S`     |
 * | F5..F12                     | `ESC [ 15~`, `17~`, `18~`, `19~`, `20~`, `21~`, `23~`, `24~` |
 *
 * (xterm/VT220 numbering; modifier combinations with F-keys are not encoded.) Other keys (modifiers alone,
 * keypad, ...) produce nothing. Pointer
 * and other non-key events are discarded.
 *
 * The bridge is polled: `rx_ready` pulls pending events from the input
 * device (via its polled interface, so it works whether or not that device
 * also offers interrupt callbacks). `configure` accepts and remembers any
 * settings (a keyboard/screen has no baud rate) so code that configures its
 * UART keeps working; `tx_ready` is always true.
 */

#include <structo/hw/console_ref.hpp>
#include <structo/hw/input_device_ref.hpp>
#include <structo/hw/uart_ref.hpp>
#include <structo/hw/vt100.hpp>

#include <reloco/error.hpp>

#include <cstddef>
#include <cstdint>

namespace structo {

using namespace reloco;

namespace hw {

/** @brief Keyboard + VT100 console presented as a UART backend; see the @file docs. */
class console_uart {
public:
  /**
   * @param input Keyboard (or other key-producing device) adapter; must stay valid while the bridge is used.
   * @param console Text console adapter the VT100 interpreter draws on; same lifetime requirement.
   */
  console_uart(input_device_ref input, console_ref console) noexcept : input_(input), term_(console) {}

  console_uart(const console_uart &) = delete;
  console_uart &operator=(const console_uart &) = delete;

  /** @brief The VT100 interpreter (e.g. to reach `console()` for cursor placement or colors). */
  [[nodiscard]] vt100_terminal &terminal() noexcept { return term_; }

  /** @brief Settings last given to `configure` (informational only). */
  [[nodiscard]] const uart_config &config() const noexcept { return cfg_; }

  /** @brief Whether a translated byte is available, pulling input events as needed. */
  [[nodiscard]] result<bool> rx_ready() noexcept {
    auto r = fill();
    if (!r)
      return unexpected(r.error());
    return head_ != tail_;
  }

  /** @brief Next translated input byte; `error::try_again` if none. */
  [[nodiscard]] result<std::uint8_t> try_get_byte() noexcept {
    auto r = fill();
    if (!r)
      return unexpected(r.error());
    if (head_ == tail_)
      return unexpected(error::try_again);
    return pending_[head_++];
  }

  /** @brief Draws `b`, expanding a lone `'\n'` to `"\r\n"`. */
  void put(std::uint8_t b) noexcept {
    const char c = static_cast<char>(b);
    if (c == '\n' && last_tx_ != '\r')
      term_.feed('\r');
    term_.feed(c);
    last_tx_ = c;
  }

  void set_config(const uart_config &cfg) noexcept { cfg_ = cfg; }

private:
  static constexpr std::size_t pending_capacity = 8; // longest sequence is 5 bytes; refilled only when empty

  void push(std::uint8_t b) noexcept { pending_[tail_++] = b; }
  void push_csi(char final_byte, char prefix_digit = 0) noexcept {
    push(0x1B);
    push('[');
    if (prefix_digit) {
      push(static_cast<std::uint8_t>(prefix_digit));
      push(static_cast<std::uint8_t>(final_byte));
    } else {
      push(static_cast<std::uint8_t>(final_byte));
    }
  }

  // F1-F4: SS3 P..S; F5-F12: CSI n ~ with the xterm/VT220 numbering (gaps at 16, 22).
  void push_function_key(unsigned n) noexcept {
    static constexpr std::uint8_t csi_numbers[] = {15, 17, 18, 19, 20, 21, 23, 24};
    push(0x1B);
    if (n <= 4) {
      push('O');
      push(static_cast<std::uint8_t>('P' + (n - 1)));
      return;
    }
    const std::uint8_t num = csi_numbers[n - 5];
    push('[');
    push(static_cast<std::uint8_t>('0' + num / 10));
    push(static_cast<std::uint8_t>('0' + num % 10));
    push('~');
  }

  // Pulls events until at least one byte is pending or the device has nothing more.
  [[nodiscard]] result<void> fill() noexcept {
    if (head_ != tail_)
      return {};
    head_ = tail_ = 0;
    while (head_ == tail_) {
      auto ready = input_.event_ready();
      if (!ready)
        return unexpected(ready.error());
      if (!ready.value())
        break;
      auto ev = input_.try_read_event();
      if (!ev)
        return unexpected(ev.error());
      translate(ev.value());
    }
    return {};
  }

  void translate(const input_event &ev) noexcept {
    if (ev.type != input_event_type::key)
      return;
    const std::uint16_t code = ev.code;
    const bool down = ev.value != 0;
    const bool is_press = ev.value == static_cast<std::int32_t>(input_key_state::pressed);

    if (code == static_cast<std::uint16_t>(hid_key::left_shift) || code == static_cast<std::uint16_t>(hid_key::right_shift)) {
      (code == static_cast<std::uint16_t>(hid_key::left_shift) ? lshift_ : rshift_) = down;
      return;
    }
    if (code == static_cast<std::uint16_t>(hid_key::left_ctrl) || code == static_cast<std::uint16_t>(hid_key::right_ctrl)) {
      (code == static_cast<std::uint16_t>(hid_key::left_ctrl) ? lctrl_ : rctrl_) = down;
      return;
    }
    if (code == static_cast<std::uint16_t>(hid_key::caps_lock)) {
      if (is_press) {
        caps_ = !caps_;
        // Best effort: not every keyboard has LEDs.
        (void)input_.set_leds(caps_ ? static_cast<std::uint8_t>(input_led::caps_lock) : std::uint8_t{0});
      }
      return;
    }
    if (!down)
      return;

    switch (static_cast<hid_key>(code)) {
    case hid_key::enter: push('\r'); return;
    case hid_key::backspace: push(0x7F); return;
    case hid_key::up: push_csi('A'); return;
    case hid_key::down: push_csi('B'); return;
    case hid_key::right: push_csi('C'); return;
    case hid_key::left: push_csi('D'); return;
    case hid_key::home: push_csi('H'); return;
    case hid_key::end: push_csi('F'); return;
    case hid_key::insert: push_csi('~', '2'); return;
    case hid_key::delete_key: push_csi('~', '3'); return;
    case hid_key::page_up: push_csi('~', '5'); return;
    case hid_key::page_down: push_csi('~', '6'); return;
    default: break;
    }

    if (code >= static_cast<std::uint16_t>(hid_key::f1) && code <= static_cast<std::uint16_t>(hid_key::f12)) {
      push_function_key(code - static_cast<std::uint16_t>(hid_key::f1) + 1u);
      return;
    }

    const bool shift = lshift_ || rshift_;
    const bool letter = code >= static_cast<std::uint16_t>(hid_key::a) && code <= static_cast<std::uint16_t>(hid_key::z);
    if (letter && (lctrl_ || rctrl_)) {
      push(static_cast<std::uint8_t>(code - static_cast<std::uint16_t>(hid_key::a) + 1));
      return;
    }
    const char c = hid_key_to_ascii(code, letter ? (shift != caps_) : shift);
    if (c != 0)
      push(static_cast<std::uint8_t>(c));
  }

  input_device_ref input_;
  vt100_terminal term_;
  uart_config cfg_{};
  std::uint8_t pending_[pending_capacity]{};
  std::size_t head_ = 0;
  std::size_t tail_ = 0;
  char last_tx_ = 0;
  bool lshift_ = false, rshift_ = false, lctrl_ = false, rctrl_ = false, caps_ = false;
};

/** @brief Makes `console_uart` bindable through `uart_ref`. */
template <> struct uart_traits<console_uart> {
  static result<void> configure(console_uart &b, const uart_config &cfg) noexcept {
    b.set_config(cfg);
    return {};
  }
  static result<uart_config> current_config(console_uart &b) noexcept { return b.config(); }
  static result<bool> tx_ready(console_uart &) noexcept { return true; }
  static result<bool> rx_ready(console_uart &b) noexcept { return b.rx_ready(); }
  static result<void> try_put_byte(console_uart &b, std::uint8_t v) noexcept {
    b.put(v);
    return {};
  }
  static result<std::uint8_t> try_get_byte(console_uart &b) noexcept { return b.try_get_byte(); }
};

} // namespace hw
} // namespace structo
