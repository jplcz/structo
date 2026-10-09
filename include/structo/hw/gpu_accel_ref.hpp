// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file gpu_accel_ref.hpp
 * @brief `structo::hw::gpu_accel_ref`: a type-erased, non-owning handle
 * over a 2D drawing backend -- a software `hw::framebuffer<PixelFormat>`
 * by default, or a real hardware-accelerated renderer (e.g. an SDL
 * `SDL_Renderer` wrapper) -- plus the `gpu_accel_traits<Backend>`
 * customization point a concrete backend specializes to be bindable
 * through it.
 *
 * ## Why this exists alongside `hw::framebuffer`
 *
 * `framebuffer<PixelFormat>` already has `put_pixel`/`fill_rect`/
 * `draw_rect`/`draw_line`/`clear` -- but it is a concrete class
 * template over a pixel *encoding*, always drawing into plain memory
 * this process owns. `gpu_accel_ref` is the same small drawing
 * vocabulary, but type-erased over the *drawing implementation* rather
 * than the pixel format: a backend bound through it may draw into plain
 * memory exactly like `framebuffer` does (see the default
 * `gpu_accel_traits<framebuffer<PixelFormat>>` specialization below,
 * which simply forwards every call), or it may translate each call into
 * real GPU work -- `SDL_RenderFillRect`/`SDL_RenderDrawLine`/
 * `SDL_RenderClear` against an `SDL_Renderer`, an OpenGL/Vulkan
 * immediate-mode shim, or anything else a host application wants to
 * plug in. `structo` itself only ever ships the software/`framebuffer`
 * backend; the point of the indirection is letting
 * `hypervisor::mmio_gpu_command_buffer_device` (see that header) stay
 * completely unaware of which of those a given embedding chose.
 *
 * ## Customizing: `gpu_accel_traits<Backend>`
 *
 * A specialization must supply:
 * @code
 * template <> struct structo::hw::gpu_accel_traits<my_backend> {
 *   static std::size_t width(const my_backend &) noexcept;
 *   static std::size_t height(const my_backend &) noexcept;
 *   static void clear(my_backend &, rgb_color) noexcept;
 *   static void fill_rect(my_backend &, std::size_t x, std::size_t y, std::size_t w, std::size_t h,
 *                         rgb_color) noexcept;
 *   static void draw_line(my_backend &, std::ptrdiff_t x0, std::ptrdiff_t y0, std::ptrdiff_t x1,
 *                        std::ptrdiff_t y1, rgb_color) noexcept;
 * };
 * @endcode
 * Optionally, also `draw_rect(Backend &, x, y, w, h, rgb_color)` and
 * `put_pixel(Backend &, x, y, rgb_color) -> result<void>` -- detected
 * via SFINAE, the same optional-member idiom `console_traits`'s
 * `move_cursor`/`set_cursor_visible` use. Without them, `gpu_accel_ref`
 * synthesizes `draw_rect` from four `draw_line` calls and `put_pixel`
 * from a `1 x 1` `fill_rect`, so a minimal backend (just `clear`/
 * `fill_rect`/`draw_line`, e.g. the handful of primitives an
 * `SDL_Renderer` already has a direct call for) gets every operation
 * `gpu_accel_ref` exposes for free.
 *
 * @code
 * structo::hw::framebuffer<structo::hw::xrgb8888> fb = ...;
 * structo::hw::gpu_accel_ref gpu(fb);     // software backend, forwards directly
 * gpu.clear({0, 0, 0});
 * gpu.fill_rect(10, 10, 100, 40, {0, 128, 255});
 * (void)gpu.put_pixel(320, 240, {255, 255, 255}); // synthesized from fill_rect
 * @endcode
 */

#include <structo/hw/framebuffer.hpp>

#include <cstddef>
#include <memory>
#include <reloco/detail/compat.hpp>
#include <reloco/error.hpp>
#include <reloco/lifetime.hpp>
#include <type_traits>

