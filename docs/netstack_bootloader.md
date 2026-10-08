<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# Guide: running the bootloader netstack over a serial port

C++20 only. `bootldr::netstack` (`bootldr/netstack.hpp`) runs the network
stack as two tasks of a `bootldr::scheduler`, over a SLIP link on a polled
UART. Today it provides a static or DHCP-assigned IPv4 address and answers
ping and queues UDP datagrams for `bootldr::udp_socket`s (section 6); there is
no TCP yet, and anything else goes to a raw packet hook.

```
netstack           (bootldr/netstack.hpp)       rx task + timer task, DHCP, ping
scheduler          (bootldr/scheduler.hpp)      runs tasks, polls the device
ipv4_node          (net/ipv4_node.hpp)          IPv4 carrier
polled_net_device  (hw/polled_net_device.hpp)   parks coroutines until poll()
slip_device        (hw/slip_device.hpp)         RFC 1055 framing
uart_ref           (hw/uart_ref.hpp)            your UART
```

## 1. Host setup (Linux)

```sh
# Attach SLIP to the serial port; creates sl0.
# -L: 3-wire cable, -s: baud rate (must match the board's uart_config).
sudo slattach -L -p slip -s 115200 /dev/ttyUSB0 &

# Point-to-point link: host 192.168.7.1 <-> board 192.168.7.2 (static case).
sudo ip addr add 192.168.7.1 peer 192.168.7.2/32 dev sl0
sudo ip link set sl0 mtu 1006 up   # must equal the Mtu used on the board
```

For **DHCP** instead of a static address, also run a DHCP server on `sl0`
(the board then ignores the peer address above):

```sh
sudo dnsmasq --no-daemon --port=0 --interface=sl0 --bind-interfaces \
     --dhcp-range=192.168.7.2,192.168.7.2,255.255.255.0,1h
```

## 2. Bind your UART

Specialize `uart_traits` for your hardware once (see
[tftp_over_slip.md](tftp_over_slip.md#2-bind-your-uart) and
[uart_ref.md](uart_ref.md)): `configure`, `tx_ready`, `rx_ready`,
`try_put_byte`, `try_get_byte`.

## 3. Build and start the stack

```cpp
#include <structo/bootldr/netstack.hpp>
#include <structo/hw/slip_device.hpp>

using namespace structo;

constexpr std::size_t mtu = 1006; // largest IP datagram; keep equal to the host's sl0 MTU

// Monotonic millisecond clock (timer register, SysTick counter ...).
// The scheduler uses it for sleeps; the stack uses it for DHCP timeouts.
std::uint64_t now_ms(void *) noexcept;

int main() {
  my_uart hw_uart;
  hw::uart_ref uart{hw_uart};                        // type-erased handle over your UART
  hw::slip_device<mtu> slip{uart};                   // frames IP datagrams onto the UART
  hw::polled_net_device<decltype(slip)> pnd{slip};   // adapts the polled device to coroutines
  hw::net_device_ref nic{pnd};                       // the handle the stack talks to

  bootldr::scheduler sched;                          // task table from reloco::default_allocator()
  sched.set_clock(now_ms, nullptr);                  // required: sleeps and DHCP timers need time

  bootldr::netstack_config cfg;
  // Static address: used immediately, no server needed.
  cfg.static_ip = net::ipv4_config::make_static(
      {192, 168, 7, 2},    // our address
      {255, 255, 255, 0},  // netmask
      {192, 168, 7, 1});   // gateway (optional)
  // Or DHCP: leave static_ip empty and set cfg.dhcp = true. A leased
  // address replaces the static one; the lease is renewed automatically.
  // cfg.dhcp = true;
  // cfg.xid_seed = read_hw_random();                // makes DHCP transaction ids device-unique
  // cfg.mac = {2, 0, 0, 0, 0, 1};                   // DHCP chaddr; SLIP has none, any local MAC works

  bootldr::netstack<mtu> net{sched, nic, cfg};       // must outlive the scheduler run
  if (!net.poll_with(pnd))                           // sched calls pnd.poll() at the start of every round
    return 1;                                        // (allocation failed)
  if (!net.start())                                  // spawns the rx and timer tasks
    return 1;

  // Your own work runs as further tasks of the same scheduler.
  // sched.spawn(boot_flow(reloco::allocator_arg, sched.allocator(), sched, net));

  sched.run();                                       // loops until every task finished (the
                                                     // netstack tasks run until stop())
}
```

The `Mtu` template argument sizes the RX and TX buffers inside the node, so
the stack allocates nothing per packet. Everything else (task table, coroutine
frames) comes from the scheduler's allocator.

## 4. Waiting for the network from your own task

```cpp
// A boot task that must not start before the board has an IP address.
reloco::task<void> boot_flow(reloco::allocator_arg_t, reloco::allocator_ref,
                             bootldr::scheduler &sched, bootldr::netstack<mtu> &net) {
  // Poll ready() every 100 ms. Inner co_await sleeps; the outer one unwraps
  // the result<void> (it fails only if the scheduler has no clock).
  while (!net.ready())
    co_await co_await sched.sleep_for(100);
  // net.config().address / .gateway / .dns now hold the active configuration.
  // ... start the next stage ...
}
```

## 5. Verify

```sh
ping -c3 192.168.7.2        # answered by the stack's ICMP echo handler
```

In code, `net.stats()` counts handled/dropped datagrams and RX/TX errors, and
`net.dhcp_state()` shows the DHCP progress (`selecting` forever = no server
reachable; check `slattach`, baud and MTU).

## 6. UDP sockets

A `bootldr::udp_socket` is owned by your code (stack, struct member ...) and
plugs into the stack by reference. Its data is on the heap: received
datagrams and send buffers are allocated from the scheduler's allocator (or
one you pass), nothing is statically sized.

