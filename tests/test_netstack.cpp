// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/bootldr/netstack.hpp>
#include <structo/bootldr/tftp.hpp>
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

bytes udp_packet(ipv4_address src, std::uint16_t sport, ipv4_address dst, std::uint16_t dport, const bytes &payload) {
  bytes udp = make_bytes(udp_header_size + payload.size(), 0);
  EXPECT_TRUE(build_udp(sport, dport, payload, src, dst, udp).has_value());
  return ip_packet(ip_proto_udp, src, dst, udp);
}

// Receives one datagram on `s` and sends it straight back to its sender.
reloco::task<void> echo_once(bootldr::udp_socket &s) {
  reloco::array<std::uint8_t, 64> buf{};
  auto rx = co_await co_await s.receive_from(buf);
  co_await co_await s.send_to(rx.source, rx.source_port, {buf.data(), rx.size});
}

reloco::task<void> send_unready(bootldr::udp_socket &s, reloco::error &out) {
  const bytes payload = make_bytes({1});
  auto r = co_await s.send_to(host, 1, payload);
  if (!r)
    out = r.error();
}

} // namespace

TEST_F(Netstack, UdpSocketReceivesAndRepliesUsingHeap) {
  bootldr::netstack_config cfg;
  cfg.static_ip = ipv4_config::make_static(board);
  bootldr::netstack<mtu> net{sched, nic, cfg};
  ASSERT_TRUE(net.poll_with(pnd).has_value());
  ASSERT_TRUE(net.start().has_value());

  bootldr::udp_socket sock{net};
  auto port = sock.bind(5000);
  ASSERT_TRUE(port.has_value());
  EXPECT_EQ(*port, 5000);
  ASSERT_TRUE(sched.spawn(echo_once(sock)).has_value());
  step();

  inject(udp_packet(host, 4000, board, 5000, make_bytes({9, 8, 7})));
  step();
  EXPECT_EQ(net.stats().rx_dropped, 0u);

  const bytes tx = take_tx();
  auto ip = parse_ipv4(tx);
  ASSERT_TRUE(ip.has_value());
  EXPECT_EQ(ip->header.dst, host);
  auto udp = parse_udp(ip->payload, ip->header.src, ip->header.dst);
  ASSERT_TRUE(udp.has_value());
  EXPECT_EQ(udp->src_port, 5000);
  EXPECT_EQ(udp->dst_port, 4000);
  const bytes expect = make_bytes({9, 8, 7});
  EXPECT_TRUE(net_test::bytes_equal(udp->payload, expect));
}

TEST_F(Netstack, UdpSocketQueueLimitTruncationAndBind) {
  bootldr::netstack_config cfg;
  cfg.static_ip = ipv4_config::make_static(board);
  bootldr::netstack<mtu> net{sched, nic, cfg};
  ASSERT_TRUE(net.poll_with(pnd).has_value());
  ASSERT_TRUE(net.start().has_value());

  bootldr::udp_socket a{net, 2};
  bootldr::udp_socket b{net};
  ASSERT_TRUE(a.bind(6000).has_value());
  EXPECT_EQ(a.bind(6001).error(), reloco::error::invalid_state);
  EXPECT_EQ(b.bind(6000).error(), reloco::error::busy);
  auto eph = b.bind();
  ASSERT_TRUE(eph.has_value());
  EXPECT_GE(*eph, 49152);

  reloco::array<std::uint8_t, 2> small{};
  EXPECT_EQ(a.try_receive_from(small).error(), reloco::error::try_again);
  step();
  for (std::uint8_t i = 0; i < 3; ++i) {
    inject(udp_packet(host, 1, board, 6000, make_bytes({i, 1, 2, 3})));
    step();
  }
  EXPECT_EQ(a.pending(), 2u);
  EXPECT_EQ(a.dropped(), 1u);

  auto r = a.try_receive_from(small);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->size, 2u);
  EXPECT_TRUE(r->truncated);
  EXPECT_EQ(small[0], 0);
  ASSERT_TRUE(a.try_receive_from(small).has_value());
  EXPECT_EQ(a.try_receive_from(small).error(), reloco::error::try_again);

  // Nobody listens on this port: falls through to the dropped counter.
  inject(udp_packet(host, 1, board, 7000, make_bytes({1})));
  step();
  EXPECT_EQ(net.stats().rx_dropped, 1u);

  a.close();
  EXPECT_FALSE(a.is_bound());
  EXPECT_TRUE(b.is_bound());
  EXPECT_TRUE(a.bind(6000).has_value()); // port is free again
}