namespace structo::hw {

using namespace reloco;

// ============================================================================
// Customization Point
// ============================================================================

/**
 * @brief Opt-in customization point describing how to draw into a
 * concrete 2D backend, through @ref gpu_accel_ref. Intentionally left
 * undefined for any `Backend` that hasn't been adapted. See the
 * @file-level docs above for the required/optional member list.
 */
template <typename Backend> struct gpu_accel_traits;

namespace detail {

template <typename Backend, typename = void> struct has_gpu_accel_traits : std::false_type {};

template <typename Backend>
struct has_gpu_accel_traits<
    Backend, std::void_t<decltype(gpu_accel_traits<Backend>::width), decltype(gpu_accel_traits<Backend>::height),
                         decltype(gpu_accel_traits<Backend>::clear), decltype(gpu_accel_traits<Backend>::fill_rect),
                         decltype(gpu_accel_traits<Backend>::draw_line)>> : std::true_type {};

template <typename Traits, typename = void> struct gpu_accel_has_draw_rect : std::false_type {};
template <typename Traits>
struct gpu_accel_has_draw_rect<Traits, std::void_t<decltype(Traits::draw_rect)>> : std::true_type {};

template <typename Traits, typename = void> struct gpu_accel_has_put_pixel : std::false_type {};
template <typename Traits>
struct gpu_accel_has_put_pixel<Traits, std::void_t<decltype(Traits::put_pixel)>> : std::true_type {};

} // namespace detail

/**
 * @brief Default backend: forwards every operation straight to a bound
 * `hw::framebuffer<PixelFormat>` (software drawing into plain memory).
 * This is the backend every `gpu_accel_ref` uses unless an embedding
 * deliberately specializes `gpu_accel_traits` for some other (e.g. real
 * hardware-accelerated) backend type instead.
 */
template <typename PixelFormat> struct gpu_accel_traits<framebuffer<PixelFormat>> {
  using backend = framebuffer<PixelFormat>;

  static std::size_t width(const backend &b) noexcept { return b.width(); }
  static std::size_t height(const backend &b) noexcept { return b.height(); }
  static void clear(backend &b, rgb_color c) noexcept { b.clear(c); }
  static void fill_rect(backend &b, std::size_t x, std::size_t y, std::size_t w, std::size_t h, rgb_color c) noexcept {
    b.fill_rect(x, y, w, h, c);
  }
  static void draw_rect(backend &b, std::size_t x, std::size_t y, std::size_t w, std::size_t h, rgb_color c) noexcept {
    b.draw_rect(x, y, w, h, c);
  }
  static void draw_line(backend &b, std::ptrdiff_t x0, std::ptrdiff_t y0, std::ptrdiff_t x1, std::ptrdiff_t y1,
                        rgb_color c) noexcept {
    b.draw_line(x0, y0, x1, y1, c);
  }
  static result<void> put_pixel(backend &b, std::size_t x, std::size_t y, rgb_color c) noexcept {
    return b.put_pixel(x, y, c);
  }
};

// ============================================================================
// Type-Erased 2D Drawing Handle
// ============================================================================

/**
 * @brief Type-erased, non-owning handle over a 2D drawing backend, for
 * whatever concrete backend it is bound to -- see the @file-level docs
 * above.
 *
 * Default-constructed (or copied from a default-constructed) refs are
 * *unbound*: every drawing operation silently no-ops (mirroring
 * `console_ref`'s null-safety convention); `width()`/`height()` report
 * `0` and `put_pixel` fails with `error::unsupported_operation`.
 */
class RELOCO_POINTER gpu_accel_ref {
public:
  /** @brief Fixed, per-bound-backend-type dispatch table. */
  struct vtable {
    std::size_t (*width)(void *ctx) noexcept;
    std::size_t (*height)(void *ctx) noexcept;
    void (*clear)(void *ctx, rgb_color c) noexcept;
    void (*fill_rect)(void *ctx, std::size_t x, std::size_t y, std::size_t w, std::size_t h, rgb_color c) noexcept;
    void (*draw_rect)(void *ctx, std::size_t x, std::size_t y, std::size_t w, std::size_t h, rgb_color c) noexcept;
    void (*draw_line)(void *ctx, std::ptrdiff_t x0, std::ptrdiff_t y0, std::ptrdiff_t x1, std::ptrdiff_t y1,
                      rgb_color c) noexcept;
    result<void> (*put_pixel)(void *ctx, std::size_t x, std::size_t y, rgb_color c) noexcept;
  };

