// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file udp_socket.hpp
 * @brief `structo::bootldr::udp_socket`: a UDP socket that plugs into a
 * `bootldr::netstack`. C++20 only.
 *
 * The socket object is owned by the client (it can live on the stack, in a
 * struct ... anywhere), but all of its *data* lives on the heap: every
 * received datagram is copied into a `reloco::vector` allocated from the
 * socket's allocator (by default the scheduler's), and `send_to()` builds its
 * packet in a heap buffer too. Nothing is statically sized. Allocation
 * failures are reported (`error::allocation_failed`) or, on receive, the
 * datagram is dropped and counted in `dropped()`.
 *
 * Datagrams for a bound port are queued, at most `max_queue` of them; further
 * ones are dropped (oldest are kept, like a full kernel socket buffer).
 * `receive_from()` suspends the calling task (via the scheduler) until a
 * datagram is available.
 *
 * Lifetime: close or destroy a socket only when no task is still waiting in
 * `receive_from()` (cancel or finish such tasks first), and keep the
 * netstack alive while sockets are bound; destroying the netstack first just
 * detaches the sockets (their operations then fail with `error::invalid_state`).
 *
 * @code
 * structo::bootldr::udp_socket sock{net};   // `net` is a started bootldr::netstack<Mtu>; heap comes from its scheduler
 * sock.bind(5000);                          // receive datagrams sent to our UDP port 5000 (0 = pick a free port)
 *
 * reloco::task<void> echo(structo::bootldr::udp_socket &s) {
 *   reloco::array<std::uint8_t, 512> buf;   // receive buffer; longer datagrams are truncated (flag in the result)
 *   for (;;) {
 *     // Inner co_await suspends until a datagram arrives; outer unwraps the result or ends the task on error.
 *     auto rx = co_await co_await s.receive_from(buf);
 *     // Reply to whoever sent it: destination address, destination port, payload view.
 *     co_await co_await s.send_to(rx.source, rx.source_port, {buf.data(), rx.size});
 *   }
 * }
 * @endcode
 */

#include "scheduler.hpp"

#include "../net/ipv4.hpp"
#include "../net/udp.hpp"

#include <reloco/intrusive_c_tailq.hpp>
#include <reloco/vec_deque.hpp>

#if RELOCO_HAS_COROUTINES

namespace structo::bootldr {

class udp_demux;

/** @brief Result of `udp_socket::receive_from()`. */
struct udp_received {
  std::size_t size = 0;           ///< Bytes copied into the caller's buffer.
  net::ipv4_address source{};     ///< Sender's address.
  std::uint16_t source_port = 0;  ///< Sender's UDP port.
  bool truncated = false;         ///< The datagram was longer than the buffer; the rest was discarded.
};

class udp_socket {
public:
  /** @brief Creates an unbound socket on `stack` (a `netstack`); its heap memory comes from the stack's allocator. */
  explicit udp_socket(udp_demux &stack, std::size_t max_queue = 8) noexcept;
  /** @brief As above, with an explicit allocator for the receive queue and send buffers. */
  udp_socket(udp_demux &stack, reloco::allocator_ref alloc, std::size_t max_queue = 8) noexcept;
  udp_socket(const udp_socket &) = delete;
  udp_socket &operator=(const udp_socket &) = delete;
  ~udp_socket() { close(); }

  /**
   * @brief Starts receiving datagrams addressed to local `port` (0 = pick a free ephemeral port).
   * `error::invalid_state` if already bound, `error::busy` if the port is taken. Returns the bound port.
   */
  [[nodiscard]] inline reloco::result<std::uint16_t> bind(std::uint16_t port = 0) noexcept;

  /** @brief Unbinds, discards queued datagrams and wakes waiting receivers (they fail with `error::operation_canceled`). */
  inline void close() noexcept;

  [[nodiscard]] bool is_bound() const noexcept { return bound_; }
  [[nodiscard]] std::uint16_t local_port() const noexcept { return port_; }
  /** @brief Datagrams currently queued. */
  [[nodiscard]] std::size_t pending() const noexcept { return queue_.size(); }
  /** @brief Datagrams discarded because the queue was full or memory was short. */
  [[nodiscard]] std::uint32_t dropped() const noexcept { return dropped_; }

  /** @brief Takes one queued datagram without waiting; `error::try_again` if there is none. */
  [[nodiscard]] inline reloco::result<udp_received> try_receive_from(reloco::span<std::uint8_t> buf) noexcept;

