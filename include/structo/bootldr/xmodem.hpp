// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file xmodem.hpp
 * @brief XMODEM / XMODEM-CRC / XMODEM-1K serial file transfer for
 * boot loaders: allocation-free, sans-IO receiver and sender state
 * machines plus blocking drivers over `hw::uart_ref`.
 *
 * ## Wire format
 *
 * ```
 * <SOH|STX> <blk> <255-blk> <128|1024 data bytes> <checksum | CRC16-hi CRC16-lo>
 * ```
 *
 * - `SOH` (0x01) starts a 128-byte block, `STX` (0x02) a 1024-byte one
 *   (XMODEM-1K, only valid in CRC mode).
 * - Block numbers start at 1 and wrap modulo 256.
 * - The receiver selects the mode: it sends `'C'` to request CRC-16/XMODEM
 *   (poly 0x1021, init 0), or `NAK` to request the legacy 8-bit additive
 *   checksum. It then `ACK`s every good block, `NAK`s a bad one.
 * - The sender ends with `EOT`, which the receiver `ACK`s.
 * - `CAN` (0x18) aborts. The final block is padded by the sender
 *   (conventionally with `SUB`, 0x1A); the protocol carries no length, so
 *   trailing padding is the caller's concern.
 *
 * ## Sans-IO engines
 *
 * @ref bootldr::xmodem_receiver and @ref bootldr::xmodem_sender never
 * touch a UART or a clock. The caller feeds them each received byte
 * (`on_byte`) or a timeout notification (`on_timeout`) and transmits
 * whatever `reply()` / `packet()` holds. That makes them testable without
 * hardware and usable from polled or interrupt-driven code alike. Both
 * embed their packet buffer (<= 1030 bytes), so they allocate nothing.
 *
 * ## Blocking drivers
 *
 * `bootldr::receive` and `bootldr::send` run an engine over a
 * `hw::uart_ref`, treating `uart_ref::get_byte`'s spin-budget expiry
 * (`error::timed_out`) as the protocol timeout. The spin budget is only a
 * coarse, CPU-speed dependent clock; callers needing exact timing should
 * drive the engines directly with a real timer.
 */

#include <reloco/array.hpp>
#include <reloco/error.hpp>
#include <reloco/function_ref.hpp>
#include <reloco/span.hpp>
#include <structo/hw/uart_ref.hpp>

#include <cstddef>
#include <cstdint>

