// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file tftp.hpp
 * @brief `structo::bootldr::tftp_client`: a coroutine TFTP client (RFC 1350, octet mode, lock-step,
 * 512-byte blocks) that runs on a `bootldr::netstack` through a `udp_socket`. C++20 only.
 *
 * It uses the stack's UDP socket (heap-allocated queue and packets, ephemeral local port), so any number
 * of other sockets and the ping responder keep working during a transfer. A transfer is a task: spawn it
 * on the scheduler, or `co_await` it from another task. One transfer at a time per client object;
 * use several clients for parallel transfers.
 *
 * Timing uses the scheduler clock (`scheduler::set_clock`, via the timed `udp_socket::receive_from`): the last packet is retransmitted after
 * `tftp_options::timeout_ms` of silence, `max_retries` times, then the transfer fails with
 * `error::timed_out`. Received data is handed over per block, so a bootloader can write it to flash
 * as it arrives, or use the `span` overloads to read into / send from a memory region.
 *
 * Errors: `timed_out`; `not_found` / `permission_denied` / `invalid_state` for a server ERROR of
 * code 1 / 2 / anything else (the raw code is in `server_error()`); `out_of_range` if the
 * destination memory is too small; `invalid_state` if the stack has no address yet.
 *
 * @code
 * structo::bootldr::netstack<1006> net{sched, nic, cfg};   // a started stack with an address (static, DHCP or PPP)
 * structo::bootldr::tftp_client tftp{net};                 // uses the stack's UDP; default options
 *
 * reloco::task<void> load_kernel(structo::bootldr::tftp_client &tftp, std::uint8_t *dst, std::size_t cap) {
 *   // Download "boot.bin" from the server 192.168.7.1 (port 69) straight into memory at `dst`.
 *   // Inner co_await: run the transfer and get a result; outer co_await: unwrap it (the task ends with the error otherwise).
 *   std::size_t size = co_await co_await tftp.get({192, 168, 7, 1}, "boot.bin", {dst, cap});
 *   // ... `size` bytes were stored; verify and jump ...
 * }
 *
 * // Streaming: `sink` is called with every block (<= 512 bytes) in order; return an error to abort the transfer.
 * reloco::result<void> sink(void *ctx, reloco::span<const std::uint8_t> block) noexcept;
 * //   total = co_await co_await tftp.get(server, "boot.bin", sink, &my_flash_writer);
 * @endcode
 */

#include "udp_socket.hpp"

#include "../net/tftp.hpp"

#include <reloco/array.hpp>
#include <reloco/string_view.hpp>

#if RELOCO_HAS_COROUTINES

namespace structo::bootldr {

/** @brief Tunables of a `tftp_client`. */
struct tftp_options {
  std::uint32_t timeout_ms = 1000;           ///< Silence before the last packet is retransmitted.
  unsigned max_retries = 5;                  ///< Retransmissions before the transfer fails with `timed_out`.
  std::uint16_t server_port = net::tftp_server_port; ///< Port of the server's request listener.
  std::size_t max_queue = 4;                 ///< Datagrams the socket may buffer (lock-step needs few).
};

class tftp_client {
public:
  /** Receives each downloaded block in order; an error aborts the transfer and is its result. */
  using sink_fn = reloco::result<void> (*)(void *ctx, reloco::span<const std::uint8_t> block) noexcept;
  /** Fills `block` (up to its size, 512) with the next upload data and returns the byte count; a count below
   * the block size ends the upload (0 is valid). */
  using source_fn = reloco::result<std::size_t> (*)(void *ctx, reloco::span<std::uint8_t> block) noexcept;

  explicit tftp_client(udp_demux &stack, const tftp_options &opts = {}) noexcept
      : stack_(&stack), opts_(opts), sock_(stack, opts.max_queue) {}

  /** @brief Code of the last ERROR packet the server sent (0 if none). */
  [[nodiscard]] std::uint16_t server_error() const noexcept { return server_error_; }
  /** @brief Bytes transferred so far by the current or last transfer. */
  [[nodiscard]] std::size_t transferred() const noexcept { return transferred_; }
  [[nodiscard]] bool busy() const noexcept { return busy_; }

