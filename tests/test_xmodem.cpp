// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/bootldr/xmodem.hpp>

#include <deque>
#include <vector>

using namespace structo;
using namespace structo::bootldr;

namespace {

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

std::vector<std::uint8_t> make_image(std::size_t n) {
  std::vector<std::uint8_t> v(n);
  for (std::size_t i = 0; i < n; ++i)
    v[i] = static_cast<std::uint8_t>(i * 7 + 3);
  return v;
}

span<const std::uint8_t> view(const std::vector<std::uint8_t> &v) { return {v.data(), v.size()}; }

std::uint8_t rep(const xmodem_receiver &rx) {
  auto r = rx.reply();
  return r[0];
}

std::vector<std::uint8_t> pkt_vec(const xmodem_sender &tx) {
  auto p = tx.packet();
  return {p.begin(), p.end()};
}

std::uint8_t start_byte(xmodem_receiver &rx) {
  auto r = rx.start();
  return r[0];
}

// Wires a sender to a receiver, optionally corrupting the n-th packet once.
// Returns the bytes the receiver delivered (including SUB padding).
struct loopback_result {
  std::vector<std::uint8_t> data;
  xmodem_event rx_end = xmodem_event::none;
  xmodem_event tx_end = xmodem_event::none;
};

loopback_result run_loopback(const std::vector<std::uint8_t> &image, const xmodem_config &rxcfg,
                             const xmodem_config &txcfg, int corrupt_packet = -1) {
  loopback_result out;
  xmodem_receiver rx(rxcfg);
  xmodem_sender tx(view(image), txcfg);

  std::uint8_t first = start_byte(rx);
  xmodem_event tev = tx.on_byte(first);
  int pkt_index = 0;
  for (int guard = 0; guard < 10000 && tev == xmodem_event::transmit; ++guard) {
    std::vector<std::uint8_t> wire = pkt_vec(tx);
    if (pkt_index == corrupt_packet && wire.size() > 10)
      wire[10] ^= 0x55;
    ++pkt_index;

    xmodem_event rev = xmodem_event::none;
    std::uint8_t resp = 0;
    for (std::uint8_t b : wire) {
      rev = rx.on_byte(b);
      if (rev == xmodem_event::block)
        { auto blk = rx.block(); out.data.insert(out.data.end(), blk.begin(), blk.end()); }
      if (!rx.reply().empty())
        resp = rep(rx);
    }
    out.rx_end = rev;
    tev = tx.on_byte(resp);
  }
  out.tx_end = tev;
  return out;
}

class XmodemTest : public ::testing::Test {};

TEST_F(XmodemTest, Crc16KnownVector) {
  const char *s = "123456789";
  EXPECT_EQ(xmodem_crc16({reinterpret_cast<const std::uint8_t *>(s), 9}), 0x31C3);
}

TEST_F(XmodemTest, LoopbackCrc1k) {
  auto img = make_image(5000);
  auto r = run_loopback(img, {}, {});
  EXPECT_EQ(r.tx_end, xmodem_event::done);
  EXPECT_EQ(r.rx_end, xmodem_event::done);
  ASSERT_GE(r.data.size(), img.size());
  EXPECT_TRUE(std::equal(img.begin(), img.end(), r.data.begin()));
  for (std::size_t i = img.size(); i < r.data.size(); ++i)
    EXPECT_EQ(r.data[i], xmodem_ctl::sub);
}

TEST_F(XmodemTest, LoopbackChecksumMode) {
  auto img = make_image(300);
  xmodem_config rxcfg;
  rxcfg.use_crc = false;
  auto r = run_loopback(img, rxcfg, {});
  EXPECT_EQ(r.tx_end, xmodem_event::done);
  EXPECT_EQ(r.data.size(), 384u); // 3 x 128-byte packets, no 1K in checksum mode
  EXPECT_TRUE(std::equal(img.begin(), img.end(), r.data.begin()));
}

TEST_F(XmodemTest, LoopbackSmallBlocksOnly) {
  auto img = make_image(1000);
  xmodem_config txcfg;
  txcfg.use_1k = false;
  auto r = run_loopback(img, {}, txcfg);
  EXPECT_EQ(r.tx_end, xmodem_event::done);
  EXPECT_EQ(r.data.size(), 1024u);
}

TEST_F(XmodemTest, EmptyImageSendsOnlyEot) {
  auto r = run_loopback({}, {}, {});
  EXPECT_EQ(r.tx_end, xmodem_event::done);
  EXPECT_TRUE(r.data.empty());
}

TEST_F(XmodemTest, CorruptedPacketIsRetransmitted) {
  auto img = make_image(2500);
  auto r = run_loopback(img, {}, {}, /*corrupt_packet=*/1);
  EXPECT_EQ(r.tx_end, xmodem_event::done);
  ASSERT_GE(r.data.size(), img.size());
  EXPECT_TRUE(std::equal(img.begin(), img.end(), r.data.begin()));
}

TEST_F(XmodemTest, DuplicatePacketIsAckedButNotDelivered) {
  auto img = make_image(100);
  xmodem_sender tx(view(img));
  xmodem_receiver rx;
  ASSERT_EQ(tx.on_byte('C'), xmodem_event::transmit);
  std::vector<std::uint8_t> wire = pkt_vec(tx);

  xmodem_event ev = xmodem_event::none;
  for (auto b : wire)
    ev = rx.on_byte(b);
  EXPECT_EQ(ev, xmodem_event::block);
  for (auto b : wire)
    ev = rx.on_byte(b);
  EXPECT_EQ(ev, xmodem_event::none);
  ASSERT_EQ(rx.reply().size(), 1u);
  EXPECT_EQ(rep(rx), xmodem_ctl::ack);
  EXPECT_EQ(rx.blocks_received(), 1u);
}

TEST_F(XmodemTest, ReceiverFallsBackToChecksumOnSilence) {
  xmodem_receiver rx;
  EXPECT_EQ(start_byte(rx), 'C');
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(rx.on_timeout(), xmodem_event::none);
    ASSERT_EQ(rx.reply().size(), 1u);
  }
  EXPECT_FALSE(rx.crc_mode());
  EXPECT_EQ(rep(rx), xmodem_ctl::nak);
}

