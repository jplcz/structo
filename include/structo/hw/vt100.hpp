// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file vt100.hpp
 * @brief `structo::hw::vt100_terminal`: a small VT100/ANSI
 * escape-sequence interpreter built on top of @ref console_ref, turning
 * a byte stream into console operations -- usable with any bound
 * backend (`vga_text_console`, `framebuffer_console<PixelFormat>`, or a
 * test fake) since it only ever talks to the `console_ref` abstraction.
 *
 * ## Scope
 *
 * This is a deliberately focused *subset* of VT100/ANSI/ECMA-48, not a
 * full xterm clone:
 * - Cursor movement: `CUU`/`CUD`/`CUF`/`CUB` (`ESC [ n A/B/C/D`) and
 *   absolute positioning `CUP`/`HVP` (`ESC [ row ; col H` or `f`).
 * - Erasing: `ED` (`ESC [ n J`, erase display) and `EL` (`ESC [ n K`,
 *   erase line), `n` in `{0 = to-end, 1 = from-start, 2 = all}`.
 * - `SGR` (`ESC [ n (; n)* m`): the 16-color set via `30-37`/`40-47`
 *   (standard-intensity) and `90-97`/`100-107` (bright/high-intensity),
 *   `1` (bold, brightens the *foreground* of a subsequently-set
 *   standard-intensity color -- matching the common "bold = bright"
 *   terminal convention), `22` (normal intensity), `39`/`49` (default
 *   fg/bg), and `0` (full reset).
 * - Raw control characters outside of any escape sequence: `\r` (CR),
 *   `\n` (LF -- **strict VT100 semantics: moves the cursor down one
 *   row, scrolling if already on the last row, and does not also reset
 *   the column**, unlike `console_ref::put()`'s friendlier convenience
 *   interpretation of `'\n'` as CR+LF; pair it with an explicit `\r` for
 *   a conventional newline, exactly as real line-oriented input does),
 *   `\b` (BS, one column left), `\t` (TAB, next multiple-of-8 column).
 * - Any other/unrecognized escape sequence is consumed and silently
 *   ignored (parsed far enough to find its final byte, then dropped)
 *   rather than leaking raw escape bytes onto the screen.
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

