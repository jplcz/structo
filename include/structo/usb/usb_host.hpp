// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file usb_host.hpp
 * @brief USB host core: `usb_device` (an enumerated device with control-transfer helpers) and `usb_host`
 * (root-port enumeration: reset, address assignment, descriptor fetch, SET_CONFIGURATION). C++20 only.
 *
 * Root ports only (no hubs). Config descriptors are stored in caller-provided storage.
 *
 * @code
 * structo::hw::usb_host_controller_ref hcd{my_hcd};         // the controller driver
 * structo::usb::usb_host host{hcd};                         // address allocator + enumerator
 * reloco::array<std::uint8_t, 512> cfg_storage;             // holds the configuration descriptor
 * structo::usb::usb_device dev{hcd, cfg_storage};           // filled in by enumerate()
 * auto r = co_await host.enumerate(0, dev);                 // port 0; dev is now configured
 * // Hand `dev` to a class driver: usb::cdc_acm::attach(dev), usb::mass_storage::attach(dev) ...
 * @endcode
 */

#include "usb_defs.hpp"

#if RELOCO_HAS_COROUTINES

#include <reloco/coroutine.hpp>
#include <reloco/span.hpp>

namespace structo::usb {

/** @brief An enumerated, configured device. Created empty, populated by `usb_host::enumerate`. */
class usb_device {
public:
  /** @param cfg_storage buffer for the configuration descriptor (a larger descriptor fails with `capacity_exceeded`).
   */
  usb_device(hw::usb_host_controller_ref ctrl, reloco::span<std::uint8_t> cfg_storage) noexcept
      : ctrl_(ctrl), cfg_storage_(cfg_storage) {}

  /** @brief Replaces the configuration descriptor storage (call before `enumerate`). */
  void set_config_storage(reloco::span<std::uint8_t> storage) noexcept {
    cfg_storage_ = storage;
    cfg_len_ = 0;
  }

  [[nodiscard]] const hw::usb_host_controller_ref &controller() const noexcept { return ctrl_; }
  [[nodiscard]] std::uint8_t address() const noexcept { return address_; }
  [[nodiscard]] hw::usb_speed speed() const noexcept { return speed_; }
  [[nodiscard]] unsigned port() const noexcept { return port_; }
  [[nodiscard]] const device_descriptor &descriptor() const noexcept { return desc_; }
  [[nodiscard]] config_view config() const noexcept {
    return config_view{reloco::span<const std::uint8_t>(cfg_storage_.data(), cfg_len_)};
  }
  [[nodiscard]] bool configured() const noexcept { return configured_; }

  /** @brief Default control pipe (endpoint 0) of this device. */
  [[nodiscard]] hw::usb_pipe control_pipe() const noexcept {
    hw::usb_pipe p;
    p.address = address_;
    p.endpoint = 0;
    p.type = hw::usb_transfer_type::control;
    p.max_packet = max_packet0_;
    p.speed = speed_;
    return p;
  }

  /** @brief Pipe for an endpoint found in the descriptors. */
  [[nodiscard]] hw::usb_pipe pipe_for(const endpoint_descriptor &ep) const noexcept {
    hw::usb_pipe p;
    p.address = address_;
    p.endpoint = ep.number();
    p.direction = ep.direction();
    p.type = ep.type();
    p.max_packet = ep.max_packet;
    p.speed = speed_;
    p.interval = ep.interval;
    return p;
  }

  /** @brief Control transfer on EP0. Result is the number of data-stage bytes (`error::io_error` on STALL etc.). */
  [[nodiscard]] reloco::task<std::size_t> control(hw::usb_setup_packet setup,
                                                  reloco::span<std::uint8_t> data = {}) const noexcept {
    hw::usb_completion c = co_await ctrl_.control(control_pipe(), setup, data);
    auto r = c.to_result();
    if (!r)
      co_return reloco::unexpected(r.error());
    co_return *r;
  }

  /** @brief SET_INTERFACE (select an alternate setting) and reset the toggles of its endpoints. */
  [[nodiscard]] reloco::task<void> set_interface(const interface_info &itf) const noexcept {
    hw::usb_setup_packet s{request_type::standard_out_interface, request::set_interface, itf.alt, itf.number, 0};
    auto r = co_await control(s);
    if (!r)
      co_await reloco::unexpected(r.error());
    for (std::size_t i = 0; i < itf.endpoint_count; ++i)
      ctrl_.reset_data_toggle(pipe_for(itf.endpoints[i]));
  }

