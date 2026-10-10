// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>

#include <structo/hw/block_cache_ref.hpp>
#include <structo/hw/partition_table.hpp>

#include "partition_test_images.hpp"

#include <reloco/array.hpp>

#include <cstring>
#include <vector>

// Test fixtures index raw buffers freely; bounds are checked by the assertions.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

namespace {

using namespace structo::hw;
using reloco::error;
using reloco::span;

// RAM disk with a runtime block size so one fake covers several geometries.
struct ram_disk {
  std::vector<std::byte> mem;
  std::size_t bs = 512;
  bool read_only = false;
  int reads = 0;

  explicit ram_disk(const test_images::image_chunk *chunks, std::size_t n) : mem(test_images::image_size) {
    for (std::size_t i = 0; i < n; ++i)
      std::memcpy(mem.data() + chunks[i].offset, chunks[i].data, chunks[i].size);
  }
  explicit ram_disk(std::size_t bytes) : mem(bytes) {}
  std::byte &at(std::size_t off) { return mem[off]; }
};

} // namespace

template <> struct structo::hw::block_device_traits<ram_disk> {
  static std::size_t block_size(ram_disk &d) noexcept { return d.bs; }
  static std::uint64_t block_count(ram_disk &d) noexcept { return d.mem.size() / d.bs; }
  static reloco::result<void> try_read_blocks(ram_disk &d, std::uint64_t lba, span<std::byte> dst) noexcept {
    ++d.reads;
    std::memcpy(dst.data(), d.mem.data() + lba * d.bs, dst.size());
    return {};
  }
  static reloco::result<void> try_write_blocks(ram_disk &d, std::uint64_t lba, span<const std::byte> src) noexcept {
    std::memcpy(d.mem.data() + lba * d.bs, src.data(), src.size());
    return {};
  }
  static bool is_read_only(ram_disk &d) noexcept { return d.read_only; }
};

namespace {

using structo::hw::detail::parse_guid;

constexpr std::size_t kSector = 512;

// Results of lookups are rvalues, which `->` refuses to dereference; copy the partition out instead.
partition_info ok(reloco::result<partition_info> r) {
  EXPECT_TRUE(r);
  return r ? *r : partition_info{};
}

class PartitionTableTest : public ::testing::Test {
protected:
  ram_disk gpt{test_images::gpt_image, std::size(test_images::gpt_image)};
  ram_disk mbr{test_images::mbr_image, std::size(test_images::mbr_image)};

