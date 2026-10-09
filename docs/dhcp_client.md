<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::net::dhcp_client`

`include/structo/net/dhcp_client.hpp`

> **Warning:** the network stack is **not meant for production devices** (no authentication, encryption or hardening against hostile peers). See [tftp_over_slip.md](tftp_over_slip.md).

A DHCPv4 client for `ipv4_node` (`net/ipv4_node.hpp`): a sans-IO state
machine (`dhcp_client`) plus a small coroutine helper (`dhcp_send_due`) that
transmits its messages. C++20 only. It builds on [`udp.md`](udp.md) and
[`ipv4.md`](ipv4.md); the wire codec lives in `net/dhcp.hpp` and the leased
`ipv4_config` in `net/ipv4_config.hpp`.

Lifecycle:

```
INIT -> SELECTING (DISCOVER sent) -> REQUESTING (OFFER received, REQUEST sent)
     -> BOUND (ACK) -> RENEWING (at T1, default half the lease)
     -> INIT again if the lease expires or the server NAKs
```

DISCOVER is retransmitted with exponential backoff (4 s, doubling up to
64 s); a REQUEST is retried every 4 s up to 3 tries before falling back to
INIT; renewals retry every 10 s. A lease of `0xFFFFFFFF` is infinite, and a
missing lease option defaults to 3600 s.

There is no clock or timer in the stack: the caller supplies a monotonic
millisecond timestamp. Timers are evaluated whenever `poll()` /
`dhcp_send_due()` runs, so call it every main-loop iteration; it sends
nothing unless something is due. Static addressing needs none of this: use
`node.configure(ipv4_config::make_static(...))` instead.

## Usage

```cpp
#include <structo/net/dhcp_client.hpp>

using namespace structo::net;

// IPv4 carrier over a net device, starting unconfigured (no address yet).
// 1006 is the link MTU (SLIP's RFC 1055 maximum here).
ipv4_node<1006> ip{nic};

// Arguments: our MAC (goes into chaddr; on SLIP, which has none, use any
// locally administered address), and a seed for transaction ids -- make it
// device-unique, e.g. a hardware RNG sample, so two boards do not collide.
dhcp_client dhcp{mac, 0xC0FFEE};

reloco::task<void> app(ipv4_node<1006> &ip, dhcp_client &dhcp) {
  for (;;) {
    auto pkt = co_await co_await ip.receive();
    // handle() = on_packet() + apply(). It returns true if the packet was a
    // DHCP datagram (UDP port 68) and consumed; once an ACK arrives it also
    // pushes the leased address into `ip`. now_ms() is YOUR monotonic clock
    // in milliseconds; lease timers are measured against it.
    if (dhcp.handle(ip, pkt, now_ms()))
      continue;
    // ... false: not DHCP, so it is an application datagram ...
  }
}

// Main loop: keep the device moving and let DHCP retransmit/renew.
for (;;) {
  pnd.poll(); // polled_net_device: move bytes between UART and coroutines

  // Applies pending config changes, then transmits the next DHCP message
  // (broadcast, UDP 68 -> 67) only if one is due at now_ms().
  auto t = dhcp_send_due(ip, dhcp, now_ms());
  t.resume();
  // If it parked on a busy device, keep `t` alive until t.done().

  if (dhcp.state() == dhcp_state::bound)
    use(dhcp.config()->address); // config() is nullptr unless BOUND/RENEWING
}
```

## API

### `dhcp_state`

`init`, `selecting`, `requesting`, `bound`, `renewing`.

### `dhcp_client`

| Member | Description |
|---|---|
| `dhcp_client(mac, xid_seed)` | `mac`: hardware address for chaddr / client id. `xid_seed`: seed for transaction ids. |
| `state()` | Current `dhcp_state`. |
| `config()` | `const ipv4_config *` for the lease while BOUND/RENEWING, else `nullptr`. |
| `restart()` | Back to INIT (e.g. after link-up); drops the current lease, which the next `apply()` removes from the node. |
| `poll(now_ms)` | Next outgoing BOOTP payload (broadcast to 255.255.255.255:67 from port 68) if due, else `error::try_again`. The view is valid until the next `poll()`. Handles the INIT/SELECTING/REQUESTING/renew/expiry transitions. |
| `on_packet(pkt, now_ms)` | Feeds a received `ipv4_packet`. Returns true if it was a DHCP datagram on UDP port 68 (even one for another transaction, which is ignored), false if the caller should handle it. |
| `apply(node)` | Calls `node.configure(...)` with the lease (or an empty config if the lease was lost), only when it changed since the last call. |
| `handle(node, pkt, now_ms)` | `on_packet` followed by `apply`; returns `on_packet`'s result. |

### `dhcp_send_due`

```cpp
template <std::size_t Mtu>
reloco::task<void> dhcp_send_due(ipv4_node<Mtu> &node, dhcp_client &client, std::uint64_t now_ms) noexcept;
```

Applies any configuration change to `node`, then sends the client's next
DHCP message as a broadcast if one is due; completes immediately otherwise.
