// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file ethernet.hpp
 * @brief Ethernet II frame codec (14-byte header, no VLAN/FCS) and MAC
 * helpers, used by `hw::ethernet_device`. C++20 only (empty otherwise).
 *
 * @code
 * // Wrap an IPv4 datagram for `dst_mac`; `frame` receives header + payload and the return value is
 * // the frame length to hand to the NIC.
 * reloco::array<std::uint8_t, 1514> frame{};
 * auto n = structo::net::build_ethernet(dst_mac, my_mac, structo::net::ipv4_ethertype, datagram, frame);
 *
 * // Decode a received frame; `payload` views the bytes after the header (it may include
 * // trailing padding -- IPv4 consumers trim it with the datagram's own length field).
 * auto f = structo::net::parse_ethernet(rx_frame);
 * @endcode
 */

#include "ipv4.hpp"

#if RELOCO_HAS_COROUTINES

#include "../hw/net_device_ref.hpp"

namespace structo::net {

inline constexpr std::uint16_t ipv4_ethertype = 0x0800;
inline constexpr std::size_t ethernet_header_size = 14;

inline constexpr hw::net_mac_address ethernet_broadcast_mac{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/** @brief Decoded Ethernet II frame; `payload` views the input buffer. */
struct ethernet_frame {
  hw::net_mac_address dst{};
  hw::net_mac_address src{};
  std::uint16_t ethertype = 0;
  span<const std::uint8_t> payload;
};

/** @brief Group (multicast/broadcast) address: low bit of the first octet set. */
[[nodiscard]] constexpr bool is_group_mac(const hw::net_mac_address &m) noexcept { return (m[0] & 1) != 0; }

/** @brief The Ethernet multicast MAC for an IPv4 multicast group (RFC 1112): `01:00:5e` + low 23 bits. */
[[nodiscard]] constexpr hw::net_mac_address ipv4_multicast_mac(const ipv4_address &group) noexcept {
  return {0x01, 0x00, 0x5E, static_cast<std::uint8_t>(group.octets[1] & 0x7F), group.octets[2], group.octets[3]};
}

/** @brief Builds a frame into `out`; returns its length, or `error::out_of_range` if `out` is too small. */
[[nodiscard]] inline result<std::size_t> build_ethernet(const hw::net_mac_address &dst, const hw::net_mac_address &src,
                                                        std::uint16_t ethertype, span<const std::uint8_t> payload,
                                                        span<std::uint8_t> out) noexcept {
  if (out.size() < ethernet_header_size || out.size() - ethernet_header_size < payload.size())
    return unexpected(error::out_of_range);
  for (std::size_t i = 0; i < 6; ++i) {
    out[i] = dst[i];
    out[6 + i] = src[i];
  }
  out[12] = static_cast<std::uint8_t>(ethertype >> 8);
  out[13] = static_cast<std::uint8_t>(ethertype);
  for (std::size_t i = 0; i < payload.size(); ++i)
    out[ethernet_header_size + i] = payload[i];
  return ethernet_header_size + payload.size();
}

/** @brief Decodes a frame; `error::invalid_argument` if shorter than the header. */
[[nodiscard]] inline result<ethernet_frame> parse_ethernet(span<const std::uint8_t> d) noexcept {
  if (d.size() < ethernet_header_size)
    return unexpected(error::invalid_argument);
  ethernet_frame f;
  for (std::size_t i = 0; i < 6; ++i) {
    f.dst[i] = d[i];
    f.src[i] = d[6 + i];
  }
  f.ethertype = static_cast<std::uint16_t>((d[12] << 8) | d[13]);
  f.payload = d.subspan(ethernet_header_size);
  return f;
}

} // namespace structo::net

#endif // RELOCO_HAS_COROUTINES
