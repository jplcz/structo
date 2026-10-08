// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/polled_net_device.hpp>

#include <reloco/vec_deque.hpp>

#include "net_test_support.hpp"

using namespace structo;
using namespace structo::hw;
using net_test::bytes;
using net_test::bytes_equal;
using net_test::make_bytes;

namespace {

struct fake_nic {
  reloco::vec_deque<bytes> rx;
  reloco::vector<bytes> tx;
  std::size_t tx_room = 0;
  bool up = true;
};

} // namespace

struct fake_nic_backend {
  fake_nic &n;
  std::size_t mtu() const noexcept { return 64; }
  reloco::result<bool> link_up() noexcept { return n.up; }
  reloco::result<net_mac_address> mac_address() noexcept { return net_mac_address{2, 0, 0, 0, 0, 1}; }
  reloco::result<void> try_send(reloco::span<const std::uint8_t> f) noexcept {
    if (n.tx_room == 0)
      return reloco::unexpected(reloco::error::try_again);
    --n.tx_room;
    if (!n.tx.try_push_back(make_bytes(f)))
      return reloco::unexpected(reloco::error::allocation_failed);
    return {};
  }
  reloco::result<std::size_t> try_receive(reloco::span<std::uint8_t> dst) noexcept {
    if (n.rx.empty())
      return reloco::unexpected(reloco::error::try_again);
    if (n.rx[0].size() > dst.size())
      return reloco::unexpected(reloco::error::out_of_range);
    auto len = n.rx[0].size();
    for (std::size_t i = 0; i < len; ++i)
      dst[i] = n.rx[0][i];
    (void)n.rx.try_pop_front();
    return len;
  }
};

// No mac_address: SLIP-like.
struct slip_backend {
  std::size_t mtu() const noexcept { return 296; }
  reloco::result<bool> link_up() noexcept { return true; }
  reloco::result<void> try_send(reloco::span<const std::uint8_t>) noexcept { return {}; }
  reloco::result<std::size_t> try_receive(reloco::span<std::uint8_t>) noexcept {
    return reloco::unexpected(reloco::error::try_again);
  }
};

using fake_dev = polled_net_device<fake_nic_backend>;

struct fixture {
  void push_rx(bytes b) { ASSERT_TRUE(nic.rx.try_push_back(std::move(b)).has_value()); }
  fake_nic nic;
  fake_nic_backend be{nic};
  fake_dev dev{be};
  net_device_ref ref{dev};
};

namespace {

class NetDeviceRef : public ::testing::Test {};
class PolledNetDevice : public ::testing::Test {};

reloco::task<std::size_t> echo_one(net_device_ref nic) {
  reloco::array<std::uint8_t, 64> buf;
  std::size_t n = co_await co_await nic.receive(buf);
  co_await co_await nic.send({buf.data(), n});
  co_return n;
}

TEST_F(NetDeviceRef, Basics) {
  fixture f;
  EXPECT_EQ(f.ref.mtu(), 64u);
  EXPECT_TRUE(f.ref.link_up().value());
  EXPECT_TRUE(f.ref.mac_address().has_value());

  slip_backend sb;
  polled_net_device<slip_backend> sd{sb};
  net_device_ref sref{sd};
  EXPECT_EQ(sref.mtu(), 296u);
  EXPECT_EQ(sref.mac_address().error(), reloco::error::unsupported_operation);

  net_device_ref unbound;
  EXPECT_FALSE(unbound);
  EXPECT_EQ(unbound.mtu(), 0u);
  EXPECT_EQ(unbound.link_up().error(), reloco::error::unsupported_operation);
  reloco::array<std::uint8_t, 4> b;
  auto t = unbound.receive(b);
  t.resume();
  ASSERT_TRUE(t.done());
  EXPECT_EQ(t.take().error(), reloco::error::unsupported_operation);
}

TEST_F(PolledNetDevice, ImmediateCompletionNeverSuspends) {
  fixture f;
  f.push_rx(make_bytes({1, 2, 3}));
  f.nic.tx_room = 1;
  auto t = echo_one(f.ref);
  t.resume();
  ASSERT_TRUE(t.done());
  EXPECT_EQ(t.take().value(), 3u);
  ASSERT_EQ(f.nic.tx.size(), 1u);
  EXPECT_TRUE(bytes_equal(f.nic.tx[0], make_bytes({1, 2, 3})));
}

TEST_F(PolledNetDevice, SuspendsUntilPolled) {
  fixture f;
  auto t = echo_one(f.ref);
  t.resume();
  EXPECT_FALSE(t.done());
  EXPECT_TRUE(f.dev.receive_pending());
  EXPECT_EQ(f.dev.poll(), 0u);

  f.push_rx(make_bytes({9, 8}));
  EXPECT_EQ(f.dev.poll(), 1u); // frame received, then parks on TX
  EXPECT_FALSE(t.done());
  EXPECT_FALSE(f.dev.receive_pending());
  EXPECT_TRUE(f.dev.send_pending());

  EXPECT_EQ(f.dev.poll(), 0u);
  f.nic.tx_room = 1;
  EXPECT_EQ(f.dev.poll(), 1u);
  ASSERT_TRUE(t.done());
  EXPECT_EQ(t.take().value(), 2u);
  EXPECT_TRUE(bytes_equal(f.nic.tx[0], make_bytes({9, 8})));
}

TEST_F(PolledNetDevice, HardErrorPropagates) {
  fixture f;
  f.push_rx(make_bytes(100, 0)); // larger than the 64-byte buffer
  auto t = echo_one(f.ref);
  t.resume();
  ASSERT_TRUE(t.done());
  EXPECT_EQ(t.take().error(), reloco::error::out_of_range);
}

TEST_F(PolledNetDevice, SecondReceiverIsBusy) {
  fixture f;
  auto a = echo_one(f.ref);
  auto b = echo_one(f.ref);
  a.resume();
  b.resume();
  EXPECT_FALSE(a.done());
  ASSERT_TRUE(b.done());
  EXPECT_EQ(b.take().error(), reloco::error::busy);
}

TEST_F(PolledNetDevice, DroppingParkedTaskUnparks) {
  fixture f;
  {
    auto t = echo_one(f.ref);
    t.resume();
    EXPECT_TRUE(f.dev.receive_pending());
  }
  EXPECT_FALSE(f.dev.receive_pending());
  EXPECT_EQ(f.dev.poll(), 0u);
}

} // namespace
