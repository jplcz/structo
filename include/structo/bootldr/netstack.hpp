// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file netstack.hpp
 * @brief `structo::bootldr::netstack<Mtu>`: the bootloader's network stack
 * running as two tasks of a `bootldr::scheduler` ("semi-active": it only
 * does work when the scheduler gives it a turn). C++20 only.
 *
 * What it does today: owns an `ipv4_node` over a `hw::net_device_ref`,
 * applies a static address or runs a DHCP client, answers ICMP echo (ping),
 * and queues UDP datagrams on bound `udp_socket`s (see `udp_socket.hpp`;
 * sockets are owned by the client and use heap memory). Everything else goes
 * to `set_packet_handler()` or is counted as dropped. No TCP yet.
 *
 * Tasks (frames come from the scheduler's allocator):
 * - *rx*: loops on `ipv4_node::receive()`, feeds DHCP, passes the rest to the
 *   packet handler (or drops it). Receive errors back off for
 *   `error_backoff_ms` so a faulty link cannot starve other tasks.
 * - *timer*: every `tick_ms` lets the DHCP client retransmit/renew.
 *
 * The device itself still has to be polled: `poll_with()` registers a
 * `polled_net_device` as a scheduler poller (an IRQ driven device needs none).
 *
 * @code
 * structo::hw::slip_device<1006> slip{uart};
 * structo::hw::polled_net_device<decltype(slip)> pnd{slip};
 * structo::hw::net_device_ref nic{pnd};
 *
 * structo::bootldr::scheduler sched;     // task table from reloco::default_allocator()
 * sched.set_clock(now_ms, nullptr);      // monotonic ms clock (required: timers)
 *
 * structo::bootldr::netstack_config cfg;
 * cfg.dhcp = true;                       // learn the address; or set cfg.static_ip for a fixed one
 * cfg.xid_seed = hw_random();            // makes DHCP transaction ids device-unique
 *
 * structo::bootldr::netstack<1006> net{sched, nic, cfg}; // 1006 = max IP datagram size (SLIP MTU)
 * net.poll_with(pnd);                    // call pnd.poll() at the start of every scheduler round
 * net.start();                           // spawn the rx and timer tasks
 *
 * // Other tasks may wait for the address before using the network.
 * reloco::task<void> app(structo::bootldr::scheduler &s, structo::bootldr::netstack<1006> &n) {
 *   while (!n.ready())
 *     co_await co_await s.sleep_for(100);  // inner: sleep, outer: unwrap the result<void>
 * }
 *
 * sched.run();
 * @endcode
 */

#include "scheduler.hpp"
#include "udp_socket.hpp"

#include "../hw/polled_net_device.hpp"
#include "../net/dhcp_client.hpp"
#include "../net/ipv4_node.hpp"
#include "../net/ppp.hpp"

#if RELOCO_HAS_COROUTINES

namespace structo::bootldr {

/** @brief Static configuration of a `netstack`. */
struct netstack_config {
  net::ipv4_config static_ip{};            ///< Applied at construction when it has an address.
  bool dhcp = false;                       ///< Run a DHCP client (it overrides `static_ip` once bound).
  hw::net_mac_address mac{};               ///< Used by DHCP; all zero = ask the device, else a built-in local one.
  std::uint32_t xid_seed = 1;              ///< DHCP transaction id seed.
  std::uint32_t tick_ms = 100;             ///< Period of the timer task.
  std::uint32_t error_backoff_ms = 50;     ///< Pause after a failed receive.
};

/** @brief Counters kept by `netstack`. */
struct netstack_stats {
  std::uint32_t rx_dropped = 0;  ///< Datagrams nobody handled.
  std::uint32_t rx_handled = 0;  ///< Datagrams consumed by DHCP or the packet handler.
  std::uint32_t rx_errors = 0;   ///< Failed receives.
  std::uint32_t tx_errors = 0;   ///< Failed DHCP transmissions.
};

template <std::size_t Mtu = 1006> class netstack : public udp_demux {
public:
  /** @brief Handler for IPv4 datagrams the stack does not process itself; the packet is valid only during the call. */
  using packet_fn = void (*)(void *ctx, const net::ipv4_packet &pkt) noexcept;

  netstack(scheduler &sched, hw::net_device_ref dev, const netstack_config &cfg = {}) noexcept
      : udp_demux(sched, this, &send_udp, &local_address), sched_(&sched), cfg_(cfg), ip_(dev, cfg.static_ip),
        dhcp_(resolve_mac(dev, cfg), cfg.xid_seed) {}
  netstack(const netstack &) = delete;
  netstack &operator=(const netstack &) = delete;
  ~netstack() { stop(); }

  /** @brief Registers `pnd.poll()` as a scheduler poller; `pnd` must outlive the scheduler's use of it. */
  template <typename Backend> [[nodiscard]] reloco::result<void> poll_with(hw::polled_net_device<Backend> &pnd) noexcept {
    return sched_->add_poller(
        [](void *p) noexcept { static_cast<hw::polled_net_device<Backend> *>(p)->poll(); }, &pnd);
  }

  /** @brief Spawns the rx and timer tasks. `error::invalid_state` if already started. */
  [[nodiscard]] reloco::result<void> start() noexcept {
    if (running_)
      return reloco::unexpected(reloco::error::invalid_state);
    auto rx = sched_->spawn(rx_loop(reloco::allocator_arg, sched_->allocator(), *this));
    if (!rx)
      return reloco::unexpected(rx.error());
    auto tick = sched_->spawn(timer_loop(reloco::allocator_arg, sched_->allocator(), *this));
    if (!tick) {
      (void)sched_->cancel(*rx);
      return reloco::unexpected(tick.error());
    }
    rx_id_ = *rx;
    timer_id_ = *tick;
    running_ = true;
    return {};
  }

  /** @brief Cancels both tasks (also done by the destructor). */
  void stop() noexcept {
    if (!running_)
      return;
    running_ = false;
    (void)sched_->cancel(rx_id_);
    (void)sched_->cancel(timer_id_);
  }

  /**
   * @brief Takes the address (and gateway) from a PPP link: each timer tick the negotiated IPv4
   * configuration is applied while `link.ip_up()`, and cleared when the link drops. The link must
   * outlive the netstack (usually `ppp_device::link()`).
   */
  void use_ppp(net::ppp_link &link) noexcept { ppp_ = &link; }

  [[nodiscard]] bool running() const noexcept { return running_; }
  /** @brief True once the node has an address (static or leased). */
  [[nodiscard]] bool ready() const noexcept { return ip_.configured(); }
  [[nodiscard]] const net::ipv4_config &config() const noexcept { return ip_.config(); }
  [[nodiscard]] const netstack_stats &stats() const noexcept { return stats_; }
  [[nodiscard]] net::dhcp_state dhcp_state() const noexcept { return dhcp_.state(); }
  /** @brief The IPv4 endpoint, for future protocol layers. */
  [[nodiscard]] net::ipv4_node<Mtu> &ip() noexcept { return ip_; }

  /** @brief Receives every datagram that is not DHCP, ICMP echo or UDP for a bound `udp_socket` (`nullptr` = drop them). */
  void set_packet_handler(packet_fn fn, void *ctx) noexcept {
    handler_ = fn;
    handler_ctx_ = ctx;
  }

private:
  static hw::net_mac_address resolve_mac(hw::net_device_ref dev, const netstack_config &cfg) noexcept {
    bool zero = true;
    for (auto b : cfg.mac)
      zero = zero && b == 0;
    if (!zero)
      return cfg.mac;
    if (auto m = dev.mac_address())
      return *m;
    return {0x02, 0x00, 0x00, 0x00, 0x00, 0x01}; // locally administered
  }

  // udp_demux hooks: transmit a UDP datagram as IPv4, and report our address.
  static reloco::task<void> send_udp(void *self, net::ipv4_address dst,
                                     reloco::span<const std::uint8_t> udp) noexcept {
    auto sent = co_await static_cast<netstack *>(self)->ip_.send(net::ip_proto_udp, dst, udp);
    co_await std::move(sent);
  }
  static net::ipv4_address local_address(void *self) noexcept { return static_cast<netstack *>(self)->ip_.address(); }

  static reloco::task<void> rx_loop(reloco::allocator_arg_t, reloco::allocator_ref, netstack &n) noexcept {
    for (;;) {
      auto pkt = co_await n.ip_.receive();
      if (!pkt) {
        ++n.stats_.rx_errors;
        (void)co_await n.pause(n.cfg_.error_backoff_ms);
        continue;
      }
      if (n.cfg_.dhcp && n.dhcp_.handle(n.ip_, *pkt, n.sched_->now_ms())) {
        ++n.stats_.rx_handled;
      } else if (n.deliver_udp(*pkt)) {
        ++n.stats_.rx_handled; // queued on a bound udp_socket
      } else if (n.handler_) {
        ++n.stats_.rx_handled;
        n.handler_(n.handler_ctx_, *pkt);
      } else {
        ++n.stats_.rx_dropped;
      }
      co_await n.sched_->yield(); // let other tasks run between datagrams
    }
  }

  static reloco::task<void> timer_loop(reloco::allocator_arg_t, reloco::allocator_ref, netstack &n) noexcept {
    for (;;) {
      n.sync_ppp();
      if (n.cfg_.dhcp) {
        auto sent = co_await net::dhcp_send_due(n.ip_, n.dhcp_, n.sched_->now_ms());
        if (!sent)
          ++n.stats_.tx_errors;
      }
      (void)co_await n.pause(n.cfg_.tick_ms);
    }
  }

  void sync_ppp() noexcept {
    if (!ppp_)
      return;
    if (ppp_->ip_up()) {
      const auto want = ppp_->ipv4();
      if (!ip_.configured() || ip_.config().address != want.address)
        ip_.configure(want);
      ppp_applied_ = true;
    } else if (ppp_applied_) {
      ip_.configure(net::ipv4_config{});
      ppp_applied_ = false;
    }
  }

  // Sleeps, or just yields when the scheduler has no clock, so a loop never spins without letting others run.
  reloco::task<void> pause(std::uint32_t ms) noexcept {
    auto r = co_await sched_->sleep_for(ms);
    if (!r)
      (void)co_await sched_->yield();
  }

  scheduler *sched_;
  netstack_config cfg_;
  net::ipv4_node<Mtu> ip_;
  net::dhcp_client dhcp_;
  netstack_stats stats_{};
  packet_fn handler_ = nullptr;
  void *handler_ctx_ = nullptr;
  task_id rx_id_{};
  task_id timer_id_{};
  net::ppp_link *ppp_ = nullptr;
  bool ppp_applied_ = false;
  bool running_ = false;
};

} // namespace structo::bootldr

#endif // RELOCO_HAS_COROUTINES
