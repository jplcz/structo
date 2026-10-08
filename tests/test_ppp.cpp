// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/bootldr/netstack.hpp>
#include <structo/hw/ppp_device.hpp>

#include <reloco/vec_deque.hpp>

#include "net_test_support.hpp"

using namespace structo;
using namespace structo::net;
using namespace structo::hw;
using net_test::bytes;

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
constexpr ipv4_address host{192, 168, 7, 1};
constexpr ipv4_address board{192, 168, 7, 2};

std::uint64_t clock_now(void *ctx) noexcept { return *static_cast<std::uint64_t *>(ctx); }

// Encodes one PPP frame and returns it as bytes.
bytes encode(std::uint16_t proto, const bytes &payload) {
  reloco::array<std::uint8_t, 2 * mtu + 16> w{};
  reloco::span<const std::uint8_t> p(payload.data(), payload.size());
  auto n = ppp_encode(proto, p, reloco::span<std::uint8_t>(w));
  EXPECT_TRUE(n.has_value());
  bytes out;
  for (std::size_t i = 0; i < n.value(); ++i)
    EXPECT_TRUE(out.try_push_back(w[i]).has_value());
  return out;
}

bytes lcp(std::uint8_t code, std::uint8_t id, std::initializer_list<std::uint8_t> opts) {
  bytes b;
  const auto len = static_cast<std::uint16_t>(4 + opts.size());
  (void)b.try_push_back(code);
  (void)b.try_push_back(id);
  (void)b.try_push_back(static_cast<std::uint8_t>(len >> 8));
  (void)b.try_push_back(static_cast<std::uint8_t>(len));
  for (auto o : opts)
    (void)b.try_push_back(o);
  return b;
}

// Splits what the device wrote into decoded frames.
struct frame_copy {
  std::uint16_t proto = 0;
  bytes data;
};

class Ppp : public ::testing::Test {
protected:
  std::uint64_t now = 0;
  fake_uart uart;
  ppp_config cfg{};
  reloco::vec_deque<frame_copy> sent;
  reloco::array<std::uint8_t, 2 * mtu> dbuf{};
  ppp_decoder dec{reloco::span<std::uint8_t>(dbuf)};

  void feed(std::uint16_t proto, const bytes &payload) {
    auto f = encode(proto, payload);
    for (std::size_t i = 0; i < f.size(); ++i)
      ASSERT_TRUE(uart.rx.try_push_back(f[i]).has_value());
  }

  // Collects frames the device transmitted since the last call.
  void collect() {
    for (std::size_t i = 0; i < uart.tx.size(); ++i) {
      if (!dec.push(uart.tx[i]))
        continue;
      auto f = dec.frame();
      frame_copy c;
      c.proto = f.protocol;
      for (std::size_t k = 0; k < f.payload.size(); ++k)
        ASSERT_TRUE(c.data.try_push_back(f.payload[k]).has_value());
      ASSERT_TRUE(sent.try_push_back(std::move(c)).has_value());
    }
    uart.tx.clear();
  }

  bool have(std::uint16_t proto, std::uint8_t code) {
    for (std::size_t i = 0; i < sent.size(); ++i)
      if (sent[i].proto == proto && !sent[i].data.empty() && sent[i].data[0] == code)
        return true;
    return false;
  }

  std::uint8_t last_id(std::uint16_t proto, std::uint8_t code) {
    for (std::size_t i = sent.size(); i-- > 0;)
      if (sent[i].proto == proto && !sent[i].data.empty() && sent[i].data[0] == code)
        return sent[i].data[1];
    return 0;
  }

  // Runs the scripted peer: LCP, then IPCP handing out `board`.
  template <class Dev> void negotiate(Dev &dev) {
    reloco::array<std::uint8_t, mtu> buf{};
    auto pump = [&] {
      (void)dev.try_receive(reloco::span<std::uint8_t>(buf));
      collect();
    };
    pump();
    ASSERT_TRUE(have(ppp_proto_lcp, 1));
    feed(ppp_proto_lcp, lcp(2, last_id(ppp_proto_lcp, 1), {})); // ack ours
    feed(ppp_proto_lcp, lcp(1, 7, {1, 4, 0x05, 0xdc, 5, 6, 1, 2, 3, 4})); // their request: MRU + magic
    pump();
    ASSERT_TRUE(have(ppp_proto_lcp, 2));
    ASSERT_EQ(dev.link().lcp_state(), ppp_state::opened);
    pump();
    ASSERT_TRUE(have(ppp_proto_ipcp, 1));
    // Nak our 0.0.0.0 with the address to use, then ack the retry.
    feed(ppp_proto_ipcp, lcp(3, last_id(ppp_proto_ipcp, 1), {3, 6, board.octets[0], board.octets[1], board.octets[2], board.octets[3]}));
    pump();
    feed(ppp_proto_ipcp, lcp(2, last_id(ppp_proto_ipcp, 1), {3, 6, board.octets[0], board.octets[1], board.octets[2], board.octets[3]}));
    feed(ppp_proto_ipcp, lcp(1, 9, {3, 6, host.octets[0], host.octets[1], host.octets[2], host.octets[3]}));
    pump();
  }
};

} // namespace

