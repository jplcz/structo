<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::framebuffer<PixelFormat>`

`include/structo/hw/framebuffer.hpp`

A simple, non-owning wrapper over a linear pixel buffer (an MMIO-mapped
graphics adapter, a simulator's backing store, a software scanout
surface, ...), plus a handful of pixel formats and basic 2D rendering
helpers built on top of them -- `put_pixel`/`get_pixel`,
`fill_rect`/`clear`, `draw_hline`/`draw_vline`/`draw_rect`/`draw_line`,
and `blit`.

Unlike [`uart_ref`](uart_ref.md)/[`timer_ref`](timer_ref.md)/
[`hw_rng_ref`](hw_rng.md), `framebuffer` is not type-erased over a
backend: real framebuffer hardware (a BAR-mapped linear display surface,
a devicetree `simple-framebuffer`, a VGA/VESA mode, a hypervisor's
virtio-gpu scanout) is overwhelmingly just a flat run of pixels at some
stride -- what differs between devices is the *pixel encoding*, not an
operation set a backend needs to implement. That single axis of
variation is exactly what `PixelFormat` captures; everything else
(bounds checking, rect filling, line drawing, blitting) is identical
regardless of format and is implemented once, on top of it.

```cpp
std::byte storage[640 * 480 * 4];
auto fb = structo::hw::framebuffer<structo::hw::xrgb8888>::try_create(
    reloco::span<std::byte>(storage, sizeof(storage)), 640, 480, 640 * 4);
if (!fb) { /* handle fb.error() */ }

fb->clear({0, 0, 0});                               // black background
fb->fill_rect(10, 10, 100, 40, {0, 128, 255});       // a blue-ish panel
fb->draw_rect(10, 10, 100, 40, {255, 255, 255});     // white outline
fb->draw_line(0, 0, 639, 479, {255, 0, 0});          // a red diagonal
(void)fb->put_pixel(320, 240, {0, 255, 0});          // bounds-checked
```

## Pixel formats

A `PixelFormat` is a stateless struct with:

```cpp
static constexpr std::size_t bytes_per_pixel = ...;
static void encode(std::byte *dst, structo::hw::rgb_color c) noexcept;
static structo::hw::rgb_color decode(const std::byte *src) noexcept;
```

Six are provided:

| Format | Bytes/pixel | Layout |
|---|---|---|
| `rgb888` | 3 | R, G, B |
| `bgr888` | 3 | B, G, R |
| `xrgb8888` | 4 | padding, R, G, B |
| `rgba8888` | 4 | R, G, B, A |
| `rgb565` | 2 | packed 5-6-5 bits |
| `gray8` | 1 | luminance only (lossy `encode`) |

A caller can supply its own `PixelFormat` for anything else (e.g. a
packed `rgb555`, an indexed palette format) without needing to touch
this header.

`rgb_color` is a plain `{r, g, b, a}` 8-bit-per-channel struct,
independent of any particular `PixelFormat`'s in-memory encoding, with
`black()`/`white()`/`red()`/`green()`/`blue()` convenience constants.

## API

- `framebuffer<PixelFormat>::try_create(span<std::byte> buffer, width, height, stride_bytes)`
  binds a framebuffer to `buffer`, interpreting it as `height` rows of
  `width` pixels each, `stride_bytes` apart (allowing row padding, e.g.
  a scanout surface wider than its visible area). `buffer` must outlive
  the framebuffer. Fails with `error::invalid_argument` if `width`/
  `height` are zero or `stride_bytes` is too small for `width`;
  `error::out_of_range` if `buffer` is smaller than `stride_bytes * height`.
- `width()`/`height()`/`stride_bytes()`/`raw()` report geometry and the
  bound backing buffer.
- `put_pixel(x, y, color)`/`get_pixel(x, y)` are bounds-checked,
  failing with `error::out_of_bounds` if `(x, y)` falls outside
  `[0, width()) x [0, height())`.
- `fill_rect`/`clear`/`draw_hline`/`draw_vline`/`draw_rect`/`draw_line`/
  `blit` are clipped instead: each silently restricts itself to
  whatever portion actually falls within the framebuffer's bounds and
  never fails, matching how ordinary 2D graphics APIs treat partially-
  or fully-offscreen shapes.
- `draw_line` takes signed coordinates (`std::ptrdiff_t`) via
  Bresenham's algorithm, so a line may start/end offscreen in any
  direction.
- `blit(dst_x, dst_y, src, src_x, src_y, w, h)` copies a rectangle from
  another framebuffer `src` (which may use a *different* `PixelFormat`)
  into this one, clipped to both framebuffers' bounds; each pixel is
  decoded from `src` and re-encoded into the destination rather than
  copied byte-for-byte.

See also: [`uart_ref.md`](uart_ref.md), [`timer_ref.md`](timer_ref.md).
