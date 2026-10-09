// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/bootldr/netstack.hpp>
#include <structo/hw/ethernet_device.hpp>
#include <structo/hw/polled_net_device.hpp>

#include <reloco/vec_deque.hpp>

#include "net_test_support.hpp"

using namespace structo;
using namespace structo::net;
using net_test::bytes;
using net_test::make_bytes;

namespace {

struct fake_nic {
  reloco::vec_deque<bytes> rx;
  reloco::vector<bytes> tx;
};

struct fake_nic_backend {
  fake_nic &n;
  std::size_t mtu() const noexcept { return 1514; }
  reloco::result<bool> link_up() noexcept { return true; }
  reloco::result<hw::net_mac_address> mac_address() noexcept { return hw::net_mac_address{2, 0, 0, 0, 0, 1}; }
  reloco::result<void> try_send(reloco::span<const std::uint8_t> f) noexcept {
    if (!n.tx.try_push_back(make_bytes(f)))
      return reloco::unexpected(reloco::error::allocation_failed);
    return {};
  }
  reloco::result<std::size_t> try_receive(reloco::span<std::uint8_t> dst) noexcept {
    if (n.rx.empty())
      return reloco::unexpected(reloco::error::try_again);
    const auto len = n.rx[0].size();
    for (std::size_t i = 0; i < len; ++i)
      dst[i] = n.rx[0][i];
    (void)n.rx.try_pop_front();
    return len;
  }
};

constexpr hw::net_mac_address my_mac{2, 0, 0, 0, 0, 1};
constexpr hw::net_mac_address peer_mac{2, 0, 0, 0, 0, 2};
constexpr hw::net_mac_address gw_mac{2, 0, 0, 0, 0, 3};
constexpr ipv4_address me{10, 0, 0, 2};
constexpr ipv4_address peer{10, 0, 0, 7};
constexpr ipv4_address gw{10, 0, 0, 1};
constexpr ipv4_address far_host{8, 8, 8, 8};
constexpr ipv4_address mask{255, 255, 255, 0};

std::uint64_t clock_now(void *ctx) noexcept { return *static_cast<std::uint64_t *>(ctx); }

class EthernetDevice : public ::testing::Test {
protected:
  EthernetDevice() {
    eth.configure(ipv4_config::make_static(me, mask, gw));
    auto t = eth.service(); // flushes the gratuitous ARP announcement
    t.resume();
    nic.tx.clear();
  }

  void push_frame(const hw::net_mac_address &dst, const hw::net_mac_address &src, std::uint16_t type,
                  net_test::byte_span payload) {
    bytes f = make_bytes(ethernet_header_size + payload.size(), 0);
    ASSERT_TRUE(build_ethernet(dst, src, type, payload, f).has_value());
    ASSERT_TRUE(nic.rx.try_push_back(std::move(f)).has_value());
  }

  void push_arp_reply(const ipv4_address &ip, const hw::net_mac_address &mac) {
    bytes p = make_bytes(arp_packet_size, 0);
    ASSERT_TRUE(build_arp({arp_reply, mac, ip, my_mac, me}, p).has_value());
    push_frame(my_mac, mac, arp_ethertype, p);
  }

  bytes datagram(const ipv4_address &dst, std::size_t payload_len = 4) {
    bytes d = make_bytes(20 + payload_len, 0);
    ipv4_header h;
    h.protocol = ip_proto_udp;
    h.src = me;
    h.dst = dst;
    bytes payload = make_bytes(payload_len, 0x5A);
    EXPECT_TRUE(build_ipv4(h, payload, d).has_value());
    return d;
  }

  void run_send(const bytes &d) {
    auto t = eth.send(net_test::as_span(d));
    t.resume();
    ASSERT_TRUE(t.done());
    ASSERT_TRUE(t.take().has_value());
  }

  void run_service() {
    auto t = eth.service();
    t.resume();
    ASSERT_TRUE(t.done());
  }

  std::uint64_t now = 0;
  fake_nic nic;
  fake_nic_backend be{nic};
  hw::polled_net_device<fake_nic_backend> pnd{be};
  hw::net_device_ref raw{pnd};
  hw::ethernet_device<1500, 4> eth{raw, &clock_now, &now};
};

} // namespace