  static std::size_t count(const partition_table &t) {
    std::size_t n = 0;
    auto it = t.entries();
    for (auto p : it) {
      EXPECT_TRUE(p);
      if (!p)
        break;
      ++n;
    }
    return n;
  }
};

TEST_F(PartitionTableTest, GuidParserMatchesKnownTypes) {
  EXPECT_EQ(gpt_types::efi_system.data1, 0xC12A7328u);
  EXPECT_EQ(gpt_types::efi_system.data2, 0xF81Fu);
  EXPECT_EQ(gpt_types::efi_system.data3, 0x11D2u);
  EXPECT_EQ(gpt_types::efi_system.data4[0], 0xBA);
  EXPECT_EQ(gpt_types::efi_system.data4[7], 0x3B);
  EXPECT_EQ(parse_guid("not-a-guid"), structo::hw::guid{});
}

TEST_F(PartitionTableTest, GptEnumeratesPartitions) {
  block_device_ref dev(gpt);
  auto t = partition_table::try_open(dev);
  ASSERT_TRUE(t);
  EXPECT_EQ(t->scheme(), partition_scheme::gpt);
  EXPECT_EQ(t->disk_guid(), parse_guid("11111111-2222-3333-4444-555555555555"));
  EXPECT_FALSE(t->used_backup_header());
  EXPECT_EQ(t->gpt_entry_capacity(), 128u);
  EXPECT_EQ(count(*t), 3u);

  auto esp = t->try_get(1);
  ASSERT_TRUE(esp);
  EXPECT_EQ(esp->first_lba, 2048u);
  EXPECT_EQ(esp->block_count, 512u);
  EXPECT_EQ(esp->type_guid, gpt_types::efi_system);
  EXPECT_EQ(esp->unique_guid, parse_guid("AAAAAAAA-0000-0000-0000-000000000001"));
  EXPECT_EQ(esp->name_view(), "EFI");
  EXPECT_FALSE(esp->bootable);

  auto data = t->try_get(4);
  ASSERT_TRUE(data);
  EXPECT_EQ(data->first_lba, 3072u);
  EXPECT_EQ(data->block_count, 929u);
  EXPECT_EQ(data->type_guid, gpt_types::linux_root_x86_64);
  EXPECT_TRUE(data->bootable);

  EXPECT_EQ(t->try_get(3).error(), error::not_found);
}

TEST_F(PartitionTableTest, GptLookups) {
  auto t = partition_table::try_open(block_device_ref(gpt));
  ASSERT_TRUE(t);
  EXPECT_EQ(ok(t->find_by_type_guid(gpt_types::linux_filesystem)).number, 2u);
  EXPECT_EQ(ok(t->find_by_unique_guid(parse_guid("AAAAAAAA-0000-0000-0000-000000000004"))).number, 4u);
  EXPECT_EQ(ok(t->find_by_name("root")).number, 2u);
  EXPECT_EQ(t->find_by_name("nope").error(), error::not_found);
  EXPECT_EQ(t->find_by_type_guid(gpt_types::linux_swap).error(), error::not_found);
  EXPECT_EQ(t->find_by_mbr_type(0x83).error(), error::not_found);
}

TEST_F(PartitionTableTest, PartitionBecomesBlockDevice) {
  block_device_ref dev(gpt);
  auto t = partition_table::try_open(dev);
  ASSERT_TRUE(t);
  auto part = t->try_open_partition(2);
  ASSERT_TRUE(part);
  block_device_ref pdev = part->as_block_device();
  EXPECT_EQ(pdev.block_size(), kSector);
  EXPECT_EQ(pdev.block_count(), 512u);

  reloco::array<std::byte, kSector> buf{};
  buf[0] = std::byte{0x5A};
  ASSERT_TRUE(pdev.try_write_blocks(3, span<const std::byte>(buf)));
  EXPECT_EQ(gpt.at((2560 + 3) * kSector), std::byte{0x5A}); // LBA is shifted by the partition start

  reloco::array<std::byte, kSector> back{};
  ASSERT_TRUE(pdev.try_read_blocks(3, span<std::byte>(back)));
  EXPECT_EQ(back[0], std::byte{0x5A});
  EXPECT_EQ(pdev.try_read_blocks(512, span<std::byte>(back)).error(), error::out_of_range); // end of partition

  EXPECT_EQ(t->try_open_partition(9).error(), error::not_found);
}

TEST_F(PartitionTableTest, PartitionDeviceValidation) {
  block_device_ref dev(gpt);
  EXPECT_EQ(partition_device::try_create(block_device_ref{}, 0, 1).error(), error::unsupported_operation);
  EXPECT_EQ(partition_device::try_create(dev, 4000, 200).error(), error::out_of_range);
  auto ro = partition_device::try_create(dev, 0, 16, true);
  ASSERT_TRUE(ro);
  block_device_ref r = ro->as_block_device();
  EXPECT_TRUE(r.is_read_only());
  reloco::array<std::byte, kSector> buf{};
  EXPECT_FALSE(r.try_write_blocks(0, span<const std::byte>(buf)).has_value() && !r.is_read_only());
}

TEST_F(PartitionTableTest, GptFallsBackToBackupHeader) {
  gpt.at(kSector + 16) ^= std::byte{0xFF}; // damage the primary header's CRC field
  auto t = partition_table::try_open(block_device_ref(gpt));
  ASSERT_TRUE(t);
  EXPECT_TRUE(t->used_backup_header());
  EXPECT_EQ(count(*t), 3u);
  EXPECT_EQ(ok(t->find_by_name("data")).first_lba, 3072u);
}

TEST_F(PartitionTableTest, GptEntryArrayCrcMismatchUsesBackup) {
  gpt.at(2 * kSector + 3) ^= std::byte{0x01}; // primary entry array, first entry
  auto t = partition_table::try_open(block_device_ref(gpt));
  ASSERT_TRUE(t);
  EXPECT_TRUE(t->used_backup_header());
  EXPECT_EQ(ok(t->try_get(1)).type_guid, gpt_types::efi_system);
}

TEST_F(PartitionTableTest, GptBothHeadersDamaged) {
  gpt.at(kSector + 16) ^= std::byte{0xFF};
  gpt.at(4095 * kSector + 16) ^= std::byte{0xFF};
  // A protective MBR says a GPT should be here, so a corrupt one is an error rather than "no table".
  EXPECT_EQ(partition_table::try_open(block_device_ref(gpt)).error(), error::invalid_argument);
  EXPECT_EQ(partition_table::try_open_gpt(block_device_ref(gpt)).error(), error::invalid_argument);
}

TEST_F(PartitionTableTest, ProtectiveMbrIsRecognised) {
  auto m = partition_table::try_open_mbr(block_device_ref(gpt));
  ASSERT_TRUE(m);
  EXPECT_TRUE(m->has_protective_entry());
  EXPECT_EQ(ok(m->try_get(1)).mbr_type, mbr_types::gpt_protective);
}

TEST_F(PartitionTableTest, MbrEnumeratesPrimaryAndLogical) {
  auto t = partition_table::try_open(block_device_ref(mbr));
  ASSERT_TRUE(t);
  EXPECT_EQ(t->scheme(), partition_scheme::mbr);
  EXPECT_EQ(count(*t), 5u);

  auto p1 = t->try_get(1);
  ASSERT_TRUE(p1);
  EXPECT_EQ(p1->first_lba, 64u);
  EXPECT_EQ(p1->block_count, 256u);
  EXPECT_EQ(p1->mbr_type, mbr_types::linux_native);
  EXPECT_TRUE(p1->bootable);
  EXPECT_FALSE(p1->is_extended);

  auto p2 = t->try_get(2);
  ASSERT_TRUE(p2);
  EXPECT_EQ(p2->mbr_type, mbr_types::fat32_lba);
  EXPECT_FALSE(p2->bootable);

  auto ext = t->try_get(3);
  ASSERT_TRUE(ext);
  EXPECT_TRUE(ext->is_extended);
  EXPECT_EQ(ext->first_lba, 1024u);
  EXPECT_EQ(ext->block_count, 2048u);

  auto l5 = t->try_get(5);
  ASSERT_TRUE(l5);
  EXPECT_EQ(l5->first_lba, 1088u);
  EXPECT_EQ(l5->block_count, 256u);
  EXPECT_EQ(l5->mbr_type, mbr_types::linux_native);

  auto l6 = t->try_get(6);
  ASSERT_TRUE(l6);
  EXPECT_EQ(l6->first_lba, 1400u);
  EXPECT_EQ(l6->block_count, 512u);
  EXPECT_EQ(l6->mbr_type, mbr_types::linux_swap);

  EXPECT_EQ(t->try_get(4).error(), error::not_found);
  EXPECT_EQ(t->try_get(7).error(), error::not_found);
  EXPECT_EQ(ok(t->find_by_mbr_type(mbr_types::linux_swap)).number, 6u);
  EXPECT_EQ(t->find_by_mbr_type(mbr_types::extended_chs).error(), error::not_found); // containers are skipped
}

TEST_F(PartitionTableTest, MbrLogicalPartitionIsABlockDevice) {
  auto t = partition_table::try_open(block_device_ref(mbr));
  ASSERT_TRUE(t);
  auto part = t->try_open_partition(6);
  ASSERT_TRUE(part);
  block_device_ref pdev = part->as_block_device();
  EXPECT_EQ(pdev.block_count(), 512u);
  mbr.at(1400 * kSector + 7) = std::byte{0x42};
  reloco::array<std::byte, kSector> buf{};
  ASSERT_TRUE(pdev.try_read_blocks(0, span<std::byte>(buf)));
  EXPECT_EQ(buf[7], std::byte{0x42});
}

TEST_F(PartitionTableTest, MbrEbrLoopIsRejected) {
  // Make the first EBR's link point at itself (relative start 0 -> next == this EBR).
  for (std::size_t i = 0; i < 4; ++i)
    mbr.at(1024 * kSector + 446 + 16 + 8 + i) = std::byte{0};
  auto t = partition_table::try_open(block_device_ref(mbr));
  ASSERT_TRUE(t);
  EXPECT_EQ(t->try_get(5).error(), error::invalid_argument);
  EXPECT_EQ(ok(t->try_get(1)).number, 1u); // primaries still resolve
}

TEST_F(PartitionTableTest, NonPartitionedDisksAreRejected) {
  ram_disk blank(std::size_t{1} << 20);
  EXPECT_EQ(partition_table::try_open(block_device_ref(blank)).error(), error::not_found);

  // A FAT-style boot sector: 0x55AA signature but boot-code bytes where the partition entries would be.
  ram_disk fat(std::size_t{1} << 20);
  fat.at(446) = std::byte{0x4B};
  fat.at(510) = std::byte{0x55};
  fat.at(511) = std::byte{0xAA};
  EXPECT_EQ(partition_table::try_open(block_device_ref(fat)).error(), error::not_found);

  EXPECT_EQ(partition_table::try_open(block_device_ref{}).error(), error::unsupported_operation);
  ram_disk huge(std::size_t{1} << 20);
  huge.bs = 8192;
  EXPECT_EQ(partition_table::try_open(block_device_ref(huge)).error(), error::unsupported_operation);
}

TEST_F(PartitionTableTest, ReadsThroughBlockCache) {
  block_device_ref raw(gpt);
  reloco::array<std::byte, 16 + 48 * (24 + kSector)> storage{};
  auto cache = block_cache_ref::try_create(raw, span<std::byte>(storage));
  ASSERT_TRUE(cache);
  block_device_ref cached(*cache);

  auto t = partition_table::try_open(cached);
  ASSERT_TRUE(t);
  const int first_open_reads = gpt.reads;
  ASSERT_EQ(count(*t), 3u);
  ASSERT_EQ(count(*t), 3u);
  // Enumerating again is served from the cache.
  const int before = gpt.reads;
  ASSERT_EQ(count(*t), 3u);
  EXPECT_EQ(gpt.reads, before);
  EXPECT_GT(first_open_reads, 0);
}

} // namespace

RELOCO_END_UNSAFE_BUFFER_USAGE
