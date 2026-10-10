// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/bootldr/usb_stack.hpp>
#include <structo/hw/block_device_ref.hpp>
#include <structo/hw/net_device_ref.hpp>
#include <structo/hw/uart_ref.hpp>
#include <structo/usb/cdc_acm.hpp>
#include <structo/usb/cdc_ecm.hpp>
#include <structo/usb/mass_storage.hpp>
#include <structo/usb/usb_host.hpp>

#include <reloco/array.hpp>
#include <reloco/span.hpp>
#include <reloco/string_view.hpp>
#include <reloco/vec_deque.hpp>

#include "net_test_support.hpp"

using namespace structo;
using namespace structo::hw;
using net_test::append;
using net_test::bytes;
using net_test::make_bytes;
using net_test::push;

namespace {

// The data stage buffer of a transfer as a span.
reloco::span<std::uint8_t> xfer_buf(const usb_transfer &t) noexcept {
  return {static_cast<std::uint8_t *>(t.data), t.length};
}

struct reply {
  bool nak = false; // device has nothing yet: the controller keeps polling
  usb_status status = usb_status::ok;
  std::size_t actual = 0;
};

// A simulated USB device: standard control requests are handled here, classes override bulk/class requests.
struct sim_device {
  bytes dev_desc;
  bytes cfg_desc;
  reloco::array<bytes, 8> strings; // string descriptors by index (UTF-16, header included)
  std::uint8_t address = 0;
  std::uint8_t config = 0;
  unsigned set_interface_count = 0;
  std::uint16_t last_alt = 0;
  unsigned clear_halt_count = 0;
  std::uint8_t max_packet0 = 64;

  virtual ~sim_device() = default;

  void make_device_descriptor() {
    dev_desc = make_bytes({18, 1, 0x00, 0x02, 0, 0, 0, max_packet0, 0x34, 0x12, 0x78, 0x56, 0x00, 0x01, 1, 2, 3, 1});
  }

  void set_string(std::size_t index, const char *ascii) {
    bytes b;
    push(b, 0);
    push(b, 3);
    for (const char c : reloco::string_view{ascii}) {
      push(b, static_cast<std::uint8_t>(c));
      push(b, 0);
    }
    b[0] = static_cast<std::uint8_t>(b.size());
    strings[index] = std::move(b);
  }

  // Fixes up wTotalLength after the config body was assembled.
  void finish_config() {
    cfg_desc[2] = static_cast<std::uint8_t>(cfg_desc.size());
    cfg_desc[3] = static_cast<std::uint8_t>(cfg_desc.size() >> 8);
  }

  static void put_config_header(bytes &b, std::uint8_t interfaces) {
    const bytes h = make_bytes({9, 2, 0, 0, interfaces, 1, 0, 0x80, 50});
    append(b, net_test::as_span(h));
  }

  reply control(usb_transfer &t) {
    const usb_setup_packet &s = t.setup;
    const auto buf = xfer_buf(t);
    auto send = [&](const bytes &src) {
      const std::size_t n = src.size() < s.length ? src.size() : s.length;
      const std::size_t m = n < t.length ? n : t.length;
      for (std::size_t i = 0; i < m; ++i)
        buf[i] = src[i];
      return reply{false, usb_status::ok, m};
    };
    if (s.request_type == 0x80 && s.request == usb::request::get_descriptor) {
      const std::uint8_t type = static_cast<std::uint8_t>(s.value >> 8);
      const std::uint8_t idx = static_cast<std::uint8_t>(s.value);
      if (type == usb::descriptor_type::device)
        return send(dev_desc);
      if (type == usb::descriptor_type::configuration)
        return send(cfg_desc);
      if (type == usb::descriptor_type::string && idx < 8 && !strings[idx].empty())
        return send(strings[idx]);
      return {false, usb_status::stall, 0};
    }
    if (s.request_type == 0x00 && s.request == usb::request::set_address) {
      address = static_cast<std::uint8_t>(s.value);
      return {};
    }
    if (s.request_type == 0x00 && s.request == usb::request::set_configuration) {
      config = static_cast<std::uint8_t>(s.value);
      return {};
    }
    if (s.request_type == 0x01 && s.request == usb::request::set_interface) {
      ++set_interface_count;
      last_alt = s.value;
      return {};
    }
    if (s.request_type == 0x02 && s.request == usb::request::clear_feature) {
      ++clear_halt_count;
      return {};
    }
    return class_request(t);
  }

  virtual reply class_request(usb_transfer &) { return {false, usb_status::stall, 0}; }
  virtual reply bulk(usb_transfer &) { return {false, usb_status::stall, 0}; }
};

// Fake host controller: runs transfers against one simulated device. Completes inline, or on pump() when
// `defer` is set; NAKed transfers stay queued and are re-polled on pump().
struct fake_hcd {
  sim_device *dev = nullptr;
  bool defer = false;
  bool reset_done = false;
  bool changed = false; // port change flag, cleared by port_status()
  unsigned resets = 0;
  unsigned cancels = 0;
  unsigned toggle_resets = 0;
  static constexpr std::size_t slots = 8;
  reloco::array<usb_transfer *, slots> pending{};
  reloco::array<reply, slots> stored{};
  reloco::array<bool, slots> has_reply{};

  reply process(usb_transfer &t) {
    if (!dev) // unplugged: transfers hang until the stack cancels them
      return {true, usb_status::ok, 0};
    if (t.pipe.address != dev->address)
      return {false, usb_status::timeout, 0};
    return t.pipe.type == usb_transfer_type::control ? dev->control(t) : dev->bulk(t);
  }

  void pump() {
    for (std::size_t i = 0; i < slots; ++i) {
      usb_transfer *t = pending[i];
      if (!t)
        continue;
      if (!has_reply[i])
        stored[i] = process(*t);
      if (stored[i].nak) {
        has_reply[i] = false;
        continue;
      }
      pending[i] = nullptr;
      const reply r = stored[i];
      t->complete(r.status, r.actual);
    }
  }

  std::size_t pending_count() const {
    std::size_t n = 0;
    for (auto *p : pending)
      n += p != nullptr;
    return n;
  }
};

} // namespace

