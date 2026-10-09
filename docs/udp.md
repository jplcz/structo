<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::net::udp_datagram`

`include/structo/net/udp.hpp`

> **Warning:** the network stack is **not meant for production devices** (no authentication, encryption or hardening against hostile peers). See [tftp_over_slip.md](tftp_over_slip.md).

UDP (RFC 768) datagram parse/build, including the IPv4 pseudo-header
checksum. It operates on the *payload* of an IPv4 packet, so it builds on
[`ipv4.md`](ipv4.md) (`ipv4_address`, `internet_checksum`,
`ipv4_pseudo_header_sum`). C++20 only (empty otherwise). It is used by
[`dhcp_client.md`](dhcp_client.md) and [`tftp_client.md`](tftp_client.md).

## Usage

```cpp
#include <structo/net/udp.hpp>

using namespace structo::net;

void on_ip(const ipv4_packet &pkt) {
  if (pkt.header.protocol != ip_proto_udp)
    return;

  // Arguments: the IPv4 payload, then the packet's src and dst addresses.
  // The addresses are needed because the UDP checksum covers a pseudo-header
  // made from them. Fails with invalid_argument if the datagram is truncated,
  // its length field is inconsistent, or a non-zero checksum is wrong
  // (a zero checksum means "sender did not compute one" and is accepted).
  auto d = parse_udp(pkt.payload, pkt.header.src, pkt.header.dst);
  if (d && d->dst_port == 68)
    use(d->payload); // view into pkt.payload, valid only as long as it is
}

void send(reloco::span<const std::uint8_t> payload) {
  // Output buffer: 8-byte header + payload. Must not overlap `payload`.
  reloco::array<std::uint8_t, 256> out;

  // Arguments: source port, destination port, payload, then source and
  // destination IPv4 addresses (for the pseudo-header checksum; they must
  // match what the IPv4 layer will put in the header), then the output span.
  // Returns the datagram size, or error::out_of_range if `out` is too small
  // or the datagram would exceed 65535 bytes.
  auto n = build_udp(5000, 7, payload, {10, 0, 0, 2}, {10, 0, 0, 1}, out);
  if (n) {
    // Hand out[0..*n) to build_ipv4(...) with protocol ip_proto_udp.
  }
}
```

## API

| Name | Description |
|---|---|
| `udp_header_size` | 8 bytes. |
| `udp_datagram` | `src_port`, `dst_port`, `payload` (`span<const std::uint8_t>` viewing the buffer given to `parse_udp`). |
| `parse_udp(d, src, dst) -> result<udp_datagram>` | Decodes a datagram carried between `src` and `dst`; `error::invalid_argument` on malformed input or bad checksum. |
| `build_udp(src_port, dst_port, payload, src, dst, out) -> result<size_t>` | Builds the datagram with the checksum computed (a computed value of 0 is sent as `0xFFFF`, since 0 on the wire means "no checksum"). Returns bytes written or `error::out_of_range`. |
