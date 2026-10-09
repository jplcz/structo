// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file tftp.hpp
 * @brief TFTP (RFC 1350) packet codec: RRQ/WRQ build, DATA/ACK/ERROR build
 * and parse. Octet mode, 512-byte blocks, no options. C++20 only.
 *
 * @code
 * // Request to read "boot.bin" (octet mode); `pkt` must hold at least 2 + name + 1 + 6 bytes.
 * reloco::array<std::uint8_t, structo::net::tftp_max_packet> pkt{};
 * auto n = structo::net::build_tftp_request(structo::net::tftp_rrq, "boot.bin", pkt);
 *
 * // Decode a reply (the UDP payload) from the server.
 * auto p = structo::net::parse_tftp(udp.payload);
 * if (p && p->opcode == structo::net::tftp_data) consume(p->block, p->data);
 * @endcode
 */

#include "ipv4.hpp"

#if RELOCO_HAS_COROUTINES

#include <reloco/string_view.hpp>

namespace structo::net {

inline constexpr std::uint16_t tftp_server_port = 69;
inline constexpr std::size_t tftp_block_size = 512;
inline constexpr std::size_t tftp_max_packet = 4 + tftp_block_size;

enum tftp_opcode : std::uint16_t { tftp_rrq = 1, tftp_wrq = 2, tftp_data = 3, tftp_ack = 4, tftp_error = 5 };

/** @brief Decoded server-to-client packet; `data`/`message` view the parsed buffer. */
struct tftp_packet {
  std::uint16_t opcode = 0;
  std::uint16_t block = 0;      ///< DATA/ACK block number
  std::uint16_t error_code = 0; ///< ERROR code (RFC 1350 appendix)
  span<const std::uint8_t> data;
  reloco::string_view message; ///< ERROR text (may be empty)
};

namespace detail {
inline void tftp_put16(span<std::uint8_t> out, std::size_t at, std::uint16_t v) noexcept {
  out[at] = static_cast<std::uint8_t>(v >> 8);
  out[at + 1] = static_cast<std::uint8_t>(v);
}
} // namespace detail

/** @brief Builds an RRQ or WRQ (`op`) for `filename` in octet mode; `error::out_of_range` if it doesn't fit,
 * `error::invalid_argument` for an empty name, a NUL in it, or a different opcode. */
[[nodiscard]] inline result<std::size_t> build_tftp_request(std::uint16_t op, reloco::string_view filename,
                                                            span<std::uint8_t> out) noexcept {
  constexpr reloco::string_view mode = "octet";
  if ((op != tftp_rrq && op != tftp_wrq) || filename.empty() || filename.find('\0') != reloco::string_view::npos)
    return unexpected(error::invalid_argument);
  const std::size_t total = 2 + filename.size() + 1 + mode.size() + 1;
  if (out.size() < total)
    return unexpected(error::out_of_range);
  detail::tftp_put16(out, 0, op);
  std::size_t at = 2;
  for (char c : filename)
    out[at++] = static_cast<std::uint8_t>(c);
  out[at++] = 0;
  for (char c : mode)
    out[at++] = static_cast<std::uint8_t>(c);
  out[at++] = 0;
  return at;
}

/** @brief Builds DATA(`block`, `data`); `data` is at most 512 bytes (`error::out_of_range` otherwise or if `out` is
 * small). */
[[nodiscard]] inline result<std::size_t> build_tftp_data(std::uint16_t block, span<const std::uint8_t> data,
                                                         span<std::uint8_t> out) noexcept {
  if (data.size() > tftp_block_size || out.size() < 4 + data.size())
    return unexpected(error::out_of_range);
  detail::tftp_put16(out, 0, tftp_data);
  detail::tftp_put16(out, 2, block);
  for (std::size_t i = 0; i < data.size(); ++i)
    out[4 + i] = data[i];
  return 4 + data.size();
}

/** @brief Builds ACK(`block`). */
[[nodiscard]] inline result<std::size_t> build_tftp_ack(std::uint16_t block, span<std::uint8_t> out) noexcept {
  if (out.size() < 4)
    return unexpected(error::out_of_range);
  detail::tftp_put16(out, 0, tftp_ack);
  detail::tftp_put16(out, 2, block);
  return std::size_t{4};
}

/** @brief Builds ERROR(`code`, `message`). */
[[nodiscard]] inline result<std::size_t> build_tftp_error(std::uint16_t code, reloco::string_view message,
                                                          span<std::uint8_t> out) noexcept {
  if (out.size() < 5 + message.size())
    return unexpected(error::out_of_range);
  detail::tftp_put16(out, 0, tftp_error);
  detail::tftp_put16(out, 2, code);
  std::size_t at = 4;
  for (char c : message)
    out[at++] = static_cast<std::uint8_t>(c);
  out[at++] = 0;
  return at;
}

/** @brief Decodes DATA/ACK/ERROR; `error::invalid_argument` if malformed, `error::unsupported_operation`
 * for other opcodes (requests, OACK). */
[[nodiscard]] inline result<tftp_packet> parse_tftp(span<const std::uint8_t> d) noexcept {
  if (d.size() < 4)
    return unexpected(error::invalid_argument);
  tftp_packet p;
  p.opcode = static_cast<std::uint16_t>((d[0] << 8) | d[1]);
  const auto word = static_cast<std::uint16_t>((d[2] << 8) | d[3]);
  switch (p.opcode) {
  case tftp_data:
    if (d.size() > tftp_max_packet)
      return unexpected(error::invalid_argument);
    p.block = word;
    p.data = d.subspan(4);
    return p;
  case tftp_ack:
    if (d.size() != 4)
      return unexpected(error::invalid_argument);
    p.block = word;
    return p;
  case tftp_error: {
    p.error_code = word;
    std::size_t len = d.size() - 4;
    if (len > 0 && d[d.size() - 1] == 0)
      --len; // strip the terminator
    const auto tail = d.subspan(4);
    p.message = reloco::string_view(reinterpret_cast<const char *>(tail.data()), len);
    return p;
  }
  default:
    return unexpected(error::unsupported_operation);
  }
}

} // namespace structo::net

#endif // RELOCO_HAS_COROUTINES
