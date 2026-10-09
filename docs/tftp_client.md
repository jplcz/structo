<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::net::tftp_client`

`include/structo/net/tftp_client.hpp`

> **Warning:** the network stack is **not meant for production devices** (no authentication, encryption or hardening against hostile peers). See [tftp_over_slip.md](tftp_over_slip.md).

A TFTP client for `ipv4_node` (`net/ipv4_node.hpp`): a sans-IO state machine
(`tftp_client`) plus a coroutine helper (`tftp_send_due`) that transmits its
packets. One transfer at a time, octet mode, lock-step (RFC 1350), 512-byte
blocks. C++20 only. The packet codec is in `net/tftp.hpp`; UDP framing in
[`udp.md`](udp.md), addressing in [`ipv4.md`](ipv4.md). For a complete stack
see [tftp_over_slip.md](tftp_over_slip.md), [netstack_bootloader.md](netstack_bootloader.md)
and [bootloader_tftp_boot.md](bootloader_tftp_boot.md). The sibling
[`dhcp_client.md`](dhcp_client.md) follows the same pattern.

Like the DHCP client it has no timer: the caller supplies a monotonic
millisecond clock. The last packet is retransmitted after 1 s of silence, up
to 5 times, then the transfer fails and `timed_out()` is true.

- **Reading:** `start_read()`, feed packets to `handle()`; on
  `tftp_event::data` consume `data()` before the next `handle()`;
  `finished()` becomes true once the last (short) block arrived.
- **Writing:** `start_write()`; on `tftp_event::need_data` call `supply()`
  with the next block (up to 512 bytes; a shorter block, possibly empty,
  ends the transfer); `tftp_event::done` means the server acknowledged
  everything.

## Usage

```cpp
#include <structo/net/tftp_client.hpp>

using namespace structo::net;

// IPv4 carrier with a static address (192.168.7.2); 1006 is the link MTU.
ipv4_node<1006> ip{nic, {192, 168, 7, 2}};

// Constructor argument: the local UDP source port (the TFTP "TID") for this
// client. Pick one not used by other sockets; the default is 49200.
tftp_client tftp{49200};

// Download: arguments are the server address, the remote file name, and the
// current time from your monotonic millisecond clock. Fails if the name does
// not fit in a request packet. The request is queued, not yet sent.
(void)tftp.start_read({192, 168, 7, 1}, "boot.bin", now_ms());

reloco::task<void> rx(ipv4_node<1006> &ip, tftp_client &tftp) {
  while (!tftp.finished() && !tftp.failed()) {
    auto pkt = co_await co_await ip.receive();
    // Offer every received packet. tftp_event::ignored means it was not part
    // of this transfer (wrong protocol/server/port/TID).
    switch (tftp.handle(pkt, now_ms())) {
    case tftp_event::data:
      flash_write(tftp.data()); // one block; valid only until the next handle()
      break;
    case tftp_event::failed:
      log_error(tftp.error_code()); // code from the server's ERROR packet
      break;
    default:
      break;
    }
  }
}

// Main loop: sends the request / ACKs and retransmits on timeout; completes
// immediately (sends nothing) if nothing is due.
auto t = tftp_send_due(ip, tftp, now_ms());
t.resume();
// After the loop ends: tftp.timed_out() distinguishes a retry timeout from a
// server error (failed() is true for both).

// Upload variant: after start_write(...), on tftp_event::need_data provide the
// next block. Fewer than 512 bytes (even 0) marks the final block.
//   if (tftp.handle(pkt, now_ms()) == tftp_event::need_data)
//     (void)tftp.supply(next_block_up_to_512_bytes);
```

## API

### Enums

`tftp_state`: `idle`, `reading`, `writing`, `done`, `failed`.

`tftp_event` (result of `handle()`):

| Value | Meaning |
|---|---|
| `ignored` | Not part of this transfer. |
| `none` | Consumed; nothing for the application to do (e.g. a repeated block). |
| `data` | A new block is available in `data()`. |
| `need_data` | Call `supply()` with the next block. |
| `done` | Write acknowledged to the end. |
| `failed` | Server sent ERROR (see `error_code()`). |

### `tftp_client`

| Member | Description |
|---|---|
| `retry_timeout_ms` / `max_retries` | 1000 ms / 5. |
| `tftp_client(local_port = 49200)` | Sets the local UDP port. |
| `state()`, `finished()`, `failed()`, `timed_out()` | Transfer status; `finished()` is `state() == done`. |
| `error_code()` | Code from the server's ERROR packet (0 if none or on timeout). |
| `local_port()`, `server()` | Source port and server address in use. |
| `remote_port()` | 69 until the server answers, then the server's transfer port. |
| `data()` | Block received by the last `tftp_event::data`. |
| `start_read(server, filename, now_ms)` | Begin a download; `result<void>`. |
| `start_write(server, filename, now_ms)` | Begin an upload; `result<void>`. |
| `abort()` | Abandon the transfer (state becomes `idle`; the server times out on its own). |
| `supply(block)` | Provide the next block after `need_data`; `error::invalid_argument` if not awaiting one. Fewer than 512 bytes ends the upload. |
| `poll(now_ms)` | Next outgoing UDP payload (send to `server():remote_port()` from `local_port()`) if due, else `error::try_again`. View valid until the next `poll()`/`handle()`/`supply()`. After the retries are exhausted it moves to `failed` with `timed_out()`. |
| `handle(pkt, now_ms)` | Offers a received `ipv4_packet`; returns a `tftp_event`. |

### `tftp_send_due`

```cpp
template <std::size_t Mtu>
reloco::task<void> tftp_send_due(ipv4_node<Mtu> &node, tftp_client &client, std::uint64_t now_ms) noexcept;
```

Sends the client's next packet through `node` as UDP if one is due;
completes immediately otherwise.