TEST_F(XmodemTest, ReceiverFailsAfterRetries) {
  xmodem_config cfg;
  cfg.max_retries = 2;
  xmodem_receiver rx(cfg);
  (void)rx.start();
  EXPECT_EQ(rx.on_timeout(), xmodem_event::none);
  EXPECT_EQ(rx.on_timeout(), xmodem_event::none);
  EXPECT_EQ(rx.on_timeout(), xmodem_event::failed);
  EXPECT_EQ(rep(rx), xmodem_ctl::can);
}

TEST_F(XmodemTest, CancelFromPeer) {
  xmodem_receiver rx;
  EXPECT_EQ(rx.on_byte(xmodem_ctl::can), xmodem_event::cancelled);
  auto img = make_image(10);
  xmodem_sender tx(view(img));
  EXPECT_EQ(tx.on_byte(xmodem_ctl::can), xmodem_event::cancelled);
}

TEST_F(XmodemTest, SenderGivesUpAfterRepeatedNak) {
  auto img = make_image(10);
  xmodem_config cfg;
  cfg.max_retries = 3;
  xmodem_sender tx(view(img), cfg);
  EXPECT_EQ(tx.on_byte('C'), xmodem_event::transmit);
  EXPECT_EQ(tx.on_byte(xmodem_ctl::nak), xmodem_event::transmit);
  EXPECT_EQ(tx.on_byte(xmodem_ctl::nak), xmodem_event::transmit);
  EXPECT_EQ(tx.on_byte(xmodem_ctl::nak), xmodem_event::transmit);
  EXPECT_EQ(tx.on_byte(xmodem_ctl::nak), xmodem_event::failed);
}

TEST_F(XmodemTest, BlockNumberWrapsAt256) {
  auto img = make_image(128 * 300);
  xmodem_config txcfg;
  txcfg.use_1k = false;
  auto r = run_loopback(img, {}, txcfg);
  EXPECT_EQ(r.tx_end, xmodem_event::done);
  EXPECT_EQ(r.data.size(), img.size());
  EXPECT_EQ(r.data, img);
}

TEST_F(XmodemTest, UartDriversRoundTrip) {
  auto img = make_image(3000);

  // Record the sender's wire output over a scripted receiver.
  fake_uart tx_dev;
  tx_dev.rx.push_back('C');
  for (int i = 0; i < 16; ++i)
    tx_dev.rx.push_back(xmodem_ctl::ack);
  ASSERT_TRUE(send(hw::uart_ref(tx_dev), view(img)).has_value());

  // Replay it into the receiver driver.
  fake_uart rx_dev;
  rx_dev.rx.assign(tx_dev.tx.begin(), tx_dev.tx.end());
  std::vector<std::uint8_t> got;
  auto sink = [&](span<const std::uint8_t> p) {
    got.insert(got.end(), p.begin(), p.end());
    return true;
  };
  auto r = receive(hw::uart_ref(rx_dev), function_ref<bool(span<const std::uint8_t>)>(sink));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r.value(), 3u);
  ASSERT_GE(got.size(), img.size());
  EXPECT_TRUE(std::equal(img.begin(), img.end(), got.begin()));
  EXPECT_EQ(rx_dev.tx.front(), 'C');
}

TEST_F(XmodemTest, ReceiveAbortsWhenSinkRefuses) {
  auto img = make_image(200);
  fake_uart tx_dev;
  tx_dev.rx.push_back('C');
  for (int i = 0; i < 4; ++i)
    tx_dev.rx.push_back(xmodem_ctl::ack);
  ASSERT_TRUE(send(hw::uart_ref(tx_dev), view(img)).has_value());

  fake_uart rx_dev;
  rx_dev.rx.assign(tx_dev.tx.begin(), tx_dev.tx.end());
  auto sink = [](span<const std::uint8_t>) { return false; };
  auto r = receive(hw::uart_ref(rx_dev), function_ref<bool(span<const std::uint8_t>)>(sink));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), error::capacity_exceeded);
  EXPECT_EQ(rx_dev.tx.back(), xmodem_ctl::can);
}

} // namespace
