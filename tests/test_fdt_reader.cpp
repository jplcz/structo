// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/fdt_reader.hpp>
#include <structo/fdt_writer.hpp>

#include <array>
#include <vector>

using structo::error;
using structo::span;
using structo::fdt::fdt_event_kind;
using structo::fdt::fdt_reader;
using structo::fdt::fdt_writer;

namespace {

// Builds a small, well-formed blob shared by several tests:
//   /memreserve/ 0x1000 0x2000;
//   /memreserve/ 0x5000 0x100;
//   / {
//     #address-cells = <2>;
//     compatible = "linux,dummy-board";
//     cpus {
//       cpu@0 {
//         reg = <0 0 0 0 0 0 0 0>; // 8 bytes, big enough for a u64 check
//       };
//     };
//   };
std::vector<std::byte> build_sample_blob() {
  std::vector<std::byte> storage(4096);
  auto made = fdt_writer::try_create(span<std::byte>(storage.data(), storage.size()));
  auto w = std::move(made).value();
  (void)w.add_mem_reserve(0x1000, 0x2000);
  (void)w.add_mem_reserve(0x5000, 0x100);
  (void)w.begin_node("");
  (void)w.property_u32("#address-cells", 2);
  (void)w.property_string("compatible", "linux,dummy-board");
  (void)w.begin_node("cpus");
  (void)w.begin_node("cpu@0");
  (void)w.property_u64("reg", 0);
  (void)w.end_node();
  (void)w.end_node();
  (void)w.end_node();
  auto blob = w.finish();
  return std::vector<std::byte>(blob->begin(), blob->end());
}

} // namespace

TEST(FdtReaderTest, TryCreateRejectsSpanTooSmallForHeader) {
  std::array<std::byte, 8> storage{};
  auto made = fdt_reader::try_create(span<const std::byte>(storage.data(), storage.size()));
  ASSERT_FALSE(made);
  EXPECT_EQ(made.error(), error::out_of_bounds);
}

TEST(FdtReaderTest, TryCreateRejectsBadMagic) {
  auto blob = build_sample_blob();
  blob[0] = std::byte{0xff};
  auto made = fdt_reader::try_create(span<const std::byte>(blob.data(), blob.size()));
  ASSERT_FALSE(made);
  EXPECT_EQ(made.error(), error::invalid_argument);
}

TEST(FdtReaderTest, TryCreateRejectsTruncatedBlob) {
  auto blob = build_sample_blob();
  blob.resize(blob.size() - 4); // totalsize now claims more than the buffer holds.
  auto made = fdt_reader::try_create(span<const std::byte>(blob.data(), blob.size()));
  ASSERT_FALSE(made);
  EXPECT_EQ(made.error(), error::out_of_bounds);
}

TEST(FdtReaderTest, TryProbeSizeRejectsSpanTooSmallForHeader) {
  std::array<std::byte, 8> storage{};
  auto probed = fdt_reader::try_probe_size(span<const std::byte>(storage.data(), storage.size()));
  ASSERT_FALSE(probed);
  EXPECT_EQ(probed.error(), error::out_of_bounds);
}

TEST(FdtReaderTest, TryProbeSizeRejectsBadMagic) {
  auto blob = build_sample_blob();
  blob[0] = std::byte{0xff};
  auto probed = fdt_reader::try_probe_size(span<const std::byte>(blob.data(), blob.size()));
  ASSERT_FALSE(probed);
  EXPECT_EQ(probed.error(), error::invalid_argument);
}

TEST(FdtReaderTest, TryProbeSizeReturnsDeclaredTotalsizeFromJustTheHeaderPrefix) {
  auto blob = build_sample_blob();

  // Only the fixed 40-byte header needs to be readable up front -- this is
  // the whole point: learn how much more to map/allocate before the rest
  // of the blob is even available.
  ASSERT_GE(blob.size(), structo::fdt::detail::header_size);
  auto probed =
      fdt_reader::try_probe_size(span<const std::byte>(blob.data(), structo::fdt::detail::header_size));
  ASSERT_TRUE(probed);
  EXPECT_EQ(*probed, blob.size());

  // The probed size is exactly what's needed to hand the full blob to
  // `try_create`.
  auto made = fdt_reader::try_create(span<const std::byte>(blob.data(), *probed));
  ASSERT_TRUE(made);
}

