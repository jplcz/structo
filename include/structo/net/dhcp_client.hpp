// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file dhcp_client.hpp
 * @brief DHCPv4 client for `ipv4_node`: a sans-IO state machine
 * (`dhcp_client`) plus a small coroutine helper that transmits its messages.
 * C++20 only.
 *
 * Lifecycle: INIT -> SELECTING (DISCOVER sent) -> REQUESTING (OFFER received,
 * REQUEST sent) -> BOUND (ACK) -> RENEWING (at T1, default half the lease)
 * -> INIT again if the lease expires or the server NAKs. DISCOVER/REQUEST
 * are retransmitted with exponential backoff (4 s doubling up to 64 s).
 *
 * There is no clock or timer in the stack: the caller supplies a monotonic
 * millisecond timestamp. Retransmission and lease timers are evaluated
 * whenever `dhcp_send_due()` is called, so call it periodically (every main
 * loop iteration is fine; it sends nothing unless something is due).
 *
 * Static addressing needs none of this: just `node.configure(
 * ipv4_config::make_static(...))` (see `ipv4_config.hpp`).
 *
 * @code
 * structo::net::ipv4_node<1006> ip{nic};                  // unconfigured
 * structo::net::dhcp_client dhcp{mac, 0xC0FFEE};          // our MAC, seed for transaction ids
 *
 * reloco::task<void> app(structo::net::ipv4_node<1006> &ip, structo::net::dhcp_client &dhcp) {
 *   for (;;) {
 *     auto pkt = co_await co_await ip.receive();
 *     // DHCP replies are consumed here and, once bound, the node is
 *     // (re)configured with the leased address; everything else is ours.
 *     if (dhcp.handle(ip, pkt, now_ms()))
 *       continue;
 *     // ... application datagrams ...
 *   }
 * }
 *
 * // Main loop: keep the device moving and let DHCP retransmit/renew.
 * for (;;) {
 *   pnd.poll();
 *   auto t = structo::net::dhcp_send_due(ip, dhcp, now_ms()); // lazy task
 *   t.resume();                                                // sends only if due
 *   // keep `t` alive until done() if it parked on a busy device
 * }
 * @endcode
 */

#include "dhcp.hpp"
#include "ipv4_config.hpp"
#include "ipv4_node.hpp"
#include "udp.hpp"

#if RELOCO_HAS_COROUTINES

namespace structo::net {

enum class dhcp_state : std::uint8_t { init, selecting, requesting, bound, renewing };

class dhcp_client {
public:
  /**
   * @param mac Hardware address put in chaddr / client id (SLIP has none: use any locally administered one).
   * @param xid_seed Seed for transaction ids; use something device-unique (e.g. a hardware RNG sample).
   */
  dhcp_client(const hw::net_mac_address &mac, std::uint32_t xid_seed) noexcept : mac_(mac), xid_(xid_seed) {}

  [[nodiscard]] dhcp_state state() const noexcept { return state_; }

  /** @brief The leased configuration while BOUND/RENEWING, else nullptr. */
  [[nodiscard]] const ipv4_config *config() const noexcept { return has_config_ ? &config_ : nullptr; }

  /** @brief Restarts from INIT (e.g. after a link-up event). The current lease, if any, is dropped. */
  void restart() noexcept {
    if (has_config_) {
      has_config_ = false;
      ++generation_;
    }
    state_ = dhcp_state::init;
  }

  /**
   * @brief Next outgoing BOOTP payload (broadcast it to 255.255.255.255:67
   * from port 68) if something is due at `now_ms`, else `error::try_again`.
   * The view is valid until the next `poll()`.
   */
  [[nodiscard]] result<span<const std::uint8_t>> poll(std::uint64_t now_ms) noexcept {
    switch (state_) {
    case dhcp_state::init:
      xid_ = xid_ * 1664525u + 1013904223u;
      retry_ms_ = initial_retry_ms;
      deadline_ms_ = now_ms + retry_ms_;
      state_ = dhcp_state::selecting;
      return emit(dhcp_discover, {}, {}, {});
    case dhcp_state::selecting:
      if (now_ms < deadline_ms_)
        break;
      backoff(now_ms);
      return emit(dhcp_discover, {}, {}, {});
    case dhcp_state::requesting:
      if (offer_pending_) {
        offer_pending_ = false;
        tries_ = 1;
        deadline_ms_ = now_ms + initial_retry_ms;
        return emit(dhcp_request, {}, offer_.your_address, offer_.server_id);
      }
      if (now_ms < deadline_ms_)
        break;
      if (++tries_ > max_request_tries) {
        state_ = dhcp_state::init;
        return poll(now_ms);
      }
      deadline_ms_ = now_ms + initial_retry_ms;
      return emit(dhcp_request, {}, offer_.your_address, offer_.server_id);
    case dhcp_state::bound:
      if (infinite_ || now_ms < renew_at_ms_)
        break;
      state_ = dhcp_state::renewing;
      deadline_ms_ = now_ms + renew_retry_ms;
      return emit(dhcp_request, config_.address, {}, {});
    case dhcp_state::renewing:
      if (!infinite_ && now_ms >= lease_end_ms_) {
        has_config_ = false;
        ++generation_;
        state_ = dhcp_state::init;
        return poll(now_ms);
      }
      if (now_ms < deadline_ms_)
        break;
      deadline_ms_ = now_ms + renew_retry_ms;
      return emit(dhcp_request, config_.address, {}, {});
    }
    return unexpected(error::try_again);
  }

