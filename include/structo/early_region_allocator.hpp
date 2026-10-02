// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file early_region_allocator.hpp
 * @brief `structo::early_region_allocator<Capacity, PhysInt>`: a
 * minimal, allocation-free physical-range allocator operating directly
 * on a caller-owned `reloco::region_set` (e.g. `boot_memory_map::free`)
 * -- the smallest allocator a boot path needs before any page-
 * granularity allocator (`buddy_allocator`, a slab, ...) exists.
 *
 * `region_set` *is* the boot memory map: a sorted, auto-merging set of
 * free physical ranges. `early_region_allocator` doesn't own a copy of
 * it or parse anything itself -- it is constructed directly from an
 * existing `region_set` (by reference) and does nothing more than carve
 * ranges out of it (`try_alloc`/`try_reserve`, both backed by
 * `region_set::try_subtract`) and heal them back in
 * (`free`, backed by `region_set::try_add`). Because it mutates the
 * caller's `region_set` in place, the exact same free-region accounting
 * that fed it can be hand straight to `buddy_allocator::init()` (or
 * folded in region-by-region) once a real page allocator is ready to
 * take over -- every early allocation already punched its hole out.
 *
 * @code
 * auto map = structo::boot_memory_map<32>::try_from_dtb(dtb_blob);
 * // ... handle map.error() ...
 *
 * structo::early_region_allocator early(map->free);
 *
 * // Reserve a region whose address is already known (e.g. the kernel
 * // image itself, loaded by the bootloader before `free` was computed):
 * (void)early.try_reserve(kernel_phys_base, kernel_phys_size);
 *
 * // Carve out scratch memory for early page tables, 4 KiB-aligned:
 * auto page_table_mem = early.try_alloc(16 * 4096, 4096);
 * // ... handle page_table_mem.error() ...
 *
 * // Once the real page allocator exists, seed it from what's left:
 * for (auto &region : map->free) {
 *   // buddy_allocator::init() each region, or feed region_set-size chunks in
 * }
 * @endcode
 */

#include <cstddef>
#include <cstdint>
#include <reloco/error.hpp>
#include <structo/region_set.hpp>

namespace structo {

using namespace reloco;

/**
 * @brief Non-owning, allocation-free physical-range allocator built
 * directly on top of a caller-owned `region_set<Capacity, PhysInt>`.
 *
 * Every operation here is simply a thin, bookkeeping-free wrapper
 * around the underlying `region_set`'s own `try_subtract`/`try_add` --
 * this class adds exactly one thing `region_set` doesn't already do on
 * its own: *finding* a free range of a requested size/alignment to
 * subtract in the first place (`try_alloc`'s best-fit search).
 *
 * @tparam Capacity Must match the `region_set` passed to the
 * constructor (enforced at compile time, not merely by convention).
 * @tparam PhysInt Physical-address integer type (default: `uint64_t`).
 */
template <size_t Capacity, typename PhysInt = uint64_t> class early_region_allocator {
public:
  using region_set_type = region_set<Capacity, PhysInt>;

  /**
   * @brief Binds this allocator to @p regions, mutated in place by
   * every subsequent `try_alloc`/`try_reserve`/`free` call. @p regions
   * must outlive this `early_region_allocator`.
   */
  constexpr explicit early_region_allocator(region_set_type &regions) noexcept : regions_(&regions) {}

  /**
   * @brief Allocates @p size bytes aligned to @p alignment (must be a
   * power of two), carved from whichever currently-free region wastes
   * the fewest bytes doing so (best-fit) -- keeping larger free regions
   * intact for later large/contiguous allocations as long as possible.
   * @return `error::invalid_argument` if @p size is zero or
   * @p alignment is zero or not a power of two; `error::allocation_failed`
   * if no free region can satisfy the request.
   */
  [[nodiscard]] result<memory_region<PhysInt>> try_alloc(PhysInt size, PhysInt alignment = 1) noexcept {
    if (size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0) {
      return unexpected(error::invalid_argument);
    }

    bool found = false;
    PhysInt best_base = 0;
    PhysInt best_waste = 0;

    for (size_t i = 0; i < regions_->size(); ++i) {
      const auto &r = (*regions_)[i];

      PhysInt aligned_base = (r.base + (alignment - 1)) & ~(alignment - 1);
      if (aligned_base < r.base) {
        continue; // Alignment padding overflowed `PhysInt`; this region can't fit it.
      }

      PhysInt padding = aligned_base - r.base;
      if (padding >= r.size) {
        continue; // Alignment alone consumes the whole region.
      }

      PhysInt available = r.size - padding;
      if (available < size) {
        continue; // Not big enough even before considering alignment waste.
      }

      PhysInt waste = available - size;
      if (!found || waste < best_waste) {
        found = true;
        best_waste = waste;
        best_base = aligned_base;
      }
    }

    if (!found) {
      return unexpected(error::allocation_failed);
    }

    auto sub_res = regions_->try_subtract(best_base, size);
    if (!sub_res) {
      return unexpected(sub_res.error());
    }

    return memory_region<PhysInt>{best_base, size};
  }

  /**
   * @brief Reserves the exact, caller-known range `[base, base + size)`
   * -- e.g. the kernel image, a DTB blob, or a firmware reservation
   * only discovered after `free` was first computed. Unlike
   * `try_alloc`, the caller picks the address; this call only succeeds
   * if that entire range is currently free.
   * @return `error::invalid_argument` if @p size is zero;
   * `error::invalid_state` if any part of `[base, base + size)` is not
   * currently entirely free (already reserved, or never free to begin
   * with -- e.g. outside every tracked region).
   */
  [[nodiscard]] result<void> try_reserve(PhysInt base, PhysInt size) noexcept {
    if (size == 0) {
      return unexpected(error::invalid_argument);
    }
    if (!is_entirely_free(base, size)) {
      return unexpected(error::invalid_state);
    }
    return regions_->try_subtract(base, size);
  }

  /**
   * @brief Returns `[base, base + size)` back to the free set, merging
   * with adjacent free regions exactly like `region_set::try_add`.
   */
  result<void> free(PhysInt base, PhysInt size) noexcept { return regions_->try_add(base, size); }

  /** @brief Whether the underlying `region_set` has no free regions left. */
  [[nodiscard]] bool empty() const noexcept { return regions_->empty(); }

  /** @brief Total number of free bytes across every region. */
  [[nodiscard]] PhysInt free_bytes() const noexcept {
    PhysInt total = 0;
    for (size_t i = 0; i < regions_->size(); ++i) {
      total += (*regions_)[i].size;
    }
    return total;
  }

  /** @brief The single largest currently-free region (zero-sized if none). */
  [[nodiscard]] memory_region<PhysInt> largest_region() const noexcept { return regions_->largest_region(); }

  /** @brief The underlying `region_set` this allocator mutates. */
  [[nodiscard]] region_set_type &regions() const noexcept { return *regions_; }

private:
  [[nodiscard]] bool is_entirely_free(PhysInt base, PhysInt size) const noexcept {
    PhysInt end = base + size;
    for (size_t i = 0; i < regions_->size(); ++i) {
      if ((*regions_)[i].base <= base && (*regions_)[i].end() >= end) {
        return true;
      }
    }
    return false;
  }

  region_set_type *regions_;
};

} // namespace structo
