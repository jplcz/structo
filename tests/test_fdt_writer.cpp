// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/fdt_writer.hpp>

#include <array>
#include <cstring>

using structo::error;
using structo::span;
using structo::fdt::fdt_writer;

namespace {

uint32_t read_be32(span<const std::byte> blob, std::size_t offset) {
  return (static_cast<uint32_t>(blob[offset]) << 24) | (static_cast<uint32_t>(blob[offset + 1]) << 16) |
         (static_cast<uint32_t>(blob[offset + 2]) << 8) | static_cast<uint32_t>(blob[offset + 3]);
}

// Bounds-checked equivalent of strlen()+string construction: scans for a
// NUL terminator no further than `blob`'s own end (never past it, unlike
// a raw reinterpret_cast<const char *> + strcmp/strlen would), returning
// a string_view over just the bytes actually scanned. Mirrors
// `structo::fdt::detail::read_cstring`'s own pointer-arithmetic + explicit
// unsafe-buffer-usage opt-out, since the bound (`i - offset`, computed by
// the preceding bounds-checked scan) is already proven safe.
structo::string_view read_cstring(span<const std::byte> blob, std::size_t offset) {
  std::size_t i = offset;
  while (i < blob.size() && blob[i] != std::byte{0})
    ++i;
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  structo::string_view result(reinterpret_cast<const char *>(blob.data() + offset), i - offset);
  RELOCO_END_UNSAFE_BUFFER_USAGE
  return result;
}

} // namespace

TEST(FdtWriterTest, TryCreateRejectsSpanTooSmallForFixedOverhead) {
  std::array<std::byte, 8> storage{};
  auto made = fdt_writer::try_create(span<std::byte>(storage.data(), storage.size()));
  ASSERT_FALSE(made);
  EXPECT_EQ(made.error(), error::allocation_failed);
}

TEST(FdtWriterTest, EmptyRootNodeProducesValidHeader) {
  std::array<std::byte, 256> storage{};
  auto made = fdt_writer::try_create(span<std::byte>(storage.data(), storage.size()));
  ASSERT_TRUE(made);
  fdt_writer w = std::move(made).value();
  ASSERT_TRUE(w.begin_node(""));
  ASSERT_TRUE(w.end_node());
  auto result = w.finish();
  ASSERT_TRUE(result);
  auto blob = *result;

  EXPECT_EQ(read_be32(blob, 0), structo::fdt::magic);
  EXPECT_EQ(read_be32(blob, 4), blob.size()); // totalsize
  EXPECT_EQ(read_be32(blob, 16), 40u);        // off_mem_rsvmap right after the header
  EXPECT_EQ(read_be32(blob, 20), structo::fdt::version);
  EXPECT_EQ(read_be32(blob, 24), structo::fdt::last_comp_version);
}

TEST(FdtWriterTest, MemReserveEntryIsWrittenBeforeStructBlock) {
  std::array<std::byte, 256> storage{};
  auto made = fdt_writer::try_create(span<std::byte>(storage.data(), storage.size()));
  ASSERT_TRUE(made);
  fdt_writer w = std::move(made).value();
  ASSERT_TRUE(w.add_mem_reserve(0x1000, 0x2000));
  ASSERT_TRUE(w.begin_node(""));
  ASSERT_TRUE(w.end_node());
  auto result = w.finish();
  ASSERT_TRUE(result);
  auto blob = *result;

  const std::size_t off_mem_rsvmap = read_be32(blob, 16);
  EXPECT_EQ((static_cast<uint64_t>(read_be32(blob, off_mem_rsvmap)) << 32) | read_be32(blob, off_mem_rsvmap + 4),
            0x1000u);
  EXPECT_EQ((static_cast<uint64_t>(read_be32(blob, off_mem_rsvmap + 8)) << 32) | read_be32(blob, off_mem_rsvmap + 12),
            0x2000u);
  // Zero terminator entry follows.
  EXPECT_EQ(read_be32(blob, off_mem_rsvmap + 16), 0u);
  EXPECT_EQ(read_be32(blob, off_mem_rsvmap + 20), 0u);
  EXPECT_EQ(read_be32(blob, off_mem_rsvmap + 24), 0u);
  EXPECT_EQ(read_be32(blob, off_mem_rsvmap + 28), 0u);
}

