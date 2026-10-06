<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hypervisor::mmio_gpu_command_buffer_device`

`include/structo/hypervisor/mmio_gpu_command_buffer_device.hpp`

A pseudo hardware-accelerated 2D drawing device: the guest fills a
directly-mapped command buffer with a batch of drawing primitives, then
triggers their synchronous execution with one trapped MMIO write --
exactly like a real GPU's command-buffer/ring submission model, just
executed inline rather than asynchronously by actual silicon.

```cpp
structo::hw::framebuffer<structo::hw::xrgb8888> screen = ...;
structo::hw::gpu_accel_ref target(screen); // or an SDL_Renderer-backed type instead

auto maker = structo::hypervisor::mmio_gpu_command_buffer_device::try_create(target, 64);
auto accel = std::move(maker.value());
structo::hypervisor::mmio_device_ref control(accel); // the small trapped register window
// map accel.command_buffer() directly into guest physical memory for the command data itself.

// Guest driver writes two commands (clear, then fill_rect) into slots 0 and 1 of command_buffer(),
// sets count = 2, then writes anything to the execute register -- synchronously, right here:
(void)control.try_write(mmio_gpu_command_buffer_device::control_off_count, ...);   // count = 2
(void)control.try_write(mmio_gpu_command_buffer_device::control_off_execute, ...); // runs both commands now
```

## Two address ranges, a third time

- `command_buffer()` returns a directly-mapped, allocator-owned,
  page-aligned block of fixed-size command slots: the guest driver
  writes `count()` commands into it with ordinary stores, no VM exit
  per command -- exactly the same "plain-memory data plane" every other
  header in this series ([`mmio_text_console`](mmio_text_console.md),
  [`mmio_framebuffer_device`](mmio_framebuffer_device.md)) already
  establishes, just holding drawing commands instead of cells or pixels
  this time.
- The `mmio_device_traits<mmio_gpu_command_buffer_device>`
  specialization models a tiny trapped "virtual GPU control" register
  window: a read-only `capacity` (how many command slots the buffer
  holds), a read/write `count` (how many of those slots the guest has
  actually filled in since the last execute), and a doorbell `execute`
  register. Writing *any* value to `execute` **synchronously** decodes
  and runs `count()` commands from the buffer against a bound
  `hw::gpu_accel_ref` target -- the trapped write does not return until
  every command has been executed; there is no asynchronous completion/
  interrupt to model. `count()` is reset to `0` immediately afterwards,
  ready for the next batch.

## Commands

Each 24-byte slot decodes (via `decode_gpu_command`) to a `gpu_command`:
a `gpu_command_opcode` plus four signed 32-bit operands (`x0`/`y0`/
`x1`/`y1`, reused as `x`/`y`/`w`/`h` for the rectangle opcodes) and one
packed `0xRRGGBBAA` color.

| Opcode | Operands used | `gpu_accel_ref` call |
| --- | --- | --- |
| `nop` (`0`) | none | nothing -- also what a zeroed/unfilled slot decodes as |
| `clear` (`1`) | color only | `target.clear(color)` |
| `fill_rect` (`2`) | `x0, y0` = x, y; `x1, y1` = w, h | `target.fill_rect(x, y, w, h, color)` |
| `draw_rect` (`3`) | `x0, y0` = x, y; `x1, y1` = w, h | `target.draw_rect(x, y, w, h, color)` |
| `draw_line` (`4`) | `x0, y0, x1, y1` (signed) | `target.draw_line(x0, y0, x1, y1, color)` |
| `put_pixel` (`5`) | `x0, y0` = x, y | `target.put_pixel(x, y, color)` |

A negative operand where a `std::size_t` coordinate/extent is required
(e.g. a negative `x0` for `fill_rect`) makes that single command a
silent no-op (still counted as executed) rather than wrapping around to
a huge unsigned value -- a buggy or malicious guest driver cannot use
this to draw outside the bound target's own clipped bounds.
`draw_line`'s operands are the one exception: they stay signed and are
forwarded as-is, since a line may legitimately start/end offscreen in
any direction (matching `framebuffer::draw_line`'s own clipping).

No endianness conversion is performed anywhere in this header (every
other trapped register in this codebase makes the same choice) --
`decode_gpu_command`/`encode_gpu_command` just `memcpy` each field.

## Backend: `hw::gpu_accel_ref`

This header only *decodes and dispatches* commands; the actual drawing
happens wherever the bound `hw::gpu_accel_ref` (see
[`gpu_accel_ref.md`](gpu_accel_ref.md)) forwards to -- a plain software
`hw::framebuffer<PixelFormat>` by default, or a real GPU-backed
renderer (e.g. an SDL `SDL_Renderer` wrapper specializing
`hw::gpu_accel_traits`) an embedding plugs in instead, with this device
needing no changes either way.

## Ownership

`try_allocate`/`try_create` allocate and own a page-aligned command
buffer via a `reloco::allocator_ref`, following the same rationale
(page alignment, zeroed padding, direct `owned_base_`/`owned_size_`
ownership tracking rather than recovered indirectly through some other
composed view) established by `mmio_text_console`/
`mmio_framebuffer_device` -- see those headers' own docs for the full
background on why this matters.

## API

- `static constexpr std::size_t command_slot_size = 24;`
- `static constexpr std::size_t default_page_size = 4096;`
- `static constexpr std::size_t control_window_size = 12;`
- `control_off_capacity`/`control_off_count`/`control_off_execute` --
  the control window's 4-byte register offsets.
- `static result<mmio_gpu_command_buffer_device> try_allocate(allocator_ref alloc, hw::gpu_accel_ref target, std::size_t capacity, std::size_t page_size = default_page_size) noexcept`
- `static result<mmio_gpu_command_buffer_device> try_create(hw::gpu_accel_ref target, std::size_t capacity, std::size_t page_size = default_page_size) noexcept`
- `capacity()`, `count()`, `last_executed()`, `target()`,
  `command_buffer()` (the directly-mapped span).
- `result<void> set_count(std::size_t n) noexcept` -- `error::out_of_range`
  if `n > capacity()`.
- `std::size_t execute_pending() noexcept` -- synchronously runs
  `count()` commands, resets `count()` to `0`, returns how many ran.
- `void reset_queue() noexcept` -- discards staged commands without
  running them.
- Free functions: `decode_gpu_command`/`encode_gpu_command` (wire
  format <-> `gpu_command`), `pack_gpu_color`/`unpack_gpu_color`
  (`rgb_color` <-> the wire color), `execute_gpu_command` (dispatches
  one decoded command to a `gpu_accel_ref`).
- `mmio_device_traits<mmio_gpu_command_buffer_device>`: `capacity` is
  read-only; `count` is read/write (write validates against
  `capacity()`); `execute` is write-triggered (any value) and also
  readable (reports `last_executed()`); `is_available()` reflects
  whether the bound target is itself bound; `try_reset()` discards
  pending commands via `reset_queue()`.

Move-only (owns an allocation); copying is `delete`d, matching every
other owning device in this series.

## Testing

`tests/test_mmio_gpu_command_buffer_device.cpp` covers: rejecting zero
capacity; a default-constructed (storage-less) device being safe to
destroy; moves transferring the owned allocation; the command buffer
being page-aligned; `execute_pending()` running staged commands
directly against a bound framebuffer; a zeroed/`nop` slot being a safe
no-op; negative rectangle operands being silently ignored rather than
wrapped; `draw_line` allowing offscreen endpoints; `set_count`
rejecting values above capacity; `reset_queue` discarding pending
commands; the control window reporting `capacity`/`count`/
`last_executed` and actually executing on a write to `execute`;
`capacity` being read-only; an out-of-range `count` write being
rejected; an out-of-range control offset being rejected; `try_reset()`
discarding pending commands through the MMIO window; `is_available()`
reflecting the bound target; and `decode_gpu_command`/
`encode_gpu_command`/`pack_gpu_color`/`unpack_gpu_color` round-tripping.

## See also

- [`gpu_accel_ref.md`](gpu_accel_ref.md) -- the type-erased drawing
  backend every decoded command is dispatched through.
- [`mmio_device_ref.md`](mmio_device_ref.md) -- the type-erased handle
  this device's control window is bound through.
- [`mmio_framebuffer_device.md`](mmio_framebuffer_device.md) -- the
  plain pixel-buffer sibling this device is typically paired with as
  its `gpu_accel_ref` target.
- [`mmio_text_console.md`](mmio_text_console.md) -- the same owned,
  page-aligned, directly-mapped allocation pattern, for text cells
  instead of commands.
- [`reference.md`](reference.md) -- the full per-header API map.
