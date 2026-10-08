// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file ipv4_node.hpp
 * @brief `structo::net::ipv4_node<Mtu>`: a minimal coroutine IPv4 endpoint
 * over a `hw::net_device_ref` (SLIP today; PPP later, since the device
 * carries bare IP datagrams). C++20 only.
 *
 * `receive()` awaits the next datagram addressed to this node. Echo requests
 * (ping) are answered internally; malformed packets, fragments, packets for
 * other hosts and other ICMP types are dropped silently. `send()` builds and
 * transmits a datagram. Both are `reloco::task`s, so they suspend while the
 * device has nothing to deliver / no room to send and are resumed by whoever
 * drives the device (`polled_net_device::poll()` or an IRQ handler).
 *
 * The node owns one RX and one TX buffer of `Mtu` bytes (so the largest
 * payload is `Mtu - 20`). A received packet's `payload` views the RX buffer
 * and is valid only until the next `receive()` starts. Only one `receive()`
 * and one `send()` (or echo reply) may be in flight at a time; a second one
 * fails with `error::busy`.
 *
 * ## Addressing
 *
 * The node starts either with a static address (constructor) or
 * unconfigured. While unconfigured (0.0.0.0) it accepts datagrams for any
 * destination and sends with source 0.0.0.0, which is what DHCP needs.
 * `configure()` applies a static or DHCP-learned `ipv4_config` at any time
 * (see `dhcp_client.hpp`); `configure({})` returns to the unconfigured state.
 *
 * @code
 * structo::hw::slip_device<1006> slip{uart};
 * structo::hw::polled_net_device<decltype(slip)> pnd{slip};
 * structo::hw::net_device_ref nic{pnd};
 * structo::net::ipv4_node<1006> ip{nic, {192, 168, 7, 2}}; // our address
 *
 * reloco::task<void> app(structo::net::ipv4_node<1006> &ip) {
 *   for (;;) {
 *     // Inner co_await suspends until a datagram for us arrives (pings are
 *     // answered meanwhile); outer one unwraps the result or ends the task on error.
 *     auto pkt = co_await co_await ip.receive();
 *     if (pkt.header.protocol == structo::net::ip_proto_udp) {
 *       // Send the same payload back to the sender.
 *       co_await co_await ip.send(structo::net::ip_proto_udp, pkt.header.src, pkt.payload);
 *     }
 *   }
 * }
 * @endcode
 */

#include "icmp.hpp"
#include "ipv4_config.hpp"

#if RELOCO_HAS_COROUTINES

#include "../hw/net_device_ref.hpp"

#include <utility>

namespace structo::net {

template <std::size_t Mtu = 1006> class ipv4_node {
public:
  /** @brief Node with a static address (no netmask/gateway). */
  ipv4_node(hw::net_device_ref dev, ipv4_address local) noexcept : dev_(dev), cfg_(ipv4_config::make_static(local)) {}
  /** @brief Node with a full static configuration, or unconfigured if `cfg` has no address. */
  explicit ipv4_node(hw::net_device_ref dev, const ipv4_config &cfg = {}) noexcept : dev_(dev), cfg_(cfg) {}
  ipv4_node(const ipv4_node &) = delete;
  ipv4_node &operator=(const ipv4_node &) = delete;

  [[nodiscard]] ipv4_address address() const noexcept { return cfg_.address; }
  [[nodiscard]] const ipv4_config &config() const noexcept { return cfg_; }
  [[nodiscard]] bool configured() const noexcept { return cfg_.configured(); }

  /** @brief Applies a static or DHCP-learned configuration (`{}` to unconfigure). */
  void configure(const ipv4_config &cfg) noexcept { cfg_ = cfg; }

  /** @brief Awaits the next valid datagram for this node (see file docs for what is consumed). */
  [[nodiscard]] reloco::task<ipv4_packet> receive() noexcept {
    if (rx_busy_)
      co_await unexpected(error::busy);
    flag_guard guard{rx_busy_};
    for (;;) {
      auto frame = co_await dev_.receive(span<std::uint8_t>(rx_));
      std::size_t n = co_await std::move(frame);
      auto pkt = parse_ipv4(span<const std::uint8_t>(rx_.data(), n));
      if (!pkt)
        continue;
      const auto &h = pkt->header;
      if (configured() && h.dst != cfg_.address && !h.dst.is_broadcast())
        continue;
      if (h.protocol == ip_proto_icmp) {
        auto echo = parse_icmp_echo(pkt->payload);
        if (configured() && echo && echo->type == icmp_echo_request) {
          auto sent = co_await reply_echo(h.src, *echo);
          (void)sent; // a failed reply (e.g. tx busy) is just a lost ping
        }
        continue;
      }
      co_return *pkt;
    }
  }

  /** @brief Builds and sends one datagram to `dst`; completes once the device accepted it. */
  [[nodiscard]] reloco::task<void> send(std::uint8_t protocol, ipv4_address dst,
                                        span<const std::uint8_t> payload) noexcept {
    if (tx_busy_)
      co_await unexpected(error::busy);
    flag_guard guard{tx_busy_};
    ipv4_header h;
    h.protocol = protocol;
    h.src = cfg_.address;
    h.dst = dst;
    h.identification = next_id_++;
    std::size_t n = co_await build_ipv4(h, payload, span<std::uint8_t>(tx_));
    auto sent = co_await dev_.send(span<const std::uint8_t>(tx_.data(), n));
    co_await std::move(sent);
  }

private:
  // Clears the in-flight flag on every exit path, including destruction of a parked coroutine.
  struct flag_guard {
    bool &f;
    explicit flag_guard(bool &flag) noexcept : f(flag) { f = true; }
    ~flag_guard() { f = false; }
  };

  // The echo data views rx_ while the reply is built into tx_, so they never overlap.
  reloco::task<void> reply_echo(ipv4_address to, icmp_echo req) noexcept {
    if (tx_busy_)
      co_await unexpected(error::busy);
    flag_guard guard{tx_busy_};
    std::array<std::uint8_t, Mtu> icmp{};
    std::size_t m = co_await build_icmp_echo(icmp_echo_reply, req.id, req.seq, req.data, span<std::uint8_t>(icmp));
    ipv4_header h;
    h.protocol = ip_proto_icmp;
    h.src = cfg_.address;
    h.dst = to;
    h.identification = next_id_++;
    std::size_t n = co_await build_ipv4(h, span<const std::uint8_t>(icmp.data(), m), span<std::uint8_t>(tx_));
    auto sent = co_await dev_.send(span<const std::uint8_t>(tx_.data(), n));
    co_await std::move(sent);
  }

  hw::net_device_ref dev_;
  ipv4_config cfg_;
  std::array<std::uint8_t, Mtu> rx_{};
  std::array<std::uint8_t, Mtu> tx_{};
  std::uint16_t next_id_ = 1;
  bool rx_busy_ = false;
  bool tx_busy_ = false;
};

} // namespace structo::net

#endif // RELOCO_HAS_COROUTINES
