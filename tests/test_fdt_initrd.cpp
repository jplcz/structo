// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <reloco/array.hpp>
#include <reloco/external_vector.hpp>
#include <reloco/vector.hpp>
#include <structo/fdt_initrd.hpp>
#include <structo/fdt_writer.hpp>

#include <array>

using reloco::array;
using reloco::error;
using reloco::external_vector;
using reloco::span;
using reloco::vector;
using structo::fdt::fdt_index;
using structo::fdt::fdt_index_node;
using structo::fdt::fdt_index_phandle_entry;
using structo::fdt::fdt_reader;
using structo::fdt::fdt_writer;
using structo::fdt::find_initrd;
using structo::fdt::initrd_location;
using structo::fdt::try_bind_initrd_ram_disk;
using build_frame = structo::fdt::detail::fdt_index_build_frame;

namespace {

using index_type = fdt_index<external_vector>;

struct index_storage {
  array<fdt_index_node, 16> nodes{};
  array<fdt_index_phandle_entry, 4> phandles{};
  array<build_frame, 8> stack{};

  [[nodiscard]] reloco::result<index_type> build(const fdt_reader &reader) {
    return index_type::try_build(
        reader, external_vector<fdt_index_node>(span<fdt_index_node>(nodes.data(), nodes.size())),
        external_vector<fdt_index_phandle_entry>(span<fdt_index_phandle_entry>(phandles.data(), phandles.size())),
        external_vector<build_frame>(span<build_frame>(stack.data(), stack.size())));
  }
};

enum class initrd_prop_width { none, u32, u64 };

vector<std::byte> build_blob(initrd_prop_width width, bool only_start = false) {
  auto storage = vector<std::byte>::try_create(4096).value();
  (void)storage.try_resize(4096, std::byte{0});
  auto made = fdt_writer::try_create(span<std::byte>(storage.data(), storage.size()));
  auto w = std::move(made).value();
  (void)w.begin_node("");
  (void)w.begin_node("chosen");
  (void)w.property_string("bootargs", "console=ttyS0");
  if (width == initrd_prop_width::u32) {
    (void)w.property_u32("linux,initrd-start", 0x1000);
    if (!only_start)
      (void)w.property_u32("linux,initrd-end", 0x2000);
  } else if (width == initrd_prop_width::u64) {
    (void)w.property_u64("linux,initrd-start", 0x1'0000'0000ULL);
    if (!only_start)
      (void)w.property_u64("linux,initrd-end", 0x1'0000'1000ULL);
  }
  (void)w.end_node();
  (void)w.end_node();
  auto blob = w.finish();
  auto out = vector<std::byte>::try_create(blob->size()).value();
  (void)out.try_resize(blob->size(), std::byte{0});
  std::size_t i = 0;
  for (std::byte b : *blob)
    out[i++] = b;
  return out;
}

vector<std::byte> build_blob_no_chosen() {
  auto storage = vector<std::byte>::try_create(4096).value();
  (void)storage.try_resize(4096, std::byte{0});
  auto made = fdt_writer::try_create(span<std::byte>(storage.data(), storage.size()));
  auto w = std::move(made).value();
  (void)w.begin_node("");
  (void)w.property_string("compatible", "linux,dummy-board");
  (void)w.end_node();
  auto blob = w.finish();
  auto out = vector<std::byte>::try_create(blob->size()).value();
  (void)out.try_resize(blob->size(), std::byte{0});
  std::size_t i = 0;
  for (std::byte b : *blob)
    out[i++] = b;
  return out;
}

TEST(FdtInitrdTest, FindsU32InitrdRange) {
  auto blob = build_blob(initrd_prop_width::u32);
  auto reader = fdt_reader::try_create(span<const std::byte>(blob.data(), blob.size())).value();
  index_storage storage;
  auto idx = storage.build(reader).value();

  auto found = find_initrd(idx);
  ASSERT_TRUE(found.has_value());
  ASSERT_TRUE(found->has_value());
  EXPECT_EQ((*found)->start, 0x1000u);
  EXPECT_EQ((*found)->end, 0x2000u);
  EXPECT_EQ((*found)->size_bytes(), 0x1000u);
}

TEST(FdtInitrdTest, FindsU64InitrdRange) {
  auto blob = build_blob(initrd_prop_width::u64);
  auto reader = fdt_reader::try_create(span<const std::byte>(blob.data(), blob.size())).value();
  index_storage storage;
  auto idx = storage.build(reader).value();

  auto found = find_initrd(idx);
  ASSERT_TRUE(found.has_value());
  ASSERT_TRUE(found->has_value());
  EXPECT_EQ((*found)->start, 0x1'0000'0000ULL);
  EXPECT_EQ((*found)->end, 0x1'0000'1000ULL);
}

TEST(FdtInitrdTest, NoInitrdPropertiesReportsEmptyOptional) {
  auto blob = build_blob(initrd_prop_width::none);
  auto reader = fdt_reader::try_create(span<const std::byte>(blob.data(), blob.size())).value();
  index_storage storage;
  auto idx = storage.build(reader).value();

  auto found = find_initrd(idx);
  ASSERT_TRUE(found.has_value());
  EXPECT_FALSE(found->has_value());
}

TEST(FdtInitrdTest, NoChosenNodeReportsEmptyOptional) {
  auto blob = build_blob_no_chosen();
  auto reader = fdt_reader::try_create(span<const std::byte>(blob.data(), blob.size())).value();
  index_storage storage;
  auto idx = storage.build(reader).value();

  auto found = find_initrd(idx);
  ASSERT_TRUE(found.has_value());
  EXPECT_FALSE(found->has_value());
}

TEST(FdtInitrdTest, OnlyStartPropertyFailsWithInvalidArgument) {
  auto blob = build_blob(initrd_prop_width::u32, /*only_start=*/true);
  auto reader = fdt_reader::try_create(span<const std::byte>(blob.data(), blob.size())).value();
  index_storage storage;
  auto idx = storage.build(reader).value();

  auto found = find_initrd(idx);
  ASSERT_FALSE(found.has_value());
  EXPECT_EQ(found.error(), error::invalid_argument);
}

TEST(FdtInitrdTest, TryBindInitrdRamDiskReadsBackContent) {
  initrd_location loc{0, 4};
  std::array<std::byte, 4> image{std::byte{'c'}, std::byte{'p'}, std::byte{'i'}, std::byte{'o'}};

  structo::hw::read_only_ram_disk disk;
  structo::hw::block_device_ref ref;
  auto bound = try_bind_initrd_ram_disk(loc, span<const std::byte>(image.data(), image.size()), disk, ref);
  ASSERT_TRUE(bound.has_value());
  ASSERT_TRUE(static_cast<bool>(ref));
  EXPECT_EQ(ref.block_size(), 1u);
  EXPECT_EQ(ref.block_count(), 4u);

  std::array<std::byte, 4> readback{};
  auto r = ref.try_read_blocks(0, readback);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(readback, image);
}

TEST(FdtInitrdTest, TryBindInitrdRamDiskFailsWhenMappedSpanTooSmall) {
  initrd_location loc{0, 8};
  std::array<std::byte, 4> image{};

  structo::hw::read_only_ram_disk disk;
  structo::hw::block_device_ref ref;
  auto bound = try_bind_initrd_ram_disk(loc, span<const std::byte>(image.data(), image.size()), disk, ref);
  ASSERT_FALSE(bound.has_value());
  EXPECT_EQ(bound.error(), error::out_of_range);
}

} // namespace