  /** @brief Downloads `filename`, calling `sink` per block; completes with the total size. */
  [[nodiscard]] reloco::task<std::size_t> get(net::ipv4_address server, reloco::string_view filename, sink_fn sink,
                                              void *ctx) noexcept {
    if (busy_)
      co_await reloco::unexpected(reloco::error::busy);
    busy_guard guard{busy_};
    reset();
    auto b = sock_.bind(0);
    if (!b)
      co_await reloco::unexpected(b.error());
    struct closer {
      udp_socket &s;
      ~closer() { s.close(); }
    } close_on_exit{sock_};

    reloco::array<std::uint8_t, net::tftp_max_packet> tx{};
    reloco::array<std::uint8_t, net::tftp_max_packet + 8> rx{};
    auto req = net::build_tftp_request(net::tftp_rrq, filename, reloco::span<std::uint8_t>(tx));
    if (!req)
      co_await reloco::unexpected(req.error());
    std::size_t tx_len = *req;
    std::uint16_t block = 0; // last block accepted in order
    std::uint16_t peer_port = opts_.server_port;
    bool tid_known = false;
    unsigned retries = 0;

    co_await co_await sock_.send_to(server, peer_port, reloco::span<const std::uint8_t>(tx.data(), tx_len));
    std::uint64_t deadline = stack_->sched().now_ms() + opts_.timeout_ms;
    for (;;) {
      auto r = co_await wait_datagram(reloco::span<std::uint8_t>(rx), deadline);
      if (!r) {
        if (r.error() != reloco::error::timed_out || ++retries > opts_.max_retries)
          co_await reloco::unexpected(r.error());
        // Silence: repeat our last packet (the request, or the latest ACK).
        co_await co_await sock_.send_to(server, peer_port, reloco::span<const std::uint8_t>(tx.data(), tx_len));
        deadline = stack_->sched().now_ms() + opts_.timeout_ms;
        continue;
      }
      if (r->source != server || r->truncated || (tid_known && r->source_port != peer_port))
        continue;
      auto p = net::parse_tftp(reloco::span<const std::uint8_t>(rx.data(), r->size));
      if (!p)
        continue;
      if (p->opcode == net::tftp_error)
        co_await reloco::unexpected(remote_error(p->error_code));
      if (p->opcode != net::tftp_data)
        continue;
      if (!tid_known) {
        if (p->block != 1)
          continue;
        peer_port = r->source_port;
        tid_known = true;
      }
      if (p->block == static_cast<std::uint16_t>(block + 1)) {
        auto s = sink(ctx, p->data);
        if (!s)
          co_await reloco::unexpected(s.error());
        transferred_ += p->data.size();
        ++block;
        auto ack = net::build_tftp_ack(block, reloco::span<std::uint8_t>(tx));
        tx_len = *ack;
        retries = 0;
        co_await co_await sock_.send_to(server, peer_port, reloco::span<const std::uint8_t>(tx.data(), tx_len));
        if (p->data.size() < net::tftp_block_size)
          co_return transferred_;
        deadline = stack_->sched().now_ms() + opts_.timeout_ms;
      } else if (p->block == block && block != 0) {
        // The server did not see our ACK: send it again.
        co_await co_await sock_.send_to(server, peer_port, reloco::span<const std::uint8_t>(tx.data(), tx_len));
        deadline = stack_->sched().now_ms() + opts_.timeout_ms;
      }
    }
  }

  /** @brief Downloads `filename` into `dst` (`error::out_of_range` if it does not fit); completes with the size. */
  [[nodiscard]] reloco::task<std::size_t> get(net::ipv4_address server, reloco::string_view filename,
                                              reloco::span<std::uint8_t> dst) noexcept {
    mem_sink ms{dst, 0};
    co_return co_await co_await get(server, filename, &mem_sink::put, &ms);
  }

