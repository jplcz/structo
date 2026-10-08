// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/net/tftp_client.hpp>

#include <string>
#include <vector>

using namespace structo::net;

namespace {

using bytes = std::vector<std::uint8_t>;

constexpr ipv4_address srv{10, 0, 0, 1};
constexpr ipv4_address me{10, 0, 0, 2};
constexpr std::uint16_t my_port = 49200;
constexpr std::uint16_t srv_tid = 3333;

// Holds the UDP bytes so the ipv4_packet payload span stays valid.
struct reply {
  bytes udp;
  ipv4_packet pkt;
  reply(const bytes &tftp, std::uint16_t from_port = srv_tid, std::uint16_t to_port = my_port) {
    udp.resize(udp_header_size + tftp.size());
    EXPECT_TRUE(build_udp(from_port, to_port, tftp, srv, me, udp).has_value());
    pkt.header.protocol = ip_proto_udp;
    pkt.header.src = srv;
    pkt.header.dst = me;
    pkt.payload = span<const std::uint8_t>(udp.data(), udp.size());
  }
};

bytes data_pkt(std::uint16_t block, std::size_t n, std::uint8_t fill = 0xAB) {
  bytes d(n, fill);
  bytes out(4 + n);
  auto r = build_tftp_data(block, d, out);
  EXPECT_TRUE(r.has_value());
  return out;
}

bytes ack_pkt(std::uint16_t block) {
  bytes out(4);
  EXPECT_TRUE(build_tftp_ack(block, out).has_value());
  return out;
}

bytes copy(span<const std::uint8_t> s) { return bytes(s.data(), s.data() + s.size()); }

} // namespace

TEST(Tftp, RequestAndPacketCodec) {
  bytes buf(64);
  auto n = build_tftp_request(tftp_rrq, "a.bin", buf);
  ASSERT_TRUE(n.has_value());
  const bytes expect{0, 1, 'a', '.', 'b', 'i', 'n', 0, 'o', 'c', 't', 'e', 't', 0};
  EXPECT_EQ(bytes(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*n)), expect);

  EXPECT_EQ(build_tftp_request(tftp_rrq, "", buf).error(), error::invalid_argument);
  EXPECT_EQ(build_tftp_request(tftp_data, "x", buf).error(), error::invalid_argument);
  bytes tiny(4);
  EXPECT_EQ(build_tftp_request(tftp_wrq, "x", tiny).error(), error::out_of_range);

  const bytes d = data_pkt(7, 3);
  auto p = parse_tftp(d);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->opcode, tftp_data);
  EXPECT_EQ(p->block, 7);
  EXPECT_EQ(p->data.size(), 3u);

  bytes e(32);
  auto en = build_tftp_error(1, "nope", e);
  ASSERT_TRUE(en.has_value());
  const bytes eb(e.begin(), e.begin() + static_cast<std::ptrdiff_t>(*en));
  auto ep = parse_tftp(eb);
  ASSERT_TRUE(ep.has_value());
  EXPECT_EQ(ep->error_code, 1);
  EXPECT_EQ(ep->message, "nope");

  const bytes bad_ack{0, 4, 0, 1, 0};
  EXPECT_EQ(parse_tftp(bad_ack).error(), error::invalid_argument);
  const bytes rrq{0, 1, 'x', 0, 0, 0};
  EXPECT_EQ(parse_tftp(rrq).error(), error::unsupported_operation);
}

TEST(TftpClient, ReadMultiBlockWithLostAck) {
  tftp_client c{my_port};
  ASSERT_TRUE(c.start_read(srv, "f", 0).has_value());

  auto out = c.poll(0);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(copy(*out)[1], tftp_rrq);
  EXPECT_EQ(c.remote_port(), tftp_server_port);
  EXPECT_FALSE(c.poll(500).has_value());

  // No answer: the RRQ is retransmitted after 1 s.
  ASSERT_TRUE(c.poll(1000).has_value());

  // First contact from any port is accepted as the TID only for a valid DATA(1); a wrong block is ignored.
  reply wrong(data_pkt(5, 512), 9999);
  EXPECT_EQ(c.handle(wrong.pkt, 1100), tftp_event::ignored);

  reply d1(data_pkt(1, 512, 1));
  ASSERT_EQ(c.handle(d1.pkt, 1200), tftp_event::data);
  EXPECT_EQ(c.data().size(), 512u);
  EXPECT_EQ(c.remote_port(), srv_tid);
  auto a1 = c.poll(1200);
  ASSERT_TRUE(a1.has_value());
  EXPECT_EQ(copy(*a1), ack_pkt(1));

  // Server resends block 1 (our ACK was lost): we repeat the ACK, no new data.
  EXPECT_EQ(c.handle(d1.pkt, 1300), tftp_event::none);
  auto a1b = c.poll(1300);
  ASSERT_TRUE(a1b.has_value());
  EXPECT_EQ(copy(*a1b), ack_pkt(1));

  reply d2(data_pkt(2, 100, 2));
  ASSERT_EQ(c.handle(d2.pkt, 1400), tftp_event::data);
  EXPECT_EQ(c.data().size(), 100u);
  EXPECT_TRUE(c.finished());
  auto a2 = c.poll(1400);
  ASSERT_TRUE(a2.has_value());
  EXPECT_EQ(copy(*a2), ack_pkt(2));
  EXPECT_FALSE(c.poll(9999).has_value()); // no retransmits once done
}

