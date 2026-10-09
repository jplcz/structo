<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::ethernet_device`

`include/structo/hw/ethernet_device.hpp`, `include/structo/net/ethernet.hpp`

> **Warning:** the network code (`net/*`, `bootldr/netstack.hpp`, `hw/*_device.hpp`, TFTP, DHCP, ARP, PPP/SLIP) is **not meant for production devices**. It has no authentication, encryption or hardening against hostile peers; use it only in controlled test environments, CI and development.

> **C++ standard:** C++20 only (coroutines). Under C++17 the headers are empty.

The Ethernet layer of the net stack. It is an adapter: it wraps a
`net_device_ref` that moves **raw Ethernet frames** (a NIC driver behind
`polled_net_device`) and presents a `net_device_ref` that moves **bare IPv4
datagrams**, like SLIP/PPP do. `ipv4_node`, DHCP, UDP and TFTP therefore work
over Ethernet unchanged.

What it does:

- **TX:** picks the next hop (on-link destination, else the gateway), resolves
  its MAC through `net::arp_table`, wraps the datagram in an Ethernet II
  frame. Limited broadcast, directed broadcast and multicast
  (`01:00:5E:...`) need no ARP. While ARP is pending, one datagram is parked
  (dropped after 3 s; a second concurrent `send` returns `error::busy`).
- **RX:** drops frames for other MACs and ethertypes other than IPv4/ARP,
  answers/learns ARP, delivers IPv4 trimmed to its own length field (padding
  removed).
- **Unconfigured:** broadcasts work (needed for DHCP); unicast fails with
  `error::invalid_state`.

```cpp
// Raw frame NIC driver: provides mtu() (frame size, e.g. 1514), link_up(),
// mac_address(), try_send(frame), try_receive(frame) -- see polled_net_device.
my_nic nic;
structo::hw::polled_net_device<my_nic> pnd{nic};
structo::hw::net_device_ref raw{pnd};                 // frames in/out

std::uint64_t now_ms(void *) noexcept;                // monotonic ms clock; drives ARP retries and the 3 s park timeout
                                                      // (a clock_reader/atomic_clock_reader can be passed instead)

// 1500 = largest IPv4 datagram (the IP MTU); 8 = ARP cache slots.
structo::hw::ethernet_device<1500, 8> eth{raw, now_ms, nullptr};
structo::hw::net_device_ref ip_side{eth};             // what the netstack/ipv4_node consumes

structo::bootldr::scheduler sched;
sched.set_clock(now_ms, nullptr);
structo::bootldr::netstack_config cfg;                // leave static_ip empty to use DHCP
structo::bootldr::netstack<1500> net{sched, ip_side, cfg};
(void)net.poll_with(pnd);                             // pump the NIC every scheduler round
net.use_ethernet(eth);                                // each tick: push address/netmask/gateway (static or DHCP) into eth
                                                      // and run its ARP service (replies, announcements, retries)
(void)net.start();
```

Without a `netstack`, call `eth.configure(cfg)` yourself when the address
changes (a new address is announced with a gratuitous ARP) and `co_await
eth.service()` periodically. `eth.arp().add_static(ip, mac)` adds fixed
entries.

The frame codec (`build_ethernet`, `parse_ethernet`, `is_group_mac`,
`ipv4_multicast_mac`) in `net/ethernet.hpp` is usable on its own.

Writing a driver for real Ethernet hardware: see [`ethernet_nic.md`](ethernet_nic.md).

## Linux TAP example

[`examples/netstack_tap_demo.cpp`](../examples/netstack_tap_demo.cpp) runs the
netstack as an Ethernet host on a Linux TAP interface (a `tap_nic` backend does
one `read()`/`write()` per frame). It answers ping and offers three example UDP services: echo (port 7), upper-case (8)
and uptime in ms (9); `--port N` moves them to N, N+1, N+2.
For controlled test/CI/development setups only.

```sh
# Create a TAP owned by you and give the host side an address (as root).
ip tuntap add dev structo0 mode tap user $USER
ip addr add 192.168.77.1/24 dev structo0 && ip link set structo0 up

# Start the "board" at 192.168.77.2, then talk to it from the host.
./netstack_tap_demo --tap structo0 --ip 192.168.77.2
ping 192.168.77.2
echo hi | nc -u -w1 192.168.77.2 7   # echo
echo hi | nc -u -w1 192.168.77.2 8   # upper-case: HI
echo ? | nc -u -w1 192.168.77.2 9    # uptime in ms
```
