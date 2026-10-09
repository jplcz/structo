// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file uart_input.hpp
 * @brief `structo::hw::uart_input`: an adapter that turns any polled
 * character device (`uart_ref`: a serial port, a console UART, a pty) carrying
 * xterm-style terminal input into an @ref input_device_ref keyboard (plus
 * terminal mouse), producing HID key events from the byte stream. It is the
 * reverse of @ref console_uart's RX direction.
 *
 * @code
 * structo::hw::uart_ref uart(my_serial);            // the character device
 * structo::hw::uart_input kbd(uart);                // backend; not copyable, must outlive `input`
 * structo::hw::input_device_ref input(kbd);         // keyboard as an event stream
 * auto key = input.read_key();                      // HID usage of the next key press
 * @endcode
 *
 * ## What is decoded
 *
 * | Input bytes                                   | Events                                                  |
 * |-----------------------------------------------|---------------------------------------------------------|
 * | printable ASCII                               | the key (plus Shift for `A`, `!`, `{`, ...)             |
 * | `0x01`..`0x1A` (except Tab/Enter/BS), `0x00`  | Ctrl + letter / Ctrl + Space                            |
 * | `\r` or `\n`, `\t`, `0x7F` or `0x08`          | Enter, Tab, Backspace                                   |
 * | `ESC` + character                             | Alt + that key                                          |
 * | `ESC [ A/B/C/D/H/F`, `ESC O A/B/C/D/H/F`      | arrows, Home, End                                       |
 * | `ESC [ n ~`                                   | Home(1,7), Insert(2), Delete(3), End(4,8), PgUp(5), PgDn(6), F1-F5 (11-15), F6-F10 (17-21), F11/F12 (23/24) |
 * | `ESC O P/Q/R/S`, `ESC [ P/Q/R/S`             | F1-F4                                                   |
 * | `ESC [ Z`                                     | Shift + Tab                                             |
 * | `ESC [ 1 ; m <final>` / `ESC [ n ; m ~`       | the same keys with the xterm modifier `m` (1 + bitmask of Shift=1, Alt=2, Ctrl=4) |
 * | `ESC [ < b ; x ; y M/m` (SGR mouse, mode 1006)| `abs` x/y (0-based cell), button press/release, wheel   |
 *
 * A terminal does not report key releases, so each decoded key becomes
 * "modifiers pressed, key pressed, key released, modifiers released" followed
 * by a `sync` event. Unrecognized sequences and bytes `>= 0x80` are dropped.
 *
 * ## Lone Escape
 *
 * A bare `ESC` is ambiguous with the start of a sequence and there is no
 * clock here, so it is emitted as an Escape key only after
 * @ref uart_input_config::escape_polls consecutive polls found no following
 * byte. Sequences sent by terminals arrive together, so the default is
 * generous without making Escape feel stuck when polled in a loop.
 *
 * Input is polled; this backend supplies no interrupt callback
 * (`capabilities().supports_callback == false`).
 */

#include <structo/hw/input_device_ref.hpp>
#include <structo/hw/uart_ref.hpp>

#include <reloco/error.hpp>

#include <cstddef>
#include <cstdint>

namespace structo {

using namespace reloco;

namespace hw {

/** @brief Tuning of @ref uart_input. */
struct uart_input_config {
  /// Consecutive idle polls after a bare `ESC` before it is reported as the Escape key.
  std::uint32_t escape_polls = 100;
};

/** @brief xterm-input decoder over a `uart_ref`, bindable through `input_device_ref`; see the @file docs. */
class uart_input {
public:
  explicit uart_input(uart_ref uart, const uart_input_config &cfg = {}) noexcept : uart_(uart), cfg_(cfg) {}

  uart_input(const uart_input &) = delete;
  uart_input &operator=(const uart_input &) = delete;

  /** @brief Whether an event is queued, decoding pending UART bytes as needed. */
  [[nodiscard]] result<bool> event_ready() noexcept {
    auto r = fill();
    if (!r)
      return unexpected(r.error());
    return head_ != tail_;
  }

  /** @brief Oldest decoded event; `error::try_again` if none. */
  [[nodiscard]] result<input_event> try_read_event() noexcept {
    auto r = fill();
    if (!r)
      return unexpected(r.error());
    if (head_ == tail_)
      return unexpected(error::try_again);
    return queue_[head_++];
  }

private:
  static constexpr std::size_t queue_capacity = 16; // one decoded sequence is at most 9 events; refilled only when empty
  static constexpr std::size_t max_params = 4;

  enum class state : std::uint8_t { ground, escape, ss3, csi };

  // xterm modifier bits.
  static constexpr unsigned mod_shift = 1, mod_alt = 2, mod_ctrl = 4;

  static constexpr std::uint16_t U(hid_key k) noexcept { return static_cast<std::uint16_t>(k); }

