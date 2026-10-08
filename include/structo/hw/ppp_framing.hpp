// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file ppp_framing.hpp
 * @brief PPP in HDLC-like framing (RFC 1662): FCS-16, `ppp_encode` and the
 * incremental `ppp_decoder`. Allocation-free, sans-IO. Part of the C++20-only
 * network stack (empty otherwise).
 *
 * Wire format: `7E FF 03 <protocol:2> <payload> <FCS:2 LE> 7E`, with `7E`,
 * `7D` and every byte below 0x20 sent as `7D, byte ^ 0x20` (the default
 * async control character map, which is always safe to over-apply). The
 * decoder also accepts frames with address/control field compression
 * (ACFC) and protocol field compression (PFC) and any escaping.
 *
 * @code
 * reloco::array<std::uint8_t, 64> wire;
 * const std::uint8_t ip_datagram[] = {0x45, 0x00 ...};
 * // Encode an IPv4 datagram (protocol 0x0021) into `wire`; returns the byte count.
 * auto n = structo::hw::ppp_encode(structo::hw::ppp_proto_ip, ip_datagram, wire);
 *
 * reloco::array<std::uint8_t, 128> rx;      // decoder's frame buffer: the largest frame it can hold
 * structo::hw::ppp_decoder dec{rx};
 * for (std::uint8_t b : bytes_from_uart)    // feed bytes as they arrive
 *   if (dec.push(b)) {                      // true: a complete, FCS-valid frame is ready
 *     auto f = dec.frame();                 // f.protocol and f.payload, valid until the next push()
 *   }
 * @endcode
 */

#include <reloco/coroutine.hpp>

#if RELOCO_HAS_COROUTINES

#include <reloco/error.hpp>
#include <reloco/span.hpp>

#include <cstddef>
#include <cstdint>

namespace structo::hw {

inline constexpr std::uint8_t ppp_flag = 0x7E;
inline constexpr std::uint8_t ppp_escape = 0x7D;
inline constexpr std::uint8_t ppp_address = 0xFF;
inline constexpr std::uint8_t ppp_control = 0x03;
inline constexpr std::uint16_t ppp_fcs_init = 0xFFFF;
inline constexpr std::uint16_t ppp_fcs_good = 0xF0B8; ///< Residue of a frame that includes its own FCS.

inline constexpr std::uint16_t ppp_proto_ip = 0x0021;
inline constexpr std::uint16_t ppp_proto_ipcp = 0x8021;
inline constexpr std::uint16_t ppp_proto_lcp = 0xC021;

/** @brief Continues the RFC 1662 16-bit FCS (reflected CRC-CCITT, poly 0x8408) over `data`. */
[[nodiscard]] constexpr std::uint16_t ppp_fcs16(std::uint16_t fcs, reloco::span<const std::uint8_t> data) noexcept {
  for (std::uint8_t b : data) {
    fcs = static_cast<std::uint16_t>(fcs ^ b);
    for (int i = 0; i < 8; ++i)
      fcs = static_cast<std::uint16_t>((fcs & 1u) ? (fcs >> 1) ^ 0x8408u : fcs >> 1);
  }
  return fcs;
}

/** @brief Worst-case encoded size of a `payload`-byte frame (everything escaped). */
[[nodiscard]] constexpr std::size_t ppp_max_encoded_size(std::size_t payload) noexcept {
  return (4 + payload + 2) * 2 + 2;
}

namespace detail {

// Appends `b` (escaped when needed) to `out` at `n`; the caller guarantees room.
constexpr void ppp_put(reloco::span<std::uint8_t> out, std::size_t &n, std::uint8_t b) noexcept {
  if (b < 0x20 || b == ppp_escape || b == ppp_flag) {
    out[n++] = ppp_escape;
    out[n++] = static_cast<std::uint8_t>(b ^ 0x20);
  } else {
    out[n++] = b;
  }
}

} // namespace detail

/**
 * @brief Encodes one PPP frame (`protocol` + `payload`) into `out`.
 * @return Bytes written, or `error::out_of_range` if `out` is smaller than `ppp_max_encoded_size(payload.size())`.
 */
[[nodiscard]] inline reloco::result<std::size_t> ppp_encode(std::uint16_t protocol,
                                                            reloco::span<const std::uint8_t> payload,
                                                            reloco::span<std::uint8_t> out) noexcept {
  if (out.size() < ppp_max_encoded_size(payload.size()))
    return reloco::unexpected(reloco::error::out_of_range);
  const std::uint8_t head[4] = {ppp_address, ppp_control, static_cast<std::uint8_t>(protocol >> 8),
                                static_cast<std::uint8_t>(protocol)};
  std::uint16_t fcs = ppp_fcs16(ppp_fcs_init, reloco::span<const std::uint8_t>(head, 4));
  fcs = ppp_fcs16(fcs, payload);
  fcs = static_cast<std::uint16_t>(~fcs);
  std::size_t n = 0;
  out[n++] = ppp_flag;
  for (std::uint8_t b : head)
    detail::ppp_put(out, n, b);
  for (std::uint8_t b : payload)
    detail::ppp_put(out, n, b);
  detail::ppp_put(out, n, static_cast<std::uint8_t>(fcs));
  detail::ppp_put(out, n, static_cast<std::uint8_t>(fcs >> 8));
  out[n++] = ppp_flag;
  return n;
}

/** @brief A decoded frame: the PPP protocol number and the information field. */
struct ppp_frame {
  std::uint16_t protocol = 0;
  reloco::span<const std::uint8_t> payload;
};

/**
 * @brief Incremental PPP/HDLC decoder writing into a caller-supplied buffer.
 *
 * Feed bytes with `push()`; it returns `true` when a complete frame with a
 * valid FCS is available via `frame()` (valid until the next `push()`).
 * Frames with a bad FCS, an aborted escape, a bad header or longer than the
 * buffer are discarded at their closing flag and counted in `dropped()`.
 */
class ppp_decoder {
public:
  explicit ppp_decoder(reloco::span<std::uint8_t> buf) noexcept : buf_(buf) {}