template <> struct structo::hw::usb_host_traits<fake_hcd> {
  static unsigned port_count(fake_hcd &) noexcept { return 1; }
  static reloco::result<usb_port_status> port_status(fake_hcd &h, unsigned) noexcept {
    usb_port_status s;
    s.connected = h.dev != nullptr;
    s.enabled = h.reset_done;
    s.speed = usb_speed::high;
    s.changed = h.changed;
    h.changed = false;
    return s;
  }
  static reloco::task<void> reset_port(fake_hcd &h, unsigned) noexcept {
    h.reset_done = true;
    ++h.resets;
    co_return;
  }
  static reloco::result<void> submit(fake_hcd &h, usb_transfer &t) noexcept {
    if (!h.dev)
      return reloco::unexpected(reloco::error::not_found);
    const reply r = h.process(t);
    if (!h.defer && !r.nak) {
      t.complete(r.status, r.actual);
      return {};
    }
    for (std::size_t i = 0; i < fake_hcd::slots; ++i)
      if (!h.pending[i]) {
        h.pending[i] = &t;
        h.stored[i] = r;
        h.has_reply[i] = !r.nak;
        return {};
      }
    return reloco::unexpected(reloco::error::busy);
  }
  static void cancel(fake_hcd &h, usb_transfer &t) noexcept {
    for (auto &p : h.pending)
      if (p == &t) {
        p = nullptr;
        ++h.cancels;
      }
  }
  static void reset_data_toggle(fake_hcd &h, const usb_pipe &) noexcept { ++h.toggle_resets; }
};

namespace {

// Drives a top-level task to completion, pumping the controller between resumes.
template <typename T> reloco::result<T> run(fake_hcd &h, reloco::task<T> t) {
  t.resume();
  for (int i = 0; i < 1000 && !t.done(); ++i)
    h.pump();
  EXPECT_TRUE(t.done());
  return t.take();
}

// ---------------------------------------------------------------------------------------------
// CDC-ACM device
// ---------------------------------------------------------------------------------------------

struct acm_device : sim_device {
  bytes received;                   // bytes the host sent on the bulk OUT endpoint
  reloco::vec_deque<bytes> to_host; // packets queued for the bulk IN endpoint
  bytes line_coding;
  std::uint16_t control_line_state = 0;

  acm_device() {
    make_device_descriptor();
    put_config_header(cfg_desc, 2);
    append(cfg_desc, net_test::as_span(make_bytes({9, 4, 0, 0, 1, 2, 2, 1, 0})));
    append(cfg_desc, net_test::as_span(make_bytes({5, 0x24, 0, 0x10, 0x01})));
    append(cfg_desc, net_test::as_span(make_bytes({7, 5, 0x83, 3, 8, 0, 16})));
    append(cfg_desc, net_test::as_span(make_bytes({9, 4, 1, 0, 2, 0x0A, 0, 0, 0})));
    append(cfg_desc, net_test::as_span(make_bytes({7, 5, 0x01, 2, 64, 0, 0})));
    append(cfg_desc, net_test::as_span(make_bytes({7, 5, 0x82, 2, 64, 0, 0})));
    finish_config();
  }

  reply class_request(usb_transfer &t) override {
    const auto buf = xfer_buf(t);
    if (t.setup.request == 0x20) {
      line_coding = make_bytes(buf);
      return {false, usb_status::ok, t.length};
    }
    if (t.setup.request == 0x22) {
      control_line_state = t.setup.value;
      return {};
    }
    return {false, usb_status::stall, 0};
  }

  reply bulk(usb_transfer &t) override {
    const auto buf = xfer_buf(t);
    if (t.pipe.endpoint == 1 && !t.is_in()) {
      append(received, buf);
      return {false, usb_status::ok, t.length};
    }
    if (t.pipe.endpoint == 2 && t.is_in()) {
      if (to_host.empty())
        return {true, usb_status::ok, 0};
      const bytes p = std::move(to_host[0]);
      (void)to_host.try_pop_front();
      const std::size_t n = p.size() < t.length ? p.size() : t.length;
      for (std::size_t i = 0; i < n; ++i)
        buf[i] = p[i];
      return {false, usb_status::ok, n};
    }
    return {false, usb_status::stall, 0};
  }
};

struct fixture {
  fake_hcd hcd;
  usb_host_controller_ref ref{hcd};
  usb::usb_host host{ref};
  reloco::array<std::uint8_t, 512> cfg{};
  usb::usb_device dev{ref, reloco::span<std::uint8_t>(cfg)};

  bool enumerate(sim_device &d, unsigned expect_set_cfg = 1) {
    hcd.dev = &d;
    auto r = run(hcd, host.enumerate(0, dev));
    (void)expect_set_cfg;
    return r.has_value();
  }
};

} // namespace

TEST(UsbHost, EnumerateAssignsAddressAndConfigures) {
  fixture f;
  acm_device d;
  ASSERT_TRUE(f.enumerate(d));
  EXPECT_EQ(d.address, 1);
  EXPECT_EQ(d.config, 1);
  EXPECT_TRUE(f.dev.configured());
  EXPECT_EQ(f.dev.address(), 1);
  EXPECT_EQ(f.dev.descriptor().vendor_id, 0x1234);
  EXPECT_EQ(f.dev.descriptor().product_id, 0x5678);
  EXPECT_EQ(f.dev.descriptor().max_packet0, 64);
  EXPECT_EQ(f.hcd.resets, 1u);
  EXPECT_EQ(f.dev.speed(), usb_speed::high);
  EXPECT_TRUE(f.dev.config().valid());
  EXPECT_EQ(f.dev.config().total_length(), d.cfg_desc.size());
}

TEST(UsbHost, EnumerateNothingConnected) {
  fixture f;
  auto r = run(f.hcd, f.host.enumerate(0, f.dev));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::not_found);
}

TEST(UsbHost, EnumerateBadPort) {
  fixture f;
  acm_device d;
  f.hcd.dev = &d;
  auto r = run(f.hcd, f.host.enumerate(5, f.dev));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::invalid_argument);
}

TEST(UsbHost, ConfigTooLargeForStorage) {
  fixture f;
  acm_device d;
  reloco::array<std::uint8_t, 16> tiny{};
  usb::usb_device small{f.ref, reloco::span<std::uint8_t>(tiny)};
  f.hcd.dev = &d;
  auto r = run(f.hcd, f.host.enumerate(0, small));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::capacity_exceeded);
}

TEST(UsbHost, ReleaseRecyclesAddress) {
  fixture f;
  acm_device d;
  ASSERT_TRUE(f.enumerate(d));
  EXPECT_EQ(f.dev.address(), 1);
  f.host.release(f.dev);
  d.address = 0;
  ASSERT_TRUE(f.enumerate(d));
  EXPECT_EQ(f.dev.address(), 1);
}

