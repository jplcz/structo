<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hypervisor::mmio_text_console`

`include/structo/hypervisor/mmio_text_console.hpp`

An emulated VGA-like text console for a guest, split exactly the way
real VGA text mode is: a plain-memory cell buffer (the "framebuffer")
the guest writes glyphs into directly, with no VM exit per character,
plus a tiny trapped MMIO register window (the "virtual H/W control"
space) for CRTC-style cursor/geometry control.

```cpp
auto maker = structo::hypervisor::mmio_text_console::try_create(80, 25);
if (!maker) { panic("out of memory sizing the virtual console"); }
auto screen = std::move(maker.value());

// Guest-facing side: map screen.framebuffer() directly into guest physical memory, and bind the small
// control window to a VM-exit handler:
structo::hypervisor::mmio_device_ref control(screen);
// control.size() == structo::hypervisor::mmio_text_console::control_window_size

// Hypervisor-facing side: render straight onto the same screen.
structo::hw::console_ref console(screen);
console.write("booting guest...\n");
```

## Two address ranges, one device

Real VGA text mode never traps a single character write: the `0xB8000`
text segment is a plain, directly-mapped memory window (2 bytes/cell,
see `hw::vga_text_console`), and only the *cursor* is programmed
through a separate, much smaller I/O port pair (CRTC index/data, ports
`0x3D4`/`0x3D5`). `mmio_text_console` mirrors that split:

- `framebuffer()` returns the raw, allocator-owned cell buffer. The
  embedding hypervisor maps this span directly into the guest's
  physical address space (stage-2/EPT, out of this header's scope); every
  guest write lands straight in memory, no VM exit.
- The `mmio_device_traits<mmio_text_console>` specialization models the
  *separate*, much smaller "virtual CRTC" register window
  (`control_window_size` bytes: read-only `columns`/`rows`, read-write
  `cursor_x`/`cursor_y`/`cursor_visible`, each a 4-byte register) that
  *is* meant to be bound to an `mmio_device_ref` and trapped.

## Ownership: the console, and only the console, owns the allocation

`mmio_text_console` allocates its framebuffer via a `reloco::allocator_ref`
at construction (`try_allocate`/`try_create`) and frees it in its
destructor. The owning pointer and the exact size to hand back to
`deallocate()` (the allocator's own *absorbed* size -- see
`reloco/docs/allocator-capacity-absorption.md` -- not necessarily the
originally requested byte count) are recorded directly as the class's
own members, never recovered indirectly through the `hw::vga_text_console`
it composes on top of for cell read/write logic: that inner object only
ever holds a **non-owning view** over a leading sub-span of the owned
block. Two objects each believing they might be responsible for freeing
the same pointer is exactly the kind of double-free/use-after-free
hazard an owning type must not create for itself, so there is exactly
one place a free can happen.

## Page alignment: why, not just how much

The framebuffer allocation's base address and byte size are both
rounded/aligned up to a whole number of `page_size` bytes (default
`default_page_size == 4096`, the smallest granule every mainstream
architecture supports). This is a correctness requirement, not just
convenience: since `framebuffer()` is meant to be mapped wholesale into
guest physical memory, an allocation that is *not* a whole number of
pages would force the embedder to either map a partial trailing page
(exposing whatever unrelated heap memory happens to follow the
allocation to the guest -- an information-disclosure bug) or refuse to
map the console at all. Any padding bytes between the live cell data
(`columns() * rows() * 2`) and the end of the page-rounded allocation
are explicitly zeroed at allocation time, so a guest reading past its
own last cell never observes stale allocator memory either.

## Also a `console_traits` backend: the hypervisor's own window

Because the hypervisor itself may want to render directly onto this
same screen (early boot diagnostics before the guest OS takes over the
console, a host-side debug overlay, ...), `mmio_text_console` also
specializes `hw::console_traits`, so a `hw::console_ref` bound to it
works exactly like one bound to `hw::vga_text_console` -- writing
through it updates the very same cell buffer the guest's directly-mapped
framebuffer exposes, and moving the logical cursor updates the very
same `cursor_x`/`cursor_y` the guest's MMIO control window reports.
There is exactly one cursor, shared by both views, precisely as real
VGA hardware only has one.

## API

- `static constexpr std::size_t default_page_size = 4096;`
- `static constexpr std::size_t control_window_size = 20;` -- the
  "virtual CRTC" register window's size, entirely separate from
  `framebuffer()`'s own (much larger) size.
- `control_off_columns`/`control_off_rows`/`control_off_cursor_x`/
  `control_off_cursor_y`/`control_off_cursor_visible` -- the control
  window's 4-byte register offsets.
- `static result<mmio_text_console> try_allocate(allocator_ref alloc, std::size_t columns, std::size_t rows, std::size_t page_size = default_page_size) noexcept`
- `static result<mmio_text_console> try_create(std::size_t columns, std::size_t rows, std::size_t page_size = default_page_size) noexcept` --
  `try_allocate()` using `reloco::default_allocator()`.
- `columns()`/`rows()`, `framebuffer()`, `cursor_x()`/`cursor_y()`/`cursor_visible()`,
  `put_cell()`/`get_cell()`, `move_cursor()`/`set_cursor_visible()`.
- `mmio_device_traits<mmio_text_console>`: `size()` is `control_window_size`;
  `try_read`/`try_write` implement the register window above (4-byte
  accesses only; `columns`/`rows` reject writes with
  `error::permission_denied`; out-of-range/misaligned-width accesses
  get `error::invalid_argument`/`error::out_of_range` as usual from
  `mmio_device_ref`'s own bounds checking).
- `hw::console_traits<mmio_text_console>`: full support, including
  `move_cursor`/`set_cursor_visible`.

Move-only (owns the allocation); copying is `delete`d, matching
`dynamic_bitmap`'s own "clone explicitly if you really need one"
convention.

## Testing

`tests/test_mmio_text_console.cpp` covers: rejecting zero
columns/rows; a default-constructed (storage-less) console being safe
to destroy; moves transferring the owned allocation and leaving the
source empty; the framebuffer being page-aligned and at least
`columns * rows * 2` bytes; `console_ref` writes landing in the cell
buffer; the control window's size matching `mmio_device_ref::size()`;
`columns`/`rows` registers being read-only; cursor registers
round-tripping and clamping to the grid; the cursor-visible register
round-tripping; the cursor being shared between the hypervisor's
`console_ref` view and the guest's MMIO register view; wrong-width and
out-of-range control accesses being rejected; and all page padding past
the live cell data being zeroed.

## See also

- [`mmio_device_ref.md`](mmio_device_ref.md) -- the type-erased handle
  this device's control window is bound through.
- [`mmio_print_device.md`](mmio_print_device.md) -- a much simpler
  sibling example (one write-only byte, no allocation).
- [`console_ref.md`](console_ref.md) -- the trait this console also
  adapts to, for the hypervisor's own rendering.
- [`reference.md`](reference.md) -- the full per-header API map.