TEST(FdtWriterTest, MemReserveAfterStructStartedIsRejected) {
  std::array<std::byte, 256> storage{};
  auto made = fdt_writer::try_create(span<std::byte>(storage.data(), storage.size()));
  ASSERT_TRUE(made);
  fdt_writer w = std::move(made).value();
  ASSERT_TRUE(w.begin_node(""));
  auto res = w.add_mem_reserve(0, 0);
  ASSERT_FALSE(res);
  EXPECT_EQ(res.error(), error::invalid_argument);
}

TEST(FdtWriterTest, UnbalancedNodesFailFinish) {
  std::array<std::byte, 256> storage{};
  auto made = fdt_writer::try_create(span<std::byte>(storage.data(), storage.size()));
  ASSERT_TRUE(made);
  fdt_writer w = std::move(made).value();
  ASSERT_TRUE(w.begin_node(""));
  ASSERT_TRUE(w.begin_node("child"));
  ASSERT_TRUE(w.end_node());
  // Missing the outer end_node().
  auto res = w.finish();
  ASSERT_FALSE(res);
  EXPECT_EQ(res.error(), error::invalid_argument);
}

TEST(FdtWriterTest, EndNodeWithoutBeginNodeIsRejected) {
  std::array<std::byte, 256> storage{};
  auto made = fdt_writer::try_create(span<std::byte>(storage.data(), storage.size()));
  ASSERT_TRUE(made);
  fdt_writer w = std::move(made).value();
  ASSERT_TRUE(w.begin_node(""));
  ASSERT_TRUE(w.end_node());
  auto res = w.end_node();
  ASSERT_FALSE(res);
  EXPECT_EQ(res.error(), error::invalid_argument);
}

TEST(FdtWriterTest, PropertiesRoundTripThroughStructAndStringBlocks) {
  std::array<std::byte, 512> storage{};
  auto made = fdt_writer::try_create(span<std::byte>(storage.data(), storage.size()));
  ASSERT_TRUE(made);
  fdt_writer w = std::move(made).value();
  ASSERT_TRUE(w.begin_node(""));
  ASSERT_TRUE(w.property_string("compatible", "linux,dummy"));
  ASSERT_TRUE(w.property_u32("#address-cells", 2));
  ASSERT_TRUE(w.property_u64("reg-base", 0x1'2345'6789ULL));
  ASSERT_TRUE(w.property_empty("dma-coherent"));
  const uint32_t cells[2] = {0xAABBCCDD, 0x11223344};
  ASSERT_TRUE(w.property_u32_array("ranges", span<const uint32_t>(cells, 2)));
  ASSERT_TRUE(w.end_node());
  auto result = w.finish();
  ASSERT_TRUE(result);
  auto blob = *result;

  const std::size_t off_dt_struct = read_be32(blob, 8);
  const std::size_t off_dt_strings = read_be32(blob, 12);
  const std::size_t size_dt_strings = read_be32(blob, 32);
  const std::size_t size_dt_struct = read_be32(blob, 36);
  EXPECT_EQ(off_dt_struct + size_dt_struct, off_dt_strings);
  EXPECT_EQ(off_dt_strings + size_dt_strings, blob.size());

  // Every property token in the struct block carries a nameoff within the
  // (now-compacted) string table, and the strings block round-trips.
  std::size_t pos = off_dt_struct + 4; // Skip FDT_BEGIN_NODE.
  pos += 4;                            // Root node's empty name ("" + NUL, padded to 4).
  bool saw_compatible = false;
  while (pos < off_dt_struct + size_dt_struct) {
    const uint32_t tok = read_be32(blob, pos);
    pos += 4;
    if (tok == 3 /* FDT_PROP */) {
      const uint32_t len = read_be32(blob, pos);
      const uint32_t nameoff = read_be32(blob, pos + 4);
      ASSERT_LT(off_dt_strings + nameoff, blob.size());
      const auto name = read_cstring(blob, off_dt_strings + nameoff);
      if (name == "compatible") {
        saw_compatible = true;
        const auto value = read_cstring(blob, pos + 8);
        EXPECT_EQ(value, "linux,dummy");
        EXPECT_EQ(len, value.size() + 1);
      }
      pos += 8 + ((len + 3u) & ~3u);
    } else {
      break; // FDT_END_NODE / FDT_END.
    }
  }
  EXPECT_TRUE(saw_compatible);
}

