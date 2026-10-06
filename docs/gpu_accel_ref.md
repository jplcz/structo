<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::gpu_accel_ref`

`include/structo/hw/gpu_accel_ref.hpp`

A type-erased, non-owning handle over a 2D drawing backend -- a
software `hw::framebuffer<PixelFormat>` (see [`framebuffer.md`](framebuffer.md))
by default, or a real hardware-accelerated renderer (e.g. an SDL
`SDL_Renderer` wrapper) an embedding plugs in instead -- plus the
`gpu_accel_traits<Backend>` customization point a concrete backend
specializes to be bindable through it.

```cpp
structo::hw::framebuffer<structo::hw::xrgb8888> fb = ...;
structo::hw::gpu_accel_ref gpu(fb);     // software backend, forwards directly
gpu.clear({0, 0, 0});
gpu.fill_rect(10, 10, 100, 40, {0, 128, 255});
(void)gpu.put_pixel(320, 240, {255, 255, 255}); // synthesized from fill_rect
```

## Why this exists alongside `hw::framebuffer`

`framebuffer<PixelFormat>` already has `put_pixel`/`fill_rect`/
`draw_rect`/`draw_line`/`clear` -- but it is a concrete class template
over a pixel *encoding*, always drawing into plain memory this process
owns. `gpu_accel_ref` is the same small drawing vocabulary, but
type-erased over the *drawing implementation* rather than the pixel
format: a backend bound through it may draw into plain memory exactly
like `framebuffer` does (the default `gpu_accel_traits<framebuffer<PixelFormat>>`
specialization simply forwards every call), or it may translate each
call into real GPU work -- `SDL_RenderFillRect`/`SDL_RenderDrawLine`/
`SDL_RenderClear` against an `SDL_Renderer`, an OpenGL/Vulkan
immediate-mode shim, or anything else a host application wants to plug
in. `structo` itself only ever ships the software/`framebuffer`
backend; the point of the indirection is letting
[`mmio_gpu_command_buffer_device`](mmio_gpu_command_buffer_device.md)
stay completely unaware of which of those a given embedding chose.

## Customizing: `gpu_accel_traits<Backend>`

A specialization must supply:

```cpp
template <> struct structo::hw::gpu_accel_traits<my_backend> {
  static std::size_t width(const my_backend &) noexcept;
  static std::size_t height(const my_backend &) noexcept;
  static void clear(my_backend &, rgb_color) noexcept;
  static void fill_rect(my_backend &, std::size_t x, std::size_t y, std::size_t w, std::size_t h,
                        rgb_color) noexcept;
  static void draw_line(my_backend &, std::ptrdiff_t x0, std::ptrdiff_t y0, std::ptrdiff_t x1,
                       std::ptrdiff_t y1, rgb_color) noexcept;
};
```

Optionally, also `draw_rect(Backend &, x, y, w, h, rgb_color)` and
`put_pixel(Backend &, x, y, rgb_color) -> result<void>` -- detected via
SFINAE, the same optional-member idiom `console_traits`'s
`move_cursor`/`set_cursor_visible` use. Without them, `gpu_accel_ref`
synthesizes `draw_rect` from four `draw_line` calls and `put_pixel`
from a `1 x 1` `fill_rect`, so a minimal backend (just `clear`/
`fill_rect`/`draw_line`, e.g. the handful of primitives an
`SDL_Renderer` already has a direct call for) gets every operation
`gpu_accel_ref` exposes for free.

## API

- `struct vtable { ... }` -- the fixed, per-bound-backend-type dispatch
  table, following every other `structo` `*_ref` handle's shape.
- `gpu_accel_ref()` -- constructs an unbound ref.
- `explicit gpu_accel_ref(Backend &)` -- binds to an adapted backend
  (must have a `gpu_accel_traits` specialization); rejects rvalue/
  temporary bindings.
- `explicit operator bool() const` -- whether bound.
- `width()`/`height()` -- `0` if unbound.
- `clear(rgb_color)`, `fill_rect(x, y, w, h, rgb_color)`,
  `draw_rect(x, y, w, h, rgb_color)`, `draw_line(x0, y0, x1, y1, rgb_color)`
  -- all silently no-op if unbound.
- `put_pixel(x, y, rgb_color) -> result<void>` -- fails with
  `error::unsupported_operation` if unbound, `error::out_of_bounds` if
  `(x, y)` falls outside the surface.

## Testing

`tests/test_gpu_accel_ref.cpp` covers: an unbound ref being safe (every
drawing call a no-op, `put_pixel` failing cleanly); forwarding every
operation directly to a bound `framebuffer` backend and observing the
expected pixels; and a minimal backend implementing only the mandatory
members correctly receiving synthesized `draw_rect`/`put_pixel` calls.

## See also

- [`framebuffer.md`](framebuffer.md) -- the concrete, non-type-erased
  pixel-buffer wrapper `gpu_accel_traits<framebuffer<PixelFormat>>`
  forwards to by default.
- [`console_ref.md`](console_ref.md) -- the same type-erased,
  customizable-backend shape, for character-cell text consoles instead
  of pixels.
- [`mmio_gpu_command_buffer_device.md`](mmio_gpu_command_buffer_device.md)
  -- the emulated MMIO device decoding and dispatching guest-submitted
  drawing commands through a bound `gpu_accel_ref`.
- [`reference.md`](reference.md) -- the full per-header API map.
