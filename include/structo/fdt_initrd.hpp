// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file fdt_initrd.hpp
 * @brief `structo::fdt::find_initrd`: locates a devicetree-described
 * initial ramdisk (initrd/initramfs) image's physical address range,
 * from an already-built `fdt_index`, plus
 * `structo::fdt::try_bind_initrd_ram_disk`, a convenience that turns
 * that range directly into a ready-to-use, read-only
 * `hw::block_device_ref` over `hw::ram_disk.hpp`'s `read_only_ram_disk`.
 *
 * ## The convention: `/chosen`'s `linux,initrd-start`/`linux,initrd-end`
 *
 * This is the same convention every Linux-compatible bootloader (U-Boot,
 * GRUB's `devicetree`/`initrd` commands, EDK2's `DtPlatformDxe`, ...)
 * and the Linux kernel's own `early_init_dt_check_for_initrd` use: the
 * `/chosen` node carries two properties, `linux,initrd-start` and
 * `linux,initrd-end`, each either a single `<u32>` or `<u64>` cell (a
 * property value of exactly 4 or 8 bytes respectively -- independent of
 * the tree's own `#address-cells`, matching the kernel's own
 * `of_read_number(prop, prop_len / 4)` decoding), giving the image's
 * *physical* address range as `[start, end)`. Neither property is
 * required to be present at all -- a devicetree with no initrd simply
 * omits both, which `find_initrd` reports as an empty `optional`, not an
 * error.
 *
 * This header has no notion of *mapping* that physical range into
 * addressable memory itself (that is inherently platform/MMU-state
 * specific, e.g. identity-mapped early boot vs. a bootloader's own
 * heap); `find_initrd` only ever reports the physical `[start, end)`
 * range the devicetree describes. `try_bind_initrd_ram_disk` accepts the
 * *already-mapped* span covering that range as a parameter -- it is the
 * caller's job to have turned the physical range into one.
 */

#include "fdt_index.hpp"
#include "hw/ram_disk.hpp"

namespace structo::fdt {

using namespace reloco;

/** @brief A devicetree-described initrd image's physical address range,
 * as reported by `/chosen`'s `linux,initrd-start`/`linux,initrd-end`
 * properties -- see the @file docs above. */
struct initrd_location {
  /** @brief Physical start address (inclusive), as declared by
   * `linux,initrd-start`. */
  uint64_t start{};
  /** @brief Physical end address (exclusive), as declared by
   * `linux,initrd-end`. */
  uint64_t end{};

  /** @brief `end - start`; never underflows, since `find_initrd` already
   * rejects `end < start` as malformed. */
  [[nodiscard]] constexpr uint64_t size_bytes() const noexcept { return end - start; }
};

namespace detail {

/** @brief Decodes a `linux,initrd-start`/`linux,initrd-end` property
 * value as either a single `<u32>` or `<u64>` cell, matching the
 * kernel's own `of_read_number(prop, prop_len / 4)` convention. Fails
 * with `error::invalid_argument` if the value is neither 4 nor 8 bytes. */
[[nodiscard]] inline result<uint64_t> decode_initrd_cell(const fdt_property_view &prop) noexcept {
  if (prop.value.size() == 4) {
    auto v = prop.try_as_u32();
    if (!v)
      return unexpected(v.error());
    return static_cast<uint64_t>(*v);
  }
  if (prop.value.size() == 8)
    return prop.try_as_u64();
  return unexpected(error::invalid_argument);
}

} // namespace detail

/**
 * @brief Looks up `/chosen`'s `linux,initrd-start`/`linux,initrd-end`
 * properties in @p idx, reporting the initrd image's physical address
 * range if present.
 *
 * Returns an empty `optional` (not an error) if `/chosen` doesn't exist,
 * or exists but has neither property -- a devicetree with no initrd at
 * all is the ordinary case, not a malformed one. Fails with:
 *   - `error::invalid_argument` if only one of the two properties is
 *     present, either property is neither 4 nor 8 bytes, or `end <
 *     start`.
 *   - whatever error `fdt_index::find_by_path`/`find_property`
 *     themselves propagate from a malformed struct block.
 */
template <template <typename T> class Container>
[[nodiscard]] result<optional<initrd_location>> find_initrd(const fdt_index<Container> &idx) noexcept {
  auto chosen = idx.find_by_path("/chosen");
  if (!chosen) {
    if (chosen.error() == error::not_found)
      return optional<initrd_location>(nullopt);
    return unexpected(chosen.error());
  }

  auto start_prop = idx.find_property(*chosen, "linux,initrd-start");
  if (!start_prop)
    return unexpected(start_prop.error());
  auto end_prop = idx.find_property(*chosen, "linux,initrd-end");
  if (!end_prop)
    return unexpected(end_prop.error());

  if (!start_prop->has_value() && !end_prop->has_value())
    return optional<initrd_location>(nullopt);
  if (!start_prop->has_value() || !end_prop->has_value())
    return unexpected(error::invalid_argument); // One without the other is malformed.

  auto start = detail::decode_initrd_cell(**start_prop);
  if (!start)
    return unexpected(start.error());
  auto end = detail::decode_initrd_cell(**end_prop);
  if (!end)
    return unexpected(end.error());
  if (*end < *start)
    return unexpected(error::invalid_argument);

  return optional<initrd_location>(initrd_location{*start, *end});
}

/**
 * @brief Convenience: wraps @p mapped -- the already-mapped span
 * covering @p loc's physical range (see the @file docs above for why
 * mapping itself is out of this header's scope) -- as a byte-addressable
 * (`block_size() == 1`) `hw::read_only_ram_disk`, and binds @p ref to
 * it.
 *
 * @param loc A range previously returned by `find_initrd`.
 * @param mapped The mapped span covering `loc`'s physical range. Must
 * outlive @p disk and every `block_device_ref` bound to it (including
 * @p ref itself) and be at least `loc.size_bytes()` bytes.
 * @param disk Storage for the constructed backend; must outlive @p ref.
 * @param ref Bound to @p disk on success.
 * Fails with `error::out_of_range` if `mapped.size() < loc.size_bytes()`.
 */
[[nodiscard]] inline result<void> try_bind_initrd_ram_disk(const initrd_location &loc, span<const std::byte> mapped,
                                                           hw::read_only_ram_disk &disk,
                                                           hw::block_device_ref &ref) noexcept {
  if (mapped.size() < loc.size_bytes())
    return unexpected(error::out_of_range);
  disk = hw::read_only_ram_disk(mapped.first(static_cast<std::size_t>(loc.size_bytes())), 1);
  ref = hw::block_device_ref(disk);
  return {};
}

} // namespace structo::fdt
