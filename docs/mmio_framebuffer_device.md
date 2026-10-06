<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hypervisor::mmio_framebuffer_device<PixelFormat>`

`include/structo/hypervisor/mmio_framebuffer_device.hpp`

An emulated linear-framebuffer graphics device, mirroring `hw::framebuffer`
(see [`framebuffer.md`](framebuffer.md)) into the same "plain-memory
data plane, trapped control plane" split [`mmio_text_console`](mmio_text_console.md)
already establishes for text consoles -- here for raw pixels instead of
character cells.

```cpp
// Owned, guest-mappable framebuffer:
auto maker = structo::hypervisor::mmio_framebuffer_device<structo::hw::xrgb8888>::try_create(640, 480);
auto screen = std::move(maker.value());
structo::hypervisor::mmio_device_ref control(screen);   // the small trapped register window
// map screen.pixels().raw() directly into guest physical memory for the pixel data itself.
screen.pixels().clear({0, 0, 0});
screen.pixels().fill_rect(10, 10, 100, 40, {0, 128, 255});

// Bound to an SDL texture's locked pixels for a host-side demo, no guest involved:
void *locked_pixels; int pitch;
SDL_LockTexture(texture, nullptr, &locked_pixels, &pitch);
auto bound = structo::hypervisor::mmio_framebuffer_device<structo::hw::xrgb8888>::try_bind_external(
    reloco::span<std::byte>(static_cast<std::byte *>(locked_pixels), std::size_t(pitch) * 480), 640, 480,
    std::size_t(pitch));