  /** @brief CLEAR_FEATURE(ENDPOINT_HALT) for a stalled endpoint and reset the host-side data toggle. */
  [[nodiscard]] reloco::task<void> clear_halt(const hw::usb_pipe &pipe) const noexcept {
    const std::uint8_t addr =
        static_cast<std::uint8_t>(pipe.endpoint | (pipe.direction == hw::usb_direction::in ? 0x80 : 0));
    hw::usb_setup_packet s{request_type::standard_out_endpoint, request::clear_feature, feature_endpoint_halt, addr, 0};
    auto r = co_await control(s);
    if (!r)
      co_await reloco::unexpected(r.error());
    ctrl_.reset_data_toggle(pipe);
  }

  /** @brief Reads string descriptor `index` (English) as ASCII into `out` (non-ASCII becomes '?'); returns length. */
  [[nodiscard]] reloco::task<std::size_t> get_string(std::uint8_t index, reloco::span<char> out) const noexcept {
    reloco::array<std::uint8_t, 128> buf{};
    hw::usb_setup_packet s{request_type::standard_in_device, request::get_descriptor,
                           static_cast<std::uint16_t>((descriptor_type::string << 8) | index), 0x0409,
                           static_cast<std::uint16_t>(buf.size())};
    auto rn = co_await control(s, reloco::span<std::uint8_t>(buf.data(), buf.size()));
    if (!rn)
      co_return reloco::unexpected(rn.error());
    std::size_t n = *rn;
    if (n < 2 || buf[1] != descriptor_type::string)
      co_return reloco::unexpected(reloco::error::invalid_argument);
    n = n < buf[0] ? n : buf[0];
    std::size_t len = 0;
    for (std::size_t i = 2; i + 1 < n && len < out.size(); i += 2)
      out[len++] = buf[i + 1] == 0 && buf[i] < 0x80 ? static_cast<char>(buf[i]) : '?';
    co_return len;
  }

private:
  friend class usb_host;

  hw::usb_host_controller_ref ctrl_;
  reloco::span<std::uint8_t> cfg_storage_;
  std::size_t cfg_len_ = 0;
  std::uint8_t address_ = 0;
  std::uint8_t max_packet0_ = 8;
  hw::usb_speed speed_ = hw::usb_speed::full;
  unsigned port_ = 0;
  device_descriptor desc_{};
  bool configured_ = false;
};

/** @brief Root-port enumerator and USB address allocator (addresses 1..127). */
class usb_host {
public:
  /** @brief Waits `ms` milliseconds; needed for the 2 ms SET_ADDRESS recovery if the controller does not do it. */
  using delay_fn = reloco::task<void> (*)(void *ctx, unsigned ms) noexcept;

  explicit usb_host(hw::usb_host_controller_ref ctrl, delay_fn delay = nullptr, void *delay_ctx = nullptr) noexcept
      : ctrl_(ctrl), delay_(delay), delay_ctx_(delay_ctx) {}

  [[nodiscard]] const hw::usb_host_controller_ref &controller() const noexcept { return ctrl_; }