  /** @brief Constructs an unbound ref. */
  constexpr gpu_accel_ref() noexcept = default;

  /**
   * @brief Binds this ref to an existing, adapted backend.
   * @tparam Backend Concrete backend type, deduced. Must have a
   * @ref gpu_accel_traits specialization.
   * @param b Backend to bind. Must outlive this handle and every copy
   * of it. Marked `explicit`: binding a backend is always a deliberate
   * step, never an implicit conversion.
   */
  template <typename Backend, std::enable_if_t<detail::has_gpu_accel_traits<Backend>::value, int> = 0>
  constexpr explicit gpu_accel_ref(Backend &b RELOCO_LIFETIMEBOUND RELOCO_LIFETIME_CAPTURE_BY_THIS) noexcept
      : ctx_(std::addressof(b)), vtbl_(&s_vtbl<Backend>) {}

  /** @brief Rejects rvalue/temporary backend bindings. */
  template <typename Backend, std::enable_if_t<!std::is_lvalue_reference_v<Backend>, int> = 0>
  gpu_accel_ref(Backend &&) = delete;

  /** @brief Whether this ref is bound to a backend. */
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return vtbl_ != nullptr; }

  /** @brief Surface width in pixels, `0` if unbound. */
  [[nodiscard]] std::size_t width() const noexcept { return vtbl_ ? vtbl_->width(ctx_) : 0; }

  /** @brief Surface height in pixels, `0` if unbound. */
  [[nodiscard]] std::size_t height() const noexcept { return vtbl_ ? vtbl_->height(ctx_) : 0; }

  /** @brief Fills the whole surface with @p color. No-op if unbound. */
  void clear(rgb_color color) noexcept {
    if (vtbl_) {
      vtbl_->clear(ctx_, color);
    }
  }

  /** @brief Fills the `w x h` rectangle at (@p x, @p y) with @p color, clipped to the surface's bounds
   * (per the bound backend's own clipping, mirroring `framebuffer::fill_rect`). No-op if unbound. */
  void fill_rect(std::size_t x, std::size_t y, std::size_t w, std::size_t h, rgb_color color) noexcept {
    if (vtbl_) {
      vtbl_->fill_rect(ctx_, x, y, w, h, color);
    }
  }

  /** @brief Draws the 1-pixel-wide outline of the `w x h` rectangle at (@p x, @p y), clipped to the
   * surface's bounds. No-op if unbound. */
  void draw_rect(std::size_t x, std::size_t y, std::size_t w, std::size_t h, rgb_color color) noexcept {
    if (vtbl_) {
      vtbl_->draw_rect(ctx_, x, y, w, h, color);
    }
  }

  /** @brief Draws a straight line from (@p x0, @p y0) to (@p x1, @p y1), clipped to the surface's bounds.
   * No-op if unbound. */
  void draw_line(std::ptrdiff_t x0, std::ptrdiff_t y0, std::ptrdiff_t x1, std::ptrdiff_t y1, rgb_color color) noexcept {
    if (vtbl_) {
      vtbl_->draw_line(ctx_, x0, y0, x1, y1, color);
    }
  }

  /** @brief Writes @p color at (@p x, @p y).
   * @return `error::out_of_bounds` if (@p x, @p y) falls outside the surface's bounds;
   * `error::unsupported_operation` if unbound. */
  result<void> put_pixel(std::size_t x, std::size_t y, rgb_color color) noexcept {
    if (!vtbl_) {
      return unexpected(error::unsupported_operation);
    }
    return vtbl_->put_pixel(ctx_, x, y, color);
  }

private:
  template <typename Backend> static std::size_t width_entry(void *ctx) noexcept {
    return gpu_accel_traits<Backend>::width(*static_cast<const Backend *>(ctx));
  }