bound->pixels().draw_line(0, 0, 639, 479, {255, 0, 0});
SDL_UnlockTexture(texture);
```

## Two address ranges, one device (again)

- `pixels()` returns the underlying `hw::framebuffer<PixelFormat>` directly:
  the embedding hypervisor maps its `raw()` bytes straight into the guest's
  physical address space (stage-2/EPT, out of this header's scope), so
  guest pixel writes land in memory with no VM exit, and the *hypervisor*
  can call every `framebuffer` drawing helper (`put_pixel`/`fill_rect`/
  `draw_line`/`blit`/...) directly on the same memory. Unlike
  `mmio_text_console`, there is no separate shadow-cell state to keep in
  sync here: a pixel buffer is both readable and writable in place, so
  one `framebuffer` object already serves both sides.
- The `mmio_device_traits<mmio_framebuffer_device<PixelFormat>>`
  specialization models a small trapped "virtual GPU control" register
  window: read-only `width`/`height`/`stride_bytes`/`bytes_per_pixel`/
  `pixel_format_id` (so a guest driver can size and self-identify the
  surface without any side channel), plus a write-only `present`
  doorbell the guest pokes after finishing a frame -- purely a hint a
  polling hypervisor main loop can check via `take_dirty()` instead of
  re-blitting every pixel on every tick.

## Ownership: two construction modes

- `try_allocate`/`try_create` allocate and own a page-aligned backing
  block via a `reloco::allocator_ref`, exactly like `mmio_text_console`
  (see that header's docs for the full rationale: page alignment so the
  whole block maps cleanly into guest physical memory with no partial
  trailing page, padding zeroed, ownership tracked directly as the
  class's own members rather than recovered indirectly through the
  `framebuffer` it hands out).
- `try_bind_external` instead *wraps* caller-supplied memory this device
  never allocates and never frees -- e.g. an SDL texture's locked pixel
  buffer, already at whatever stride/alignment SDL picked, with an
  optional byte `offset_bytes` into it (for an atlas or a sub-rectangle
  of a larger surface). This is the intended path for plugging a
  `mmio_framebuffer_device` straight into a host renderer for a demo/debug
  UI: construct it over `SDL_LockTexture`'s own buffer, draw guest/hypervisor
  output directly into exactly the pixels SDL is about to present, and
  skip the extra allocate-then-copy step an owned backing block would
  otherwise require. A caller choosing this mode is responsible for the
  external memory's lifetime (keeping it locked/valid) and, if it is
  ever mapped into a guest at all, for that memory's own
  alignment/isolation properties -- this header makes no page-alignment
  guarantee for it, unlike the owned-allocation path.

## `pixel_format_id<PixelFormat>`

A compile-time trait mapping a `hw::framebuffer` `PixelFormat` to a
stable wire-format identifier the control window's
`control_off_pixel_format_id` register reports, so a guest driver can
self-identify the surface's pixel encoding:

| `PixelFormat` | `pixel_format_id<PixelFormat>::value` |
| --- | --- |
| `hw::rgb888` | 1 |
| `hw::bgr888` | 2 |
| `hw::xrgb8888` | 3 |
| `hw::rgba8888` | 4 |
| `hw::rgb565` | 5 |
| `hw::gray8` | 6 |
| any other (caller-supplied) format | `pixel_format_id_custom` (`0xFFFFFFFF`) |

Specialize `pixel_format_id` for a custom `PixelFormat` to report a
project-specific id instead of the generic "custom" fallback.

## API

- `static constexpr std::size_t default_page_size = 4096;`
- `static constexpr std::size_t control_window_size = 24;`
- `control_off_width`/`control_off_height`/`control_off_stride_bytes`/
  `control_off_bytes_per_pixel`/`control_off_pixel_format_id`/
  `control_off_present` -- the control window's 4-byte register offsets.
- `static result<mmio_framebuffer_device> try_allocate(allocator_ref alloc, std::size_t width, std::size_t height, std::size_t page_size = default_page_size) noexcept`
- `static result<mmio_framebuffer_device> try_create(std::size_t width, std::size_t height, std::size_t page_size = default_page_size) noexcept`
- `static result<mmio_framebuffer_device> try_bind_external(span<std::byte> external_memory, std::size_t width, std::size_t height, std::size_t stride_bytes, std::size_t offset_bytes = 0) noexcept`
- `width()`/`height()`/`stride_bytes()`, `pixels()` (mutable and `const`
  overloads, returning `hw::framebuffer<PixelFormat>&`), `owns_allocation()`.
- `take_dirty()` -- reports then clears the present flag; `mark_dirty()`
  -- sets it from the hypervisor side without going through the MMIO
  window.
- `mmio_device_traits<mmio_framebuffer_device<PixelFormat>>`: `size()`
  is `control_window_size`; `try_read` implements every register above
  (4-byte accesses only; reading `present` also acknowledges it, same
  as `take_dirty()`); `try_write` only accepts `control_off_present`
  (every geometry/format register rejects writes with
  `error::permission_denied`).

Move-only (may own an allocation); copying is `delete`d, matching
`mmio_text_console`/`dynamic_bitmap`'s own "clone explicitly if you
really need one" convention -- not that a framebuffer clone is
currently provided; bind a second device over different (owned or
external) memory instead.

## Testing

`tests/test_mmio_framebuffer_device.cpp` covers: rejecting zero
width/height; a default-constructed (storage-less) device being safe to
destroy; moves transferring the owned allocation and leaving the source
empty (including `owns_allocation()`); the owned framebuffer being
page-aligned and at least tightly packed; hypervisor-side `framebuffer`
drawing calls writing directly into the pixel buffer; the control
window reporting geometry and the pixel-format id; geometry registers
being read-only; the `present` doorbell setting the dirty flag via
`try_write`, `take_dirty()` clearing it, and reading the register itself
also acknowledging it; out-of-range control accesses being rejected;
binding over caller-owned external memory without this device ever
freeing it; respecting a byte offset into the external buffer; and
rejecting a too-small external buffer.

[`examples/sdl3_gpu_accel_demo.cpp`](../examples/sdl3_gpu_accel_demo.cpp)
presents this device's owned pixel buffer through a real SDL3 window
every frame (only built when SDL3 is found).

## See also

- [`framebuffer.md`](framebuffer.md) -- the non-owning pixel-buffer
  wrapper and pixel formats this device wraps.
- [`mmio_device_ref.md`](mmio_device_ref.md) -- the type-erased handle
  this device's control window is bound through.
- [`mmio_text_console.md`](mmio_text_console.md) -- the same
  "plain-memory data plane, trapped control plane" split, for a
  character-cell text console instead of raw pixels.
- [`mmio_gpu_command_buffer_device.md`](mmio_gpu_command_buffer_device.md)
  -- a pseudo hardware-accelerated 2D drawing device typically paired
  with this device as its `gpu_accel_ref` rendering target.
- [`reference.md`](reference.md) -- the full per-header API map.
