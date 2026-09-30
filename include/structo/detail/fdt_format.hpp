// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file fdt_format.hpp
 * @brief Shared Flattened Device Tree (DTB, `/dts-v1/`) binary-format
 * constants and big-endian codec primitives used by both
 * `reloco/fdt_writer.hpp` and `reloco/fdt_reader.hpp`, so the two never
 * drift apart on token IDs, header layout, or byte order.
 *
 * All multi-byte fields are big-endian, matching the DTB spec. No
 * floating point is used anywhere in this file.
 */

#include <cstddef>
#include <cstdint>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/span.hpp>
#include <reloco/string_view.hpp>
#include <utility>

namespace structo::fdt {

// This header writes/reads big-endian fields through raw std::byte*
// pointer arithmetic throughout; treated as a single checked boundary
// like reloco/bytes.hpp and reloco/string_view.hpp (see fdt_writer.hpp
// and fdt_reader.hpp, which both wrap their own callers of these
// functions in the same pragma pair -- redundant but harmless nesting).
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

inline constexpr uint32_t magic = 0xd00dfeedu;
inline constexpr uint32_t version = 17u;
inline constexpr uint32_t last_comp_version = 16u;

namespace detail {

inline constexpr uint32_t token_begin_node = 1u;
inline constexpr uint32_t token_end_node = 2u;
inline constexpr uint32_t token_prop = 3u;
inline constexpr uint32_t token_nop = 4u;
inline constexpr uint32_t token_end = 9u;

inline constexpr std::size_t header_size = 40; // 10 big-endian uint32 fields.

[[nodiscard]] inline constexpr std::size_t align4(std::size_t n) noexcept {
  return (n + 3u) & ~static_cast<std::size_t>(3u);
}

inline void store_be32(std::byte *p, uint32_t v) noexcept {
  p[0] = static_cast<std::byte>((v >> 24) & 0xffu);
  p[1] = static_cast<std::byte>((v >> 16) & 0xffu);
  p[2] = static_cast<std::byte>((v >> 8) & 0xffu);
  p[3] = static_cast<std::byte>(v & 0xffu);
}

inline void store_be64(std::byte *p, uint64_t v) noexcept {
  for (std::size_t i = 0; i < 8; ++i)
    p[i] = static_cast<std::byte>((v >> (8u * (7u - i))) & 0xffu);
}

[[nodiscard]] inline uint32_t load_be32(const std::byte *p) noexcept {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

[[nodiscard]] inline uint64_t load_be64(const std::byte *p) noexcept {
  uint64_t v = 0;
  for (std::size_t i = 0; i < 8; ++i)
    v = (v << 8) | static_cast<uint64_t>(p[i]);
  return v;
}

/** @brief Reads a big-endian `uint32_t` at `offset` within `region`,
 * bounds-checked via `span::try_subspan`. Shared by `fdt_reader.hpp` (its
 * sequential streaming decode) and `fdt_index.hpp` (its own, independent
 * struct-block walk when building a random-access index). */
[[nodiscard]] inline reloco::result<uint32_t> read_u32_at(reloco::span<const std::byte> region,
                                                          std::size_t offset) noexcept {
  auto slice = region.try_subspan(offset, 4);
  if (!slice)
    return reloco::unexpected(reloco::error::out_of_bounds);
  return load_be32(slice->data());
}

/** @brief Scans `region` for a NUL byte starting at `start`, returning the
 * `(name, position-just-past-the-NUL)` pair. Fails with
 * `error::out_of_bounds` if `start` itself is out of range, or
 * `error::invalid_argument` if no NUL terminator is found before the end
 * of `region` (an unterminated string is malformed, not merely "not there
 * yet"). Shared by `fdt_reader.hpp` and `fdt_index.hpp` for the same
 * reason as `read_u32_at` above. */
[[nodiscard]] inline reloco::result<std::pair<reloco::string_view, std::size_t>>
read_cstring(reloco::span<const std::byte> region, std::size_t start) noexcept {
  if (start > region.size())
    return reloco::unexpected(reloco::error::out_of_bounds);
  std::size_t i = start;
  while (i < region.size() && region[i] != std::byte{0})
    ++i;
  if (i == region.size())
    return reloco::unexpected(reloco::error::invalid_argument);
  return std::pair<reloco::string_view, std::size_t>(
      reloco::string_view(reinterpret_cast<const char *>(region.data() + start), i - start), i + 1);
}

} // namespace detail

RELOCO_END_UNSAFE_BUFFER_USAGE

} // namespace structo::fdt