  /** @brief Awaits one datagram (see `try_receive_from`); `error::invalid_state` if the socket is not bound. */
  [[nodiscard]] inline reloco::task<udp_received> receive_from(reloco::span<std::uint8_t> buf) noexcept;

  /**
   * @brief Sends one datagram. Binds an ephemeral port first if needed. Fails with `error::invalid_state` when
   * the stack has no address yet and `error::out_of_range` if it exceeds the IP MTU; waits while the link is busy.
   */
  [[nodiscard]] inline reloco::task<void> send_to(net::ipv4_address dst, std::uint16_t dst_port,
                                                  reloco::span<const std::uint8_t> data) noexcept;

private:
  friend class udp_demux;

  struct entry {
    net::ipv4_address source{};
    std::uint16_t port = 0;
    reloco::vector<std::uint8_t> data;
  };

  // Queues a datagram for this socket; always consumes it (a full queue counts as a drop).
  inline void enqueue(net::ipv4_address src, const net::udp_datagram &d) noexcept;

  // Same layout as FreeBSD TAILQ_ENTRY, as expected by reloco::c_tailq.
  struct {
    udp_socket *next = nullptr;
    udp_socket **prev = nullptr;
  } link_;

  udp_demux *stack_;
  reloco::allocator_ref alloc_;
  std::size_t max_queue_;
  reloco::vec_deque<entry> queue_;
  scheduler::event ready_;
  std::uint16_t port_ = 0;
  bool bound_ = false;
  std::uint32_t dropped_ = 0;
};

/**
 * @brief The non-template part of a network stack that sockets plug into; `netstack<Mtu>` derives from it.
 * Holds the bound sockets, demultiplexes received UDP datagrams to them and sends their datagrams as IPv4.
 */
class udp_demux {
public:
  using send_fn = reloco::task<void> (*)(void *ctx, net::ipv4_address dst,
                                         reloco::span<const std::uint8_t> udp) noexcept;
  using local_fn = net::ipv4_address (*)(void *ctx) noexcept;

  /** @param send Transmits one UDP datagram (header included) as IPv4 to `dst`. @param local Our current address. */
  udp_demux(scheduler &sched, void *ctx, send_fn send, local_fn local) noexcept
      : sched_(&sched), ctx_(ctx), send_(send), local_(local) {}
  udp_demux(const udp_demux &) = delete;
  udp_demux &operator=(const udp_demux &) = delete;
  ~udp_demux() {
    while (udp_socket *s = sockets_.pop_front())
      s->stack_ = nullptr;
  }

  [[nodiscard]] scheduler &sched() const noexcept { return *sched_; }
  [[nodiscard]] reloco::allocator_ref allocator() const noexcept { return sched_->allocator(); }

  /** @brief Delivers `pkt` to the socket bound to its destination port. False if it is not UDP or nobody listens. */
  bool deliver_udp(const net::ipv4_packet &pkt) noexcept {
    if (pkt.header.protocol != net::ip_proto_udp)
      return false;
    auto d = net::parse_udp(pkt.payload, pkt.header.src, pkt.header.dst);
    if (!d)
      return false;
    for (udp_socket &s : sockets_)
      if (s.port_ == d->dst_port) {
        s.enqueue(pkt.header.src, *d);
        return true;
      }
    return false;
  }

private:
  friend class udp_socket;

  [[nodiscard]] bool port_in_use(std::uint16_t port) noexcept {
    for (udp_socket &s : sockets_)
      if (s.port_ == port)
        return true;
    return false;
  }

  [[nodiscard]] std::uint16_t pick_port() noexcept {
    for (std::uint32_t i = 0; i < 16384; ++i) {
      const std::uint16_t p = next_port_;
      next_port_ = next_port_ == 65535 ? std::uint16_t{49152} : static_cast<std::uint16_t>(next_port_ + 1);
      if (!port_in_use(p))
        return p;
    }
    return 0;
  }

