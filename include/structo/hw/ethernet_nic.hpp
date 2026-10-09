// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file ethernet_nic.hpp
 * @brief `structo::hw::ethernet_nic<Driver>`: the adaptation layer for writing real Ethernet hardware
 * drivers. A driver only implements the few descriptor-ring primitives of its MAC; the adapter turns
 * them into a `polled_net_device` backend (raw Ethernet frames), taking care of the chores every
 * driver would otherwise repeat: frame size validation, minimum-frame padding, FCS stripping,
 * dropping runt/oversized/oversize-for-caller frames so the RX ring never wedges, and counters.
 * C++20 only (empty otherwise).
 *
 * Driver contract (all members `noexcept`; "no progress now" is `error::try_again`):
 *
 * @code
 * struct my_mac_driver {
 *   // Burned-in / configured station address.
 *   structo::hw::net_mac_address read_mac() noexcept;
 *   // PHY link state (cable plugged in, negotiation finished).
 *   bool link_up() noexcept;
 *
 *   // TX: copy `frame` (already >= 60 bytes, without FCS) into the next free TX descriptor/buffer and
 *   // hand it to the hardware. error::try_again if the TX ring is full.
 *   reloco::result<void> tx_submit(reloco::span<const std::uint8_t> frame) noexcept;
 *
 *   // RX (zero-copy): view the oldest frame the hardware has completed. The span stays valid until
 *   // rx_release(). error::try_again if the RX ring is empty.
 *   reloco::result<reloco::span<const std::uint8_t>> rx_peek() noexcept;
 *   // Return that frame's descriptor to the hardware (the adapter always calls it after a peek).
 *   void rx_release() noexcept;
 *
 *   // Optional: true if the hardware leaves the 4-byte FCS on received frames (adapter strips it).
 *   static constexpr bool rx_includes_fcs = false;
 *   // Optional: largest frame (without FCS) the driver can send/receive; default 1514.
 *   static constexpr std::size_t max_frame_size = 1514;
 *   // Optional MAC filter controls, forwarded by ethernet_nic::set_promiscuous/add_multicast:
 *   reloco::result<void> set_promiscuous(bool on) noexcept;
 *   reloco::result<void> add_multicast(const structo::hw::net_mac_address &group) noexcept;
 * };
 * @endcode
 *
 * Plugging a driver into the stack:
 *
 * @code
 * my_mac_driver drv;                                     // the hardware driver (register access, rings)
 * structo::hw::ethernet_nic<my_mac_driver> nic{drv};     // validated raw-frame backend
 * structo::hw::polled_net_device<decltype(nic)> pnd{nic};// coroutine interface, retried by pnd.poll()
 * structo::hw::net_device_ref raw{pnd};
 * structo::hw::ethernet_device<1500> eth{raw, now_ms, nullptr}; // ARP + IPv4 on top
 * @endcode
 */

#include "net_device_ref.hpp"

#include <reloco/coroutine.hpp>
#include <reloco/detail/compat.hpp>

#if RELOCO_HAS_COROUTINES

#include <reloco/array.hpp>
#include <reloco/error.hpp>
#include <reloco/span.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace structo::hw {

/** @brief Counters kept by `ethernet_nic`. */
struct ethernet_nic_stats {
  std::uint64_t tx_frames = 0;
  std::uint64_t tx_bytes = 0;
  std::uint64_t tx_ring_full = 0; ///< `tx_submit` reported `try_again` (the caller retries).
  std::uint64_t tx_errors = 0;    ///< Driver failures other than a full ring.
  std::uint64_t rx_frames = 0;
  std::uint64_t rx_bytes = 0;
  std::uint64_t rx_runts = 0;     ///< Dropped: shorter than an Ethernet header.
  std::uint64_t rx_oversized = 0; ///< Dropped: longer than `max_frame_size`, or than the caller's buffer.
  std::uint64_t rx_errors = 0;    ///< Driver failures other than an empty ring.
};

namespace detail {

template <typename D, typename = void> struct nic_fcs : std::false_type {};
template <typename D>
struct nic_fcs<D, std::void_t<decltype(D::rx_includes_fcs)>> : std::bool_constant<D::rx_includes_fcs> {};

template <typename D, typename = void> struct nic_max_frame : std::integral_constant<std::size_t, 1514> {};
template <typename D>
struct nic_max_frame<D, std::void_t<decltype(D::max_frame_size)>>
    : std::integral_constant<std::size_t, D::max_frame_size> {};

template <typename D, typename = void> struct nic_has_promisc : std::false_type {};
template <typename D>
struct nic_has_promisc<D, std::void_t<decltype(std::declval<D &>().set_promiscuous(true))>> : std::true_type {};

template <typename D, typename = void> struct nic_has_multicast : std::false_type {};
template <typename D>
struct nic_has_multicast<
    D, std::void_t<decltype(std::declval<D &>().add_multicast(std::declval<const net_mac_address &>()))>>
    : std::true_type {};

} // namespace detail

