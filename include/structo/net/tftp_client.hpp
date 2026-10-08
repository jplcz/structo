// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file tftp_client.hpp
 * @brief TFTP client for `ipv4_node`: a sans-IO state machine
 * (`tftp_client`) plus a coroutine helper that transmits its packets.
 * One transfer at a time, octet mode, lock-step (RFC 1350). C++20 only.
 *
 * Like the DHCP client it has no timer: the caller supplies a monotonic
 * millisecond clock. The last packet is retransmitted after 1 s of silence,
 * up to 5 times, then the transfer fails with `timed_out()`.
 *
 * Reading: `start_read()`, feed packets to `handle()`; on `tftp_event::data`
 * consume `data()` before the next `handle()`; `finished()` is true once the
 * last (short) block arrived.
 * Writing: `start_write()`; on `tftp_event::need_data` call `supply()` with
 * the next block (up to 512 bytes; a shorter block, possibly empty, ends the
 * transfer); `tftp_event::done` means the server acknowledged everything.
 *
 * @code
 * structo::net::ipv4_node<1006> ip{nic, {192, 168, 7, 2}};
 * structo::net::tftp_client tftp{49200};              // local UDP port for this client
 * tftp.start_read({192, 168, 7, 1}, "boot.bin", now_ms()); // server address, remote file, current time
 *
 * reloco::task<void> rx(structo::net::ipv4_node<1006> &ip, structo::net::tftp_client &tftp) {
 *   while (!tftp.finished() && !tftp.failed()) {
 *     auto pkt = co_await co_await ip.receive();
 *     // Returns ignored for packets that aren't part of the transfer.
 *     if (tftp.handle(pkt, now_ms()) == structo::net::tftp_event::data)
 *       flash_write(tftp.data()); // valid until the next handle()
 *   }
 * }
 *
 * // Main loop: sends the request / ACKs and retransmits on timeout; sends nothing if idle.
 * auto t = structo::net::tftp_send_due(ip, tftp, now_ms());
 * t.resume();
 * @endcode
 */

#include "ipv4_node.hpp"
#include "tftp.hpp"
#include "udp.hpp"

#if RELOCO_HAS_COROUTINES

namespace structo::net {

enum class tftp_state : std::uint8_t { idle, reading, writing, done, failed };

/** @brief Result of feeding a packet to `tftp_client::handle()`. */
enum class tftp_event : std::uint8_t {
  ignored,   ///< not part of this transfer
  none,      ///< consumed, nothing for the application to do
  data,      ///< a new block is available in `data()`
  need_data, ///< call `supply()` with the next block
  done,      ///< write acknowledged to the end
  failed     ///< server sent ERROR (see `error_code()`)
};

class tftp_client {
public:
  static constexpr std::uint64_t retry_timeout_ms = 1000;
  static constexpr unsigned max_retries = 5;

  /** @param local_port UDP source port for this client (the "TID"); pick one not used by other sockets. */
  explicit tftp_client(std::uint16_t local_port = 49200) noexcept : local_port_(local_port) {}

  [[nodiscard]] tftp_state state() const noexcept { return state_; }
  [[nodiscard]] bool finished() const noexcept { return state_ == tftp_state::done; }
  [[nodiscard]] bool failed() const noexcept { return state_ == tftp_state::failed; }
  [[nodiscard]] bool timed_out() const noexcept { return timed_out_; }
  /** @brief Code from the server's ERROR packet (0 if none or on timeout). */
  [[nodiscard]] std::uint16_t error_code() const noexcept { return error_code_; }
  [[nodiscard]] std::uint16_t local_port() const noexcept { return local_port_; }
  [[nodiscard]] const ipv4_address &server() const noexcept { return server_; }
  /** @brief Remote port for outgoing packets: 69 until the server answers, then its transfer port. */
  [[nodiscard]] std::uint16_t remote_port() const noexcept { return tid_known_ ? tid_ : tftp_server_port; }
  /** @brief The block received by the last `tftp_event::data`. */
  [[nodiscard]] span<const std::uint8_t> data() const noexcept { return span<const std::uint8_t>(rx_.data(), rx_len_); }