  /**
   * @brief Resets `port`, assigns an address, reads descriptors and selects the first configuration into `dev`.
   * Errors: `invalid_argument` (bad port), `not_found` (nothing connected), `capacity_exceeded` (config does not
   * fit `dev`'s storage), `invalid_argument` (malformed descriptors), plus transfer errors.
   */
  [[nodiscard]] reloco::task<void> enumerate(unsigned port, usb_device &dev) noexcept {
    if (port >= ctrl_.port_count())
      co_await reloco::unexpected(reloco::error::invalid_argument);
    auto st = ctrl_.port_status(port);
    if (!st)
      co_await reloco::unexpected(st.error());
    if (!st->connected)
      co_await reloco::unexpected(reloco::error::not_found);

    auto rr = co_await ctrl_.reset_port(port);
    if (!rr)
      co_await reloco::unexpected(rr.error());
    st = ctrl_.port_status(port);
    if (!st)
      co_await reloco::unexpected(st.error());
    if (!st->connected || !st->enabled)
      co_await reloco::unexpected(reloco::error::io_error);

    dev.port_ = port;
    dev.speed_ = st->speed;
    dev.address_ = 0;
    dev.configured_ = false;
    dev.cfg_len_ = 0;
    dev.max_packet0_ = st->speed == hw::usb_speed::low
                           ? std::uint8_t{8}
                           : (st->speed == hw::usb_speed::full ? std::uint8_t{8} : std::uint8_t{64});

    // First 8 bytes of the device descriptor tell us the real EP0 packet size.
    reloco::array<std::uint8_t, 18> dd{};
    {
      auto rn = co_await dev.control(get_desc(descriptor_type::device, 0, 8), reloco::span<std::uint8_t>(dd.data(), 8));
      if (!rn)
        co_await reloco::unexpected(rn.error());
      std::size_t n = *rn;
      if (n < 8 || dd[1] != descriptor_type::device || dd[7] == 0)
        co_await reloco::unexpected(reloco::error::invalid_argument);
      dev.max_packet0_ = dd[7];
    }

    const std::uint8_t addr = alloc_address();
    if (addr == 0)
      co_await reloco::unexpected(reloco::error::capacity_exceeded);
    {
      hw::usb_setup_packet s{request_type::standard_out_device, request::set_address, addr, 0, 0};
      auto r = co_await dev.control(s);
      if (!r) {
        free_address(addr);
        co_await reloco::unexpected(r.error());
      }
    }
    dev.address_ = addr;
    if (delay_)
      (void)co_await delay_(delay_ctx_, 2);

    {
      auto rn = co_await dev.control(get_desc(descriptor_type::device, 0, 18),
                                     reloco::span<std::uint8_t>(dd.data(), dd.size()));
      if (!rn)
        co_await reloco::unexpected(rn.error());
      std::size_t n = *rn;
      auto parsed = parse_device_descriptor(reloco::span<const std::uint8_t>(dd.data(), n));
      if (!parsed)
        co_await reloco::unexpected(parsed.error());
      dev.desc_ = *parsed;
    }

    // Configuration header first (total length), then the whole thing.
    reloco::array<std::uint8_t, 9> hdr{};
    auto hdr_span = reloco::span(hdr.data(), hdr.size());
    {
      auto rn = co_await dev.control(get_desc(descriptor_type::configuration, 0, 9), hdr_span);
      if (!rn)
        co_await reloco::unexpected(rn.error());
      std::size_t n = *rn;
      if (n < 9 || hdr[1] != descriptor_type::configuration)
        co_await reloco::unexpected(reloco::error::invalid_argument);
    }
    const std::size_t total = le16(hdr_span.subspan(2));
    if (total < 9)
      co_await reloco::unexpected(reloco::error::invalid_argument);
    if (total > dev.cfg_storage_.size())
      co_await reloco::unexpected(reloco::error::capacity_exceeded);
    {
      auto rn = co_await dev.control(get_desc(descriptor_type::configuration, 0, static_cast<std::uint16_t>(total)),
                                     reloco::span<std::uint8_t>(dev.cfg_storage_.data(), total));
      if (!rn)
        co_await reloco::unexpected(rn.error());
      std::size_t n = *rn;
      if (n != total)
        co_await reloco::unexpected(reloco::error::invalid_argument);
      dev.cfg_len_ = n;
    }

    hw::usb_setup_packet sc{request_type::standard_out_device, request::set_configuration,
                            dev.config().configuration_value(), 0, 0};
    auto rc = co_await dev.control(sc);
    if (!rc)
      co_await reloco::unexpected(rc.error());
    dev.configured_ = true;
  }

  /** @brief Returns the device's address to the pool (call after disconnect). */
  void release(usb_device &dev) noexcept {
    free_address(dev.address_);
    dev.address_ = 0;
    dev.configured_ = false;
  }

private:
  static hw::usb_setup_packet get_desc(std::uint8_t type, std::uint8_t index, std::uint16_t len) noexcept {
    return {request_type::standard_in_device, request::get_descriptor, static_cast<std::uint16_t>((type << 8) | index),
            0, len};
  }

  std::uint8_t alloc_address() noexcept {
    for (unsigned a = 1; a < 128; ++a)
      if (!(used_[a / 32] & (1u << (a % 32)))) {
        used_[a / 32] |= 1u << (a % 32);
        return static_cast<std::uint8_t>(a);
      }
    return 0;
  }
  void free_address(std::uint8_t a) noexcept {
    if (a != 0 && a < 128)
      used_[a / 32] &= ~(1u << (a % 32));
  }

  hw::usb_host_controller_ref ctrl_;
  delay_fn delay_;
  void *delay_ctx_;
  reloco::array<std::uint32_t, 4> used_ = {1, 0, 0, 0}; // address 0 is reserved
};

} // namespace structo::usb

#endif // RELOCO_HAS_COROUTINES