TEST(UsbHost, DeferredCompletion) {
  fixture f;
  acm_device d;
  f.hcd.defer = true;
  ASSERT_TRUE(f.enumerate(d));
  EXPECT_TRUE(f.dev.configured());
}

TEST(UsbHost, GetStringAscii) {
  fixture f;
  acm_device d;
  d.set_string(1, "Structo");
  ASSERT_TRUE(f.enumerate(d));
  reloco::array<char, 16> out{};
  auto r = run(f.hcd, f.dev.get_string(1, reloco::span<char>(out)));
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(*r, 7u);
  EXPECT_EQ(reloco::string_view(out.data()), "Structo");
}

TEST(UsbHost, TransferToGoneDeviceReportsDisconnected) {
  fixture f;
  acm_device d;
  ASSERT_TRUE(f.enumerate(d));
  f.hcd.dev = nullptr;
  auto r = run(f.hcd, f.dev.control({0x80, usb::request::get_status, 0, 0, 2}));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::operation_canceled);
}

TEST(UsbDefs, FindInterfaceAndEndpoints) {
  acm_device d;
  usb::config_view cv{net_test::as_span(d.cfg_desc)};
  ASSERT_TRUE(cv.valid());
  auto comm = cv.find_interface(usb::usb_class::cdc, 2);
  ASSERT_TRUE(comm.has_value());
  EXPECT_EQ(comm->number, 0);
  EXPECT_EQ(comm->endpoint_count, 1u);
  EXPECT_NE(comm->find_endpoint(usb_transfer_type::interrupt, usb_direction::in), nullptr);
  auto cs = cv.find_cs_descriptor(*comm, 0);
  ASSERT_TRUE(cs.has_value());
  EXPECT_EQ(cs->size(), 5u);
  auto data = cv.find_interface(usb::usb_class::cdc_data);
  ASSERT_TRUE(data.has_value());
  EXPECT_EQ(data->endpoint_count, 2u);
  const auto *in = data->find_endpoint(usb_transfer_type::bulk, usb_direction::in);
  ASSERT_NE(in, nullptr);
  EXPECT_EQ(in->number(), 2);
  EXPECT_EQ(in->max_packet, 64);
  EXPECT_EQ(cv.find_interface(usb::usb_class::mass_storage).error(), reloco::error::not_found);
  EXPECT_EQ(cv.find_interface(usb::usb_class::cdc, 2, usb::any, 1).error(), reloco::error::not_found);
}

TEST(UsbDefs, MalformedDescriptorChainRejected) {
  bytes b = make_bytes({9, 2, 12, 0, 1, 1, 0, 0x80, 50, 9, 4, 0, 0, 0, 8, 6, 0x50, 0});
  usb::config_view cv{net_test::as_span(b)};
  auto r = cv.find_interface(usb::usb_class::mass_storage);
  ASSERT_TRUE(r.has_value());
  bytes bad = make_bytes({9, 2, 12, 0, 1, 1, 0, 0x80, 50, 200, 4});
  usb::config_view bv{net_test::as_span(bad)};
  EXPECT_EQ(bv.find_interface(usb::usb_class::mass_storage).error(), reloco::error::invalid_argument);
}

// ---------------------------------------------------------------------------------------------
// CDC-ACM through uart_ref
// ---------------------------------------------------------------------------------------------

TEST(UsbCdcAcm, AttachSetsControlLinesAndLineCoding) {
  fixture f;
  acm_device d;
  ASSERT_TRUE(f.enumerate(d));
  usb::cdc_acm<256, 256> acm;
  auto r = run(f.hcd, acm.attach(f.dev));
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(acm.attached());
  EXPECT_EQ(d.control_line_state, 3);

  auto tx = acm.run_tx();
  tx.resume();
  ASSERT_EQ(d.line_coding.size(), 7u);
  EXPECT_EQ(d.line_coding[0], 0x00); // 115200 = 0x0001C200
  EXPECT_EQ(d.line_coding[1], 0xC2);
  EXPECT_EQ(d.line_coding[2], 0x01);
  EXPECT_EQ(d.line_coding[6], 8);

  hw::uart_config cfg;
  cfg.baud_rate = 9600;
  cfg.parity = hw::uart_parity::even;
  hw::uart_ref uart{acm};
  ASSERT_TRUE(uart.configure(cfg).has_value());
  EXPECT_EQ(d.line_coding[0], 0x80); // 9600 = 0x2580
  EXPECT_EQ(d.line_coding[1], 0x25);
  EXPECT_EQ(d.line_coding[5], 2); // even parity
  acm.detach();
  EXPECT_TRUE(tx.done());
}

TEST(UsbCdcAcm, TransmitThroughUart) {
  fixture f;
  acm_device d;
  ASSERT_TRUE(f.enumerate(d));
  usb::cdc_acm<256, 256> acm;
  ASSERT_TRUE(run(f.hcd, acm.attach(f.dev)).has_value());
  auto tx = acm.run_tx();
  tx.resume();
  hw::uart_ref uart{acm};
  ASSERT_TRUE(uart.write_string("hello").has_value());
  const bytes expect = make_bytes({'h', 'e', 'l', 'l', 'o'});
  EXPECT_TRUE(net_test::bytes_equal(d.received, expect));
  acm.detach();
  EXPECT_TRUE(tx.done());
}

TEST(UsbCdcAcm, ReceiveThroughUart) {
  fixture f;
  acm_device d;
  ASSERT_TRUE(f.enumerate(d));
  usb::cdc_acm<256, 256> acm;
  ASSERT_TRUE(run(f.hcd, acm.attach(f.dev)).has_value());
  auto rx = acm.run_rx();
  rx.resume();
  EXPECT_EQ(f.hcd.pending_count(), 1u); // IN transfer waiting for data
  hw::uart_ref uart{acm};
  EXPECT_FALSE(acm.rx_ready());

  ASSERT_TRUE(d.to_host.try_push_back(make_bytes({'o', 'k', '!'})).has_value());
  f.hcd.pump();
  ASSERT_TRUE(acm.rx_ready());
  auto a = uart.get_byte(1);
  auto b = uart.get_byte(1);
  auto c = uart.get_byte(1);
  ASSERT_TRUE(a && b && c);
  EXPECT_EQ(*a, 'o');
  EXPECT_EQ(*b, 'k');
  EXPECT_EQ(*c, '!');
  EXPECT_FALSE(acm.rx_ready());
  EXPECT_EQ(f.hcd.pending_count(), 1u); // re-armed

  // Destroying the pump cancels the pending transfer at the controller.
  rx = reloco::task<void>{};
  EXPECT_EQ(f.hcd.cancels, 1u);
  EXPECT_EQ(f.hcd.pending_count(), 0u);
}