```cpp
#include <structo/bootldr/udp_socket.hpp> // already included by netstack.hpp

// A tiny UDP echo service: replies to every datagram sent to port 5000.
reloco::task<void> echo(bootldr::udp_socket &sock) {
  reloco::array<std::uint8_t, 512> buf;   // your receive buffer; longer datagrams are truncated (rx.truncated)
  for (;;) {
    // Inner co_await suspends this task until a datagram arrives; the outer one
    // unwraps the result or ends the task with the error (e.g. socket closed).
    auto rx = co_await co_await sock.receive_from(buf);
    // Reply: destination address, destination port, payload view. The address
    // must be configured (static or DHCP); otherwise this fails with invalid_state.
    co_await co_await sock.send_to(rx.source, rx.source_port, {buf.data(), rx.size});
  }
}

// In main(), after net.start():
bootldr::udp_socket sock{net};        // unbound; heap comes from sched's allocator. 2nd arg: max queued datagrams (8)
if (!sock.bind(5000))                 // 0 would pick a free port from 49152 up; fails with busy if taken
  return 1;
sched.spawn(echo(sock));              // detached task; sock must outlive it
```

Host side: `echo hello | nc -u -w1 192.168.7.2 5000`.

Rules: at most `max_queue` datagrams wait per socket, extra ones are dropped
and counted in `sock.dropped()` (so are datagrams that could not be copied
because the heap is exhausted). `try_receive_from()` is the non-blocking
variant (`error::try_again` when empty), handy for your own timeouts together
with `sched.sleep_for()`. Cancel or finish tasks waiting in `receive_from()`
before closing or destroying the socket. DHCP traffic (port 68) is consumed
by the stack before sockets see it.

## 7. Handling other protocols (raw hook)

Anything that is not DHCP, an ICMP echo request or UDP for a bound socket goes
to the packet handler (or is counted in `stats().rx_dropped`). It is the attach
point for other protocols and can be used today to experiment:

