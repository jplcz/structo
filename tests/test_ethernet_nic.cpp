// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/ethernet_device.hpp>
#include <structo/hw/ethernet_nic.hpp>
#include <structo/hw/polled_net_device.hpp>

#include <reloco/vec_deque.hpp>

#include "net_test_support.hpp"

using namespace structo;
using namespace structo::hw;
using net_test::bytes;
using net_test::make_bytes;

namespace {

// Descriptor-ring style driver: RX ring of completed frames, TX ring with limited room.
struct fake_driver {
  reloco::vec_deque<bytes> rx;
  reloco::vector<bytes> tx;
  std::size_t tx_room = 8;
  bool up = true;
  bool released = false;

  net_mac_address read_mac() noexcept { return {2, 0, 0, 0, 0, 7}; }
  bool link_up() noexcept { return up; }
  reloco::result<void> tx_submit(reloco::span<const std::uint8_t> f) noexcept {
    if (tx_room == 0)
      return reloco::unexpected(reloco::error::try_again);
    --tx_room;
    return tx.try_push_back(make_bytes(f));
  }
  reloco::result<reloco::span<const std::uint8_t>> rx_peek() noexcept {
    if (rx.empty())
      return reloco::unexpected(reloco::error::try_again);
    return reloco::span<const std::uint8_t>(rx[0].data(), rx[0].size());
  }
  void rx_release() noexcept {
    (void)rx.try_pop_front();
    released = true;
  }
};

struct fcs_driver : fake_driver {
  static constexpr bool rx_includes_fcs = true;
  static constexpr std::size_t max_frame_size = 100;
  bool promisc = false;
  reloco::result<void> set_promiscuous(bool on) noexcept {
    promisc = on;
    return {};
  }
};

template <typename Nic> reloco::result<void> send(Nic &nic, std::size_t n, std::uint8_t fill) {
  const bytes b = make_bytes(n, fill);
  return nic.try_send(net_test::as_span(b));
}

class EthernetNic : public ::testing::Test {};

} // namespace

TEST_F(EthernetNic, ForwardsIdentityAndPadsShortFrames) {
  fake_driver d;
  ethernet_nic<fake_driver> nic{d};
  EXPECT_EQ(nic.mtu(), 1514u);
  EXPECT_TRUE(nic.link_up().value());
  EXPECT_EQ(nic.mac_address().value(), (net_mac_address{2, 0, 0, 0, 0, 7}));

  ASSERT_TRUE(send(nic, 20, 0x11).has_value());
  ASSERT_EQ(d.tx.size(), 1u);
  EXPECT_EQ(d.tx[0].size(), 60u);
  EXPECT_EQ(d.tx[0][19], 0x11);
  EXPECT_EQ(d.tx[0][20], 0);

  ASSERT_TRUE(send(nic, 100, 0x22).has_value());
  EXPECT_EQ(d.tx[1].size(), 100u);
  EXPECT_EQ(nic.stats().tx_frames, 2u);
  EXPECT_EQ(nic.stats().tx_bytes, 120u);
}

TEST_F(EthernetNic, SendValidatesAndReportsFullRing) {
  fake_driver d;
  ethernet_nic<fake_driver> nic{d};
  EXPECT_EQ(send(nic, 10, 0).error(), reloco::error::invalid_argument);
  EXPECT_EQ(send(nic, 1515, 0).error(), reloco::error::out_of_range);
  d.tx_room = 0;
  EXPECT_EQ(send(nic, 60, 0).error(), reloco::error::try_again);
  EXPECT_EQ(nic.stats().tx_ring_full, 1u);
  EXPECT_TRUE(d.tx.empty());
}

TEST_F(EthernetNic, ReceiveCopiesAndReleases) {
  fake_driver d;
  ethernet_nic<fake_driver> nic{d};
  reloco::array<std::uint8_t, 1514> buf{};
  EXPECT_EQ(nic.try_receive(buf).error(), reloco::error::try_again);

  ASSERT_TRUE(d.rx.try_push_back(make_bytes(64, 0x33)).has_value());
  auto n = nic.try_receive(buf);
  ASSERT_TRUE(n.has_value());
  EXPECT_EQ(*n, 64u);
  EXPECT_EQ(buf[63], 0x33);
  EXPECT_TRUE(d.rx.empty());
  EXPECT_EQ(nic.stats().rx_frames, 1u);
}

