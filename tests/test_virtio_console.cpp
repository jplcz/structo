// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "virtio_mmio_fixture.hpp"

#include <structo/virtio/virtio_console.hpp>

namespace {

using namespace virtio_test;

struct capture_sink {
  reloco::result<void> try_write(reloco::span<const std::byte> bytes) noexcept {
    if (fail)
      return reloco::unexpected(reloco::error::io_error);
    for (auto b : bytes)
      if (len < data.size())
        data[len++] = b;
    return {};
  }
  reloco::array<std::byte, 1024> data{};
  std::size_t len = 0;
  bool fail = false;
};

using console_t = virtio_console_function<guest_space, capture_sink, 8>;

struct sink_holder {
  capture_sink sink;
};

class VirtioConsoleTest : private sink_holder, public mmio_fixture<console_t, 2> {
protected:
  VirtioConsoleTest() : mmio_fixture<console_t, 2>(sink) {}

  static reloco::span<const std::byte> bytes(const char *s, std::size_t n) {
    return reloco::span<const std::byte>(reinterpret_cast<const std::byte *>(s), n);
  }
  capture_sink &out() { return sink; }
};

TEST_F(VirtioConsoleTest, IdentityAndConfig) {
  EXPECT_EQ(rd(reg::device_id), 3u);
  wr(reg::device_features_sel, 0);
  EXPECT_EQ(rd(reg::device_features) & 1u, 0u); // no SIZE without a size
  EXPECT_EQ(dev_.size(), reg::config + console::config_size);
  EXPECT_EQ(rd(reg::config + 4), 1u); // max_nr_ports
}

TEST_F(VirtioConsoleTest, SizeFeatureAndConfig) {
  fn_.set_size(120, 40);
  wr(reg::device_features_sel, 0);
  EXPECT_EQ(rd(reg::device_features) & 1u, 1u);
  const std::uint32_t cr = rd(reg::config);
  EXPECT_EQ(cr & 0xffffu, 120u);
  EXPECT_EQ(cr >> 16, 40u);
}

TEST_F(VirtioConsoleTest, GuestOutputReachesSink) {
  bring_up();
  reloco::array<char, 300> text{};
  for (std::size_t i = 0; i < text.size(); ++i)
    text[i] = static_cast<char>('a' + i % 26);
  put(0, text.data(), text.size());
  post_out(1, 0, text.size());
  kick(1);
  EXPECT_EQ(reap(1), 0);
  ASSERT_EQ(out().len, 300u);
  for (std::size_t i = 0; i < text.size(); ++i)
    ASSERT_EQ(static_cast<char>(out().data[i]), text[i]) << i;
  EXPECT_EQ(fn_.stats().output_bytes, 300u);
}

TEST_F(VirtioConsoleTest, SinkFailureIsCountedNotFatal) {
  bring_up();
  out().fail = true;
  reloco::array<char, 10> text{};
  put(0, text.data(), text.size());
  post_out(1, 0, 10);
  kick(1);
  EXPECT_EQ(reap(1), 0);
  EXPECT_EQ(fn_.stats().output_dropped, 10u);
}

TEST_F(VirtioConsoleTest, InputIsDeliveredIntoGuestBuffer) {
  bring_up();
  post_in(0, 0, 16);
  EXPECT_EQ(fn_.try_input(bytes("hello", 5)), 5u);
  ASSERT_TRUE(dev_.try_kick(0).has_value());
  EXPECT_EQ(reap(0), 5);
  char got[5] = {};
  get(0, got, 5);
  EXPECT_EQ(std::string(got, 5), "hello");
  EXPECT_EQ(fn_.pending_input(), 0u);
  EXPECT_TRUE(dev_.irq_asserted());
}

TEST_F(VirtioConsoleTest, InputWaitsForBuffersAndSplitsAcrossThem) {
  bring_up();
  EXPECT_EQ(fn_.try_input(bytes("0123456789", 8)), 8u); // capacity is 8
  ASSERT_TRUE(dev_.try_kick(0).has_value());
  EXPECT_EQ(fn_.pending_input(), 8u); // no buffers yet
  post_in(0, 0, 3, 1);
  post_in(0, 0x100, 3, 2);
  post_in(0, 0x200, 3, 3);
  kick(0);
  EXPECT_EQ(reap(0), 3);
  EXPECT_EQ(reap(0), 3);
  EXPECT_EQ(reap(0), 2);
  EXPECT_EQ(fn_.pending_input(), 0u);
  char a[3], b[3], c[2];
  get(0, a, 3);
  get(0x100, b, 3);
  get(0x200, c, 2);
  EXPECT_EQ(std::string(a, 3) + std::string(b, 3) + std::string(c, 2), "01234567");
}

TEST_F(VirtioConsoleTest, FifoBackpressureAndWraparound) {
  bring_up();
  EXPECT_EQ(fn_.try_input(bytes("abcdefghijklmnop", 16)), 8u);
  EXPECT_EQ(fn_.try_input(bytes("z", 1)), 0u); // full
  post_in(0, 0, 6);
  ASSERT_TRUE(dev_.try_kick(0).has_value());
  EXPECT_EQ(reap(0), 6);
  EXPECT_EQ(fn_.try_input(bytes("QRSTUV", 6)), 6u); // wraps inside the FIFO
  post_in(0, 0x100, 16);
  ASSERT_TRUE(dev_.try_kick(0).has_value());
  EXPECT_EQ(reap(0), 8);
  char got[8];
  get(0x100, got, 8);
  EXPECT_EQ(std::string(got, 8), "ghQRSTUV");
}

} // namespace
