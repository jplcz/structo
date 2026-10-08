// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/polled_net_device.hpp>
#include <structo/hw/slip_device.hpp>
#include <structo/net/dhcp_client.hpp>

#include <deque>
#include <vector>

using namespace structo;
using namespace structo::net;

namespace {

using bytes = std::vector<std::uint8_t>;

constexpr hw::net_mac_address mac{2, 0, 0, 0, 0, 9};
constexpr ipv4_address server{10, 0, 0, 1};
constexpr ipv4_address leased{10, 0, 0, 50};

// Builds a server reply (OFFER/ACK/NAK) the way a DHCP server would.
bytes server_reply(std::uint8_t type, std::uint32_t xid, std::uint32_t lease_s, std::uint32_t t1_s = 0) {
  bytes b(dhcp_fixed_size, 0);
  b[0] = 2;
  b[1] = 1;
  b[2] = 6;
  for (int i = 0; i < 4; ++i)
    b[4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(xid >> (24 - 8 * i));
  if (type != dhcp_nak)
    for (std::size_t i = 0; i < 4; ++i)
      b[16 + i] = leased.octets[i];
  for (std::size_t i = 0; i < 6; ++i)
    b[28 + i] = mac[i];
  auto opt = [&](std::uint8_t code, std::initializer_list<std::uint8_t> v) {
    b.push_back(code);
    b.push_back(static_cast<std::uint8_t>(v.size()));
    b.insert(b.end(), v.begin(), v.end());
  };
  b.insert(b.end(), {0x63, 0x82, 0x53, 0x63});
  opt(53, {type});
  opt(54, {10, 0, 0, 1});
  opt(1, {255, 255, 255, 0});
  opt(3, {10, 0, 0, 1, 10, 0, 0, 2}); // two routers: first wins
  opt(6, {10, 0, 0, 53});
  opt(51, {static_cast<std::uint8_t>(lease_s >> 24), static_cast<std::uint8_t>(lease_s >> 16),
           static_cast<std::uint8_t>(lease_s >> 8), static_cast<std::uint8_t>(lease_s)});
  if (t1_s)
    opt(58, {0, 0, static_cast<std::uint8_t>(t1_s >> 8), static_cast<std::uint8_t>(t1_s)});
  b.push_back(255);
  return b;
}

// Wraps a BOOTP payload as an IPv4/UDP packet from the server to the client port.
bytes wrap(const bytes &bootp, ipv4_address dst = ipv4_address{255, 255, 255, 255}) {
  bytes udp(udp_header_size + bootp.size());
  auto u = build_udp(dhcp_server_port, dhcp_client_port, bootp, server, dst, udp);
  EXPECT_TRUE(u.has_value());
  bytes ip(ipv4_header_size + udp.size());
  ipv4_header h;
  h.protocol = ip_proto_udp;
  h.src = server;
  h.dst = dst;
  EXPECT_TRUE(build_ipv4(h, udp, ip).has_value());
  return ip;
}

std::uint32_t xid_of(span<const std::uint8_t> m) {
  return (static_cast<std::uint32_t>(m[4]) << 24) | (static_cast<std::uint32_t>(m[5]) << 16) |
         (static_cast<std::uint32_t>(m[6]) << 8) | m[7];
}

TEST(Udp, RoundTripAndChecksum) {
  const bytes payload{1, 2, 3};
  const ipv4_address a{10, 0, 0, 1}, b{10, 0, 0, 2};
  bytes out(udp_header_size + payload.size());
  ASSERT_TRUE(build_udp(1000, 2000, payload, a, b, out).has_value());
  auto d = parse_udp(out, a, b);
  ASSERT_TRUE(d.has_value());
  EXPECT_EQ(d->src_port, 1000);
  EXPECT_EQ(d->dst_port, 2000);
  EXPECT_EQ(d->payload.size(), 3u);
  EXPECT_EQ(parse_udp(out, b, a).has_value(), true); // pseudo-header sum is symmetric
  EXPECT_FALSE(parse_udp(out, a, ipv4_address{10, 0, 0, 3}).has_value());
  out[8] ^= 1;
  EXPECT_FALSE(parse_udp(out, a, b).has_value());
  out[6] = out[7] = 0; // checksum "not computed" is accepted
  EXPECT_TRUE(parse_udp(out, a, b).has_value());
}

TEST(Dhcp, BuildAndParseRoundTrip) {
  dhcp_request_fields f;
  f.type = dhcp_request;
  f.xid = 0xAABBCCDD;
  f.mac = mac;
  f.requested_address = leased;
  f.server_id = server;
  bytes out(dhcp_max_request_size);
  auto n = build_dhcp(f, out);
  ASSERT_TRUE(n.has_value());
  EXPECT_EQ(out[0], 1); // BOOTREQUEST
  EXPECT_EQ(xid_of(out), 0xAABBCCDDu);
  EXPECT_EQ(out[10], 0x80); // broadcast flag
  EXPECT_EQ(out[dhcp_fixed_size + 4], 53);
  EXPECT_EQ(out[dhcp_fixed_size + 6], dhcp_request);
  EXPECT_EQ(out[n.value() - 1], 255);

  const bytes offer = server_reply(dhcp_offer, 7, 600, 100);
  auto m = parse_dhcp(offer);
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->type, dhcp_offer);
  EXPECT_EQ(m->your_address, leased);
  EXPECT_EQ(m->server_id, server);
  EXPECT_EQ(m->netmask, (ipv4_address{255, 255, 255, 0}));
  EXPECT_EQ(m->gateway, server);
  EXPECT_EQ(m->dns, (ipv4_address{10, 0, 0, 53}));
  EXPECT_EQ(m->lease_seconds, 600u);
  EXPECT_EQ(m->renew_seconds, 100u);

  auto bad = server_reply(dhcp_offer, 7, 600);
  bad[dhcp_fixed_size] = 0;
  EXPECT_FALSE(parse_dhcp(bad).has_value());
  const bytes tiny(10, 0);
  EXPECT_FALSE(parse_dhcp(tiny).has_value());
}

// Drives the client through one pass: discover -> offer -> request -> ack.
struct handshake {
  dhcp_client c{mac, 1234};
  std::uint64_t now = 0;