TEST(PppFraming, FcsCheckVector) {
  const std::uint8_t d[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  auto fcs = ppp_fcs16(ppp_fcs_init, reloco::span<const std::uint8_t>(d, sizeof d));
  EXPECT_EQ(static_cast<std::uint16_t>(~fcs), 0x906E);
}

TEST(PppFraming, RoundTripWithEscaping) {
  bytes payload;
  for (int v : {0x00, 0x7E, 0x7D, 0x11, 0x41, 0xFF})
    ASSERT_TRUE(payload.try_push_back(static_cast<std::uint8_t>(v)).has_value());
  auto wire = encode(ppp_proto_ip, payload);
  reloco::array<std::uint8_t, 64> buf{};
  ppp_decoder d{reloco::span<std::uint8_t>(buf)};
  bool got = false;
  for (std::size_t i = 0; i < wire.size(); ++i)
    got = d.push(wire[i]) || got;
  ASSERT_TRUE(got);
  auto f = d.frame();
  EXPECT_EQ(f.protocol, ppp_proto_ip);
  ASSERT_EQ(f.payload.size(), payload.size());
  for (std::size_t i = 0; i < payload.size(); ++i)
    EXPECT_EQ(f.payload[i], payload[i]);
}

TEST(PppFraming, BadFcsDropped) {
  bytes payload;
  ASSERT_TRUE(payload.try_push_back(1).has_value());
  auto wire = encode(ppp_proto_ip, payload);
  wire[wire.size() - 3] ^= 0x55;
  reloco::array<std::uint8_t, 64> buf{};
  ppp_decoder d{reloco::span<std::uint8_t>(buf)};
  bool got = false;
  for (std::size_t i = 0; i < wire.size(); ++i)
    got = d.push(wire[i]) || got;
  EXPECT_FALSE(got);
  EXPECT_EQ(d.dropped(), 1u);
}

TEST(PppFraming, AcceptsCompressedHeader) {
  // Flag, protocol 0x21 as one byte (PFC), payload 0x45, FCS over {0x21, 0x45}.
  const std::uint8_t body[] = {0x21, 0x45};
  auto fcs = static_cast<std::uint16_t>(~ppp_fcs16(ppp_fcs_init, reloco::span<const std::uint8_t>(body, 2)));
  bytes wire;
  for (std::uint8_t v : {std::uint8_t{0x7E}, std::uint8_t{0x21}, std::uint8_t{0x45}, static_cast<std::uint8_t>(fcs),
                         static_cast<std::uint8_t>(fcs >> 8), std::uint8_t{0x7E}})
    ASSERT_TRUE(wire.try_push_back(v).has_value());
  reloco::array<std::uint8_t, 16> buf{};
  ppp_decoder d{reloco::span<std::uint8_t>(buf)};
  bool got = false;
  for (std::size_t i = 0; i < wire.size(); ++i)
    got = d.push(wire[i]) || got;
  ASSERT_TRUE(got);
  EXPECT_EQ(d.frame().protocol, ppp_proto_ip);
  EXPECT_EQ(d.frame().payload.size(), 1u);
}

TEST_F(Ppp, NegotiatesAddress) {
  ppp_device<mtu> dev{uart_ref{uart}, clock_now, &now, cfg};
  negotiate(dev);
  ASSERT_TRUE(dev.link().ip_up());
  EXPECT_EQ(dev.link().local_address(), board);
  EXPECT_EQ(dev.link().peer_address(), host);
  EXPECT_EQ(dev.link().ipv4().gateway, host);
  EXPECT_TRUE(dev.link_up().value());
}

TEST_F(Ppp, RejectsAuthentication) {
  ppp_device<mtu> dev{uart_ref{uart}, clock_now, &now, cfg};
  reloco::array<std::uint8_t, mtu> buf{};
  (void)dev.try_receive(reloco::span<std::uint8_t>(buf));
  collect();
  feed(ppp_proto_lcp, lcp(1, 3, {3, 4, 0xc0, 0x23})); // PAP
  (void)dev.try_receive(reloco::span<std::uint8_t>(buf));
  collect();
  EXPECT_TRUE(have(ppp_proto_lcp, 4)); // Configure-Reject
}

TEST_F(Ppp, AnswersEcho) {
  ppp_device<mtu> dev{uart_ref{uart}, clock_now, &now, cfg};
  negotiate(dev);
  feed(ppp_proto_lcp, lcp(9, 5, {0, 0, 0, 0}));
  reloco::array<std::uint8_t, mtu> buf{};
  (void)dev.try_receive(reloco::span<std::uint8_t>(buf));
  collect();
  EXPECT_TRUE(have(ppp_proto_lcp, 10));
}

TEST_F(Ppp, ProtocolRejectForUnknown) {
  ppp_device<mtu> dev{uart_ref{uart}, clock_now, &now, cfg};
  negotiate(dev);
  feed(0x8057, lcp(1, 1, {})); // IPv6CP
  reloco::array<std::uint8_t, mtu> buf{};
  (void)dev.try_receive(reloco::span<std::uint8_t>(buf));
  collect();
  EXPECT_TRUE(have(ppp_proto_lcp, 8));
}

TEST_F(Ppp, RetransmitsConfigureRequest) {
  ppp_device<mtu> dev{uart_ref{uart}, clock_now, &now, cfg};
  reloco::array<std::uint8_t, mtu> buf{};
  (void)dev.try_receive(reloco::span<std::uint8_t>(buf));
  collect();
  const auto first = sent.size();
  now += cfg.restart_ms + 1;
  (void)dev.try_receive(reloco::span<std::uint8_t>(buf));
  collect();
  EXPECT_GT(sent.size(), first);
}

TEST_F(Ppp, GivesUpWithoutPeer) {
  cfg.max_configure = 3;
  ppp_device<mtu> dev{uart_ref{uart}, clock_now, &now, cfg};
  reloco::array<std::uint8_t, mtu> buf{};
  for (int i = 0; i < 10; ++i) {
    (void)dev.try_receive(reloco::span<std::uint8_t>(buf));
    collect();
    now += cfg.restart_ms + 1;
  }
  EXPECT_TRUE(dev.link().failed());
}

TEST_F(Ppp, IpTrafficOnlyWhenUp) {
  ppp_device<mtu> dev{uart_ref{uart}, clock_now, &now, cfg};
  const std::uint8_t pkt[] = {0x45, 0, 0, 20};
  auto early = dev.try_send(reloco::span<const std::uint8_t>(pkt, sizeof pkt));
  ASSERT_FALSE(early.has_value());
  EXPECT_EQ(early.error(), reloco::error::try_again);

  negotiate(dev);
  sent = {};
  ASSERT_TRUE(dev.try_send(reloco::span<const std::uint8_t>(pkt, sizeof pkt)).has_value());
  collect();
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].proto, ppp_proto_ip);
  EXPECT_EQ(sent[0].data.size(), sizeof pkt);

  bytes in;
  for (auto v : pkt)
    ASSERT_TRUE(in.try_push_back(v).has_value());
  feed(ppp_proto_ip, in);
  reloco::array<std::uint8_t, mtu> buf{};
  auto r = dev.try_receive(reloco::span<std::uint8_t>(buf));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r.value(), sizeof pkt);
  EXPECT_EQ(buf[0], 0x45);
}