TEST(UsbCdcAcm, ReceiveBackpressureWhenRingFull) {
  fixture f;
  acm_device d;
  ASSERT_TRUE(f.enumerate(d));
  usb::cdc_acm<64, 64> acm; // ring holds exactly one packet
  ASSERT_TRUE(run(f.hcd, acm.attach(f.dev)).has_value());
  auto rx = acm.run_rx();
  rx.resume();
  ASSERT_TRUE(d.to_host.try_push_back(make_bytes(64, 0x55)).has_value());
  f.hcd.pump();
  EXPECT_EQ(f.hcd.pending_count(), 0u); // ring full: no new IN is issued
  EXPECT_EQ(d.to_host.size(), 0u);
  std::uint8_t b = 0;
  hw::uart_ref uart{acm};
  auto g = uart.get_byte(1);
  ASSERT_TRUE(g.has_value());
  b = *g;
  EXPECT_EQ(b, 0x55);
  EXPECT_EQ(f.hcd.pending_count(), 0u); // still not a whole packet free
  for (int i = 0; i < 63; ++i)
    ASSERT_TRUE(uart.get_byte(1).has_value());
  EXPECT_EQ(f.hcd.pending_count(), 1u); // drained: reading resumed
  acm.detach();
}

TEST(UsbCdcAcm, AttachFailsWithoutAcmFunction) {
  fixture f;
  acm_device d;
  d.cfg_desc[10] = 0; // interface 0 no longer class 2
  d.cfg_desc[14] = 0;
  ASSERT_TRUE(f.enumerate(d));
  usb::cdc_acm<> acm;
  auto r = run(f.hcd, acm.attach(f.dev));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::not_found);
}

// ---------------------------------------------------------------------------------------------
// CDC-ECM
// ---------------------------------------------------------------------------------------------

struct ecm_device : sim_device {
  reloco::vector<bytes> sent;       // bulk OUT transfers
  reloco::vec_deque<bytes> to_host; // frames for bulk IN
  std::uint16_t packet_filter = 0;

  ecm_device() {
    make_device_descriptor();
    set_string(4, "02aabbccdd01");
    put_config_header(cfg_desc, 2);
    append(cfg_desc, net_test::as_span(make_bytes({9, 4, 0, 0, 1, 2, 6, 0, 0})));
    append(cfg_desc, net_test::as_span(make_bytes({13, 0x24, 0x0F, 4, 0, 0, 0, 0, 0xDC, 0x05, 0, 0, 0})));
    append(cfg_desc, net_test::as_span(make_bytes({7, 5, 0x81, 3, 16, 0, 16})));
    append(cfg_desc, net_test::as_span(make_bytes({9, 4, 1, 0, 0, 0x0A, 0, 0, 0})));
    append(cfg_desc, net_test::as_span(make_bytes({9, 4, 1, 1, 2, 0x0A, 0, 0, 0})));
    append(cfg_desc, net_test::as_span(make_bytes({7, 5, 0x02, 2, 64, 0, 0})));
    append(cfg_desc, net_test::as_span(make_bytes({7, 5, 0x83, 2, 64, 0, 0})));
    finish_config();
  }

  reply class_request(usb_transfer &t) override {
    if (t.setup.request == 0x43) {
      packet_filter = t.setup.value;
      return {};
    }
    return {false, usb_status::stall, 0};
  }

  reply bulk(usb_transfer &t) override {
    const auto buf = xfer_buf(t);
    if (t.pipe.endpoint == 2 && !t.is_in()) {
      (void)sent.try_push_back(make_bytes(buf));
      return {false, usb_status::ok, t.length};
    }
    if (t.pipe.endpoint == 3 && t.is_in()) {
      if (to_host.empty())
        return {true, usb_status::ok, 0};
      const bytes p = std::move(to_host[0]);
      (void)to_host.try_pop_front();
      const std::size_t n = p.size() < t.length ? p.size() : t.length;
      for (std::size_t i = 0; i < n; ++i)
        buf[i] = p[i];
      return {false, usb_status::ok, n};
    }
    return {false, usb_status::stall, 0};
  }
};

TEST(UsbCdcEcm, AttachReadsMacAndSelectsDataInterface) {
  fixture f;
  ecm_device d;
  ASSERT_TRUE(f.enumerate(d));
  usb::cdc_ecm ecm;
  ASSERT_TRUE(run(f.hcd, ecm.attach(f.dev)).has_value());
  const hw::net_mac_address expect{0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0x01};
  EXPECT_EQ(ecm.mac(), expect);
  EXPECT_EQ(ecm.mtu(), 1500u);
  EXPECT_EQ(d.set_interface_count, 1u);
  EXPECT_EQ(d.last_alt, 1);
  EXPECT_EQ(d.packet_filter, 0x0E);
  EXPECT_EQ(f.hcd.toggle_resets, 2u);
}

TEST(UsbCdcEcm, SendAddsZeroLengthPacketOnPacketBoundary) {
  fixture f;
  ecm_device d;
  ASSERT_TRUE(f.enumerate(d));
  usb::cdc_ecm ecm;
  ASSERT_TRUE(run(f.hcd, ecm.attach(f.dev)).has_value());

  const bytes small = make_bytes(100, 0xA5);
  ASSERT_TRUE(run(f.hcd, ecm.send(net_test::as_span(small))).has_value());
  ASSERT_EQ(d.sent.size(), 1u);
  EXPECT_EQ(d.sent[0].size(), 100u);

  const bytes exact = make_bytes(128, 0x5A);
  ASSERT_TRUE(run(f.hcd, ecm.send(net_test::as_span(exact))).has_value());
  ASSERT_EQ(d.sent.size(), 3u);
  EXPECT_EQ(d.sent[1].size(), 128u);
  EXPECT_EQ(d.sent[2].size(), 0u);

  const bytes huge = make_bytes(1600, 0);
  auto r = run(f.hcd, ecm.send(net_test::as_span(huge)));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::invalid_argument);
}