  /** @brief Uploads to `filename`, pulling blocks from `source`; completes with the total size. */
  [[nodiscard]] reloco::task<std::size_t> put(net::ipv4_address server, reloco::string_view filename, source_fn source,
                                              void *ctx) noexcept {
    if (busy_)
      co_await reloco::unexpected(reloco::error::busy);
    busy_guard guard{busy_};
    reset();
    auto b = sock_.bind(0);
    if (!b)
      co_await reloco::unexpected(b.error());
    struct closer {
      udp_socket &s;
      ~closer() { s.close(); }
    } close_on_exit{sock_};

    reloco::array<std::uint8_t, net::tftp_max_packet> tx{};
    reloco::array<std::uint8_t, net::tftp_max_packet + 8> rx{};
    reloco::array<std::uint8_t, net::tftp_block_size> blk{};
    auto req = net::build_tftp_request(net::tftp_wrq, filename, reloco::span<std::uint8_t>(tx));
    if (!req)
      co_await reloco::unexpected(req.error());
    std::size_t tx_len = *req;
    std::uint16_t block = 0; // block whose ACK we are waiting for (0 = the request)
    std::size_t block_len = 0;
    bool final_block = false;
    std::uint16_t peer_port = opts_.server_port;
    bool tid_known = false;
    unsigned retries = 0;

    co_await co_await sock_.send_to(server, peer_port, reloco::span<const std::uint8_t>(tx.data(), tx_len));
    std::uint64_t deadline = stack_->sched().now_ms() + opts_.timeout_ms;
    for (;;) {
      auto r = co_await wait_datagram(reloco::span<std::uint8_t>(rx), deadline);
      if (!r) {
        if (r.error() != reloco::error::timed_out || ++retries > opts_.max_retries)
          co_await reloco::unexpected(r.error());
        co_await co_await sock_.send_to(server, peer_port, reloco::span<const std::uint8_t>(tx.data(), tx_len));
        deadline = stack_->sched().now_ms() + opts_.timeout_ms;
        continue;
      }
      if (r->source != server || r->truncated || (tid_known && r->source_port != peer_port))
        continue;
      auto p = net::parse_tftp(reloco::span<const std::uint8_t>(rx.data(), r->size));
      if (!p)
        continue;
      if (p->opcode == net::tftp_error)
        co_await reloco::unexpected(remote_error(p->error_code));
      if (p->opcode != net::tftp_ack)
        continue;
      if (!tid_known) {
        if (p->block != 0)
          continue;
        peer_port = r->source_port;
        tid_known = true;
      }
      // Old or duplicate ACKs are ignored: answering them would duplicate DATA.
      if (p->block != block)
        continue;
      transferred_ += block_len;
      if (final_block)
        co_return transferred_;

      auto n = source(ctx, reloco::span<std::uint8_t>(blk));
      if (!n)
        co_await reloco::unexpected(n.error());
      block_len = *n < net::tftp_block_size ? *n : net::tftp_block_size;
      final_block = block_len < net::tftp_block_size;
      ++block;
      auto d = net::build_tftp_data(block, reloco::span<const std::uint8_t>(blk.data(), block_len),
                                    reloco::span<std::uint8_t>(tx));
      tx_len = *d;
      retries = 0;
      co_await co_await sock_.send_to(server, peer_port, reloco::span<const std::uint8_t>(tx.data(), tx_len));
      deadline = stack_->sched().now_ms() + opts_.timeout_ms;
    }
  }

  /** @brief Uploads `src` as `filename`; completes with its size. */
  [[nodiscard]] reloco::task<std::size_t> put(net::ipv4_address server, reloco::string_view filename,
                                              reloco::span<const std::uint8_t> src) noexcept {
    mem_source ms{src, 0};
    co_return co_await co_await put(server, filename, &mem_source::get, &ms);
  }

private:
  struct mem_sink {
    reloco::span<std::uint8_t> dst;
    std::size_t used;
    static reloco::result<void> put(void *c, reloco::span<const std::uint8_t> b) noexcept {
      auto &m = *static_cast<mem_sink *>(c);
      if (b.size() > m.dst.size() - m.used)
        return reloco::unexpected(reloco::error::out_of_range);
      for (std::size_t i = 0; i < b.size(); ++i)
        m.dst[m.used + i] = b[i];
      m.used += b.size();
      return {};
    }
  };

  struct mem_source {
    reloco::span<const std::uint8_t> src;
    std::size_t used;
    static reloco::result<std::size_t> get(void *c, reloco::span<std::uint8_t> b) noexcept {
      auto &m = *static_cast<mem_source *>(c);
      const std::size_t left = m.src.size() - m.used;
      const std::size_t n = left < b.size() ? left : b.size();
      for (std::size_t i = 0; i < n; ++i)
        b[i] = m.src[m.used + i];
      m.used += n;
      return n;
    }
  };

  struct busy_guard {
    bool &flag;
    explicit busy_guard(bool &f) noexcept : flag(f) { flag = true; }
    ~busy_guard() { flag = false; }
  };

  void reset() noexcept {
    server_error_ = 0;
    transferred_ = 0;
  }

  reloco::error remote_error(std::uint16_t code) noexcept {
    server_error_ = code;
    switch (code) {
    case 1:
      return reloco::error::not_found;
    case 2:
      return reloco::error::permission_denied;
    default:
      return reloco::error::invalid_state;
    }
  }

  // Awaits one datagram until `deadline_ms`; `error::timed_out` afterwards.
  reloco::task<udp_received> wait_datagram(reloco::span<std::uint8_t> buf, std::uint64_t deadline_ms) noexcept {
    const std::uint64_t now = stack_->sched().now_ms();
    co_return co_await co_await sock_.receive_from(buf, deadline_ms > now ? deadline_ms - now : 0);
  }

  udp_demux *stack_;
  tftp_options opts_;
  udp_socket sock_;
  std::uint16_t server_error_ = 0;
  std::size_t transferred_ = 0;
  bool busy_ = false;
};

} // namespace structo::bootldr

#endif // RELOCO_HAS_COROUTINES
