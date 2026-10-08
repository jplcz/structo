// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file ipv4.hpp
 * @brief Allocation-free IPv4 primitives: `ipv4_address`, the RFC 1071
 * Internet checksum, and header parse/build. C++20 only (empty otherwise;
 * `RELOCO_HAS_COROUTINES` is 0), like the rest of the network stack.
 *
 * Scope is deliberately small: options are skipped on receive and never
 * generated, and fragmented packets are rejected (`error::unsupported_operation`)
 * -- reassembly is out of scope.
 *
 * @code
 * using namespace structo::net;
 * constexpr ipv4_address me{192, 168, 7, 2};
 *
 * // Parse a received datagram (e.g. a frame from net_device_ref::receive).
 * auto pkt = parse_ipv4(frame);          // validates version/IHL/length/checksum
 * if (pkt && pkt->header.dst == me && pkt->header.protocol == ip_proto_udp)
 *   handle_udp(pkt->payload);            // payload view into `frame`
 *
 * // Build one: protocol, src, dst, payload; `out` must not overlap `payload`.
 * reloco::array<std::uint8_t, 128> out;
 * auto n = build_ipv4({ip_proto_udp, me, peer}, payload, out); // -> bytes written
 * @endcode
 */

#include <reloco/coroutine.hpp>

#if RELOCO_HAS_COROUTINES

#include <reloco/error.hpp>
#include <reloco/span.hpp>

#include <reloco/array.hpp>
#include <cstddef>
#include <cstdint>

namespace structo::net {

using namespace reloco;

inline constexpr std::uint8_t ip_proto_icmp = 1;
inline constexpr std::uint8_t ip_proto_tcp = 6;
inline constexpr std::uint8_t ip_proto_udp = 17;

inline constexpr std::size_t ipv4_header_size = 20; ///< Header without options.

/** @brief IPv4 address in network byte order. */
struct ipv4_address {
  reloco::array<std::uint8_t, 4> octets{};

  constexpr ipv4_address() noexcept = default;
  constexpr ipv4_address(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) noexcept
      : octets{a, b, c, d} {}