namespace structo {

using namespace reloco;

/** @brief Boot-loader facilities (image transfer protocols, ...). */
namespace bootldr {

/** @brief XMODEM control bytes. */
namespace xmodem_ctl {
inline constexpr std::uint8_t soh = 0x01;
inline constexpr std::uint8_t stx = 0x02;
inline constexpr std::uint8_t eot = 0x04;
inline constexpr std::uint8_t ack = 0x06;
inline constexpr std::uint8_t nak = 0x15;
inline constexpr std::uint8_t can = 0x18;
inline constexpr std::uint8_t sub = 0x1A;
inline constexpr std::uint8_t crc_request = 'C';
} // namespace xmodem_ctl

/** @brief Largest data payload of one packet (XMODEM-1K). */
inline constexpr std::size_t xmodem_max_block = 1024;
/** @brief Payload of a classic (SOH) packet. */
inline constexpr std::size_t xmodem_small_block = 128;
/** @brief Largest on-wire packet: header(3) + payload + CRC16(2). */
inline constexpr std::size_t xmodem_max_packet = 3 + xmodem_max_block + 2;

/** @brief CRC-16/XMODEM (poly 0x1021, init 0, no reflection), bitwise. */
[[nodiscard]] constexpr std::uint16_t xmodem_crc16(span<const std::uint8_t> data, std::uint16_t crc = 0) noexcept {
  for (std::uint8_t b : data) {
    crc = static_cast<std::uint16_t>(crc ^ (static_cast<std::uint16_t>(b) << 8));
    for (int i = 0; i < 8; ++i)
      crc = static_cast<std::uint16_t>((crc & 0x8000u) ? (static_cast<unsigned>(crc << 1) ^ 0x1021u) : static_cast<unsigned>(crc << 1));
  }
  return crc;
}

/** @brief Legacy XMODEM checksum: sum of the payload bytes modulo 256. */
[[nodiscard]] constexpr std::uint8_t xmodem_checksum(span<const std::uint8_t> data) noexcept {
  std::uint8_t sum = 0;
  for (std::uint8_t b : data)
    sum = static_cast<std::uint8_t>(sum + b);
  return sum;
}

/** @brief What a receiver/sender step produced. */
enum class xmodem_event : std::uint8_t {
  none,      ///< Nothing for the caller to act on besides `reply()`.
  block,     ///< (receiver) A new in-order payload is available via `block()`.
  transmit,  ///< (sender) A packet is ready in `packet()` and must be sent.
  done,      ///< Transfer completed successfully.
  cancelled, ///< The peer sent CAN.
  failed,    ///< Retries exhausted or protocol violation; CAN is queued for sending.
};

/** @brief Tunables shared by both engines. */
struct xmodem_config {
  /** @brief Receiver: request CRC mode (`'C'`); the sender always follows the receiver's request. */
  bool use_crc = true;
  /** @brief Consecutive timeouts/NAKs tolerated before `failed`. */
  std::uint32_t max_retries = 10;
  /** @brief Receiver: after this many unanswered `'C'`, fall back to checksum mode (0 = never). */
  std::uint32_t crc_fallback_after = 3;
  /** @brief Sender: use 1024-byte packets when the receiver asked for CRC. */
  bool use_1k = true;
};

// ============================================================================
// Receiver
// ============================================================================

/**
 * @brief Byte-driven XMODEM receiver.
 *
 * @code
 * structo::bootldr::xmodem_receiver rx;
 * send(rx.start());                       // 'C' (or NAK): invites the sender
 * for (;;) {
 *   // Feed each received byte, or call on_timeout() when none arrives in time.
 *   auto ev = have_byte ? rx.on_byte(b) : rx.on_timeout();
 *   if (ev == xmodem_event::block)        // payload of the next in-order packet
 *     append_to_image(rx.block());        // valid only until the next on_byte()
 *   if (!rx.reply().empty())              // ACK/NAK/CAN/'C' to transmit
 *     send(rx.reply());
 *   if (ev == xmodem_event::done || ev == xmodem_event::failed || ev == xmodem_event::cancelled)
 *     break;
 * }
 * @endcode
 */
class xmodem_receiver {
public:
  explicit constexpr xmodem_receiver(const xmodem_config &cfg = {}) noexcept : cfg_(cfg), crc_(cfg.use_crc) {}

  /** @brief Reply that opens the transfer (`'C'` in CRC mode, else NAK). */
  [[nodiscard]] span<const std::uint8_t> start() noexcept {
    reply_[0] = crc_ ? xmodem_ctl::crc_request : xmodem_ctl::nak;
    reply_len_ = 1;
    return reply();
  }

  /** @brief Consume one received byte. */
  xmodem_event on_byte(std::uint8_t b) noexcept {
    reply_len_ = 0;
    switch (state_) {
    case state::header:
      return on_header(b);
    case state::blk:
      blk_ = b;
      state_ = state::cblk;
      return xmodem_event::none;
    case state::cblk:
      cblk_ = b;
      pos_ = 0;
      state_ = state::data;
      return xmodem_event::none;
    case state::data:
      buf_[pos_++] = b;
      if (pos_ == size_) {
        pos_ = 0;
        state_ = state::check;
      }
      return xmodem_event::none;
    case state::check:
      check_[pos_++] = b;
      if (pos_ == (crc_ ? 2u : 1u))
        return finish_packet();
      return xmodem_event::none;
    case state::finished:
      break;
    }
    return xmodem_event::none;
  }

  /**
   * @brief The peer was silent for too long.
   * Re-requests the transfer before the first packet, otherwise NAKs and
   * discards any partial packet. Fails after `max_retries` in a row.
   */
  xmodem_event on_timeout() noexcept {
    if (state_ == state::finished)
      return xmodem_event::none;
    reply_len_ = 0;
    if (++errors_ > cfg_.max_retries)
      return fail();
    state_ = state::header;
    if (!started_) {
      if (crc_ && cfg_.crc_fallback_after != 0 && errors_ >= cfg_.crc_fallback_after)
        crc_ = false;
      (void)start();
    } else {
      reply_[0] = xmodem_ctl::nak;
      reply_len_ = 1;
    }
    return xmodem_event::none;
  }

