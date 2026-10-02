// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/uart_ref.hpp>

#include <array>
#include <deque>
#include <string>

namespace {

using namespace structo;
using namespace structo::hw;

// --------------------------------------------------------------------
// A fake, fully-featured UART backend: in-memory TX/RX FIFOs plus a
// settable `tx_busy` flag to exercise the timeout path. Implements the
// optional current_config readback.
// --------------------------------------------------------------------
struct fake_uart {
  uart_config cfg{};
  std::deque<std::uint8_t> rx;
  std::deque<std::uint8_t> tx;
  bool tx_busy = false;
};

} // namespace

template <> struct structo::hw::uart_traits<fake_uart> {
  static reloco::result<void> configure(fake_uart &b, const uart_config &cfg) noexcept {
    b.cfg = cfg;
    return {};
  }
  static reloco::result<uart_config> current_config(fake_uart &b) noexcept { return b.cfg; }
  static reloco::result<bool> tx_ready(fake_uart &b) noexcept { return !b.tx_busy; }
  static reloco::result<bool> rx_ready(fake_uart &b) noexcept { return !b.rx.empty(); }
  static reloco::result<void> try_put_byte(fake_uart &b, std::uint8_t v) noexcept {
    if (b.tx_busy)
      return reloco::unexpected(reloco::error::try_again);
    b.tx.push_back(v);
    return {};
  }
  static reloco::result<std::uint8_t> try_get_byte(fake_uart &b) noexcept {
    if (b.rx.empty())
      return reloco::unexpected(reloco::error::try_again);
    auto v = b.rx.front();
    b.rx.pop_front();
    return v;
  }
};

namespace {

// --------------------------------------------------------------------
// A minimal backend with NO current_config, exercising the
// optional-trait fallback (error::unsupported_operation).
// --------------------------------------------------------------------
struct bare_uart {
  bool ready = true;
};

} // namespace

template <> struct structo::hw::uart_traits<bare_uart> {
  static reloco::result<void> configure(bare_uart &, const uart_config &) noexcept { return {}; }
  static reloco::result<bool> tx_ready(bare_uart &b) noexcept { return b.ready; }
  static reloco::result<bool> rx_ready(bare_uart &) noexcept { return false; }
  static reloco::result<void> try_put_byte(bare_uart &, std::uint8_t) noexcept { return {}; }
  static reloco::result<std::uint8_t> try_get_byte(bare_uart &) noexcept {
    return reloco::unexpected(reloco::error::try_again);
  }
};

namespace {

TEST(UartRefTest, UnboundRefFailsEveryOperation) {
  uart_ref unbound;
  EXPECT_FALSE(static_cast<bool>(unbound));

  EXPECT_FALSE(unbound.configure(uart_config_115200_8n1).has_value());
  EXPECT_FALSE(unbound.current_config().has_value());
  EXPECT_FALSE(unbound.tx_ready().has_value());
  EXPECT_FALSE(unbound.rx_ready().has_value());
  EXPECT_FALSE(unbound.try_put_byte('x').has_value());
  EXPECT_FALSE(unbound.try_get_byte().has_value());

  auto r = unbound.put_byte('x');
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), error::unsupported_operation);
}

TEST(UartRefTest, BoundRefReportsTrue) {
  fake_uart dev;
  uart_ref ref(dev);
  EXPECT_TRUE(static_cast<bool>(ref));
}

TEST(UartRefTest, ConfigureAndReadBackCurrentConfig) {
  fake_uart dev;
  uart_ref ref(dev);

  uart_config cfg;
  cfg.baud_rate = 9600;
  cfg.data_bits = uart_data_bits::seven;
  cfg.parity = uart_parity::even;
  cfg.stop_bits = uart_stop_bits::two;
  cfg.flow_control = uart_flow_control::rts_cts;

  ASSERT_TRUE(ref.configure(cfg).has_value());
  auto got = ref.current_config();
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got.value(), cfg);
  EXPECT_TRUE(got.value() == cfg);
  EXPECT_FALSE(got.value() != cfg);
}

TEST(UartRefTest, DefaultConfigIs115200_8N1NoFlowControl) {
  EXPECT_EQ(uart_config_115200_8n1.baud_rate, 115200u);
  EXPECT_EQ(uart_config_115200_8n1.data_bits, uart_data_bits::eight);
  EXPECT_EQ(uart_config_115200_8n1.parity, uart_parity::none);
  EXPECT_EQ(uart_config_115200_8n1.stop_bits, uart_stop_bits::one);
  EXPECT_EQ(uart_config_115200_8n1.flow_control, uart_flow_control::none);
}

