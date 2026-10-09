<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::uart_input`

`include/structo/hw/uart_input.hpp`

Creates an [`input_device_ref`](input_device_ref.md) backend from any byte stream
([`uart_ref`](uart_ref.md)): a serial port, a character device, a pty. It decodes
xterm-style input into HID key events and mouse events. It is the inverse of
[`console_uart`](console_uart.md).

```cpp
#include <structo/hw/uart_input.hpp>

using namespace structo::hw;

// escape_polls: how many consecutive idle polls a lone ESC byte waits for the rest
// of a sequence before it is reported as the Escape key (the byte stream has no
// timing information, so the timeout is counted in polls). Default 100.
uart_input_config cfg{};
cfg.escape_polls = 50;

// The decoder owns only parser state and a small event queue. Not copyable/movable:
// it must outlive every input_device_ref bound to it.
uart_ref serial(my_uart);
uart_input decoder(serial, cfg);

// Use it like any other input device (polled; no interrupt callback support).
input_device_ref kbd(decoder);
while (auto ev = kbd.read_event(/*max_spins=*/1000)) {
  // ev->type/code/value follow the evdev-style model of input_device_ref.
}
```

## Decoding

| Input | Result |
|-------|--------|
| printable ASCII | key press/release, wrapped in Shift when needed (`A`, `!`, `?`) |
| `0x01`-`0x1A`, `0x00` | Ctrl + letter, Ctrl + Space |
| `\r`, `\n`, `\t`, `0x7F`/`0x08` | Enter, Tab, Backspace |
| `ESC` + char | Alt + key |
| `ESC [ A..D/H/F`, `ESC O A..D/H/F` | arrows, Home, End |
| `ESC [ n ~` | Insert, Delete, PgUp, PgDn, F5-F12 |
| `ESC O P..S`, `ESC [ 1 ; m P..S` | F1-F4 |
| `ESC [ 1 ; m X` | xterm modifier parameter: `m = 1 + (Shift 1, Alt 2, Ctrl 4)` |
| `ESC [ Z` | Shift+Tab |
| `ESC [ < b ; x ; y M/m` | SGR mouse: absolute x/y (0-based), button press/release, wheel |
| lone `ESC` | Escape, after `escape_polls` idle polls |

Each decoded key emits modifier presses, key press, key release, modifier releases
and a `sync` event. Unknown sequences and non-ASCII bytes are dropped. The device
reports `keyboard | tablet` capabilities and `supports_callback == false`.

Mouse reporting must be enabled by the peer terminal (`ESC [ ? 1000 h` and
`ESC [ ? 1006 h`).
