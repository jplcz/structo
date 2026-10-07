// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "virtio_mmio_fixture.hpp"

#include <structo/virtio/virtio_rng.hpp>

namespace {

using namespace virtio_test;

// Deterministic source: byte i of the stream is (i & 0xff); can be told to fail after N bytes.
struct counting_source {
  reloco::result<void> try_fill(reloco::span<std::byte> dst) noexcept {
    if (fail_after != ~std::uint64_t{0} && produced + dst.size() > fail_after)
      return reloco::unexpected(reloco::error::try_again);
    for (auto &b : dst)
      b = static_cast<std::byte>(produced++ & 0xff);
    return {};
  }
  std::uint64_t produced = 0;
  std::uint64_t fail_after = ~std::uint64_t{0};
};

using rng_t = virtio_rng_function<guest_space, counting_source>;

struct source_holder {
  counting_source src;
};

class VirtioRngTest : private source_holder, public mmio_fixture<rng_t, 1> {
protected:
  VirtioRngTest() : mmio_fixture<rng_t, 1>(src) {}
  counting_source &source() { return src; }
};

TEST_F(VirtioRngTest, Identity) {
  EXPECT_EQ(rd(reg::device_id), 4u);
  wr(reg::device_features_sel, 0);
  EXPECT_EQ(rd(reg::device_features) & 0x0fffffffu, 0u); // only transport bits (EVENT_IDX, INDIRECT)
  EXPECT_EQ(dev_.size(), reg::config);
}

TEST_F(VirtioRngTest, FillsBufferFromSource) {
  bring_up();
  post_in(0, 0, 64);
  kick(0);
  EXPECT_EQ(reap(0), 64);
  reloco::array<std::byte, 64> got{};
  get(0, got.data(), got.size());
  for (std::size_t i = 0; i < got.size(); ++i)
    ASSERT_EQ(got[i], static_cast<std::byte>(i)) << i;
  EXPECT_TRUE(dev_.irq_asserted());
  EXPECT_EQ(fn_.bytes_served(), 64u);
}

TEST_F(VirtioRngTest, LargeBufferCrossesInternalChunks) {
  bring_up();
  post_in(0, 0, 600);
  kick(0);
  EXPECT_EQ(reap(0), 600);
  reloco::array<std::byte, 600> got{};
  get(0, got.data(), got.size());
  for (std::size_t i = 0; i < got.size(); ++i)
    ASSERT_EQ(got[i], static_cast<std::byte>(i & 0xff)) << i;
}

TEST_F(VirtioRngTest, RequestIsCapped) {
  bring_up();
  post_in(0, 0, 8192);
  kick(0);
  EXPECT_EQ(reap(0), static_cast<std::int64_t>(rng::max_request_bytes));
}

TEST_F(VirtioRngTest, SourceFailureReturnsBytesProducedSoFar) {
  bring_up();
  source().fail_after = 300; // first 256-byte chunk fits, the second does not
  post_in(0, 0, 600);
  kick(0);
  EXPECT_EQ(reap(0), 256);
}

TEST_F(VirtioRngTest, SeveralBuffersOneKick) {
  bring_up();
  post_in(0, 0, 16, 1);
  post_in(0, 0x100, 16, 2);
  post_in(0, 0x200, 16, 3);
  kick(0);
  EXPECT_EQ(reap(0), 16);
  EXPECT_EQ(reap(0), 16);
  EXPECT_EQ(reap(0), 16);
  EXPECT_EQ(reap(0), -1);
  EXPECT_EQ(fn_.bytes_served(), 48u);
}

} // namespace