TEST(UsbCdcEcm, ReceiveFrameThroughNetDeviceRef) {
  fixture f;
  ecm_device d;
  ASSERT_TRUE(f.enumerate(d));
  usb::cdc_ecm ecm;
  ASSERT_TRUE(run(f.hcd, ecm.attach(f.dev)).has_value());

  hw::net_device_ref raw{ecm};
  auto mac = raw.mac_address();
  ASSERT_TRUE(mac.has_value());
  EXPECT_EQ((*mac)[5], 0x01);
  EXPECT_EQ(raw.mtu(), 1500u);
  auto up = raw.link_up();
  ASSERT_TRUE(up.has_value());
  EXPECT_TRUE(*up);

  ASSERT_TRUE(d.to_host.try_push_back(make_bytes(70, 0x11)).has_value());
  bytes buf = make_bytes(1514, 0);
  auto n = run(f.hcd, raw.receive(reloco::span<std::uint8_t>(buf.data(), buf.size())));
  ASSERT_TRUE(n.has_value());
  EXPECT_EQ(*n, 70u);
  EXPECT_EQ(buf[0], 0x11);
}

TEST(UsbCdcEcm, ReceiveWaitsForFrameAndCancels) {
  fixture f;
  ecm_device d;
  ASSERT_TRUE(f.enumerate(d));
  usb::cdc_ecm ecm;
  ASSERT_TRUE(run(f.hcd, ecm.attach(f.dev)).has_value());
  bytes buf = make_bytes(1514, 0);
  auto t = ecm.receive(reloco::span<std::uint8_t>(buf.data(), buf.size()));
  t.resume();
  EXPECT_FALSE(t.done());
  EXPECT_EQ(f.hcd.pending_count(), 1u);
  ASSERT_TRUE(d.to_host.try_push_back(make_bytes(60, 7)).has_value());
  f.hcd.pump();
  ASSERT_TRUE(t.done());
  auto n = t.take();
  ASSERT_TRUE(n.has_value());
  EXPECT_EQ(*n, 60u);
}

TEST(UsbCdcEcm, AttachFailsOnBadMacString) {
  fixture f;
  ecm_device d;
  d.set_string(4, "zz");
  ASSERT_TRUE(f.enumerate(d));
  usb::cdc_ecm ecm;
  auto r = run(f.hcd, ecm.attach(f.dev));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::invalid_argument);
}

// ---------------------------------------------------------------------------------------------
// Mass storage (Bulk-Only Transport, SCSI)
// ---------------------------------------------------------------------------------------------

struct msc_device : sim_device {
  static constexpr std::size_t block = 512;
  static constexpr std::size_t blocks = 64;
  bytes disk;
  unsigned tur_failures = 2;
  unsigned reads = 0, writes = 0, syncs = 0, resets = 0;
  unsigned fail_status = 0; // next command completes with this CSW status (1 = failed, 2 = phase error)

  enum class st { idle, data_in, data_out, status } state = st::idle;
  std::uint32_t tag = 0;
  std::uint8_t csw_status = 0;
  bytes in_data;
  std::uint64_t out_lba = 0;
  std::size_t out_len = 0;

  msc_device() {
    disk = make_bytes(block * blocks, 0);
    for (std::size_t i = 0; i < disk.size(); ++i)
      disk[i] = static_cast<std::uint8_t>(i * 7 + i / 512);
    make_device_descriptor();
    put_config_header(cfg_desc, 1);
    append(cfg_desc, net_test::as_span(make_bytes({9, 4, 0, 0, 2, 8, 6, 0x50, 0})));
    append(cfg_desc, net_test::as_span(make_bytes({7, 5, 0x81, 2, 64, 0, 0})));
    append(cfg_desc, net_test::as_span(make_bytes({7, 5, 0x02, 2, 64, 0, 0})));
    finish_config();
  }

  reply class_request(usb_transfer &t) override {
    if (t.setup.request == 0xFE)
      return {false, usb_status::stall, 0}; // single-LUN devices may STALL GET_MAX_LUN
    if (t.setup.request == 0xFF) {
      ++resets;
      state = st::idle;
      return {};
    }
    return {false, usb_status::stall, 0};
  }

  static std::uint32_t be32(reloco::span<const std::uint8_t> p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
  }
  static void put_be32(bytes &b, std::uint32_t v) {
    push(b, static_cast<std::uint8_t>(v >> 24));
    push(b, static_cast<std::uint8_t>(v >> 16));
    push(b, static_cast<std::uint8_t>(v >> 8));
    push(b, static_cast<std::uint8_t>(v));
  }

  void execute(reloco::span<const std::uint8_t> cdb, std::uint32_t data_len, bool dir_in) {
    csw_status = static_cast<std::uint8_t>(fail_status);
    fail_status = 0;
    in_data = bytes{};
    state = st::status;
    switch (cdb[0]) {
    case 0x00:
      if (tur_failures > 0) {
        --tur_failures;
        csw_status = 1;
      }
      break;
    case 0x03:
      in_data = make_bytes(18, 0);
      state = st::data_in;
      break;
    case 0x25:
      put_be32(in_data, static_cast<std::uint32_t>(blocks - 1));
      put_be32(in_data, static_cast<std::uint32_t>(block));
      state = st::data_in;
      break;
    case 0x28: {
      ++reads;
      const std::uint64_t lba = be32(cdb.subspan(2));
      const std::size_t n = static_cast<std::size_t>(cdb[7] << 8 | cdb[8]);
      in_data = make_bytes(n * block, 0);
      for (std::size_t i = 0; i < in_data.size(); ++i)
        in_data[i] = disk[lba * block + i];
      state = st::data_in;
      break;
    }
    case 0x2A:
      ++writes;
      out_lba = be32(cdb.subspan(2));
      out_len = static_cast<std::size_t>(cdb[7] << 8 | cdb[8]) * block;
      state = st::data_out;
      break;
    case 0x35:
      ++syncs;
      break;
    default:
      csw_status = 1;
      break;
    }
    (void)data_len;
    (void)dir_in;
  }

  reply bulk(usb_transfer &t) override {
    const auto buf = xfer_buf(t);
    if (t.pipe.endpoint == 2 && !t.is_in()) {
      if (state == st::idle && t.length == 31 && buf[0] == 'U' && buf[1] == 'S' && buf[2] == 'B' && buf[3] == 'C') {
        tag = static_cast<std::uint32_t>(buf[4] | buf[5] << 8 | buf[6] << 16 | buf[7] << 24);
        execute(buf.subspan(15), 0, (buf[12] & 0x80) != 0);
        return {false, usb_status::ok, t.length};
      }
      if (state == st::data_out && t.length == out_len) {
        for (std::size_t i = 0; i < t.length; ++i)
          disk[out_lba * block + i] = buf[i];
        state = st::status;
        return {false, usb_status::ok, t.length};
      }
      return {false, usb_status::stall, 0};
    }
    if (t.pipe.endpoint == 1 && t.is_in()) {
      if (state == st::data_in) {
        const std::size_t n = in_data.size() < t.length ? in_data.size() : t.length;
        for (std::size_t i = 0; i < n; ++i)
          buf[i] = in_data[i];
        state = st::status;
        return {false, usb_status::ok, n};
      }
      if (state == st::status) {
        const reloco::array<std::uint8_t, 13> csw{
            {'U', 'S', 'B', 'S', static_cast<std::uint8_t>(tag), static_cast<std::uint8_t>(tag >> 8),
             static_cast<std::uint8_t>(tag >> 16), static_cast<std::uint8_t>(tag >> 24), 0, 0, 0, 0, csw_status}};
        for (std::size_t i = 0; i < csw.size(); ++i)
          buf[i] = csw[i];
        state = st::idle;
        return {false, usb_status::ok, 13};
      }
    }
    return {false, usb_status::stall, 0};
  }
};

