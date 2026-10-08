// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file arp_table.hpp
 * @brief `structo::net::arp_table<N>`: sans-IO ARP cache + resolver for an
 * Ethernet interface. C++20 only.
 *
 * It answers requests for the local address, learns mappings from replies
 * (and from requests aimed at us), and resolves next-hop addresses on demand:
 * `lookup()` returns the MAC if cached, otherwise queues an ARP request and
 * reports `error::try_again`. Requests are repeated every 1 s up to 3 times,
 * then the entry is dropped (the next `lookup()` starts over). Learned entries
 * expire after 60 s. Static entries never expire.
 *
 * No clock is built in: the caller supplies a monotonic millisecond time.
 * Frame I/O is also the caller's job: wrap `poll()` output into an Ethernet
 * frame (ethertype `arp_ethertype`, destination `tx.dst_mac`) and feed received
 * ARP payloads to `handle()`.
 *
 * @code
 * structo::net::arp_table<8> arp{mac, {10, 0, 0, 2}};  // our MAC and IPv4 address; 8 cache slots
 *
 * // Receive path: payload of a frame with ethertype 0x0806.
 * arp.handle(payload, now_ms());
 *
 * // Transmit path: need the MAC for the next hop before sending an IP packet.
 * auto next_hop_mac = arp.lookup({10, 0, 0, 1}, now_ms());
 * if (!next_hop_mac) { // try_again: a request is queued; retry the send later
 * }
 *
 * // Main loop: emit queued replies/requests/retries.
 * if (auto tx = arp.poll(now_ms()))
 *   send_ethernet(tx->dst_mac, structo::net::arp_ethertype, tx->payload); // payload valid until next poll()
 * @endcode
 */

#include "arp.hpp"

#if RELOCO_HAS_COROUTINES

