// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/block_cache_ref.hpp>

#include <reloco/array.hpp>

#include <cstddef>
#include <cstring>
#include <vector>

// Test fixtures index raw buffers freely; bounds are checked by the assertions.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

namespace {

using namespace structo::hw;
using reloco::error;
using reloco::span;

constexpr std::size_t kBs = 64;
constexpr std::uint64_t kBlocks = 32;

// Counts device traffic so tests can prove what the cache absorbed.
struct counting_disk {
  std::vector<std::byte> mem = std::vector<std::byte>(kBs * kBlocks);
  int reads = 0;
  int writes = 0;
  int flushes = 0;
  bool read_only = false;
  bool fail_writes = false;

  counting_disk() {
    for (std::size_t i = 0; i < mem.size(); ++i)
      mem[i] = static_cast<std::byte>(i * 7 + i / kBs);
  }
};

} // namespace

template <> struct structo::hw::block_device_traits<counting_disk> {
  static std::size_t block_size(counting_disk &) noexcept { return kBs; }
  static std::uint64_t block_count(counting_disk &) noexcept { return kBlocks; }
  static reloco::result<void> try_read_blocks(counting_disk &d, std::uint64_t lba, span<std::byte> dst) noexcept {
    ++d.reads;
    std::memcpy(dst.data(), d.mem.data() + lba * kBs, dst.size());
    return {};
  }
  static reloco::result<void> try_write_blocks(counting_disk &d, std::uint64_t lba, span<const std::byte> src) noexcept {
    if (d.fail_writes)
      return reloco::unexpected(error::io_error);
    ++d.writes;
    std::memcpy(d.mem.data() + lba * kBs, src.data(), src.size());
    return {};
  }
  static reloco::result<void> try_flush(counting_disk &d) noexcept {
    ++d.flushes;
    return {};
  }
  static bool is_read_only(counting_disk &d) noexcept { return d.read_only; }
};

namespace {

// 16 header + 4 slots * (24 + 64).
constexpr std::size_t kStorage = 16 + 4 * (24 + kBs);

class BlockCacheTest : public ::testing::Test {
protected:
  counting_disk disk;
  reloco::array<std::byte, kStorage> storage{};
  block_device_ref dev{disk};

