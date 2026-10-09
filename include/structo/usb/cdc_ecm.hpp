// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file cdc_ecm.hpp
 * @brief USB CDC-ECM (Ethernet over USB) host driver exposed as a raw-Ethernet-frame `hw::net_device_ref`
 * backend. C++20 only. RNDIS is not implemented.
 *
 * Plug it under `hw::ethernet_device` / `ethernet_nic`-style layers exactly like any other raw-frame device.
 *
 * @code
 * structo::usb::cdc_ecm ecm;                    // one per attached adapter
 * co_await ecm.attach(dev);                     // dev: configured usb_device; reads MAC, selects the data alt setting
 * structo::hw::net_device_ref raw{ecm};         // raw Ethernet frames in/out (frame = dst MAC .. payload, no FCS)
 * auto mac = raw.mac_address();                 // from the iMACAddress string descriptor
 * co_await raw.send(frame);                     // one bulk OUT transfer (+ ZLP when a multiple of the packet size)
 * auto n = co_await raw.receive(buf);           // buf must be >= mtu() + 14 bytes; one frame per call
 * @endcode
 */

#include "../hw/net_device_ref.hpp"
#include "cdc_acm.hpp"

#if RELOCO_HAS_COROUTINES

namespace structo::usb {

class cdc_ecm {
public:
  static constexpr std::uint8_t subtype_ethernet = 0x0F;
  /** @brief Receive buffers must hold a full frame: 14-byte header + MTU. */
  static constexpr std::size_t frame_overhead = 14;

  [[nodiscard]] reloco::task<void> attach(usb_device &dev) noexcept {
    config_view cfg = dev.config();
    auto comm = cfg.find_interface(usb_class::cdc, cdc::subclass_ecm);
    if (!comm)
      co_await reloco::unexpected(comm.error());
    auto fd = cfg.find_cs_descriptor(*comm, subtype_ethernet);
    if (!fd || fd->size() < 13)
      co_await reloco::unexpected(reloco::error::invalid_argument);

    const std::uint8_t mac_string = (*fd)[3];
    const std::uint16_t segment = le16((*fd).subspan(8));
    mtu_ = segment >= 576 && segment <= 1500 ? segment : std::size_t{1500};

    // MAC is 12 hex digits in a string descriptor.
    reloco::array<char, 24> hex{};
    auto n = co_await dev.get_string(mac_string, reloco::span<char>(hex.data(), hex.size()));
    if (!n)
      co_await reloco::unexpected(n.error());
    if (*n != 12)
      co_await reloco::unexpected(reloco::error::invalid_argument);
    for (std::size_t i = 0; i < 6; ++i) {
      int hi = hex_value(hex[2 * i]);
      int lo = hex_value(hex[2 * i + 1]);
      if (hi < 0 || lo < 0)
        co_await reloco::unexpected(reloco::error::invalid_argument);
      mac_[i] = static_cast<std::uint8_t>(hi * 16 + lo);
    }

    // Data interface: the alt setting that carries endpoints is the active one.
    auto data = cfg.find_interface(usb_class::cdc_data, any, any, 1);
    if (!data)
      data = cfg.find_interface(usb_class::cdc_data, any, any, 0);
    if (!data)
      co_await reloco::unexpected(data.error());
    const endpoint_descriptor *in = data->find_endpoint(hw::usb_transfer_type::bulk, hw::usb_direction::in);
    const endpoint_descriptor *out = data->find_endpoint(hw::usb_transfer_type::bulk, hw::usb_direction::out);
    if (!in || !out || in->max_packet == 0 || out->max_packet == 0)
      co_await reloco::unexpected(reloco::error::not_found);

    if (data->alt != 0) {
      auto r = co_await dev.set_interface(*data);
      if (!r)
        co_await reloco::unexpected(r.error());
    }

    // Packet filter: directed + broadcast + all-multicast.
    hw::usb_setup_packet s{request_type::class_out_interface, cdc::req_set_ethernet_packet_filter, 0x000E, comm->number,
                           0};
    auto r = co_await dev.control(s);
    if (!r)
      co_await reloco::unexpected(r.error());

    dev_ = &dev;
    in_ = dev.pipe_for(*in);
    out_ = dev.pipe_for(*out);
    attached_ = true;
  }

  void detach() noexcept { attached_ = false; }
  [[nodiscard]] bool attached() const noexcept { return attached_; }
  [[nodiscard]] std::size_t mtu() const noexcept { return mtu_; }
  [[nodiscard]] hw::net_mac_address mac() const noexcept { return mac_; }

  [[nodiscard]] reloco::task<void> send(reloco::span<const std::uint8_t> frame) noexcept {
    if (!attached_)
      co_await reloco::unexpected(reloco::error::not_initialized);
    if (frame.size() > mtu_ + frame_overhead)
      co_await reloco::unexpected(reloco::error::invalid_argument);
    if (tx_busy_)
      co_await reloco::unexpected(reloco::error::busy);
    tx_busy_ = true;
    hw::usb_completion c = co_await dev_->controller().out(out_, frame);
    // A transfer that is an exact multiple of the packet size needs a zero-length packet to end the frame.
    if (c.ok() && !frame.empty() && frame.size() % out_.max_packet == 0)
      c = co_await dev_->controller().out(out_, reloco::span<const std::uint8_t>{});
    tx_busy_ = false;
    if (c.status == hw::usb_status::stall) {
      (void)co_await dev_->clear_halt(out_);
    }
    auto r = c.to_result();
    if (!r)
      co_await reloco::unexpected(r.error());
  }

  /** @brief Receives one frame (a short packet ends it). `dst` should be at least `mtu() + 14` bytes. */
  [[nodiscard]] reloco::task<std::size_t> receive(reloco::span<std::uint8_t> dst) noexcept {
    if (!attached_)
      co_return reloco::unexpected(reloco::error::not_initialized);
    hw::usb_completion c = co_await dev_->controller().in(in_, dst);
    if (c.status == hw::usb_status::stall)
      (void)co_await dev_->clear_halt(in_);
    auto r = c.to_result();
    if (!r)
      co_return reloco::unexpected(r.error());
    co_return *r;
  }

private:
  static int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  }

  usb_device *dev_ = nullptr;
  hw::usb_pipe in_{};
  hw::usb_pipe out_{};
  hw::net_mac_address mac_{};
  std::size_t mtu_ = 1500;
  bool attached_ = false;
  bool tx_busy_ = false;
};

} // namespace structo::usb

namespace structo::hw {

template <> struct net_device_traits<usb::cdc_ecm> {
  static std::size_t mtu(const usb::cdc_ecm &d) noexcept { return d.mtu(); }
  static reloco::result<bool> link_up(usb::cdc_ecm &d) noexcept { return d.attached(); }
  static reloco::result<net_mac_address> mac_address(usb::cdc_ecm &d) noexcept { return d.mac(); }
  static reloco::task<void> send(usb::cdc_ecm &d, reloco::span<const std::uint8_t> f) noexcept { return d.send(f); }
  static reloco::task<std::size_t> receive(usb::cdc_ecm &d, reloco::span<std::uint8_t> dst) noexcept {
    return d.receive(dst);
  }
};

} // namespace structo::hw

#endif // RELOCO_HAS_COROUTINES
