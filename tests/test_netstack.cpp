// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/bootldr/netstack.hpp>
#include <structo/hw/slip_device.hpp>

#include <reloco/vec_deque.hpp>

#include "net_test_support.hpp"

using namespace structo;
using namespace structo::net;
using net_test::append;
using net_test::bytes;
using net_test::make_bytes;
using net_test::push;

namespace {

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

constexpr std::size_t mtu = 600;
constexpr hw::net_mac_address mac{2, 0, 0, 0, 0, 9};
constexpr ipv4_address server{10, 0, 0, 1};
constexpr ipv4_address leased{10, 0, 0, 50};
constexpr ipv4_address host{192, 168, 7, 1};
constexpr ipv4_address board{192, 168, 7, 2};

std::uint64_t clock_now(void *ctx) noexcept { return *static_cast<std::uint64_t *>(ctx); }

class Netstack : public ::testing::Test {
protected:
  std::uint64_t now = 0;
  fake_uart uart;
  hw::slip_device<mtu> slip{hw::uart_ref{uart}};
  hw::polled_net_device<hw::slip_device<mtu>> pnd{slip};
  hw::net_device_ref nic{pnd};
  bootldr::scheduler sched;

  Netstack() { sched.set_clock(clock_now, &now); }

  // Advances the clock and runs a few scheduler rounds.
  void step(std::uint64_t ms = 100, int rounds = 3) {
    now += ms;
    for (int i = 0; i < rounds; ++i)
      sched.run_once();
  }

  void inject(const bytes &pkt) {
    reloco::array<std::uint8_t, 2 * mtu + 8> w{};
    auto n = hw::slip_encode(pkt, w);
    ASSERT_TRUE(n.has_value());
    for (std::size_t i = 0; i < n.value(); ++i)
      ASSERT_TRUE(uart.rx.try_push_back(w[i]).has_value());
  }

  // Returns the last IPv4 packet the stack put on the wire, then clears the TX capture.
  bytes take_tx() {
    reloco::array<std::uint8_t, mtu + 8> buf{};
    hw::slip_decoder dec(buf);
    bytes frame;
    for (std::size_t i = 0; i < uart.tx.size(); ++i)
      if (dec.push(uart.tx[i]))
        frame = make_bytes(dec.frame());
    uart.tx.clear();
    return frame;
  }
};

bytes ip_packet(std::uint8_t proto, ipv4_address src, ipv4_address dst, const bytes &payload) {
  bytes ip = make_bytes(ipv4_header_size + payload.size(), 0);
  ipv4_header h;
  h.protocol = proto;
  h.src = src;
  h.dst = dst;
  EXPECT_TRUE(build_ipv4(h, payload, ip).has_value());
  return ip;
}

bytes echo_request(ipv4_address src, ipv4_address dst) {
  bytes icmp = make_bytes(icmp_echo_header_size + 2, 0);
  const bytes data = make_bytes({0xAB, 0xCD});
  EXPECT_TRUE(build_icmp_echo(icmp_echo_request, 7, 1, data, icmp).has_value());
  return ip_packet(ip_proto_icmp, src, dst, icmp);
}

// A DHCP server reply (OFFER/ACK) carrying `leased`.
bytes server_reply(std::uint8_t type, std::uint32_t xid) {
  bytes b = make_bytes(dhcp_fixed_size, 0);
  b[0] = 2;
  b[1] = 1;
  b[2] = 6;
  for (std::size_t i = 0; i < 4; ++i)
    b[4 + i] = static_cast<std::uint8_t>(xid >> (24 - 8 * i));
  for (std::size_t i = 0; i < 4; ++i)
    b[16 + i] = leased.octets[i];
  for (std::size_t i = 0; i < 6; ++i)
    b[28 + i] = mac[i];
  auto opt = [&](std::uint8_t code, std::initializer_list<std::uint8_t> v) {
    push(b, code);
    push(b, static_cast<std::uint8_t>(v.size()));
    for (auto x : v)
      push(b, x);
  };
  const bytes cookie = make_bytes({0x63, 0x82, 0x53, 0x63});
  append(b, cookie);
  opt(53, {type});
  opt(54, {10, 0, 0, 1});
  opt(1, {255, 255, 255, 0});
  opt(3, {10, 0, 0, 1});
  opt(51, {0, 0, 0x0E, 0x10}); // 3600 s
  push(b, 255);
  return b;
}

bytes dhcp_packet(const bytes &bootp) {
  bytes udp = make_bytes(udp_header_size + bootp.size(), 0);
  const ipv4_address bcast{255, 255, 255, 255};
  EXPECT_TRUE(build_udp(dhcp_server_port, dhcp_client_port, bootp, server, bcast, udp).has_value());
  return ip_packet(ip_proto_udp, server, bcast, udp);
}

struct seen {
  int count = 0;
  std::uint8_t protocol = 0;
};

} // namespace

