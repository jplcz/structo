<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::net::arp_packet`

`include/structo/net/arp.hpp`

> **Warning:** the network stack is **not meant for production devices** (no authentication, encryption or hardening against hostile peers). See [tftp_over_slip.md](tftp_over_slip.md).

ARP (RFC 826) packet codec for Ethernet/IPv4: it only builds and parses the
28-byte ARP payload. There is no cache, timer or resolver here; the caller
puts the payload into an Ethernet frame with ethertype `arp_ethertype` and
decides what to do with replies. It uses `ipv4_address` from
[`ipv4.md`](ipv4.md) and `hw::net_mac_address` (a 6-byte array) from
`hw/net_device_ref.hpp`; see also [`ethernet_device.md`](ethernet_device.md)
and [`ethernet_nic.md`](ethernet_nic.md). C++20 only (empty otherwise).

## Usage

```cpp
#include <structo/net/arp.hpp>

using namespace structo::net;

void ask(const hw::net_mac_address &mac) {
  // "Who has 10.0.0.1? Tell 10.0.0.2." Fields in order: opcode, sender MAC
  // (us), sender IP (us), target MAC (all zero: unknown in a request),
  // target IP (the address being resolved).
  arp_packet req{arp_request, mac, {10, 0, 0, 2}, {}, {10, 0, 0, 1}};

  // Exactly arp_packet_size (28) bytes are needed; a smaller buffer fails
  // with error::out_of_range.
  reloco::array<std::uint8_t, arp_packet_size> out{};
  auto n = build_arp(req, out); // -> 28
  // Send out[0..*n) in an Ethernet frame with ethertype arp_ethertype
  // (0x0806), destination ff:ff:ff:ff:ff:ff for a request.
}

void on_arp_payload(reloco::span<const std::uint8_t> payload) {
  // Rejects (error::invalid_argument) short payloads, anything that is not
  // Ethernet/IPv4 ARP, and opcodes other than request/reply.
  auto p = parse_arp(payload);
  if (p && p->op == arp_reply) {
    // p->sender_ip is now at p->sender_mac: record it in your own cache.
  }
}
```

## API

| Name | Description |
|---|---|
| `arp_ethertype` | `0x0806`. |
| `arp_packet_size` | 28. |
| `arp_request`, `arp_reply` | Opcodes 1 and 2. |
| `arp_packet` | `op`, `sender_mac`, `sender_ip`, `target_mac` (zeros in requests), `target_ip`. |
| `build_arp(p, out) -> result<size_t>` | Encodes `p` (hardware type Ethernet, protocol IPv4, lengths 6/4); returns 28 or `error::out_of_range`. |
| `parse_arp(payload) -> result<arp_packet>` | Decodes a payload; `error::invalid_argument` as above. |
