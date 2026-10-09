<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::console_uart`

`include/structo/hw/console_uart.hpp`

A bridge presenting a keyboard ([`input_device_ref`](input_device_ref.md)) plus a
VT100 text console ([`console_ref`](console_ref.md) driven by `vt100_terminal`) as
a polled [`uart_ref`](uart_ref.md). Anything written against `uart_ref` -- the
[bootloader shell](bootloader_shell.md), prompts, the text editor -- then runs on a
machine that has only a keyboard and a screen.

```cpp
#include <structo/hw/console_uart.hpp>

using namespace structo::hw;

// Both adapters are supplied by the caller: the keyboard backend needs an
// input_traits<> specialization, the screen a console_traits<> one
// (vga_text_console and framebuffer_console already have it).
input_device_ref kbd(my_keyboard);
console_ref screen(my_vga);

// The bridge owns only translation state (modifiers, pending bytes, last TX byte).
// Not copyable/movable: it must outlive every uart_ref bound to it.
console_uart bridge(kbd, screen);

// Drop-in UART: hand it to the shell exactly like a serial port.
uart_ref uart(bridge);
structo::bootldr::shell sh{sched, uart};
```

## TX: bytes to screen

Bytes go through the VT100 interpreter, so ANSI colors/cursor/erase sequences work
as on a serial terminal. VT100 `'\n'` only moves down a row, so the bridge expands a
lone `'\n'` to `"\r\n"`; a `'\n'` that directly follows `'\r'` (what
`uart_ref::write_string` emits) passes unchanged, so lines are never `"\r\r\n"`.

## RX: key events to bytes

Presses and auto-repeats are translated as a serial terminal would send them;
releases only update modifier state (Shift, Ctrl, Caps Lock -- the latter mirrored to
the keyboard LED when supported).

| Key                      | Bytes                                          |
|--------------------------|------------------------------------------------|
| printable (US layout)    | the character, Shift/Caps applied              |
| Ctrl + letter            | `0x01`..`0x1A` (Ctrl-C `0x03`, Ctrl-U `0x15`)  |
| Enter                    | `'\r'`                                         |
| Backspace                | `0x7F`                                         |
| Tab / Escape             | `'\t'` / `0x1B`                                |
| arrows, Home, End        | `ESC [ A/B/C/D`, `ESC [ H`, `ESC [ F`; `ESC O x` once the program enabled application cursor keys (`CSI ? 1 h`) |
| Insert/Delete/PgUp/PgDn  | `ESC [ 2~`, `ESC [ 3~`, `ESC [ 5~`, `ESC [ 6~` |
| F1..F4                   | `ESC O P`, `ESC O Q`, `ESC O R`, `ESC O S`     |
| F5..F12                  | `ESC [ 15~`, `17~`, `18~`, `19~`, `20~`, `21~`, `23~`, `24~` (xterm/VT220) |

Modifier combinations with F-keys are not encoded. Other keys (modifiers alone, keypad) and all non-key events produce nothing. Input is pulled by polling the
input device (`rx_ready`/`try_get_byte`), so it works with or without that device's
interrupt callback. `configure` accepts and remembers any settings (there is no baud
rate); `tx_ready` is always true.

## Mouse

```cpp
// Pointer events from the same input device become xterm mouse reports, but only while the program
// asked for them (it sends CSI ? 1000 h / 1002 h / 1003 h; CSI ? 1006 h selects the SGR encoding).
// Positions are CHARACTER CELLS (0-based), not pixels: a pointing backend converts before queuing.
//   abs x / abs y   pointer position in cells        rel x / rel y   move by cells (clamped to the screen)
//   button          left / middle / right press and release
//   rel wheel       positive = up (report 64), negative = down (65)
//   sync            ends a group; motion is reported here (1002: only while a button is held; 1003: always)
queue.push_back(make_abs_event(input_axis::x, 10));  // column 10
queue.push_back(make_abs_event(input_axis::y, 4));   // row 4
queue.push_back(make_button_event(input_button::left, true));
queue.push_back(make_sync_event());
// With 1000 + 1006 enabled the program reads: ESC [ < 0 ; 11 ; 5 M   (button 0 pressed at column 11, row 5, 1-based)
```

`vt100_terminal::mouse_tracking()` / `mouse_sgr()` expose the requested mode. Shift/Ctrl held on the
keyboard are added to the report's modifier bits; legacy (non-SGR) reports are limited to 223 columns/rows.

## Demo

`examples/sdl3_shell_terminal_demo.cpp` wires an SDL3 keyboard (`input_traits`), a
`framebuffer_console` and this bridge into a graphical terminal emulator (keyboard, mouse buttons, motion and wheel) that spawns
`$SHELL` on a pty (`TERM=xterm` unless `STRUCTO_TERM` is set). Built when SDL3 is
found (`ninja sdl3_shell_terminal_demo`). Shell output is read by a `reloco::thread` blocked in
`poll(2)` (reloco has no poll wrapper) and handed to the UI thread through a
`reloco::heap_spsc_ring_buffer`; the thread wakes the SDL event loop with a user event.
