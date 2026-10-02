// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file framebuffer_console.hpp
 * @brief `structo::hw::framebuffer_console<PixelFormat, Font>`: a
 * `console_ref`-adaptable backend that rasterizes a character-cell text
 * console onto a `framebuffer<PixelFormat>` (`framebuffer.hpp`), one
 * monospace glyph per cell.
 *
 * ## Backing cell storage
 *
 * Pixels are a write-only medium: once a glyph is rasterized, there is
 * no way to read the character back out of the framebuffer itself. To
 * still support `console_ref::get_char` and (more importantly)
 * `console_ref::scroll_up` -- which needs to read a cell's old content
 * to move it -- this backend keeps its own shadow grid of
 * `console_cell`s, exactly mirroring what has been drawn. Per this
 * library's no-hidden-allocation convention, that storage is supplied
 * by the caller as a `reloco::span<console_cell>` of `columns * rows`
 * cells, sized to the framebuffer's dimensions divided by the glyph
 * size (see `try_create`).
 *
 * ## Fonts
 *
 * Glyph rendering is pluggable via the `Font` template parameter, a
 * compile-time trait:
 * @code
 * struct my_font {
 *   static constexpr std::size_t glyph_width = 8;
 *   static constexpr std::size_t glyph_height = 8;
 *   // Returns glyph_height bytes, each the glyph's row as a bitmap:
 *   // bit 7 (MSB) is the leftmost pixel, bit 0 the rightmost.
 *   static const std::uint8_t *glyph_bitmap(char ch) noexcept;
 * };
 * @endcode
 * This header ships exactly one built-in `Font`, @ref block_font_8x8,
 * which is an intentionally honest *placeholder*: every non-space,
 * printable character renders as a solid filled block, not a real
 * glyph shape. This is deliberate -- shipping a traced bitmap of an
 * actual existing PC font risks reproducing copyrighted glyph artwork,
 * so this library does not embed one. Bring your own real `Font` (e.g.
 * from an 8x8/8x16 font ROM dump you have the rights to use) for
 * legible text; `block_font_8x8` only proves the rendering pipeline
 * works and is good enough for cursor/cell-geometry testing.
 *
 * @code
 * std::byte fb_memory[640 * 480 * 4];
 * auto fb = structo::hw::framebuffer<structo::hw::xrgb8888>::try_create(
 *     reloco::span<std::byte>(fb_memory, sizeof(fb_memory)), 640, 480, 640 * 4);
 * structo::hw::console_cell cells[(640 / 8) * (480 / 8)]{};
 * auto console = structo::hw::framebuffer_console<structo::hw::xrgb8888>::try_create(
 *     *fb, reloco::span<structo::hw::console_cell>(cells, sizeof(cells) / sizeof(cells[0])));
 * structo::hw::console_ref ref(*console);
 * ref.write("Hello, framebuffer!\n");
 * @endcode
 */

#include "console_ref.hpp"
#include "framebuffer.hpp"

#include <reloco/error.hpp>
#include <reloco/span.hpp>

#include <cstddef>
#include <cstdint>

namespace structo {
namespace hw {

/**
 * @brief Honest placeholder `Font`: every printable, non-space
 * character is a solid filled 8x8 block; space and control characters
 * are blank. See the @file-level docs above for why this is a
 * deliberate stand-in rather than a real bitmap font.
 */
struct block_font_8x8 {
  static constexpr std::size_t glyph_width = 8;
  static constexpr std::size_t glyph_height = 8;

