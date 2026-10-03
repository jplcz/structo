// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file debug_symtab_format.hpp
 * @brief Shared `DSYM` (`"DSY1"`) debug symbol table blob binary-format
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

/** @brief `"DSY1"`, as the little-endian `uint32_t` its 4 ASCII bytes
 * decode to. Identifies both the format and its version 1 in one check;
 * an incompatible future version 2 gets its own magic (`"DSY2"`) rather
 * than a separate version field, so an old decoder's magic check rejects
 * it outright instead of silently misreading it. */
inline constexpr std::uint32_t magic = 0x31595344u;

/** @brief Fixed header size in bytes; see @ref header_offset for the
 * field layout within it. */
inline constexpr std::size_t header_size = 48;

/** @brief Maximum encodable name length (7-bit `length` field in a name
 * record's control byte). */
inline constexpr std::uint8_t max_name_len_limit = 127;

/** @brief Byte offsets of every header field, matching
 * `docs/debug_symtab_format.md`'s header table exactly. */
namespace header_offset {
inline constexpr std::size_t magic = 0;                     // u32
inline constexpr std::size_t addr_width = 4;                // u8
inline constexpr std::size_t max_name_len = 5;               // u8
inline constexpr std::size_t truncation_marker = 6;          // u8
inline constexpr std::size_t reserved0 = 7;                  // u8
inline constexpr std::size_t group_size = 8;                 // u16
inline constexpr std::size_t reserved1 = 10;                 // u16
inline constexpr std::size_t symbol_count = 12;              // u32
inline constexpr std::size_t checkpoint_count = 16;          // u32
inline constexpr std::size_t checkpoint_table_offset = 20;   // u32
inline constexpr std::size_t entry_stream_offset = 24;       // u32
inline constexpr std::size_t entry_stream_size = 28;         // u32
inline constexpr std::size_t build_id_offset = 32;           // u32
inline constexpr std::size_t build_id_size = 36;              // u8
inline constexpr std::size_t reserved2 = 37;                  // u8[3]
inline constexpr std::size_t payload_crc32 = 40;              // u32
inline constexpr std::size_t reserved3 = 44;                  // u32
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