  scheduler *sched_;
  void *ctx_;
  send_fn send_;
  local_fn local_;
  std::uint16_t next_port_ = 49152;
  reloco::c_tailq<udp_socket, &udp_socket::link_> sockets_;
};

inline udp_socket::udp_socket(udp_demux &stack, std::size_t max_queue) noexcept
    : udp_socket(stack, stack.allocator(), max_queue) {}

inline udp_socket::udp_socket(udp_demux &stack, reloco::allocator_ref alloc, std::size_t max_queue) noexcept
    : stack_(&stack), alloc_(alloc), max_queue_(max_queue), queue_(alloc), ready_(stack.sched()) {}

inline reloco::result<std::uint16_t> udp_socket::bind(std::uint16_t port) noexcept {
  if (!stack_ || bound_)
    return reloco::unexpected(reloco::error::invalid_state);
  if (port == 0)
    port = stack_->pick_port();
  if (port == 0 || stack_->port_in_use(port))
    return reloco::unexpected(reloco::error::busy);
  port_ = port;
  bound_ = true;
  stack_->sockets_.push_back(*this);
  return port;
}

inline void udp_socket::close() noexcept {
  if (!bound_)
    return;
  bound_ = false;
  if (stack_)
    stack_->sockets_.remove(*this);
  while (!queue_.empty())
    (void)queue_.try_pop_front();
  ready_.set(); // receivers wake, see bound_ == false and fail
}

inline void udp_socket::enqueue(net::ipv4_address src, const net::udp_datagram &d) noexcept {
  if (queue_.size() >= max_queue_) {
    ++dropped_;
    return;
  }
  entry e;
  e.source = src;
  e.port = d.src_port;
  e.data = reloco::vector<std::uint8_t>(alloc_);
  if (!d.payload.empty()) {
    if (!e.data.try_resize(d.payload.size())) {
      ++dropped_;
      return;
    }
    for (std::size_t i = 0; i < d.payload.size(); ++i)
      e.data[i] = d.payload[i];
  }
  if (!queue_.try_push_back(std::move(e))) {
    ++dropped_;
    return;
  }
  ready_.set();
}

inline reloco::result<udp_received> udp_socket::try_receive_from(reloco::span<std::uint8_t> buf) noexcept {
  if (!bound_)
    return reloco::unexpected(reloco::error::invalid_state);
  if (queue_.empty())
    return reloco::unexpected(reloco::error::try_again);
  entry &e = queue_[0];
  udp_received out;
  out.source = e.source;
  out.source_port = e.port;
  out.size = e.data.size() < buf.size() ? e.data.size() : buf.size();
  out.truncated = e.data.size() > buf.size();
  for (std::size_t i = 0; i < out.size; ++i)
    buf[i] = e.data[i];
  (void)queue_.try_pop_front();
  return out;
}

inline reloco::task<udp_received> udp_socket::receive_from(reloco::span<std::uint8_t> buf) noexcept {
  for (;;) {
    if (!bound_)
      co_await reloco::unexpected(reloco::error::operation_canceled);
    auto r = try_receive_from(buf);
    if (r)
      co_return *r;
    if (r.error() != reloco::error::try_again)
      co_await reloco::unexpected(r.error());
    ready_.reset();
    co_await ready_.wait(); // woken by enqueue() or close(); loop re-checks
  }
}

inline reloco::task<void> udp_socket::send_to(net::ipv4_address dst, std::uint16_t dst_port,
                                              reloco::span<const std::uint8_t> data) noexcept {
  if (!stack_)
    co_await reloco::unexpected(reloco::error::invalid_state);
  if (!bound_) {
    auto b = bind(0);
    if (!b)
      co_await reloco::unexpected(b.error());
  }
  const net::ipv4_address src = stack_->local_(stack_->ctx_);
  if (src.is_unspecified())
    co_await reloco::unexpected(reloco::error::invalid_state);

  reloco::vector<std::uint8_t> pkt(alloc_);
  if (!pkt.try_resize(net::udp_header_size + data.size()))
    co_await reloco::unexpected(reloco::error::allocation_failed);
  auto n = net::build_udp(port_, dst_port, data, src, dst, reloco::span<std::uint8_t>(pkt.data(), pkt.size()));
  if (!n)
    co_await reloco::unexpected(n.error());

  for (;;) {
    if (!stack_)
      co_await reloco::unexpected(reloco::error::invalid_state);
    auto sent = co_await stack_->send_(stack_->ctx_, dst, reloco::span<const std::uint8_t>(pkt.data(), n.value()));
    if (sent)
      co_return;
    if (sent.error() != reloco::error::busy)
      co_await reloco::unexpected(sent.error());
    (void)co_await stack_->sched().yield(); // another sender is in flight: retry next round
  }
}

} // namespace structo::bootldr

#endif // RELOCO_HAS_COROUTINES
