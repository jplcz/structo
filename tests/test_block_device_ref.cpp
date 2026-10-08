// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/block_device_ref.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <vector>

#include <reloco/lifetime.hpp>

// Test fixtures index raw buffers freely; bounds are checked by the assertions.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

namespace {

using namespace structo;
using namespace structo::hw;

constexpr std::size_t kBlockSize = 512;
constexpr std::uint64_t kBlockCount = 8;

// --------------------------------------------------------------------
// A fake block device: a fixed in-memory byte array, read/write
// capable, with a settable `available`/`read_only` flag and an
// injectable read-error. Implements all optional probes.
// --------------------------------------------------------------------
struct fake_disk {
  std::vector<std::byte> mem = std::vector<std::byte>(kBlockSize * kBlockCount);
  bool available = true;
  bool read_only = false;
  bool next_read_fails = false;
};

} // namespace

template <> struct structo::hw::block_device_traits<fake_disk> {
  static std::size_t block_size(fake_disk &) noexcept { return kBlockSize; }
  static std::uint64_t block_count(fake_disk &) noexcept { return kBlockCount; }

  static reloco::result<void> try_read_blocks(fake_disk &b, std::uint64_t lba, reloco::span<std::byte> dst) noexcept {
    if (b.next_read_fails)
      return reloco::unexpected(reloco::error::io_error);
    std::memcpy(dst.data(), b.mem.data() + lba * kBlockSize, dst.size());
    return {};
  }

  static reloco::result<void> try_write_blocks(fake_disk &b, std::uint64_t lba,
                                               reloco::span<const std::byte> src) noexcept {
    std::memcpy(b.mem.data() + lba * kBlockSize, src.data(), src.size());
    return {};
  }

  static reloco::result<void> try_flush(fake_disk &) noexcept { return {}; }
  static bool is_read_only(fake_disk &b) noexcept { return b.read_only; }
  static bool is_available(fake_disk &b) noexcept { return b.available; }
};

namespace {

// --------------------------------------------------------------------
// A minimal, read-only backend with NO optional members, exercising
// every optional-trait fallback.
// --------------------------------------------------------------------
struct bare_disk {
  std::vector<std::byte> mem = std::vector<std::byte>(kBlockSize * kBlockCount);
};

} // namespace

template <> struct structo::hw::block_device_traits<bare_disk> {
  static std::size_t block_size(bare_disk &) noexcept { return kBlockSize; }
  static std::uint64_t block_count(bare_disk &) noexcept { return kBlockCount; }
  static reloco::result<void> try_read_blocks(bare_disk &b, std::uint64_t lba, reloco::span<std::byte> dst) noexcept {
    std::memcpy(dst.data(), b.mem.data() + lba * kBlockSize, dst.size());
    return {};
  }
};

namespace {

TEST(BlockDeviceRefTest, UnboundRefFailsEveryOperation) {
  block_device_ref unbound;
  EXPECT_FALSE(static_cast<bool>(unbound));
  EXPECT_FALSE(unbound.is_available());
  EXPECT_TRUE(unbound.is_read_only());
  EXPECT_EQ(unbound.block_size(), 0u);
  EXPECT_EQ(unbound.block_count(), 0u);
  EXPECT_EQ(unbound.size_bytes(), 0u);

  std::array<std::byte, kBlockSize> buf{};
  auto r = unbound.try_read_blocks(0, buf);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), error::unsupported_operation);

  auto w = unbound.try_write_blocks(0, buf);
  ASSERT_FALSE(w.has_value());
  EXPECT_EQ(w.error(), error::unsupported_operation);

  auto f = unbound.try_flush();
  ASSERT_FALSE(f.has_value());
  EXPECT_EQ(f.error(), error::unsupported_operation);
}

TEST(BlockDeviceRefTest, BoundRefReportsSizesAndAvailability) {
  fake_disk dev;
  block_device_ref ref(dev);
  EXPECT_TRUE(static_cast<bool>(ref));
  EXPECT_EQ(ref.block_size(), kBlockSize);
  EXPECT_EQ(ref.block_count(), kBlockCount);
  EXPECT_EQ(ref.size_bytes(), kBlockSize * kBlockCount);

  dev.available = true;
  EXPECT_TRUE(ref.is_available());
  dev.available = false;
  EXPECT_FALSE(ref.is_available());
}

TEST(BlockDeviceRefTest, IsAvailableAssumedTrueWithoutOptionalProbe) {
  bare_disk dev;
  block_device_ref ref(dev);
  EXPECT_TRUE(ref.is_available());
}