TEST(FdtReaderTest, TryProbeSizeAcceptsLongerSpanThanTheHeader) {
  auto blob = build_sample_blob();
  auto probed = fdt_reader::try_probe_size(span<const std::byte>(blob.data(), blob.size()));
  ASSERT_TRUE(probed);
  EXPECT_EQ(*probed, blob.size());
}

TEST(FdtReaderTest, HeaderAccessorsMatchWriterDefaults) {
  auto blob = build_sample_blob();
  auto made = fdt_reader::try_create(span<const std::byte>(blob.data(), blob.size()));
  ASSERT_TRUE(made);
  auto r = std::move(made).value();
  EXPECT_EQ(r.version(), structo::fdt::version);
  EXPECT_EQ(r.last_comp_version(), structo::fdt::last_comp_version);
  EXPECT_EQ(r.boot_cpuid_phys(), 0u);
}

TEST(FdtReaderTest, MemReservesIterateInOrderAndTerminate) {
  auto blob = build_sample_blob();
  auto made = fdt_reader::try_create(span<const std::byte>(blob.data(), blob.size()));
  ASSERT_TRUE(made);
  auto r = std::move(made).value();

  std::vector<std::pair<uint64_t, uint64_t>> reserves;
  for (auto entry : r.mem_reserves()) {
    ASSERT_TRUE(entry);
    reserves.emplace_back(entry->address, entry->size);
  }
  ASSERT_EQ(reserves.size(), 2u);
  EXPECT_EQ(reserves[0].first, 0x1000u);
  EXPECT_EQ(reserves[0].second, 0x2000u);
  EXPECT_EQ(reserves[1].first, 0x5000u);
  EXPECT_EQ(reserves[1].second, 0x100u);
}

TEST(FdtReaderTest, StructureEventsRoundTripInDepthFirstOrder) {
  auto blob = build_sample_blob();
  auto made = fdt_reader::try_create(span<const std::byte>(blob.data(), blob.size()));
  ASSERT_TRUE(made);
  auto r = std::move(made).value();

  struct expected_event {
    fdt_event_kind kind;
    const char *name; // node name (begin_node) or property name (property); unused for end_node.
  };
  const std::array<expected_event, 9> expected = {{
      {fdt_event_kind::begin_node, ""},
      {fdt_event_kind::property, "#address-cells"},
      {fdt_event_kind::property, "compatible"},
      {fdt_event_kind::begin_node, "cpus"},
      {fdt_event_kind::begin_node, "cpu@0"},
      {fdt_event_kind::property, "reg"},
      {fdt_event_kind::end_node, nullptr},
      {fdt_event_kind::end_node, nullptr},
      {fdt_event_kind::end_node, nullptr},
  }};

  std::size_t i = 0;
  for (auto ev : r) {
    ASSERT_TRUE(ev) << "event #" << i << " failed with error " << static_cast<int>(ev.error());
    ASSERT_LT(i, expected.size());
    EXPECT_EQ(ev->kind, expected[i].kind) << "event #" << i;
    switch (ev->kind) {
    case fdt_event_kind::begin_node:
      EXPECT_EQ(ev->node_name, expected[i].name) << "event #" << i;
      break;
    case fdt_event_kind::property:
      EXPECT_EQ(ev->prop.name, expected[i].name) << "event #" << i;
      break;
    case fdt_event_kind::end_node:
      break;
    }
    ++i;
  }
  EXPECT_EQ(i, std::size(expected));
}

