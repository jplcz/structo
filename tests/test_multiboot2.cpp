// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/arch/x86/multiboot2.hpp>

#include <vector>

using reloco::error;
using reloco::span;
using structo::arch::x86::make_basic_header;
using structo::arch::x86::multiboot2_boot_info_reader;
using structo::arch::x86::multiboot2_bootloader_magic;
using structo::arch::x86::multiboot2_header_magic;
using structo::arch::x86::multiboot2_mmap_entries;
using structo::arch::x86::multiboot2_tag_type;

namespace {

void put_u32(std::vector<std::byte> &buf, std::uint32_t v) {
  for (int i = 0; i < 4; ++i)
    buf.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}

void put_u64(std::vector<std::byte> &buf, std::uint64_t v) {
  for (int i = 0; i < 8; ++i)
    buf.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
}

void pad_to_8(std::vector<std::byte> &buf) {
  while (buf.size() % 8 != 0)
    buf.push_back(std::byte{0});
}

// Patches the leading `total_size` field (offset 0) to the blob's current length.
void finalize_total_size(std::vector<std::byte> &buf) {
  auto total = static_cast<std::uint32_t>(buf.size());
  buf[0] = static_cast<std::byte>(total & 0xFF);
  buf[1] = static_cast<std::byte>((total >> 8) & 0xFF);
  buf[2] = static_cast<std::byte>((total >> 16) & 0xFF);
  buf[3] = static_cast<std::byte>((total >> 24) & 0xFF);
}

// Builds a minimal, well-formed boot information blob:
//   a "hi\0" cmdline tag, a one-entry memory map tag, then the end tag.
std::vector<std::byte> build_sample_blob() {
  std::vector<std::byte> blob;
  put_u32(blob, 0); // total_size placeholder, patched below.
  put_u32(blob, 0); // reserved

  put_u32(blob, 1); // type = cmdline
  put_u32(blob, 8 + 3);
  blob.push_back(static_cast<std::byte>('h'));
  blob.push_back(static_cast<std::byte>('i'));
  blob.push_back(std::byte{0});
  pad_to_8(blob);

  put_u32(blob, 6); // type = memory_map
  put_u32(blob, 8 + 8 + 24);
  put_u32(blob, 24); // entry_size
  put_u32(blob, 0);  // entry_version
  put_u64(blob, 0x100000);
  put_u64(blob, 0x200000);
  put_u32(blob, 1); // type = available
  put_u32(blob, 0); // reserved
  pad_to_8(blob);

  put_u32(blob, 0); // type = end
  put_u32(blob, 8);

  finalize_total_size(blob);
  return blob;
}

span<const std::byte> as_span(const std::vector<std::byte> &v) { return span<const std::byte>(v.data(), v.size()); }

} // namespace

TEST(Multiboot2Test, TryCreateRejectsSpanTooSmallForHeader) {
  reloco::array<std::byte, 4> storage{};
  auto made = multiboot2_boot_info_reader::try_create(span<const std::byte>(storage.data(), storage.size()));
  ASSERT_FALSE(made);
  EXPECT_EQ(made.error(), error::out_of_bounds);
}

TEST(Multiboot2Test, TryCreateRejectsTotalSizeExceedingSpan) {
  std::vector<std::byte> blob(8, std::byte{0});
  put_u32(blob, 0); // overwritten below
  blob.clear();
  put_u32(blob, 1000); // total_size, far larger than the actual span
  put_u32(blob, 0);
  auto made = multiboot2_boot_info_reader::try_create(as_span(blob));
  ASSERT_FALSE(made);
  EXPECT_EQ(made.error(), error::out_of_bounds);
}

TEST(Multiboot2Test, IteratesCmdlineMemoryMapAndEndTags) {
  auto blob = build_sample_blob();
  auto reader = multiboot2_boot_info_reader::try_create(as_span(blob));
  ASSERT_TRUE(reader);

  bool seen_cmdline = false, seen_mmap = false, seen_end = false;
  for (auto tag : *reader) {
    ASSERT_TRUE(tag);
    if (tag->type == multiboot2_tag_type::cmdline) {
      seen_cmdline = true;
      EXPECT_EQ(tag->payload.size(), 3u);
    } else if (tag->type == multiboot2_tag_type::memory_map) {
      seen_mmap = true;
    } else if (tag->type == multiboot2_tag_type::end) {
      seen_end = true;
    }
  }
  EXPECT_TRUE(seen_cmdline);
  EXPECT_TRUE(seen_mmap);
  EXPECT_TRUE(seen_end);
}

TEST(Multiboot2Test, IteratorExhaustedAfterEndTag) {
  auto blob = build_sample_blob();
  auto reader = multiboot2_boot_info_reader::try_create(as_span(blob));
  ASSERT_TRUE(reader);

  int count = 0;
  while (auto tag = reader->next()) {
    ASSERT_TRUE(*tag);
    ++count;
  }
  EXPECT_EQ(count, 3); // cmdline, memory_map, end.
  EXPECT_FALSE(reader->next().has_value());
}

TEST(Multiboot2Test, MemoryMapEntryDecodesCorrectlyAndReportsAvailability) {
  auto blob = build_sample_blob();
  auto reader = multiboot2_boot_info_reader::try_create(as_span(blob));
  ASSERT_TRUE(reader);

  bool checked = false;
  for (auto tag : *reader) {
    ASSERT_TRUE(tag);
    if (tag->type != multiboot2_tag_type::memory_map)
      continue;
    auto entries = multiboot2_mmap_entries(*tag);
    ASSERT_TRUE(entries);
    int entry_count = 0;
    for (auto entry : *entries) {
      ASSERT_TRUE(entry);
      EXPECT_EQ(entry->base_addr, 0x100000u);
      EXPECT_EQ(entry->length, 0x200000u);
      EXPECT_TRUE(entry->is_available());
      ++entry_count;
    }
    EXPECT_EQ(entry_count, 1);
    checked = true;
  }
  EXPECT_TRUE(checked);
}