namespace structo::net {

/** @brief One outgoing ARP message and the Ethernet destination to put it in. */
struct arp_tx {
  hw::net_mac_address dst_mac;
  span<const std::uint8_t> payload;
};

template <std::size_t N = 8> class arp_table {
  static_assert(N > 0, "arp_table needs at least one slot");

public:
  static constexpr std::uint64_t entry_ttl_ms = 60000;
  static constexpr std::uint64_t retry_interval_ms = 1000;
  static constexpr unsigned max_requests = 3;

  arp_table(const hw::net_mac_address &mac, const ipv4_address &local) noexcept : mac_(mac), local_(local) {}

  void set_address(const ipv4_address &local) noexcept { local_ = local; }
  [[nodiscard]] const ipv4_address &address() const noexcept { return local_; }

  /** @brief Queues a gratuitous ARP announcing our address (e.g. after DHCP binds). */
  void announce() noexcept { announce_ = true; }

  /** @brief Pins `ip` -> `mac`; `error::out_of_range` if the table is full of static entries. */
  [[nodiscard]] result<void> add_static(const ipv4_address &ip, const hw::net_mac_address &mac) noexcept {
    entry *e = find(ip);
    if (!e)
      e = victim();
    if (!e)
      return unexpected(error::out_of_range);
    *e = entry{};
    e->state = slot::fixed;
    e->ip = ip;
    e->mac = mac;
    return {};
  }

  /** @brief Cached MAC for `ip`, or `error::try_again` after queueing a request if it is unknown. */
  [[nodiscard]] result<hw::net_mac_address> lookup(const ipv4_address &ip, std::uint64_t now_ms) noexcept {
    entry *e = find(ip);
    if (e && e->state == slot::pending)
      return unexpected(error::try_again);
    if (e && (e->state == slot::fixed || now_ms < e->expires_ms))
      return e->mac;
    if (!e)
      e = victim();
    if (!e)
      return unexpected(error::out_of_range);
    *e = entry{};
    e->state = slot::pending;
    e->ip = ip;
    e->next_tx_ms = now_ms; // poll() sends the first request immediately
    return unexpected(error::try_again);
  }

  /** @brief Processes a received ARP payload; false if it was malformed. */
  bool handle(span<const std::uint8_t> payload, std::uint64_t now_ms) noexcept {
    auto p = parse_arp(payload);
    if (!p)
      return false;
    if (p->sender_ip.is_unspecified() || p->sender_ip.is_broadcast())
      return true; // probes / garbage: nothing to learn
    const bool for_us = p->target_ip == local_ && !local_.is_unspecified();
    // RFC 826: refresh an existing entry; create one only if the packet is addressed to us.
    entry *e = find(p->sender_ip);
    if (!e && for_us)
      e = victim();
    if (e && e->state != slot::fixed) {
      e->state = slot::learned;
      e->ip = p->sender_ip;
      e->mac = p->sender_mac;
      e->expires_ms = now_ms + entry_ttl_ms;
    }
    if (for_us && p->op == arp_request) {
      reply_pending_ = true;
      reply_to_mac_ = p->sender_mac;
      reply_to_ip_ = p->sender_ip;
    }
    return true;
  }

  /** @brief Next message due at `now_ms` (reply, announcement or request), else `error::try_again`. */
  [[nodiscard]] result<arp_tx> poll(std::uint64_t now_ms) noexcept {
    arp_packet p;
    arp_tx tx{};
    if (reply_pending_) {
      reply_pending_ = false;
      p = {arp_reply, mac_, local_, reply_to_mac_, reply_to_ip_};
      tx.dst_mac = reply_to_mac_;
    } else if (announce_ && !local_.is_unspecified()) {
      announce_ = false;
      p = {arp_request, mac_, local_, {}, local_};
      tx.dst_mac = broadcast_mac;
    } else {
      entry *due = nullptr;
      for (auto &e : table_)
        if (e.state == slot::pending && now_ms >= e.next_tx_ms) {
          if (e.requests >= max_requests) {
            e = entry{}; // gave up
            continue;
          }
          due = &e;
          break;
        }
      if (!due || local_.is_unspecified())
        return unexpected(error::try_again);
      ++due->requests;
      due->next_tx_ms = now_ms + retry_interval_ms;
      p = {arp_request, mac_, local_, {}, due->ip};
      tx.dst_mac = broadcast_mac;
    }
    auto n = build_arp(p, span<std::uint8_t>(out_));
    if (!n)
      return unexpected(n.error());
    tx.payload = span<const std::uint8_t>(out_.data(), *n);
    return tx;
  }

private:
  enum class slot : std::uint8_t { free, pending, learned, fixed };
  struct entry {
    slot state = slot::free;
    ipv4_address ip;
    hw::net_mac_address mac{};
    std::uint64_t expires_ms = 0;
    std::uint64_t next_tx_ms = 0;
    unsigned requests = 0;
  };

  static constexpr hw::net_mac_address broadcast_mac{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

  entry *find(const ipv4_address &ip) noexcept {
    for (auto &e : table_)
      if (e.state != slot::free && e.ip == ip)
        return &e;
    return nullptr;
  }

  // A free slot, else the learned/pending entry closest to expiry (never a static one).
  entry *victim() noexcept {
    entry *best = nullptr;
    for (auto &e : table_) {
      if (e.state == slot::free)
        return &e;
      if (e.state == slot::fixed)
        continue;
      if (!best || (e.state == slot::learned && e.expires_ms < best->expires_ms) ||
          (best->state == slot::pending && e.state == slot::learned))
        best = &e;
    }
    return best;
  }

  hw::net_mac_address mac_;
  ipv4_address local_;
  reloco::array<entry, N> table_{};
  reloco::array<std::uint8_t, arp_packet_size> out_{};
  bool reply_pending_ = false;
  bool announce_ = false;
  hw::net_mac_address reply_to_mac_{};
  ipv4_address reply_to_ip_;
};

} // namespace structo::net

#endif // RELOCO_HAS_COROUTINES
