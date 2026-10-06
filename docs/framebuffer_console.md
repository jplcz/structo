<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::framebuffer_console<PixelFormat, Font>`

`include/structo/hw/framebuffer_console.hpp`

A [`console_ref`](console_ref.md)-adaptable backend that rasterizes a
character-cell text console onto a
[`framebuffer<PixelFormat>`](framebuffer.md), one monospace glyph per
cell.

```cpp
std::byte fb_memory[640 * 480 * 4];
auto fb = structo::hw::framebuffer<structo::hw::xrgb8888>::try_create(
    reloco::span<std::byte>(fb_memory, sizeof(fb_memory)), 640, 480, 640 * 4);
if (!fb) { /* handle fb.error() */ }

structo::hw::console_cell cells[(640 / 8) * (480 / 8)]{};
auto console = structo::hw::framebuffer_console<structo::hw::xrgb8888>::try_create(
    *fb, reloco::span<structo::hw::console_cell>(cells, sizeof(cells) / sizeof(cells[0])));
if (!console) { /* handle console.error() */ }

structo::hw::console_ref ref(*console);
ref.write("Hello, framebuffer!\n");
```

## Backing cell storage

Pixels are a write-only medium: once a glyph is rasterized, there is no
way to read the character back out of the framebuffer itself. To still
support `console_ref::get_char()` and -- more importantly --
`console_ref::scroll_up()` (which needs to read a cell's old content to
move it), this backend keeps its own shadow grid of `console_cell`s,
mirroring exactly what has been drawn. Per this library's
no-hidden-allocation convention, that storage is supplied by the caller
as a `reloco::span<console_cell>` of `columns() * rows()` cells, where
`columns()`/`rows()` are the framebuffer's dimensions divided by the
glyph size.

## Fonts

Glyph rendering is pluggable via the `Font` template parameter (default
`block_font_8x8`), a compile-time trait:

```cpp
struct my_font {
  static constexpr std::size_t glyph_width = 8;
  static constexpr std::size_t glyph_height = 8;
  // Returns glyph_height bytes, each the glyph's row as a bitmap:
  // bit 7 (MSB) is the leftmost pixel, bit 0 the rightmost.
  static const std::uint8_t *glyph_bitmap(char ch) noexcept;
};
```

This header ships exactly one built-in `Font`, `block_font_8x8`, which
is an intentionally honest *placeholder*: every non-space, printable
character renders as a solid filled block, not a real glyph shape. This
is deliberate -- shipping a traced bitmap of an actual existing PC font
risks reproducing copyrighted glyph artwork, so this library does not
embed one. Bring your own real `Font` (e.g. from an 8x8/8x16 font ROM
dump you have the rights to use) for legible text; `block_font_8x8` only
proves the rendering pipeline works and is good enough for
cursor/cell-geometry testing.

## API

| Member | Behavior |
|---|---|
| `try_create(fb, cell_storage)` | `error::invalid_argument` if `fb` is too small to fit even one glyph; `error::out_of_range` if `cell_storage` is smaller than `columns() * rows()`. Clears every cell to a space on black. |
| `columns()`/`rows()` | `fb.width() / Font::glyph_width` / `fb.height() / Font::glyph_height`. |
| `pixels()` | The underlying `framebuffer<PixelFormat>` being rasterized onto. |
| `put_cell(x, y, ch, fg, bg)` / `get_cell(x, y)` | Updates the shadow cell and rasterizes the glyph / reads the shadow cell. Used by `console_traits<framebuffer_console<...>>`. |

`console_color_to_rgb(console_color)` maps the 16-color palette to RGB
using the conventional CGA/VGA default palette values; it's also usable
standalone if you need the same colors elsewhere.

`console_traits<framebuffer_console<PixelFormat, Font>>` implements
`put_cell`, `get_cell`, `columns`, `rows` -- no hardware cursor, so
`move_cursor`/`set_cursor_visible` are simply omitted (a software cursor
can be layered on by a caller drawing directly onto `pixels()`).

## See also

- [`console_ref.md`](console_ref.md) -- the type-erased console handle this backend adapts to
- [`framebuffer.md`](framebuffer.md) -- the pixel-level framebuffer this backend rasterizes onto
- [`vga_text_console.md`](vga_text_console.md) -- the VGA/CGA text-mode alternative backend
- [`vt100.md`](vt100.md) -- the VT100/ANSI escape-sequence interpreter that can drive this backend
- [`examples/sdl3_vt100_framebuffer_console_demo.cpp`](../examples/sdl3_vt100_framebuffer_console_demo.cpp) -- renders this backend with a real bitmap font (`examples/fonts/dejavu_sans_mono_8x16_font.hpp`, not `block_font_8x8`) through a real SDL3 window (only built when SDL3 is found)
