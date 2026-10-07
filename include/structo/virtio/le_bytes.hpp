// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file le_bytes.hpp
 * @brief Bounds-checked little-endian integer load/store over byte spans
 * (VIRTIO register and request-header encoding).
 */

#include <cstddef>
#include <cstdint>
#include <reloco/span.hpp>

namespace structo::virtio {

/** @brief Decodes `sizeof(T)` little-endian bytes from @p src (must be at least that long; extra bytes are ignored). */
template <typename T> [[nodiscard]] constexpr T load_le(reloco::span<const std::byte> src) noexcept {
  T v = 0;
  for (std::size_t i = 0; i < sizeof(T) && i < src.size(); ++i)
    v = static_cast<T>(v | (static_cast<T>(static_cast<std::uint8_t>(src[i])) << (8 * i)));
  return v;
}

/** @brief Encodes @p v little-endian into @p dst (at most `sizeof(T)` bytes are written). */
template <typename T> constexpr void store_le(reloco::span<std::byte> dst, T v) noexcept {
  for (std::size_t i = 0; i < sizeof(T) && i < dst.size(); ++i)
    dst[i] = static_cast<std::byte>(static_cast<std::uint8_t>(v >> (8 * i)));
}

} // namespace structo::virtio
