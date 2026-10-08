// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file icmp.hpp
 * @brief ICMPv4 echo (ping) request/reply parse and build. C++20 only (empty
 * otherwise). Only echo is modelled; other ICMP types are ignored by the
 * stack.
 *
 * @code
 * // `icmp_bytes` is the payload of an IPv4 packet whose protocol is ip_proto_icmp.
 * auto echo = structo::net::parse_icmp_echo(icmp_bytes); // validates checksum
 * if (echo && echo->type == structo::net::icmp_echo_request) {
 *   // Reply with the same id/seq/data; `out` must not overlap `data`.
 *   auto n = structo::net::build_icmp_echo(structo::net::icmp_echo_reply, echo->id, echo->seq, echo->data, out);
 * }
 * @endcode
 */

#include "ipv4.hpp"

#if RELOCO_HAS_COROUTINES

namespace structo::net {

inline constexpr std::uint8_t icmp_echo_reply = 0;
inline constexpr std::uint8_t icmp_echo_request = 8;
inline constexpr std::size_t icmp_echo_header_size = 8;

/** @brief Decoded echo message; `data` views the buffer given to `parse_icmp_echo`. */
struct icmp_echo {
  std::uint8_t type = 0; ///< `icmp_echo_request` or `icmp_echo_reply`.
  std::uint16_t id = 0;
  std::uint16_t seq = 0;
  span<const std::uint8_t> data;
};

/**
 * @brief Decodes an ICMP echo request/reply.
 * `error::invalid_argument` if truncated or the checksum is wrong;
 * `error::unsupported_operation` for any other ICMP type/code.
 */
[[nodiscard]] inline result<icmp_echo> parse_icmp_echo(span<const std::uint8_t> m) noexcept {
  if (m.size() < icmp_echo_header_size || internet_checksum(m) != 0)
    return unexpected(error::invalid_argument);
  if ((m[0] != icmp_echo_request && m[0] != icmp_echo_reply) || m[1] != 0)
    return unexpected(error::unsupported_operation);
  icmp_echo e;
  e.type = m[0];
  e.id = static_cast<std::uint16_t>((m[4] << 8) | m[5]);
  e.seq = static_cast<std::uint16_t>((m[6] << 8) | m[7]);
  e.data = m.subspan(icmp_echo_header_size);
  return e;
}

/** @brief Builds an echo message (checksum computed) into `out`; returns bytes written. */
[[nodiscard]] inline result<std::size_t> build_icmp_echo(std::uint8_t type, std::uint16_t id, std::uint16_t seq,
                                                         span<const std::uint8_t> data,
                                                         span<std::uint8_t> out) noexcept {
  const std::size_t total = icmp_echo_header_size + data.size();
  if (out.size() < total)
    return unexpected(error::out_of_range);
  out[0] = type;
  out[1] = 0;
  out[2] = out[3] = 0;
  out[4] = static_cast<std::uint8_t>(id >> 8);
  out[5] = static_cast<std::uint8_t>(id);
  out[6] = static_cast<std::uint8_t>(seq >> 8);
  out[7] = static_cast<std::uint8_t>(seq);
  for (std::size_t i = 0; i < data.size(); ++i)
    out[icmp_echo_header_size + i] = data[i];
  const std::uint16_t csum = internet_checksum(span<const std::uint8_t>(out.data(), total));
  out[2] = static_cast<std::uint8_t>(csum >> 8);
  out[3] = static_cast<std::uint8_t>(csum);
  return total;
}

} // namespace structo::net

#endif // RELOCO_HAS_COROUTINES
