// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file debug_symtab_format.hpp
 * @brief Shared `DSYM` (`"DSY2"`) debug symbol table blob binary-format
 * constants and little-endian codec primitives, used by
 * `structo/debug_symtab.hpp`'s decoder (and mirrored, independently, by
 * `scripts/elf_symtab_to_blob.py`'s encoder) so the two never drift apart
 * on header layout, field offsets, or byte order.
 *
 * See `docs/debug_symtab_format.md` for the full format specification
 * this header is a direct transcription of.
 *
 * All multi-byte fields are little-endian, regardless of host/target
 * endianness -- the decoder always reads through the explicit
 * `load_le16`/`load_le32` helpers below rather than aliasing a struct
 * over the blob, so it behaves identically on a big-endian target. No
 * floating point is used anywhere in this file.
 */

#include <cstddef>
#include <cstdint>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/span.hpp>

// Raw byte-level field extraction throughout, exactly like
// detail/fdt_format.hpp; treated as a single checked boundary (every
// caller bounds-checks the span before calling into this header).
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

namespace structo::debug_symtab::detail {

/** @brief `"DSY2"`, as the little-endian `uint32_t` its 4 ASCII bytes
 * decode to. Identifies both the format and its version 2 in one check;
 * version 1 (`"DSY1"`, uncompressed raw name bytes) is superseded by
 * this Huffman-coded name record format rather than kept as a parallel
 * decode path, so an old decoder's magic check rejects a v2 blob
 * outright instead of silently misreading it. */
inline constexpr std::uint32_t magic = 0x32595344u;

/** @brief Fixed header size in bytes; see @ref header_offset for the
 * field layout within it. */
inline constexpr std::size_t header_size = 56;

/** @brief Maximum encodable name length (7-bit `length` field in a name
 * record's control byte). */
inline constexpr std::uint8_t max_name_len_limit = 127;

/** @brief Longest Huffman code length a `huffman_table` may declare
 * (`max_code_len`), same cap DEFLATE uses for its own dynamic Huffman
 * blocks; also the upper bound on the decode loop's per-symbol work. */
inline constexpr std::uint8_t huffman_max_code_len_limit = 15;

/** @brief Byte offsets of every header field, matching
 * `docs/debug_symtab_format.md`'s header table exactly. */
namespace header_offset {
inline constexpr std::size_t magic = 0;                      // u32
inline constexpr std::size_t addr_width = 4;                 // u8
inline constexpr std::size_t max_name_len = 5;                // u8
inline constexpr std::size_t truncation_marker = 6;           // u8
inline constexpr std::size_t reserved0 = 7;                   // u8
inline constexpr std::size_t group_size = 8;                  // u16
inline constexpr std::size_t reserved1 = 10;                  // u16
inline constexpr std::size_t symbol_count = 12;               // u32
inline constexpr std::size_t checkpoint_count = 16;           // u32
inline constexpr std::size_t checkpoint_table_offset = 20;    // u32
inline constexpr std::size_t entry_stream_offset = 24;        // u32
inline constexpr std::size_t entry_stream_size = 28;          // u32
inline constexpr std::size_t build_id_offset = 32;            // u32
inline constexpr std::size_t build_id_size = 36;               // u8
inline constexpr std::size_t reserved2 = 37;                   // u8[3]
inline constexpr std::size_t payload_crc32 = 40;                // u32
inline constexpr std::size_t huffman_table_offset = 44;         // u32
inline constexpr std::size_t huffman_table_size = 48;           // u32
inline constexpr std::size_t huffman_symbol_count = 52;         // u16
inline constexpr std::size_t reserved3 = 54;                    // u16
} // namespace header_offset

/** @brief Per-checkpoint-record field offsets, relative to the start of
 * that record (`checkpoint_table_offset + index * record_size(addr_width)`). */
namespace checkpoint_offset {
inline constexpr std::size_t address = 0; // addr_width bytes
// `stream_offset` (u32) immediately follows the address field.
} // namespace checkpoint_offset

/** @brief Size in bytes of one checkpoint table record. */
[[nodiscard]] inline constexpr std::size_t checkpoint_record_size(std::uint8_t addr_width) noexcept {
  return static_cast<std::size_t>(addr_width) + 4;
}

/** @brief Name record control-byte bit layout. */
inline constexpr std::uint8_t name_control_repeat_flag = 0x80u;
inline constexpr std::uint8_t name_control_length_mask = 0x7Fu;

[[nodiscard]] inline std::uint16_t load_le16(const std::byte *p) noexcept {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8));
}

[[nodiscard]] inline std::uint32_t load_le32(const std::byte *p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

[[nodiscard]] inline std::uint64_t load_le64(const std::byte *p) noexcept {
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < 8; ++i)
    v |= static_cast<std::uint64_t>(p[i]) << (8u * i);
  return v;
}

/** @brief Loads an `addr_width`-byte (4 or 8) little-endian address. The
 * caller has already validated `addr_width` is one of those two values. */
[[nodiscard]] inline std::uint64_t load_le_addr(const std::byte *p, std::uint8_t addr_width) noexcept {
  return addr_width == 8 ? load_le64(p) : static_cast<std::uint64_t>(load_le32(p));
}

