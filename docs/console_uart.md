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
| arrows, Home, End        | `ESC [ A/B/C/D`, `ESC [ H`, `ESC [ F`          |
| Insert/Delete/PgUp/PgDn  | `ESC [ 2~`, `ESC [ 3~`, `ESC [ 5~`, `ESC [ 6~` |
| F1..F4                   | `ESC O P`, `ESC O Q`, `ESC O R`, `ESC O S`     |
| F5..F12                  | `ESC [ 15~`, `17~`, `18~`, `19~`, `20~`, `21~`, `23~`, `24~` (xterm/VT220) |

Modifier combinations with F-keys are not encoded. Other keys (modifiers alone, keypad) and all non-key events produce nothing. Input is pulled by polling the
input device (`rx_ready`/`try_get_byte`), so it works with or without that device's
interrupt callback. `configure` accepts and remembers any settings (there is no baud
rate); `tx_ready` is always true.

## Demo

`examples/sdl3_shell_terminal_demo.cpp` wires an SDL3 keyboard (`input_traits`), a
`framebuffer_console` and this bridge into a graphical terminal emulator that spawns
`$SHELL` on a pty (`TERM=vt100` unless `STRUCTO_TERM` is set). Built when SDL3 is
found (`ninja sdl3_shell_terminal_demo`).
