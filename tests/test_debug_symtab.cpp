// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <cstring>
#include <gtest/gtest.h>
#include <reloco/vector.hpp>
#include <string>
#include <structo/debug_symtab.hpp>
#include <structo/detail/debug_symtab_format.hpp>
#include <utility>
#include <vector>

using reloco::span;
using reloco::vector;
using structo::debug_symtab_view;
namespace fmt = structo::debug_symtab::detail;

namespace {

// A hand-rolled, minimal `DSYM` blob builder covering exactly the
// knobs these tests exercise (fixed addr_width = 8, one truncation
// marker, caller-controlled group boundaries) -- not a general-purpose
// encoder (that's `scripts/elf_symtab_to_blob.py`'s job), just enough
// to build well-formed test fixtures without duplicating
// `docs/debug_symtab_format.md`'s encoding rules by hand in every test.
class blob_builder {
public:
  explicit blob_builder(std::uint8_t max_name_len = 31, std::uint8_t truncation_marker = '~')
      : max_name_len_(max_name_len), truncation_marker_(truncation_marker) {}

  // Starts a new checkpoint group whose first entry is at `addr` with
  // `name`. Every group must start with begin_group(); subsequent
  // entries in the same group are added with add_entry().
  blob_builder &begin_group(std::uint64_t addr, const char *name) {
    checkpoints_.push_back({addr, stream_.size()});
    group_start_addr_ = addr;
    push_name(name);
    ++symbol_count_;
    return *this;
  }

  // Adds a subsequent entry within the current group at `addr` (must be
  // strictly greater than the previous entry's address in this group).
  blob_builder &add_entry(std::uint64_t addr, const char *name) {
    push_uleb128(addr - group_start_addr_ - running_delta_base_);
    running_delta_base_ = addr - group_start_addr_;
    push_name(name);
    ++symbol_count_;
    return *this;
  }

  // Adds a subsequent entry whose name should be encoded as
  // repeat_previous (must match the immediately preceding entry's name).
  blob_builder &add_repeat_entry(std::uint64_t addr) {
    push_uleb128(addr - group_start_addr_ - running_delta_base_);
    running_delta_base_ = addr - group_start_addr_;
    stream_.push_back(static_cast<std::byte>(fmt::name_control_repeat_flag));
    ++symbol_count_;
    return *this;
  }

  void end_group() { running_delta_base_ = 0; }

  vector<std::byte> build(bool build_id_present = false, bool corrupt_crc = false) {
    std::vector<std::byte> build_id;
    if (build_id_present)
      for (std::uint8_t b : {std::uint8_t{0x11}, std::uint8_t{0x22}, std::uint8_t{0x33}, std::uint8_t{0x44}})
        build_id.push_back(std::byte{b});

    const std::size_t checkpoint_table_offset = fmt::header_size;
    const std::size_t checkpoint_table_bytes = checkpoints_.size() * fmt::checkpoint_record_size(8);
    const std::size_t build_id_offset = checkpoint_table_offset + checkpoint_table_bytes;
    const std::size_t entry_stream_offset = build_id_offset + build_id.size();
    const std::size_t total_size = entry_stream_offset + stream_.size();

    auto blob = vector<std::byte>::try_create(total_size).value();
    (void)blob.try_resize(total_size, std::byte{0});

    put32(blob, fmt::header_offset::magic, fmt::magic);
    blob[fmt::header_offset::addr_width] = std::byte{8};
    blob[fmt::header_offset::max_name_len] = std::byte{max_name_len_};
    blob[fmt::header_offset::truncation_marker] = std::byte{truncation_marker_};
    put16(blob, fmt::header_offset::group_size, group_size_);
    put32(blob, fmt::header_offset::symbol_count, static_cast<std::uint32_t>(symbol_count_));
    put32(blob, fmt::header_offset::checkpoint_count, static_cast<std::uint32_t>(checkpoints_.size()));
    put32(blob, fmt::header_offset::checkpoint_table_offset, static_cast<std::uint32_t>(checkpoint_table_offset));
    put32(blob, fmt::header_offset::entry_stream_offset, static_cast<std::uint32_t>(entry_stream_offset));
    put32(blob, fmt::header_offset::entry_stream_size, static_cast<std::uint32_t>(stream_.size()));
    put32(blob, fmt::header_offset::build_id_offset, static_cast<std::uint32_t>(build_id_offset));
    blob[fmt::header_offset::build_id_size] = std::byte{static_cast<std::uint8_t>(build_id.size())};

    for (std::size_t i = 0; i < build_id.size(); ++i)
      blob[build_id_offset + i] = build_id[i];
    for (std::size_t i = 0; i < stream_.size(); ++i)
      blob[entry_stream_offset + i] = stream_[i];
    for (std::size_t i = 0; i < checkpoints_.size(); ++i) {
      const std::size_t rec = checkpoint_table_offset + i * fmt::checkpoint_record_size(8);
      put64(blob, rec, checkpoints_[i].first);
      put32(blob, rec + 8, static_cast<std::uint32_t>(checkpoints_[i].second));
    }

    span<const std::byte> payload(blob.data() + fmt::header_size, blob.size() - fmt::header_size);
    std::uint32_t crc = fmt::crc32_ieee(payload);
    if (corrupt_crc)
      crc ^= 0xFFFFFFFFu;
    put32(blob, fmt::header_offset::payload_crc32, crc);

    return blob;
  }

