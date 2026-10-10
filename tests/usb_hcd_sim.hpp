// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

// Packet-level simulated USB device shared by the host controller driver tests (test_ohci_hcd.cpp,
// test_ehci_hcd.cpp, test_xhci_hcd.cpp). The controller *models* in those tests parse the driver's
// schedule structures (TDs/QHs/TRBs) and, for every packet the hardware would put on the wire, call
// setup()/in()/out() here -- exactly what a real device sees. No std:: containers (tests must not use them).
//
// Device layout: address assigned by SET_ADDRESS; one configuration with one interface and three endpoints:
//   0x01 bulk OUT  -> loopback FIFO       0x81 bulk IN <- the same FIFO (NAK while empty)
//   0x82 interrupt IN: returns `int_data` once after arm_interrupt(), NAK otherwise
//   ep 3 (0x03 / 0x83): always STALLs
// Control requests: GET_DESCRIPTOR (device/config/string 1), SET_ADDRESS, SET_CONFIGURATION, SET_INTERFACE,
// CLEAR_FEATURE (counted); class request 0x21/0x20 accepts an OUT data stage into `last_ctl_out`; anything
// else STALLs in the data/status stage (as real devices do), not at SETUP.
//
// Data toggles are verified: `toggle_errors` counts packets where the host used the wrong DATA0/DATA1.
// The controller model passes the toggle it received for OUT/SETUP packets and compares the toggle the
// device reports for IN packets with the one it expects, calling `note_toggle_error()` on a mismatch.

#pragma once

#include <reloco/array.hpp>
#include <reloco/span.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace usb_sim {

enum class reply : std::uint8_t { ack, nak, stall };

struct device {
  static constexpr std::size_t loop_cap = 8192;

  std::uint8_t address = 0;
  std::uint8_t config = 0;
  unsigned bulk_mps = 64; ///< wMaxPacketSize advertised for the bulk endpoints (64 FS, 512 HS).
  bool connected_hs = false;

  // Loopback FIFO.
  reloco::array<std::uint8_t, loop_cap> loop{};
  std::size_t loop_wr = 0;
  std::size_t loop_rd = 0;

  // Interrupt endpoint.
  reloco::array<std::uint8_t, 8> int_data{1, 2, 3, 4, 5, 6, 7, 8};
  std::size_t int_len = 8;
  bool int_armed = false;

  // Observations for the tests.
  unsigned toggle_errors = 0;
  unsigned set_config_count = 0;
  unsigned set_interface_count = 0;
  unsigned clear_halt_count = 0;
  unsigned bulk_out_packets = 0;
  unsigned bulk_in_packets = 0;
  reloco::array<std::uint8_t, 64> last_ctl_out{};
  std::size_t last_ctl_out_len = 0;

  // ---- control pipe state ----
  bool ctl_in = false;
  bool ctl_stall = false;
  bool ctl_addr_pending = false;
  std::uint8_t ctl_new_addr = 0;
  reloco::array<std::uint8_t, 256> ctl_data{};
  std::size_t ctl_len = 0; ///< bytes the device will send (IN requests) / expects (OUT requests).
  std::size_t ctl_pos = 0;
  bool ctl_class_out = false;
  bool ctl_exp_toggle = false;
  bool bulk_out_exp_toggle = false;
  bool bulk_in_toggle = false;
  bool int_toggle = false;

  void note_toggle_error() noexcept { ++toggle_errors; }

  [[nodiscard]] bool accepts(unsigned addr) const noexcept { return addr == address; }

  void reset_bus() noexcept {
    address = 0;
    config = 0;
    ctl_stall = false;
    ctl_addr_pending = false;
    ctl_exp_toggle = false;
    bulk_out_exp_toggle = false;
    bulk_in_toggle = false;
    int_toggle = false;
  }

  void arm_interrupt() noexcept { int_armed = true; }

  [[nodiscard]] std::size_t loop_pending() const noexcept { return loop_wr - loop_rd; }

  // ---- packets ----

