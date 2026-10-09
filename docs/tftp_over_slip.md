<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# Guide: a basic TFTP client over a serial SLIP link

> **Warning:** the network code (`net/*`, `bootldr/netstack.hpp`, `hw/*_device.hpp`, TFTP, DHCP, ARP, PPP/SLIP) is **not meant for production devices**. It has no authentication, encryption or hardening against hostile peers; use it only in controlled test environments, CI and development.

C++20 only (the network stack is built on `reloco` coroutines). This guide
wires the layers together, bottom to top, so a board with just a polled UART
can download (or upload) a file from a host over a serial cable:

```
tftp_client        (net/tftp_client.hpp)        RFC 1350 state machine
ipv4_node          (net/ipv4_node.hpp)          IPv4 carrier, static address
net_device_ref     (hw/net_device_ref.hpp)      coroutine send/receive handle
polled_net_device  (hw/polled_net_device.hpp)   parks coroutines until poll()
slip_device        (hw/slip_device.hpp)         RFC 1055 framing
uart_ref           (hw/uart_ref.hpp)            your UART
```

SLIP carries bare IP datagrams (no MAC, no ARP, no DHCP on the wire), so the
board uses a **static address** and the host is the point-to-point peer.

## 1. Host setup (Linux)

```sh
# Attach SLIP to the serial port; this creates the sl0 interface.
# -L: 3-wire cable (no modem control lines), -s: baud rate (must match the board).
sudo slattach -L -p slip -s 115200 /dev/ttyUSB0 &

# Point-to-point link: host 192.168.7.1 <-> board 192.168.7.2.
sudo ip addr add 192.168.7.1 peer 192.168.7.2/32 dev sl0
sudo ip link set sl0 mtu 1006 up

# Any TFTP server reachable on the host address, serving /srv/tftp (example: dnsmasq).
sudo dnsmasq --no-daemon --port=0 --enable-tftp --tftp-root=/srv/tftp \
     --listen-address=192.168.7.1 --bind-interfaces
```

The host kernel needs `CONFIG_SLIP` (some distributions no longer ship it).
For uploads use a server that accepts writes (e.g. `in.tftpd --create`);
dnsmasq's TFTP server is read-only.

## 2. Bind your UART

`uart_ref` is bound to any backend that specializes `uart_traits` (see
[uart_ref.md](uart_ref.md)). Do this once for your hardware; these five
functions are the whole contract.

```cpp
struct my_uart { /* register base, etc. */ };

template <> struct structo::hw::uart_traits<my_uart> {
  // Apply baud/format; SLIP needs 8 data bits, no parity.
  static reloco::result<void> configure(my_uart &, const structo::hw::uart_config &) noexcept;
  // True when the TX FIFO can accept a byte (SLIP pumps bytes as room appears).
  static reloco::result<bool> tx_ready(my_uart &) noexcept;
  // True when a received byte is waiting.
  static reloco::result<bool> rx_ready(my_uart &) noexcept;
  static reloco::result<void> try_put_byte(my_uart &, std::uint8_t) noexcept;
  static reloco::result<std::uint8_t> try_get_byte(my_uart &) noexcept;
};
```

## 3. Build the stack

```cpp
#include <structo/hw/polled_net_device.hpp>
#include <structo/hw/slip_device.hpp>
#include <structo/net/tftp_client.hpp>

using namespace structo;

// RFC 1055 maximum frame size; keep it equal to the host's sl0 MTU.
constexpr std::size_t mtu = 1006;

my_uart hw_uart;
hw::uart_ref uart{hw_uart};                             // type-erased handle over your UART
hw::slip_device<mtu> slip{uart};                        // frames IP datagrams onto the UART
hw::polled_net_device<hw::slip_device<mtu>> pnd{slip};  // coroutine send/receive; resumed by pnd.poll()
hw::net_device_ref nic{pnd};                            // what ipv4_node consumes

// Our static address (the host is 192.168.7.1). Netmask/gateway are not needed on a point-to-point link.
net::ipv4_node<mtu> ip{nic, net::ipv4_address{192, 168, 7, 2}};

// Local UDP port for this client; the server answers to it. Pick any free port.
net::tftp_client tftp{49200};
```

A TFTP packet is at most 516 bytes, plus 8 (UDP) and 20 (IPv4) = 544, so any
MTU of at least 544 works; the node's buffers are `Mtu` bytes each.

## 4. Download a file