  /** @brief Bytes (0 or 1) to transmit back to the sender after the last call. */
  [[nodiscard]] span<const std::uint8_t> reply() const noexcept {
    return span<const std::uint8_t>(reply_, reply_len_);
  }

  /** @brief Payload of the packet signalled by `xmodem_event::block`; valid until the next `on_byte`. */
  [[nodiscard]] span<const std::uint8_t> block() const noexcept { return span<const std::uint8_t>(buf_.data(), size_); }

  /** @brief Packets delivered so far (duplicates excluded). */
  [[nodiscard]] constexpr std::uint32_t blocks_received() const noexcept { return delivered_; }

  /** @brief Whether CRC-16 (rather than the 8-bit checksum) is in use. */
  [[nodiscard]] constexpr bool crc_mode() const noexcept { return crc_; }

private:
  enum class state : std::uint8_t { header, blk, cblk, data, check, finished };

  xmodem_event on_header(std::uint8_t b) noexcept {
    switch (b) {
    case xmodem_ctl::soh:
      size_ = xmodem_small_block;
      state_ = state::blk;
      started_ = true;
      return xmodem_event::none;
    case xmodem_ctl::stx:
      if (!crc_)
        return reject(); // 1K packets are CRC-only
      size_ = xmodem_max_block;
      state_ = state::blk;
      started_ = true;
      return xmodem_event::none;
    case xmodem_ctl::eot:
      state_ = state::finished;
      reply_[0] = xmodem_ctl::ack;
      reply_len_ = 1;
      return xmodem_event::done;
    case xmodem_ctl::can:
      state_ = state::finished;
      return xmodem_event::cancelled;
    default:
      return xmodem_event::none; // line noise between packets
    }
  }

  xmodem_event finish_packet() noexcept {
    state_ = state::header;
    const span<const std::uint8_t> payload(buf_.data(), size_);
    bool ok = static_cast<std::uint8_t>(blk_ ^ cblk_) == 0xFF;
    if (ok) {
      if (crc_)
        ok = xmodem_crc16(payload) == static_cast<std::uint16_t>((check_[0] << 8) | check_[1]);
      else
        ok = xmodem_checksum(payload) == check_[0];
    }
    if (!ok)
      return reject();

    if (blk_ == expected_) {
      expected_ = static_cast<std::uint8_t>(expected_ + 1);
      ++delivered_;
      errors_ = 0;
      reply_[0] = xmodem_ctl::ack;
      reply_len_ = 1;
      return xmodem_event::block;
    }
    if (blk_ == static_cast<std::uint8_t>(expected_ - 1)) {
      // The sender missed our ACK and re-sent: acknowledge, deliver nothing.
      reply_[0] = xmodem_ctl::ack;
      reply_len_ = 1;
      return xmodem_event::none;
    }
    return fail(); // out-of-sequence: unrecoverable
  }

  xmodem_event reject() noexcept {
    if (++errors_ > cfg_.max_retries)
      return fail();
    state_ = state::header;
    reply_[0] = xmodem_ctl::nak;
    reply_len_ = 1;
    return xmodem_event::none;
  }

  xmodem_event fail() noexcept {
    state_ = state::finished;
    reply_[0] = xmodem_ctl::can;
    reply_len_ = 1;
    return xmodem_event::failed;
  }

  xmodem_config cfg_;
  bool crc_;
  bool started_ = false;
  state state_ = state::header;
  std::uint8_t blk_ = 0;
  std::uint8_t cblk_ = 0;
  std::uint8_t expected_ = 1;
  array<std::uint8_t, 2> check_{};
  std::uint8_t reply_[1]{};
  std::size_t reply_len_ = 0;
  std::size_t size_ = xmodem_small_block;
  std::size_t pos_ = 0;
  std::uint32_t errors_ = 0;
  std::uint32_t delivered_ = 0;
  array<std::uint8_t, xmodem_max_block> buf_{};
};

// ============================================================================
// Sender
// ============================================================================

/**
 * @brief Byte-driven XMODEM sender over an in-memory image.
 *
 * @code
 * structo::bootldr::xmodem_sender tx(image);      // image: span<const uint8_t>, must outlive tx
 * for (;;) {
 *   // Feed the receiver's response byte (or on_timeout() if none came).
 *   auto ev = have_byte ? tx.on_byte(b) : tx.on_timeout();
 *   if (ev == xmodem_event::transmit)             // next packet, or EOT
 *     send(tx.packet());
 *   else if (ev == xmodem_event::done || ev == xmodem_event::failed || ev == xmodem_event::cancelled)
 *     break;
 * }
 * @endcode
 *
 * The first transfer byte must be the receiver's `'C'` or NAK, which
 * selects CRC or checksum mode. The last block is padded with `SUB`.
 */
class xmodem_sender {
public:
  explicit constexpr xmodem_sender(span<const std::uint8_t> image, const xmodem_config &cfg = {}) noexcept
      : cfg_(cfg), image_(image) {}