  blob_builder &group_size(std::uint16_t n) {
    group_size_ = n;
    return *this;
  }

private:
  void push_name(const char *name) {
    const std::size_t len = std::strlen(name);
    stream_.push_back(static_cast<std::byte>(len & 0x7F));
    for (std::size_t i = 0; i < len; ++i)
      stream_.push_back(static_cast<std::byte>(name[i]));
  }

  void push_uleb128(std::uint64_t value) {
    for (;;) {
      std::uint8_t byte = value & 0x7Fu;
      value >>= 7;
      if (value) {
        stream_.push_back(static_cast<std::byte>(byte | 0x80u));
      } else {
        stream_.push_back(static_cast<std::byte>(byte));
        return;
      }
    }
  }

  static void put16(vector<std::byte> &blob, std::size_t off, std::uint16_t v) {
    blob[off] = static_cast<std::byte>(v & 0xFF);
    blob[off + 1] = static_cast<std::byte>((v >> 8) & 0xFF);
  }
  static void put32(vector<std::byte> &blob, std::size_t off, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
      blob[off + i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
  }
  static void put64(vector<std::byte> &blob, std::size_t off, std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
      blob[off + i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
  }

  std::uint8_t max_name_len_;
  std::uint8_t truncation_marker_;
  std::uint16_t group_size_{2};
  std::size_t symbol_count_{0};
  std::uint64_t group_start_addr_{0};
  std::uint64_t running_delta_base_{0};
  std::vector<std::pair<std::uint64_t, std::size_t>> checkpoints_{};
  std::vector<std::byte> stream_{};
};

// Builds the standard three-group fixture used by most tests below:
//   group0: 0x1000 "foo", 0x1010 "bar"
//   group1: 0x1020 "baz", 0x1021 "baz" (repeat_previous)
//   group2: 0x1030 "qux"
vector<std::byte> make_standard_blob() {
  blob_builder b;
  b.group_size(2);
  b.begin_group(0x1000, "foo").add_entry(0x1010, "bar").end_group();
  b.begin_group(0x1020, "baz").add_repeat_entry(0x1021).end_group();
  b.begin_group(0x1030, "qux").end_group();
  return b.build();
}

std::string resolved_name(const debug_symtab_view::resolved &r) { return std::string(r.name.data(), r.name.size()); }

} // namespace

TEST(DebugSymtabTest, TryCreateAcceptsWellFormedBlob) {
  auto blob = make_standard_blob();
  auto view = debug_symtab_view::try_create(span<const std::byte>(blob.data(), blob.size()));
  ASSERT_TRUE(view.has_value());
  EXPECT_TRUE(view.value().valid());
  EXPECT_EQ(view.value().symbol_count(), 5u);
  EXPECT_EQ(view.value().max_name_len(), 31u);
  EXPECT_EQ(view.value().addr_width(), 8u);
}

TEST(DebugSymtabTest, TryCreateRejectsTooShortBlob) {
  vector<std::byte> blob = vector<std::byte>::try_create(10).value();
  (void)blob.try_resize(10, std::byte{0});
  auto view = debug_symtab_view::try_create(span<const std::byte>(blob.data(), blob.size()));
  EXPECT_FALSE(view.has_value());
}

TEST(DebugSymtabTest, TryCreateRejectsBadMagic) {
  auto blob = make_standard_blob();
  blob[0] ^= std::byte{0xFF};
  auto view = debug_symtab_view::try_create(span<const std::byte>(blob.data(), blob.size()));
  EXPECT_FALSE(view.has_value());
}

TEST(DebugSymtabTest, TryCreateRejectsBadAddrWidth) {
  auto blob = make_standard_blob();
  blob[fmt::header_offset::addr_width] = std::byte{5};
  auto view = debug_symtab_view::try_create(span<const std::byte>(blob.data(), blob.size()));
  EXPECT_FALSE(view.has_value());
}

TEST(DebugSymtabTest, TryCreateRejectsCrcMismatch) {
  auto blob = make_standard_blob();
  blob[fmt::header_size] ^= std::byte{0xFF}; // Corrupt one payload byte.
  auto view = debug_symtab_view::try_create(span<const std::byte>(blob.data(), blob.size()));
  EXPECT_FALSE(view.has_value());
}

TEST(DebugSymtabTest, TryCreateSkipsCrcCheckWhenDisabled) {
  auto blob = make_standard_blob();
  blob[fmt::header_size] ^= std::byte{0xFF};
  auto view = debug_symtab_view::try_create(span<const std::byte>(blob.data(), blob.size()), /*verify_crc=*/false);
  EXPECT_TRUE(view.has_value());
}

TEST(DebugSymtabTest, TryCreateRejectsOutOfBoundsOffsets) {
  auto blob = make_standard_blob();
  // Point entry_stream_offset far past the blob's actual size.
  blob[fmt::header_offset::entry_stream_offset] = std::byte{0xFF};
  blob[fmt::header_offset::entry_stream_offset + 1] = std::byte{0xFF};
  auto view = debug_symtab_view::try_create(span<const std::byte>(blob.data(), blob.size()));
  EXPECT_FALSE(view.has_value());
}

class DebugSymtabResolveTest : public ::testing::Test {
protected:
  void SetUp() override {
    blob_ = make_standard_blob();
    auto result = debug_symtab_view::try_create(span<const std::byte>(blob_.data(), blob_.size()));
    ASSERT_TRUE(result.has_value());
    view_ = result.value();
  }