  /**
   * @brief Feeds a received IPv4 packet. Returns true if it was a DHCP
   * datagram (UDP port 68) and has been consumed, false if the caller
   * should handle it.
   */
  bool on_packet(const ipv4_packet &pkt, std::uint64_t now_ms) noexcept {
    if (pkt.header.protocol != ip_proto_udp)
      return false;
    auto udp = parse_udp(pkt.payload, pkt.header.src, pkt.header.dst);
    if (!udp || udp->dst_port != dhcp_client_port)
      return false;
    auto m = parse_dhcp(udp->payload);
    if (!m || m->xid != xid_ || m->mac != mac_)
      return true; // DHCP-looking but not for our transaction
    switch (state_) {
    case dhcp_state::selecting:
      if (m->type == dhcp_offer && !m->your_address.is_unspecified() && !m->server_id.is_unspecified()) {
        offer_ = *m;
        offer_pending_ = true;
        state_ = dhcp_state::requesting;
      }
      break;
    case dhcp_state::requesting:
    case dhcp_state::renewing:
      if (m->type == dhcp_ack)
        bind(*m, now_ms);
      else if (m->type == dhcp_nak)
        restart();
      break;
    default:
      break;
    }
    return true;
  }

  /** @brief Pushes the leased configuration (or its loss) to `node` if it changed since the last call. */
  template <std::size_t Mtu> void apply(ipv4_node<Mtu> &node) noexcept {
    if (applied_generation_ == generation_)
      return;
    applied_generation_ = generation_;
    node.configure(has_config_ ? config_ : ipv4_config{});
  }

  /** @brief `on_packet` followed by `apply`. */
  template <std::size_t Mtu>
  bool handle(ipv4_node<Mtu> &node, const ipv4_packet &pkt, std::uint64_t now_ms) noexcept {
    const bool consumed = on_packet(pkt, now_ms);
    apply(node);
    return consumed;
  }

private:
  static constexpr std::uint32_t initial_retry_ms = 4000;
  static constexpr std::uint32_t max_retry_ms = 64000;
  static constexpr std::uint32_t renew_retry_ms = 10000;
  static constexpr unsigned max_request_tries = 3;
  static constexpr std::uint32_t default_lease_s = 3600;

  void backoff(std::uint64_t now_ms) noexcept {
    retry_ms_ = retry_ms_ * 2 > max_retry_ms ? max_retry_ms : retry_ms_ * 2;
    deadline_ms_ = now_ms + retry_ms_;
  }

  result<span<const std::uint8_t>> emit(std::uint8_t type, ipv4_address ciaddr, ipv4_address requested,
                                        ipv4_address server) noexcept {
    dhcp_request_fields f;
    f.type = type;
    f.xid = xid_;
    f.mac = mac_;
    f.client_address = ciaddr;
    f.requested_address = requested;
    f.server_id = server;
    auto n = build_dhcp(f, span<std::uint8_t>(buf_));
    if (!n)
      return unexpected(n.error());
    return span<const std::uint8_t>(buf_.data(), n.value());
  }

  void bind(const dhcp_message &m, std::uint64_t now_ms) noexcept {
    config_.address = m.your_address;
    config_.netmask = m.netmask;
    config_.gateway = m.gateway;
    config_.dns = m.dns;
    const std::uint32_t lease = m.lease_seconds ? m.lease_seconds : default_lease_s;
    config_.lease_seconds = lease;
    infinite_ = lease == 0xFFFFFFFFu;
    const std::uint64_t t1 = m.renew_seconds && m.renew_seconds < lease ? m.renew_seconds : lease / 2;
    renew_at_ms_ = now_ms + t1 * 1000;
    lease_end_ms_ = now_ms + static_cast<std::uint64_t>(lease) * 1000;
    has_config_ = true;
    ++generation_;
    state_ = dhcp_state::bound;
  }

  hw::net_mac_address mac_;
  std::uint32_t xid_;
  dhcp_state state_ = dhcp_state::init;
  dhcp_message offer_{};
  bool offer_pending_ = false;
  ipv4_config config_{};
  bool has_config_ = false;
  bool infinite_ = false;
  std::uint32_t retry_ms_ = initial_retry_ms;
  unsigned tries_ = 0;
  std::uint64_t deadline_ms_ = 0;
  std::uint64_t renew_at_ms_ = 0;
  std::uint64_t lease_end_ms_ = 0;
  std::uint32_t generation_ = 0;
  std::uint32_t applied_generation_ = 0;
  std::array<std::uint8_t, dhcp_max_request_size> buf_{};
};

/**
 * @brief Applies any configuration change to `node`, then transmits the
 * client's next DHCP message if one is due at `now_ms` (broadcast, UDP
 * 68 -> 67). Completes immediately when nothing is due.
 */
template <std::size_t Mtu>
[[nodiscard]] reloco::task<void> dhcp_send_due(ipv4_node<Mtu> &node, dhcp_client &client,
                                               std::uint64_t now_ms) noexcept {
  client.apply(node);
  auto msg = client.poll(now_ms);
  if (!msg)
    co_return;
  const ipv4_address broadcast{255, 255, 255, 255};
  std::array<std::uint8_t, udp_header_size + dhcp_max_request_size> datagram{};
  std::size_t n = co_await build_udp(dhcp_client_port, dhcp_server_port, *msg, node.address(), broadcast,
                                     span<std::uint8_t>(datagram));
  auto sent = co_await node.send(ip_proto_udp, broadcast, span<const std::uint8_t>(datagram.data(), n));
  co_await std::move(sent);
}

} // namespace structo::net

#endif // RELOCO_HAS_COROUTINES