  /** @brief Begins downloading `filename` from `server`. */
  [[nodiscard]] result<void> start_read(const ipv4_address &server, std::string_view filename,
                                        std::uint64_t now_ms) noexcept {
    return start(tftp_rrq, tftp_state::reading, server, filename, now_ms);
  }

  /** @brief Begins uploading `filename` to `server`; blocks are provided via `supply()`. */
  [[nodiscard]] result<void> start_write(const ipv4_address &server, std::string_view filename,
                                         std::uint64_t now_ms) noexcept {
    return start(tftp_wrq, tftp_state::writing, server, filename, now_ms);
  }

  /** @brief Abandons the current transfer (the server will time out on its own). */
  void abort() noexcept {
    state_ = tftp_state::idle;
    pending_ = false;
  }

  /** @brief Provides the next block after `tftp_event::need_data`; fewer than 512 bytes ends the upload. */
  [[nodiscard]] result<void> supply(span<const std::uint8_t> block) noexcept {
    if (state_ != tftp_state::writing || !awaiting_supply_)
      return unexpected(error::invalid_argument);
    auto n = build_tftp_data(static_cast<std::uint16_t>(block_ + 1), block, span<std::uint8_t>(tx_));
    if (!n)
      return unexpected(n.error());
    tx_len_ = *n;
    ++block_;
    final_ = block.size() < tftp_block_size;
    awaiting_supply_ = false;
    pending_ = true;
    retries_ = 0;
    return {};
  }

  /**
   * @brief Next outgoing UDP payload (send to `server():remote_port()` from `local_port()`) if one is
   * due at `now_ms`, else `error::try_again`. The view is valid until the next `poll()`/`handle()`/`supply()`.
   */
  [[nodiscard]] result<span<const std::uint8_t>> poll(std::uint64_t now_ms) noexcept {
    if (pending_) {
      pending_ = false;
      deadline_ms_ = now_ms + retry_timeout_ms;
      return span<const std::uint8_t>(tx_.data(), tx_len_);
    }
    const bool active = state_ == tftp_state::reading || state_ == tftp_state::writing;
    if (!active || awaiting_supply_ || now_ms < deadline_ms_)
      return unexpected(error::try_again);
    if (retries_ >= max_retries) {
      state_ = tftp_state::failed;
      timed_out_ = true;
      return unexpected(error::try_again);
    }
    ++retries_;
    deadline_ms_ = now_ms + retry_timeout_ms;
    return span<const std::uint8_t>(tx_.data(), tx_len_);
  }

  /** @brief Offers a received IPv4 packet to the client. */
  tftp_event handle(const ipv4_packet &pkt, std::uint64_t now_ms) noexcept {
    (void)now_ms;
    if (pkt.header.protocol != ip_proto_udp || (state_ != tftp_state::reading && state_ != tftp_state::writing) ||
        pkt.header.src != server_)
      return tftp_event::ignored;
    auto u = parse_udp(pkt.payload, pkt.header.src, pkt.header.dst);
    if (!u || u->dst_port != local_port_ || (tid_known_ && u->src_port != tid_))
      return tftp_event::ignored;
    auto p = parse_tftp(u->payload);
    if (!p)
      return tftp_event::ignored;

    if (p->opcode == tftp_error) {
      state_ = tftp_state::failed;
      error_code_ = p->error_code;
      pending_ = false;
      return tftp_event::failed;
    }

    if (state_ == tftp_state::reading)
      return on_read(*p, u->src_port);
    return on_write(*p, u->src_port);
  }

private:
  [[nodiscard]] result<void> start(std::uint16_t op, tftp_state st, const ipv4_address &server,
                                   std::string_view filename, std::uint64_t now_ms) noexcept {
    auto n = build_tftp_request(op, filename, span<std::uint8_t>(tx_));
    if (!n)
      return unexpected(n.error());
    (void)now_ms;
    tx_len_ = *n;
    server_ = server;
    state_ = st;
    tid_known_ = false;
    tid_ = 0;
    block_ = 0;
    retries_ = 0;
    rx_len_ = 0;
    error_code_ = 0;
    timed_out_ = false;
    awaiting_supply_ = false;
    final_ = false;
    pending_ = true;
    return {};
  }

