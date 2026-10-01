// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/arch/fdt_cpu_map.hpp>
#include <structo/fdt_writer.hpp>

using reloco::error;
using reloco::result;
using reloco::span;
using reloco::unexpected;
using structo::arch::hw_id_lut;
using structo::arch::try_populate_hw_id_lut_from_fdt;
using structo::fdt::fdt_reader;
using structo::fdt::fdt_writer;

namespace {

result<span<const std::byte>> build_cpus_tree(span<std::byte> storage, uint32_t address_cells = 1) {
  auto made = fdt_writer::try_create(storage);
  if (!made)
    return unexpected(made.error());
  fdt_writer w = std::move(made).value();

  if (auto r = w.begin_node(""); !r)
    return unexpected(r.error());

  if (auto r = w.begin_node("cpus"); !r)
    return unexpected(r.error());
  if (auto r = w.property_u32("#address-cells", address_cells); !r)
    return unexpected(r.error());
  if (auto r = w.property_u32("#size-cells", 0); !r)
    return unexpected(r.error());

  if (auto r = w.begin_node("cpu@0"); !r)
    return unexpected(r.error());
  if (auto r = w.property_string("device_type", "cpu"); !r)
    return unexpected(r.error());
  if (address_cells == 1) {
    const uint32_t reg0 = 0x0;
    if (auto r = w.property_u32_array("reg", span<const uint32_t>(&reg0, 1)); !r)
      return unexpected(r.error());
  } else {
    const uint32_t reg0[2] = {0x0, 0x0};
    if (auto r = w.property_u32_array("reg", span<const uint32_t>(reg0, 2)); !r)
      return unexpected(r.error());
  }
  if (auto r = w.end_node(); !r)
    return unexpected(r.error());

  if (auto r = w.begin_node("cpu@1"); !r)
    return unexpected(r.error());
  if (address_cells == 1) {
    const uint32_t reg1 = 0x1;
    if (auto r = w.property_u32_array("reg", span<const uint32_t>(&reg1, 1)); !r)
      return unexpected(r.error());
  } else {
    const uint32_t reg1[2] = {0x0, 0x1};
    if (auto r = w.property_u32_array("reg", span<const uint32_t>(reg1, 2)); !r)
      return unexpected(r.error());
  }
  if (auto r = w.end_node(); !r)
    return unexpected(r.error());

  // A disabled CPU must be skipped entirely (not even counted).
  if (auto r = w.begin_node("cpu@2"); !r)
    return unexpected(r.error());
  if (auto r = w.property_string("status", "disabled"); !r)
    return unexpected(r.error());
  const uint32_t reg2 = 0x2;
  if (auto r = w.property_u32_array("reg", span<const uint32_t>(&reg2, 1)); !r)
    return unexpected(r.error());
  if (auto r = w.end_node(); !r)
    return unexpected(r.error());

  if (auto r = w.end_node(); !r) // end /cpus
    return unexpected(r.error());

  if (auto r = w.end_node(); !r) // end /
    return unexpected(r.error());

  return w.finish();
}

result<span<const std::byte>> build_tree_without_cpus(span<std::byte> storage) {
  auto made = fdt_writer::try_create(storage);
  if (!made)
    return unexpected(made.error());
  fdt_writer w = std::move(made).value();
  if (auto r = w.begin_node(""); !r)
    return unexpected(r.error());
  if (auto r = w.end_node(); !r)
    return unexpected(r.error());
  return w.finish();
}

} // namespace

class FdtCpuMapTest : public ::testing::Test {};

TEST_F(FdtCpuMapTest, RegistersEnabledCpusWithSequentialLogicalIndices) {
  reloco::array<std::byte, 1024> storage{};
  auto blob = build_cpus_tree(span<std::byte>(storage.data(), storage.size()));
  ASSERT_TRUE(blob);

  auto reader_made = fdt_reader::try_create(*blob);
  ASSERT_TRUE(reader_made);
  auto reader = std::move(reader_made).value();

  hw_id_lut<uint32_t, 8> lut;
  auto count = try_populate_hw_id_lut_from_fdt(reader, lut);
  ASSERT_TRUE(count);
  EXPECT_EQ(*count, 2u);

  std::size_t idx = 0;
  EXPECT_TRUE(lut.lookup(0x0, idx));
  EXPECT_EQ(idx, 0u);
  EXPECT_TRUE(lut.lookup(0x1, idx));
  EXPECT_EQ(idx, 1u);
  // The disabled cpu@2 must not have been registered at all.
  EXPECT_FALSE(lut.lookup(0x2, idx));
}

TEST_F(FdtCpuMapTest, FailsWithNotFoundWhenNoCpusNodePresent) {
  reloco::array<std::byte, 256> storage{};
  auto blob = build_tree_without_cpus(span<std::byte>(storage.data(), storage.size()));
  ASSERT_TRUE(blob);

  auto reader_made = fdt_reader::try_create(*blob);
  ASSERT_TRUE(reader_made);
  auto reader = std::move(reader_made).value();

  hw_id_lut<uint32_t, 8> lut;
  auto count = try_populate_hw_id_lut_from_fdt(reader, lut);
  ASSERT_FALSE(count);
  EXPECT_EQ(count.error(), error::not_found);
}

TEST_F(FdtCpuMapTest, HonorsCpusAddressCellsForTwoCellRegIds) {
  reloco::array<std::byte, 1024> storage{};
  auto blob = build_cpus_tree(span<std::byte>(storage.data(), storage.size()), /*address_cells=*/2);
  ASSERT_TRUE(blob);

  auto reader_made = fdt_reader::try_create(*blob);
  ASSERT_TRUE(reader_made);
  auto reader = std::move(reader_made).value();

  hw_id_lut<uint32_t, 8> lut;
  auto count = try_populate_hw_id_lut_from_fdt(reader, lut);
  ASSERT_TRUE(count);
  EXPECT_EQ(*count, 2u);

  std::size_t idx = 0;
  EXPECT_TRUE(lut.lookup(0x0, idx));
  EXPECT_EQ(idx, 0u);
  EXPECT_TRUE(lut.lookup(0x1, idx));
  EXPECT_EQ(idx, 1u);
}

TEST_F(FdtCpuMapTest, FailsWithCapacityExceededWhenLutIsTooSmall) {
  reloco::array<std::byte, 1024> storage{};
  auto blob = build_cpus_tree(span<std::byte>(storage.data(), storage.size()));
  ASSERT_TRUE(blob);

  auto reader_made = fdt_reader::try_create(*blob);
  ASSERT_TRUE(reader_made);
  auto reader = std::move(reader_made).value();

  hw_id_lut<uint32_t, 1> lut; // only room for 1 CPU; the tree has 2 enabled
  auto count = try_populate_hw_id_lut_from_fdt(reader, lut);
  ASSERT_FALSE(count);
  EXPECT_EQ(count.error(), error::capacity_exceeded);
}