TEST_F(Netstack, UdpSendFailsWithoutAddress) {
  bootldr::netstack<mtu> net{sched, nic};
  bootldr::udp_socket sock{net};
  reloco::error err{};
  ASSERT_TRUE(sched.spawn(send_unready(sock, err)).has_value());
  sched.run();
  EXPECT_EQ(err, reloco::error::invalid_state);
}

TEST_F(Netstack, SocketOutlivingStackIsDetached) {
  alignas(bootldr::udp_socket) unsigned char storage[sizeof(bootldr::udp_socket)];
  bootldr::udp_socket *sock = nullptr;
  {
    bootldr::netstack<mtu> net{sched, nic};
    sock = new (storage) bootldr::udp_socket{net};
    ASSERT_TRUE(sock->bind(100).has_value());
  }
  EXPECT_EQ(sock->bind(101).error(), reloco::error::invalid_state);
  sock->~udp_socket();
}

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

namespace {

bytes tftp_pkt(std::uint16_t op, std::uint16_t arg, const bytes &data = bytes()) {
  bytes b;
  push(b, static_cast<std::uint8_t>(op >> 8));
  push(b, static_cast<std::uint8_t>(op));
  push(b, static_cast<std::uint8_t>(arg >> 8));
  push(b, static_cast<std::uint8_t>(arg));
  append(b, net_test::as_span(data));
  return b;
}

struct tftp_out {
  bool done = false;
  std::size_t size = 0;
  reloco::error err = reloco::error::invalid_state;
  bool ok = false;
};

reloco::task<void> run_get(bootldr::tftp_client &c, ipv4_address srv, reloco::span<std::uint8_t> dst, tftp_out &out) {
  auto r = co_await c.get(srv, "boot.bin", dst);
  out.done = true;
  if (r) {
    out.ok = true;
    out.size = *r;
  } else {
    out.err = r.error();
  }
}

reloco::task<void> run_put(bootldr::tftp_client &c, ipv4_address srv, reloco::span<const std::uint8_t> src,
                           tftp_out &out) {
  auto r = co_await c.put(srv, "up.bin", src);
  out.done = true;
  if (r) {
    out.ok = true;
    out.size = *r;
  } else {
    out.err = r.error();
  }
}

// The UDP payload and ports of an IPv4 frame the stack transmitted.
struct sent_udp {
  bytes frame;
  std::uint16_t src_port = 0;
  std::uint16_t dst_port = 0;
  bytes payload;
};

sent_udp as_udp(bytes frame) {
  sent_udp o;
  o.frame = std::move(frame);
  auto ip = parse_ipv4(o.frame);
  EXPECT_TRUE(ip.has_value());
  if (!ip)
    return o;
  auto u = parse_udp(ip->payload, ip->header.src, ip->header.dst);
  EXPECT_TRUE(u.has_value());
  if (!u)
    return o;
  o.src_port = u->src_port;
  o.dst_port = u->dst_port;
  o.payload = make_bytes(u->payload);
  return o;
}

} // namespace