TFTP needs two things running at once: a **receive task** that is always
parked in `ip.receive()` (it sees the server's replies), and a **send pump**
that transmits the request, ACKs and retransmissions. Both are lazy
`reloco::task`s, driven from your main loop together with `pnd.poll()`.

```cpp
// Receive side: loops until the transfer ends. The inner co_await suspends
// until a datagram for us arrives; the outer one unwraps the result (or ends
// the task with the error, e.g. error::busy if receive() is already running).
reloco::task<void> receive_loop(net::ipv4_node<mtu> &ip, net::tftp_client &tftp) {
  while (!tftp.finished() && !tftp.failed()) {
    auto pkt = co_await co_await ip.receive();
    // `ignored` means the datagram isn't part of the transfer; `data` means one new block.
    if (tftp.handle(pkt, now_ms()) == net::tftp_event::data)
      store_chunk(tftp.data()); // your sink (flash, RAM ...); valid until the next handle()
  }
}

// `name` is the remote file name; now_ms() is any monotonic millisecond clock you provide.
bool download(reloco::string_view name) {
  // Server address, file name, current time. Fails if the name is empty or too long.
  if (!tftp.start_read(net::ipv4_address{192, 168, 7, 1}, name, now_ms()))
    return false;

  auto rx = receive_loop(ip, tftp);
  rx.resume();           // runs until it parks in ip.receive()
  reloco::task<void> tx; // current send attempt (empty task = idle)

  while (!rx.done()) {
    slip.service(); // keeps the UART TX FIFO fed and drains RX bytes
    pnd.poll();     // resumes coroutines whose device became ready

    // Start a new send attempt when the previous one finished. It sends the
    // request / the pending ACK / a retransmission, or does nothing if idle.
    if (tx.done()) {
      tx = net::tftp_send_due(ip, tftp, now_ms());
      tx.resume();
    }
  }

  // The ACK for the last block is queued when that block arrives: flush it
  // (and any send still in flight) so the server sees it.
  for (;;) {
    while (!tx.done()) {
      slip.service();
      pnd.poll();
    }
    tx = net::tftp_send_due(ip, tftp, now_ms());
    tx.resume();
    if (tx.done())
      break; // nothing was due: everything has been sent
  }
  return tftp.finished(); // false: server ERROR (tftp.error_code()) or tftp.timed_out()
}
```

What happens on the wire: the first send attempt transmits the RRQ to port
69; the server answers DATA(1) from a fresh port, which the client adopts for
the rest of the transfer (`tftp.remote_port()`); each block is ACKed by the
next send attempt; a block shorter than 512 bytes ends the transfer. If
nothing arrives for 1 s the last packet is resent, up to 5 times, then
`tftp.failed()` and `tftp.timed_out()` become true. A server ERROR sets
`tftp.failed()` and `tftp.error_code()` (1 = file not found, 2 = access
violation, ...).

## 5. Upload a file

Same two tasks, but the receive side provides data on demand:

```cpp
// `src` is your data source; read() fills up to 512 bytes and returns the count.
reloco::task<void> upload_receive_loop(net::ipv4_node<mtu> &ip, net::tftp_client &tftp, source &src) {
  while (!tftp.finished() && !tftp.failed()) {
    auto pkt = co_await co_await ip.receive();
    // need_data: the server acknowledged the WRQ or the previous block.
    if (tftp.handle(pkt, now_ms()) == net::tftp_event::need_data) {
      std::uint8_t block[net::tftp_block_size];
      std::size_t n = src.read(block); // fewer than 512 bytes marks the last block
      (void)tftp.supply(reloco::span<const std::uint8_t>(block, n));
    }
  }
}
// start_write(server, name, now_ms()) and the main loop are the same as in download().
```

If the file size is an exact multiple of 512, finish with an empty block
(`n == 0`): TFTP signals end-of-file with a short block. `tftp.finished()`
becomes true once the server ACKs that last block.

## 6. Notes and limits

- One transfer at a time per `tftp_client`; call `start_*` again to reuse it.
- Octet mode only; no option negotiation (blksize, tsize, windowsize), so the
  server must accept plain RFC 1350 requests.
- Only one `receive()` and one `send()` may be in flight on an `ipv4_node`;
  the pattern above keeps exactly one of each.
- Timeouts need a clock you provide; nothing in the library blocks.
- To go interrupt-driven later, replace `polled_net_device` with a backend
  whose coroutines are resumed from the IRQ handler; `tftp_client` and
  `ipv4_node` do not change.
- No DHCP on SLIP: use the static address. `net/dhcp_client.hpp` is for links
  that carry broadcast traffic.