/** @brief Reads an unsigned LEB128 varint starting at `*pos` within
 * `region`, advancing `*pos` past it.
 * @return The decoded value, or `error::out_of_bounds` if the varint runs
 * past `region`'s end before its terminating (high-bit-clear) byte, or
 * `error::invalid_argument` if it would overflow a `uint64_t` (more than
 * 10 continuation bytes). */
[[nodiscard]] inline reloco::result<std::uint64_t> read_uleb128(reloco::span<const std::byte> region,
                                                                std::size_t &pos) noexcept {
  std::uint64_t value = 0;
  unsigned shift = 0;
  std::size_t i = pos;
  for (;;) {
    if (i >= region.size())
      return reloco::unexpected(reloco::error::out_of_bounds);
    const auto byte = static_cast<std::uint8_t>(region[i]);
    ++i;
    if (shift >= 64 || (shift == 63 && byte > 1))
      return reloco::unexpected(reloco::error::invalid_argument);
    value |= static_cast<std::uint64_t>(byte & 0x7Fu) << shift;
    if ((byte & 0x80u) == 0) {
      pos = i;
      return value;
    }
    shift += 7;
  }
}

/** @brief MSB-first bit cursor over a byte span, used to decode the
 * Huffman-packed symbols of one name record (see
 * `docs/debug_symtab_format.md`'s "Name records" section). A record's
 * packed bits always start and end byte-aligned, so a fresh
 * `bit_reader` is constructed per record rather than threading bit
 * position across records. */
struct bit_reader {
  reloco::span<const std::byte> stream;
  std::size_t byte_pos{0};
  unsigned bit_pos{0}; // 0..7; which bit of stream[byte_pos] is next (MSB = 0).

  /** @brief Reads the next bit, or `error::out_of_bounds` once `stream`
   * is exhausted. */
  [[nodiscard]] reloco::result<unsigned> next_bit() noexcept {
    if (byte_pos >= stream.size())
      return reloco::unexpected(reloco::error::out_of_bounds);
    const auto byte = static_cast<std::uint8_t>(stream[byte_pos]);
    const unsigned bit = (byte >> (7u - bit_pos)) & 1u;
    ++bit_pos;
    if (bit_pos == 8) {
      bit_pos = 0;
      ++byte_pos;
    }
    return bit;
  }

  /** @brief Advances past any partially-consumed byte, so the caller's
   * own `pos` (tracking the record after this one) resumes byte-aligned. */
  void align() noexcept {
    if (bit_pos != 0) {
      bit_pos = 0;
      ++byte_pos;
    }
  }
};

/** @brief Decodes one canonical-Huffman-coded symbol from `reader`,
 * given the blob's per-length symbol-count histogram (`length_counts`,
 * one byte per length `1..=max_code_len`) and canonically-ordered
 * alphabet (`sorted_symbols`, ascending by length then by byte value) --
 * see `docs/debug_symtab_format.md`'s "Huffman decode table" section.
 *
 * Standard canonical-Huffman bit-at-a-time decode (the same algorithm
 * zlib's reference `puff.c` inflate uses): needs only `O(max_code_len)`
 * working state, not `O(alphabet size)`, so no decode table is built or
 * cached -- this runs directly off the blob's own `length_counts`/
 * `sorted_symbols` spans.
 *
 * @return The decoded byte value, or `error::out_of_bounds` if `reader`
 * runs out of bits before a valid code is matched, or
 * `error::invalid_argument` if the bits decoded do not correspond to any
 * code in the alphabet (corrupt blob). */
[[nodiscard]] inline reloco::result<std::uint8_t> decode_huffman_symbol(bit_reader &reader,
                                                                        reloco::span<const std::byte> length_counts,
                                                                        reloco::span<const std::byte> sorted_symbols,
                                                                        std::uint8_t max_code_len) noexcept {
  unsigned code = 0;
  unsigned first = 0;
  std::size_t index = 0;
  for (std::uint8_t len = 1; len <= max_code_len; ++len) {
    auto bit = reader.next_bit();
    if (!bit)
      return reloco::unexpected(bit.error());
    code = (code << 1) | *bit;
    const auto count = static_cast<unsigned>(static_cast<std::uint8_t>(length_counts[len - 1]));
    if (code - first < count) {
      const std::size_t symbol_index = index + (code - first);
      if (symbol_index >= sorted_symbols.size())
        return reloco::unexpected(reloco::error::invalid_argument);
      return static_cast<std::uint8_t>(sorted_symbols[symbol_index]);
    }
    index += count;
    first = (first + count) << 1;
  }
  return reloco::unexpected(reloco::error::invalid_argument);
}

/** @brief CRC-32 (IEEE 802.3 polynomial, same as `zlib`/`gzip`/Ethernet),
 * computed bit-by-bit rather than through a precomputed 256-entry table
 * -- this format is already optimizing for binary footprint, and
 * integrity-checking a blob is not a hot-path operation. */
[[nodiscard]] inline std::uint32_t crc32_ieee(reloco::span<const std::byte> data) noexcept {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < data.size(); ++i) {
    crc ^= static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[i]));
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = static_cast<std::uint32_t>(-static_cast<std::int32_t>(crc & 1u));
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

} // namespace structo::debug_symtab::detail

RELOCO_END_UNSAFE_BUFFER_USAGE