TEST_F(Ppp, NetstackTakesPppAddress) {
  ppp_device<mtu> dev{uart_ref{uart}, clock_now, &now, cfg};
  polled_net_device<ppp_device<mtu>> pnd{dev};
  net_device_ref nic{pnd};
  bootldr::scheduler sched;
  sched.set_clock(clock_now, &now);
  bootldr::netstack<mtu> stack{sched, nic};
  ASSERT_TRUE(stack.poll_with(pnd).has_value());
  stack.use_ppp(dev.link());
  ASSERT_TRUE(stack.start().has_value());

  auto step = [&] {
    now += 100;
    for (int i = 0; i < 3; ++i)
      sched.run_once();
    collect();
  };
  step();
  ASSERT_TRUE(have(ppp_proto_lcp, 1));
  feed(ppp_proto_lcp, lcp(2, last_id(ppp_proto_lcp, 1), {}));
  feed(ppp_proto_lcp, lcp(1, 7, {}));
  step();
  step();
  ASSERT_TRUE(have(ppp_proto_ipcp, 1));
  feed(ppp_proto_ipcp, lcp(3, last_id(ppp_proto_ipcp, 1), {3, 6, board.octets[0], board.octets[1], board.octets[2], board.octets[3]}));
  step();
  feed(ppp_proto_ipcp, lcp(2, last_id(ppp_proto_ipcp, 1), {3, 6, board.octets[0], board.octets[1], board.octets[2], board.octets[3]}));
  feed(ppp_proto_ipcp, lcp(1, 9, {3, 6, host.octets[0], host.octets[1], host.octets[2], host.octets[3]}));
  step();
  step();
  step();
  EXPECT_TRUE(stack.ready());
  EXPECT_EQ(stack.config().address, board);
}