TEST(UsbMassStorage, AttachWaitsUntilReadyAndReadsCapacity) {
  fixture f;
  msc_device d;
  ASSERT_TRUE(f.enumerate(d));
  usb::mass_storage msc;
  ASSERT_TRUE(run(f.hcd, msc.attach(f.dev)).has_value());
  EXPECT_TRUE(msc.ready());
  EXPECT_EQ(msc.block_size(), 512u);
  EXPECT_EQ(msc.block_count(), 64u);
  EXPECT_EQ(d.tur_failures, 0u);
}

TEST(UsbMassStorage, AttachGivesUpWhenNeverReady) {
  fixture f;
  msc_device d;
  d.tur_failures = 100;
  ASSERT_TRUE(f.enumerate(d));
  usb::mass_storage msc;
  auto r = run(f.hcd, msc.attach(f.dev));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::io_error);
  EXPECT_FALSE(msc.ready());
}

TEST(UsbMassStorage, ReadWriteRoundTrip) {
  fixture f;
  msc_device d;
  ASSERT_TRUE(f.enumerate(d));
  usb::mass_storage msc;
  ASSERT_TRUE(run(f.hcd, msc.attach(f.dev)).has_value());

  bytes buf = make_bytes(2 * 512, 0);
  ASSERT_TRUE(run(f.hcd, msc.read_blocks(3, reloco::span<std::uint8_t>(buf.data(), buf.size()))).has_value());
  for (std::size_t i = 0; i < buf.size(); ++i)
    ASSERT_EQ(buf[i], d.disk[3 * 512 + i]);

  const bytes data = make_bytes(512, 0xEE);
  ASSERT_TRUE(run(f.hcd, msc.write_blocks(10, net_test::as_span(data))).has_value());
  for (std::size_t i = 0; i < 512; ++i)
    ASSERT_EQ(d.disk[10 * 512 + i], 0xEE);
  EXPECT_EQ(d.writes, 1u);

  ASSERT_TRUE(run(f.hcd, msc.flush()).has_value());
  EXPECT_EQ(d.syncs, 1u);
}

TEST(UsbMassStorage, LargeReadIsSplitIntoChunks) {
  fixture f;
  msc_device d;
  ASSERT_TRUE(f.enumerate(d));
  usb::mass_storage msc;
  ASSERT_TRUE(run(f.hcd, msc.attach(f.dev)).has_value());
  bytes buf = make_bytes(64 * 512, 0);
  ASSERT_TRUE(run(f.hcd, msc.read_blocks(0, reloco::span<std::uint8_t>(buf.data(), buf.size()))).has_value());
  EXPECT_EQ(d.reads, 2u); // 32 KiB with 16 KiB chunks
  EXPECT_TRUE(net_test::bytes_equal(buf, d.disk));
}

TEST(UsbMassStorage, RejectsBadRanges) {
  fixture f;
  msc_device d;
  ASSERT_TRUE(f.enumerate(d));
  usb::mass_storage msc;
  ASSERT_TRUE(run(f.hcd, msc.attach(f.dev)).has_value());
  bytes buf = make_bytes(512, 0);
  auto oor = run(f.hcd, msc.read_blocks(64, reloco::span<std::uint8_t>(buf.data(), buf.size())));
  ASSERT_FALSE(oor.has_value());
  EXPECT_EQ(oor.error(), reloco::error::out_of_range);
  bytes odd = make_bytes(100, 0);
  auto bad = run(f.hcd, msc.read_blocks(0, reloco::span<std::uint8_t>(odd.data(), odd.size())));
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error(), reloco::error::invalid_argument);
  EXPECT_EQ(d.reads, 0u);
}

TEST(UsbMassStorage, CommandFailedStatusIsIoError) {
  fixture f;
  msc_device d;
  ASSERT_TRUE(f.enumerate(d));
  usb::mass_storage msc;
  ASSERT_TRUE(run(f.hcd, msc.attach(f.dev)).has_value());
  const bytes data = make_bytes(512, 1);
  d.fail_status = 1;
  auto r = run(f.hcd, msc.write_blocks(0, net_test::as_span(data)));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::io_error);
  EXPECT_EQ(d.resets, 0u);
  // The next command still works.
  EXPECT_TRUE(run(f.hcd, msc.flush()).has_value());
}

TEST(UsbMassStorage, PhaseErrorTriggersResetRecovery) {
  fixture f;
  msc_device d;
  ASSERT_TRUE(f.enumerate(d));
  usb::mass_storage msc;
  ASSERT_TRUE(run(f.hcd, msc.attach(f.dev)).has_value());
  d.fail_status = 2;
  auto r = run(f.hcd, msc.flush());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::io_error);
  EXPECT_EQ(d.resets, 1u);
  EXPECT_EQ(d.clear_halt_count, 2u); // both bulk endpoints
  EXPECT_TRUE(run(f.hcd, msc.flush()).has_value());
}

TEST(UsbMassStorage, AttachFailsWithoutBotInterface) {
  fixture f;
  acm_device d;
  ASSERT_TRUE(f.enumerate(d));
  usb::mass_storage msc;
  auto r = run(f.hcd, msc.attach(f.dev));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::not_found);
}

struct pump_ctx {
  fake_hcd *hcd;
  unsigned calls = 0;
};
void pump_hcd(void *c) noexcept {
  auto *p = static_cast<pump_ctx *>(c);
  ++p->calls;
  p->hcd->pump();
}
void pump_nothing(void *c) noexcept { ++static_cast<pump_ctx *>(c)->calls; }

