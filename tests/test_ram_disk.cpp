// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/ram_disk.hpp>

#include <array>
#include <cstring>

namespace {

using namespace structo;
using namespace structo::hw;

TEST(RamDiskTest, ReadWriteRoundTrip) {
  std::array<std::byte, 512 * 4> storage{};
  ram_disk disk(storage);
  block_device_ref ref(disk);

  EXPECT_TRUE(static_cast<bool>(ref));
  EXPECT_EQ(ref.block_size(), 512u);
  EXPECT_EQ(ref.block_count(), 4u);
  EXPECT_FALSE(ref.is_read_only());

  std::array<std::byte, 512> pattern{};
  pattern.fill(std::byte{0xAB});
  ASSERT_TRUE(ref.try_write_blocks(1, pattern).has_value());

  std::array<std::byte, 512> readback{};
  ASSERT_TRUE(ref.try_read_blocks(1, readback).has_value());
  EXPECT_EQ(readback, pattern);
}

TEST(RamDiskTest, BlockSizeOneIsByteAddressable) {
  std::array<std::byte, 7> storage{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}, std::byte{'d'},
                                   std::byte{'e'}, std::byte{'f'}, std::byte{'g'}};
  ram_disk disk(storage, 1);
  block_device_ref ref(disk);

  EXPECT_EQ(ref.block_count(), 7u);
  std::array<std::byte, 7> readback{};
  ASSERT_TRUE(ref.try_read_blocks(0, readback).has_value());
  EXPECT_EQ(readback, storage);
}

TEST(RamDiskTest, PartialTrailingBlockExcludedFromBlockCount) {
  std::array<std::byte, 1000> storage{};
  ram_disk disk(storage, 512);
  // 1000 / 512 == 1 whole block; the trailing 488 bytes are excluded.
  EXPECT_EQ(disk.block_count(), 1u);
}

TEST(ReadOnlyRamDiskTest, ReadsSucceedWritesFail) {
  std::array<std::byte, 512 * 2> storage{};
  storage.fill(std::byte{0x42});
  read_only_ram_disk disk(storage);
  block_device_ref ref(disk);

  EXPECT_TRUE(ref.is_read_only());

  std::array<std::byte, 512> readback{};
  ASSERT_TRUE(ref.try_read_blocks(0, readback).has_value());
  EXPECT_EQ(readback[0], std::byte{0x42});

  std::array<std::byte, 512> zeros{};
  auto w = ref.try_write_blocks(0, zeros);
  ASSERT_FALSE(w.has_value());
  EXPECT_EQ(w.error(), error::unsupported_operation);
}

} // namespace
