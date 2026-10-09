// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file udp.hpp
 * @brief UDP (RFC 768) datagram parse/build with the IPv4 pseudo-header
 * checksum. C++20 only (empty otherwise).
 *
 * @code
 * // `ip_payload` is the payload of an IPv4 packet with protocol ip_proto_udp.
 * auto d = structo::net::parse_udp(ip_payload, pkt.header.src, pkt.header.dst);
 * if (d && d->dst_port == 68) use(d->payload);
 *
 * // Wrap `payload` for sending from 10.0.0.2:5000 to 10.0.0.1:7; `out` must not overlap it.
 * auto n = structo::net::build_udp(5000, 7, payload, {10, 0, 0, 2}, {10, 0, 0, 1}, out);
 * @endcode
 */

#include "ipv4.hpp"

#if RELOCO_HAS_COROUTINES

namespace structo::net {

inline constexpr std::size_t udp_header_size = 8;

/** @brief Decoded UDP datagram; `payload` views the buffer given to `parse_udp`. */
struct udp_datagram {
  std::uint16_t src_port = 0;
  std::uint16_t dst_port = 0;
  span<const std::uint8_t> payload;
};

/**
 * @brief Decodes a UDP datagram carried between `src` and `dst`.
 * `error::invalid_argument` if truncated, the length field is inconsistent,
 * or a non-zero checksum is wrong (a zero checksum means "not computed").
 */
[[nodiscard]] inline result<udp_datagram> parse_udp(span<const std::uint8_t> d, const ipv4_address &src,
                                                    const ipv4_address &dst) noexcept {
  if (d.size() < udp_header_size)
    return unexpected(error::invalid_argument);
  const std::size_t len = static_cast<std::size_t>((d[4] << 8) | d[5]);
  if (len < udp_header_size || len > d.size())
    return unexpected(error::invalid_argument);
  if ((d[6] | d[7]) != 0 && internet_checksum(span<const std::uint8_t>(d.data(), len),
                                              ipv4_pseudo_header_sum(src, dst, ip_proto_udp, len)) != 0)
    return unexpected(error::invalid_argument);
  udp_datagram out;
  out.src_port = static_cast<std::uint16_t>((d[0] << 8) | d[1]);
  out.dst_port = static_cast<std::uint16_t>((d[2] << 8) | d[3]);
  out.payload = d.subspan(udp_header_size, len - udp_header_size);
  return out;
}

/**
 * @brief Builds a UDP datagram (checksum computed over the pseudo-header of
 * `src`/`dst`) into `out`; returns bytes written, or `error::out_of_range`
 * if `out` is too small or the datagram exceeds 65535 bytes.
 */
[[nodiscard]] inline result<std::size_t> build_udp(std::uint16_t src_port, std::uint16_t dst_port,
                                                   span<const std::uint8_t> payload, const ipv4_address &src,
                                                   const ipv4_address &dst, span<std::uint8_t> out) noexcept {
  const std::size_t total = udp_header_size + payload.size();
  if (total > 0xFFFFu || out.size() < total)
    return unexpected(error::out_of_range);
  out[0] = static_cast<std::uint8_t>(src_port >> 8);
  out[1] = static_cast<std::uint8_t>(src_port);
  out[2] = static_cast<std::uint8_t>(dst_port >> 8);
  out[3] = static_cast<std::uint8_t>(dst_port);
  out[4] = static_cast<std::uint8_t>(total >> 8);
  out[5] = static_cast<std::uint8_t>(total);
  out[6] = out[7] = 0;
  for (std::size_t i = 0; i < payload.size(); ++i)
    out[udp_header_size + i] = payload[i];
  std::uint16_t csum = internet_checksum(span<const std::uint8_t>(out.data(), total),
                                         ipv4_pseudo_header_sum(src, dst, ip_proto_udp, total));
  if (csum == 0)
    csum = 0xFFFF; // 0 on the wire means "no checksum"
  out[6] = static_cast<std::uint8_t>(csum >> 8);
  out[7] = static_cast<std::uint8_t>(csum);
  return total;
}

} // namespace structo::net

#endif // RELOCO_HAS_COROUTINES
