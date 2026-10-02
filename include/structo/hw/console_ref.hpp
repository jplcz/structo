// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file console_ref.hpp
 * @brief `structo::hw::console_ref`: a type-erased, non-owning handle
 * over a character-cell, 16-color text console -- the text-mode
 * counterpart of `uart_ref`/`framebuffer`, plus the
 * `console_traits<Backend>` customization point a concrete backend
 * specializes to be bindable through it.
 *
 * ## Why cursor state lives in `console_ref`, not the backend
 *
 * Real console hardware splits "where is the cursor" across two very
 * different concerns: the *logical* position text gets written at next
 * (needed for every higher-level operation: line wrap, scrolling,
 * backspace, an escape-sequence interpreter's `CUP`/`CUU`/...), and the
 * *physical* side effect of showing it (a VGA adapter's blinking CRTC
 * hardware cursor register, a framebuffer renderer drawing a cursor
 * glyph, or nothing at all for a backend with no visible cursor).
 *
 * `console_ref` owns the logical cursor position (and the "current
 * colors" new text is written with) itself, directly as members of the
 * handle -- unlike `uart_ref`/`timer_ref`, which are pure two-pointer
 * (ctx + vtable) forwarders with no state of their own. Every operation
 * that moves the cursor (`write_char`, `put`, `set_cursor`, ...) updates
 * that state locally, then -- only if the bound backend opted in by
 * implementing the optional `console_traits::move_cursor` -- forwards
 * the *new* position to the backend as a side effect, e.g. so a VGA
 * backend can poke CRTC registers 0x0E/0x0F to move the hardware
 * cursor. A backend with no hardware cursor (a software-rendered
 * framebuffer console, a test fake) simply omits `move_cursor` and
 * nothing is forwarded; `console_ref` still tracks the logical position
 * perfectly well on its own.
 *
 * This is also exactly what lets a single `vt100_terminal` (built on top
 * of `console_ref`, see `vt100.hpp`) implement cursor-movement escape
 * sequences once, generically, regardless of which concrete backend --
 * `vga_text_console` or `framebuffer_console<PixelFormat>` -- it was
 * bound to.
 *
 * ## Customizing: `console_traits<Backend>`
 *
 * A specialization must supply three mandatory functions:
 * @code
 * template <> struct structo::hw::console_traits<my_backend> {
 *   static void put_cell(my_backend &, std::size_t x, std::size_t y,
 *                        char ch, structo::hw::console_color fg, structo::hw::console_color bg) noexcept;
 *   static std::size_t columns(const my_backend &) noexcept;
 *   static std::size_t rows(const my_backend &) noexcept;
 * };
 * @endcode
 * Optionally, also `get_cell` (readback; without it,
 * `console_ref::get_char` fails with `error::unsupported_operation`),
 * `move_cursor(Backend &, std::size_t x, std::size_t y)` (hardware
 * cursor forwarding), and `set_cursor_visible(Backend &, bool)`
 * (hide/show the hardware cursor). Detected via SFINAE, the same
 * optional-member idiom `uart_traits::current_config` uses.
 */

#include <reloco/detail/assert.hpp>
#include <reloco/detail/compat.hpp>
#include <reloco/error.hpp>
#include <reloco/fmt.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/string_view.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace structo {

using namespace reloco;

namespace hw {

// ============================================================================
// Colors
// ============================================================================

/** @brief The classic 16-color text-console palette (CGA/EGA/VGA text
 * mode attribute colors), used for both foreground and background. */
enum class console_color : std::uint8_t {
  black = 0,
  blue,
  green,
  cyan,
  red,
  magenta,
  brown,
  light_gray,
  dark_gray,
  light_blue,
  light_green,
  light_cyan,
  light_red,
  light_magenta,
  yellow,
  white,
};

/** @brief One character cell: a glyph plus its foreground/background color. */
struct console_cell {
  char ch = ' ';
  console_color fg = console_color::light_gray;
  console_color bg = console_color::black;

  [[nodiscard]] friend constexpr bool operator==(const console_cell &a, const console_cell &b) noexcept {
    return a.ch == b.ch && a.fg == b.fg && a.bg == b.bg;
  }
  [[nodiscard]] friend constexpr bool operator!=(const console_cell &a, const console_cell &b) noexcept {
    return !(a == b);
  }
};

// ============================================================================
// Customization Point
// ============================================================================

/**
 * @brief Opt-in customization point describing how to read/write cells
 * of a concrete text-console backend, through @ref console_ref.
 * Intentionally left undefined for any `Backend` that hasn't been
 * adapted. See the @file-level docs above for the required/optional
 * member list.
 */
template <typename Backend> struct console_traits;

namespace detail {

template <typename Backend, typename = void> struct has_console_traits : std::false_type {};

template <typename Backend>
struct has_console_traits<
    Backend, std::void_t<decltype(console_traits<Backend>::put_cell), decltype(console_traits<Backend>::columns),
                         decltype(console_traits<Backend>::rows)>> : std::true_type {};

template <typename Traits, typename = void> struct console_has_get_cell : std::false_type {};
template <typename Traits>
struct console_has_get_cell<Traits, std::void_t<decltype(Traits::get_cell)>> : std::true_type {};

template <typename Traits, typename = void> struct console_has_move_cursor : std::false_type {};
template <typename Traits>
struct console_has_move_cursor<Traits, std::void_t<decltype(Traits::move_cursor)>> : std::true_type {};

template <typename Traits, typename = void> struct console_has_set_cursor_visible : std::false_type {};
template <typename Traits>
struct console_has_set_cursor_visible<Traits, std::void_t<decltype(Traits::set_cursor_visible)>> : std::true_type {};

} // namespace detail

// ============================================================================
// Type-Erased Console Handle
// ============================================================================

/**
 * @brief Type-erased, non-owning handle over a character-cell, 16-color
 * text console, for whatever concrete backend it is bound to -- plus
 * the logical cursor position and "current colors" state described in
 * the @file-level docs above, owned directly by this handle.
 *
 * Default-constructed (or copied from a default-constructed) refs are
 * *unbound*: every operation that reaches the backend fails with
 * `error::unsupported_operation` rather than trapping, mirroring
 * `uart_ref`'s null-safety convention. `columns()`/`rows()` report `0`
 * when unbound.
 */
class RELOCO_POINTER console_ref {
public:
  /** @brief Fixed, per-bound-backend-type dispatch table. */
  struct vtable {
    void (*put_cell)(void *ctx, std::size_t x, std::size_t y, char ch, console_color fg, console_color bg) noexcept;
    result<console_cell> (*get_cell)(void *ctx, std::size_t x, std::size_t y) noexcept;
    std::size_t (*columns)(void *ctx) noexcept;
    std::size_t (*rows)(void *ctx) noexcept;
    void (*move_cursor)(void *ctx, std::size_t x, std::size_t y) noexcept;
    void (*set_cursor_visible)(void *ctx, bool visible) noexcept;
  };

  /** @brief Constructs an unbound ref. */
  constexpr console_ref() noexcept = default;

  /**
   * @brief Binds this ref to an existing, adapted backend. The cursor
   * is reset to `(0, 0)` and colors to `light_gray` on `black`.
   * @tparam Backend Concrete backend type, deduced. Must have a
   * @ref console_traits specialization.
   * @param b Backend to bind. Must outlive this handle and every copy
   * of it. Marked `explicit`: binding a backend is always a deliberate
   * step, never an implicit conversion.
   */
  template <typename Backend, std::enable_if_t<detail::has_console_traits<Backend>::value, int> = 0>
  constexpr explicit console_ref(Backend &b RELOCO_LIFETIMEBOUND RELOCO_LIFETIME_CAPTURE_BY_THIS) noexcept
      : ctx_(std::addressof(b)), vtbl_(&s_vtbl<Backend>) {}

  /** @brief Rejects rvalue/temporary backend bindings. */
  template <typename Backend, std::enable_if_t<!std::is_lvalue_reference_v<Backend>, int> = 0>
  console_ref(Backend &&) = delete;

  /** @brief Whether this ref is bound to a backend. */
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return vtbl_ != nullptr; }

  /** @brief Number of character columns, `0` if unbound. */
  [[nodiscard]] std::size_t columns() const noexcept { return vtbl_ ? vtbl_->columns(ctx_) : 0; }

  /** @brief Number of character rows, `0` if unbound. */
  [[nodiscard]] std::size_t rows() const noexcept { return vtbl_ ? vtbl_->rows(ctx_) : 0; }

  // --------------------------------------------------------------------
  // Current colors (used by write_char/put/write; does not touch any
  // already-written cell).
  // --------------------------------------------------------------------

  [[nodiscard]] constexpr console_color foreground() const noexcept { return fg_; }
  [[nodiscard]] constexpr console_color background() const noexcept { return bg_; }

  constexpr void set_colors(console_color fg, console_color bg) noexcept {
    fg_ = fg;
    bg_ = bg;
  }

  // --------------------------------------------------------------------
  // Cursor (logical position owned here; forwarded to the backend's
  // optional hardware cursor on every change).
  // --------------------------------------------------------------------

  [[nodiscard]] constexpr std::size_t cursor_x() const noexcept { return cursor_x_; }
  [[nodiscard]] constexpr std::size_t cursor_y() const noexcept { return cursor_y_; }

  /** @brief Moves the logical cursor to `(x, y)`, clamped to
   * `[0, columns()) x [0, rows())` (a no-op coordinate if the console
   * is unbound or has zero columns/rows). Forwards the new position to
   * the backend's optional hardware cursor, if implemented. */
  void set_cursor(std::size_t x, std::size_t y) noexcept {
    std::size_t cols = columns();
    std::size_t r = rows();
    cursor_x_ = (cols == 0) ? 0 : (x >= cols ? cols - 1 : x);
    cursor_y_ = (r == 0) ? 0 : (y >= r ? r - 1 : y);
    if (vtbl_) {
      vtbl_->move_cursor(ctx_, cursor_x_, cursor_y_);
    }
  }

  /** @brief Shows/hides the hardware cursor, if the backend implements
   * the optional `set_cursor_visible`; otherwise a no-op. */
  void set_cursor_visible(bool visible) noexcept {
    if (vtbl_) {
      vtbl_->set_cursor_visible(ctx_, visible);
    }
  }

  // --------------------------------------------------------------------
  // Direct cell access (explicit coordinates, bounds-checked, never
  // touches the cursor).
  // --------------------------------------------------------------------

  /** @brief Writes one cell at `(x, y)`.
   * @return `error::out_of_bounds` if `(x, y)` falls outside
   * `[0, columns()) x [0, rows())`; `error::unsupported_operation` if unbound. */
  result<void> put_char(std::size_t x, std::size_t y, char ch, console_color fg, console_color bg) noexcept {
    if (!vtbl_) {
      return unexpected(error::unsupported_operation);
    }
    if (x >= columns() || y >= rows()) {
      return unexpected(error::out_of_bounds);
    }
    vtbl_->put_cell(ctx_, x, y, ch, fg, bg);
    return {};
  }

  /** @brief Reads the cell at `(x, y)`.
   * @return `error::out_of_bounds` if `(x, y)` falls outside bounds;
   * `error::unsupported_operation` if unbound, or if the backend does
   * not implement the optional `console_traits::get_cell`. */
  [[nodiscard]] result<console_cell> get_char(std::size_t x, std::size_t y) const noexcept {
    if (!vtbl_) {
      return unexpected(error::unsupported_operation);
    }
    if (x >= columns() || y >= rows()) {
      return unexpected(error::out_of_bounds);
    }
    return vtbl_->get_cell(ctx_, x, y);
  }

  // --------------------------------------------------------------------
  // Clipped, non-failing multi-cell helpers (never touch the cursor).
  // --------------------------------------------------------------------

  /** @brief Fills the `w x h` rectangle at `(x, y)` with `ch`/`fg`/`bg`,
   * clipped to the console's bounds. */
  void fill_rect(std::size_t x, std::size_t y, std::size_t w, std::size_t h, char ch, console_color fg,
                 console_color bg) noexcept {
    if (!vtbl_) {
      return;
    }
    std::size_t cols = columns();
    std::size_t r = rows();
    std::size_t x1 = (x + w > cols) ? cols : x + w;
    std::size_t y1 = (y + h > r) ? r : y + h;
    if (x >= cols || y >= r || x >= x1 || y >= y1) {
      return;
    }
    for (std::size_t py = y; py < y1; ++py) {
      for (std::size_t px = x; px < x1; ++px) {
        vtbl_->put_cell(ctx_, px, py, ch, fg, bg);
      }
    }
  }

  /** @brief Fills every cell with a space on `bg` (`fg` only matters for
   * any later `write_char`-style text at that cell). Does not move the cursor. */
  void clear(console_color fg, console_color bg) noexcept { fill_rect(0, 0, columns(), rows(), ' ', fg, bg); }

  /**
   * @brief Scrolls the whole console up by `lines` rows: row `i`'s
   * content becomes row `i - lines`'s, and the `lines` newly-exposed
   * rows at the bottom are blanked with `fg`/`bg`. A no-op if unbound,
   * `lines == 0`, or `lines >= rows()` (the latter is equivalent to `clear`).
   * Moving existing content requires the backend's optional
   * `console_traits::get_cell`; without it, `scroll_up` can still blank
   * the bottom `lines` rows but cannot read back and shift what was
   * above them. Both `vga_text_console` and `framebuffer_console`
   * implement `get_cell`.
   */
  void scroll_up(std::size_t lines, console_color fg, console_color bg) noexcept {
    if (!vtbl_ || lines == 0) {
      return;
    }
    std::size_t cols = columns();
    std::size_t r = rows();
    if (lines >= r) {
      clear(fg, bg);
      return;
    }
    for (std::size_t y = 0; y + lines < r; ++y) {
      for (std::size_t x = 0; x < cols; ++x) {
        auto cell = vtbl_->get_cell(ctx_, x, y + lines);
        if (cell) {
          vtbl_->put_cell(ctx_, x, y, cell->ch, cell->fg, cell->bg);
        }
      }
    }
    fill_rect(0, r - lines, cols, lines, ' ', fg, bg);
  }

  // --------------------------------------------------------------------
  // Cursor-driven writing.
  // --------------------------------------------------------------------

  /**
   * @brief Core "insert one glyph" primitive: writes `ch` at the
   * current cursor position using the current colors, then advances
   * the cursor one column, wrapping to the next row (and scrolling if
   * already on the last row) when it runs past the last column. Unlike
   * `put`, this never special-cases `'\n'`/`'\r'`/`'\t'`/`'\b'` -- every
   * `char` is written as a literal glyph. This is the primitive
   * `vt100_terminal` (`vt100.hpp`) builds printable-character insertion
   * on top of.
   */
  void write_char(char ch) noexcept {
    if (!vtbl_) {
      return;
    }
    vtbl_->put_cell(ctx_, cursor_x_, cursor_y_, ch, fg_, bg_);
    advance_cursor();
  }

  /**
   * @brief Friendly text convenience: `'\n'` performs a carriage-return
   * + line-feed (column 0, one row down, scrolling if needed), `'\r'`
   * returns to column 0, `'\t'` advances to the next multiple-of-8
   * column (clamped to the last column), `'\b'` moves one column left
   * (clamped to column 0), and every other character is written via
   * `write_char`.
   */
  void put(char ch) noexcept {
    if (!vtbl_) {
      return;
    }
    switch (ch) {
    case '\n':
      set_cursor(0, cursor_y_);
      newline();
      break;
    case '\r':
      set_cursor(0, cursor_y_);
      break;
    case '\t': {
      std::size_t next = (cursor_x_ / 8 + 1) * 8;
      set_cursor(next, cursor_y_);
      break;
    }
    case '\b':
      if (cursor_x_ > 0) {
        set_cursor(cursor_x_ - 1, cursor_y_);
      }
      break;
    default:
      write_char(ch);
      break;
    }
  }

  /** @brief Writes every character of `text` via `put`. */
  void write(string_view text) noexcept {
    for (char c : text) {
      put(c);
    }
  }

  /**
   * @brief Returns a type-erased `reloco::sink` view of this console,
   * writing through `write()` (i.e. with `'\n'`'s friendly CR+LF
   * convenience handling) -- so this `console_ref` can be handed
   * directly to `microfmt::format_to`/similar formatting pipelines
   * (`microfmt::sink` is itself just an alias for `reloco::sink`, same
   * `ctx`/`write_fn` shape, so no further adapting is needed).
   *
   * The returned `sink` stores a pointer back to *this* `console_ref`
   * (not to the bound backend) -- it must not outlive it.
   */
  [[nodiscard]] constexpr sink as_sink() noexcept RELOCO_LIFETIMEBOUND { return sink{this, &sink_write_thunk}; }

private:
  static void sink_write_thunk(void *ctx, string_view sv) noexcept { static_cast<console_ref *>(ctx)->write(sv); }

  void advance_cursor() noexcept {
    std::size_t cols = columns();
    std::size_t r = rows();
    if (cols == 0 || r == 0) {
      return;
    }
    std::size_t x = cursor_x_ + 1;
    std::size_t y = cursor_y_;
    if (x >= cols) {
      x = 0;
      ++y;
    }
    if (y >= r) {
      scroll_up(1, fg_, bg_);
      y = r - 1;
    }
    cursor_x_ = x;
    cursor_y_ = y;
    if (vtbl_) {
      vtbl_->move_cursor(ctx_, cursor_x_, cursor_y_);
    }
  }

  void newline() noexcept {
    std::size_t r = rows();
    if (r == 0) {
      return;
    }
    std::size_t y = cursor_y_ + 1;
    if (y >= r) {
      scroll_up(1, fg_, bg_);
      y = r - 1;
    }
    cursor_y_ = y;
    if (vtbl_) {
      vtbl_->move_cursor(ctx_, cursor_x_, cursor_y_);
    }
  }

  template <typename Backend>
  static void put_cell_entry(void *ctx, std::size_t x, std::size_t y, char ch, console_color fg,
                             console_color bg) noexcept {
    console_traits<Backend>::put_cell(*static_cast<Backend *>(ctx), x, y, ch, fg, bg);
  }

  template <typename Backend>
  static result<console_cell> get_cell_entry(void *ctx, std::size_t x, std::size_t y) noexcept {
    using traits = console_traits<Backend>;
    if constexpr (detail::console_has_get_cell<traits>::value) {
      return traits::get_cell(*static_cast<Backend *>(ctx), x, y);
    } else {
      (void)ctx;
      (void)x;
      (void)y;
      return unexpected(error::unsupported_operation);
    }
  }

  template <typename Backend> static std::size_t columns_entry(void *ctx) noexcept {
    return console_traits<Backend>::columns(*static_cast<Backend *>(ctx));
  }

  template <typename Backend> static std::size_t rows_entry(void *ctx) noexcept {
    return console_traits<Backend>::rows(*static_cast<Backend *>(ctx));
  }

  template <typename Backend> static void move_cursor_entry(void *ctx, std::size_t x, std::size_t y) noexcept {
    using traits = console_traits<Backend>;
    if constexpr (detail::console_has_move_cursor<traits>::value) {
      traits::move_cursor(*static_cast<Backend *>(ctx), x, y);
    } else {
      (void)ctx;
      (void)x;
      (void)y;
    }
  }

  template <typename Backend> static void set_cursor_visible_entry(void *ctx, bool visible) noexcept {
    using traits = console_traits<Backend>;
    if constexpr (detail::console_has_set_cursor_visible<traits>::value) {
      traits::set_cursor_visible(*static_cast<Backend *>(ctx), visible);
    } else {
      (void)ctx;
      (void)visible;
    }
  }

  template <typename Backend>
  static constexpr vtable s_vtbl{&put_cell_entry<Backend>,    &get_cell_entry<Backend>,
                                 &columns_entry<Backend>,     &rows_entry<Backend>,
                                 &move_cursor_entry<Backend>, &set_cursor_visible_entry<Backend>};

  void *ctx_ = nullptr;
  const vtable *vtbl_ = nullptr;
  std::size_t cursor_x_ = 0;
  std::size_t cursor_y_ = 0;
  console_color fg_ = console_color::light_gray;
  console_color bg_ = console_color::black;
};

} // namespace hw
} // namespace structo