  void emit(const input_event &e) noexcept { queue_[tail_++] = e; }

  void emit_key(std::uint16_t usage, unsigned mods) noexcept {
    const std::uint16_t mod_keys[] = {U(hid_key::left_shift), U(hid_key::left_ctrl), U(hid_key::left_alt)};
    const unsigned mod_bits[] = {mod_shift, mod_ctrl, mod_alt};
    for (std::size_t i = 0; i < 3; ++i)
      if (mods & mod_bits[i])
        emit(make_key_event(mod_keys[i], input_key_state::pressed));
    emit(make_key_event(usage, input_key_state::pressed));
    emit(make_key_event(usage, input_key_state::released));
    for (std::size_t i = 3; i-- > 0;)
      if (mods & mod_bits[i])
        emit(make_key_event(mod_keys[i], input_key_state::released));
    emit(make_sync_event());
  }

  // Inverse of hid_key_to_ascii (US layout) for printable characters.
  static bool ascii_to_key(char c, std::uint16_t &usage, bool &shift) noexcept {
    if (c < 0x20 || c >= 0x7F)
      return false;
    for (std::uint16_t u = U(hid_key::a); u <= U(hid_key::slash); ++u) {
      if (hid_key_to_ascii(u, false) == c) {
        usage = u;
        shift = false;
        return true;
      }
      if (hid_key_to_ascii(u, true) == c) {
        usage = u;
        shift = true;
        return true;
      }
    }
    return false;
  }

  // Ground-state byte, `alt` set when it followed ESC.
  void decode_char(std::uint8_t b, unsigned alt) noexcept {
    switch (b) {
    case '\r':
    case '\n': emit_key(U(hid_key::enter), alt); return;
    case '\t': emit_key(U(hid_key::tab), alt); return;
    case 0x7F:
    case 0x08: emit_key(U(hid_key::backspace), alt); return;
    case 0x00: emit_key(U(hid_key::space), mod_ctrl | alt); return;
    default: break;
    }
    if (b >= 0x01 && b <= 0x1A) {
      emit_key(static_cast<std::uint16_t>(U(hid_key::a) + (b - 1)), mod_ctrl | alt);
      return;
    }
    std::uint16_t usage = 0;
    bool shift = false;
    if (ascii_to_key(static_cast<char>(b), usage, shift))
      emit_key(usage, (shift ? mod_shift : 0u) | alt);
    // else: bytes >= 0x80 (UTF-8) and rarely used controls are dropped.
  }

  void reset_params() noexcept {
    for (auto &p : params_)
      p = 0;
    nparams_ = 0;
    cur_ = 0;
    have_cur_ = false;
    prefix_ = 0;
  }

  void push_param() noexcept {
    if (nparams_ < max_params)
      params_[nparams_] = cur_;
    ++nparams_; // may exceed max_params; only the first max_params are kept
    cur_ = 0;
    have_cur_ = false;
  }

  [[nodiscard]] unsigned xterm_mods() const noexcept {
    const std::uint32_t m = nparams_ > 1 ? params_[1] : 0;
    return m > 1 ? static_cast<unsigned>(m - 1) & (mod_shift | mod_alt | mod_ctrl) : 0u;
  }

  static std::uint16_t function_key(unsigned n) noexcept { return static_cast<std::uint16_t>(U(hid_key::f1) + (n - 1)); }

  void decode_mouse(char final_byte) noexcept {
    if (nparams_ < 3)
      return;
    const std::uint32_t b = params_[0];
    const std::int32_t x = static_cast<std::int32_t>(params_[1]) - 1;
    const std::int32_t y = static_cast<std::int32_t>(params_[2]) - 1;
    emit(make_abs_event(input_axis::x, x));
    emit(make_abs_event(input_axis::y, y));
    if (b & 64u) { // wheel: 64 = up, 65 = down, 66/67 = horizontal
      if (final_byte == 'M') {
        const std::int32_t d = (b & 1u) ? -1 : 1;
        emit(make_rel_event((b & 2u) ? input_axis::hwheel : input_axis::wheel, d));
      }
    } else if (!(b & 32u)) { // 32 = motion without a button change
      static constexpr input_button buttons[] = {input_button::left, input_button::middle, input_button::right};
      if ((b & 3u) < 3)
        emit(make_button_event(buttons[b & 3u], final_byte == 'M'));
    }
    emit(make_sync_event());
  }