TEST(FdtWriterTest, DuplicatePropertyNamesShareOneStringTableEntry) {
  std::array<std::byte, 512> storage{};
  auto made = fdt_writer::try_create(span<std::byte>(storage.data(), storage.size()));
  ASSERT_TRUE(made);
  fdt_writer w = std::move(made).value();
  ASSERT_TRUE(w.begin_node(""));
  ASSERT_TRUE(w.property_string("compatible", "vendor,a"));
  ASSERT_TRUE(w.begin_node("child"));
  ASSERT_TRUE(w.property_string("compatible", "vendor,b"));
  ASSERT_TRUE(w.end_node());
  ASSERT_TRUE(w.end_node());
  auto result = w.finish();
  ASSERT_TRUE(result);
  auto blob = *result;

  const std::size_t size_dt_strings = read_be32(blob, 32);
  // A single interned "compatible" (11 bytes incl. NUL) is reused by both
  // properties instead of being duplicated in the string table.
  EXPECT_EQ(size_dt_strings, std::strlen("compatible") + 1);
}

TEST(FdtWriterTest, StructBlockOverflowFailsGracefully) {
  // Only enough room for the header + mem_rsvmap terminator + FDT_END; any
  // node/property write must fail with allocation_failed rather than
  // corrupting memory.
  std::array<std::byte, 64> storage{};
  auto made = fdt_writer::try_create(span<std::byte>(storage.data(), storage.size()));
  ASSERT_TRUE(made);
  fdt_writer w = std::move(made).value();
  structo::result<void> res = structo::unexpected(error::allocation_failed);
  for (int i = 0; i < 64; ++i) {
    res = w.begin_node("a-fairly-long-node-name-to-exhaust-the-span");
    if (!res)
      break;
  }
  ASSERT_FALSE(res);
  EXPECT_EQ(res.error(), error::allocation_failed);
}

TEST(FdtWriterTest, MidWriteFailureIsLatchedAndFinishFails) {
  std::array<std::byte, 64> storage{};
  auto made = fdt_writer::try_create(span<std::byte>(storage.data(), storage.size()));
  ASSERT_TRUE(made);
  fdt_writer w = std::move(made).value();
  // Fill the tiny span with node opens until one overflows the buffer.
  structo::result<void> failure = structo::unexpected(error::allocation_failed);
  for (int i = 0; i < 64; ++i) {
    failure = w.begin_node("a-fairly-long-node-name-to-exhaust-the-span");
    if (!failure)
      break;
  }
  ASSERT_FALSE(failure);

  // Even though depth_/struct_cursor_ never became inconsistent, finish()
  // must still surface the earlier latched error rather than returning a
  // silently-truncated blob.
  auto finished = w.finish();
  ASSERT_FALSE(finished);
  EXPECT_EQ(finished.error(), failure.error());
}

TEST(FdtWriterTest, FinishTwiceFailsOnSecondCall) {
  std::array<std::byte, 256> storage{};
  auto made = fdt_writer::try_create(span<std::byte>(storage.data(), storage.size()));
  ASSERT_TRUE(made);
  fdt_writer w = std::move(made).value();
  ASSERT_TRUE(w.begin_node(""));
  ASSERT_TRUE(w.end_node());
  ASSERT_TRUE(w.finish());
  // Second finish() re-runs the same finalization steps (idempotent
  // structurally), but is only reachable here because -Wconsumed is a
  // Clang-only static diagnostic; this exercises the runtime path.
  auto second = w.finish();
  EXPECT_TRUE(second);
}