  /** @brief Consume the receiver's response byte. */
  xmodem_event on_byte(std::uint8_t b) noexcept {
    switch (stage_) {
    case stage::wait_start:
      if (b == xmodem_ctl::crc_request || b == xmodem_ctl::nak) {
        crc_ = b == xmodem_ctl::crc_request;
        return build_next();
      }
      return b == xmodem_ctl::can ? cancelled() : xmodem_event::none;
    case stage::wait_ack:
      if (b == xmodem_ctl::ack) {
        errors_ = 0;
        offset_ += last_payload_;
        ++blk_;
        return build_next();
      }
      if (b == xmodem_ctl::nak)
        return retry();
      return b == xmodem_ctl::can ? cancelled() : xmodem_event::none;
    case stage::wait_eot_ack:
      if (b == xmodem_ctl::ack) {
        stage_ = stage::finished;
        return xmodem_event::done;
      }
      if (b == xmodem_ctl::nak)
        return retry();
      return b == xmodem_ctl::can ? cancelled() : xmodem_event::none;
    case stage::finished:
      break;
    }
    return xmodem_event::none;
  }

  /** @brief No response in time: retransmit the last packet (nothing to resend before the start byte). */
  xmodem_event on_timeout() noexcept {
    if (stage_ == stage::finished)
      return xmodem_event::none;
    if (stage_ == stage::wait_start) // receivers may start late, so be patient
      return ++errors_ > cfg_.max_retries * 6 ? fail() : xmodem_event::none;
    return retry();
  }

  /** @brief Bytes to put on the wire after a `transmit` (or `failed`, which holds CAN) event. */
  [[nodiscard]] span<const std::uint8_t> packet() const noexcept { return span<const std::uint8_t>(pkt_.data(), pkt_len_); }

  /** @brief Whether CRC-16 mode was negotiated. */
  [[nodiscard]] constexpr bool crc_mode() const noexcept { return crc_; }

  /** @brief Payload bytes (including none of the padding) the receiver has acknowledged so far. */
  [[nodiscard]] constexpr std::size_t bytes_acked() const noexcept { return offset_; }

private:
  enum class stage : std::uint8_t { wait_start, wait_ack, wait_eot_ack, finished };

  xmodem_event build_next() noexcept {
    if (offset_ >= image_.size()) {
      pkt_[0] = xmodem_ctl::eot;
      pkt_len_ = 1;
      stage_ = stage::wait_eot_ack;
      return xmodem_event::transmit;
    }
    const std::size_t remaining = image_.size() - offset_;
    const bool big = crc_ && cfg_.use_1k && remaining > xmodem_small_block;
    const std::size_t size = big ? xmodem_max_block : xmodem_small_block;
    last_payload_ = remaining < size ? remaining : size;

    pkt_[0] = big ? xmodem_ctl::stx : xmodem_ctl::soh;
    pkt_[1] = blk_;
    pkt_[2] = static_cast<std::uint8_t>(~blk_);
    const span<std::uint8_t> data = span<std::uint8_t>(pkt_.data(), pkt_.size()).subspan(3);
    for (std::size_t i = 0; i < size; ++i)
      data[i] = i < last_payload_ ? image_[offset_ + i] : xmodem_ctl::sub;

    const span<const std::uint8_t> payload(data.data(), size);
    pkt_len_ = 3 + size;
    if (crc_) {
      const std::uint16_t crc = xmodem_crc16(payload);
      pkt_[pkt_len_++] = static_cast<std::uint8_t>(crc >> 8);
      pkt_[pkt_len_++] = static_cast<std::uint8_t>(crc);
    } else {
      pkt_[pkt_len_++] = xmodem_checksum(payload);
    }
    stage_ = stage::wait_ack;
    return xmodem_event::transmit;
  }