TEST(UartRefTest, PutByteAndGetByteRoundTrip) {
  fake_uart dev;
  uart_ref ref(dev);

  ASSERT_TRUE(ref.put_byte(std::uint8_t('A')).has_value());
  ASSERT_FALSE(dev.tx.empty());
  EXPECT_EQ(dev.tx.back(), 'A');

  dev.rx.push_back('Z');
  auto b = ref.get_byte();
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(b.value(), 'Z');
}

TEST(UartRefTest, PutByteTimesOutWhenNeverReady) {
  fake_uart dev;
  uart_ref ref(dev);
  dev.tx_busy = true;

  auto r = ref.put_byte(std::uint8_t('Q'), 10);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), error::timed_out);
}

TEST(UartRefTest, GetByteTimesOutWhenNeverReady) {
  fake_uart dev;
  uart_ref ref(dev);

  auto r = ref.get_byte(10);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), error::timed_out);
}

TEST(UartRefTest, WriteSendsEveryByteInOrder) {
  fake_uart dev;
  uart_ref ref(dev);

  std::array<std::uint8_t, 3> data{'a', 'b', 'c'};
  ASSERT_TRUE(ref.write(span<const std::uint8_t>(data.data(), data.size())).has_value());
  ASSERT_EQ(dev.tx.size(), 3u);
  EXPECT_EQ(dev.tx[0], 'a');
  EXPECT_EQ(dev.tx[1], 'b');
  EXPECT_EQ(dev.tx[2], 'c');
}

TEST(UartRefTest, WriteStopsAtFirstFailure) {
  fake_uart dev;
  uart_ref ref(dev);
  dev.tx_busy = true;

  std::array<std::uint8_t, 3> data{'a', 'b', 'c'};
  auto r = ref.write(span<const std::uint8_t>(data.data(), data.size()), 5);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), error::timed_out);
  EXPECT_TRUE(dev.tx.empty());
}

TEST(UartRefTest, WriteStringTranslatesNewlinesToCrlf) {
  fake_uart dev;
  uart_ref ref(dev);

  ASSERT_TRUE(ref.write_string("hi\n").has_value());
  std::string got;
  for (auto c : dev.tx)
    got.push_back(static_cast<char>(c));
  EXPECT_EQ(got, "hi\r\n");
}

TEST(UartRefTest, WriteStringWithoutNewlinesIsUnchanged) {
  fake_uart dev;
  uart_ref ref(dev);

  ASSERT_TRUE(ref.write_string("boot").has_value());
  std::string got;
  for (auto c : dev.tx)
    got.push_back(static_cast<char>(c));
  EXPECT_EQ(got, "boot");
}

TEST(UartRefTest, ReadAvailableDrainsOnlyWhatIsReady) {
  fake_uart dev;
  uart_ref ref(dev);
  dev.rx.push_back('x');
  dev.rx.push_back('y');

  std::array<std::uint8_t, 8> dst{};
  auto n = ref.read_available(span<std::uint8_t>(dst.data(), dst.size()));
  ASSERT_TRUE(n.has_value());
  EXPECT_EQ(n.value(), 2u);
  EXPECT_EQ(dst[0], 'x');
  EXPECT_EQ(dst[1], 'y');
}

TEST(UartRefTest, ReadAvailableReturnsZeroWhenNothingReady) {
  fake_uart dev;
  uart_ref ref(dev);

  std::array<std::uint8_t, 8> dst{};
  auto n = ref.read_available(span<std::uint8_t>(dst.data(), dst.size()));
  ASSERT_TRUE(n.has_value());
  EXPECT_EQ(n.value(), 0u);
}

TEST(UartRefTest, ReadAvailableStopsEarlyWhenDestinationFillsUp) {
  fake_uart dev;
  uart_ref ref(dev);
  dev.rx.push_back('1');
  dev.rx.push_back('2');
  dev.rx.push_back('3');

  std::array<std::uint8_t, 2> dst{};
  auto n = ref.read_available(span<std::uint8_t>(dst.data(), dst.size()));
  ASSERT_TRUE(n.has_value());
  EXPECT_EQ(n.value(), 2u);
  EXPECT_EQ(dev.rx.size(), 1u); // one byte still buffered in the backend
}

TEST(UartRefTest, CurrentConfigUnsupportedOnBareBackend) {
  bare_uart bare;
  uart_ref ref(bare);

  auto cfg = ref.current_config();
  ASSERT_FALSE(cfg.has_value());
  EXPECT_EQ(cfg.error(), error::unsupported_operation);
}

TEST(UartRefTest, TryPutByteAndTryGetByteReportTryAgainWhenNotReady) {
  fake_uart dev;
  uart_ref ref(dev);
  dev.tx_busy = true;

  auto w = ref.try_put_byte(std::uint8_t('x'));
  ASSERT_FALSE(w.has_value());
  EXPECT_EQ(w.error(), error::try_again);

  auto r = ref.try_get_byte();
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), error::try_again);
}

} // namespace
