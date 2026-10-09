// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file arp.hpp
 * @brief ARP (RFC 826) packet codec for Ethernet/IPv4. C++20 only (empty otherwise).
 *
 * @code
 * // Ask "who has 10.0.0.1?" as 10.0.0.2 (our MAC is `mac`); `out` receives the 28-byte ARP payload
 * // that goes into an Ethernet frame with ethertype structo::net::arp_ethertype.
 * structo::net::arp_packet req{structo::net::arp_request, mac, {10, 0, 0, 2}, {}, {10, 0, 0, 1}};
 * reloco::array<std::uint8_t, structo::net::arp_packet_size> out{};
 * auto n = structo::net::build_arp(req, out);
 *
 * // Decode a received payload; rejects anything that isn't Ethernet/IPv4 ARP.
 * auto p = structo::net::parse_arp(payload);
 * @endcode
 */

#include "ipv4.hpp"

#if RELOCO_HAS_COROUTINES

#include "../hw/net_device_ref.hpp"

namespace structo::net {

inline constexpr std::uint16_t arp_ethertype = 0x0806;
inline constexpr std::size_t arp_packet_size = 28;
inline constexpr std::uint16_t arp_request = 1;
inline constexpr std::uint16_t arp_reply = 2;

/** @brief Decoded Ethernet/IPv4 ARP packet. */
struct arp_packet {
  std::uint16_t op = 0;
  hw::net_mac_address sender_mac{};
  ipv4_address sender_ip;
  hw::net_mac_address target_mac{}; ///< all zeros in requests
  ipv4_address target_ip;
};

/** @brief Builds `p` into `out`; returns 28, or `error::out_of_range` if `out` is smaller. */
[[nodiscard]] inline result<std::size_t> build_arp(const arp_packet &p, span<std::uint8_t> out) noexcept {
  if (out.size() < arp_packet_size)
    return unexpected(error::out_of_range);
  out[0] = 0;
  out[1] = 1; // hardware type: Ethernet
  out[2] = 0x08;
  out[3] = 0x00; // protocol type: IPv4
  out[4] = 6;
  out[5] = 4;
  out[6] = static_cast<std::uint8_t>(p.op >> 8);
  out[7] = static_cast<std::uint8_t>(p.op);
  for (std::size_t i = 0; i < 6; ++i) {
    out[8 + i] = p.sender_mac[i];
    out[18 + i] = p.target_mac[i];
  }
  for (std::size_t i = 0; i < 4; ++i) {
    out[14 + i] = p.sender_ip.octets[i];
    out[24 + i] = p.target_ip.octets[i];
  }
  return arp_packet_size;
}

/** @brief Decodes an ARP payload; `error::invalid_argument` if short or not Ethernet/IPv4, or the opcode isn't
 * request/reply. */
[[nodiscard]] inline result<arp_packet> parse_arp(span<const std::uint8_t> d) noexcept {
  if (d.size() < arp_packet_size || d[0] != 0 || d[1] != 1 || d[2] != 0x08 || d[3] != 0x00 || d[4] != 6 || d[5] != 4)
    return unexpected(error::invalid_argument);
  arp_packet p;
  p.op = static_cast<std::uint16_t>((d[6] << 8) | d[7]);
  if (p.op != arp_request && p.op != arp_reply)
    return unexpected(error::invalid_argument);
  for (std::size_t i = 0; i < 6; ++i) {
    p.sender_mac[i] = d[8 + i];
    p.target_mac[i] = d[18 + i];
  }
  for (std::size_t i = 0; i < 4; ++i) {
    p.sender_ip.octets[i] = d[14 + i];
    p.target_ip.octets[i] = d[24 + i];
  }
  return p;
}

} // namespace structo::net

#endif // RELOCO_HAS_COROUTINES