  template <typename Backend> static std::size_t height_entry(void *ctx) noexcept {
    return gpu_accel_traits<Backend>::height(*static_cast<const Backend *>(ctx));
  }

  template <typename Backend> static void clear_entry(void *ctx, rgb_color c) noexcept {
    gpu_accel_traits<Backend>::clear(*static_cast<Backend *>(ctx), c);
  }

  template <typename Backend>
  static void fill_rect_entry(void *ctx, std::size_t x, std::size_t y, std::size_t w, std::size_t h,
                              rgb_color c) noexcept {
    gpu_accel_traits<Backend>::fill_rect(*static_cast<Backend *>(ctx), x, y, w, h, c);
  }

  template <typename Backend>
  static void draw_rect_entry(void *ctx, std::size_t x, std::size_t y, std::size_t w, std::size_t h,
                              rgb_color c) noexcept {
    using traits = gpu_accel_traits<Backend>;
    auto &backend = *static_cast<Backend *>(ctx);
    if constexpr (detail::gpu_accel_has_draw_rect<traits>::value) {
      traits::draw_rect(backend, x, y, w, h, c);
    } else {
      // Synthesized from draw_line: four edges, exactly matching framebuffer::draw_rect's own shape.
      if (w == 0 || h == 0) {
        return;
      }
      auto line = [&](std::ptrdiff_t x0, std::ptrdiff_t y0, std::ptrdiff_t x1, std::ptrdiff_t y1) {
        traits::draw_line(backend, x0, y0, x1, y1, c);
      };
      auto sx = static_cast<std::ptrdiff_t>(x);
      auto sy = static_cast<std::ptrdiff_t>(y);
      auto sw = static_cast<std::ptrdiff_t>(w);
      auto sh = static_cast<std::ptrdiff_t>(h);
      line(sx, sy, sx + sw - 1, sy);
      line(sx, sy + sh - 1, sx + sw - 1, sy + sh - 1);
      line(sx, sy, sx, sy + sh - 1);
      line(sx + sw - 1, sy, sx + sw - 1, sy + sh - 1);
    }
  }

  template <typename Backend>
  static void draw_line_entry(void *ctx, std::ptrdiff_t x0, std::ptrdiff_t y0, std::ptrdiff_t x1, std::ptrdiff_t y1,
                              rgb_color c) noexcept {
    gpu_accel_traits<Backend>::draw_line(*static_cast<Backend *>(ctx), x0, y0, x1, y1, c);
  }

  template <typename Backend>
  static result<void> put_pixel_entry(void *ctx, std::size_t x, std::size_t y, rgb_color c) noexcept {
    using traits = gpu_accel_traits<Backend>;
    auto &backend = *static_cast<Backend *>(ctx);
    if constexpr (detail::gpu_accel_has_put_pixel<traits>::value) {
      return traits::put_pixel(backend, x, y, c);
    } else {
      // Synthesized from a 1 x 1 fill_rect: bounds-check against width()/height() ourselves first, since
      // fill_rect itself silently clips rather than failing.
      if (x >= traits::width(backend) || y >= traits::height(backend)) {
        return unexpected(error::out_of_bounds);
      }
      traits::fill_rect(backend, x, y, 1, 1, c);
      return {};
    }
  }

  template <typename Backend>
  static constexpr vtable s_vtbl{&width_entry<Backend>,     &height_entry<Backend>,    &clear_entry<Backend>,
                                 &fill_rect_entry<Backend>, &draw_rect_entry<Backend>, &draw_line_entry<Backend>,
                                 &put_pixel_entry<Backend>};

  void *ctx_ = nullptr;
  const vtable *vtbl_ = nullptr;
};

} // namespace structo::hw
