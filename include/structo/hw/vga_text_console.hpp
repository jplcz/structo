// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file vga_text_console.hpp
 * @brief `structo::hw::vga_text_console`: a `console_ref`-adaptable
 * backend for the classic PC VGA/CGA text-mode framebuffer (2 bytes per
 * cell: an ASCII/CP437 code point byte followed by a packed attribute
 * byte, `0xBHHHLLLL`-style: bit 7 blink/bright-background, bits 6-4
 * background color, bits 3-0 foreground color).
 *
 * This header only wraps the memory-mapped text buffer itself (e.g. the
 * identity/linear-mapped `0xB8000` VGA text segment) -- it has no
 * dependency on actual I/O port access. Forwarding the logical cursor
 * position to the real CRTC index/data registers (ports `0x3D4`/`0x3D5`)
 * is left to an optional, caller-supplied `reloco::function_ref`
 * callback, so this header stays portable/testable without any
 * `io_space_ref`/port-I/O dependency; a caller targeting real VGA
 * hardware plugs that in, while a caller only emulating a text buffer
 * (or running under a framebuffer-less test) can simply omit it.
 *
 * @code
 * std::byte vga_memory[80 * 25 * 2];
 * auto console = structo::hw::vga_text_console::try_create(
 *     reloco::span<std::byte>(vga_memory, sizeof(vga_memory)), 80, 25);
 * structo::hw::console_ref ref(*console);
 * ref.write("Hello, VGA!\n");
 * @endcode
 */

#include "console_ref.hpp"

#include <reloco/error.hpp>
#include <reloco/function_ref.hpp>
#include <reloco/optional.hpp>
#include <reloco/span.hpp>

#include <cstddef>
#include <cstdint>

namespace structo {
namespace hw {

/**
 * @brief Backend wrapping a raw 2-bytes-per-cell VGA text buffer,
 * adaptable to @ref console_ref. See the @file-level docs above.
 */
class vga_text_console {
public:
  /** @brief Hook invoked with `(x, y)` whenever the logical cursor
   * moves, so a caller can forward it to real CRTC registers. */
  using cursor_sink = reloco::function_ref<void(std::size_t, std::size_t)>;

  constexpr vga_text_console() noexcept = default;

  /**
   * @brief Binds a VGA text console to @p buffer, interpreting it as
   * @p columns x @p rows cells, 2 bytes each (code point, then packed
   * attribute). @p buffer must outlive this console and is not copied.
   * @param on_move_cursor Optional callback invoked with the new
   * `(x, y)` whenever the logical cursor moves (see @ref console_ref);
   * must outlive this console if supplied.
   * @return `error::invalid_argument` if @p columns/@p rows are zero;
   * `error::out_of_range` if @p buffer is smaller than
   * `columns * rows * 2` bytes.
   */
  [[nodiscard]] static result<vga_text_console> try_create(reloco::span<std::byte> buffer, std::size_t columns,
                                                           std::size_t rows,
                                                           reloco::optional<cursor_sink> on_move_cursor = {}) noexcept {
    if (columns == 0 || rows == 0) {
      return unexpected(error::invalid_argument);
    }
    if (buffer.size() < columns * rows * 2) {
      return unexpected(error::out_of_range);
    }
    return vga_text_console(buffer, columns, rows, on_move_cursor);
  }

  [[nodiscard]] constexpr std::size_t columns() const noexcept { return columns_; }
  [[nodiscard]] constexpr std::size_t rows() const noexcept { return rows_; }

  /** @brief The raw backing buffer this console was bound to. */
  [[nodiscard]] constexpr reloco::span<std::byte> raw() const noexcept { return buffer_; }

  /** @brief Writes `ch`/`fg`/`bg` at `(x, y)`. UB if out of bounds;
   * callers normally reach this only through a bounds-checked
   * @ref console_ref, which this backend is adapted to. */
  void put_cell(std::size_t x, std::size_t y, char ch, console_color fg, console_color bg) noexcept {
    std::byte *cell = cell_ptr(x, y);
    cell[0] = static_cast<std::byte>(ch);
    cell[1] = pack_attribute(fg, bg);
  }

  /** @brief Reads the cell at `(x, y)`. UB if out of bounds. */
  [[nodiscard]] console_cell get_cell(std::size_t x, std::size_t y) const noexcept {
    const std::byte *cell = cell_ptr(x, y);
    std::uint8_t attr = static_cast<std::uint8_t>(cell[1]);
    return console_cell{static_cast<char>(cell[0]), static_cast<console_color>(attr & 0x0F),
                        static_cast<console_color>((attr >> 4) & 0x07)};
  }

  /** @brief Forwards `(x, y)` to the optional cursor-sink callback, if
   * one was supplied at `try_create` time; otherwise a no-op. */
  void move_cursor(std::size_t x, std::size_t y) noexcept {
    if (on_move_cursor_) {
      (*on_move_cursor_)(x, y);
    }
  }

private:
  vga_text_console(reloco::span<std::byte> buffer, std::size_t columns, std::size_t rows,
                   reloco::optional<cursor_sink> on_move_cursor) noexcept
      : buffer_(buffer), columns_(columns), rows_(rows), on_move_cursor_(on_move_cursor) {}

  [[nodiscard]] std::byte *cell_ptr(std::size_t x, std::size_t y) noexcept {
    return buffer_.data() + (y * columns_ + x) * 2;
  }
  [[nodiscard]] const std::byte *cell_ptr(std::size_t x, std::size_t y) const noexcept {
    return buffer_.data() + (y * columns_ + x) * 2;
  }

  /** @brief Packs `fg`/`bg` into the standard VGA attribute byte:
   * bits 3-0 foreground (0-15), bits 6-4 background (masked to 0-7:
   * VGA text mode reserves bit 7 for either a bright background or a
   * blink flag depending on the controller's mode-control register, so
   * only the low 3 bits of `bg` are representable here). */
  [[nodiscard]] static std::byte pack_attribute(console_color fg, console_color bg) noexcept {
    std::uint8_t fg_bits = static_cast<std::uint8_t>(fg) & 0x0F;
    std::uint8_t bg_bits = static_cast<std::uint8_t>(bg) & 0x07;
    return static_cast<std::byte>(fg_bits | (bg_bits << 4));
  }

  reloco::span<std::byte> buffer_{};
  std::size_t columns_ = 0;
  std::size_t rows_ = 0;
  reloco::optional<cursor_sink> on_move_cursor_{};
};

/** @brief Adapts @ref vga_text_console to @ref console_ref. */
template <> struct console_traits<vga_text_console> {
  static void put_cell(vga_text_console &b, std::size_t x, std::size_t y, char ch, console_color fg,
                       console_color bg) noexcept {
    b.put_cell(x, y, ch, fg, bg);
  }
  static result<console_cell> get_cell(const vga_text_console &b, std::size_t x, std::size_t y) noexcept {
    return b.get_cell(x, y);
  }
  static std::size_t columns(const vga_text_console &b) noexcept { return b.columns(); }
  static std::size_t rows(const vga_text_console &b) noexcept { return b.rows(); }
  static void move_cursor(vga_text_console &b, std::size_t x, std::size_t y) noexcept { b.move_cursor(x, y); }
};

} // namespace hw
} // namespace structo
