<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::slip_device`

`include/structo/hw/slip_device.hpp`

SLIP (RFC 1055) framing over a [`uart_ref`](uart_ref.md): an
allocation-free, sans-IO codec (`slip_encode`, `slip_decoder`) and
`slip_device<Mtu>`, a *polled* network backend usable through
`polled_net_device` and [`net_device_ref`](net_device_ref.md).

SLIP has no link-layer address (no `mac_address`) and no link negotiation
(`link_up()` is always `true`).

> **C++ standard:** C++20 only (part of the coroutine network stack); the
> header is empty when `RELOCO_HAS_COROUTINES` is 0.

## Wire format

Frames end with `END` (`0xC0`). `END`/`ESC` inside data are sent as
`ESC ESC_END` / `ESC ESC_ESC`. A frame is also *started* with `END`, so line
noise before it is flushed as a dropped garbage frame. The decoder ignores
empty frames (back-to-back `END`s). Constants: `slip_end` `0xC0`, `slip_esc`
`0xDB`, `slip_esc_end` `0xDC`, `slip_esc_esc` `0xDD`.

## Usage

```cpp
structo::hw::uart_ref uart{my_uart};          // the serial line; must outlive the device
// Mtu = largest frame payload carried (RFC 1055 suggests 1006). The device
// owns an Mtu-byte RX buffer and a worst-case TX buffer, so no allocation.
structo::hw::slip_device<1006> slip{uart};
// Adds coroutine send/receive on top of the polled try_send/try_receive.
structo::hw::polled_net_device<decltype(slip)> pnd{slip};
structo::hw::net_device_ref nic{pnd};         // what the IP stack consumes

for (;;) {
  slip.service();                             // keep the UART TX FIFO fed even if no coroutine waits
  pnd.poll();                                 // retry parked coroutines
}

// Direct, non-coroutine use:
std::array<std::uint8_t, 1006> rx;
auto sent = slip.try_send(frame);             // try_again while the previous frame still drains;
                                              // out_of_range if frame.size() > Mtu
auto got = slip.try_receive(rx);              // frame length; try_again if none yet;
                                              // out_of_range (frame dropped) if rx is too small
```

## Codec

```cpp
std::array<std::uint8_t, structo::hw::slip_max_encoded_size(100)> out; // worst case: payload*2 + 2
auto n = structo::hw::slip_encode(payload, out);   // result<size_t> bytes written; out_of_range if too small
std::size_t exact = structo::hw::slip_encoded_size(payload); // exact size incl. both ENDs

std::array<std::uint8_t, 1006> buf;           // decoder writes frames here (caller-supplied)
structo::hw::slip_decoder dec{buf};
for (std::uint8_t byte : incoming) {
  if (dec.push(byte)) {                       // true when a frame completed
    auto f = dec.frame();                     // valid until the next push()
  }
}
std::size_t bad = dec.dropped();              // bad escapes or oversize frames, dropped at their END
dec.reset();                                  // abandon any partial frame
```

## `slip_device<Mtu = 1006>` API

Non-copyable.

| Member | Description |
| --- | --- |
| `explicit slip_device(uart_ref)` | Binds the UART. |
| `mtu()` | `Mtu`. |
| `link_up()` | Always `true`. |
| `service()` | Moves queued TX bytes to the UART while it accepts them; never blocks. |
| `try_send(frame)` | Encodes and queues one frame (one at a time), then calls `service()`. |
| `try_receive(dst)` | Calls `service()`, drains the UART RX FIFO into the decoder and copies out one frame. |
| `rx_dropped()` | Malformed/oversized received frames discarded. |