  xmodem_event retry() noexcept {
    if (++errors_ > cfg_.max_retries)
      return fail();
    return xmodem_event::transmit; // packet() still holds the last packet
  }

  xmodem_event cancelled() noexcept {
    stage_ = stage::finished;
    return xmodem_event::cancelled;
  }

  xmodem_event fail() noexcept {
    stage_ = stage::finished;
    pkt_[0] = xmodem_ctl::can;
    pkt_len_ = 1;
    return xmodem_event::failed;
  }

  xmodem_config cfg_;
  span<const std::uint8_t> image_;
  stage stage_ = stage::wait_start;
  bool crc_ = true;
  std::uint8_t blk_ = 1;
  std::size_t offset_ = 0;
  std::size_t last_payload_ = 0;
  std::size_t pkt_len_ = 0;
  std::uint32_t errors_ = 0;
  array<std::uint8_t, xmodem_max_packet> pkt_{};
};

// ============================================================================
// Blocking drivers over hw::uart_ref
// ============================================================================

/**
 * @brief Receives a whole file over `uart`, handing each in-order payload to `sink`.
 *
 * @param uart  Polled UART to talk over.
 * @param sink  Called with each new payload (128/1024 bytes, including any
 *              trailing `SUB` padding); return `false` to abort (e.g. image
 *              buffer full) -- CAN is then sent and `error::capacity_exceeded`
 *              returned.
 * @param cfg   Protocol tunables.
 * @param spins Per-byte `get_byte` spin budget; expiry counts as one protocol timeout.
 * @return Number of packets received; `error::operation_canceled` if the
 *         sender cancelled, `error::io_error` on retry exhaustion/sequence
 *         error, or a UART error.
 */
[[nodiscard]] inline result<std::uint32_t> receive(const hw::uart_ref &uart,
                                                   function_ref<bool(span<const std::uint8_t>)> sink,
                                                   const xmodem_config &cfg = {},
                                                   std::uint32_t spins = hw::uart_ref::default_max_spins) noexcept {
  xmodem_receiver rx(cfg);
  if (auto r = uart.write(rx.start()); !r)
    return unexpected(r.error());
  for (;;) {
    xmodem_event ev;
    auto byte = uart.get_byte(spins);
    if (byte)
      ev = rx.on_byte(byte.value());
    else if (byte.error() == error::timed_out)
      ev = rx.on_timeout();
    else
      return unexpected(byte.error());

    if (ev == xmodem_event::block && !sink(rx.block())) {
      (void)uart.put_byte(xmodem_ctl::can);
      return unexpected(error::capacity_exceeded);
    }
    if (auto r = uart.write(rx.reply()); !r)
      return unexpected(r.error());
    if (ev == xmodem_event::done)
      return rx.blocks_received();
    if (ev == xmodem_event::cancelled)
      return unexpected(error::operation_canceled);
    if (ev == xmodem_event::failed)
      return unexpected(error::io_error);
  }
}

/**
 * @brief Sends `image` over `uart` and waits for the receiver to finish.
 * @return `{}` on success; `error::operation_canceled` if the receiver
 *         cancelled, `error::io_error` on retry exhaustion, or a UART error.
 */
[[nodiscard]] inline result<void> send(const hw::uart_ref &uart, span<const std::uint8_t> image,
                                       const xmodem_config &cfg = {},
                                       std::uint32_t spins = hw::uart_ref::default_max_spins) noexcept {
  xmodem_sender tx(image, cfg);
  for (;;) {
    xmodem_event ev;
    auto byte = uart.get_byte(spins);
    if (byte)
      ev = tx.on_byte(byte.value());
    else if (byte.error() == error::timed_out)
      ev = tx.on_timeout();
    else
      return unexpected(byte.error());

    if (ev == xmodem_event::transmit || ev == xmodem_event::failed) {
      if (auto r = uart.write(tx.packet()); !r)
        return r;
    }
    if (ev == xmodem_event::done)
      return {};
    if (ev == xmodem_event::cancelled)
      return unexpected(error::operation_canceled);
    if (ev == xmodem_event::failed)
      return unexpected(error::io_error);
  }
}

} // namespace bootldr
} // namespace structo