TEST_F(Netstack, TftpDownloadsIntoMemory) {
  bootldr::netstack_config cfg;
  cfg.static_ip = ipv4_config::make_static(board);
  bootldr::netstack<mtu> net{sched, nic, cfg};
  ASSERT_TRUE(net.poll_with(pnd).has_value());
  ASSERT_TRUE(net.start().has_value());
  bootldr::tftp_client tftp{net};

  bytes mem = make_bytes(1000, 0);
  tftp_out out;
  ASSERT_TRUE(sched.spawn(run_get(tftp, host, reloco::span<std::uint8_t>(mem.data(), mem.size()), out)).has_value());
  step();

  auto rrq = as_udp(take_tx());
  EXPECT_EQ(rrq.dst_port, 69);
  ASSERT_GE(rrq.payload.size(), 2u);
  EXPECT_EQ(rrq.payload[1], 1); // RRQ
  const std::uint16_t local = rrq.src_port;

  bytes b1 = make_bytes(512, 0x11);
  inject(udp_packet(host, 3000, board, local, tftp_pkt(3, 1, b1)));
  step();
  auto ack1 = as_udp(take_tx());
  EXPECT_EQ(ack1.dst_port, 3000); // replies go to the server's transfer port
  EXPECT_TRUE(net_test::bytes_equal(ack1.payload, tftp_pkt(4, 1)));
  EXPECT_FALSE(out.done);

  bytes b2 = make_bytes(88, 0x22);
  inject(udp_packet(host, 3000, board, local, tftp_pkt(3, 2, b2)));
  step();
  EXPECT_TRUE(net_test::bytes_equal(as_udp(take_tx()).payload, tftp_pkt(4, 2)));
  ASSERT_TRUE(out.done);
  ASSERT_TRUE(out.ok);
  EXPECT_EQ(out.size, 600u);
  EXPECT_EQ(mem[0], 0x11);
  EXPECT_EQ(mem[511], 0x11);
  EXPECT_EQ(mem[512], 0x22);
  EXPECT_EQ(mem[599], 0x22);
  EXPECT_FALSE(tftp.busy());
}

TEST_F(Netstack, TftpDownloadReportsServerErrorAndSmallBuffer) {
  bootldr::netstack_config cfg;
  cfg.static_ip = ipv4_config::make_static(board);
  bootldr::netstack<mtu> net{sched, nic, cfg};
  ASSERT_TRUE(net.poll_with(pnd).has_value());
  ASSERT_TRUE(net.start().has_value());
  bootldr::tftp_client tftp{net};

  bytes mem = make_bytes(100, 0);
  tftp_out out;
  ASSERT_TRUE(sched.spawn(run_get(tftp, host, reloco::span<std::uint8_t>(mem.data(), mem.size()), out)).has_value());
  step();
  const auto local = as_udp(take_tx()).src_port;
  bytes msg = make_bytes({'n', 'o', 0});
  inject(udp_packet(host, 3000, board, local, tftp_pkt(5, 1, msg)));
  step();
  ASSERT_TRUE(out.done);
  EXPECT_FALSE(out.ok);
  EXPECT_EQ(out.err, reloco::error::not_found);
  EXPECT_EQ(tftp.server_error(), 1);

  // A second transfer reuses the client; the 512-byte block does not fit into 100 bytes.
  tftp_out out2;
  ASSERT_TRUE(sched.spawn(run_get(tftp, host, reloco::span<std::uint8_t>(mem.data(), mem.size()), out2)).has_value());
  step();
  const auto local2 = as_udp(take_tx()).src_port;
  inject(udp_packet(host, 3001, board, local2, tftp_pkt(3, 1, make_bytes(512, 1))));
  step();
  ASSERT_TRUE(out2.done);
  EXPECT_EQ(out2.err, reloco::error::out_of_range);
}

TEST_F(Netstack, TftpIgnoresOtherHostsAndTimesOut) {
  bootldr::netstack_config cfg;
  cfg.static_ip = ipv4_config::make_static(board);
  bootldr::netstack<mtu> net{sched, nic, cfg};
  ASSERT_TRUE(net.poll_with(pnd).has_value());
  ASSERT_TRUE(net.start().has_value());
  bootldr::tftp_client tftp{net};

  bytes mem = make_bytes(100, 0);
  tftp_out out;
  ASSERT_TRUE(sched.spawn(run_get(tftp, host, reloco::span<std::uint8_t>(mem.data(), mem.size()), out)).has_value());
  step();
  const auto local = as_udp(take_tx()).src_port;
  inject(udp_packet(ipv4_address{192, 168, 7, 99}, 3000, board, local, tftp_pkt(3, 1, make_bytes(4, 7))));
  for (int i = 0; i < 80 && !out.done; ++i)
    step();
  ASSERT_TRUE(out.done);
  EXPECT_EQ(out.err, reloco::error::timed_out);
}