  vector<std::byte> blob_{vector<std::byte>::try_create().value()};
  debug_symtab_view view_{};
  char scratch_[64]{};
};

TEST_F(DebugSymtabResolveTest, ExactMatchOnFirstEntryOfFirstGroup) {
  auto r = view_.try_resolve(0x1000, span<char>(scratch_, sizeof(scratch_)));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(resolved_name(r.value()), "foo");
  EXPECT_EQ(r.value().symbol_base, 0x1000u);
  EXPECT_TRUE(r.value().is_exact);
}

TEST_F(DebugSymtabResolveTest, ExactMatchOnSecondEntryOfFirstGroup) {
  auto r = view_.try_resolve(0x1010, span<char>(scratch_, sizeof(scratch_)));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(resolved_name(r.value()), "bar");
  EXPECT_TRUE(r.value().is_exact);
}

TEST_F(DebugSymtabResolveTest, MidGroupAddressResolvesToPrecedingSymbol) {
  auto r = view_.try_resolve(0x1005, span<char>(scratch_, sizeof(scratch_)));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(resolved_name(r.value()), "foo");
  EXPECT_EQ(r.value().symbol_base, 0x1000u);
  EXPECT_FALSE(r.value().is_exact);
}

TEST_F(DebugSymtabResolveTest, AddressBetweenGroupsResolvesToLastEntryOfPrecedingGroup) {
  auto r = view_.try_resolve(0x1018, span<char>(scratch_, sizeof(scratch_)));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(resolved_name(r.value()), "bar");
  EXPECT_EQ(r.value().symbol_base, 0x1010u);
  EXPECT_FALSE(r.value().is_exact);
}

TEST_F(DebugSymtabResolveTest, FirstEntryOfNonFirstGroupResolvesCorrectly) {
  auto r = view_.try_resolve(0x1020, span<char>(scratch_, sizeof(scratch_)));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(resolved_name(r.value()), "baz");
  EXPECT_TRUE(r.value().is_exact);
}

TEST_F(DebugSymtabResolveTest, RepeatPreviousNameDecodesToSameNameAsPriorEntry) {
  auto r = view_.try_resolve(0x1021, span<char>(scratch_, sizeof(scratch_)));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(resolved_name(r.value()), "baz");
  EXPECT_EQ(r.value().symbol_base, 0x1021u);
  EXPECT_TRUE(r.value().is_exact);
}

TEST_F(DebugSymtabResolveTest, LastEntryOfLastGroupResolvesAndHandlesAfterLastLookup) {
  auto exact = view_.try_resolve(0x1030, span<char>(scratch_, sizeof(scratch_)));
  ASSERT_TRUE(exact.has_value());
  EXPECT_EQ(resolved_name(exact.value()), "qux");
  EXPECT_TRUE(exact.value().is_exact);

  auto after = view_.try_resolve(0x9999, span<char>(scratch_, sizeof(scratch_)));
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(resolved_name(after.value()), "qux");
  EXPECT_FALSE(after.value().is_exact);
}

TEST_F(DebugSymtabResolveTest, AddressBeforeFirstSymbolFailsToResolve) {
  auto r = view_.try_resolve(0x500, span<char>(scratch_, sizeof(scratch_)));
  EXPECT_FALSE(r.has_value());
}

TEST_F(DebugSymtabResolveTest, NameIsCopiedIntoCallerScratchBoundedBySmallerBuffer) {
  char tiny[2];
  auto r = view_.try_resolve(0x1000, span<char>(tiny, sizeof(tiny)));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r.value().name.size(), 2u);
  EXPECT_EQ(std::string(r.value().name.data(), r.value().name.size()), "fo");
}

TEST(DebugSymtabTruncationTest, TruncatedNameIsFlaggedWhenScratchFitsFullMarker) {
  blob_builder b(/*max_name_len=*/5, /*truncation_marker=*/'~');
  b.group_size(1);
  // "abcde" is exactly 5 bytes: the 5th/last byte is the truncation
  // marker, simulating a name the encoder cut to fit max_name_len.
  b.begin_group(0x2000, "abcd~").end_group();
  auto blob = b.build();

  auto view = debug_symtab_view::try_create(span<const std::byte>(blob.data(), blob.size()));
  ASSERT_TRUE(view.has_value());

  char scratch[64];
  auto r = view.value().try_resolve(0x2000, span<char>(scratch, sizeof(scratch)));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(resolved_name(r.value()), "abcd~");
  EXPECT_TRUE(r.value().name_truncated);
}

TEST(DebugSymtabBuildIdTest, TryMatchesBuildIdReturnsTrueWhenBlobHasNoBuildId) {
  auto blob = make_standard_blob();
  auto view = debug_symtab_view::try_create(span<const std::byte>(blob.data(), blob.size()));
  ASSERT_TRUE(view.has_value());

  std::byte running_id[4] = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  EXPECT_TRUE(view.value().try_matches_build_id(span<const std::byte>(running_id, 4)));
}

TEST(DebugSymtabBuildIdTest, TryMatchesBuildIdComparesByteForByte) {
  blob_builder b;
  b.group_size(2);
  b.begin_group(0x1000, "foo").end_group();
  auto blob = b.build(/*build_id_present=*/true);

  auto view = debug_symtab_view::try_create(span<const std::byte>(blob.data(), blob.size()));
  ASSERT_TRUE(view.has_value());

  std::byte matching[4] = {std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
  EXPECT_TRUE(view.value().try_matches_build_id(span<const std::byte>(matching, 4)));

  std::byte mismatching[4] = {std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x45}};
  EXPECT_FALSE(view.value().try_matches_build_id(span<const std::byte>(mismatching, 4)));

  std::byte wrong_length[3] = {std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
  EXPECT_FALSE(view.value().try_matches_build_id(span<const std::byte>(wrong_length, 3)));
}
