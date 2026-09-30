// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/boot_memory_map.hpp>
#include <structo/fdt_writer.hpp>

using reloco::span;
using reloco::unexpected;
using structo::fdt::fdt_writer;

namespace {

// Same shape as reloco's own tests/test_fdt_memory.cpp fixture: a single
// 1 GiB "/memory@40000000" range with a 4 KiB reserved carveout at its
// base, so free == full minus that one carveout.
reloco::result<reloco::span<const std::byte>> build_sample_tree(reloco::span<std::byte> storage) {
  auto made = fdt_writer::try_create(storage);
  if (!made)
    return unexpected(made.error());
  fdt_writer w = std::move(made).value();

  if (auto r = w.begin_node(""); !r)
    return unexpected(r.error());
  if (auto r = w.property_u32("#address-cells", 2); !r)
    return unexpected(r.error());
  if (auto r = w.property_u32("#size-cells", 1); !r)
    return unexpected(r.error());

  if (auto r = w.begin_node("memory@40000000"); !r)
    return unexpected(r.error());
  if (auto r = w.property_string("device_type", "memory"); !r)
    return unexpected(r.error());
  const uint32_t reg[3] = {0x0, 0x40000000, 0x40000000};
  if (auto r = w.property_u32_array("reg", span<const uint32_t>(reg, 3)); !r)
    return unexpected(r.error());
  if (auto r = w.end_node(); !r)
    return unexpected(r.error());

  if (auto r = w.begin_node("reserved-memory"); !r)
    return unexpected(r.error());
  if (auto r = w.property_u32("#address-cells", 2); !r)
    return unexpected(r.error());
  if (auto r = w.property_u32("#size-cells", 1); !r)
    return unexpected(r.error());

  if (auto r = w.begin_node("carveout@40000000"); !r)
    return unexpected(r.error());
  const uint32_t carveout_reg[3] = {0x0, 0x40000000, 0x1000};
  if (auto r = w.property_u32_array("reg", span<const uint32_t>(carveout_reg, 3)); !r)
    return unexpected(r.error());
  if (auto r = w.end_node(); !r)
    return unexpected(r.error());

  if (auto r = w.end_node(); !r) // end /reserved-memory
    return unexpected(r.error());

  if (auto r = w.end_node(); !r) // end /
    return unexpected(r.error());

  return w.finish();
}

} // namespace

TEST(BootMemoryMapTest, TryFromDtbExtractsFullAndFreeRanges) {
  reloco::array<std::byte, 1024> storage{};
  auto blob = build_sample_tree(span<std::byte>(storage.data(), storage.size()));
  ASSERT_TRUE(blob);

  auto map = structo::boot_memory_map<4>::try_from_dtb(*blob);
  ASSERT_TRUE(map);

  ASSERT_EQ(map->full.size(), 1u);
  EXPECT_EQ(map->full[0].base, 0x40000000u);
  EXPECT_EQ(map->full[0].size, 0x40000000u);

  ASSERT_EQ(map->free.size(), 1u);
  EXPECT_EQ(map->free[0].base, 0x40000000u + 0x1000u);
  EXPECT_EQ(map->free[0].size, 0x40000000u - 0x1000u);
}

TEST(BootMemoryMapTest, TryLargestFreeRegionReturnsTheOnlyRegion) {
  reloco::array<std::byte, 1024> storage{};
  auto blob = build_sample_tree(span<std::byte>(storage.data(), storage.size()));
  ASSERT_TRUE(blob);

  auto map = structo::boot_memory_map<4>::try_from_dtb(*blob);
  ASSERT_TRUE(map);

  auto largest = map->try_largest_free_region();
  ASSERT_TRUE(largest);
  EXPECT_EQ(largest->base, 0x40000000u + 0x1000u);
  EXPECT_EQ(largest->size, 0x40000000u - 0x1000u);
  EXPECT_EQ(map->free_bytes(), largest->size);
}

TEST(BootMemoryMapTest, TryLargestFreeRegionFailsWhenEmpty) {
  // No /memory node at all: try_from_dtb itself fails with not_found.
  reloco::array<std::byte, 256> storage{};
  auto made = fdt_writer::try_create(span<std::byte>(storage.data(), storage.size()));
  ASSERT_TRUE(made);
  fdt_writer w = std::move(made).value();
  ASSERT_TRUE(w.begin_node(""));
  ASSERT_TRUE(w.end_node());
  auto blob = w.finish();
  ASSERT_TRUE(blob);

  auto map = structo::boot_memory_map<4>::try_from_dtb(*blob);
  EXPECT_FALSE(map);
  EXPECT_EQ(map.error(), reloco::error::not_found);
}
