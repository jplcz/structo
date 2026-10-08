// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/polled_net_device.hpp>
#include <structo/hw/slip_device.hpp>

#include <reloco/vec_deque.hpp>

#include "net_test_support.hpp"

using namespace structo;
using namespace structo::hw;
using net_test::bytes;
using net_test::bytes_equal;
using net_test::make_bytes;

namespace {

struct fake_uart {
  reloco::vec_deque<std::uint8_t> rx;
  bytes tx;
  std::size_t tx_budget = 1000000; // bytes the TX FIFO accepts before reporting busy
};

} // namespace

template <> struct structo::hw::uart_traits<fake_uart> {
  static reloco::result<void> configure(fake_uart &, const uart_config &) noexcept { return {}; }
  static reloco::result<bool> tx_ready(fake_uart &b) noexcept { return b.tx_budget > 0; }
  static reloco::result<bool> rx_ready(fake_uart &b) noexcept { return !b.rx.empty(); }
  static reloco::result<void> try_put_byte(fake_uart &b, std::uint8_t v) noexcept {
    --b.tx_budget;
    return b.tx.try_push_back(v);
  }
  static reloco::result<std::uint8_t> try_get_byte(fake_uart &b) noexcept {
    auto v = b.rx[0];
    (void)b.rx.try_pop_front();
    return v;
  }
};

namespace {

class Slip : public ::testing::Test {};
class SlipDevice : public ::testing::Test {};

void feed(fake_uart &u, const bytes &v) {
  for (std::size_t i = 0; i < v.size(); ++i)
    ASSERT_TRUE(u.rx.try_push_back(v[i]).has_value());
}

TEST_F(Slip, EncodeEscapes) {
  const bytes in = make_bytes({1, slip_end, 2, slip_esc, 3});
  reloco::array<std::uint8_t, 16> out{};
  auto n = slip_encode(in, out);
  ASSERT_TRUE(n.has_value());
  const bytes expect = make_bytes({slip_end, 1, slip_esc, slip_esc_end, 2, slip_esc, slip_esc_esc, 3, slip_end});
  EXPECT_TRUE(bytes_equal(reloco::span<const std::uint8_t>(out.data(), n.value()), expect));
  EXPECT_EQ(slip_encoded_size(in), n.value());
  reloco::array<std::uint8_t, 4> small{};
  EXPECT_EQ(slip_encode(in, small).error(), reloco::error::out_of_range);
}

TEST_F(Slip, DecoderRoundTripAndEdgeCases) {
  reloco::array<std::uint8_t, 8> buf{};
  slip_decoder d(buf);
  const bytes wire = make_bytes({slip_end, slip_end, 1, slip_esc, slip_esc_end, 3, slip_end, // good frame
                                 5, slip_esc, 0x01, 6, slip_end,                             // bad escape
                                 1, 2, 3, 4, 5, 6, 7, 8, 9, slip_end,                        // too long
                                 7, slip_end});                                              // good again
  reloco::vector<bytes> frames;
  for (std::size_t i = 0; i < wire.size(); ++i)
    if (d.push(wire[i]))
    {
      auto f = d.frame();
      ASSERT_TRUE(frames.try_push_back(make_bytes(f)).has_value());
    }
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_TRUE(bytes_equal(frames[0], make_bytes({1, slip_end, 3})));
  EXPECT_TRUE(bytes_equal(frames[1], make_bytes({7})));
  EXPECT_EQ(d.dropped(), 2u);
}

TEST_F(SlipDevice, SendQueuesAndDrainsWithBackpressure) {
  fake_uart u;
  u.tx_budget = 3;
  slip_device<16> dev{uart_ref{u}};
  const bytes f = make_bytes({1, 2, 3, 4});
  ASSERT_TRUE(dev.try_send(f).has_value());
  EXPECT_EQ(u.tx.size(), 3u);
  EXPECT_EQ(dev.try_send(f).error(), reloco::error::try_again); // still draining
  u.tx_budget = 100;
  ASSERT_TRUE(dev.try_send(f).has_value()); // first drains, second queued
  EXPECT_TRUE(bytes_equal(u.tx, make_bytes({slip_end, 1, 2, 3, 4, slip_end, slip_end, 1, 2, 3, 4, slip_end})));
  const bytes big = make_bytes(17, 0);
  EXPECT_EQ(dev.try_send(big).error(), reloco::error::out_of_range);
}

TEST_F(SlipDevice, ReceiveOverCoroutineInterface) {
  fake_uart u;
  slip_device<32> dev{uart_ref{u}};
  polled_net_device<slip_device<32>> pnd{dev};
  net_device_ref nic{pnd};
  EXPECT_EQ(nic.mtu(), 32u);
  EXPECT_EQ(nic.mac_address().error(), reloco::error::unsupported_operation);

  reloco::array<std::uint8_t, 32> buf{};
  auto t = nic.receive(buf);
  t.resume();
  EXPECT_FALSE(t.done());

  feed(u, make_bytes({slip_end, 9, slip_esc})); // partial frame
  EXPECT_EQ(pnd.poll(), 0u);
  feed(u, make_bytes({slip_esc_end, 8, slip_end}));
  EXPECT_EQ(pnd.poll(), 1u);
  ASSERT_TRUE(t.done());
  EXPECT_EQ(t.take().value(), 3u);
  EXPECT_TRUE(bytes_equal(make_bytes({buf[0], buf[1], buf[2]}), make_bytes({9, slip_end, 8})));
}

} // namespace