```cpp
// Called from the rx task for every other IPv4 datagram addressed to us.
// `pkt` (and its payload view) is valid only during the call: copy what you need.
void on_packet(void *ctx, const net::ipv4_packet &pkt) noexcept {
  if (pkt.header.protocol == net::ip_proto_udp) {
    // parse_udp(pkt.payload, pkt.header.src, pkt.header.dst) ...
  }
}

net.set_packet_handler(on_packet, /*ctx=*/nullptr);
```

The handler must not block (no waiting inside it); hand the data to another
task, e.g. through a `bootldr::scheduler::event`. To transmit, use
`net.ip().send(proto, dst, payload)` from a task (one send in flight at a time;
the stack's own DHCP sender shares this slot, so a second concurrent send
fails with `error::busy`: retry after a `yield()`).

## Notes

- Drive everything from one thread; the scheduler is not thread-safe. If an
  interrupt handler feeds the UART, keep it to filling a FIFO that
  `try_get_byte` drains.
- `net.stop()` cancels the tasks; the destructor does the same, so declare
  the stack *after* the scheduler (destroyed first).
- For a TFTP client on top of the same link see
  [tftp_over_slip.md](tftp_over_slip.md); it currently drives `ipv4_node`
  directly rather than through `netstack`.

## Using PPP instead of SLIP

PPP negotiates the addresses itself (LCP, then IPCP), so there is no static
IP or DHCP to configure. Authentication is not supported: run the host side
with `noauth`.

```
# Host: hands 192.168.7.2 to the board, takes 192.168.7.1 for itself.
pppd /dev/ttyUSB0 115200 192.168.7.1:192.168.7.2 noauth local nodetach
```

```cpp
std::uint64_t now_ms(void *) noexcept;                  // monotonic ms clock, drives PPP retransmit timers

structo::bootldr::scheduler sched;
sched.set_clock(now_ms, nullptr);

structo::hw::uart_ref uart{my_uart};                    // the serial line, 115200 8N1
structo::net::ppp_config pcfg;                          // defaults: ask the peer for our address and DNS
structo::hw::ppp_device<1500> ppp{uart, now_ms, nullptr, pcfg}; // 1500 = largest IP datagram; starts LCP right away
structo::hw::polled_net_device<decltype(ppp)> pnd{ppp}; // coroutine send/receive on top of the polled backend
structo::hw::net_device_ref nic{pnd};                   // what the netstack consumes

structo::bootldr::netstack<1500> net{sched, nic};       // no static_ip/dhcp: the address comes from PPP
(void)net.poll_with(pnd);                               // pump the device every scheduler round (also drives PPP)
net.use_ppp(ppp.link());                                // apply the negotiated address/gateway when IPCP opens, clear it when the link drops
(void)net.start();

for (;;) {
  sched.run_once();                                     // net.ready() becomes true once IPCP is open
}
```

After `net.ready()` the board answers ping at the address
`ppp.link().local_address()`; `ppp.link().dns()` holds the DNS server the peer
offered (unspecified if none). If negotiation fails (`ppp.link().failed()`),
check that `pppd` runs with `noauth` and the same baud rate.

## TFTP and receive timeouts

```cpp
// Needs a scheduler clock; timeouts are measured against it.
sched.set_clock(now_ms, nullptr);

// tftp_client binds an ephemeral UDP socket per transfer on the netstack's demux.
bootldr::tftp_options opt;
opt.timeout_ms = 1000; // silence before the last packet is retransmitted
opt.max_retries = 5;   // retransmits before failing with timed_out
bootldr::tftp_client tftp{net, opt};

// Download "kernel.bin" from the server into a memory buffer (runs as a scheduler task).
reloco::array<std::uint8_t, 65536> image{};
auto r = co_await tftp.get(server_ip, "kernel.bin", reloco::span<std::uint8_t>(image));

// A plain socket receive can also time out: returns error::timed_out after 500 ms.
auto d = co_await sock.receive_from(buf, 500);
```
