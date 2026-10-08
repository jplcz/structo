// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file framebuffer.hpp
 * @brief `structo::hw::framebuffer<PixelFormat>`: a simple, non-owning
 * wrapper over a linear pixel buffer (an MMIO-mapped graphics adapter,
 * a simulator's backing store, a software scanout surface, ...), plus
 * a handful of pixel formats (`rgb888`, `bgr888`, `xrgb8888`,
 * `rgba8888`, `rgb565`, `gray8`) and basic 2D rendering helpers built
 * on top of them -- `put_pixel`/`get_pixel`, `fill_rect`/`clear`,
 * `draw_hline`/`draw_vline`/`draw_rect`/`draw_line`, and `blit`.
 *
 * Unlike `uart_ref`/`timer_ref`/`hw_rng_ref`, `framebuffer` is not
 * type-erased over a backend: real framebuffer hardware (a BAR-mapped
 * linear display surface, a devicetree `simple-framebuffer`, a VGA/VESA
 * mode, a hypervisor's virtio-gpu scanout) is overwhelmingly just a
 * flat run of pixels at some stride -- what differs between devices is
 * the *pixel encoding*, not an operation set a backend needs to
 * implement. That single axis of variation is exactly what
 * `PixelFormat` captures; everything else (bounds checking, rect
 * filling, line drawing, blitting) is identical regardless of format
 * and is implemented once, here, on top of it.
 *
 * ## Pixel formats
 *
 * A `PixelFormat` is a stateless struct with:
 * @code
 * static constexpr std::size_t bytes_per_pixel = ...;
 * static void encode(std::byte *dst, rgb_color c) noexcept;
 * static rgb_color decode(const std::byte *src) noexcept;
 * @endcode
 * Six are provided: `rgb888`, `bgr888`, `xrgb8888`, `rgba8888`,
 * `rgb565`, and `gray8` (luminance-only, lossy on encode). A caller
 * can supply its own `PixelFormat` for anything else (e.g. a packed
 * `rgb555`, an indexed palette format) without needing to touch this
 * header.
 *
 * ## Clipping, not failing
 *
 * `put_pixel`/`get_pixel` take a single caller-named coordinate and
 * are bounds-checked, failing with `error::out_of_bounds` if it falls
 * outside the framebuffer -- the caller picked that exact coordinate
 * and almost certainly wants to know if it was wrong. Every multi-pixel
 * helper (`fill_rect`/`draw_hline`/`draw_vline`/`draw_rect`/`draw_line`/
 * `blit`), in contrast, silently clips to the framebuffer's bounds and
 * never fails -- matching how ordinary 2D graphics APIs treat
 * partially- or fully-offscreen shapes, and letting a caller draw UI
 * elements straddling an edge without special-casing it.
 *
 * @code
 * std::byte storage[640 * 480 * 4];
 * auto fb = structo::hw::framebuffer<structo::hw::xrgb8888>::try_create(
 *     reloco::span<std::byte>(storage, sizeof(storage)), 640, 480, 640 * 4);
 * // ... handle fb.error() ...
 *
 * fb->clear({0, 0, 0});                               // black background
 * fb->fill_rect(10, 10, 100, 40, {0, 128, 255});       // a blue-ish panel
 * fb->draw_rect(10, 10, 100, 40, {255, 255, 255});     // white outline
 * fb->draw_line(0, 0, 639, 479, {255, 0, 0});          // a red diagonal
 * (void)fb->put_pixel(320, 240, {0, 255, 0});          // bounds-checked
 * @endcode
 */

#include <cstddef>
#include <cstdint>
#include <reloco/error.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/span.hpp>

namespace structo {

namespace hw {

using namespace reloco;

// ============================================================================
// Color
// ============================================================================

/** @brief A simple 8-bit-per-channel RGB(A) color, independent of any
 * particular `PixelFormat`'s in-memory encoding. */
struct rgb_color {
  std::uint8_t r = 0;
  std::uint8_t g = 0;
  std::uint8_t b = 0;
  std::uint8_t a = 0xff;

  friend constexpr bool operator==(const rgb_color &lhs, const rgb_color &rhs) noexcept {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b && lhs.a == rhs.a;
  }
  friend constexpr bool operator!=(const rgb_color &lhs, const rgb_color &rhs) noexcept { return !(lhs == rhs); }

  static constexpr rgb_color black() noexcept { return {0, 0, 0}; }
  static constexpr rgb_color white() noexcept { return {0xff, 0xff, 0xff}; }
  static constexpr rgb_color red() noexcept { return {0xff, 0, 0}; }
  static constexpr rgb_color green() noexcept { return {0, 0xff, 0}; }
  static constexpr rgb_color blue() noexcept { return {0, 0, 0xff}; }
};

// ============================================================================
// Pixel Formats
// ============================================================================

// Pixel codecs index a pixel's bytes at fixed offsets; callers guarantee
// bytes_per_pixel bytes are available (the framebuffer bounds-checks first).
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

/** @brief 3 bytes per pixel, stored red-then-green-then-blue. */
struct rgb888 {
  static constexpr std::size_t bytes_per_pixel = 3;

  static void encode(std::byte *dst, rgb_color c) noexcept {
    dst[0] = static_cast<std::byte>(c.r);
    dst[1] = static_cast<std::byte>(c.g);
    dst[2] = static_cast<std::byte>(c.b);
  }

  static rgb_color decode(const std::byte *src) noexcept {
    return {static_cast<std::uint8_t>(src[0]), static_cast<std::uint8_t>(src[1]), static_cast<std::uint8_t>(src[2])};
  }
};

/** @brief 3 bytes per pixel, stored blue-then-green-then-red. */
struct bgr888 {
  static constexpr std::size_t bytes_per_pixel = 3;

  static void encode(std::byte *dst, rgb_color c) noexcept {
    dst[0] = static_cast<std::byte>(c.b);
    dst[1] = static_cast<std::byte>(c.g);
    dst[2] = static_cast<std::byte>(c.r);
  }

  static rgb_color decode(const std::byte *src) noexcept {
    return {static_cast<std::uint8_t>(src[2]), static_cast<std::uint8_t>(src[1]), static_cast<std::uint8_t>(src[0])};
  }
};

/** @brief 4 bytes per pixel: an unused/padding byte followed by R, G, B
 * (the common 32-bit-per-pixel linear-framebuffer layout, alpha ignored). */
struct xrgb8888 {
  static constexpr std::size_t bytes_per_pixel = 4;

  static void encode(std::byte *dst, rgb_color c) noexcept {
    dst[0] = std::byte{0};
    dst[1] = static_cast<std::byte>(c.r);
    dst[2] = static_cast<std::byte>(c.g);
    dst[3] = static_cast<std::byte>(c.b);
  }

  static rgb_color decode(const std::byte *src) noexcept {
    return {static_cast<std::uint8_t>(src[1]), static_cast<std::uint8_t>(src[2]), static_cast<std::uint8_t>(src[3])};
  }
};

/** @brief 4 bytes per pixel: R, G, B, then alpha. */
struct rgba8888 {
  static constexpr std::size_t bytes_per_pixel = 4;

  static void encode(std::byte *dst, rgb_color c) noexcept {
    dst[0] = static_cast<std::byte>(c.r);
    dst[1] = static_cast<std::byte>(c.g);
    dst[2] = static_cast<std::byte>(c.b);
    dst[3] = static_cast<std::byte>(c.a);
  }

  static rgb_color decode(const std::byte *src) noexcept {
    return {static_cast<std::uint8_t>(src[0]), static_cast<std::uint8_t>(src[1]), static_cast<std::uint8_t>(src[2]),
            static_cast<std::uint8_t>(src[3])};
  }
};

/** @brief 2 bytes per pixel, packed 5-6-5 bits (little-endian in memory). */
struct rgb565 {
  static constexpr std::size_t bytes_per_pixel = 2;

  static void encode(std::byte *dst, rgb_color c) noexcept {
    std::uint16_t packed = static_cast<std::uint16_t>(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
    dst[0] = static_cast<std::byte>(packed & 0xff);
    dst[1] = static_cast<std::byte>((packed >> 8) & 0xff);
  }

  static rgb_color decode(const std::byte *src) noexcept {
    std::uint16_t packed =
        static_cast<std::uint16_t>(static_cast<std::uint8_t>(src[0]) | (static_cast<std::uint8_t>(src[1]) << 8));
    std::uint8_t r5 = static_cast<std::uint8_t>((packed >> 11) & 0x1f);
    std::uint8_t g6 = static_cast<std::uint8_t>((packed >> 5) & 0x3f);
    std::uint8_t b5 = static_cast<std::uint8_t>(packed & 0x1f);
    // Replicate the high bits into the vacated low bits so black/white round-trip exactly.
    return {static_cast<std::uint8_t>((r5 << 3) | (r5 >> 2)), static_cast<std::uint8_t>((g6 << 2) | (g6 >> 4)),
            static_cast<std::uint8_t>((b5 << 3) | (b5 >> 2))};
  }
};

/** @brief 1 byte per pixel, luminance only. `encode` is necessarily
 * lossy (averages R/G/B); `decode` reports the same value on all three
 * channels. */
struct gray8 {
  static constexpr std::size_t bytes_per_pixel = 1;

  static void encode(std::byte *dst, rgb_color c) noexcept {
    unsigned luminance = (static_cast<unsigned>(c.r) + c.g + c.b) / 3;
    dst[0] = static_cast<std::byte>(luminance);
  }

  static rgb_color decode(const std::byte *src) noexcept {
    auto v = static_cast<std::uint8_t>(src[0]);
    return {v, v, v};
  }
};

RELOCO_END_UNSAFE_BUFFER_USAGE

// ============================================================================
// Framebuffer
// ============================================================================

/**
 * @brief Non-owning, allocation-free wrapper over a linear pixel buffer,
 * plus bounds-checked single-pixel access and clipped 2D drawing helpers.
 * @tparam PixelFormat One of `rgb888`/`bgr888`/`xrgb8888`/`rgba8888`/
 * `rgb565`/`gray8`, or a caller-supplied type with the same
 * `bytes_per_pixel`/`encode`/`decode` members.
 */
template <typename PixelFormat> class framebuffer {
public:
  using pixel_format = PixelFormat;
  static constexpr std::size_t bytes_per_pixel = PixelFormat::bytes_per_pixel;

  constexpr framebuffer() noexcept = default;

  /**
   * @brief Binds a framebuffer to @p buffer, interpreting it as
   * @p height rows of @p width pixels each, @p stride_bytes apart
   * (allowing row padding, e.g. a scanout surface wider than its
   * visible area). @p buffer must outlive this `framebuffer` and is
   * not copied.
   * @return `error::invalid_argument` if @p width/@p height are zero,
   * or @p stride_bytes is smaller than `width * bytes_per_pixel`;
   * `error::out_of_range` if @p buffer is smaller than
   * `stride_bytes * height`.
   */
  [[nodiscard]] static result<framebuffer> try_create(span<std::byte> buffer, std::size_t width, std::size_t height,
                                                      std::size_t stride_bytes) noexcept {
    if (width == 0 || height == 0 || stride_bytes < width * bytes_per_pixel) {
      return unexpected(error::invalid_argument);
    }
    if (buffer.size() < stride_bytes * height) {
      return unexpected(error::out_of_range);
    }
    return framebuffer(buffer, width, height, stride_bytes);
  }

  [[nodiscard]] constexpr std::size_t width() const noexcept { return width_; }
  [[nodiscard]] constexpr std::size_t height() const noexcept { return height_; }
  [[nodiscard]] constexpr std::size_t stride_bytes() const noexcept { return stride_bytes_; }

  /** @brief The raw backing buffer this framebuffer was bound to. */
  [[nodiscard]] constexpr span<std::byte> raw() const noexcept { return buffer_; }

  /**
   * @brief Writes @p color at (@p x, @p y).
   * @return `error::out_of_bounds` if (@p x, @p y) falls outside
   * `[0, width()) x [0, height())`.
   */
  result<void> put_pixel(std::size_t x, std::size_t y, rgb_color color) noexcept {
    if (x >= width_ || y >= height_) {
      return unexpected(error::out_of_bounds);
    }
    PixelFormat::encode(pixel_ptr(x, y), color);
    return {};
  }

  /**
   * @brief Reads the color at (@p x, @p y).
   * @return `error::out_of_bounds` if (@p x, @p y) falls outside
   * `[0, width()) x [0, height())`.
   */
  [[nodiscard]] result<rgb_color> get_pixel(std::size_t x, std::size_t y) const noexcept {
    if (x >= width_ || y >= height_) {
      return unexpected(error::out_of_bounds);
    }
    return PixelFormat::decode(pixel_ptr(x, y));
  }

  /** @brief Fills every pixel with @p color. */
  void clear(rgb_color color) noexcept { fill_rect(0, 0, width_, height_, color); }

  /** @brief Fills the `w x h` rectangle at (@p x, @p y) with @p color,
   * clipped to the framebuffer's bounds (a fully offscreen rectangle is
   * a no-op). */
  void fill_rect(std::size_t x, std::size_t y, std::size_t w, std::size_t h, rgb_color color) noexcept {
    auto [x0, y0, x1, y1] = clip_rect(x, y, w, h);
    for (std::size_t py = y0; py < y1; ++py) {
      for (std::size_t px = x0; px < x1; ++px) {
        PixelFormat::encode(pixel_ptr(px, py), color);
      }
    }
  }

  /** @brief Draws a horizontal run of @p length pixels starting at
   * (@p x, @p y), clipped to the framebuffer's bounds. */
  void draw_hline(std::size_t x, std::size_t y, std::size_t length, rgb_color color) noexcept {
    fill_rect(x, y, length, 1, color);
  }

  /** @brief Draws a vertical run of @p length pixels starting at
   * (@p x, @p y), clipped to the framebuffer's bounds. */
  void draw_vline(std::size_t x, std::size_t y, std::size_t length, rgb_color color) noexcept {
    fill_rect(x, y, 1, length, color);
  }

  /** @brief Draws the 1-pixel-wide outline of the `w x h` rectangle at
   * (@p x, @p y), clipped to the framebuffer's bounds. */
  void draw_rect(std::size_t x, std::size_t y, std::size_t w, std::size_t h, rgb_color color) noexcept {
    if (w == 0 || h == 0) {
      return;
    }
    draw_hline(x, y, w, color);
    draw_hline(x, y + h - 1, w, color);
    draw_vline(x, y, h, color);
    draw_vline(x + w - 1, y, h, color);
  }

  /** @brief Draws a straight line from (@p x0, @p y0) to (@p x1, @p y1)
   * (Bresenham's algorithm), clipping any portion outside the
   * framebuffer's bounds pixel-by-pixel. Coordinates are signed so a
   * line may start/end offscreen in any direction. */
  void draw_line(std::ptrdiff_t x0, std::ptrdiff_t y0, std::ptrdiff_t x1, std::ptrdiff_t y1, rgb_color color) noexcept {
    std::ptrdiff_t dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    std::ptrdiff_t sx = (x0 < x1) ? 1 : -1;
    std::ptrdiff_t dy = -((y1 > y0) ? (y1 - y0) : (y0 - y1));
    std::ptrdiff_t sy = (y0 < y1) ? 1 : -1;
    std::ptrdiff_t err = dx + dy;

    for (;;) {
      if (x0 >= 0 && y0 >= 0 && static_cast<std::size_t>(x0) < width_ && static_cast<std::size_t>(y0) < height_) {
        PixelFormat::encode(pixel_ptr(static_cast<std::size_t>(x0), static_cast<std::size_t>(y0)), color);
      }
      if (x0 == x1 && y0 == y1) {
        break;
      }
      std::ptrdiff_t e2 = 2 * err;
      if (e2 >= dy) {
        err += dy;
        x0 += sx;
      }
      if (e2 <= dx) {
        err += dx;
        y0 += sy;
      }
    }
  }

  /**
   * @brief Copies the `w x h` rectangle starting at (@p src_x, @p src_y)
   * in @p src to (@p dst_x, @p dst_y) in this framebuffer, clipped to
   * both framebuffers' bounds. Source and destination pixel formats may
   * differ -- each pixel is decoded from @p src and re-encoded into
   * this framebuffer, rather than copied byte-for-byte.
   */
  template <typename SrcPixelFormat>
  void blit(std::size_t dst_x, std::size_t dst_y, const framebuffer<SrcPixelFormat> &src, std::size_t src_x,
            std::size_t src_y, std::size_t w, std::size_t h) noexcept {
    if (src_x >= src.width() || src_y >= src.height()) {
      return;
    }
    w = (w < src.width() - src_x) ? w : (src.width() - src_x);
    h = (h < src.height() - src_y) ? h : (src.height() - src_y);

    auto [x0, y0, x1, y1] = clip_rect(dst_x, dst_y, w, h);
    for (std::size_t py = y0; py < y1; ++py) {
      for (std::size_t px = x0; px < x1; ++px) {
        std::size_t sx = src_x + (px - dst_x);
        std::size_t sy = src_y + (py - dst_y);
        auto src_pixel = src.get_pixel(sx, sy);
        PixelFormat::encode(pixel_ptr(px, py), *src_pixel);
      }
    }
  }

private:
  constexpr framebuffer(span<std::byte> buffer, std::size_t width, std::size_t height,
                        std::size_t stride_bytes) noexcept
      : buffer_(buffer), width_(width), height_(height), stride_bytes_(stride_bytes) {}

  [[nodiscard]] std::byte *pixel_ptr(std::size_t x, std::size_t y) noexcept {
    return buffer_.data() + (y * stride_bytes_) + (x * bytes_per_pixel);
  }

  [[nodiscard]] const std::byte *pixel_ptr(std::size_t x, std::size_t y) const noexcept {
    return buffer_.data() + (y * stride_bytes_) + (x * bytes_per_pixel);
  }

  struct clipped_rect {
    std::size_t x0, y0, x1, y1;
  };

  [[nodiscard]] clipped_rect clip_rect(std::size_t x, std::size_t y, std::size_t w, std::size_t h) const noexcept {
    std::size_t x0 = x;
    std::size_t y0 = y;
    std::size_t x1 = (x + w > width_) ? width_ : x + w;
    std::size_t y1 = (y + h > height_) ? height_ : y + h;
    if (x0 >= width_ || y0 >= height_ || x0 >= x1 || y0 >= y1) {
      return {0, 0, 0, 0};
    }
    return {x0, y0, x1, y1};
  }

  span<std::byte> buffer_;
  std::size_t width_ = 0;
  std::size_t height_ = 0;
  std::size_t stride_bytes_ = 0;
};

} // namespace hw
} // namespace structo
