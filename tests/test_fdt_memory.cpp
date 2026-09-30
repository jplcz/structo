// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/fdt_memory.hpp>
#include <structo/fdt_writer.hpp>

using structo::error;
using structo::region_set;
using structo::result;
using structo::span;
using structo::unexpected;
using structo::fdt::fdt_reader;
using structo::fdt::fdt_writer;
using structo::fdt::try_extract_memory;

namespace {

result<span<const std::byte>> build_full_tree(span<std::byte> storage) {
  auto made = fdt_writer::try_create(storage);
  if (!made)
    return unexpected(made.error());
  fdt_writer w = std::move(made).value();

  if (auto r = w.add_mem_reserve(0x1000, 0x1000); !r)
    return unexpected(r.error());

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

  if (auto r = w.begin_node("disabled@40010000"); !r)
    return unexpected(r.error());
  if (auto r = w.property_string("status", "disabled"); !r)
    return unexpected(r.error());
  const uint32_t disabled_reg[3] = {0x0, 0x40010000, 0x1000};
  if (auto r = w.property_u32_array("reg", span<const uint32_t>(disabled_reg, 3)); !r)
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

TEST(FdtMemoryTest, ExtractsFullAndFreeMemoryExcludingReservations) {
  reloco::array<std::byte, 1024> storage{};
  auto blob = build_full_tree(span<std::byte>(storage.data(), storage.size()));
  ASSERT_TRUE(blob);

  auto reader_made = fdt_reader::try_create(*blob);
  ASSERT_TRUE(reader_made);
  auto reader = std::move(reader_made).value();

  region_set<4> full;
  region_set<4> free;
  auto extracted = try_extract_memory(reader, full, free);
  ASSERT_TRUE(extracted);

  ASSERT_EQ(full.size(), 1u);
  EXPECT_EQ(full[0].base, 0x40000000u);
  EXPECT_EQ(full[0].size, 0x40000000u);

  // free = full, minus the [0x40000000, 0x40001000) carveout (the legacy
  // [0x1000, 0x2000) memreserve doesn't overlap `full` at all, so it's a
  // no-op here); the *disabled* reservation must NOT be subtracted.
  ASSERT_EQ(free.size(), 1u);
  EXPECT_EQ(free[0].base, 0x40001000u);
  EXPECT_EQ(free[0].size, 0x40000000u - 0x1000u);
}

TEST(FdtMemoryTest, FailsWithNotFoundWhenNoMemoryClassNodePresent) {
  reloco::array<std::byte, 256> storage{};
  auto made = fdt_writer::try_create(span<std::byte>(storage.data(), storage.size()));
  ASSERT_TRUE(made);
  fdt_writer w = std::move(made).value();
  ASSERT_TRUE(w.begin_node(""));
  ASSERT_TRUE(w.end_node());
  auto blob = w.finish();
  ASSERT_TRUE(blob);

  auto reader_made = fdt_reader::try_create(*blob);
  ASSERT_TRUE(reader_made);
  auto reader = std::move(reader_made).value();

  region_set<4> full;
  region_set<4> free;
  auto extracted = try_extract_memory(reader, full, free);
  ASSERT_FALSE(extracted);
  EXPECT_EQ(extracted.error(), error::not_found);
}

TEST(FdtMemoryTest, RecognizesSecureMemoryNodeByNameWithoutDeviceType) {
  reloco::array<std::byte, 512> storage{};
  auto made = fdt_writer::try_create(span<std::byte>(storage.data(), storage.size()));
  ASSERT_TRUE(made);
  fdt_writer w = std::move(made).value();
  ASSERT_TRUE(w.begin_node(""));
  ASSERT_TRUE(w.property_u32("#address-cells", 2));
  ASSERT_TRUE(w.property_u32("#size-cells", 1));
  ASSERT_TRUE(w.begin_node("secure-memory@e0000000"));
  const uint32_t reg[3] = {0x0, 0xe0000000, 0x100000};
  ASSERT_TRUE(w.property_u32_array("reg", span<const uint32_t>(reg, 3)));
  ASSERT_TRUE(w.end_node());
  ASSERT_TRUE(w.end_node());
  auto blob = w.finish();
  ASSERT_TRUE(blob);

  auto reader_made = fdt_reader::try_create(*blob);
  ASSERT_TRUE(reader_made);
  auto reader = std::move(reader_made).value();

  region_set<4> full;
  region_set<4> free;
  auto extracted = try_extract_memory(reader, full, free);
  ASSERT_TRUE(extracted);
  ASSERT_EQ(full.size(), 1u);
  EXPECT_EQ(full[0].base, 0xe0000000u);
  EXPECT_EQ(full[0].size, 0x100000u);
  ASSERT_EQ(free.size(), 1u);
  EXPECT_EQ(free[0].base, 0xe0000000u);
  EXPECT_EQ(free[0].size, 0x100000u);
}

TEST(FdtMemoryTest, SecureMemoryNodeIsGatedBySecureStatusNotStatus) {
  reloco::array<std::byte, 512> storage{};
  auto made = fdt_writer::try_create(span<std::byte>(storage.data(), storage.size()));
  ASSERT_TRUE(made);
  fdt_writer w = std::move(made).value();
  ASSERT_TRUE(w.begin_node(""));
  ASSERT_TRUE(w.property_u32("#address-cells", 2));
  ASSERT_TRUE(w.property_u32("#size-cells", 1));

  // Not claimed by the TEE (secure-status != "okay"), even though a plain
  // "status" would've been absent/"okay" -- secure-status must win.
  ASSERT_TRUE(w.begin_node("secure-memory@e0000000"));
  ASSERT_TRUE(w.property_string("secure-status", "disabled"));
  const uint32_t unclaimed_reg[3] = {0x0, 0xe0000000, 0x100000};
  ASSERT_TRUE(w.property_u32_array("reg", span<const uint32_t>(unclaimed_reg, 3)));
  ASSERT_TRUE(w.end_node());

  // Claimed by the TEE.
  ASSERT_TRUE(w.begin_node("secure-memory@e0100000"));
  ASSERT_TRUE(w.property_string("secure-status", "okay"));
  const uint32_t claimed_reg[3] = {0x0, 0xe0100000, 0x100000};
  ASSERT_TRUE(w.property_u32_array("reg", span<const uint32_t>(claimed_reg, 3)));
  ASSERT_TRUE(w.end_node());

  ASSERT_TRUE(w.end_node());
  auto blob = w.finish();
  ASSERT_TRUE(blob);

  auto reader_made = fdt_reader::try_create(*blob);
  ASSERT_TRUE(reader_made);
  auto reader = std::move(reader_made).value();

  region_set<4> full;
  region_set<4> free;
  auto extracted = try_extract_memory(reader, full, free);
  ASSERT_TRUE(extracted);
  ASSERT_EQ(full.size(), 1u);
  EXPECT_EQ(full[0].base, 0xe0100000u);
  EXPECT_EQ(full[0].size, 0x100000u);
}

TEST(FdtMemoryTest, MemoryNodeWithNonOkayStatusOtherThanDisabledIsSkipped) {
  reloco::array<std::byte, 512> storage{};
  auto made = fdt_writer::try_create(span<std::byte>(storage.data(), storage.size()));
  ASSERT_TRUE(made);
  fdt_writer w = std::move(made).value();
  ASSERT_TRUE(w.begin_node(""));
  ASSERT_TRUE(w.property_u32("#address-cells", 2));
  ASSERT_TRUE(w.property_u32("#size-cells", 1));

  // "reserved" (not "okay", not literally "disabled") must still be excluded.
  ASSERT_TRUE(w.begin_node("memory@f0000000"));
  ASSERT_TRUE(w.property_string("status", "reserved"));
  const uint32_t reserved_reg[3] = {0x0, 0xf0000000, 0x1000};
  ASSERT_TRUE(w.property_u32_array("reg", span<const uint32_t>(reserved_reg, 3)));
  ASSERT_TRUE(w.end_node());

  ASSERT_TRUE(w.begin_node("memory@40000000"));
  ASSERT_TRUE(w.property_string("status", "okay"));
  const uint32_t reg[3] = {0x0, 0x40000000, 0x1000};
  ASSERT_TRUE(w.property_u32_array("reg", span<const uint32_t>(reg, 3)));
  ASSERT_TRUE(w.end_node());

  ASSERT_TRUE(w.end_node());
  auto blob = w.finish();
  ASSERT_TRUE(blob);

  auto reader_made = fdt_reader::try_create(*blob);
  ASSERT_TRUE(reader_made);
  auto reader = std::move(reader_made).value();

  region_set<4> full;
  region_set<4> free;
  auto extracted = try_extract_memory(reader, full, free);
  ASSERT_TRUE(extracted);
  ASSERT_EQ(full.size(), 1u);
  EXPECT_EQ(full[0].base, 0x40000000u);
  EXPECT_EQ(full[0].size, 0x1000u);
}