  tftp_event on_read(const tftp_packet &p, std::uint16_t src_port) noexcept {
    if (p.opcode != tftp_data)
      return tftp_event::ignored;
    if (!tid_known_) {
      if (p.block != 1)
        return tftp_event::ignored;
      tid_ = src_port;
      tid_known_ = true;
    }
    if (p.block == static_cast<std::uint16_t>(block_ + 1)) {
      for (std::size_t i = 0; i < p.data.size(); ++i)
        rx_[i] = p.data[i];
      rx_len_ = p.data.size();
      ++block_;
      auto ack = build_tftp_ack(block_, span<std::uint8_t>(tx_));
      tx_len_ = *ack;
      pending_ = true;
      retries_ = 0;
      if (p.data.size() < tftp_block_size)
        state_ = tftp_state::done;
      return tftp_event::data;
    }
    if (p.block == block_ && block_ != 0) {
      pending_ = true; // our ACK was lost: repeat it
      return tftp_event::none;
    }
    return tftp_event::ignored;
  }

  tftp_event on_write(const tftp_packet &p, std::uint16_t src_port) noexcept {
    if (p.opcode != tftp_ack)
      return tftp_event::ignored;
    if (!tid_known_) {
      if (p.block != 0)
        return tftp_event::ignored;
      tid_ = src_port;
      tid_known_ = true;
    }
    // Duplicate ACKs are dropped (answering them would duplicate DATA).
    if (p.block != block_ || awaiting_supply_)
      return tftp_event::none;
    if (final_) {
      state_ = tftp_state::done;
      return tftp_event::done;
    }
    awaiting_supply_ = true;
    return tftp_event::need_data;
  }

  std::uint16_t local_port_;
  ipv4_address server_{};
  std::uint16_t tid_ = 0;
  bool tid_known_ = false;
  tftp_state state_ = tftp_state::idle;
  std::uint16_t block_ = 0; // last block received (read) or sent (write)
  std::uint16_t error_code_ = 0;
  std::array<std::uint8_t, tftp_max_packet> tx_{};
  std::size_t tx_len_ = 0;
  std::array<std::uint8_t, tftp_block_size> rx_{};
  std::size_t rx_len_ = 0;
  std::uint64_t deadline_ms_ = 0;
  unsigned retries_ = 0;
  bool pending_ = false;
  bool awaiting_supply_ = false;
  bool final_ = false;
  bool timed_out_ = false;
};

/**
 * @brief Sends the client's next packet through `node` as UDP if one is due. Completes immediately
 * when nothing is due.
 */
template <std::size_t Mtu>
[[nodiscard]] reloco::task<void> tftp_send_due(ipv4_node<Mtu> &node, tftp_client &client,
                                               std::uint64_t now_ms) noexcept {
  auto msg = client.poll(now_ms);
  if (!msg)
    co_return;
  std::array<std::uint8_t, udp_header_size + tftp_max_packet> datagram{};
  std::size_t n = co_await build_udp(client.local_port(), client.remote_port(), *msg, node.address(),
                                     client.server(), span<std::uint8_t>(datagram));
  auto sent = co_await node.send(ip_proto_udp, client.server(), span<const std::uint8_t>(datagram.data(), n));
  co_await std::move(sent);
}

} // namespace structo::net

#endif // RELOCO_HAS_COROUTINES
