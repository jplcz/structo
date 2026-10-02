<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::vga_text_console`

`include/structo/hw/vga_text_console.hpp`

A [`console_ref`](console_ref.md)-adaptable backend for the classic PC
VGA/CGA text-mode framebuffer: 2 bytes per cell (an ASCII/CP437 code
point byte followed by a packed attribute byte -- bits 3-0 foreground
color, bits 6-4 background color).

This header only wraps the memory-mapped text buffer itself (e.g. the
identity/linear-mapped `0xB8000` VGA text segment); it has no dependency
on actual I/O port access. Forwarding the logical cursor position to the
real CRTC index/data registers (ports `0x3D4`/`0x3D5`) is left to an
optional, caller-supplied `reloco::function_ref` callback, so this
header stays portable/testable without an `io_space_ref`/port-I/O
dependency -- a caller targeting real hardware plugs that callback in,
while a caller only emulating a text buffer (or a test) can omit it.

```cpp
std::byte vga_memory[80 * 25 * 2];
auto console = structo::hw::vga_text_console::try_create(
    reloco::span<std::byte>(vga_memory, sizeof(vga_memory)), 80, 25);
if (!console) { /* handle console.error() */ }

structo::hw::console_ref ref(*console);
ref.write("Hello, VGA!\n");
```

With a hardware cursor callback:

```cpp
auto console = structo::hw::vga_text_console::try_create(
    reloco::span<std::byte>(vga_memory, sizeof(vga_memory)), 80, 25,
    structo::hw::vga_text_console::cursor_sink([](std::size_t x, std::size_t y) noexcept {
      // forward (x, y) to real CRTC registers 0x3D4/0x3D5 here
    }));
```

## API

| Member | Behavior |
|---|---|
| `try_create(buffer, columns, rows, on_move_cursor = {})` | `error::invalid_argument` if `columns`/`rows` are zero; `error::out_of_range` if `buffer` is smaller than `columns * rows * 2` bytes. |
| `columns()`/`rows()` | Console dimensions. |
| `raw()` | The bound `reloco::span<std::byte>` backing buffer. |
| `put_cell(x, y, ch, fg, bg)` / `get_cell(x, y)` | Direct, unchecked cell access (used by `console_traits<vga_text_console>`; normally reached only through the bounds-checked `console_ref`). |
| `move_cursor(x, y)` | Forwards to the optional `cursor_sink` callback, or a no-op. |

Background colors are masked to the low 3 bits (`0-7`) per the standard
VGA attribute-byte convention, which reserves bit 7 for either a
bright-background or a blink flag depending on the controller's
mode-control register.

`console_traits<vga_text_console>` implements all of `put_cell`,
`get_cell`, `columns`, `rows`, and `move_cursor` (VGA text memory is
readable, so [`console_ref::scroll_up`](console_ref.md) works fully).

## See also

- [`console_ref.md`](console_ref.md) -- the type-erased console handle this backend adapts to
- [`framebuffer_console.md`](framebuffer_console.md) -- the pixel-rasterized alternative backend
- [`vt100.md`](vt100.md) -- the VT100/ANSI escape-sequence interpreter that can drive this backend
