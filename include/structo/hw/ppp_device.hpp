// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file ppp_device.hpp
 * @brief `structo::hw::ppp_device<Mtu>`: PPP (RFC 1661/1662, LCP + IPCP) over
 * a `uart_ref` as a *polled* network backend for `polled_net_device` /
 * `net_device_ref`. Part of the C++20-only network stack (empty otherwise).
 *
 * It frames IPv4 datagrams like `slip_device` does, but first brings the link
 * up: LCP, then IPCP, which also *assigns our IP address* (and DNS) from the
 * peer. `link_up()` is false until that is done, and `try_send()` reports
 * `error::try_again` meanwhile, so coroutines simply wait for the link.
 * Negotiation lives in `net::ppp_link`; the device only moves bytes, so
 * control traffic is processed whenever the device is serviced: every
 * `try_receive()`/`try_send()` and `service()`. A clock is required for the
 * retransmission timers.
 *
 * @code
 * std::uint64_t now_ms(void *) noexcept;                    // your monotonic millisecond clock
 *
 * structo::hw::uart_ref uart{my_uart};                      // the serial line
 * structo::net::ppp_config cfg;                             // defaults: peer assigns our address and DNS
 * structo::hw::ppp_device<1500> ppp{uart, now_ms, nullptr, cfg}; // 1500 = largest IP datagram; starts negotiating
 * structo::hw::polled_net_device<decltype(ppp)> pnd{ppp};   // adds coroutine send/receive
 * structo::hw::net_device_ref nic{pnd};                     // what the IP stack consumes
 * @endcode
 */

#include <reloco/coroutine.hpp>

#if RELOCO_HAS_COROUTINES

#include "ppp_framing.hpp"
#include "uart_ref.hpp"

#include "../net/ppp.hpp"

#include <reloco/array.hpp>

namespace structo::hw {

template <std::size_t Mtu = 1500> class ppp_device {
public:
  using clock_fn = std::uint64_t (*)(void *) noexcept;

  /**
   * @param uart The serial line (8N1, no flow control assumed).
   * @param clock Monotonic millisecond clock, called with `clock_ctx`.
   * @param cfg PPP settings; `mru` is set to `Mtu` so the peer never sends more than we can hold.
   */
  ppp_device(uart_ref uart, clock_fn clock, void *clock_ctx, net::ppp_config cfg = {}) noexcept
      : uart_(uart), clock_(clock), clock_ctx_(clock_ctx), link_(with_mru(cfg)), dec_(reloco::span<std::uint8_t>(rx_)) {
    link_.open();
  }
  ppp_device(const ppp_device &) = delete;
  ppp_device &operator=(const ppp_device &) = delete;

  [[nodiscard]] std::size_t mtu() const noexcept { return Mtu; }
  /** @brief True once IPCP is open and we have an address. */
  [[nodiscard]] reloco::result<bool> link_up() noexcept { return link_.ip_up(); }

  /** @brief The negotiation engine, for addresses and states. */
  [[nodiscard]] net::ppp_link &link() noexcept { return link_; }
  [[nodiscard]] const net::ppp_link &link() const noexcept { return link_; }

  /** @brief Moves pending control frames and queued TX bytes to the UART; never blocks. */
  reloco::result<void> service() noexcept {
    for (;;) {
      if (auto d = drain(); !d)
        return d;
      if (tx_pos_ < tx_len_)
        return {}; // UART is full
      auto pkt = link_.next_packet(clock_(clock_ctx_));
      if (!pkt)
        return {};
      auto n = ppp_encode(pkt->protocol, pkt->data, reloco::span<std::uint8_t>(tx_));
      if (!n)
        return reloco::unexpected(n.error());
      tx_len_ = n.value();
      tx_pos_ = 0;
    }
  }

  /** @brief Queues one IPv4 datagram; `try_again` while the link is down or the previous frame is draining. */
  [[nodiscard]] reloco::result<void> try_send(reloco::span<const std::uint8_t> frame) noexcept {
    if (frame.size() > Mtu || frame.size() > link_.peer_mru())
      return reloco::unexpected(reloco::error::out_of_range);
    if (auto s = service(); !s)
      return s;
    if (!link_.ip_up() || tx_pos_ < tx_len_)
      return reloco::unexpected(reloco::error::try_again);
    auto n = ppp_encode(ppp_proto_ip, frame, reloco::span<std::uint8_t>(tx_));
    if (!n)
      return reloco::unexpected(n.error());
    tx_len_ = n.value();
    tx_pos_ = 0;
    return drain();
  }

  /**
   * @brief Drains the UART RX FIFO, runs control frames through the link and copies out one IPv4 datagram.
   * `try_again` if none has arrived; `out_of_range` (datagram dropped) if `dst` is too small.
   */
  [[nodiscard]] reloco::result<std::size_t> try_receive(reloco::span<std::uint8_t> dst) noexcept {
    if (auto s = service(); !s)
      return reloco::unexpected(s.error());
    for (;;) {
      auto ready = uart_.rx_ready();
      if (!ready)
        return reloco::unexpected(ready.error());
      if (!ready.value())
        return reloco::unexpected(reloco::error::try_again);
      auto b = uart_.try_get_byte();
      if (!b)
        return reloco::unexpected(b.error());
      if (!dec_.push(b.value()))
        continue;
      const ppp_frame f = dec_.frame();
      if (link_.on_frame(f.protocol, f.payload, clock_(clock_ctx_))) {
        if (auto s = service(); !s) // push the answer out right away
          return reloco::unexpected(s.error());
        continue;
      }
      if (!link_.ip_up())
        continue; // IP before IPCP finished: ignore
      if (f.payload.size() > dst.size())
        return reloco::unexpected(reloco::error::out_of_range);
      for (std::size_t i = 0; i < f.payload.size(); ++i)
        dst[i] = f.payload[i];
      return f.payload.size();
    }
  }

  /** @brief Malformed/oversized received frames discarded so far. */
  [[nodiscard]] std::size_t rx_dropped() const noexcept { return dec_.dropped(); }

private:
  static constexpr std::size_t control_max = 96; // largest control frame of ppp_link
  static constexpr std::size_t payload_max = Mtu > control_max ? Mtu : control_max;

  static net::ppp_config with_mru(net::ppp_config cfg) noexcept {
    cfg.mru = static_cast<std::uint16_t>(Mtu);
    return cfg;
  }

  reloco::result<void> drain() noexcept {
    while (tx_pos_ < tx_len_) {
      auto ready = uart_.tx_ready();
      if (!ready)
        return reloco::unexpected(ready.error());
      if (!ready.value())
        return {};
      auto r = uart_.try_put_byte(tx_[tx_pos_]);
      if (!r)
        return r;
      ++tx_pos_;
    }
    return {};
  }

  uart_ref uart_;
  clock_fn clock_;
  void *clock_ctx_;
  net::ppp_link link_;
  reloco::array<std::uint8_t, Mtu + 8> rx_{};
  reloco::array<std::uint8_t, ppp_max_encoded_size(payload_max)> tx_{};
  ppp_decoder dec_;
  std::size_t tx_len_ = 0;
  std::size_t tx_pos_ = 0;
};

} // namespace structo::hw

#endif // RELOCO_HAS_COROUTINES