TEST(BlockDeviceRefTest, TryWriteThenReadBackSucceeds) {
  fake_disk dev;
  block_device_ref ref(dev);

  std::vector<std::byte> pattern(kBlockSize * 2);
  for (std::size_t i = 0; i < pattern.size(); ++i)
    pattern[i] = std::byte(i & 0xFF);

  auto w = ref.try_write_blocks(1, pattern);
  ASSERT_TRUE(w.has_value());

  std::vector<std::byte> readback(kBlockSize * 2);
  auto r = ref.try_read_blocks(1, readback);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(readback, pattern);
}

TEST(BlockDeviceRefTest, TryReadBlocksRejectsMisalignedSize) {
  fake_disk dev;
  block_device_ref ref(dev);

  std::array<std::byte, kBlockSize - 1> buf{};
  auto r = ref.try_read_blocks(0, buf);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), error::invalid_argument);
}

TEST(BlockDeviceRefTest, TryReadBlocksOutOfRangeFails) {
  fake_disk dev;
  block_device_ref ref(dev);

  std::array<std::byte, kBlockSize * 2> buf{};
  auto r = ref.try_read_blocks(kBlockCount - 1, buf);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), error::out_of_range);
}

TEST(BlockDeviceRefTest, TryWriteBlocksOutOfRangeFails) {
  fake_disk dev;
  block_device_ref ref(dev);

  std::array<std::byte, kBlockSize> buf{};
  auto w = ref.try_write_blocks(kBlockCount, buf);
  ASSERT_FALSE(w.has_value());
  EXPECT_EQ(w.error(), error::out_of_range);
}

TEST(BlockDeviceRefTest, TryReadBlocksPropagatesBackendError) {
  fake_disk dev;
  dev.next_read_fails = true;
  block_device_ref ref(dev);

  std::array<std::byte, kBlockSize> buf{};
  auto r = ref.try_read_blocks(0, buf);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), error::io_error);
}

TEST(BlockDeviceRefTest, TryWriteBlocksFailsWithPermissionDeniedWhenReadOnly) {
  fake_disk dev;
  dev.read_only = true;
  block_device_ref ref(dev);
  EXPECT_TRUE(ref.is_read_only());

  std::array<std::byte, kBlockSize> buf{};
  auto w = ref.try_write_blocks(0, buf);
  ASSERT_FALSE(w.has_value());
  EXPECT_EQ(w.error(), error::permission_denied);
}

TEST(BlockDeviceRefTest, WriteSupportAbsentReportsReadOnlyAndUnsupported) {
  bare_disk dev;
  block_device_ref ref(dev);
  EXPECT_TRUE(ref.is_read_only());

  std::array<std::byte, kBlockSize> buf{};
  auto w = ref.try_write_blocks(0, buf);
  ASSERT_FALSE(w.has_value());
  EXPECT_EQ(w.error(), error::unsupported_operation);
}

TEST(BlockDeviceRefTest, TryFlushFallsBackToSuccessWithoutOptionalProbe) {
  bare_disk dev;
  block_device_ref ref(dev);
  auto f = ref.try_flush();
  EXPECT_TRUE(f.has_value());
}

TEST(BlockDeviceRefTest, TryReadBytesHandlesUnalignedOffsetAndSize) {
  fake_disk dev;
  block_device_ref ref(dev);

  // Fill the whole device with a recognizable byte pattern.
  std::vector<std::byte> pattern(kBlockSize * kBlockCount);
  for (std::size_t i = 0; i < pattern.size(); ++i)
    pattern[i] = std::byte(i & 0xFF);
  ASSERT_TRUE(ref.try_write_blocks(0, pattern).has_value());

  // Read a range that starts and ends mid-block, spanning 3 blocks.
  const std::uint64_t offset = kBlockSize - 10;
  const std::size_t len = kBlockSize + 20;
  std::vector<std::byte> dst(len);
  std::vector<std::byte> scratch(kBlockSize);
  auto r = ref.try_read_bytes(offset, dst, scratch);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(std::vector<std::byte>(pattern.begin() + offset, pattern.begin() + offset + len), dst);
}

TEST(BlockDeviceRefTest, TryReadBytesFailsWhenScratchTooSmall) {
  fake_disk dev;
  block_device_ref ref(dev);

  std::vector<std::byte> dst(kBlockSize + 1);
  std::vector<std::byte> scratch(kBlockSize - 1);
  auto r = ref.try_read_bytes(0, dst, scratch);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), error::invalid_argument);
}

TEST(BlockDeviceRefTest, TryReadBytesOutOfRangePropagatesError) {
  fake_disk dev;
  block_device_ref ref(dev);

  std::vector<std::byte> dst(kBlockSize);
  std::vector<std::byte> scratch(kBlockSize);
  auto r = ref.try_read_bytes(kBlockSize * kBlockCount, dst, scratch);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), error::out_of_range);
}

} // namespace

RELOCO_END_UNSAFE_BUFFER_USAGE