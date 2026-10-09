// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file vt100.hpp
 * @brief `structo::hw::vt100_terminal`: an xterm-flavoured VT100/ANSI
 * escape-sequence interpreter built on top of @ref console_ref, turning
 * a byte stream into console operations -- usable with any bound
 * backend (`vga_text_console`, `framebuffer_console<PixelFormat>`, or a
 * test fake) since it only ever talks to the `console_ref` abstraction.
 *
 * ## Scope
 *
 * A VT100 core plus the commonly used xterm extensions -- enough for
 * shells (bash, zsh with line editing), `less`, `top`-style tools -- but
 * not a complete xterm. Everything is 16-colour text-cell based; there is
 * no scrollback, alternate screen buffer, mouse generation or Unicode.
 * - Cursor movement: `CUU`/`CUD`/`CUF`/`CUB` (`A`/`B`/`C`/`D`), `CNL`/`CPL`
 *   (`E`/`F`), `CHA` (`G`), `VPA` (`d`), `CUP`/`HVP` (`H`/`f`), `HPA`/`VPR`
 *   aliases (`` ` ``, `a`, `e`), save/restore (`ESC 7`/`ESC 8`, `CSI s`/`CSI u`).
 * - Erasing and editing: `ED` (`J`), `EL` (`K`), `ECH` (`X`), `ICH` (`@`),
 *   `DCH` (`P`), `IL` (`L`), `DL` (`M`).
 * - Scrolling: `IND`/`NEL`/`RI` (`ESC D`/`E`/`M`), `SU`/`SD` (`CSI S`/`T`)
 *   and a top/bottom scroll region (`DECSTBM`, `CSI t ; b r`).
 * - Deferred ("xenl") autowrap like xterm: writing in the last column
 *   leaves the cursor there with a pending wrap, so a full-width line
 *   followed by `\r\n` does not produce a blank row. `CSI ? 7 l/h` toggles it.
 * - `SGR` (`m`): 16 colours (`30-37`, `40-47`, `90-97`, `100-107`), `1`/`22`
 *   (bold = bright foreground), `7`/`27` (reverse), `39`/`49`, `0`, and the
 *   256-colour / true-colour forms `38;5;n`, `38;2;r;g;b` (and `48;...`),
 *   mapped to the nearest of the 16 colours. Underline/italic/etc. are ignored.
 * - `CSI ? 25 h/l` shows/hides the cursor; `ESC c` (RIS) resets everything.
 * - `OSC` strings (`ESC ] ... BEL` or `... ESC \`) are consumed; window
 *   titles and other payloads are delivered to callbacks (see below).
 * - Control characters: `\r`, `\n`/`\v`/`\f` (**strict line feed: moves down
 *   one row without returning the carriage**, scrolling at the bottom of
 *   the scroll region -- pair with `\r`, as a pty's `onlcr` does), `\b`,
 *   `\t`, BEL (callback). NUL, DEL and bytes >= 0x80 are ignored.
 * - Anything else (unknown CSI/ESC/private modes) is consumed and ignored
 *   rather than leaking onto the screen.
 *
 * ## Events
 *
 * Things that are not drawing but a program on the other end might care
 * about are reported through @ref vt100_callbacks: bell, window title,
 * other OSC payloads, DEC private mode changes (application cursor keys,
 * bracketed paste, mouse reporting modes, alternate screen, ...), cursor
 * style (`DECSCUSR`), terminal reset, and *replies* the terminal must send
 * back to the application (`DSR` cursor-position/status reports and `DA`
 * device attributes -- write the given bytes to the application's input).
 * All callbacks are optional.
 *
 * The ANSI color index to `console_color` mapping follows the
 * conventional Linux-console/most-terminal-emulators assignment:
 * `{black, red, green, yellow, blue, magenta, cyan, white}` for `0-7`
 * (brightened to `{dark_gray, light_red, light_green, yellow,
 * light_blue, light_magenta, light_cyan, white}` by bold/`90-97`),
 * noting ANSI "yellow" is CGA's `brown` at standard intensity.
 */

#include "console_ref.hpp"

#include <reloco/array.hpp>
#include <reloco/string_view.hpp>

#include <cstddef>
#include <cstdint>

namespace structo {
namespace hw {

/** @brief DEC private modes (`CSI ? n h/l`, and `ESC =`/`ESC >`) reported to @ref vt100_callbacks::mode.
 * Values are the DEC mode numbers, so an unlisted mode is still passed through as `static_cast<vt100_mode>(n)`. */
enum class vt100_mode : std::uint16_t {
  application_cursor_keys = 1, ///< DECCKM: arrows should be sent as `ESC O A..D` instead of `ESC [ A..D`
  autowrap = 7,                ///< DECAWM (handled by the terminal itself, also reported)
  mouse_x10 = 9,
  cursor_visible = 25, ///< DECTCEM (handled by the terminal itself, also reported)
  alt_screen_47 = 47,
  application_keypad = 66, ///< DECNKM: reported for `ESC =` (on) / `ESC >` (off)
  mouse_normal = 1000,
  mouse_button = 1002,
  mouse_any = 1003,
  focus_events = 1004,
  mouse_sgr = 1006,
  alt_screen = 1047,
  alt_screen_save = 1049,
  bracketed_paste = 2004,
};

/**
 * @brief Optional hooks for events a terminal emulator can detect in the byte stream. Every member may be
 * left null. `ctx` is passed as the first argument; string views are valid only for the call's duration.
 */
struct vt100_callbacks {
  void *ctx = nullptr;
  /** BEL (`0x07`) outside an escape sequence. */
  void (*bell)(void *ctx) noexcept = nullptr;
  /** Window title: `OSC 0 ; text ST` or `OSC 2 ; text ST` (truncated to 255 bytes). */
  void (*title)(void *ctx, reloco::string_view text) noexcept = nullptr;
  /** Any other `OSC code ; payload ST` (cwd report 7, hyperlink 8, clipboard 52, ...). */
  void (*osc)(void *ctx, std::uint32_t code, reloco::string_view payload) noexcept = nullptr;
  /** A DEC private mode was set (`true`) or reset (`false`). */
  void (*mode)(void *ctx, vt100_mode mode, bool enabled) noexcept = nullptr;
  /** `CSI n SP q` (DECSCUSR): cursor style 0..6 (blinking/steady block/underline/bar). */
  void (*cursor_style)(void *ctx, std::uint32_t style) noexcept = nullptr;
  /** Bytes the terminal answers with (DSR `CSI 5n`/`CSI 6n`, DA `CSI c`): send them to the application. */
  void (*reply)(void *ctx, reloco::string_view bytes) noexcept = nullptr;
  /** `ESC c` (RIS): full terminal reset was performed. */
  void (*reset)(void *ctx) noexcept = nullptr;
};

/**
 * @brief An xterm-flavoured escape-sequence interpreter that owns a
 * @ref console_ref and drives it from a fed byte stream. See the
 * @file-level docs above for the supported subset.
 */
class vt100_terminal {
public:
  constexpr vt100_terminal() noexcept = default;

  /** @brief Constructs a terminal driving @p console (copied by value;
   * `console_ref` is itself just a lightweight non-owning handle). */
  constexpr explicit vt100_terminal(console_ref console) noexcept : console_(console) {}

  /** @brief The underlying console this terminal drives. */
  [[nodiscard]] constexpr console_ref &console() noexcept { return console_; }
  [[nodiscard]] constexpr const console_ref &console() const noexcept { return console_; }

  /** @brief Installs event callbacks (copied; `callbacks.ctx` must outlive the terminal's use of it). */
  constexpr void set_callbacks(const vt100_callbacks &callbacks) noexcept { cb_ = callbacks; }

  /** @brief Feeds one byte of input, advancing the escape-sequence
   * state machine and/or performing the resulting console operation. */
  void feed(char c) noexcept {
    switch (state_) {
    case state::ground:
      feed_ground(c);
      break;
    case state::escape:
      feed_escape(c);
      break;
    case state::csi:
      feed_csi(c);
      break;
    case state::osc:
      feed_osc(c);
      break;
    case state::osc_esc:
      if (c == '\\') {
        finish_osc();
        state_ = state::ground;
      } else {
        state_ = state::escape; // ESC inside an OSC aborts it and starts a new sequence
        feed_escape(c);
      }
      break;
    }
  }

  /** @brief Feeds every byte of @p text via `feed`. */
  void feed(reloco::string_view text) noexcept {
    for (char c : text) {
      feed(c);
    }
  }

  /**
   * @brief Returns a type-erased `reloco::sink` view of this terminal,
   * writing through `feed()` -- i.e. interpreting every byte written to
   * it as terminal input (escape sequences included), not as literal
   * glyphs -- so this `vt100_terminal` can be handed directly to
   * `microfmt::format_to`/similar formatting pipelines
   * (`microfmt::sink` is itself just an alias for `reloco::sink`, same
   * `ctx`/`write_fn` shape, so no further adapting is needed). Compare
   * with `console()`'s own underlying `console_ref::as_sink()`, which
   * writes literally, with no escape-sequence interpretation.
   *
   * The returned `sink` stores a pointer back to *this* `vt100_terminal`
   * -- it must not outlive it.
   */
  [[nodiscard]] reloco::sink as_sink() noexcept RELOCO_LIFETIMEBOUND { return reloco::sink{this, &sink_write_thunk}; }

private:
  static void sink_write_thunk(void *ctx, reloco::string_view sv) noexcept {
    static_cast<vt100_terminal *>(ctx)->feed(sv);
  }

  enum class state { ground, escape, csi, osc, osc_esc };

  static constexpr std::size_t max_params = 16;
  static constexpr std::size_t osc_capacity = 255;
  static constexpr std::size_t no_bottom = static_cast<std::size_t>(-1);

  // ANSI 0-7 -> console_color, standard intensity and bright/bold.
  static constexpr reloco::array<console_color, 8> ansi_dark{
      console_color::black, console_color::red,     console_color::green, console_color::brown,
      console_color::blue,  console_color::magenta, console_color::cyan,  console_color::light_gray};
  static constexpr reloco::array<console_color, 8> ansi_bright{
      console_color::dark_gray,  console_color::light_red,     console_color::light_green, console_color::yellow,
      console_color::light_blue, console_color::light_magenta, console_color::light_cyan,  console_color::white};

  struct rgb3 {
    int r, g, b;
  };
  // RGB of each console_color, in enum order (CGA/VGA palette), used to map 256/true colours.
  static constexpr reloco::array<rgb3, 16> palette{{{0, 0, 0},       {0, 0, 170},     {0, 170, 0},    {0, 170, 170},
                                                    {170, 0, 0},     {170, 0, 170},   {170, 85, 0},   {170, 170, 170},
                                                    {85, 85, 85},    {85, 85, 255},   {85, 255, 85},  {85, 255, 255},
                                                    {255, 85, 85},   {255, 85, 255},  {255, 255, 85}, {255, 255, 255}}};

  // Current graphic rendition. `*_direct` is a console_color index (0..15) set by 256/true-colour SGR.
  struct attributes {
    int fg = -1;
    int bg = -1;
    int fg_direct = -1;
    int bg_direct = -1;
    bool bold = false;
    bool reverse = false;
    bool fg_bright = false;
    bool bg_bright = false;
  };

  // ------------------------------------------------------------------ ground / escape

  void feed_ground(char c) noexcept {
    switch (c) {
    case '\x1B':
      state_ = state::escape;
      break;
    case '\r':
      goto_xy(0, console_.cursor_y());
      break;
    case '\n':
    case '\v':
    case '\f':
      line_feed();
      break;
    case '\b':
      goto_xy(console_.cursor_x() > 0 ? console_.cursor_x() - 1 : 0, console_.cursor_y());
      break;
    case '\t':
      goto_xy((console_.cursor_x() / 8 + 1) * 8, console_.cursor_y());
      break;
    case '\a':
      if (cb_.bell) {
        cb_.bell(cb_.ctx);
      }
      break;
    default: {
      // NUL (terminfo padding), DEL, other C0 controls and non-ASCII bytes have no glyph: drop them.
      auto uc = static_cast<unsigned char>(c);
      if (uc >= 0x20 && uc < 0x7F) {
        print(c);
      }
      break;
    }
    }
  }

  void feed_escape(char c) noexcept {
    if (c == '[') {
      state_ = state::csi;
      param_count_ = 0;
      current_param_ = 0;
      current_param_started_ = false;
      csi_ignore_ = false;
      csi_private_ = false;
      csi_intermediate_ = 0;
      return;
    }
    if (c == ']') {
      state_ = state::osc;
      osc_len_ = 0;
      osc_code_ = 0;
      osc_have_code_ = false;
      return;
    }
    auto uc = static_cast<unsigned char>(c);
    if (uc == 0x1B) {
      return; // ESC ESC: restart
    }
    // Per ECMA-48, an escape sequence is `ESC`, zero or more "intermediate" bytes (0x20-0x2F), then one
    // "final" byte. Unsupported ones (e.g. charset select `ESC ( B`) are consumed so the final byte does not
    // leak onto the screen as text.
    if (uc < 0x20 || (uc >= 0x20 && uc <= 0x2F)) {
      return;
    }
    state_ = state::ground;
    switch (c) {
    case '7':
      save_cursor();
      break;
    case '8':
      restore_cursor();
      break;
    case 'D': // IND
      line_feed();
      break;
    case 'E': // NEL
      goto_xy(0, console_.cursor_y());
      line_feed();
      break;
    case 'M': // RI
      reverse_index();
      break;
    case 'c': // RIS
      full_reset();
      break;
    case '=':
      report_mode(vt100_mode::application_keypad, true);
      break;
    case '>':
      report_mode(vt100_mode::application_keypad, false);
      break;
    default:
      break;
    }
  }

  // ------------------------------------------------------------------ CSI

  void feed_csi(char c) noexcept {
    auto uc = static_cast<unsigned char>(c);
    if (c >= '0' && c <= '9') {
      current_param_ = current_param_ * 10 + static_cast<std::uint32_t>(c - '0');
      if (current_param_ > 65535) {
        current_param_ = 65535;
      }
      current_param_started_ = true;
      return;
    }
    if (c == ';') {
      push_param();
      return;
    }
    if (c == '?') {
      csi_private_ = true;
      return;
    }
    // ':' sub-parameters and the other private markers (`<`, `=`, `>`) are not implemented: consume the
    // whole sequence without dispatching, so e.g. `ESC [ > c` is not misread as a plain `c`.
    if (c == ':' || (c >= '<' && c <= '>')) {
      csi_ignore_ = true;
      return;
    }
    if (uc >= 0x20 && uc <= 0x2F) { // intermediate, as in `ESC [ 1 SP q`
      csi_intermediate_ = c;
      return;
    }
    if (uc == 0x1B) {
      state_ = state::escape;
      return;
    }
    if (uc < 0x20) {
      return; // C0 inside a CSI is ignored
    }
    if (uc > 0x7E) {
      state_ = state::ground;
      return;
    }
    push_param();
    if (!csi_ignore_) {
      dispatch_csi(c);
    }
    state_ = state::ground;
  }

  void push_param() noexcept {
    if (param_count_ < max_params) {
      params_[param_count_++] = current_param_started_ ? current_param_ : 0;
    }
    current_param_ = 0;
    current_param_started_ = false;
  }

  /** @brief Returns params_[i], or @p def if omitted or explicitly 0
   * (ECMA-48 treats an explicit 0 count the same as "default"). */
  [[nodiscard]] std::size_t param_or(std::size_t i, std::uint32_t def) const noexcept {
    if (i >= param_count_ || params_[i] == 0) {
      return def;
    }
    return params_[i];
  }

  void dispatch_csi(char cmd) noexcept {
    if (csi_private_) {
      if (cmd == 'h' || cmd == 'l') {
        for (std::size_t i = 0; i < param_count_; ++i) {
          set_private_mode(params_[i], cmd == 'h');
        }
      }
      return;
    }
    if (csi_intermediate_ != 0) {
      if (csi_intermediate_ == ' ' && cmd == 'q' && cb_.cursor_style) {
        cb_.cursor_style(cb_.ctx, params_[0]);
      }
      return;
    }

    const std::size_t x = console_.cursor_x();
    const std::size_t y = console_.cursor_y();
    const std::size_t cols = console_.columns();
    const std::size_t rows = console_.rows();
    const std::size_t n = param_or(0, 1);
    switch (cmd) {
    case 'A': // CUU
      goto_xy(x, n > y ? 0 : y - n);
      break;
    case 'B': // CUD
    case 'e': // VPR
      goto_xy(x, y + n);
      break;
    case 'C': // CUF
    case 'a': // HPR
      goto_xy(x + n, y);
      break;
    case 'D': // CUB
      goto_xy(n > x ? 0 : x - n, y);
      break;
    case 'E': // CNL
      goto_xy(0, y + n);
      break;
    case 'F': // CPL
      goto_xy(0, n > y ? 0 : y - n);
      break;
    case 'G': // CHA
    case '`': // HPA
      goto_xy(n - 1, y);
      break;
    case 'd': // VPA
      goto_xy(x, n - 1);
      break;
    case 'H': // CUP
    case 'f': // HVP
      goto_xy(param_or(1, 1) - 1, n - 1);
      break;
    case 'J': // ED
      erase_display(params_[0]);
      break;
    case 'K': // EL
      erase_line(params_[0]);
      break;
    case 'L': // IL
      if (y >= region_top() && y <= region_bottom()) {
        scroll_region_down(y, region_bottom(), n);
        goto_xy(0, y);
      }
      break;
    case 'M': // DL
      if (y >= region_top() && y <= region_bottom()) {
        scroll_region_up(y, region_bottom(), n);
        goto_xy(0, y);
      }
      break;
    case '@': { // ICH
      const std::size_t k = n < cols - x ? n : cols - x;
      for (std::size_t i = cols; i-- > x + k;) {
        copy_cell(i - k, y, i, y);
      }
      console_.fill_rect(x, y, k, 1, ' ', console_.foreground(), console_.background());
      break;
    }
    case 'P': { // DCH
      const std::size_t k = n < cols - x ? n : cols - x;
      for (std::size_t i = x; i + k < cols; ++i) {
        copy_cell(i + k, y, i, y);
      }
      console_.fill_rect(cols - k, y, k, 1, ' ', console_.foreground(), console_.background());
      break;
    }
    case 'X': { // ECH
      const std::size_t k = n < cols - x ? n : cols - x;
      console_.fill_rect(x, y, k, 1, ' ', console_.foreground(), console_.background());
      break;
    }
    case 'S': // SU
      scroll_region_up(region_top(), region_bottom(), n);
      break;
    case 'T': // SD
      scroll_region_down(region_top(), region_bottom(), n);
      break;
    case 'r': { // DECSTBM
      const std::size_t top = param_or(0, 1);
      const std::size_t bottom = param_or(1, static_cast<std::uint32_t>(rows));
      if (top < bottom && bottom <= rows) {
        top_ = top - 1;
        bottom_ = bottom - 1;
        goto_xy(0, 0);
      }
      break;
    }
    case 's':
      save_cursor();
      break;
    case 'u':
      restore_cursor();
      break;
    case 'm': // SGR
      apply_sgr_params();
      update_colors();
      break;
    case 'n': // DSR
      if (params_[0] == 5) {
        reply("\x1b[0n");
      } else if (params_[0] == 6) {
        char buf[24];
        std::size_t len = 0;
        buf[len++] = '\x1b';
        buf[len++] = '[';
        len = append_number(buf, len, y + 1);
        buf[len++] = ';';
        len = append_number(buf, len, x + 1);
        buf[len++] = 'R';
        reply(reloco::string_view(buf, len));
      }
      break;
    case 'c': // DA: identify as a VT100 with advanced video option
      if (params_[0] == 0) {
        reply("\x1b[?1;2c");
      }
      break;
    default:
      break; // Unsupported final byte: no-op.
    }
  }

  static std::size_t append_number(char *buf, std::size_t len, std::size_t v) noexcept {
    char tmp[8];
    std::size_t t = 0;
    do {
      tmp[t++] = static_cast<char>('0' + v % 10);
      v /= 10;
    } while (v != 0 && t < sizeof tmp);
    while (t != 0) {
      buf[len++] = tmp[--t];
    }
    return len;
  }

  void reply(reloco::string_view bytes) const noexcept {
    if (cb_.reply) {
      cb_.reply(cb_.ctx, bytes);
    }
  }

  void report_mode(vt100_mode m, bool on) const noexcept {
    if (cb_.mode) {
      cb_.mode(cb_.ctx, m, on);
    }
  }

  void set_private_mode(std::uint32_t mode, bool on) noexcept {
    if (mode == static_cast<std::uint32_t>(vt100_mode::autowrap)) {
      autowrap_ = on;
      if (!on) {
        wrap_pending_ = false;
      }
    } else if (mode == static_cast<std::uint32_t>(vt100_mode::cursor_visible)) {
      console_.set_cursor_visible(on);
    }
    report_mode(static_cast<vt100_mode>(mode), on);
  }

  // ------------------------------------------------------------------ OSC

  void feed_osc(char c) noexcept {
    auto uc = static_cast<unsigned char>(c);
    if (uc == 0x07) {
      finish_osc();
      state_ = state::ground;
      return;
    }
    if (uc == 0x1B) {
      state_ = state::osc_esc;
      return;
    }
    if (uc == 0x18 || uc == 0x1A) { // CAN / SUB abort the string
      state_ = state::ground;
      return;
    }
    if (uc < 0x20) {
      return;
    }
    if (!osc_have_code_) {
      if (c >= '0' && c <= '9') {
        osc_code_ = osc_code_ * 10 + static_cast<std::uint32_t>(c - '0');
        if (osc_code_ > 65535) {
          osc_code_ = 65535;
        }
      } else if (c == ';') {
        osc_have_code_ = true;
      } else {
        state_ = state::ground; // malformed OSC
      }
      return;
    }
    if (osc_len_ < osc_capacity) {
      osc_buf_[osc_len_++] = c;
    }
  }

  void finish_osc() noexcept {
    const reloco::string_view payload(osc_buf_.data(), osc_len_);
    if (osc_code_ == 0 || osc_code_ == 2) {
      if (cb_.title) {
        cb_.title(cb_.ctx, payload);
      }
    } else if (cb_.osc) {
      cb_.osc(cb_.ctx, osc_code_, payload);
    }
  }

  // ------------------------------------------------------------------ drawing

  void goto_xy(std::size_t x, std::size_t y) noexcept {
    wrap_pending_ = false;
    console_.set_cursor(x, y);
  }

  // Writes one glyph with xterm's deferred wrap: the cursor stays on the last column until the *next*
  // printable character, which first wraps to the next line.
  void print(char c) noexcept {
    const std::size_t cols = console_.columns();
    if (cols == 0 || console_.rows() == 0) {
      return;
    }
    if (wrap_pending_) {
      wrap_pending_ = false;
      if (autowrap_) {
        goto_xy(0, console_.cursor_y());
        line_feed();
      }
    }
    const std::size_t x = console_.cursor_x();
    const std::size_t y = console_.cursor_y();
    (void)console_.put_char(x, y, c, console_.foreground(), console_.background());
    if (x + 1 >= cols) {
      wrap_pending_ = autowrap_;
    } else {
      console_.set_cursor(x + 1, y);
    }
  }

  [[nodiscard]] std::size_t region_top() const noexcept { return top_; }
  [[nodiscard]] std::size_t region_bottom() const noexcept {
    const std::size_t rows = console_.rows();
    return (bottom_ >= rows) ? (rows == 0 ? 0 : rows - 1) : bottom_;
  }

  /** @brief Strict line feed: down one row, or scroll the region if on its bottom row. */
  void line_feed() noexcept {
    const std::size_t rows = console_.rows();
    if (rows == 0) {
      return;
    }
    const std::size_t y = console_.cursor_y();
    if (y == region_bottom()) {
      scroll_region_up(region_top(), region_bottom(), 1);
      wrap_pending_ = false;
    } else if (y + 1 < rows) {
      goto_xy(console_.cursor_x(), y + 1);
    }
  }

  void reverse_index() noexcept {
    const std::size_t y = console_.cursor_y();
    if (y == region_top()) {
      scroll_region_down(region_top(), region_bottom(), 1);
      wrap_pending_ = false;
    } else if (y > 0) {
      goto_xy(console_.cursor_x(), y - 1);
    }
  }

  void copy_cell(std::size_t sx, std::size_t sy, std::size_t dx, std::size_t dy) noexcept {
    auto cell = console_.get_char(sx, sy);
    if (cell) {
      (void)console_.put_char(dx, dy, cell->ch, cell->fg, cell->bg);
    }
  }

  void copy_row(std::size_t src, std::size_t dst) noexcept {
    for (std::size_t x = 0; x < console_.columns(); ++x) {
      copy_cell(x, src, x, dst);
    }
  }

  // Scrolls rows [top, bottom] up by n, blanking the vacated bottom rows.
  void scroll_region_up(std::size_t top, std::size_t bottom, std::size_t n) noexcept {
    const std::size_t rows = console_.rows();
    if (rows == 0 || top > bottom) {
      return;
    }
    n = n < bottom - top + 1 ? n : bottom - top + 1;
    if (top == 0 && bottom == rows - 1) {
      console_.scroll_up(n, console_.foreground(), console_.background());
      return;
    }
    for (std::size_t y = top; y + n <= bottom; ++y) {
      copy_row(y + n, y);
    }
    console_.fill_rect(0, bottom + 1 - n, console_.columns(), n, ' ', console_.foreground(), console_.background());
  }

  // Scrolls rows [top, bottom] down by n, blanking the vacated top rows.
  void scroll_region_down(std::size_t top, std::size_t bottom, std::size_t n) noexcept {
    if (console_.rows() == 0 || top > bottom) {
      return;
    }
    n = n < bottom - top + 1 ? n : bottom - top + 1;
    for (std::size_t y = bottom + 1; y-- > top + n;) {
      copy_row(y - n, y);
    }
    console_.fill_rect(0, top, console_.columns(), n, ' ', console_.foreground(), console_.background());
  }

  void erase_display(std::uint32_t mode) noexcept {
    console_color fg = console_.foreground();
    console_color bg = console_.background();
    std::size_t cols = console_.columns();
    std::size_t rows = console_.rows();
    std::size_t y = console_.cursor_y();
    std::size_t x = console_.cursor_x();
    switch (mode) {
    case 0: // cursor to end of screen
      console_.fill_rect(x, y, cols - x, 1, ' ', fg, bg);
      if (y + 1 < rows) {
        console_.fill_rect(0, y + 1, cols, rows - y - 1, ' ', fg, bg);
      }
      break;
    case 1: // start of screen to cursor
      console_.fill_rect(0, y, x + 1, 1, ' ', fg, bg);
      if (y > 0) {
        console_.fill_rect(0, 0, cols, y, ' ', fg, bg);
      }
      break;
    case 2: // entire screen
    default: // 3 (scrollback) is treated like 2: there is no scrollback
      console_.clear(fg, bg);
      break;
    }
  }

  void erase_line(std::uint32_t mode) noexcept {
    console_color fg = console_.foreground();
    console_color bg = console_.background();
    std::size_t cols = console_.columns();
    std::size_t y = console_.cursor_y();
    std::size_t x = console_.cursor_x();
    switch (mode) {
    case 0: // cursor to end of line
      console_.fill_rect(x, y, cols - x, 1, ' ', fg, bg);
      break;
    case 1: // start of line to cursor
      console_.fill_rect(0, y, x + 1, 1, ' ', fg, bg);
      break;
    case 2: // entire line
    default:
      console_.fill_rect(0, y, cols, 1, ' ', fg, bg);
      break;
    }
  }

  // ------------------------------------------------------------------ cursor save / reset

  void save_cursor() noexcept {
    saved_x_ = console_.cursor_x();
    saved_y_ = console_.cursor_y();
    saved_attrs_ = attrs_;
  }

  void restore_cursor() noexcept {
    attrs_ = saved_attrs_;
    update_colors();
    goto_xy(saved_x_, saved_y_);
  }

  void full_reset() noexcept {
    attrs_ = attributes{};
    saved_attrs_ = attributes{};
    saved_x_ = saved_y_ = 0;
    top_ = 0;
    bottom_ = no_bottom;
    autowrap_ = true;
    update_colors();
    console_.set_cursor_visible(true);
    console_.clear(console_.foreground(), console_.background());
    goto_xy(0, 0);
    if (cb_.reset) {
      cb_.reset(cb_.ctx);
    }
  }

  // ------------------------------------------------------------------ SGR

  static constexpr int nearest_color(int r, int g, int b) noexcept {
    int best = 0;
    int best_d = 0x7fffffff;
    for (std::size_t i = 0; i < 16; ++i) {
      const int dr = r - palette[i].r;
      const int dg = g - palette[i].g;
      const int db = b - palette[i].b;
      const int d = dr * dr + dg * dg + db * db;
      if (d < best_d) {
        best_d = d;
        best = static_cast<int>(i);
      }
    }
    return best;
  }

  static constexpr int color_from_256(std::uint32_t n) noexcept {
    if (n < 8) {
      return static_cast<int>(ansi_dark[n]);
    }
    if (n < 16) {
      return static_cast<int>(ansi_bright[n - 8]);
    }
    if (n < 232) {
      const int v = static_cast<int>(n) - 16;
      const auto level = [](int c) { return c == 0 ? 0 : 55 + 40 * c; };
      return nearest_color(level(v / 36), level((v / 6) % 6), level(v % 6));
    }
    if (n < 256) {
      const int gray = 8 + 10 * (static_cast<int>(n) - 232);
      return nearest_color(gray, gray, gray);
    }
    return -1;
  }

  void apply_sgr_params() noexcept {
    for (std::size_t i = 0; i < param_count_; ++i) {
      const std::uint32_t code = params_[i];
      if ((code == 38 || code == 48) && i + 1 < param_count_) {
        int color = -1;
        if (params_[i + 1] == 5 && i + 2 < param_count_) {
          color = color_from_256(params_[i + 2]);
          i += 2;
        } else if (params_[i + 1] == 2 && i + 4 < param_count_) {
          color = nearest_color(static_cast<int>(params_[i + 2] & 0xFF), static_cast<int>(params_[i + 3] & 0xFF),
                                static_cast<int>(params_[i + 4] & 0xFF));
          i += 4;
        } else {
          break; // malformed extended colour: drop the rest
        }
        if (color >= 0) {
          (code == 38 ? attrs_.fg_direct : attrs_.bg_direct) = color;
        }
        continue;
      }
      apply_sgr(code);
    }
  }

  void apply_sgr(std::uint32_t code) noexcept {
    if (code == 0) {
      attrs_ = attributes{};
    } else if (code == 1) {
      attrs_.bold = true;
    } else if (code == 22) {
      attrs_.bold = false;
    } else if (code == 7) {
      attrs_.reverse = true;
    } else if (code == 27) {
      attrs_.reverse = false;
    } else if (code >= 30 && code <= 37) {
      attrs_.fg = static_cast<int>(code - 30);
      attrs_.fg_direct = -1;
      attrs_.fg_bright = false;
    } else if (code == 39) {
      attrs_.fg = -1;
      attrs_.fg_direct = -1;
      attrs_.fg_bright = false;
    } else if (code >= 90 && code <= 97) {
      attrs_.fg = static_cast<int>(code - 90);
      attrs_.fg_direct = -1;
      attrs_.fg_bright = true;
    } else if (code >= 40 && code <= 47) {
      attrs_.bg = static_cast<int>(code - 40);
      attrs_.bg_direct = -1;
      attrs_.bg_bright = false;
    } else if (code == 49) {
      attrs_.bg = -1;
      attrs_.bg_direct = -1;
      attrs_.bg_bright = false;
    } else if (code >= 100 && code <= 107) {
      attrs_.bg = static_cast<int>(code - 100);
      attrs_.bg_direct = -1;
      attrs_.bg_bright = true;
    }
    // Any other SGR code (underline, italic, ...) is unsupported and silently ignored.
  }

  void update_colors() noexcept {
    console_color fg = console_color::light_gray;
    if (attrs_.fg_direct >= 0) {
      fg = static_cast<console_color>(attrs_.fg_direct);
    } else if (attrs_.fg >= 0) {
      const auto i = static_cast<std::size_t>(attrs_.fg);
      fg = (attrs_.bold || attrs_.fg_bright) ? ansi_bright[i] : ansi_dark[i];
    }
    console_color bg = console_color::black;
    if (attrs_.bg_direct >= 0) {
      bg = static_cast<console_color>(attrs_.bg_direct);
    } else if (attrs_.bg >= 0) {
      const auto i = static_cast<std::size_t>(attrs_.bg);
      bg = attrs_.bg_bright ? ansi_bright[i] : ansi_dark[i];
    }
    if (attrs_.reverse) {
      console_.set_colors(bg, fg);
    } else {
      console_.set_colors(fg, bg);
    }
  }

  console_ref console_{};
  vt100_callbacks cb_{};
  state state_ = state::ground;

  reloco::array<std::uint32_t, max_params> params_{};
  std::size_t param_count_ = 0;
  std::uint32_t current_param_ = 0;
  bool current_param_started_ = false;
  bool csi_ignore_ = false;
  bool csi_private_ = false;
  char csi_intermediate_ = 0;

  reloco::array<char, osc_capacity> osc_buf_{};
  std::size_t osc_len_ = 0;
  std::uint32_t osc_code_ = 0;
  bool osc_have_code_ = false;

  attributes attrs_{};
  attributes saved_attrs_{};
  std::size_t saved_x_ = 0;
  std::size_t saved_y_ = 0;
  std::size_t top_ = 0;
  std::size_t bottom_ = no_bottom;
  bool autowrap_ = true;
  bool wrap_pending_ = false;
};

} // namespace hw
} // namespace structo