TEST_F(Netstack, StaticAddressIsReadyAndAnswersPing) {
  bootldr::netstack_config cfg;
  cfg.static_ip = ipv4_config::make_static(board, {255, 255, 255, 0}, host);
  bootldr::netstack<mtu> net{sched, nic, cfg};
  ASSERT_TRUE(net.poll_with(pnd).has_value());
  EXPECT_TRUE(net.ready());
  ASSERT_TRUE(net.start().has_value());
  EXPECT_TRUE(net.running());
  EXPECT_EQ(net.start().error(), reloco::error::invalid_state);
  EXPECT_EQ(sched.live(), 2u);

  step();
  inject(echo_request(host, board));
  step();
  const bytes reply = take_tx();
  auto pkt = parse_ipv4(reply);
  ASSERT_TRUE(pkt.has_value());
  EXPECT_EQ(pkt->header.src, board);
  EXPECT_EQ(pkt->header.dst, host);
  auto echo = parse_icmp_echo(pkt->payload);
  ASSERT_TRUE(echo.has_value());
  EXPECT_EQ(echo->type, icmp_echo_reply);
  EXPECT_EQ(echo->id, 7);
}

TEST_F(Netstack, DhcpLeasesAddress) {
  bootldr::netstack_config cfg;
  cfg.dhcp = true;
  cfg.mac = mac;
  cfg.xid_seed = 99;
  bootldr::netstack<mtu> net{sched, nic, cfg};
  ASSERT_TRUE(net.poll_with(pnd).has_value());
  ASSERT_TRUE(net.start().has_value());
  EXPECT_FALSE(net.ready());

  step();
  const bytes discover = take_tx();
  auto ip = parse_ipv4(discover);
  ASSERT_TRUE(ip.has_value());
  auto udp = parse_udp(ip->payload, ip->header.src, ip->header.dst);
  ASSERT_TRUE(udp.has_value());
  EXPECT_EQ(udp->dst_port, dhcp_server_port);
  const auto xid = (static_cast<std::uint32_t>(udp->payload[4]) << 24) |
                   (static_cast<std::uint32_t>(udp->payload[5]) << 16) |
                   (static_cast<std::uint32_t>(udp->payload[6]) << 8) | udp->payload[7];
  EXPECT_EQ(net.dhcp_state(), dhcp_state::selecting);

  inject(dhcp_packet(server_reply(dhcp_offer, xid)));
  step();
  EXPECT_EQ(net.dhcp_state(), dhcp_state::requesting);
  EXPECT_FALSE(take_tx().empty()); // REQUEST went out
  inject(dhcp_packet(server_reply(dhcp_ack, xid)));
  step();

  EXPECT_EQ(net.dhcp_state(), dhcp_state::bound);
  EXPECT_TRUE(net.ready());
  EXPECT_EQ(net.config().address, leased);
  EXPECT_EQ(net.config().gateway, server);
  EXPECT_EQ(net.stats().rx_handled, 2u);
}

TEST_F(Netstack, UnhandledDatagramsAreDroppedOrPassedToHandler) {
  bootldr::netstack_config cfg;
  cfg.static_ip = ipv4_config::make_static(board);
  bootldr::netstack<mtu> net{sched, nic, cfg};
  ASSERT_TRUE(net.poll_with(pnd).has_value());
  ASSERT_TRUE(net.start().has_value());
  step();

  const bytes payload = make_bytes({1, 2, 3});
  inject(ip_packet(ip_proto_udp, host, board, payload));
  step();
  EXPECT_EQ(net.stats().rx_dropped, 1u);

  seen s;
  net.set_packet_handler(
      [](void *ctx, const ipv4_packet &p) noexcept {
        auto *out = static_cast<seen *>(ctx);
        ++out->count;
        out->protocol = p.header.protocol;
      },
      &s);
  inject(ip_packet(ip_proto_udp, host, board, payload));
  step();
  EXPECT_EQ(s.count, 1);
  EXPECT_EQ(s.protocol, ip_proto_udp);
  EXPECT_EQ(net.stats().rx_dropped, 1u);
}

TEST_F(Netstack, StopCancelsTasksAndAllowsRestart) {
  bootldr::netstack<mtu> net{sched, nic};
  ASSERT_TRUE(net.poll_with(pnd).has_value());
  ASSERT_TRUE(net.start().has_value());
  step();
  net.stop();
  EXPECT_FALSE(net.running());
  EXPECT_EQ(sched.live(), 0u);
  ASSERT_TRUE(net.start().has_value());
  EXPECT_EQ(sched.live(), 2u);
}

TEST_F(Netstack, DestroyingStackWhileRunningIsSafe) {
  {
    bootldr::netstack<mtu> net{sched, nic};
    ASSERT_TRUE(net.start().has_value());
    step();
  }
  EXPECT_EQ(sched.live(), 0u);
}
