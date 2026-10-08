// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file dhcp.hpp
 * @brief DHCPv4 (RFC 2131/2132) message codec: build DISCOVER/REQUEST and
 * parse server replies. Allocation-free, sans-IO. C++20 only. The state
 * machine lives in `dhcp_client.hpp`.
 *
 * @code
 * // Build a DISCOVER for `mac` with transaction id `xid`; `out` receives the BOOTP payload.
 * structo::net::dhcp_request rq;
 * rq.type = structo::net::dhcp_discover;
 * rq.xid = 0x1234;
 * rq.mac = mac;
 * auto n = structo::net::build_dhcp(rq, out);
 *
 * // Parse a reply (the UDP payload received on port 68).
 * auto m = structo::net::parse_dhcp(udp_payload);
 * if (m && m->type == structo::net::dhcp_offer)
 *   use(m->your_address, m->server_id, m->lease_seconds);
 * @endcode
 */

#include "../hw/net_device_ref.hpp"
#include "ipv4.hpp"

#if RELOCO_HAS_COROUTINES

namespace structo::net {

inline constexpr std::uint16_t dhcp_server_port = 67;
inline constexpr std::uint16_t dhcp_client_port = 68;

inline constexpr std::uint8_t dhcp_discover = 1;
inline constexpr std::uint8_t dhcp_offer = 2;
inline constexpr std::uint8_t dhcp_request = 3;
inline constexpr std::uint8_t dhcp_ack = 5;
inline constexpr std::uint8_t dhcp_nak = 6;

inline constexpr std::size_t dhcp_fixed_size = 236; ///< BOOTP header up to (excluding) the magic cookie.
inline constexpr std::size_t dhcp_max_request_size = 320; ///< Always enough for `build_dhcp`.

/** @brief What the client wants to send. */
struct dhcp_request_fields {
  std::uint8_t type = dhcp_discover; ///< `dhcp_discover` or `dhcp_request`.
  std::uint32_t xid = 0;
  hw::net_mac_address mac{};
  ipv4_address client_address{};    ///< ciaddr: set only when renewing an address we hold.
  ipv4_address requested_address{}; ///< Option 50 (REQUEST answering an OFFER); unspecified to omit.
  ipv4_address server_id{};         ///< Option 54 (REQUEST answering an OFFER); unspecified to omit.
};

/** @brief Fields of a server reply we care about. */
struct dhcp_message {
  std::uint8_t type = 0; ///< Option 53.
  std::uint32_t xid = 0;
  hw::net_mac_address mac{}; ///< chaddr.
  ipv4_address your_address{};
  ipv4_address server_id{};
  ipv4_address netmask{};
  ipv4_address gateway{}; ///< First router (option 3).
  ipv4_address dns{};     ///< First DNS server (option 6).
  std::uint32_t lease_seconds = 0; ///< Option 51; 0 if absent.
  std::uint32_t renew_seconds = 0; ///< T1, option 58; 0 if absent.
};

/**
 * @brief Builds a client message (BOOTREQUEST, broadcast flag set so replies
 * arrive even before we own an address) into `out`; returns bytes written
 * (at most `dhcp_max_request_size`) or `error::out_of_range`.
 */
[[nodiscard]] inline result<std::size_t> build_dhcp(const dhcp_request_fields &r, span<std::uint8_t> out) noexcept {
  if (out.size() < dhcp_max_request_size)
    return unexpected(error::out_of_range);
  for (std::size_t i = 0; i < dhcp_max_request_size; ++i)
    out[i] = 0;
  out[0] = 1; // BOOTREQUEST
  out[1] = 1; // Ethernet hardware type (also what SLIP/PPP clients conventionally send)
  out[2] = 6;
  for (std::size_t i = 0; i < 4; ++i)
    out[4 + i] = static_cast<std::uint8_t>(r.xid >> (24 - 8 * i));
  out[10] = 0x80; // flags: broadcast
  for (std::size_t i = 0; i < 4; ++i)
    out[12 + i] = r.client_address.octets[i];
  for (std::size_t i = 0; i < 6; ++i)
    out[28 + i] = r.mac[i];
  std::size_t n = dhcp_fixed_size;
  out[n++] = 0x63;
  out[n++] = 0x82;
  out[n++] = 0x53;
  out[n++] = 0x63;
  out[n++] = 53; // message type
  out[n++] = 1;
  out[n++] = r.type;
  out[n++] = 61; // client identifier: hardware type + MAC
  out[n++] = 7;
  out[n++] = 1;
  for (std::size_t i = 0; i < 6; ++i)
    out[n++] = r.mac[i];
  if (!r.requested_address.is_unspecified()) {
    out[n++] = 50;
    out[n++] = 4;
    for (std::size_t i = 0; i < 4; ++i)
      out[n++] = r.requested_address.octets[i];
  }
  if (!r.server_id.is_unspecified()) {
    out[n++] = 54;
    out[n++] = 4;
    for (std::size_t i = 0; i < 4; ++i)
      out[n++] = r.server_id.octets[i];
  }
  out[n++] = 55; // parameter request list: mask, router, DNS, lease, T1, T2
  out[n++] = 6;
  for (std::uint8_t o : {std::uint8_t{1}, std::uint8_t{3}, std::uint8_t{6}, std::uint8_t{51}, std::uint8_t{58},
                         std::uint8_t{59}})
    out[n++] = o;
  out[n++] = 255;
  return n;
}

/**
 * @brief Parses a BOOTREPLY.
 * `error::invalid_argument` if truncated, not a reply, lacking the magic
 * cookie or message type, or with a malformed option.
 */
[[nodiscard]] inline result<dhcp_message> parse_dhcp(span<const std::uint8_t> p) noexcept {
  if (p.size() < dhcp_fixed_size + 4 || p[0] != 2 || p[1] != 1 || p[2] != 6 || p[dhcp_fixed_size] != 0x63 ||
      p[dhcp_fixed_size + 1] != 0x82 || p[dhcp_fixed_size + 2] != 0x53 || p[dhcp_fixed_size + 3] != 0x63)
    return unexpected(error::invalid_argument);
  dhcp_message m;
  for (std::size_t i = 0; i < 4; ++i)
    m.xid = (m.xid << 8) | p[4 + i];
  m.your_address = ipv4_address(p[16], p[17], p[18], p[19]);
  for (std::size_t i = 0; i < 6; ++i)
    m.mac[i] = p[28 + i];

  auto ip_at = [&](std::size_t o) { return ipv4_address(p[o], p[o + 1], p[o + 2], p[o + 3]); };
  auto u32_at = [&](std::size_t o) {
    return (static_cast<std::uint32_t>(p[o]) << 24) | (static_cast<std::uint32_t>(p[o + 1]) << 16) |
           (static_cast<std::uint32_t>(p[o + 2]) << 8) | p[o + 3];
  };
  std::size_t i = dhcp_fixed_size + 4;
  while (i < p.size()) {
    const std::uint8_t code = p[i++];
    if (code == 0)
      continue;
    if (code == 255)
      break;
    if (i >= p.size())
      return unexpected(error::invalid_argument);
    const std::size_t len = p[i++];
    if (i + len > p.size())
      return unexpected(error::invalid_argument);
    switch (code) {
    case 53:
      if (len == 1)
        m.type = p[i];
      break;
    case 54:
      if (len == 4)
        m.server_id = ip_at(i);
      break;
    case 1:
      if (len == 4)
        m.netmask = ip_at(i);
      break;
    case 3:
      if (len >= 4)
        m.gateway = ip_at(i);
      break;
    case 6:
      if (len >= 4)
        m.dns = ip_at(i);
      break;
    case 51:
      if (len == 4)
        m.lease_seconds = u32_at(i);
      break;
    case 58:
      if (len == 4)
        m.renew_seconds = u32_at(i);
      break;
    default:
      break;
    }
    i += len;
  }
  if (m.type == 0)
    return unexpected(error::invalid_argument);
  return m;
}

} // namespace structo::net

#endif // RELOCO_HAS_COROUTINES