TEST_F(EthernetNic, DropsRuntAndOversizedFramesWithoutWedgingTheRing) {
  fake_driver d;
  ethernet_nic<fake_driver> nic{d};
  ASSERT_TRUE(d.rx.try_push_back(make_bytes(5, 1)).has_value());   // runt
  ASSERT_TRUE(d.rx.try_push_back(make_bytes(200, 2)).has_value()); // does not fit the caller's buffer
  ASSERT_TRUE(d.rx.try_push_back(make_bytes(80, 3)).has_value());  // good
  reloco::array<std::uint8_t, 128> buf{};
  auto n = nic.try_receive(buf);
  ASSERT_TRUE(n.has_value());
  EXPECT_EQ(*n, 80u);
  EXPECT_EQ(buf[0], 3);
  EXPECT_EQ(nic.stats().rx_runts, 1u);
  EXPECT_EQ(nic.stats().rx_oversized, 1u);
  EXPECT_TRUE(d.rx.empty());
}

TEST_F(EthernetNic, StripsFcsAndHonoursDriverLimits) {
  fcs_driver d;
  ethernet_nic<fcs_driver> nic{d};
  EXPECT_EQ(nic.mtu(), 100u);
  ASSERT_TRUE(d.rx.try_push_back(make_bytes(64, 9)).has_value());
  reloco::array<std::uint8_t, 128> buf{};
  EXPECT_EQ(nic.try_receive(buf).value(), 60u);

  ASSERT_TRUE(d.rx.try_push_back(make_bytes(110, 9)).has_value()); // 106 after FCS > 100
  EXPECT_EQ(nic.try_receive(buf).error(), reloco::error::try_again);
  EXPECT_EQ(nic.stats().rx_oversized, 1u);
  EXPECT_EQ(send(nic, 101, 0).error(), reloco::error::out_of_range);
}

TEST_F(EthernetNic, OptionalFilterControls) {
  fake_driver plain;
  ethernet_nic<fake_driver> a{plain};
  EXPECT_EQ(a.set_promiscuous(true).error(), reloco::error::unsupported_operation);
  EXPECT_EQ(a.add_multicast({1, 0, 0x5E, 0, 0, 1}).error(), reloco::error::unsupported_operation);

  fcs_driver d;
  ethernet_nic<fcs_driver> b{d};
  ASSERT_TRUE(b.set_promiscuous(true).has_value());
  EXPECT_TRUE(d.promisc);
  EXPECT_EQ(b.add_multicast({1, 0, 0x5E, 0, 0, 1}).error(), reloco::error::unsupported_operation);
}

namespace {
std::uint64_t clock_zero(void *) noexcept { return 0; }
} // namespace

TEST_F(EthernetNic, DrivesEthernetDeviceEndToEnd) {
  fake_driver d;
  ethernet_nic<fake_driver> nic{d};
  polled_net_device<ethernet_nic<fake_driver>> pnd{nic};
  net_device_ref raw{pnd};
  ethernet_device<1500, 4> eth{raw, &clock_zero, nullptr}; // MAC comes from the driver
  EXPECT_EQ(eth.mac(), (net_mac_address{2, 0, 0, 0, 0, 7}));
  eth.configure(net::ipv4_config::make_static({10, 0, 0, 2}, {255, 255, 255, 0}));

  auto svc = eth.service(); // sends the gratuitous ARP through the driver, padded to 60 bytes
  svc.resume();
  ASSERT_EQ(d.tx.size(), 1u);
  EXPECT_EQ(d.tx[0].size(), 60u);
  auto f = net::parse_ethernet(d.tx[0]);
  ASSERT_TRUE(f.has_value());
  EXPECT_EQ(f->ethertype, net::arp_ethertype);
}
