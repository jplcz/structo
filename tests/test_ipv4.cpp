// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/polled_net_device.hpp>
#include <structo/hw/slip_device.hpp>
#include <structo/net/ipv4_node.hpp>

#include <reloco/vec_deque.hpp>

#include "net_test_support.hpp"

using namespace structo;
using namespace structo::net;
using net_test::bytes;
using net_test::bytes_equal;
using net_test::make_bytes;
using net_test::push;

namespace {

class Ipv4 : public ::testing::Test {};
class Icmp : public ::testing::Test {};
class Ipv4Node : public ::testing::Test {};

constexpr ipv4_address me{10, 0, 0, 2};
constexpr ipv4_address peer{10, 0, 0, 1};

bytes make_ip(std::uint8_t proto, ipv4_address src, ipv4_address dst, const bytes &payload) {
  bytes out = make_bytes(ipv4_header_size + payload.size(), 0);
  ipv4_header h;
  h.protocol = proto;
  h.src = src;
  h.dst = dst;
  auto n = build_ipv4(h, payload, out);
  EXPECT_TRUE(n.has_value());
  return out;
}

TEST_F(Ipv4, ChecksumKnownVector) {
  // RFC 1071 example words 0001 f203 f4f5 f6f7 -> sum ddf2 -> complement 220d.
  const bytes v = make_bytes({0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7});
  EXPECT_EQ(internet_checksum(v), 0x220d);
  const bytes odd = make_bytes({0xff});
  EXPECT_EQ(internet_checksum(odd), 0x00ff); // odd length: padded with zero
}

TEST_F(Ipv4, BuildParseRoundTrip) {
  const bytes payload = make_bytes({1, 2, 3, 4, 5});
  auto pkt = make_ip(ip_proto_udp, peer, me, payload);
  auto p = parse_ipv4(pkt);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->header.protocol, ip_proto_udp);
  EXPECT_EQ(p->header.src, peer);
  EXPECT_EQ(p->header.dst, me);
  EXPECT_TRUE(bytes_equal(p->payload, payload));
}

TEST_F(Ipv4, ParseRejectsBadInput) {
  const bytes good = make_ip(ip_proto_udp, peer, me, make_bytes({1, 2, 3}));
  bytes bad = make_bytes(good);
  bad[10] ^= 1; // checksum
  EXPECT_EQ(parse_ipv4(bad).error(), reloco::error::invalid_argument);
  bad = make_bytes(good);
  bad[0] = 0x65; // version 6
  EXPECT_EQ(parse_ipv4(bad).error(), reloco::error::invalid_argument);
  bad = make_bytes(good);
  bad.pop_back(); // shorter than total length
  EXPECT_EQ(parse_ipv4(bad).error(), reloco::error::invalid_argument);
  const bytes tiny = make_bytes(10, 0);
  EXPECT_EQ(parse_ipv4(tiny).error(), reloco::error::invalid_argument);

  bad = make_bytes(good);
  bad[6] = 0x20; // MF
  bad[10] = bad[11] = 0;
  auto c = internet_checksum(span<const std::uint8_t>(bad.data(), ipv4_header_size));
  bad[10] = static_cast<std::uint8_t>(c >> 8);
  bad[11] = static_cast<std::uint8_t>(c);
  EXPECT_EQ(parse_ipv4(bad).error(), reloco::error::unsupported_operation);

  // Link-layer padding after the total length is ignored.
  bytes padded = make_bytes(good);
  push(padded, 0);
  auto pp = parse_ipv4(padded);
  ASSERT_TRUE(pp.has_value());
  EXPECT_EQ(pp->payload.size(), 3u);
}

TEST_F(Icmp, EchoRoundTrip) {
  reloco::array<std::uint8_t, 32> out{};
  const bytes data = make_bytes({9, 8, 7});
  auto n = build_icmp_echo(icmp_echo_request, 0x1234, 7, data, out);
  ASSERT_TRUE(n.has_value());
  auto e = parse_icmp_echo(span<const std::uint8_t>(out.data(), n.value()));
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->type, icmp_echo_request);
  EXPECT_EQ(e->id, 0x1234);
  EXPECT_EQ(e->seq, 7);
  EXPECT_TRUE(bytes_equal(e->data, data));
  out[3] ^= 1;
  EXPECT_EQ(parse_icmp_echo(span<const std::uint8_t>(out.data(), n.value())).error(), reloco::error::invalid_argument);
}

// ---- node over SLIP over a fake UART ------------------------------------

struct fake_uart {
  reloco::vec_deque<std::uint8_t> rx;
  bytes tx;
};

} // namespace

