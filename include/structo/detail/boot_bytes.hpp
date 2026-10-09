// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file boot_bytes.hpp
 * @brief Private, bounds-checked little-/big-endian field readers and a
 * CRC-32 used by the boot-protocol parsers under `structo/boot/`.
 *
 * Every boot-protocol header in `structo/boot/` parses firmware/
 * bootloader-supplied (i.e. untrusted) bytes. They all funnel their field
 * access through these helpers so each read is checked against the
 * caller-owned span exactly once, in one place, mirroring the private
 * `detail::read_u32_at` helpers `arch/x86/multiboot2.hpp` already uses.
 */

#include <cstddef>
#include <cstdint>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/span.hpp>

namespace structo::boot::detail {

/** @brief Bounds-checked little-endian read of a `T` at @p offset in @p region. */
template <typename T>
[[nodiscard]] reloco::result<T> read_le_at(reloco::span<const std::byte> region, std::size_t offset) noexcept {
  auto slice = region.try_subspan(offset, sizeof(T));
  if (!slice)
    return reloco::unexpected(reloco::error::out_of_bounds);
  T value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i)
    value = static_cast<T>(value | (static_cast<T>(static_cast<unsigned char>((*slice)[i])) << (8 * i)));
  return value;
}

/** @brief Bounds-checked big-endian read of a `T` at @p offset in @p region. */
template <typename T>
[[nodiscard]] reloco::result<T> read_be_at(reloco::span<const std::byte> region, std::size_t offset) noexcept {
  auto slice = region.try_subspan(offset, sizeof(T));
  if (!slice)
    return reloco::unexpected(reloco::error::out_of_bounds);
  T value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i)
    value = static_cast<T>((value << 8) | static_cast<T>(static_cast<unsigned char>((*slice)[i])));
  return value;
}

/** @brief Bounds-checked little-endian store of @p value at @p offset in @p region. */
template <typename T>
[[nodiscard]] reloco::result<void> write_le_at(reloco::span<std::byte> region, std::size_t offset, T value) noexcept {
  auto slice = region.try_subspan(offset, sizeof(T));
  if (!slice)
    return reloco::unexpected(reloco::error::out_of_bounds);
  for (std::size_t i = 0; i < sizeof(T); ++i)
    (*slice)[i] = static_cast<std::byte>((value >> (8 * i)) & 0xFF);
  return {};
}

/** @brief Bounds-checked big-endian store of @p value at @p offset in @p region. */
template <typename T>
[[nodiscard]] reloco::result<void> write_be_at(reloco::span<std::byte> region, std::size_t offset, T value) noexcept {
  auto slice = region.try_subspan(offset, sizeof(T));
  if (!slice)
    return reloco::unexpected(reloco::error::out_of_bounds);
  for (std::size_t i = 0; i < sizeof(T); ++i)
    (*slice)[i] = static_cast<std::byte>((value >> (8 * (sizeof(T) - 1 - i))) & 0xFF);
  return {};
}

/** @brief Incremental CRC-32 (IEEE 802.3 / zlib polynomial 0xEDB88320).
 * Pass `0` as @p crc to start; feed the previous return value to continue.
 * Init/final-xor are handled internally, matching zlib's `crc32()` and
 * U-Boot's `crc32()`. */
[[nodiscard]] constexpr uint32_t crc32_update(uint32_t crc, reloco::span<const std::byte> data) noexcept {
  uint32_t c = ~crc;
  for (const std::byte b : data) {
    c ^= static_cast<uint32_t>(static_cast<unsigned char>(b));
    for (int bit = 0; bit < 8; ++bit)
      c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

/** @brief CRC-32 of @p data in one call. */
[[nodiscard]] constexpr uint32_t crc32(reloco::span<const std::byte> data) noexcept { return crc32_update(0, data); }

} // namespace structo::boot::detail