TEST(TftpClient, ReadExactMultipleEndsWithEmptyBlock) {
  tftp_client c{my_port};
  ASSERT_TRUE(c.start_read(srv, "f", 0).has_value());
  (void)c.poll(0);
  reply d1(data_pkt(1, 512));
  ASSERT_EQ(c.handle(d1.pkt, 1), tftp_event::data);
  EXPECT_FALSE(c.finished());
  (void)c.poll(1);
  reply d2(data_pkt(2, 0));
  ASSERT_EQ(c.handle(d2.pkt, 2), tftp_event::data);
  EXPECT_TRUE(c.finished());
  EXPECT_EQ(c.data().size(), 0u);
}

TEST(TftpClient, ReadErrorAndTimeout) {
  tftp_client c{my_port};
  ASSERT_TRUE(c.start_read(srv, "missing", 0).has_value());
  (void)c.poll(0);
  bytes e(32);
  auto n = build_tftp_error(1, "File not found", e);
  e.resize(*n);
  reply r(e);
  EXPECT_EQ(c.handle(r.pkt, 10), tftp_event::failed);
  EXPECT_TRUE(c.failed());
  EXPECT_EQ(c.error_code(), 1);

  tftp_client t{my_port};
  ASSERT_TRUE(t.start_read(srv, "x", 0).has_value());
  std::uint64_t now = 0;
  unsigned sends = 0;
  while (!t.failed() && now < 20000) {
    if (t.poll(now).has_value())
      ++sends;
    now += 100;
  }
  EXPECT_TRUE(t.timed_out());
  EXPECT_EQ(sends, 1u + tftp_client::max_retries);
}

TEST(TftpClient, WriteMultiBlock) {
  tftp_client c{my_port};
  ASSERT_TRUE(c.start_write(srv, "up", 0).has_value());
  auto wrq = c.poll(0);
  ASSERT_TRUE(wrq.has_value());
  EXPECT_EQ(copy(*wrq)[1], tftp_wrq);

  const bytes one{1};
  EXPECT_FALSE(c.supply(one).has_value()); // nothing requested yet

  reply a0(ack_pkt(0));
  ASSERT_EQ(c.handle(a0.pkt, 5), tftp_event::need_data);
  EXPECT_FALSE(c.poll(99999).has_value()); // timer paused while waiting for the application

  const bytes b1(512, 0x11);
  ASSERT_TRUE(c.supply(b1).has_value());
  auto d1 = c.poll(10);
  ASSERT_TRUE(d1.has_value());
  EXPECT_EQ(copy(*d1), data_pkt(1, 512, 0x11));

  // Retransmit on silence, then a duplicate ACK(0) must not trigger anything.
  ASSERT_TRUE(c.poll(1010).has_value());
  EXPECT_EQ(c.handle(a0.pkt, 1011), tftp_event::none);

  reply a1(ack_pkt(1));
  ASSERT_EQ(c.handle(a1.pkt, 1020), tftp_event::need_data);
  const bytes b2(10, 0x22);
  ASSERT_TRUE(c.supply(b2).has_value());
  auto d2 = c.poll(1020);
  ASSERT_TRUE(d2.has_value());
  EXPECT_EQ(copy(*d2), data_pkt(2, 10, 0x22));

  reply a2(ack_pkt(2));
  EXPECT_EQ(c.handle(a2.pkt, 1030), tftp_event::done);
  EXPECT_TRUE(c.finished());
}