TEST(Multiboot2Test, MemoryMapEntryReportsUnavailableForNonAvailableType) {
  std::vector<std::byte> blob;
  put_u32(blob, 0);
  put_u32(blob, 0);

  put_u32(blob, 6);
  put_u32(blob, 8 + 8 + 24);
  put_u32(blob, 24);
  put_u32(blob, 0);
  put_u64(blob, 0x1000);
  put_u64(blob, 0x1000);
  put_u32(blob, structo::arch::x86::multiboot2_mmap_entry::type_reserved);
  put_u32(blob, 0);
  pad_to_8(blob);

  put_u32(blob, 0);
  put_u32(blob, 8);
  finalize_total_size(blob);

  auto reader = multiboot2_boot_info_reader::try_create(as_span(blob));
  ASSERT_TRUE(reader);
  auto tag = reader->next();
  ASSERT_TRUE(tag.has_value());
  ASSERT_TRUE(*tag);
  auto entries = multiboot2_mmap_entries(**tag);
  ASSERT_TRUE(entries);
  auto entry = entries->next();
  ASSERT_TRUE(entry.has_value());
  ASSERT_TRUE(*entry);
  EXPECT_FALSE((*entry)->is_available());
}

TEST(Multiboot2Test, MmapEntriesRejectsNonMemoryMapTag) {
  auto blob = build_sample_blob();
  auto reader = multiboot2_boot_info_reader::try_create(as_span(blob));
  ASSERT_TRUE(reader);
  auto tag = reader->next(); // the cmdline tag.
  ASSERT_TRUE(tag.has_value());
  ASSERT_TRUE(*tag);
  auto entries = multiboot2_mmap_entries(**tag);
  ASSERT_FALSE(entries);
  EXPECT_EQ(entries.error(), error::invalid_argument);
}

TEST(Multiboot2Test, MmapEntriesRejectsEntrySizeTooSmall) {
  std::vector<std::byte> blob;
  put_u32(blob, 0);
  put_u32(blob, 0);

  put_u32(blob, 6);
  put_u32(blob, 8 + 8); // header + entry_size/version, no actual entries.
  put_u32(blob, 16);    // entry_size smaller than the 24 bytes required.
  put_u32(blob, 0);
  pad_to_8(blob);

  put_u32(blob, 0);
  put_u32(blob, 8);
  finalize_total_size(blob);

  auto reader = multiboot2_boot_info_reader::try_create(as_span(blob));
  ASSERT_TRUE(reader);
  auto tag = reader->next();
  ASSERT_TRUE(tag.has_value());
  ASSERT_TRUE(*tag);
  auto entries = multiboot2_mmap_entries(**tag);
  ASSERT_FALSE(entries);
  EXPECT_EQ(entries.error(), error::invalid_argument);
}

TEST(Multiboot2Test, MalformedTagSizeLessThanHeaderReportsInvalidArgument) {
  std::vector<std::byte> blob;
  put_u32(blob, 0);
  put_u32(blob, 0);
  put_u32(blob, 1); // type = cmdline
  put_u32(blob, 4); // size < 8: malformed, can't even cover its own header.
  finalize_total_size(blob);

  auto reader = multiboot2_boot_info_reader::try_create(as_span(blob));
  ASSERT_TRUE(reader);
  auto tag = reader->next();
  ASSERT_TRUE(tag.has_value());
  ASSERT_FALSE(*tag);
  EXPECT_EQ(tag->error(), error::invalid_argument);
  // Permanently exhausted after the first error, matching iterator_adaptor's contract.
  EXPECT_FALSE(reader->next().has_value());
}

TEST(Multiboot2Test, TruncatedTagPayloadReportsOutOfBounds) {
  std::vector<std::byte> blob;
  put_u32(blob, 0);
  put_u32(blob, 0);
  put_u32(blob, 1);  // type = cmdline
  put_u32(blob, 64); // size claims 56 bytes of payload that aren't actually there.
  finalize_total_size(blob);

  auto reader = multiboot2_boot_info_reader::try_create(as_span(blob));
  ASSERT_TRUE(reader);
  auto tag = reader->next();
  ASSERT_TRUE(tag.has_value());
  ASSERT_FALSE(*tag);
  EXPECT_EQ(tag->error(), error::out_of_bounds);
}

TEST(Multiboot2Test, MakeBasicHeaderChecksumSatisfiesSpecInvariant) {
  auto [hdr, end_tag] = make_basic_header();
  EXPECT_EQ(hdr.magic, multiboot2_header_magic);
  EXPECT_EQ(hdr.architecture, structo::arch::x86::multiboot2_architecture_i386);
  EXPECT_EQ(hdr.header_length, sizeof(hdr) + sizeof(end_tag));
  // Per spec: magic + architecture + header_length + checksum == 0 (mod 2^32).
  std::uint32_t sum = hdr.magic + hdr.architecture + hdr.header_length + hdr.checksum;
  EXPECT_EQ(sum, 0u);
  EXPECT_EQ(end_tag.type, 0u);
  EXPECT_EQ(end_tag.size, 8u);
}

TEST(Multiboot2Test, BootloaderMagicConstantMatchesSpec) { EXPECT_EQ(multiboot2_bootloader_magic, 0x36D76289u); }