TEST(EthernetCodec, RoundTripAndHelpers) {
  const bytes payload = make_bytes({1, 2, 3});
  bytes f = make_bytes(ethernet_header_size + 3, 0);
  auto n = build_ethernet(peer_mac, my_mac, ipv4_ethertype, payload, f);
  ASSERT_TRUE(n.has_value());
  EXPECT_EQ(*n, f.size());
  auto p = parse_ethernet(f);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->dst, peer_mac);
  EXPECT_EQ(p->src, my_mac);
  EXPECT_EQ(p->ethertype, ipv4_ethertype);
  EXPECT_TRUE(net_test::bytes_equal(p->payload, payload));

  bytes small = make_bytes(15, 0);
  EXPECT_EQ(build_ethernet(peer_mac, my_mac, ipv4_ethertype, payload, small).error(), reloco::error::out_of_range);
  bytes shortb = make_bytes(13, 0);
  EXPECT_EQ(parse_ethernet(shortb).error(), reloco::error::invalid_argument);

  EXPECT_TRUE(is_group_mac(ethernet_broadcast_mac));
  EXPECT_FALSE(is_group_mac(my_mac));
  const hw::net_mac_address mc{0x01, 0x00, 0x5E, 0x01, 0x02, 0x03};
  EXPECT_EQ(ipv4_multicast_mac({225, 129, 2, 3}), mc);
}

TEST_F(EthernetDevice, ReportsMtuAndMac) {
  hw::net_device_ref ip_side{eth};
  EXPECT_EQ(ip_side.mtu(), 1500u);
  EXPECT_EQ(ip_side.mac_address().value(), my_mac);
  EXPECT_TRUE(ip_side.link_up().value());
}

TEST_F(EthernetDevice, BroadcastGoesOutImmediately) {
  run_send(datagram({255, 255, 255, 255}));
  ASSERT_EQ(nic.tx.size(), 1u);
  auto f = parse_ethernet(nic.tx[0]);
  ASSERT_TRUE(f.has_value());
  EXPECT_EQ(f->dst, ethernet_broadcast_mac);
  EXPECT_EQ(f->ethertype, ipv4_ethertype);
}

TEST_F(EthernetDevice, UnicastParksUntilArpReplyThenFlushes) {
  const bytes d = datagram(peer);
  run_send(d);

  ASSERT_EQ(nic.tx.size(), 1u); // only the ARP request so far
  auto req = parse_ethernet(nic.tx[0]);
  ASSERT_TRUE(req.has_value());
  EXPECT_EQ(req->ethertype, arp_ethertype);
  EXPECT_EQ(req->dst, ethernet_broadcast_mac);
  auto a = parse_arp(req->payload);
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->target_ip, peer);
  EXPECT_TRUE(eth.has_pending());

  push_arp_reply(peer, peer_mac);
  reloco::array<std::uint8_t, 1500> buf{};
  auto rx = eth.receive(buf); // consumes the reply, flushes the parked datagram, then waits for IPv4
  rx.resume();
  EXPECT_FALSE(rx.done());

  ASSERT_EQ(nic.tx.size(), 2u);
  auto out = parse_ethernet(nic.tx[1]);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->dst, peer_mac);
  EXPECT_EQ(out->ethertype, ipv4_ethertype);
  EXPECT_TRUE(net_test::bytes_equal(out->payload, d));
  EXPECT_FALSE(eth.has_pending());
}

TEST_F(EthernetDevice, OffLinkDestinationUsesGatewayMac) {
  ASSERT_TRUE(eth.arp().add_static(gw, gw_mac).has_value());
  run_send(datagram(far_host));
  ASSERT_EQ(nic.tx.size(), 1u);
  auto f = parse_ethernet(nic.tx[0]);
  ASSERT_TRUE(f.has_value());
  EXPECT_EQ(f->dst, gw_mac);
}

TEST_F(EthernetDevice, DirectedBroadcastAndMulticastMacs) {
  run_send(datagram({10, 0, 0, 255}));
  run_send(datagram({224, 0, 0, 251}));
  ASSERT_EQ(nic.tx.size(), 2u);
  auto f0 = parse_ethernet(nic.tx[0]);
  auto f1 = parse_ethernet(nic.tx[1]);
  ASSERT_TRUE(f0.has_value() && f1.has_value());
  EXPECT_EQ(f0->dst, ethernet_broadcast_mac);
  EXPECT_EQ(f1->dst, (hw::net_mac_address{0x01, 0x00, 0x5E, 0, 0, 0xFB}));
}