TEST(FdtReaderTest, PropertyValueHelpersDecodeCorrectly) {
  auto blob = build_sample_blob();
  auto made = fdt_reader::try_create(span<const std::byte>(blob.data(), blob.size()));
  ASSERT_TRUE(made);
  auto r = std::move(made).value();

  bool saw_address_cells = false;
  bool saw_compatible = false;
  bool saw_reg = false;
  for (auto ev : r) {
    ASSERT_TRUE(ev);
    if (ev->kind != fdt_event_kind::property)
      continue;
    if (ev->prop.name == "#address-cells") {
      auto v = ev->prop.try_as_u32();
      ASSERT_TRUE(v);
      EXPECT_EQ(*v, 2u);
      saw_address_cells = true;
    } else if (ev->prop.name == "compatible") {
      auto v = ev->prop.try_as_string();
      ASSERT_TRUE(v);
      EXPECT_EQ(*v, "linux,dummy-board");
      saw_compatible = true;
    } else if (ev->prop.name == "reg") {
      auto v = ev->prop.try_as_u64();
      ASSERT_TRUE(v);
      EXPECT_EQ(*v, 0u);
      saw_reg = true;
    }
  }
  EXPECT_TRUE(saw_address_cells);
  EXPECT_TRUE(saw_compatible);
  EXPECT_TRUE(saw_reg);
}

TEST(FdtReaderTest, PropertyValueHelpersRejectWrongSizedValues) {
  auto blob = build_sample_blob();
  auto made = fdt_reader::try_create(span<const std::byte>(blob.data(), blob.size()));
  ASSERT_TRUE(made);
  auto r = std::move(made).value();

  for (auto ev : r) {
    ASSERT_TRUE(ev);
    if (ev->kind != fdt_event_kind::property || ev->prop.name != "#address-cells")
      continue;
    // "#address-cells" is a 4-byte <u32>; asking for it as a u64/string must fail cleanly.
    EXPECT_FALSE(ev->prop.try_as_u64());
    EXPECT_FALSE(ev->prop.try_as_string());
  }
}

TEST(FdtReaderTest, UnknownTokenInStructBlockFailsThatEventThenExhausts) {
  auto blob = build_sample_blob();

  // Locate the struct block via the header and stomp the very first token
  // (the root node's FDT_BEGIN_NODE) with an invalid token value.
  auto load_be32 = [&](std::size_t off) {
    return (static_cast<uint32_t>(blob[off]) << 24) | (static_cast<uint32_t>(blob[off + 1]) << 16) |
           (static_cast<uint32_t>(blob[off + 2]) << 8) | static_cast<uint32_t>(blob[off + 3]);
  };
  const uint32_t off_dt_struct = load_be32(8);
  blob[off_dt_struct] = std::byte{0xff};
  blob[off_dt_struct + 1] = std::byte{0xff};
  blob[off_dt_struct + 2] = std::byte{0xff};
  blob[off_dt_struct + 3] = std::byte{0xff};

  auto made = fdt_reader::try_create(span<const std::byte>(blob.data(), blob.size()));
  ASSERT_TRUE(made);
  auto r = std::move(made).value();

  auto first = r.next();
  ASSERT_TRUE(first.has_value());
  EXPECT_FALSE(*first);
  EXPECT_EQ(first->error(), error::invalid_argument);

  // Fused: every subsequent poll must keep returning empty, never re-parsing.
  EXPECT_FALSE(r.next().has_value());
  EXPECT_FALSE(r.next().has_value());
}