  [[nodiscard]] constexpr bool is_unspecified() const noexcept {
    return octets[0] == 0 && octets[1] == 0 && octets[2] == 0 && octets[3] == 0;
  }
  [[nodiscard]] constexpr bool is_broadcast() const noexcept {
    return octets[0] == 255 && octets[1] == 255 && octets[2] == 255 && octets[3] == 255;
  }
  [[nodiscard]] friend constexpr bool operator==(const ipv4_address &a, const ipv4_address &b) noexcept {
    return a.octets == b.octets;
  }
  [[nodiscard]] friend constexpr bool operator!=(const ipv4_address &a, const ipv4_address &b) noexcept {
    return !(a == b);
  }
};

/**
 * @brief RFC 1071 Internet checksum (ones'-complement sum of big-endian
 * 16-bit words, odd trailing byte padded with zero) of `data`, returned
 * already complemented. Verifying a block that contains its own checksum
 * yields 0.
 * @param initial Partial (uncomplemented, folded-later) sum to continue from,
 * e.g. a pseudo-header sum.
 */
[[nodiscard]] constexpr std::uint16_t internet_checksum(span<const std::uint8_t> data,
                                                        std::uint32_t initial = 0) noexcept {
  std::uint32_t sum = initial;
  std::size_t i = 0;
  for (; i + 1 < data.size(); i += 2)
    sum += static_cast<std::uint32_t>((data[i] << 8) | data[i + 1]);
  if (i < data.size())
    sum += static_cast<std::uint32_t>(data[i] << 8);
  while (sum >> 16)
    sum = (sum & 0xFFFFu) + (sum >> 16);
  return static_cast<std::uint16_t>(~sum & 0xFFFFu);
}

/** @brief Partial sum of the TCP/UDP pseudo-header, to pass as `internet_checksum`'s `initial`. */
[[nodiscard]] constexpr std::uint32_t ipv4_pseudo_header_sum(const ipv4_address &src, const ipv4_address &dst,
                                                             std::uint8_t protocol, std::size_t length) noexcept {
  std::uint32_t sum = protocol;
  sum += static_cast<std::uint32_t>(length);
  for (std::size_t i = 0; i < 4; i += 2) {
    sum += static_cast<std::uint32_t>((src.octets[i] << 8) | src.octets[i + 1]);
    sum += static_cast<std::uint32_t>((dst.octets[i] << 8) | dst.octets[i + 1]);
  }
  return sum;
}

/** @brief Fields of an IPv4 header the stack cares about. */
struct ipv4_header {
  std::uint8_t protocol = 0;
  ipv4_address src{};
  ipv4_address dst{};
  std::uint16_t identification = 0;
  std::uint8_t ttl = 64;
  std::uint8_t header_length = ipv4_header_size; ///< Bytes, set by `parse_ipv4`; ignored by `build_ipv4`.
};

/** @brief A validated packet; `payload` views the buffer passed to `parse_ipv4`. */
struct ipv4_packet {
  ipv4_header header;
  span<const std::uint8_t> payload;
};

/**
 * @brief Validates and decodes an IPv4 packet.
 * Fails with `error::invalid_argument` (truncated, wrong version, bad IHL,
 * bad total length, bad header checksum) or `error::unsupported_operation`
 * (fragment). Trailing link-layer padding beyond the total length is ignored.
 */
[[nodiscard]] inline result<ipv4_packet> parse_ipv4(span<const std::uint8_t> p) noexcept {
  if (p.size() < ipv4_header_size || (p[0] >> 4) != 4)
    return unexpected(error::invalid_argument);
  const std::size_t hlen = static_cast<std::size_t>(p[0] & 0x0F) * 4;
  const std::size_t total = static_cast<std::size_t>((p[2] << 8) | p[3]);
  if (hlen < ipv4_header_size || total < hlen || total > p.size())
    return unexpected(error::invalid_argument);
  if (internet_checksum(span<const std::uint8_t>(p.data(), hlen)) != 0)
    return unexpected(error::invalid_argument);
  const unsigned flags_frag = static_cast<unsigned>((p[6] << 8) | p[7]);
  if ((flags_frag & 0x2000u) || (flags_frag & 0x1FFFu)) // MF set or non-zero offset
    return unexpected(error::unsupported_operation);

  ipv4_packet out;
  out.header.header_length = static_cast<std::uint8_t>(hlen);
  out.header.identification = static_cast<std::uint16_t>((p[4] << 8) | p[5]);
  out.header.ttl = p[8];
  out.header.protocol = p[9];
  out.header.src = ipv4_address(p[12], p[13], p[14], p[15]);
  out.header.dst = ipv4_address(p[16], p[17], p[18], p[19]);
  out.payload = span<const std::uint8_t>(p.data() + hlen, total - hlen);
  return out;
}

/**
 * @brief Writes an option-less IPv4 header (DF set, checksum computed) followed
 * by `payload` into `out`.
 * @return Bytes written, `error::out_of_range` if `out` is too small or the
 * packet would exceed 65535 bytes. `out` must not overlap `payload`.
 */
[[nodiscard]] inline result<std::size_t> build_ipv4(const ipv4_header &h, span<const std::uint8_t> payload,
                                                    span<std::uint8_t> out) noexcept {
  const std::size_t total = ipv4_header_size + payload.size();
  if (total > 0xFFFFu || out.size() < total)
    return unexpected(error::out_of_range);
  out[0] = 0x45; // version 4, IHL 5
  out[1] = 0;
  out[2] = static_cast<std::uint8_t>(total >> 8);
  out[3] = static_cast<std::uint8_t>(total);
  out[4] = static_cast<std::uint8_t>(h.identification >> 8);
  out[5] = static_cast<std::uint8_t>(h.identification);
  out[6] = 0x40; // DF, no fragmentation
  out[7] = 0;
  out[8] = h.ttl;
  out[9] = h.protocol;
  out[10] = out[11] = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    out[12 + i] = h.src.octets[i];
    out[16 + i] = h.dst.octets[i];
  }
  const std::uint16_t csum = internet_checksum(span<const std::uint8_t>(out.data(), ipv4_header_size));
  out[10] = static_cast<std::uint8_t>(csum >> 8);
  out[11] = static_cast<std::uint8_t>(csum);
  for (std::size_t i = 0; i < payload.size(); ++i)
    out[ipv4_header_size + i] = payload[i];
  return total;
}

} // namespace structo::net

#endif // RELOCO_HAS_COROUTINES
