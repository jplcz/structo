// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file early_region_allocator.hpp
 * @brief `structo::early_region_allocator<Capacity, PhysInt>`: a
 * minimal, allocation-free physical-range allocator seeded from a
 * caller-owned `reloco::region_set` (e.g. `boot_memory_map::free`) --
 * the smallest allocator a boot path needs before any page-granularity
 * allocator (`buddy_allocator`, a slab, ...) exists.
 *
 * `region_set` *is* the boot memory map: a sorted, auto-merging set of
 * free physical ranges. The boot memory map is typically the *full*
 * authoritative record of installed/reserved RAM for the whole boot --
 * other, unrelated code may still read it later, so
 * `early_region_allocator` must not mutate it. Instead, `try_create()`
 * takes an immutable snapshot of the seed `region_set` into its own,
 * separate internal `region_set` at construction time; every subsequent
 * `try_alloc`/`try_reserve`/`free` call only ever mutates that private
 * copy. The seed `region_set` the allocator was built from is left
 * completely untouched and can keep being read (or handed to something
 * else) for as long as the caller likes.
 *
 * @code
 * auto map = structo::boot_memory_map<32>::try_from_dtb(dtb_blob);
 * // ... handle map.error() ...
 *
 * auto early = structo::early_region_allocator<32>::try_create(map->free);
 * // ... handle early.error() ...
 * // `map->free` itself is untouched by everything below.
 *
 * // Reserve a region whose address is already known (e.g. the kernel
 * // image itself, loaded by the bootloader before `free` was computed):
 * (void)early->try_reserve(kernel_phys_base, kernel_phys_size);
 *
 * // Carve out scratch memory for early page tables, 4 KiB-aligned:
 * auto page_table_mem = early->try_alloc(16 * 4096, 4096);
 * // ... handle page_table_mem.error() ...
 *
 * // Once the real page allocator exists, seed it from what's left:
 * for (auto &region : early->regions()) {
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
 * @brief Allocation-free physical-range allocator tracking its own,
 * private `region_set<Capacity, PhysInt>`, snapshotted from a seed
 * `region_set` at construction time.
 *
 * Every operation here is simply a thin, bookkeeping-free wrapper
 * around the internal `region_set`'s own `try_subtract`/`try_add` --
 * this class adds exactly one thing `region_set` doesn't already do on
 * its own: *finding* a free range of a requested size/alignment to
 * subtract in the first place (`try_alloc`'s best-fit search). The seed
 * `region_set` passed to `try_create()` is read once and never modified.
 *
 * @tparam Capacity Must match the seed `region_set` passed to
 * `try_create` (enforced at compile time, not merely by convention).
 * @tparam PhysInt Physical-address integer type (default: `uint64_t`).
 */
template <size_t Capacity, typename PhysInt = uint64_t> class early_region_allocator {
public:
  using region_set_type = region_set<Capacity, PhysInt>;

  constexpr early_region_allocator() noexcept = default;

  /**
   * @brief Builds an allocator whose internal free-region bookkeeping
   * starts as a copy of every region in @p seed. @p seed itself is only
   * read here and is never mutated, by this call or by any later
   * `try_alloc`/`try_reserve`/`free` on the returned allocator.
   * @return `error::capacity_exceeded` if @p seed's regions can't all
   * be copied in (should not happen: both sets share `Capacity`, so
   * this can only occur if @p seed's own merging invariant was somehow
   * violated via direct `inline_vector` access).
   */
  [[nodiscard]] static result<early_region_allocator> try_create(const region_set_type &seed) noexcept {
    early_region_allocator alloc;
    for (size_t i = 0; i < seed.size(); ++i) {
      auto add_res = alloc.regions_.try_add(seed[i].base, seed[i].size);
      if (!add_res) {
        return unexpected(add_res.error());
      }
    }
    return alloc;
  }

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

    for (size_t i = 0; i < regions_.size(); ++i) {
      const auto &r = regions_[i];

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

    auto sub_res = regions_.try_subtract(best_base, size);
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
    return regions_.try_subtract(base, size);
  }

  /**
   * @brief Returns `[base, base + size)` back to this allocator's own
   * free set, merging with adjacent free regions exactly like
   * `region_set::try_add`.
   */
  result<void> free(PhysInt base, PhysInt size) noexcept { return regions_.try_add(base, size); }

  /** @brief Whether this allocator's internal free set is empty. */
  [[nodiscard]] bool empty() const noexcept { return regions_.empty(); }

  /** @brief Total number of free bytes across every region. */
  [[nodiscard]] PhysInt free_bytes() const noexcept {
    PhysInt total = 0;
    for (size_t i = 0; i < regions_.size(); ++i) {
      total += regions_[i].size;
    }
    return total;
  }

  /** @brief The single largest currently-free region (zero-sized if none). */
  [[nodiscard]] memory_region<PhysInt> largest_region() const noexcept { return regions_.largest_region(); }

  /** @brief This allocator's own, private free-region bookkeeping (not the seed it was built from). */
  [[nodiscard]] const region_set_type &regions() const noexcept { return regions_; }

private:
  [[nodiscard]] bool is_entirely_free(PhysInt base, PhysInt size) const noexcept {
    PhysInt end = base + size;
    for (size_t i = 0; i < regions_.size(); ++i) {
      if (regions_[i].base <= base && regions_[i].end() >= end) {
        return true;
      }
    }
    return false;
  }

  region_set_type regions_{};
};

} // namespace structo