TEST(UsbMassStorage, BlockingAdapterThroughBlockDeviceRef) {
  fixture f;
  msc_device d;
  f.hcd.defer = true; // every transfer completes only when the "IRQ" pump runs
  ASSERT_TRUE(f.enumerate(d));
  usb::mass_storage msc;
  ASSERT_TRUE(run(f.hcd, msc.attach(f.dev)).has_value());

  pump_ctx pc{&f.hcd};
  usb::usb_msc_block_device disk{msc, &pump_hcd, &pc, 100};
  hw::block_device_ref blk{disk};
  EXPECT_EQ(blk.block_size(), 512u);
  EXPECT_EQ(blk.block_count(), 64u);
  EXPECT_TRUE(blk.is_available());
  EXPECT_FALSE(blk.is_read_only());

  bytes buf = make_bytes(512, 0);
  ASSERT_TRUE(blk.try_read_blocks(5, reloco::span<std::byte>(reinterpret_cast<std::byte *>(buf.data()), buf.size()))
                  .has_value());
  for (std::size_t i = 0; i < 512; ++i)
    ASSERT_EQ(buf[i], d.disk[5 * 512 + i]);

  bytes w = make_bytes(512, 0x42);
  ASSERT_TRUE(
      blk.try_write_blocks(7, reloco::span<const std::byte>(reinterpret_cast<const std::byte *>(w.data()), w.size()))
          .has_value());
  EXPECT_EQ(d.disk[7 * 512], 0x42);
  ASSERT_TRUE(blk.try_flush().has_value());
  EXPECT_EQ(d.syncs, 1u);
  EXPECT_GT(pc.calls, 0u);
}

TEST(UsbMassStorage, BlockingAdapterTimesOutAndCancels) {
  fixture f;
  msc_device d;
  ASSERT_TRUE(f.enumerate(d));
  usb::mass_storage msc;
  ASSERT_TRUE(run(f.hcd, msc.attach(f.dev)).has_value());
  f.hcd.defer = true;

  pump_ctx pc{&f.hcd};
  usb::usb_msc_block_device disk{msc, &pump_nothing, &pc, 10};
  hw::block_device_ref blk{disk};
  bytes buf = make_bytes(512, 0);
  auto r = blk.try_read_blocks(0, reloco::span<std::byte>(reinterpret_cast<std::byte *>(buf.data()), buf.size()));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::timed_out);
  EXPECT_EQ(pc.calls, 10u);
  EXPECT_EQ(f.hcd.cancels, 1u);
  EXPECT_EQ(f.hcd.pending_count(), 0u);
}

// ---------------------------------------------------------------------------------------------
// bootldr::usb_stack: hot-plug + per-device driver coroutines
// ---------------------------------------------------------------------------------------------

namespace {

struct stack_env {
  fake_hcd hcd;
  std::uint64_t now = 0;
  bootldr::scheduler sched;
  usb_host_controller_ref ref{hcd};
  bootldr::usb_stack stack;
  unsigned attached = 0, unclaimed = 0, detached = 0, failed = 0;
  reloco::error last_error = reloco::error::not_found;
  // driver bookkeeping
  unsigned started = 0, saw_disconnect = 0, cleaned = 0;
  bool gone_flag_at_cleanup = false;
  std::uint8_t last_address = 0;

  static bootldr::usb_stack_config config(std::size_t cfg_bytes) {
    bootldr::usb_stack_config c;
    c.poll_ms = 10;
    c.debounce_ms = 20;
    c.max_config_bytes = cfg_bytes;
    return c;
  }

  explicit stack_env(std::size_t cfg_bytes = 1024) : stack(sched, ref, config(cfg_bytes)) {
    sched.set_clock(&clock, this);
    EXPECT_TRUE(sched.add_poller(&service, this).has_value());
    stack.set_event_handler(&on_event, this);
  }

  static std::uint64_t clock(void *c) noexcept { return static_cast<stack_env *>(c)->now; }
  static void service(void *c) noexcept {
    auto *e = static_cast<stack_env *>(c);
    e->now += 10;
    e->hcd.pump();
  }
  static void on_event(void *c, bootldr::usb_event_kind k, unsigned, bootldr::usb_attached_device *,
                       const reloco::result<void> &st) noexcept {
    auto *e = static_cast<stack_env *>(c);
    switch (k) {
    case bootldr::usb_event_kind::attached:
      ++e->attached;
      break;
    case bootldr::usb_event_kind::unclaimed:
      ++e->unclaimed;
      break;
    case bootldr::usb_event_kind::detached:
      ++e->detached;
      break;
    case bootldr::usb_event_kind::enumeration_failed:
      ++e->failed;
      if (!st)
        e->last_error = st.error();
      break;
    }
  }

  void rounds(int n) {
    for (int i = 0; i < n; ++i)
      sched.run_once();
  }

  void plug(sim_device &d) {
    hcd.dev = &d;
    hcd.changed = true;
  }
  void unplug() {
    hcd.dev = nullptr;
    hcd.reset_done = false;
    hcd.changed = true;
  }
};

bool match_acm(void *, const usb::usb_device &d) noexcept {
  return d.config().find_interface(usb::usb_class::cdc, usb::cdc::subclass_acm).has_value();
}

// Driver coroutine: blocks on a bulk IN (NAKed by the fake device), so an unplug must fail it.
reloco::task<void> acm_driver(void *ctx, bootldr::usb_stack &stack, bootldr::usb_device_ptr dev) noexcept {
  auto *e = static_cast<stack_env *>(ctx);
  ++e->started;
  e->last_address = dev->device().address();
  const usb_pipe in{dev->device().address(), 2, usb_direction::in, usb_transfer_type::bulk, 64, usb_speed::high, 0};
  reloco::array<std::uint8_t, 64> buf{};
  usb_completion c = co_await dev->device().controller().in(in, reloco::span<std::uint8_t>(buf));
  if (c.status == usb_status::disconnected)
    ++e->saw_disconnect;
  // A new transfer on the gone device is rejected right away.
  usb_completion again = co_await dev->device().controller().in(in, reloco::span<std::uint8_t>(buf));
  if (again.status != usb_status::disconnected)
    --e->saw_disconnect;
  e->gone_flag_at_cleanup = dev->gone();
  co_await dev->gone_event().wait();
  ++e->cleaned;
  (void)stack;
}

} // namespace