  void decode_csi(char f) noexcept {
    if (have_cur_)
      push_param();
    if (prefix_ == '<') {
      if (f == 'M' || f == 'm')
        decode_mouse(f);
      return;
    }
    const unsigned mods = xterm_mods();
    switch (f) {
    case 'A': emit_key(U(hid_key::up), mods); return;
    case 'B': emit_key(U(hid_key::down), mods); return;
    case 'C': emit_key(U(hid_key::right), mods); return;
    case 'D': emit_key(U(hid_key::left), mods); return;
    case 'H': emit_key(U(hid_key::home), mods); return;
    case 'F': emit_key(U(hid_key::end), mods); return;
    case 'P':
    case 'Q':
    case 'R':
    case 'S': emit_key(function_key(static_cast<unsigned>(f - 'P') + 1u), mods); return;
    case 'Z': emit_key(U(hid_key::tab), mod_shift); return;
    case '~': break;
    default: return;
    }
    switch (params_[0]) {
    case 1:
    case 7: emit_key(U(hid_key::home), mods); return;
    case 2: emit_key(U(hid_key::insert), mods); return;
    case 3: emit_key(U(hid_key::delete_key), mods); return;
    case 4:
    case 8: emit_key(U(hid_key::end), mods); return;
    case 5: emit_key(U(hid_key::page_up), mods); return;
    case 6: emit_key(U(hid_key::page_down), mods); return;
    case 11: case 12: case 13: case 14: case 15:
      emit_key(function_key(params_[0] - 10), mods);
      return;
    case 17: case 18: case 19: case 20: case 21:
      emit_key(function_key(params_[0] - 11), mods); // 17 -> F6 ... 21 -> F10
      return;
    case 23: emit_key(function_key(11), mods); return;
    case 24: emit_key(function_key(12), mods); return;
    default: return;
    }
  }

  void feed(std::uint8_t b) noexcept {
    switch (state_) {
    case state::ground:
      if (b == 0x1B) {
        state_ = state::escape;
        return;
      }
      decode_char(b, 0);
      return;
    case state::escape:
      state_ = state::ground;
      if (b == '[') {
        state_ = state::csi;
        reset_params();
      } else if (b == 'O') {
        state_ = state::ss3;
      } else if (b == 0x1B) {
        emit_key(U(hid_key::escape), 0); // ESC ESC: report one, treat the second as a new start
        state_ = state::escape;
      } else {
        decode_char(b, mod_alt);
      }
      return;
    case state::ss3:
      state_ = state::ground;
      switch (b) {
      case 'A': emit_key(U(hid_key::up), 0); return;
      case 'B': emit_key(U(hid_key::down), 0); return;
      case 'C': emit_key(U(hid_key::right), 0); return;
      case 'D': emit_key(U(hid_key::left), 0); return;
      case 'H': emit_key(U(hid_key::home), 0); return;
      case 'F': emit_key(U(hid_key::end), 0); return;
      case 'P': case 'Q': case 'R': case 'S':
        emit_key(function_key(static_cast<unsigned>(b - 'P') + 1u), 0);
        return;
      default: return;
      }
    case state::csi:
      if (b >= '0' && b <= '9') {
        cur_ = cur_ * 10 + (b - '0');
        have_cur_ = true;
      } else if (b == ';') {
        push_param();
      } else if (b == '<' || b == '?' || b == '>' || b == '=') {
        prefix_ = static_cast<char>(b);
      } else if (b >= 0x40 && b <= 0x7E) { // final byte
        state_ = state::ground;
        decode_csi(static_cast<char>(b));
      } else if (b < 0x20 || b > 0x2F) { // not an intermediate byte: malformed, abandon
        state_ = state::ground;
      }
      return;
    }
  }

  // Decodes UART bytes until at least one event is queued or the UART runs dry.
  [[nodiscard]] result<void> fill() noexcept {
    if (head_ != tail_)
      return {};
    head_ = tail_ = 0;
    while (head_ == tail_) {
      auto ready = uart_.rx_ready();
      if (!ready)
        return unexpected(ready.error());
      if (!ready.value()) {
        if (state_ == state::escape && ++idle_ >= cfg_.escape_polls) {
          state_ = state::ground;
          idle_ = 0;
          emit_key(U(hid_key::escape), 0);
        }
        break;
      }
      idle_ = 0;
      auto b = uart_.try_get_byte();
      if (!b)
        return unexpected(b.error());
      feed(b.value());
    }
    return {};
  }

  uart_ref uart_;
  uart_input_config cfg_;
  input_event queue_[queue_capacity]{};
  std::size_t head_ = 0;
  std::size_t tail_ = 0;
  state state_ = state::ground;
  std::uint32_t params_[max_params]{};
  std::size_t nparams_ = 0;
  std::uint32_t cur_ = 0;
  bool have_cur_ = false;
  char prefix_ = 0;
  std::uint32_t idle_ = 0;
};

/** @brief Makes `uart_input` bindable through `input_device_ref`. */
template <> struct input_traits<uart_input> {
  static input_capabilities capabilities(const uart_input &) noexcept {
    return {input_class::keyboard | input_class::tablet};
  }
  static result<bool> event_ready(uart_input &b) noexcept { return b.event_ready(); }
  static result<input_event> try_read_event(uart_input &b) noexcept { return b.try_read_event(); }
};

} // namespace hw
} // namespace structo
