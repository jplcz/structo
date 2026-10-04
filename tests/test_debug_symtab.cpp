// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <cstring>
#include <gtest/gtest.h>
#include <map>
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

  // Enables Huffman-coded name records for every push_name() call made
  // after this (raw bytes otherwise, as today). `length_counts`/
  // `sorted_symbols`/`codes` must together describe one valid canonical
  // Huffman code (as `scripts/elf_symtab_to_blob.py`'s encoder would
  // build), caller-computed so each test can hand-pick a small,
  // easy-to-verify alphabet.
  blob_builder &enable_huffman(std::uint8_t max_code_len, std::vector<std::uint8_t> length_counts,
                               std::vector<std::uint8_t> sorted_symbols,
                               std::map<std::uint8_t, std::pair<std::uint32_t, std::uint8_t>> codes) {
    huffman_enabled_ = true;
    huffman_max_code_len_ = max_code_len;
    huffman_codes_ = std::move(codes);
    huffman_table_.push_back(static_cast<std::byte>(max_code_len));
    for (auto c : length_counts)
      huffman_table_.push_back(static_cast<std::byte>(c));
    for (auto s : sorted_symbols)
      huffman_table_.push_back(static_cast<std::byte>(s));
    huffman_symbol_count_ = static_cast<std::uint16_t>(sorted_symbols.size());
    return *this;
  }

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

    const std::size_t huffman_table_offset = huffman_enabled_ ? fmt::header_size : 0;
    const std::size_t checkpoint_table_offset = fmt::header_size + huffman_table_.size();
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
    put32(blob, fmt::header_offset::huffman_table_offset, static_cast<std::uint32_t>(huffman_table_offset));
    put32(blob, fmt::header_offset::huffman_table_size, static_cast<std::uint32_t>(huffman_table_.size()));
    put16(blob, fmt::header_offset::huffman_symbol_count, huffman_symbol_count_);

    for (std::size_t i = 0; i < huffman_table_.size(); ++i)
      blob[huffman_table_offset + i] = huffman_table_[i];
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
  // Bit-packs one name record's symbols MSB-first, byte-aligning at the
  // end -- the test-side mirror of `scripts/elf_symtab_to_blob.py`'s
  // `_BitWriter` and `structo::debug_symtab::detail::bit_reader`.
  class bit_writer {
  public:
    void write_bits(std::uint32_t value, std::uint8_t length) {
      for (int i = length - 1; i >= 0; --i) {
        cur_ = static_cast<std::uint8_t>((cur_ << 1) | ((value >> i) & 1u));
        if (++nbits_ == 8) {
          out.push_back(static_cast<std::byte>(cur_));
          cur_ = 0;
          nbits_ = 0;
        }
      }
    }
    void align() {
      if (nbits_ != 0) {
        out.push_back(static_cast<std::byte>(cur_ << (8 - nbits_)));
        cur_ = 0;
        nbits_ = 0;
      }
    }
    std::vector<std::byte> out;

  private:
    std::uint8_t cur_{0};
    unsigned nbits_{0};
  };

  void push_name(const char *name) {
    const std::size_t len = std::strlen(name);
    stream_.push_back(static_cast<std::byte>(len & 0x7F));
    if (!huffman_enabled_) {
      for (std::size_t i = 0; i < len; ++i)
        stream_.push_back(static_cast<std::byte>(name[i]));
      return;
    }
    bit_writer writer;
    for (std::size_t i = 0; i < len; ++i) {
      const auto [code, bits] = huffman_codes_.at(static_cast<std::uint8_t>(name[i]));
      writer.write_bits(code, bits);
    }
    writer.align();
    for (auto b : writer.out)
      stream_.push_back(b);
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
  bool huffman_enabled_{false};
  std::uint8_t huffman_max_code_len_{0};
  std::uint16_t huffman_symbol_count_{0};
  std::vector<std::byte> huffman_table_{};
  std::map<std::uint8_t, std::pair<std::uint32_t, std::uint8_t>> huffman_codes_{};
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

namespace {

// A tiny, hand-picked 2-symbol canonical Huffman code ('a' -> 0, 'b' ->
// 1, both 1 bit) -- the simplest nondegenerate alphabet, used by several
// tests below.
blob_builder make_two_symbol_huffman_builder() {
  blob_builder b;
  b.group_size(2);
  b.enable_huffman(/*max_code_len=*/1, /*length_counts=*/{2}, /*sorted_symbols=*/{'a', 'b'},
                   /*codes=*/{{'a', {0, 1}}, {'b', {1, 1}}});
  return b;
}

// A 3-symbol canonical Huffman code ('a' -> 0 (1 bit), 'b' -> 10, 'c' ->
// 11 (2 bits each)) -- the standard optimal code for frequencies
// roughly 4:2:1, used by the byte-boundary-crossing test below.
blob_builder make_three_symbol_huffman_builder() {
  blob_builder b;
  b.group_size(2);
  b.enable_huffman(/*max_code_len=*/2, /*length_counts=*/{1, 2}, /*sorted_symbols=*/{'a', 'b', 'c'},
                   /*codes=*/{{'a', {0, 1}}, {'b', {2, 2}}, {'c', {3, 2}}});
  return b;
}

} // namespace

TEST(DebugSymtabHuffmanTest, SingleBitCodeDecodesLiteralName) {
  auto b = make_two_symbol_huffman_builder();
  b.begin_group(0x1000, "abba").end_group();
  auto blob = b.build();

  auto view = debug_symtab_view::try_create(span<const std::byte>(blob.data(), blob.size()));
  ASSERT_TRUE(view.has_value());
  EXPECT_EQ(view.value().symbol_count(), 1u);

  char scratch[64];
  auto r = view.value().try_resolve(0x1000, span<char>(scratch, sizeof(scratch)));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(resolved_name(r.value()), "abba");
}

TEST(DebugSymtabHuffmanTest, RepeatPreviousStillWorksWithHuffmanCodedLiteral) {
  auto b = make_two_symbol_huffman_builder();
  b.begin_group(0x1000, "aabb").add_repeat_entry(0x1010).end_group();
  auto blob = b.build();

  auto view = debug_symtab_view::try_create(span<const std::byte>(blob.data(), blob.size()));
  ASSERT_TRUE(view.has_value());

  char scratch[64];
  auto first = view.value().try_resolve(0x1000, span<char>(scratch, sizeof(scratch)));
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(resolved_name(first.value()), "aabb");

  auto second = view.value().try_resolve(0x1010, span<char>(scratch, sizeof(scratch)));
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(resolved_name(second.value()), "aabb");
  EXPECT_TRUE(second.value().is_exact);
}

TEST(DebugSymtabHuffmanTest, NameCrossingMultipleByteBoundariesDecodesCorrectly) {
  auto b = make_three_symbol_huffman_builder();
  // 10 'c's at 2 bits each = 20 bits, spanning 3 packed bytes.
  b.begin_group(0x2000, "cccccccccc").end_group();
  auto blob = b.build();

  auto view = debug_symtab_view::try_create(span<const std::byte>(blob.data(), blob.size()));
  ASSERT_TRUE(view.has_value());

  char scratch[64];
  auto r = view.value().try_resolve(0x2000, span<char>(scratch, sizeof(scratch)));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(resolved_name(r.value()), "cccccccccc");
}

TEST(DebugSymtabHuffmanTest, RecordAfterHuffmanCodedNameStaysByteAligned) {
  auto b = make_three_symbol_huffman_builder();
  // "cccccccccc" (20 bits, non-byte-multiple) followed by a second,
  // differently-shaped entry in the same group -- only decodes
  // correctly if the first record's bit-packing padded back out to a
  // byte boundary before the next entry's ULEB128 delta.
  b.begin_group(0x2000, "cccccccccc").add_entry(0x2010, "abc").end_group();
  auto blob = b.build();

  auto view = debug_symtab_view::try_create(span<const std::byte>(blob.data(), blob.size()));
  ASSERT_TRUE(view.has_value());

  char scratch[64];
  auto r = view.value().try_resolve(0x2010, span<char>(scratch, sizeof(scratch)));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(resolved_name(r.value()), "abc");
  EXPECT_TRUE(r.value().is_exact);
}

TEST(DebugSymtabHuffmanTest, MaximalLengthNameDecodesCorrectly) {
  auto b = make_two_symbol_huffman_builder();
  std::string name(127, 'a');
  name[126] = 'b';
  b.begin_group(0x3000, name.c_str()).end_group();
  auto blob = b.build();

  auto view = debug_symtab_view::try_create(span<const std::byte>(blob.data(), blob.size()));
  ASSERT_TRUE(view.has_value());

  char scratch[200];
  auto r = view.value().try_resolve(0x3000, span<char>(scratch, sizeof(scratch)));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(resolved_name(r.value()), name);
}

TEST(DebugSymtabHuffmanTest, TryCreateRejectsHuffmanTableSizeMismatch) {
  auto b = make_two_symbol_huffman_builder();
  b.begin_group(0x1000, "abba").end_group();
  auto blob = b.build();

  // huffman_table_size no longer matches 1 + max_code_len + symbol_count.
  blob[fmt::header_offset::huffman_table_size] ^= std::byte{0xFF};
  auto view = debug_symtab_view::try_create(span<const std::byte>(blob.data(), blob.size()), /*verify_crc=*/false);
  EXPECT_FALSE(view.has_value());
}

TEST(DebugSymtabHuffmanTest, TryCreateRejectsLengthCountsNotSummingToSymbolCount) {
  auto b = make_two_symbol_huffman_builder();
  b.begin_group(0x1000, "abba").end_group();
  auto blob = b.build();

  // Corrupt length_counts[1] (the single byte right after max_code_len
  // at huffman_table_offset) so it no longer sums to huffman_symbol_count.
  const auto huffman_table_offset =
      fmt::load_le32(blob.data() + fmt::header_offset::huffman_table_offset);
  blob[huffman_table_offset + 1] = std::byte{1};
  auto view = debug_symtab_view::try_create(span<const std::byte>(blob.data(), blob.size()), /*verify_crc=*/false);
  EXPECT_FALSE(view.has_value());
}

TEST(DebugSymtabHuffmanTest, TryCreateRejectsZeroMaxCodeLen) {
  auto b = make_two_symbol_huffman_builder();
  b.begin_group(0x1000, "abba").end_group();
  auto blob = b.build();

  const auto huffman_table_offset =
      fmt::load_le32(blob.data() + fmt::header_offset::huffman_table_offset);
  blob[huffman_table_offset] = std::byte{0};
  auto view = debug_symtab_view::try_create(span<const std::byte>(blob.data(), blob.size()), /*verify_crc=*/false);
  EXPECT_FALSE(view.has_value());
}
