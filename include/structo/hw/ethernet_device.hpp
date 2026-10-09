// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file ethernet_device.hpp
 * @brief `structo::hw::ethernet_device<Mtu, ArpSlots>`: the Ethernet layer
 * of the network stack. Wraps a raw Ethernet NIC (a `net_device_ref` that
 * carries whole Ethernet frames) and presents it as a `net_device_ref` that
 * carries bare IPv4 datagrams -- exactly what `slip_device`/`ppp_device`
 * present -- so `net::ipv4_node`, DHCP, UDP and TFTP run over Ethernet
 * unchanged. C++20 only.
 *
 * What it does:
 * - **TX**: picks the next hop (on-link destination, else the gateway), resolves
 *   its MAC through an `arp_table`, wraps the datagram in an Ethernet II frame.
 *   Limited and directed-broadcast destinations get the broadcast MAC, multicast
 *   ones `01:00:5e:...`. If the MAC is not known yet the datagram is parked in a
 *   single slot and an ARP request is sent; it goes out when the reply arrives, or
 *   is dropped after `pending_timeout_ms` (upper layers retransmit). A newer parked
 *   datagram replaces an older one.
 * - **RX**: drops frames not addressed to us (unicast MAC, broadcast, multicast),
 *   answers/learns ARP, and delivers IPv4 payloads (padding trimmed with the
 *   datagram's own length). Other ethertypes are dropped.
 * - **ARP housekeeping**: replies, announcements and request retries are emitted
 *   by `service()`, which must run periodically (`bootldr::netstack::use_ethernet`
 *   does this from its timer task) and also runs after every received ARP frame.
 *
 * Addressing is pushed in with `configure()` (netstack does it when the static or
 * DHCP address changes). Unconfigured, only broadcast/multicast traffic can be
 * sent, which is all DHCP needs.
 *
 * Time comes from a millisecond callback or, like `ppp_device`, a
 * `clock_reader`/`atomic_clock_reader` (`clock_ref.hpp`).
 *
 * @code
 * // `my_eth_nic` is a polled Ethernet NIC driver (try_send/try_receive of whole frames, a MAC).
 * my_eth_nic raw_nic;
 * structo::hw::polled_net_device<my_eth_nic> raw_pnd{raw_nic};
 * structo::hw::net_device_ref raw{raw_pnd};             // frames in, frames out
 *
 * // `clock` is an already reset() structo::hw::clock_reader; ARP timeouts are measured with it.
 * // 1500 = largest IPv4 datagram carried; 8 = ARP cache slots.
 * structo::hw::ethernet_device<1500, 8> eth{raw, clock};
 * structo::hw::net_device_ref nic{eth};                 // IP datagrams in, IP datagrams out
 *
 * structo::bootldr::netstack<1500> net{sched, nic, cfg};
 * net.use_ethernet(eth);                                // push IP config into it, run its ARP service
 * net.poll_with(raw_pnd);                               // poll the real NIC every scheduler round
 * @endcode
 */

#include "../net/arp_table.hpp"
#include "../net/ethernet.hpp"
#include "../net/ipv4_config.hpp"
#include "clock_ref.hpp"
#include "net_device_ref.hpp"

#include <reloco/coroutine.hpp>

#if RELOCO_HAS_COROUTINES

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace structo::hw {

template <std::size_t Mtu = 1500, std::size_t ArpSlots = 8> class ethernet_device {
  static_assert(Mtu >= 68, "an IPv4 link must carry at least 68 bytes (RFC 791)");

public:
  using clock_fn = std::uint64_t (*)(void *) noexcept;

  /** @brief How long a datagram waits for ARP resolution before it is dropped. */
  static constexpr std::uint64_t pending_timeout_ms = 3000;

  /**
   * @param raw NIC carrying Ethernet frames (must outlive this device).
   * @param clock Monotonic millisecond clock, called with `clock_ctx`.
   * @param mac Our MAC; all zero = ask `raw.mac_address()`, else a built-in locally administered one.
   */
  ethernet_device(net_device_ref raw, clock_fn clock, void *clock_ctx, const net_mac_address &mac = {}) noexcept
      : raw_(raw), clock_(clock), clock_ctx_(clock_ctx), mac_(resolve_mac(raw, mac)), arp_(mac_, {}) {}

  /** @brief Same, with time from a `clock_reader`/`atomic_clock_reader` (already `reset()`). */
  template <
      typename Reader,
      std::enable_if_t<std::is_same_v<Reader, clock_reader> || std::is_same_v<Reader, atomic_clock_reader>, int> = 0>
  ethernet_device(net_device_ref raw, Reader &reader, const net_mac_address &mac = {}) noexcept
      : ethernet_device(raw, &reader_now_ms<Reader>, &reader, mac) {}

  ethernet_device(const ethernet_device &) = delete;
  ethernet_device &operator=(const ethernet_device &) = delete;

  [[nodiscard]] const net_mac_address &mac() const noexcept { return mac_; }

  /** @brief Direct access to the ARP cache, e.g. for `add_static()`. */
  [[nodiscard]] net::arp_table<ArpSlots> &arp() noexcept { return arp_; }

  /** @brief Applies address/netmask/gateway; a newly set address is announced with a gratuitous ARP. */
  void configure(const net::ipv4_config &cfg) noexcept {
    const bool changed = cfg.address != arp_.address();
    cfg_ = cfg;
    arp_.set_address(cfg.address);
    if (changed && cfg.configured())
      arp_.announce();
  }

  [[nodiscard]] const net::ipv4_config &config() const noexcept { return cfg_; }
  [[nodiscard]] bool has_pending() const noexcept { return pending_len_ != 0; }

  // --- net_device_traits backend interface (IP datagrams) ---

  [[nodiscard]] std::size_t mtu() const noexcept { return Mtu; }
  [[nodiscard]] result<bool> link_up() noexcept { return raw_.link_up(); }

  /**
   * @brief Sends one IPv4 datagram (parked behind ARP resolution if needed). Fails with `error::invalid_argument`
   * if it is not a plausible IPv4 header, `error::out_of_range` if larger than `Mtu`, `error::busy` if another
   * send is in flight, `error::invalid_state` for a unicast destination while unconfigured.
   */
  [[nodiscard]] reloco::task<void> send(span<const std::uint8_t> datagram) noexcept {
    if (datagram.size() < 20 || (datagram[0] >> 4) != 4)
      co_await unexpected(error::invalid_argument);
    if (datagram.size() > Mtu)
      co_await unexpected(error::out_of_range);
    if (tx_busy_)
      co_await unexpected(error::busy);
    flag_guard guard{tx_busy_};

    const net::ipv4_address dst = ip_at(datagram, 16);
    const std::uint64_t now = clock_(clock_ctx_);
    auto mac = next_hop_mac(dst, now);
    if (!mac) {
      if (mac.error() != error::try_again)
        co_await unexpected(mac.error());
      park(datagram, now); // an ARP request is queued; flushed once the reply is learned
      (void)co_await service_arp(now);
      co_return;
    }
    (void)co_await transmit(*mac, datagram);
  }

  /** @brief Awaits the next IPv4 datagram addressed to us, copied into `dst` (ARP is handled on the way). */
  [[nodiscard]] reloco::task<std::size_t> receive(span<std::uint8_t> dst) noexcept {
    for (;;) {
      auto got = co_await raw_.receive(span<std::uint8_t>(rx_));
      std::size_t n = co_await std::move(got);
      auto f = net::parse_ethernet(span<const std::uint8_t>(rx_.data(), n));
      if (!f || !accepts(f->dst))
        continue;
      if (f->ethertype == net::arp_ethertype) {
        const std::uint64_t now = clock_(clock_ctx_);
        if (arp_.handle(f->payload, now))
          (void)co_await service_arp(now);
        continue;
      }
      if (f->ethertype != net::ipv4_ethertype || f->payload.size() < 20)
        continue;
      std::size_t len = f->payload.size();
      const std::size_t total = (static_cast<std::size_t>(f->payload[2]) << 8) | f->payload[3];
      if (total >= 20 && total <= len)
        len = total; // trim Ethernet minimum-size padding
      if (len > dst.size())
        continue;
      for (std::size_t i = 0; i < len; ++i)
        dst[i] = f->payload[i];
      co_return len;
    }
  }

  /**
   * @brief ARP housekeeping: sends due ARP replies/announcements/requests and releases or expires the parked
   * datagram. Cheap when idle; call it periodically. NIC errors are not reported (ARP retries).
   */
  [[nodiscard]] reloco::task<void> service() noexcept { (void)co_await service_arp(clock_(clock_ctx_)); }

private:
  struct flag_guard {
    bool &f;
    explicit flag_guard(bool &flag) noexcept : f(flag) { f = true; }
    ~flag_guard() { f = false; }
  };

  static net_mac_address resolve_mac(net_device_ref raw, const net_mac_address &mac) noexcept {
    bool zero = true;
    for (auto b : mac)
      zero = zero && b == 0;
    if (!zero)
      return mac;
    if (auto m = raw.mac_address())
      return *m;
    return {0x02, 0x00, 0x00, 0x00, 0x00, 0x01}; // locally administered
  }

  static net::ipv4_address ip_at(span<const std::uint8_t> d, std::size_t off) noexcept {
    return net::ipv4_address(d[off], d[off + 1], d[off + 2], d[off + 3]);
  }

  [[nodiscard]] bool accepts(const net_mac_address &dst) const noexcept {
    return dst == mac_ || net::is_group_mac(dst);
  }

  [[nodiscard]] bool on_link(const net::ipv4_address &ip) const noexcept {
    for (std::size_t i = 0; i < 4; ++i)
      if ((ip.octets[i] & cfg_.netmask.octets[i]) != (cfg_.address.octets[i] & cfg_.netmask.octets[i]))
        return false;
    return true;
  }

  [[nodiscard]] bool directed_broadcast(const net::ipv4_address &ip) const noexcept {
    if (cfg_.netmask.is_unspecified() || !on_link(ip))
      return false;
    for (std::size_t i = 0; i < 4; ++i)
      if ((ip.octets[i] | cfg_.netmask.octets[i]) != 0xFF)
        return false;
    return true;
  }

  // The MAC for `dst`'s next hop, or `error::try_again` once an ARP request is queued for it.
  [[nodiscard]] result<net_mac_address> next_hop_mac(const net::ipv4_address &dst, std::uint64_t now) noexcept {
    if (dst.is_broadcast() || directed_broadcast(dst))
      return net::ethernet_broadcast_mac;
    if ((dst.octets[0] & 0xF0) == 0xE0)
      return net::ipv4_multicast_mac(dst);
    if (cfg_.address.is_unspecified())
      return unexpected(error::invalid_state); // cannot ARP without a source address
    const bool direct = cfg_.netmask.is_unspecified() || on_link(dst);
    pending_hop_ = direct || cfg_.gateway.is_unspecified() ? dst : cfg_.gateway;
    return arp_.lookup(pending_hop_, now);
  }

  void park(span<const std::uint8_t> datagram, std::uint64_t now) noexcept {
    for (std::size_t i = 0; i < datagram.size(); ++i)
      pending_[i] = datagram[i];
    pending_len_ = datagram.size();
    pending_since_ = now;
  }

  reloco::task<void> transmit(net_mac_address mac, span<const std::uint8_t> datagram) noexcept {
    std::size_t n = co_await net::build_ethernet(mac, mac_, net::ipv4_ethertype, datagram, span<std::uint8_t>(tx_));
    auto sent = co_await raw_.send(span<const std::uint8_t>(tx_.data(), n));
    co_await std::move(sent);
  }

  reloco::task<void> service_arp(std::uint64_t now) noexcept {
    // ARP frames use their own buffer, so this can run while a datagram send is parked on the NIC.
    // NIC errors are ignored: requests are retried, and a lost reply is re-requested by the peer.
    if (!arp_busy_) {
      flag_guard guard{arp_busy_};
      for (unsigned i = 0; i < 4; ++i) {
        auto tx = arp_.poll(now);
        if (!tx)
          break;
        auto n =
            net::build_ethernet(tx->dst_mac, mac_, net::arp_ethertype, tx->payload, span<std::uint8_t>(arp_frame_));
        if (!n)
          break;
        (void)co_await raw_.send(span<const std::uint8_t>(arp_frame_.data(), *n));
      }
    }

    if (pending_len_ == 0)
      co_return;
    if (now - pending_since_ >= pending_timeout_ms) {
      pending_len_ = 0;
      co_return;
    }
    if (tx_busy_)
      co_return;
    auto mac = arp_.lookup(pending_hop_, now);
    if (!mac)
      co_return;
    flag_guard guard{tx_busy_};
    const std::size_t len = pending_len_;
    pending_len_ = 0;
    (void)co_await transmit(*mac, span<const std::uint8_t>(pending_.data(), len));
  }

  net_device_ref raw_;
  clock_fn clock_;
  void *clock_ctx_;
  net_mac_address mac_;
  net::ipv4_config cfg_{};
  net::arp_table<ArpSlots> arp_;
  reloco::array<std::uint8_t, Mtu + net::ethernet_header_size> rx_{};
  reloco::array<std::uint8_t, Mtu + net::ethernet_header_size> tx_{};
  reloco::array<std::uint8_t, net::ethernet_header_size + net::arp_packet_size> arp_frame_{};
  reloco::array<std::uint8_t, Mtu> pending_{};
  std::size_t pending_len_ = 0;
  std::uint64_t pending_since_ = 0;
  net::ipv4_address pending_hop_{};
  bool tx_busy_ = false;
  bool arp_busy_ = false;
};

template <std::size_t Mtu, std::size_t ArpSlots> struct net_device_traits<ethernet_device<Mtu, ArpSlots>> {
  using device = ethernet_device<Mtu, ArpSlots>;
  static std::size_t mtu(const device &d) noexcept { return d.mtu(); }
  static result<bool> link_up(device &d) noexcept { return d.link_up(); }
  static result<net_mac_address> mac_address(device &d) noexcept { return d.mac(); }
  static task<std::size_t> receive(device &d, span<std::uint8_t> dst) noexcept { return d.receive(dst); }
  static task<void> send(device &d, span<const std::uint8_t> frame) noexcept { return d.send(frame); }
};

} // namespace structo::hw

#endif // RELOCO_HAS_COROUTINES