TEST_F(Netstack, TftpUploadsFromMemory) {
  bootldr::netstack_config cfg;
  cfg.static_ip = ipv4_config::make_static(board);
  bootldr::netstack<mtu> net{sched, nic, cfg};
  ASSERT_TRUE(net.poll_with(pnd).has_value());
  ASSERT_TRUE(net.start().has_value());
  bootldr::tftp_client tftp{net};

  bytes src = make_bytes(600, 0x5A);
  tftp_out out;
  ASSERT_TRUE(
      sched.spawn(run_put(tftp, host, reloco::span<const std::uint8_t>(src.data(), src.size()), out)).has_value());
  step();
  auto wrq = as_udp(take_tx());
  EXPECT_EQ(wrq.dst_port, 69);
  EXPECT_EQ(wrq.payload[1], 2); // WRQ
  const std::uint16_t local = wrq.src_port;

  inject(udp_packet(host, 3000, board, local, tftp_pkt(4, 0)));
  step();
  auto d1 = as_udp(take_tx());
  EXPECT_EQ(d1.dst_port, 3000);
  ASSERT_EQ(d1.payload.size(), 516u);
  EXPECT_EQ(d1.payload[1], 3);
  EXPECT_EQ(d1.payload[3], 1);

  inject(udp_packet(host, 3000, board, local, tftp_pkt(4, 1)));
  step();
  auto d2 = as_udp(take_tx());
  ASSERT_EQ(d2.payload.size(), 4u + 88u);
  EXPECT_EQ(d2.payload[3], 2);
  EXPECT_FALSE(out.done);

  inject(udp_packet(host, 3000, board, local, tftp_pkt(4, 2)));
  step();
  ASSERT_TRUE(out.done);
  ASSERT_TRUE(out.ok);
  EXPECT_EQ(out.size, 600u);
}

namespace {

reloco::task<void> recv_timed(bootldr::udp_socket &s, std::uint64_t ms, tftp_out &out) {
  reloco::array<std::uint8_t, 16> buf{};
  auto r = co_await s.receive_from(buf, ms);
  out.done = true;
  if (r) {
    out.ok = true;
    out.size = r->size;
  } else {
    out.err = r.error();
  }
}

} // namespace

TEST_F(Netstack, UdpReceiveTimeout) {
  bootldr::netstack_config cfg;
  cfg.static_ip = ipv4_config::make_static(board);
  bootldr::netstack<mtu> net{sched, nic, cfg};
  ASSERT_TRUE(net.poll_with(pnd).has_value());
  ASSERT_TRUE(net.start().has_value());
  bootldr::udp_socket sock{net};
  ASSERT_TRUE(sock.bind(5000).has_value());

  tftp_out timed_out;
  ASSERT_TRUE(sched.spawn(recv_timed(sock, 250, timed_out)).has_value());
  step(100);
  EXPECT_FALSE(timed_out.done);
  step(100);
  EXPECT_FALSE(timed_out.done);
  step(100);
  step(100); // the task started at t=100, so its 250 ms deadline is t=350
  ASSERT_TRUE(timed_out.done);
  EXPECT_EQ(timed_out.err, reloco::error::timed_out);

  // A datagram arriving before the deadline is delivered.
  tftp_out got;
  ASSERT_TRUE(sched.spawn(recv_timed(sock, 1000, got)).has_value());
  step(100);
  inject(udp_packet(host, 4000, board, 5000, make_bytes({1, 2, 3})));
  step(100);
  ASSERT_TRUE(got.done);
  EXPECT_TRUE(got.ok);
  EXPECT_EQ(got.size, 3u);

  // Timeout 0 only inspects the queue.
  tftp_out poll0;
  ASSERT_TRUE(sched.spawn(recv_timed(sock, 0, poll0)).has_value());
  step(10);
  ASSERT_TRUE(poll0.done);
  EXPECT_EQ(poll0.err, reloco::error::timed_out);
}
