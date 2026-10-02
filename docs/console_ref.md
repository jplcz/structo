<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::console_ref`

`include/structo/hw/console_ref.hpp`

A type-erased, non-owning handle over a character-cell, 16-color text
console -- the text-mode counterpart of
[`uart_ref`](uart_ref.md)/[`timer_ref`](timer_ref.md): a `void *` context
plus a dispatch table built from a `console_traits<Backend>`
specialization, with an unbound default state where every fallible
operation returns `error::unsupported_operation` rather than trapping.

Two concrete backends are provided on top of it:
[`vga_text_console`](vga_text_console.md) (the classic PC VGA/CGA
text-mode framebuffer) and
[`framebuffer_console<PixelFormat>`](framebuffer_console.md) (rasterizes
text onto a [`framebuffer<PixelFormat>`](framebuffer.md)) -- plus a small
VT100/ANSI escape-sequence interpreter, [`vt100_terminal`](vt100.md),
built entirely on top of `console_ref` so it works with either backend
(or a test fake) unchanged.

```cpp
std::byte vga_memory[80 * 25 * 2];
auto backend = structo::hw::vga_text_console::try_create(
    reloco::span<std::byte>(vga_memory, sizeof(vga_memory)), 80, 25);
if (!backend) { /* handle backend.error() */ }

structo::hw::console_ref console(*backend);
console.set_colors(structo::hw::console_color::light_green, structo::hw::console_color::black);
console.write("Hello, console!\n");
```

## Why the cursor lives in `console_ref`, not the backend

Unlike `uart_ref`/`timer_ref`, which are purely stateless two-pointer
forwarders, `console_ref` itself owns the logical cursor position
(`cursor_x()`/`cursor_y()`) and the "current colors" new text is written
with (`foreground()`/`background()`). Every operation that moves the
cursor -- `write_char`, `put`, `write`, `set_cursor` -- updates that
state locally first, then, *only if* the bound backend opted in by
implementing the optional `console_traits::move_cursor`, forwards the
new position to the backend as a side effect (e.g. so `vga_text_console`
can poke real CRTC cursor registers). A backend with no hardware cursor
(`framebuffer_console`, a test fake) simply omits `move_cursor` and
nothing is forwarded.

This is also what lets `vt100_terminal` implement cursor-movement escape
sequences once, generically, regardless of which concrete backend it
drives.

## Customizing: `console_traits<Backend>`

```cpp
template <> struct structo::hw::console_traits<my_backend> {
  // Required:
  static void put_cell(my_backend &, std::size_t x, std::size_t y,
                        char ch, structo::hw::console_color fg, structo::hw::console_color bg) noexcept;
  static std::size_t columns(const my_backend &) noexcept;
  static std::size_t rows(const my_backend &) noexcept;

  // Optional (SFINAE-detected; omit any that don't apply):
  static structo::result<structo::hw::console_cell> get_cell(const my_backend &, std::size_t x, std::size_t y) noexcept;
  static void move_cursor(my_backend &, std::size_t x, std::size_t y) noexcept;
  static void set_cursor_visible(my_backend &, bool visible) noexcept;
};
```

Without `get_cell`, `console_ref::get_char()` always fails with
`error::unsupported_operation`, and `scroll_up()` can still blank the
newly-exposed rows but cannot read back and shift the rows above them
(see `scroll_up`'s docs). Both built-in backends implement `get_cell`.

## API

| Member | Behavior |
|---|---|
| `console_ref()` | Unbound: `columns()`/`rows()` return `0`, every fallible op returns `error::unsupported_operation`. |
| `explicit console_ref(Backend &)` | Binds to an adapted backend (lvalue only -- binding a temporary is a compile error). Resets cursor to `(0, 0)` and colors to `light_gray` on `black`. |
| `columns()`/`rows()` | Console dimensions, `0` if unbound. |
| `foreground()`/`background()`, `set_colors(fg, bg)` | Current colors new text is written with (`write_char`/`put`/`write`). |
| `cursor_x()`/`cursor_y()`, `set_cursor(x, y)` | Logical cursor position, clamped to bounds; `set_cursor` forwards to the optional `move_cursor` trait. |
| `set_cursor_visible(bool)` | Forwards to the optional `set_cursor_visible` trait, or a no-op. |
| `put_char(x, y, ch, fg, bg)` / `get_char(x, y)` | Explicit-coordinate, bounds-checked single-cell write/read (`error::out_of_bounds`). Never touches the cursor. |
| `fill_rect(x, y, w, h, ch, fg, bg)` / `clear(fg, bg)` | Clipped, never-failing multi-cell fill. Never touches the cursor. |
| `scroll_up(lines, fg, bg)` | Moves content up `lines` rows (needs the backend's `get_cell`), blanking the newly-exposed bottom rows. |
| `write_char(ch)` | Core "insert one glyph" primitive: writes at the cursor using the current colors, then advances it with wrap/scroll. No special-case characters. |
| `put(ch)` / `write(text)` | Friendly convenience: interprets `'\n'` as CR+LF, `'\r'`, `'\t'` (next multiple-of-8 column), `'\b'`; everything else goes through `write_char`. |

Note `put`'s friendly `'\n'` handling (CR+LF) is intentionally different
from [`vt100_terminal`](vt100.md)'s strict VT100 line-feed semantics
(down-only, no column reset) -- `vt100_terminal` talks to `console_ref`
through `write_char`/`set_cursor`/`scroll_up` directly, not through
`put`/`write`.

## See also

- [`vga_text_console.md`](vga_text_console.md) -- the VGA/CGA text-mode backend
- [`framebuffer_console.md`](framebuffer_console.md) -- the framebuffer-rasterized-text backend
- [`vt100.md`](vt100.md) -- the VT100/ANSI escape-sequence interpreter built on top
- [`framebuffer.md`](framebuffer.md) -- the pixel-level framebuffer wrapper `framebuffer_console` rasterizes onto
- [`uart_ref.md`](uart_ref.md) -- the type-erasure pattern `console_ref` follows (stateless variant)