template <typename Driver> class ethernet_nic {
public:
  /** @brief Ethernet header (14) + payload, no FCS. */
  static constexpr std::size_t header_size = 14;
  /** @brief Smallest frame put on the wire (without FCS); shorter ones are zero-padded. */
  static constexpr std::size_t min_frame_size = 60;
  static constexpr std::size_t max_frame_size = detail::nic_max_frame<Driver>::value;
  static constexpr std::size_t fcs_size = 4;

  static_assert(max_frame_size > min_frame_size, "max_frame_size too small");

  /** @param drv Driver to wrap; must outlive this adapter. */
  explicit ethernet_nic(Driver &drv) noexcept : drv_(&drv) {}

  [[nodiscard]] Driver &driver() noexcept { return *drv_; }
  [[nodiscard]] const ethernet_nic_stats &stats() const noexcept { return stats_; }

  // --- polled_net_device backend interface (raw Ethernet frames) ---

  [[nodiscard]] std::size_t mtu() const noexcept { return max_frame_size; }
  [[nodiscard]] result<bool> link_up() noexcept { return drv_->link_up(); }
  [[nodiscard]] result<net_mac_address> mac_address() noexcept { return drv_->read_mac(); }

  /**
   * @brief Sends one frame (no FCS). Frames shorter than `min_frame_size` are zero-padded.
   * `error::invalid_argument` if shorter than an Ethernet header, `error::out_of_range` if longer than
   * `max_frame_size`, `error::try_again` if the TX ring is full.
   */
  [[nodiscard]] result<void> try_send(reloco::span<const std::uint8_t> frame) noexcept {
    if (frame.size() < header_size)
      return unexpected(error::invalid_argument);
    if (frame.size() > max_frame_size)
      return unexpected(error::out_of_range);
    result<void> r;
    if (frame.size() < min_frame_size) {
      reloco::array<std::uint8_t, min_frame_size> padded{};
      for (std::size_t i = 0; i < frame.size(); ++i)
        padded[i] = frame[i];
      r = drv_->tx_submit(reloco::span<const std::uint8_t>(padded.data(), min_frame_size));
    } else {
      r = drv_->tx_submit(frame);
    }
    if (r) {
      ++stats_.tx_frames;
      stats_.tx_bytes += frame.size();
    } else if (r.error() == error::try_again) {
      ++stats_.tx_ring_full;
    } else {
      ++stats_.tx_errors;
    }
    return r;
  }

  /**
   * @brief Copies the next valid received frame (FCS stripped) into `dst`. Runt frames and frames that
   * do not fit `max_frame_size` or `dst` are dropped and counted (the RX descriptor is always returned
   * to the hardware); the next one is tried. `error::try_again` when the ring is empty.
   */
  [[nodiscard]] result<std::size_t> try_receive(reloco::span<std::uint8_t> dst) noexcept {
    for (;;) {
      auto peek = drv_->rx_peek();
      if (!peek) {
        if (peek.error() != error::try_again)
          ++stats_.rx_errors;
        return unexpected(peek.error());
      }
      std::size_t len = peek->size();
      if constexpr (detail::nic_fcs<Driver>::value) {
        len = len >= fcs_size ? len - fcs_size : 0;
      }
      if (len < header_size) {
        ++stats_.rx_runts;
      } else if (len > max_frame_size || len > dst.size()) {
        ++stats_.rx_oversized;
      } else {
        for (std::size_t i = 0; i < len; ++i)
          dst[i] = (*peek)[i];
        drv_->rx_release();
        ++stats_.rx_frames;
        stats_.rx_bytes += len;
        return len;
      }
      drv_->rx_release();
    }
  }

  /** @brief Enables/disables promiscuous reception; `error::unsupported_operation` if the driver has no such control.
   */
  [[nodiscard]] result<void> set_promiscuous(bool on) noexcept {
    if constexpr (detail::nic_has_promisc<Driver>::value)
      return drv_->set_promiscuous(on);
    else
      return unexpected(error::unsupported_operation);
  }

  /** @brief Accepts frames for a multicast group; `error::unsupported_operation` if the driver has no such control. */
  [[nodiscard]] result<void> add_multicast(const net_mac_address &group) noexcept {
    if constexpr (detail::nic_has_multicast<Driver>::value)
      return drv_->add_multicast(group);
    else
      return unexpected(error::unsupported_operation);
  }

private:
  Driver *drv_;
  ethernet_nic_stats stats_{};
};

} // namespace structo::hw

#endif // RELOCO_HAS_COROUTINES