  [[nodiscard]] bool push(std::uint8_t b) noexcept {
    if (done_) {
      len_ = 0;
      done_ = false;
    }
    if (b == ppp_flag) {
      const bool had_data = len_ > 0 || bad_ || esc_;
      const bool ok = !bad_ && !esc_ && len_ > 0 && finish();
      if (had_data && !ok)
        ++dropped_;
      bad_ = esc_ = false;
      if (ok) {
        done_ = true;
        return true;
      }
      len_ = 0;
      return false;
    }
    if (bad_)
      return false;
    if (b == ppp_escape) {
      esc_ = true;
      return false;
    }
    if (esc_) {
      esc_ = false;
      b = static_cast<std::uint8_t>(b ^ 0x20);
    }
    if (len_ == buf_.size()) {
      bad_ = true;
      return false;
    }
    buf_[len_++] = b;
    return false;
  }

  [[nodiscard]] ppp_frame frame() const noexcept { return done_ ? frame_ : ppp_frame{}; }
  [[nodiscard]] std::size_t dropped() const noexcept { return dropped_; }

  void reset() noexcept {
    len_ = 0;
    done_ = esc_ = bad_ = false;
  }

private:
  // Validates the FCS and header, and fills `frame_`.
  bool finish() noexcept {
    if (len_ < 4 || ppp_fcs16(ppp_fcs_init, reloco::span<const std::uint8_t>(buf_.data(), len_)) != ppp_fcs_good)
      return false;
    std::size_t pos = 0;
    const std::size_t end = len_ - 2; // strip the FCS
    if (buf_[0] == ppp_address) {
      if (buf_[1] != ppp_control)
        return false;
      pos = 2;
    }
    if (pos >= end)
      return false;
    std::uint16_t proto = buf_[pos++];
    if ((proto & 1u) == 0) { // two-byte protocol field (an odd first byte means PFC)
      if (pos >= end)
        return false;
      proto = static_cast<std::uint16_t>((proto << 8) | buf_[pos++]);
    }
    frame_.protocol = proto;
    frame_.payload = reloco::span<const std::uint8_t>(buf_.data() + pos, end - pos);
    return true;
  }

  reloco::span<std::uint8_t> buf_;
  ppp_frame frame_{};
  std::size_t len_ = 0;
  std::size_t dropped_ = 0;
  bool done_ = false;
  bool esc_ = false;
  bool bad_ = false;
};

} // namespace structo::hw

#endif // RELOCO_HAS_COROUTINES