TEST_F(EthernetDevice, ParkedDatagramExpires) {
  run_send(datagram(peer));
  ASSERT_TRUE(eth.has_pending());
  now = 3500;
  run_service();
  EXPECT_FALSE(eth.has_pending());
}

TEST_F(EthernetDevice, ServiceRetriesArpRequest) {
  run_send(datagram(peer));
  ASSERT_EQ(nic.tx.size(), 1u);
  now = 1100;
  run_service();
  EXPECT_EQ(nic.tx.size(), 2u); // retry after 1 s
}

TEST_F(EthernetDevice, AnswersArpRequestForUs) {
  bytes p = make_bytes(arp_packet_size, 0);
  ASSERT_TRUE(build_arp({arp_request, peer_mac, peer, {}, me}, p).has_value());
  push_frame(ethernet_broadcast_mac, peer_mac, arp_ethertype, p);

  reloco::array<std::uint8_t, 1500> buf{};
  auto rx = eth.receive(buf);
  rx.resume();
  EXPECT_FALSE(rx.done());
  ASSERT_GE(nic.tx.size(), 1u);
  auto last = parse_ethernet(nic.tx[nic.tx.size() - 1]);
  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(last->dst, peer_mac);
  auto a = parse_arp(last->payload);
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->op, arp_reply);
  EXPECT_EQ(a->sender_mac, my_mac);
}

TEST_F(EthernetDevice, ReceiveFiltersAndTrimsPadding) {
  const bytes d = datagram(me, 4);
  bytes padded = make_bytes(d);
  for (int i = 0; i < 20; ++i)
    net_test::push(padded, 0);

  push_frame(gw_mac, peer_mac, ipv4_ethertype, d);      // someone else's unicast: ignored
  push_frame(my_mac, peer_mac, 0x86DD, d);              // not IPv4/ARP: ignored
  push_frame(my_mac, peer_mac, ipv4_ethertype, padded); // ours, with padding

  reloco::array<std::uint8_t, 1500> buf{};
  auto rx = eth.receive(buf);
  rx.resume();
  ASSERT_TRUE(rx.done());
  auto n = rx.take();
  ASSERT_TRUE(n.has_value());
  EXPECT_EQ(*n, d.size());
  EXPECT_TRUE(net_test::bytes_equal(reloco::span<const std::uint8_t>(buf.data(), *n), d));
}

TEST_F(EthernetDevice, RejectsBadDatagramsAndUnconfiguredUnicast) {
  bytes junk = make_bytes(8, 0);
  auto t = eth.send(junk);
  t.resume();
  ASSERT_TRUE(t.done());
  EXPECT_EQ(t.take().error(), reloco::error::invalid_argument);

  eth.configure({});
  const bytes d = datagram(peer);
  auto u = eth.send(net_test::as_span(d));
  u.resume();
  ASSERT_TRUE(u.done());
  EXPECT_EQ(u.take().error(), reloco::error::invalid_state);
}

TEST_F(EthernetDevice, NetstackPushesConfigAndAnnouncesOverEthernet) {
  hw::ethernet_device<1500, 4> dev{raw, &clock_now, &now};
  hw::net_device_ref ip_side{dev};
  bootldr::scheduler sched;
  sched.set_clock(clock_now, &now);
  bootldr::netstack_config cfg;
  cfg.static_ip = ipv4_config::make_static(me, mask, gw);
  bootldr::netstack<1500> stack{sched, ip_side, cfg};
  ASSERT_TRUE(stack.poll_with(pnd).has_value());
  stack.use_ethernet(dev);
  ASSERT_TRUE(stack.start().has_value());

  now += 100;
  for (int i = 0; i < 3; ++i)
    sched.run_once();
  EXPECT_EQ(dev.config().address, me);
  EXPECT_EQ(dev.config().gateway, gw);
  ASSERT_GE(nic.tx.size(), 1u); // gratuitous ARP announcement
  auto f = parse_ethernet(nic.tx[nic.tx.size() - 1]);
  ASSERT_TRUE(f.has_value());
  EXPECT_EQ(f->ethertype, arp_ethertype);
}