template <> struct structo::hw::uart_traits<fake_uart> {
  static reloco::result<void> configure(fake_uart &, const uart_config &) noexcept { return {}; }
  static reloco::result<bool> tx_ready(fake_uart &) noexcept { return true; }
  static reloco::result<bool> rx_ready(fake_uart &b) noexcept { return !b.rx.empty(); }
  static reloco::result<void> try_put_byte(fake_uart &b, std::uint8_t v) noexcept { return b.tx.try_push_back(v); }
  static reloco::result<std::uint8_t> try_get_byte(fake_uart &b) noexcept {
    auto v = b.rx[0];
    (void)b.rx.try_pop_front();
    return v;
  }
};

namespace {

struct stack {
  fake_uart uart;
  hw::slip_device<128> slip{hw::uart_ref{uart}};
  hw::polled_net_device<hw::slip_device<128>> pnd{slip};
  hw::net_device_ref nic{pnd};
  ipv4_node<128> ip{nic, me};

  void inject(const bytes &ip_packet) {
    reloco::array<std::uint8_t, 600> w{};
    auto n = hw::slip_encode(ip_packet, w);
    ASSERT_TRUE(n.has_value());
    for (std::size_t i = 0; i < n.value(); ++i)
      ASSERT_TRUE(uart.rx.try_push_back(w[i]).has_value());
  }

  // Decodes the next SLIP frame from what the node transmitted.
  bytes sent_packet() {
    reloco::array<std::uint8_t, 256> buf{};
    hw::slip_decoder d(buf);
    for (std::size_t i = 0; i < uart.tx.size(); ++i)
      if (d.push(uart.tx[i])) {
        auto f = d.frame();
        bytes frame = make_bytes(f);
        uart.tx.clear();
        return frame;
      }
    return bytes{};
  }
};

reloco::task<std::uint8_t> first_udp_byte(ipv4_node<128> &ip) {
  auto pkt = co_await co_await ip.receive();
  co_return pkt.payload[0];
}

TEST_F(Ipv4Node, AnswersPingAndDeliversUdp) {
  stack s;
  auto t = first_udp_byte(s.ip);
  t.resume();
  EXPECT_FALSE(t.done());

  // Echo request from the peer: answered internally, receive() keeps waiting.
  reloco::array<std::uint8_t, 32> icmp{};
  const bytes data = make_bytes({1, 2, 3});
  auto m = build_icmp_echo(icmp_echo_request, 5, 6, data, icmp);
  s.inject(make_ip(ip_proto_icmp, peer, me, make_bytes(reloco::span<const std::uint8_t>(icmp.data(), m.value()))));
  s.pnd.poll();
  EXPECT_FALSE(t.done());
  const bytes sent = s.sent_packet();
  auto reply = parse_ipv4(sent);
  ASSERT_TRUE(reply.has_value());
  EXPECT_EQ(reply->header.src, me);
  EXPECT_EQ(reply->header.dst, peer);
  auto echo = parse_icmp_echo(reply->payload);
  ASSERT_TRUE(echo.has_value());
  EXPECT_EQ(echo->type, icmp_echo_reply);
  EXPECT_EQ(echo->id, 5);
  EXPECT_EQ(echo->seq, 6);
  EXPECT_TRUE(bytes_equal(echo->data, data));

  // Packets for another host are dropped; one for us completes the task.
  s.inject(make_ip(ip_proto_udp, peer, ipv4_address{10, 0, 0, 99}, make_bytes({0x11})));
  s.inject(make_ip(ip_proto_udp, peer, me, make_bytes({0x42})));
  s.pnd.poll();
  ASSERT_TRUE(t.done());
  EXPECT_EQ(t.take().value(), 0x42);
}

TEST_F(Ipv4Node, SendBuildsDatagram) {
  stack s;
  const bytes small = make_bytes({7, 7});
  auto t = s.ip.send(ip_proto_udp, peer, small);
  t.resume();
  ASSERT_TRUE(t.done());
  EXPECT_TRUE(t.take().has_value());
  const bytes sent = s.sent_packet();
  auto p = parse_ipv4(sent);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->header.dst, peer);
  EXPECT_EQ(p->header.src, me);
  EXPECT_EQ(p->payload.size(), 2u);

  const bytes too_big = make_bytes(200, 0);
  auto t2 = s.ip.send(ip_proto_udp, peer, too_big);
  t2.resume();
  ASSERT_TRUE(t2.done());
  EXPECT_EQ(t2.take().error(), reloco::error::out_of_range);
}

} // namespace