  // SETUP token + 8-byte DATA0 packet.
  reply setup(reloco::span<const std::uint8_t> s, bool data0) noexcept {
    if (!data0)
      ++toggle_errors;
    const std::uint8_t type = s[0];
    const std::uint8_t req = s[1];
    const std::uint16_t value = static_cast<std::uint16_t>(s[2] | (s[3] << 8));
    const std::uint16_t length = static_cast<std::uint16_t>(s[6] | (s[7] << 8));
    ctl_in = (type & 0x80) != 0;
    ctl_stall = false;
    ctl_pos = 0;
    ctl_len = 0;
    ctl_class_out = false;
    ctl_exp_toggle = true; // the data (or status) stage starts with DATA1
    ctl_addr_pending = false;
    last_ctl_out_len = 0;

    if (type == 0x80 && req == 6) { // GET_DESCRIPTOR
      const std::uint8_t dt = static_cast<std::uint8_t>(value >> 8);
      const std::uint8_t idx = static_cast<std::uint8_t>(value);
      std::size_t n = 0;
      if (dt == 1) {
        n = device_descriptor();
      } else if (dt == 2) {
        n = config_descriptor();
      } else if (dt == 3 && idx == 1) {
        n = string_descriptor();
      } else {
        ctl_stall = true;
        return reply::ack;
      }
      ctl_len = n < length ? n : length;
    } else if (type == 0x00 && req == 5) { // SET_ADDRESS
      ctl_addr_pending = true;
      ctl_new_addr = static_cast<std::uint8_t>(value);
    } else if (type == 0x00 && req == 9) { // SET_CONFIGURATION
      config = static_cast<std::uint8_t>(value);
      ++set_config_count;
    } else if (type == 0x01 && req == 11) { // SET_INTERFACE
      ++set_interface_count;
    } else if (type == 0x02 && req == 1) { // CLEAR_FEATURE(ENDPOINT_HALT)
      ++clear_halt_count;
    } else if (type == 0x21 && req == 0x20) { // class request with an OUT data stage
      ctl_class_out = true;
      ctl_len = length < last_ctl_out.size() ? length : last_ctl_out.size();
    } else {
      ctl_stall = true;
    }
    return reply::ack;
  }

  // IN token on endpoint `ep` (0..15). Fills `buf` (its size is the max packet size) with the reply packet,
  // sets `n` to the packet length and `toggle` to the DATA toggle the device used.
  reply in(unsigned ep, reloco::span<std::uint8_t> buf, std::size_t &n, bool &toggle) noexcept {
    n = 0;
    const std::size_t mps = buf.size();
    if (ep == 0)
      return control_in(buf, n, toggle);
    if (ep == 1) { // bulk IN loopback
      if (loop_rd == loop_wr)
        return reply::nak;
      std::size_t m = loop_wr - loop_rd;
      if (m > mps)
        m = mps;
      copy(buf, loop.as_span().subspan(loop_rd, m));
      loop_rd += m;
      n = m;
      toggle = bulk_in_toggle;
      bulk_in_toggle = !bulk_in_toggle;
      ++bulk_in_packets;
      return reply::ack;
    }
    if (ep == 2) { // interrupt IN
      if (!int_armed)
        return reply::nak;
      int_armed = false;
      n = int_len < mps ? int_len : mps;
      copy(buf, int_data.as_span().subspan(0, n));
      toggle = int_toggle;
      int_toggle = !int_toggle;
      return reply::ack;
    }
    return reply::stall;
  }

  // OUT token + DATA packet of `n` bytes on endpoint `ep`; `toggle` = DATA0/1 the host sent.
  reply out(unsigned ep, reloco::span<const std::uint8_t> data, bool toggle) noexcept {
    if (ep == 0)
      return control_out(data, toggle);
    if (ep == 1) {
      if (toggle != bulk_out_exp_toggle) {
        ++toggle_errors;
        return reply::ack; // device ACKs a duplicate but drops it
      }
      if (data.size() > loop_cap - loop_wr)
        return reply::stall;
      copy(loop.as_span().subspan(loop_wr), data);
      loop_wr += data.size();
      bulk_out_exp_toggle = !bulk_out_exp_toggle;
      ++bulk_out_packets;
      return reply::ack;
    }
    return reply::stall;
  }