  [[nodiscard]] static const std::uint8_t *glyph_bitmap(char ch) noexcept {
    static constexpr std::uint8_t blank[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    static constexpr std::uint8_t block[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    bool printable = ch > ' ' && ch < 0x7F;
    return printable ? block : blank;
  }
};

/**
 * @brief Maps the 16-color @ref console_color palette to RGB, using the
 * conventional CGA/VGA default palette values.
 */
[[nodiscard]] constexpr rgb_color console_color_to_rgb(console_color c) noexcept {
  switch (c) {
  case console_color::black:
    return {0x00, 0x00, 0x00};
  case console_color::blue:
    return {0x00, 0x00, 0xAA};
  case console_color::green:
    return {0x00, 0xAA, 0x00};
  case console_color::cyan:
    return {0x00, 0xAA, 0xAA};
  case console_color::red:
    return {0xAA, 0x00, 0x00};
  case console_color::magenta:
    return {0xAA, 0x00, 0xAA};
  case console_color::brown:
    return {0xAA, 0x55, 0x00};
  case console_color::light_gray:
    return {0xAA, 0xAA, 0xAA};
  case console_color::dark_gray:
    return {0x55, 0x55, 0x55};
  case console_color::light_blue:
    return {0x55, 0x55, 0xFF};
  case console_color::light_green:
    return {0x55, 0xFF, 0x55};
  case console_color::light_cyan:
    return {0x55, 0xFF, 0xFF};
  case console_color::light_red:
    return {0xFF, 0x55, 0x55};
  case console_color::light_magenta:
    return {0xFF, 0x55, 0xFF};
  case console_color::yellow:
    return {0xFF, 0xFF, 0x55};
  case console_color::white:
    return {0xFF, 0xFF, 0xFF};
  }
  return rgb_color::black();
}

/**
 * @brief Backend rasterizing a character-cell console onto a
 * `framebuffer<PixelFormat>`, adaptable to @ref console_ref. See the
 * @file-level docs above.
 */
template <typename PixelFormat, typename Font = block_font_8x8> class framebuffer_console {
public:
  using pixel_format = PixelFormat;
  using font = Font;

  constexpr framebuffer_console() noexcept = default;

  /**
   * @brief Binds a text console to @p fb, dividing its pixel area into
   * `fb.width() / Font::glyph_width` columns by
   * `fb.height() / Font::glyph_height` rows, using @p cell_storage as
   * the backing shadow grid (see the @file-level docs above). Clears
   * every cell to a space on black.
   * @param cell_storage Caller-owned storage, must outlive this
   * console and hold at least `columns() * rows()` cells.
   * @return `error::invalid_argument` if @p fb is too small to fit even
   * one glyph; `error::out_of_range` if @p cell_storage is smaller than
   * `columns() * rows()`.
   */
  [[nodiscard]] static result<framebuffer_console> try_create(framebuffer<PixelFormat> fb,
                                                              span<console_cell> cell_storage) noexcept {
    std::size_t cols = fb.width() / Font::glyph_width;
    std::size_t r = fb.height() / Font::glyph_height;
    if (cols == 0 || r == 0) {
      return unexpected(error::invalid_argument);
    }
    if (cell_storage.size() < cols * r) {
      return unexpected(error::out_of_range);
    }
    framebuffer_console console(fb, cols, r, cell_storage);
    console.clear();
    return console;
  }

  [[nodiscard]] constexpr std::size_t columns() const noexcept { return columns_; }
  [[nodiscard]] constexpr std::size_t rows() const noexcept { return rows_; }

  /** @brief The underlying framebuffer being rasterized onto. */
  [[nodiscard]] constexpr const framebuffer<PixelFormat> &pixels() const noexcept { return fb_; }

  /** @brief Writes `ch`/`fg`/`bg` at `(x, y)`: updates the shadow cell
   * and rasterizes the glyph. UB if out of bounds; callers normally
   * reach this only through a bounds-checked @ref console_ref. */
  void put_cell(std::size_t x, std::size_t y, char ch, console_color fg, console_color bg) noexcept {
    cell_storage_[y * columns_ + x] = {ch, fg, bg};
    draw_glyph(x, y, ch, fg, bg);
  }

  /** @brief Reads the shadow cell at `(x, y)`. UB if out of bounds. */
  [[nodiscard]] console_cell get_cell(std::size_t x, std::size_t y) const noexcept {
    return cell_storage_[y * columns_ + x];
  }

private:
  framebuffer_console(framebuffer<PixelFormat> fb, std::size_t cols, std::size_t rows,
                      span<console_cell> cell_storage) noexcept
      : fb_(fb), columns_(cols), rows_(rows), cell_storage_(cell_storage) {}

  void clear() noexcept {
    for (std::size_t y = 0; y < rows_; ++y) {
      for (std::size_t x = 0; x < columns_; ++x) {
        put_cell(x, y, ' ', console_color::light_gray, console_color::black);
      }
    }
  }

  void draw_glyph(std::size_t x, std::size_t y, char ch, console_color fg, console_color bg) noexcept {
    const std::uint8_t *bitmap = Font::glyph_bitmap(ch);
    rgb_color fg_rgb = console_color_to_rgb(fg);
    rgb_color bg_rgb = console_color_to_rgb(bg);
    std::size_t base_x = x * Font::glyph_width;
    std::size_t base_y = y * Font::glyph_height;
    for (std::size_t row = 0; row < Font::glyph_height; ++row) {
      std::uint8_t bits = bitmap[row];
      for (std::size_t col = 0; col < Font::glyph_width; ++col) {
        bool set = (bits & (0x80u >> col)) != 0;
        auto discard = fb_.put_pixel(base_x + col, base_y + row, set ? fg_rgb : bg_rgb);
        (void)discard;
      }
    }
  }

  framebuffer<PixelFormat> fb_{};
  std::size_t columns_ = 0;
  std::size_t rows_ = 0;
  span<console_cell> cell_storage_{};
};

/** @brief Adapts @ref framebuffer_console to @ref console_ref. */
template <typename PixelFormat, typename Font> struct console_traits<framebuffer_console<PixelFormat, Font>> {
  using backend = framebuffer_console<PixelFormat, Font>;

  static void put_cell(backend &b, std::size_t x, std::size_t y, char ch, console_color fg, console_color bg) noexcept {
    b.put_cell(x, y, ch, fg, bg);
  }
  static result<console_cell> get_cell(const backend &b, std::size_t x, std::size_t y) noexcept {
    return b.get_cell(x, y);
  }
  static std::size_t columns(const backend &b) noexcept { return b.columns(); }
  static std::size_t rows(const backend &b) noexcept { return b.rows(); }
};

} // namespace hw
} // namespace structo