/**
 * @brief A VT100/ANSI escape-sequence interpreter that owns a
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
   * it as VT100/ANSI input (escape sequences included), not as literal
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

  enum class state { ground, escape, csi };

  static constexpr std::size_t max_params = 8;

  // ANSI 0-7 -> console_color, standard intensity and bright/bold.
  static constexpr reloco::array<console_color, 8> ansi_dark{
      console_color::black, console_color::red,     console_color::green, console_color::brown,
      console_color::blue,  console_color::magenta, console_color::cyan,  console_color::light_gray};
  static constexpr reloco::array<console_color, 8> ansi_bright{
      console_color::dark_gray,  console_color::light_red,     console_color::light_green, console_color::yellow,
      console_color::light_blue, console_color::light_magenta, console_color::light_cyan,  console_color::white};

  void feed_ground(char c) noexcept {
    switch (c) {
    case '\x1B':
      state_ = state::escape;
      break;
    case '\r':
      console_.set_cursor(0, console_.cursor_y());
      break;
    case '\n':
      line_feed();
      break;
    case '\b':
      if (console_.cursor_x() > 0) {
        console_.set_cursor(console_.cursor_x() - 1, console_.cursor_y());
      }
      break;
    case '\t': {
      std::size_t next = (console_.cursor_x() / 8 + 1) * 8;
      console_.set_cursor(next, console_.cursor_y());
      break;
    }
    default:
      console_.write_char(c);
      break;
    }
  }

  void feed_escape(char c) noexcept {
    if (c == '[') {
      state_ = state::csi;
      param_count_ = 0;
      current_param_ = 0;
      current_param_started_ = false;
      return;
    }
    // Per ECMA-48, every escape sequence other than CSI is `ESC`
    // followed by zero or more "intermediate" bytes (0x20-0x2F) and
    // then exactly one "final" byte (0x30-0x7E). This isn't a sequence
    // we support, but it must still be fully consumed -- stopping after
    // only the first byte would leak a sequence's own final byte (e.g.
    // the `B` of the 3-byte charset-select `ESC ( B`) onto the screen
    // as if it were ordinary text.
    auto uc = static_cast<unsigned char>(c);
    if (uc < 0x20 || uc > 0x2F) {
      state_ = state::ground;
    }
  }

  void feed_csi(char c) noexcept {
    if (c >= '0' && c <= '9') {
      current_param_ = current_param_ * 10 + static_cast<std::uint32_t>(c - '0');
      current_param_started_ = true;
      return;
    }
    if (c == ';') {
      push_param();
      return;
    }
    // Any other byte terminates the CSI sequence (treated as the final
    // byte, even if we don't recognize/support it).
    push_param();
    dispatch_csi(c);
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
  [[nodiscard]] std::uint32_t param_or(std::size_t i, std::uint32_t def) const noexcept {
    if (i >= param_count_ || params_[i] == 0) {
      return def;
    }
    return params_[i];
  }

  void dispatch_csi(char cmd) noexcept {
    switch (cmd) {
    case 'A': { // CUU
      std::uint32_t n = param_or(0, 1);
      std::size_t y = console_.cursor_y();
      console_.set_cursor(console_.cursor_x(), n > y ? 0 : y - n);
      break;
    }
    case 'B': { // CUD
      std::uint32_t n = param_or(0, 1);
      console_.set_cursor(console_.cursor_x(), console_.cursor_y() + n);
      break;
    }
    case 'C': { // CUF
      std::uint32_t n = param_or(0, 1);
      console_.set_cursor(console_.cursor_x() + n, console_.cursor_y());
      break;
    }
    case 'D': { // CUB
      std::uint32_t n = param_or(0, 1);
      std::size_t x = console_.cursor_x();
      console_.set_cursor(n > x ? 0 : x - n, console_.cursor_y());
      break;
    }
    case 'H':   // CUP
    case 'f': { // HVP
      std::uint32_t row = param_or(0, 1);
      std::uint32_t col = param_or(1, 1);
      console_.set_cursor(col - 1, row - 1);
      break;
    }
    case 'J': // ED
      erase_display(param_or(0, 0));
      break;
    case 'K': // EL
      erase_line(param_or(0, 0));
      break;
    case 'm': // SGR
      if (param_count_ == 0) {
        apply_sgr(0);
      } else {
        for (std::size_t i = 0; i < param_count_; ++i) {
          apply_sgr(params_[i]);
        }
      }
      update_colors();
      break;
    default:
      break; // Unsupported final byte: no-op.
    }
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
    default:
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

  void apply_sgr(std::uint32_t code) noexcept {
    if (code == 0) {
      bold_ = false;
      fg_index_ = -1;
      bg_index_ = -1;
      fg_forced_bright_ = false;
      bg_forced_bright_ = false;
    } else if (code == 1) {
      bold_ = true;
    } else if (code == 22) {
      bold_ = false;
    } else if (code >= 30 && code <= 37) {
      fg_index_ = static_cast<int>(code - 30);
      fg_forced_bright_ = false;
    } else if (code == 39) {
      fg_index_ = -1;
      fg_forced_bright_ = false;
    } else if (code >= 90 && code <= 97) {
      fg_index_ = static_cast<int>(code - 90);
      fg_forced_bright_ = true;
    } else if (code >= 40 && code <= 47) {
      bg_index_ = static_cast<int>(code - 40);
      bg_forced_bright_ = false;
    } else if (code == 49) {
      bg_index_ = -1;
      bg_forced_bright_ = false;
    } else if (code >= 100 && code <= 107) {
      bg_index_ = static_cast<int>(code - 100);
      bg_forced_bright_ = true;
    }
    // Any other SGR code is unsupported and silently ignored.
  }

  void update_colors() noexcept {
    console_color fg = fg_index_ < 0 ? console_color::light_gray
                                     : ((bold_ || fg_forced_bright_) ? ansi_bright[static_cast<std::size_t>(fg_index_)]
                                                                     : ansi_dark[static_cast<std::size_t>(fg_index_)]);
    console_color bg = bg_index_ < 0 ? console_color::black
                                     : (bg_forced_bright_ ? ansi_bright[static_cast<std::size_t>(bg_index_)]
                                                          : ansi_dark[static_cast<std::size_t>(bg_index_)]);
    console_.set_colors(fg, bg);
  }

  /** @brief Strict VT100 line feed: moves the cursor down one row
   * (scrolling if already on the last row), without touching the
   * column. */
  void line_feed() noexcept {
    std::size_t rows = console_.rows();
    if (rows == 0) {
      return;
    }
    if (console_.cursor_y() + 1 >= rows) {
      console_.scroll_up(1, console_.foreground(), console_.background());
    } else {
      console_.set_cursor(console_.cursor_x(), console_.cursor_y() + 1);
    }
  }

  console_ref console_{};
  state state_ = state::ground;

  reloco::array<std::uint32_t, max_params> params_{};
  std::size_t param_count_ = 0;
  std::uint32_t current_param_ = 0;
  bool current_param_started_ = false;

  int fg_index_ = -1;
  int bg_index_ = -1;
  bool bold_ = false;
  bool fg_forced_bright_ = false;
  bool bg_forced_bright_ = false;
};

} // namespace hw
} // namespace structo