  /// Host reset the endpoint's data toggle (CLEAR_FEATURE / SET_INTERFACE): device side restarts at DATA0.
  void reset_toggles() noexcept {
    bulk_out_exp_toggle = false;
    bulk_in_toggle = false;
    int_toggle = false;
  }

private:
  reply control_in(reloco::span<std::uint8_t> buf, std::size_t &n, bool &toggle) noexcept {
    if (ctl_stall)
      return reply::stall;
    if (ctl_in) { // data stage
      std::size_t m = ctl_len - ctl_pos;
      if (m > buf.size())
        m = buf.size();
      copy(buf, ctl_data.as_span().subspan(ctl_pos, m));
      ctl_pos += m;
      n = m;
      toggle = ctl_exp_toggle;
      ctl_exp_toggle = !ctl_exp_toggle;
      return reply::ack;
    }
    // status stage of an OUT/no-data request: zero-length DATA1
    n = 0;
    toggle = true;
    if (ctl_addr_pending) {
      address = ctl_new_addr;
      ctl_addr_pending = false;
    }
    return reply::ack;
  }

  reply control_out(reloco::span<const std::uint8_t> data, bool toggle) noexcept {
    if (ctl_stall)
      return reply::stall;
    if (!ctl_in && ctl_class_out && ctl_pos < ctl_len) { // data stage
      if (toggle != ctl_exp_toggle)
        ++toggle_errors;
      ctl_exp_toggle = !ctl_exp_toggle;
      copy(last_ctl_out.as_span().subspan(last_ctl_out_len), data);
      last_ctl_out_len += data.size();
      ctl_pos += data.size();
      return reply::ack;
    }
    // status stage of an IN request: zero-length DATA1
    if (!toggle)
      ++toggle_errors;
    return reply::ack;
  }

  // Copies `src` to the start of `dst` (dst must be at least as large).
  static void copy(reloco::span<std::uint8_t> dst, reloco::span<const std::uint8_t> src) noexcept {
    for (std::size_t i = 0; i < src.size(); ++i)
      dst[i] = src[i];
  }

  // Appends `src` to ctl_data at `pos`; returns the new end.
  std::size_t put(std::size_t pos, reloco::span<const std::uint8_t> src) noexcept {
    copy(ctl_data.as_span().subspan(pos), src);
    return pos + src.size();
  }

  std::size_t device_descriptor() noexcept {
    const reloco::array<std::uint8_t, 18> d{18,   1,    0x00, 0x02, 0,    0, 0, 64, 0x34,
                                            0x12, 0x78, 0x56, 0x00, 0x01, 0, 1, 0,  1};
    return put(0, d.as_span());
  }

  std::size_t config_descriptor() noexcept {
    const std::uint8_t lo = static_cast<std::uint8_t>(bulk_mps);
    const std::uint8_t hi = static_cast<std::uint8_t>(bulk_mps >> 8);
    const reloco::array<std::uint8_t, 9> cfg{9, 2, 39, 0, 1, 1, 0, 0x80, 50};
    const reloco::array<std::uint8_t, 9> ifc{9, 4, 0, 0, 3, 0xFF, 0, 0, 0};
    const reloco::array<std::uint8_t, 7> e1{7, 5, 0x81, 2, lo, hi, 0};
    const reloco::array<std::uint8_t, 7> e2{7, 5, 0x01, 2, lo, hi, 0};
    const reloco::array<std::uint8_t, 7> e3{7, 5, 0x82, 3, 8, 0, 4};
    std::size_t pos = put(0, cfg.as_span());
    pos = put(pos, ifc.as_span());
    pos = put(pos, e1.as_span());
    pos = put(pos, e2.as_span());
    return put(pos, e3.as_span());
  }

  std::size_t string_descriptor() noexcept {
    const reloco::array<std::uint8_t, 10> s{10, 3, 'H', 0, 'C', 0, 'D', 0, '1', 0};
    return put(0, s.as_span());
  }
};

} // namespace usb_sim