  block_cache_ref make() {
    auto c = block_cache_ref::try_create(dev, span<std::byte>(storage));
    EXPECT_TRUE(c);
    return *c;
  }
  std::byte expected(std::uint64_t lba, std::size_t i = 0) const { return disk.mem[lba * kBs + i]; }
};

TEST_F(BlockCacheTest, CreateValidatesArguments) {
  EXPECT_EQ(block_cache_ref::try_create(block_device_ref{}, span<std::byte>(storage)).error(),
            error::unsupported_operation);
  EXPECT_EQ(block_cache_ref::try_create(dev, span<std::byte>(storage).first(16 + 24 + kBs - 1)).error(),
            error::invalid_argument);
  auto c = make();
  EXPECT_EQ(c.slot_count(), 4u);
  EXPECT_EQ(c.block_size(), kBs);
  EXPECT_EQ(c.block_count(), kBlocks);
  EXPECT_FALSE(block_cache_ref{});
}

TEST_F(BlockCacheTest, ReadsHitTheCache) {
  auto c = make();
  reloco::array<std::byte, kBs> buf{};
  ASSERT_TRUE(c.try_read_blocks(3, span<std::byte>(buf)));
  EXPECT_EQ(buf[5], expected(3, 5));
  ASSERT_TRUE(c.try_read_blocks(3, span<std::byte>(buf)));
  EXPECT_EQ(disk.reads, 1);
}

TEST_F(BlockCacheTest, MultiBlockReadAndLruEviction) {
  auto c = make();
  reloco::array<std::byte, kBs * 4> big{};
  ASSERT_TRUE(c.try_read_blocks(0, span<std::byte>(big)));   // fills all 4 slots
  EXPECT_EQ(big[kBs * 2], expected(2));
  EXPECT_EQ(disk.reads, 4);

  reloco::array<std::byte, kBs> one{};
  ASSERT_TRUE(c.try_read_blocks(0, span<std::byte>(one)));    // touch 0: now 1 is the LRU
  ASSERT_TRUE(c.try_read_blocks(10, span<std::byte>(one)));   // evicts 1
  EXPECT_EQ(disk.reads, 5);
  ASSERT_TRUE(c.try_read_blocks(0, span<std::byte>(one)));    // still cached
  EXPECT_EQ(disk.reads, 5);
  ASSERT_TRUE(c.try_read_blocks(1, span<std::byte>(one)));    // was evicted
  EXPECT_EQ(disk.reads, 6);
}

TEST_F(BlockCacheTest, WritesAreDeferredUntilFlush) {
  auto c = make();
  reloco::array<std::byte, kBs> src{};
  for (auto &b : src)
    b = std::byte{0xAB};
  ASSERT_TRUE(c.try_write_blocks(7, span<const std::byte>(src)));
  EXPECT_EQ(disk.writes, 0);
  EXPECT_EQ(disk.reads, 0);   // a whole-block write needs no fill
  EXPECT_EQ(c.dirty_count(), 1u);

  reloco::array<std::byte, kBs> back{};
  ASSERT_TRUE(c.try_read_blocks(7, span<std::byte>(back)));
  EXPECT_EQ(back[0], std::byte{0xAB});
  EXPECT_EQ(disk.reads, 0);

  EXPECT_EQ(c.try_invalidate().error(), error::busy);
  ASSERT_TRUE(c.try_flush());
  EXPECT_EQ(disk.writes, 1);
  EXPECT_EQ(disk.flushes, 1);
  EXPECT_EQ(c.dirty_count(), 0u);
  EXPECT_EQ(disk.mem[7 * kBs], std::byte{0xAB});
  EXPECT_TRUE(c.try_invalidate());
}

TEST_F(BlockCacheTest, EvictingDirtyBlockWritesItBack) {
  auto c = make();
  reloco::array<std::byte, kBs> src{};
  src[0] = std::byte{0x11};
  ASSERT_TRUE(c.try_write_blocks(0, span<const std::byte>(src)));
  reloco::array<std::byte, kBs> sink{};
  for (std::uint64_t l = 1; l <= 4; ++l)   // 4 more blocks push block 0 out
    ASSERT_TRUE(c.try_read_blocks(l, span<std::byte>(sink)));
  EXPECT_EQ(disk.writes, 1);
  EXPECT_EQ(disk.mem[0], std::byte{0x11});
}

TEST_F(BlockCacheTest, FailedWriteBackKeepsBlockDirty) {
  auto c = make();
  reloco::array<std::byte, kBs> src{};
  ASSERT_TRUE(c.try_write_blocks(0, span<const std::byte>(src)));
  disk.fail_writes = true;
  EXPECT_EQ(c.try_flush().error(), error::io_error);
  EXPECT_EQ(c.dirty_count(), 1u);
  disk.fail_writes = false;
  EXPECT_TRUE(c.try_flush());
  EXPECT_EQ(c.dirty_count(), 0u);
}

TEST_F(BlockCacheTest, LargeRequestsBypassButStayCoherent) {
  auto c = make();
  reloco::array<std::byte, kBs> src{};
  src[0] = std::byte{0x5A};
  ASSERT_TRUE(c.try_write_blocks(2, span<const std::byte>(src)));   // dirty in cache

  std::vector<std::byte> big(kBs * 6);   // > 4 slots
  ASSERT_TRUE(c.try_read_blocks(0, span<std::byte>(big)));
  EXPECT_EQ(big[2 * kBs], std::byte{0x5A});   // saw the dirty data
  EXPECT_EQ(c.dirty_count(), 0u);

  std::vector<std::byte> fill(kBs * 6, std::byte{0x77});
  ASSERT_TRUE(c.try_write_blocks(0, span<const std::byte>(fill)));   // straight to the device
  reloco::array<std::byte, kBs> one{};
  ASSERT_TRUE(c.try_read_blocks(2, span<std::byte>(one)));           // must not return stale cache
  EXPECT_EQ(one[0], std::byte{0x77});
}

TEST_F(BlockCacheTest, ByteReadsSpanBlocks) {
  auto c = make();
  reloco::array<std::byte, 100> out{};
  ASSERT_TRUE(c.try_read_bytes(kBs - 10, span<std::byte>(out)));
  for (std::size_t i = 0; i < out.size(); ++i)
    ASSERT_EQ(out[i], disk.mem[kBs - 10 + i]) << i;
  EXPECT_EQ(c.try_read_bytes(kBs * kBlocks - 1, span<std::byte>(out)).error(), error::out_of_range);
}

TEST_F(BlockCacheTest, ErrorsAndReadOnly) {
  auto c = make();
  reloco::array<std::byte, kBs - 1> odd{};
  EXPECT_EQ(c.try_read_blocks(0, span<std::byte>(odd)).error(), error::invalid_argument);
  reloco::array<std::byte, kBs> one{};
  EXPECT_EQ(c.try_read_blocks(kBlocks, span<std::byte>(one)).error(), error::out_of_range);
  disk.read_only = true;
  EXPECT_EQ(c.try_write_blocks(0, span<const std::byte>(one)).error(), error::permission_denied);
  EXPECT_TRUE(c.is_read_only());
}

TEST_F(BlockCacheTest, ActsAsBlockDevice) {
  auto c = make();
  block_device_ref cached(c);
  reloco::array<std::byte, kBs> one{};
  ASSERT_TRUE(cached.try_read_blocks(9, span<std::byte>(one)));
  ASSERT_TRUE(cached.try_read_blocks(9, span<std::byte>(one)));
  EXPECT_EQ(disk.reads, 1);
  EXPECT_EQ(cached.block_size(), kBs);
  ASSERT_TRUE(cached.try_flush());
}

} // namespace

RELOCO_END_UNSAFE_BUFFER_USAGE