TEST(FdtReaderTest, UnbalancedEndNodeFailsThatEventThenExhausts) {
  std::array<std::byte, 256> storage{};
  auto made_writer = fdt_writer::try_create(span<std::byte>(storage.data(), storage.size()));
  auto w = std::move(made_writer).value();
  // A single, correctly-balanced empty root node; we then hand-splice an
  // extra FDT_END_NODE token into the struct block (adjusting every header
  // field that records a size/offset past the insertion point) so the
  // reader sees an END_NODE with depth already back at 0.
  (void)w.begin_node("");
  (void)w.end_node();
  auto blob_r = w.finish();
  ASSERT_TRUE(blob_r);
  std::vector<std::byte> blob(blob_r->begin(), blob_r->end());

  auto load_be32 = [&](std::size_t off) {
    return (static_cast<uint32_t>(blob[off]) << 24) | (static_cast<uint32_t>(blob[off + 1]) << 16) |
           (static_cast<uint32_t>(blob[off + 2]) << 8) | static_cast<uint32_t>(blob[off + 3]);
  };
  auto store_be32 = [&](std::size_t off, uint32_t v) {
    blob[off + 0] = static_cast<std::byte>((v >> 24) & 0xffu);
    blob[off + 1] = static_cast<std::byte>((v >> 16) & 0xffu);
    blob[off + 2] = static_cast<std::byte>((v >> 8) & 0xffu);
    blob[off + 3] = static_cast<std::byte>(v & 0xffu);
  };

  const uint32_t off_dt_struct = load_be32(8);
  const uint32_t off_dt_strings = load_be32(12);
  const uint32_t size_dt_struct = load_be32(36);
  // Struct block layout for `begin_node(""); end_node();` is exactly:
  // FDT_BEGIN_NODE, name (4 padded bytes), FDT_END_NODE, FDT_END -- insert
  // the extra FDT_END_NODE right before the final FDT_END token.
  const std::size_t insert_at = off_dt_struct + size_dt_struct - 4;
  const std::array<std::byte, 4> extra_end_node = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{2}};
  blob.insert(blob.begin() + static_cast<std::ptrdiff_t>(insert_at), extra_end_node.begin(), extra_end_node.end());

  // Fix up every header field that counts bytes at or after the insertion
  // point: totalsize, off_dt_strings (strings follow the struct block),
  // and size_dt_struct.
  store_be32(4, load_be32(4) + 4);
  store_be32(12, off_dt_strings + 4);
  store_be32(36, size_dt_struct + 4);

  auto reader_made = fdt_reader::try_create(span<const std::byte>(blob.data(), blob.size()));
  ASSERT_TRUE(reader_made);
  auto r = std::move(reader_made).value();

  auto begin_ev = r.next();
  ASSERT_TRUE(begin_ev.has_value());
  ASSERT_TRUE(*begin_ev);
  EXPECT_EQ((*begin_ev)->kind, fdt_event_kind::begin_node);

  auto first_end = r.next();
  ASSERT_TRUE(first_end.has_value());
  ASSERT_TRUE(*first_end);
  EXPECT_EQ((*first_end)->kind, fdt_event_kind::end_node);

  auto second_end = r.next();
  ASSERT_TRUE(second_end.has_value());
  EXPECT_FALSE(*second_end);
  EXPECT_EQ(second_end->error(), error::invalid_argument);

  EXPECT_FALSE(r.next().has_value());
}

TEST(FdtReaderTest, InteroperatesWithFdtWriterRoundTripAcrossMultipleSizes) {
  // A larger tree, to exercise more than one struct-block page/alignment
  // boundary and the string-table dedup path together.
  std::vector<std::byte> storage(8192);
  auto made = fdt_writer::try_create(span<std::byte>(storage.data(), storage.size()));
  ASSERT_TRUE(made);
  auto w = std::move(made).value();
  ASSERT_TRUE(w.begin_node(""));
  ASSERT_TRUE(w.property_u32("#address-cells", 2));
  for (int i = 0; i < 16; ++i) {
    std::string name = "node" + std::to_string(i);
    ASSERT_TRUE(w.begin_node(structo::string_view(name.data(), name.size())));
    ASSERT_TRUE(w.property_u32("#address-cells", static_cast<uint32_t>(i))); // shared string-table entry
    ASSERT_TRUE(w.end_node());
  }
  ASSERT_TRUE(w.end_node());
  auto blob_r = w.finish();
  ASSERT_TRUE(blob_r);

  auto reader_made = fdt_reader::try_create(span<const std::byte>(blob_r->data(), blob_r->size()));
  ASSERT_TRUE(reader_made);
  auto r = std::move(reader_made).value();

  int begin_count = 0, end_count = 0, prop_count = 0;
  for (auto ev : r) {
    ASSERT_TRUE(ev);
    switch (ev->kind) {
    case fdt_event_kind::begin_node:
      ++begin_count;
      break;
    case fdt_event_kind::end_node:
      ++end_count;
      break;
    case fdt_event_kind::property:
      ++prop_count;
      EXPECT_EQ(ev->prop.name, "#address-cells");
      break;
    }
  }
  EXPECT_EQ(begin_count, 17); // root + 16 children.
  EXPECT_EQ(end_count, 17);
  EXPECT_EQ(prop_count, 17);
}
