// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file boot_memory_map.hpp
 * @brief `structo::boot_memory_map<Capacity, PhysInt>`: a caller-owned,
 * allocation-free physical memory map decoded straight from a Flattened
 * Device Tree (DTB) blob, built on `structo::fdt::fdt_reader` +
 * `structo::fdt::try_extract_memory` + `reloco::region_set`.
 *
 * This is the first thing a kernel/hypervisor boot path typically needs
 * once it has the raw DTB pointer handed to it by firmware/bootloader:
 * "which physical ranges are installed RAM, and which of those are
 * actually free to hand to the page allocator" -- before any allocator,
 * MMU, or scheduler exists. `try_from_dtb()` performs both scans (`full`
 * = every byte of installed RAM, `free` = `full` minus every
 * `/reserved-memory` node and legacy `/memreserve/` entry) in one call,
 * and `try_largest_free_region()` adds the one extra query most early
 * allocators bootstrap from: the single biggest free range, used to seed
 * a first arena/buddy allocator before the rest of `free` is folded in.
 */

#include <structo/fdt_memory.hpp>
#include <structo/fdt_reader.hpp>
#include <structo/region_set.hpp>

namespace structo {

/**
 * @brief Fixed-capacity physical memory map decoded from a devicetree
 * blob: every installed RAM range (`full`) and the subset of it not
 * already claimed by firmware/bootloader reservations (`free`).
 *
 * @tparam Capacity Maximum number of disjoint regions either set can
 * hold; size this from a known bound on the target's `/memory` +
 * `/reserved-memory` node count (see `reloco::region_set`).
 * @tparam PhysInt Physical-address integer type (default: `uint64_t`).
 */
template <std::size_t Capacity, typename PhysInt = std::uint64_t> struct boot_memory_map {
  structo::region_set<Capacity, PhysInt> full;
  structo::region_set<Capacity, PhysInt> free;

  /** @brief Parses @p dtb_blob as a Flattened Device Tree and extracts
   * its physical memory description. Fails with whatever error
   * `structo::fdt::fdt_reader::try_create` or
   * `structo::fdt::try_extract_memory` reports: a malformed/truncated
   * blob, no `/memory`-class node, a malformed `reg` property, or
   * `Capacity` being too small for the blob's region count. */
  [[nodiscard]] static reloco::result<boot_memory_map> try_from_dtb(reloco::span<const std::byte> dtb_blob) noexcept {
    auto reader = structo::fdt::fdt_reader::try_create(dtb_blob);
    if (!reader)
      return reloco::unexpected(reader.error());

    boot_memory_map out;
    auto extracted = structo::fdt::try_extract_memory(*reader, out.full, out.free);
    if (!extracted)
      return reloco::unexpected(extracted.error());

    return out;
  }

  /** @brief The single largest region in `free`, i.e. the best candidate
   * to seed a first-stage (arena/buddy) allocator from before the
   * remainder of `free` is folded in. Fails with `error::not_found` if
   * `free` is empty. */
  [[nodiscard]] reloco::result<structo::memory_region<PhysInt>> try_largest_free_region() const noexcept {
    if (free.empty())
      return reloco::unexpected(reloco::error::not_found);

    std::size_t best = 0;
    for (std::size_t i = 1; i < free.size(); ++i) {
      if (free[i].size > free[best].size)
        best = i;
    }
    return free[best];
  }

  /** @brief Total number of bytes across every region in `free`. */
  [[nodiscard]] PhysInt free_bytes() const noexcept {
    PhysInt total = 0;
    for (std::size_t i = 0; i < free.size(); ++i)
      total += free[i].size;
    return total;
  }
};

} // namespace structo
