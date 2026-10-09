<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::net::ipv4_address`

`include/structo/net/ipv4.hpp`

> **Warning:** the network stack is **not meant for production devices** (no authentication, encryption or hardening against hostile peers). See [tftp_over_slip.md](tftp_over_slip.md).

Allocation-free IPv4 primitives: the `ipv4_address` value type, the RFC 1071
Internet checksum, and header parse/build. C++20 only (the header is empty
when `RELOCO_HAS_COROUTINES` is 0), like the rest of the network stack.

Scope is deliberately small: options are skipped on receive and never
generated, and fragmented packets are rejected with
`error::unsupported_operation` -- there is no reassembly. Everything is
`noexcept` and returns `reloco::result`. It is the base of
[`udp.md`](udp.md), [`arp.md`](arp.md), [`dhcp_client.md`](dhcp_client.md)
and [`tftp_client.md`](tftp_client.md).

## Usage

```cpp
#include <structo/net/ipv4.hpp>

using namespace structo::net;

// Our own address; the four octets are in network (wire) order.
constexpr ipv4_address me{192, 168, 7, 2};
constexpr ipv4_address peer{192, 168, 7, 1};

void on_frame(reloco::span<const std::uint8_t> frame) {
  // `frame` is a raw IPv4 datagram (e.g. received from a net_device_ref).
  // parse_ipv4 checks version, IHL, total length and the header checksum, so
  // anything it returns is structurally valid. It fails with invalid_argument
  // for malformed data and unsupported_operation for fragments.
  auto pkt = parse_ipv4(frame);
  if (!pkt)
    return; // drop it

  // Filter on destination address and upper-layer protocol number.
  if (pkt->header.dst == me && pkt->header.protocol == ip_proto_udp) {
    // pkt->payload is a view INTO `frame`: it is only valid while `frame` is.
    handle_udp(pkt->payload);
  }
}

void send_payload(reloco::span<const std::uint8_t> payload) {
  // Output buffer: header (20 bytes) + payload. It must not overlap `payload`.
  reloco::array<std::uint8_t, 128> out;

  // Header fields, in declaration order: protocol, src, dst. identification
  // and ttl keep their defaults (0 and 64); header_length is ignored on build.
  // DF is always set and the header checksum is computed for you.
  auto n = build_ipv4({ip_proto_udp, me, peer}, payload, out);
  if (n)
    transmit(reloco::span<const std::uint8_t>(out.data(), *n)); // *n = bytes written
}
```

## `ipv4_address`

Four octets in network byte order (`reloco::array<std::uint8_t, 4> octets`).

| Member | Description |
|---|---|
| `ipv4_address()` | `0.0.0.0` (unspecified). |
| `ipv4_address(a, b, c, d)` | Builds `a.b.c.d`; `constexpr`. |
| `is_unspecified()` | True for `0.0.0.0`. |
| `is_broadcast()` | True for `255.255.255.255`. |
| `==`, `!=` | Octet-wise comparison. |

## Constants

| Name | Value |
|---|---|
| `ip_proto_icmp` | 1 |
| `ip_proto_tcp` | 6 |
| `ip_proto_udp` | 17 |
| `ipv4_header_size` | 20 (header without options) |

## Checksum helpers

| Function | Description |
|---|---|
| `internet_checksum(data, initial = 0)` | RFC 1071 ones'-complement checksum of `data`, already complemented; an odd trailing byte is zero-padded. Checking a block that contains its own checksum yields 0. `initial` is a partial sum to continue from. `constexpr`. |
| `ipv4_pseudo_header_sum(src, dst, protocol, length)` | Partial sum of the TCP/UDP pseudo-header; pass it as `initial` to `internet_checksum` (used by [`udp.md`](udp.md)). |

## Header and packet types

`ipv4_header`: `protocol`, `src`, `dst`, `identification` (0), `ttl` (64),
`header_length` (bytes; set by `parse_ipv4`, ignored by `build_ipv4`).

`ipv4_packet`: `header` plus `payload` (`span<const std::uint8_t>` viewing the
buffer given to `parse_ipv4`).

## Functions

| Function | Description |
|---|---|
| `parse_ipv4(span<const uint8_t>) -> result<ipv4_packet>` | Validates and decodes. `error::invalid_argument`: truncated, wrong version, bad IHL, bad total length or bad header checksum. `error::unsupported_operation`: fragment (MF set or non-zero offset). Link-layer padding beyond the total length is ignored; options are skipped. |
| `build_ipv4(const ipv4_header &, payload, out) -> result<size_t>` | Writes an option-less header (DF set, checksum computed) followed by `payload`; returns bytes written. `error::out_of_range` if `out` is too small or the packet would exceed 65535 bytes. `out` must not overlap `payload`. |
