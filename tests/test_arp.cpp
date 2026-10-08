// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/net/arp_table.hpp>

#include "net_test_support.hpp"

using namespace structo::net;
using structo::hw::net_mac_address;
using net_test::bytes;
using net_test::make_bytes;

namespace {

constexpr net_mac_address my_mac{2, 0, 0, 0, 0, 1};
constexpr net_mac_address peer_mac{2, 0, 0, 0, 0, 2};
constexpr ipv4_address me{10, 0, 0, 2};
constexpr ipv4_address peer{10, 0, 0, 1};

class Arp : public ::testing::Test {};
class ArpTable : public ::testing::Test {};

bytes wire(const arp_packet &p) {
  bytes b = make_bytes(arp_packet_size, 0);
  EXPECT_TRUE(build_arp(p, b).has_value());
  return b;
}

} // namespace

TEST_F(Arp, CodecRoundTripAndValidation) {
  const arp_packet p{arp_request, my_mac, me, {}, peer};
  const bytes b = wire(p);
  auto q = parse_arp(b);
  ASSERT_TRUE(q.has_value());
  EXPECT_EQ(q->op, arp_request);
  EXPECT_EQ(q->sender_mac, my_mac);
  EXPECT_EQ(q->sender_ip, me);
  EXPECT_EQ(q->target_ip, peer);

  bytes bad = make_bytes(b);
  bad[1] = 6; // not Ethernet
  EXPECT_EQ(parse_arp(bad).error(), error::invalid_argument);
  bad = make_bytes(b);
  bad[7] = 9; // unknown opcode
  EXPECT_EQ(parse_arp(bad).error(), error::invalid_argument);
  bytes shortb = make_bytes(10, 0);
  EXPECT_EQ(parse_arp(shortb).error(), error::invalid_argument);
  bytes small = make_bytes(10, 0);
  EXPECT_EQ(build_arp(p, small).error(), error::out_of_range);
}

TEST_F(ArpTable, ResolvesViaRequestAndReply) {
  arp_table<4> t{my_mac, me};
  EXPECT_EQ(t.lookup(peer, 0).error(), error::try_again);
  EXPECT_EQ(t.lookup(peer, 1).error(), error::try_again); // still pending, no duplicate request

  auto tx = t.poll(0);
  ASSERT_TRUE(tx.has_value());
  EXPECT_EQ(tx->dst_mac, (net_mac_address{255, 255, 255, 255, 255, 255}));
  auto req = parse_arp(tx->payload);
  ASSERT_TRUE(req.has_value());
  EXPECT_EQ(req->op, arp_request);
  EXPECT_EQ(req->target_ip, peer);
  EXPECT_FALSE(t.poll(10).has_value());

  const bytes rep = wire({arp_reply, peer_mac, peer, my_mac, me});
  EXPECT_TRUE(t.handle(rep, 100));
  auto m = t.lookup(peer, 200);
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(*m, peer_mac);
  EXPECT_FALSE(t.poll(300).has_value());

  // Learned entries expire.
  EXPECT_EQ(t.lookup(peer, 100 + arp_table<4>::entry_ttl_ms).error(), error::try_again);
}

TEST_F(ArpTable, RetriesThenGivesUp) {
  arp_table<2> t{my_mac, me};
  EXPECT_EQ(t.lookup(peer, 0).error(), error::try_again);
  unsigned sent = 0;
  for (std::uint64_t now = 0; now < 10000; now += 100)
    if (t.poll(now).has_value())
      ++sent;
  EXPECT_EQ(sent, arp_table<2>::max_requests);
  // Entry was dropped, so a new lookup starts a fresh request.
  EXPECT_EQ(t.lookup(peer, 10000).error(), error::try_again);
  EXPECT_TRUE(t.poll(10000).has_value());
}

TEST_F(ArpTable, AnswersRequestsForUsAndLearnsSender) {
  arp_table<4> t{my_mac, me};
  const bytes req = wire({arp_request, peer_mac, peer, {}, me});
  EXPECT_TRUE(t.handle(req, 0));
  auto tx = t.poll(0);
  ASSERT_TRUE(tx.has_value());
  EXPECT_EQ(tx->dst_mac, peer_mac);
  auto r = parse_arp(tx->payload);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->op, arp_reply);
  EXPECT_EQ(r->sender_mac, my_mac);
  EXPECT_EQ(r->sender_ip, me);
  EXPECT_EQ(r->target_ip, peer);
  const auto learned = t.lookup(peer, 1); // learned from the request
  ASSERT_TRUE(learned.has_value());
  EXPECT_EQ(*learned, peer_mac);

  // Requests for others are not answered and don't create entries.
  const bytes other = wire({arp_request, peer_mac, {10, 0, 0, 9}, {}, {10, 0, 0, 77}});
  EXPECT_TRUE(t.handle(other, 2));
  EXPECT_FALSE(t.poll(2).has_value());
  EXPECT_EQ(t.lookup({10, 0, 0, 9}, 3).error(), error::try_again);
}

TEST_F(ArpTable, StaticEntriesAndAnnounce) {
  arp_table<2> t{my_mac, me};
  ASSERT_TRUE(t.add_static(peer, peer_mac).has_value());
  const auto pinned = t.lookup(peer, 1'000'000);
  ASSERT_TRUE(pinned.has_value());
  EXPECT_EQ(*pinned, peer_mac);
  // A reply can't override a static entry.
  const bytes rep = wire({arp_reply, net_mac_address{9, 9, 9, 9, 9, 9}, peer, my_mac, me});
  EXPECT_TRUE(t.handle(rep, 0));
  const auto still = t.lookup(peer, 1);
  ASSERT_TRUE(still.has_value());
  EXPECT_EQ(*still, peer_mac);

  t.announce();
  auto tx = t.poll(0);
  ASSERT_TRUE(tx.has_value());
  auto g = parse_arp(tx->payload);
  ASSERT_TRUE(g.has_value());
  EXPECT_EQ(g->sender_ip, me);
  EXPECT_EQ(g->target_ip, me);
}