  std::uint32_t discover() {
    auto m = c.poll(now);
    EXPECT_TRUE(m.has_value());
    EXPECT_EQ(m->operator[](dhcp_fixed_size + 6), dhcp_discover);
    return xid_of(*m);
  }
  bool feed(const bytes &ip) {
    auto pkt = parse_ipv4(ip);
    EXPECT_TRUE(pkt.has_value());
    return c.on_packet(*pkt, now);
  }
};

TEST(DhcpClient, FullLeaseCycleWithRenewAndExpiry) {
  handshake h;
  EXPECT_EQ(h.c.state(), dhcp_state::init);
  const auto xid = h.discover();
  EXPECT_EQ(h.c.state(), dhcp_state::selecting);
  EXPECT_EQ(h.c.poll(h.now + 1000).error(), reloco::error::try_again);

  // Retransmission with backoff: 4 s, then 8 s.
  h.now = 4000;
  ASSERT_TRUE(h.c.poll(h.now).has_value());
  EXPECT_EQ(h.c.poll(h.now + 7999).error(), reloco::error::try_again);

  // Reply for a foreign transaction is consumed but ignored.
  EXPECT_TRUE(h.feed(wrap(server_reply(dhcp_offer, xid + 1, 600))));
  EXPECT_EQ(h.c.state(), dhcp_state::selecting);

  EXPECT_TRUE(h.feed(wrap(server_reply(dhcp_offer, xid, 600))));
  EXPECT_EQ(h.c.state(), dhcp_state::requesting);
  auto req = h.c.poll(h.now);
  ASSERT_TRUE(req.has_value());
  EXPECT_EQ((*req)[dhcp_fixed_size + 6], dhcp_request);
  EXPECT_EQ(h.c.config(), nullptr);

  EXPECT_TRUE(h.feed(wrap(server_reply(dhcp_ack, xid, 600, 100))));
  EXPECT_EQ(h.c.state(), dhcp_state::bound);
  ASSERT_NE(h.c.config(), nullptr);
  EXPECT_EQ(h.c.config()->address, leased);
  EXPECT_EQ(h.c.config()->gateway, server);
  EXPECT_EQ(h.c.config()->lease_seconds, 600u);

  // T1 (100 s) -> renewing: REQUEST with ciaddr, no requested-ip/server-id options.
  EXPECT_EQ(h.c.poll(h.now + 99'999).error(), reloco::error::try_again);
  auto renew = h.c.poll(h.now + 100'000);
  ASSERT_TRUE(renew.has_value());
  EXPECT_EQ(h.c.state(), dhcp_state::renewing);
  EXPECT_EQ((*renew)[12], 10);
  EXPECT_EQ((*renew)[15], 50);
  const std::uint64_t t_renew = h.now + 100'000;

  h.now = t_renew;
  EXPECT_TRUE(h.feed(wrap(server_reply(dhcp_ack, xid, 600), leased)));
  EXPECT_EQ(h.c.state(), dhcp_state::bound);

  // Lease runs out while renewing -> config dropped, back to discovering.
  ASSERT_TRUE(h.c.poll(t_renew + 300'000).has_value()); // renew attempt
  auto lost = h.c.poll(t_renew + 600'000);
  ASSERT_TRUE(lost.has_value());
  EXPECT_EQ((*lost)[dhcp_fixed_size + 6], dhcp_discover);
  EXPECT_EQ(h.c.config(), nullptr);
  EXPECT_EQ(h.c.state(), dhcp_state::selecting);
}

TEST(DhcpClient, NakRestartsAndIgnoresOtherTraffic) {
  handshake h;
  const auto xid = h.discover();
  EXPECT_TRUE(h.feed(wrap(server_reply(dhcp_offer, xid, 600))));
  ASSERT_TRUE(h.c.poll(h.now).has_value());
  EXPECT_TRUE(h.feed(wrap(server_reply(dhcp_nak, xid, 0))));
  EXPECT_EQ(h.c.state(), dhcp_state::init);

  // UDP to another port is not ours.
  bytes udp(udp_header_size + 1);
  const bytes one{0};
  ASSERT_TRUE(build_udp(1, 2, one, server, leased, udp).has_value());
  bytes ip(ipv4_header_size + udp.size());
  ipv4_header hdr;
  hdr.protocol = ip_proto_udp;
  hdr.src = server;
  hdr.dst = leased;
  ASSERT_TRUE(build_ipv4(hdr, udp, ip).has_value());
  EXPECT_FALSE(h.feed(ip));
}

// ---- node integration over SLIP ------------------------------------------

struct fake_uart {
  std::deque<std::uint8_t> rx;
  std::vector<std::uint8_t> tx;
};

} // namespace

template <> struct structo::hw::uart_traits<fake_uart> {
  static reloco::result<void> configure(fake_uart &, const uart_config &) noexcept { return {}; }
  static reloco::result<bool> tx_ready(fake_uart &) noexcept { return true; }
  static reloco::result<bool> rx_ready(fake_uart &b) noexcept { return !b.rx.empty(); }
  static reloco::result<void> try_put_byte(fake_uart &b, std::uint8_t v) noexcept {
    b.tx.push_back(v);
    return {};
  }
  static reloco::result<std::uint8_t> try_get_byte(fake_uart &b) noexcept {
    auto v = b.rx.front();
    b.rx.pop_front();
    return v;
  }
};

namespace {

TEST(DhcpNode, LeasesAddressOverSlip) {
  fake_uart uart;
  hw::slip_device<600> slip{hw::uart_ref{uart}};
  hw::polled_net_device<hw::slip_device<600>> pnd{slip};
  hw::net_device_ref nic{pnd};
  ipv4_node<600> ip{nic}; // unconfigured
  dhcp_client dhcp{mac, 99};
  EXPECT_FALSE(ip.configured());

  // Application task: consume DHCP replies, hand everything else back.
  auto app = [](ipv4_node<600> &n, dhcp_client &d) -> reloco::task<void> {
    for (;;) {
      auto pkt = co_await co_await n.receive();
      (void)d.handle(n, pkt, 0);
    }
  };
  auto rx = app(ip, dhcp);
  rx.resume();

  auto tx = dhcp_send_due(ip, dhcp, 0);
  tx.resume();
  ASSERT_TRUE(tx.done());
  EXPECT_TRUE(tx.take().has_value());

  // Decode what went out on the wire: IPv4 0.0.0.0 -> 255.255.255.255, UDP 68 -> 67, DISCOVER.
  std::array<std::uint8_t, 700> buf{};
  hw::slip_decoder dec(buf);
  bytes frame;
  for (auto b : uart.tx)
    if (dec.push(b)) {
      auto f = dec.frame();
      frame.assign(f.begin(), f.end());
    }
  uart.tx.clear();
  auto ipp = parse_ipv4(frame);
  ASSERT_TRUE(ipp.has_value());
  EXPECT_TRUE(ipp->header.src.is_unspecified());
  EXPECT_TRUE(ipp->header.dst.is_broadcast());
  auto udp = parse_udp(ipp->payload, ipp->header.src, ipp->header.dst);
  ASSERT_TRUE(udp.has_value());
  EXPECT_EQ(udp->src_port, dhcp_client_port);
  EXPECT_EQ(udp->dst_port, dhcp_server_port);
  EXPECT_EQ(udp->payload[dhcp_fixed_size + 6], dhcp_discover);
  const auto xid = xid_of(udp->payload);

  auto inject = [&](const bytes &pkt) {
    std::array<std::uint8_t, 800> w{};
    auto n = hw::slip_encode(pkt, w);
    ASSERT_TRUE(n.has_value());
    uart.rx.insert(uart.rx.end(), w.begin(), w.begin() + static_cast<std::ptrdiff_t>(n.value()));
    pnd.poll();
  };
  inject(wrap(server_reply(dhcp_offer, xid, 3600)));
  auto tx2 = dhcp_send_due(ip, dhcp, 0); // sends the REQUEST
  tx2.resume();
  ASSERT_TRUE(tx2.done());
  uart.tx.clear();
  inject(wrap(server_reply(dhcp_ack, xid, 3600)));

  EXPECT_EQ(dhcp.state(), dhcp_state::bound);
  EXPECT_TRUE(ip.configured());
  EXPECT_EQ(ip.address(), leased);
  EXPECT_EQ(ip.config().netmask, (ipv4_address{255, 255, 255, 0}));
  EXPECT_EQ(ip.config().dns, (ipv4_address{10, 0, 0, 53}));
}

TEST(IpConfig, StaticConfigurationAndUnconfiguredNodeIgnoresPing) {
  fake_uart uart;
  hw::slip_device<128> slip{hw::uart_ref{uart}};
  hw::polled_net_device<hw::slip_device<128>> pnd{slip};
  hw::net_device_ref nic{pnd};
  ipv4_node<128> ip{nic};
  EXPECT_FALSE(ip.configured());
  ip.configure(ipv4_config::make_static({192, 168, 7, 2}, {255, 255, 255, 0}, {192, 168, 7, 1}));
  EXPECT_TRUE(ip.configured());
  EXPECT_EQ(ip.address(), (ipv4_address{192, 168, 7, 2}));
  EXPECT_EQ(ip.config().gateway, (ipv4_address{192, 168, 7, 1}));
  EXPECT_EQ(ip.config().lease_seconds, 0u);
  ip.configure({});
  EXPECT_FALSE(ip.configured());
}

} // namespace
