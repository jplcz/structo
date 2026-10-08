<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::bootldr` XMODEM

`include/structo/bootldr/xmodem.hpp`

XMODEM / XMODEM-CRC / XMODEM-1K for loading a kernel image over a serial
line. Two allocation-free, sans-IO state machines (`xmodem_receiver`,
`xmodem_sender`) plus blocking drivers (`receive`, `send`) over
[`hw::uart_ref`](uart_ref.md).

## Receiving an image into RAM (typical boot loader use)

```cpp
#include <structo/bootldr/xmodem.hpp>

using namespace structo;

// UART the host's terminal program (lrzsz `sx -k`, minicom, ...) is attached to.
hw::uart_ref uart(my_uart_backend);

// Destination for the image; XMODEM carries no length, so size it generously.
std::uint8_t *dst = load_address;
std::size_t used = 0;

// Called once per in-order packet (128 or 1024 bytes). The last packet
// is padded with 0x1A (SUB) by the sender -- trim it yourself if the
// image format needs an exact length. Returning false aborts the transfer.
auto sink = [&](reloco::span<const std::uint8_t> payload) {
  if (used + payload.size() > load_capacity)
    return false;                      // image too big: abort (CAN sent)
  std::memcpy(dst + used, payload.data(), payload.size());
  used += payload.size();
  return true;
};

bootldr::xmodem_config cfg;            // defaults: CRC mode, 10 retries, 1K allowed
// Per-byte spin budget for uart.get_byte(); expiry = one protocol timeout.
// This is a coarse clock, so tune it to your CPU (default: 1'000'000 spins).
auto r = bootldr::receive(uart, reloco::function_ref<bool(reloco::span<const std::uint8_t>)>(sink), cfg);
if (!r) { /* io_error, operation_canceled, capacity_exceeded, or a UART error */ }
```

## Driving the engines yourself

Use `xmodem_receiver` / `xmodem_sender` directly when you have a real
timer or an interrupt-driven UART: feed `on_byte(b)` for every received
byte and `on_timeout()` when the peer is silent, and transmit what
`reply()` (receiver) or `packet()` (sender) holds. See the class
documentation in the header for complete loops.

## Behaviour summary

- Receiver opens with `'C'` (CRC-16); after `crc_fallback_after`
  unanswered requests it falls back to the 8-bit checksum (`NAK`).
- Bad CRC/checksum/complement → `NAK`; duplicate block → `ACK` without
  re-delivering; out-of-sequence block → `CAN` (`failed`).
- `max_retries` consecutive errors/timeouts → `CAN` (`failed`).
- Sender picks 1024-byte packets only in CRC mode and when more than 128
  bytes remain; the tail is padded with `SUB`.
- Block numbers wrap modulo 256.