TEST(UsbStack, PlugSpawnsDriverCoroutineAndUnplugFailsItsTransfers) {
  stack_env e;
  ASSERT_TRUE(e.stack.add_driver({&match_acm, &acm_driver, nullptr, &e}).has_value());
  ASSERT_TRUE(e.stack.start().has_value());
  acm_device d;

  e.rounds(5);
  EXPECT_EQ(e.started, 0u); // nothing plugged in

  e.plug(d);
  e.rounds(20);
  EXPECT_EQ(e.attached, 1u);
  EXPECT_EQ(e.started, 1u);
  EXPECT_EQ(e.last_address, 1);
  EXPECT_EQ(e.saw_disconnect, 0u);
  ASSERT_TRUE(static_cast<bool>(e.stack.attached(0)));
  EXPECT_EQ(e.hcd.pending_count(), 1u); // driver is parked on its IN transfer

  bootldr::usb_device_ptr keep = e.stack.attached(0); // a user reference outlives the unplug

  e.unplug();
  e.rounds(40);
  EXPECT_EQ(e.detached, 1u);
  EXPECT_FALSE(static_cast<bool>(e.stack.attached(0)));
  EXPECT_TRUE(keep->gone());
  EXPECT_EQ(e.saw_disconnect, 1u); // the pending transfer and the retry both failed
  EXPECT_TRUE(e.gone_flag_at_cleanup);
  EXPECT_EQ(e.cleaned, 1u); // coroutine saw gone_event and finished by itself
  EXPECT_EQ(e.hcd.pending_count(), 0u);
  EXPECT_EQ(keep->device().descriptor().vendor_id, 0x1234); // still readable

  // The address is reusable and the next device gets its own driver coroutine.
  e.hcd.dev = nullptr;
  acm_device d2;
  keep = bootldr::usb_device_ptr{};
  e.plug(d2);
  e.rounds(20);
  EXPECT_EQ(e.attached, 2u);
  EXPECT_EQ(e.started, 2u);
  EXPECT_EQ(e.last_address, 1);
}

TEST(UsbStack, UnclaimedDeviceStaysUntilUnplugged) {
  stack_env e;
  ASSERT_TRUE(e.stack.start().has_value()); // no drivers registered
  msc_device d;
  e.plug(d);
  e.rounds(20);
  EXPECT_EQ(e.unclaimed, 1u);
  EXPECT_EQ(e.attached, 0u);
  ASSERT_TRUE(static_cast<bool>(e.stack.attached(0)));
  bootldr::usb_device_ptr up = e.stack.attached(0);
  EXPECT_TRUE(up->device().configured());
  e.unplug();
  e.rounds(40);
  EXPECT_EQ(e.detached, 1u);
}

TEST(UsbStack, EnumerationFailureIsReportedOnceAndNotRetried) {
  stack_env e(16); // configuration buffer too small for the ACM descriptor
  ASSERT_TRUE(e.stack.start().has_value());
  acm_device d;
  e.plug(d);
  e.rounds(50);
  EXPECT_EQ(e.failed, 1u);
  EXPECT_EQ(e.last_error, reloco::error::capacity_exceeded);
  EXPECT_EQ(e.hcd.resets, 1u);
  EXPECT_FALSE(static_cast<bool>(e.stack.attached(0)));

  e.unplug();
  e.rounds(40);
  EXPECT_EQ(e.detached, 0u); // never attached
}

TEST(UsbStack, QuickReplugIsDetachThenAttach) {
  stack_env e;
  ASSERT_TRUE(e.stack.add_driver({&match_acm, &acm_driver, nullptr, &e}).has_value());
  ASSERT_TRUE(e.stack.start().has_value());
  acm_device a, b;
  e.plug(a);
  e.rounds(20);
  ASSERT_EQ(e.started, 1u);
  e.hcd.dev = &b; // swapped between two polls: still connected but `changed`
  e.hcd.reset_done = false;
  e.hcd.changed = true;
  e.rounds(30);
  EXPECT_EQ(e.detached, 1u);
  EXPECT_EQ(e.started, 2u);
}

TEST(UsbStack, DriverDetachCallbackRuns) {
  stack_env e;
  ASSERT_TRUE(
      e.stack
          .add_driver({&match_acm, &acm_driver,
                       [](void *c, bootldr::usb_attached_device &) noexcept { ++static_cast<stack_env *>(c)->cleaned; },
                       &e})
          .has_value());
  ASSERT_TRUE(e.stack.start().has_value());
  acm_device d;
  e.plug(d);
  e.rounds(20);
  ASSERT_EQ(e.saw_disconnect, 0u);
  e.unplug();
  e.rounds(40);
  EXPECT_EQ(e.saw_disconnect, 1u);
  EXPECT_EQ(e.cleaned, 2u); // once from the detach callback, once from the coroutine
}

TEST(UsbStack, StopDetachesEverything) {
  stack_env e;
  ASSERT_TRUE(e.stack.add_driver({&match_acm, &acm_driver, nullptr, &e}).has_value());
  ASSERT_TRUE(e.stack.start().has_value());
  acm_device d;
  e.plug(d);
  e.rounds(20);
  ASSERT_EQ(e.started, 1u);
  e.stack.stop();
  e.rounds(5);
  EXPECT_EQ(e.detached, 1u);
  EXPECT_EQ(e.saw_disconnect, 1u);
  EXPECT_EQ(e.cleaned, 1u);
}

TEST(UsbEvent, NotifyBeforeWaitIsRemembered) {
  usb::event e;
  e.notify();
  int stage = 0;
  auto t = [](usb::event &ev, int &s) -> reloco::task<void> {
    co_await ev.wait();
    s = 1;
    co_await ev.wait();
    s = 2;
  }(e, stage);
  t.resume();
  EXPECT_EQ(stage, 1); // first wait returned immediately
  EXPECT_FALSE(t.done());
  e.notify();
  EXPECT_EQ(stage, 2);
  EXPECT_TRUE(t.done());
}

TEST(UsbByteRing, WrapsAndReportsFullness) {
  usb::byte_ring<8> r;
  EXPECT_TRUE(r.empty());
  const reloco::array<std::uint8_t, 10> in{{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}};
  EXPECT_EQ(r.push(reloco::span<const std::uint8_t>(in)), 8u);
  EXPECT_EQ(r.free(), 0u);
  reloco::array<std::uint8_t, 4> out{};
  EXPECT_EQ(r.pop(reloco::span<std::uint8_t>(out)), 4u);
  EXPECT_EQ(out[0], 1);
  EXPECT_EQ(r.push(reloco::span<const std::uint8_t>(in).first(3)), 3u);
  reloco::array<std::uint8_t, 8> all{};
  EXPECT_EQ(r.pop(reloco::span<std::uint8_t>(all)), 7u);
  EXPECT_EQ(all[3], 8);
  EXPECT_EQ(all[4], 1);
}
