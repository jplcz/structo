// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file page_table_occupancy.hpp
 * @brief `structo::arch::page_table_occupancy<CounterInt>`: an O(1)
 * live-entry counter for one page-table level's table, so an unmap walk
 * can detect "this table is now completely empty, free it" without
 * rescanning the table.
 *
 * ## Why this exists
 *
 * A naive unmap walk that wants to release now-unused page-table pages
 * back to the allocator (rather than letting the page-table tree only
 * ever grow, wasting memory on tables every entry of which has since
 * been unmapped) needs to answer "is this child table completely empty
 * now?" after every unmap that touches it. Answering that by rescanning
 * all `entry_count` entries (`page_table_level_range`'s own `step::
 * entry().is_null()` checked in a loop, for example) costs O(entry_count)
 * *per table, per unmap* -- for a 512-entry level this is 512 loads and
 * branches to determine a single bit of information, repeated at every
 * level the unmap cascades through, for every unmap call. Real kernels
 * (Linux's per-`pgtable_t` use count, FreeBSD's page-table page wire
 * count) instead keep a small live-entry counter *with* the table,
 * incremented/decremented exactly once per entry transition (null <->
 * non-null) as it happens -- turning "is this table empty" into reading
 * one integer. `page_table_occupancy<CounterInt>` is that counter.
 *
 * ## This is bookkeeping only -- the caller drives every transition
 *
 * Consistent with every other `structo::arch` header, this type does
 * **not** itself observe entry writes, walk tables, free memory, or own
 * any table's storage. It is a plain counter the caller increments
 * exactly once at every null -> non-null entry transition (`increment()`)
 * and decrements exactly once at every non-null -> null transition
 * (`decrement()`, which reports whether the table has just become
 * empty). Getting a transition's accounting wrong (incrementing without
 * a matching decrement, or vice versa) desynchronizes the counter from
 * reality exactly as a hand-rolled reference count would; `increment()`/
 * `decrement()` only guard against the counter's own overflow/underflow,
 * not against a caller that forgets to call them.
 *
 * ## Where the counter lives
 *
 * `structo::arch` headers never own page-table memory, so this counter
 * cannot either -- the caller embeds one `page_table_occupancy<...>` per
 * table inside whatever per-table-page metadata/descriptor it already
 * tracks (e.g. a `struct page`-equivalent, a side array indexed by table
 * physical frame number, ...). There is deliberately no "attach a
 * counter to this `reloco::span<Entry>`" API here: this header has no
 * opinion on how a caller indexes from a table back to its counter.
 *
 * ## Cold-start: seeding a counter for an already-populated table
 *
 * The one legitimate full-table scan is a *one-time* cost: initializing
 * a counter for a table that already has live entries before this
 * accounting scheme started tracking it (e.g. attaching to a table
 * built by firmware/a bootloader, or recovering after a crash). Use
 * `count_non_null()` for that one-time seed, then maintain the result
 * purely via `increment()`/`decrement()` from then on -- never call
 * `count_non_null()` again on the hot unmap path.
 *
 * ## Example: cascading release during a recursive unmap
 *
 * @code
 * // Caller's own per-table-page metadata/descriptor, one per physical
 * // page backing a non-leaf-adjacent-level table (the leaf-adjacent
 * // level's tables need one too, tracking leaf-page entries instead of
 * // child-table entries -- same counter type either way).
 * struct table_page_descriptor {
 *   structo::arch::page_table_occupancy<std::uint32_t> occupancy;
 *   // ... physical address, lock, free-list linkage, etc.
 * };
 *
 * // Recursive, post-order unmap: clear leaf entries first, then unwind
 * // back up, freeing and un-linking any table that just became empty.
 * // `table_descriptor_for(step)` / `free_table(...)` are caller-supplied
 * // (resolve/release a child table's own descriptor); not shown here.
 * void unmap_level(reloco::span<entry> table, table_page_descriptor &desc, std::uint64_t base,
 *                   std::uint64_t start, std::uint64_t end, int level) {
 *   using l_range = structo::arch::page_table_level_range<levels, 0, entry>; // LevelIndex selected per `level`
 *   for (auto &step : l_range(table, base, start, end)) {
 *     if (step.entry().is_null()) {
 *       continue;
 *     }
 *     if (is_leaf_level(level)) {
 *       step.entry() = entry{}; // unmap the page
 *       desc.occupancy.decrement(); // caller checks desc itself after the loop, see below
 *       continue;
 *     }
 *     table_page_descriptor &child_desc = table_descriptor_for(step.entry());
 *     unmap_level(map_child_table(step.entry()), child_desc, step.entry_base(), step.range_start(),
 *                 step.range_end(), level + 1);
 *     if (child_desc.occupancy.empty()) {
 *       free_table(step.entry()); // return the now-unused physical page
 *       step.entry() = entry{};   // unlink it from this table
 *       desc.occupancy.decrement(); // cascades the same check into *this* table
 *     }
 *   }
 * }
 * @endcode
 */

#include <cstddef>
#include <cstdint>
#include <limits>
#include <reloco/detail/assert.hpp>
#include <reloco/span.hpp>
#include <type_traits>

namespace structo::arch {

/**
 * @brief O(1) live (non-null) entry counter for one page-table level's
 * table, so an unmap walk can test "is this table empty" without
 * rescanning it.
 * @tparam CounterInt Unsigned integer type backing the counter (default
 * `std::uint32_t`; a smaller type such as `std::uint16_t` is fine for
 * any level whose `entry_count` fits, and packs tighter into a per-table
 * descriptor).
 */
template <typename CounterInt = std::uint32_t> class page_table_occupancy {
  static_assert(std::is_unsigned_v<CounterInt>, "CounterInt must be an unsigned integer type");

public:
  constexpr page_table_occupancy() noexcept = default;
  /** @brief Constructs with an already-known live-entry count (e.g. the result of `count_non_null()`). */
  constexpr explicit page_table_occupancy(CounterInt initial_count) noexcept : count_(initial_count) {}

  /** @brief Current number of non-null entries this table is tracked as having. */
  [[nodiscard]] constexpr CounterInt count() const noexcept { return count_; }
  /** @brief `true` if no entry transition has left this table with any live (non-null) entry. */
  [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0; }

  /**
   * @brief Call exactly once for every entry transition from null to
   * non-null (a new mapping or child table installed into this table).
   * Traps (via `RELOCO_ASSERT`) if the counter would overflow `CounterInt`.
   */
  constexpr void increment() noexcept {
    RELOCO_ASSERT(count_ != (std::numeric_limits<CounterInt>::max)(),
                  "page_table_occupancy::increment(): counter would overflow");
    ++count_;
  }

  /**
   * @brief Call exactly once for every entry transition from non-null to
   * null (a mapping removed, or a now-empty child table unlinked from
   * this table). Traps (via `RELOCO_ASSERT`) if the counter is already
   * zero (a caller accounting bug -- decrementing past empty).
   * @return `true` if this decrement just brought the table to `empty()`
   * (i.e. the caller should now free this table and, if this table is
   * itself someone else's child, cascade a `decrement()` into that
   * parent's own counter).
   */
  constexpr bool decrement() noexcept {
    RELOCO_ASSERT(count_ != 0, "page_table_occupancy::decrement(): counter is already zero");
    --count_;
    return count_ == 0;
  }

  /**
   * @brief One-time O(`table.size()`) scan counting already-non-null
   * entries, for seeding a counter the very first time a pre-existing,
   * already-populated table is brought under this accounting scheme.
   * Not for repeated use on the hot unmap path -- see the file-level
   * "Cold-start" section.
   * @tparam Entry Page-table-entry type; must expose `bool is_null() const`.
   */
  template <typename Entry>
  [[nodiscard]] static constexpr CounterInt count_non_null(reloco::span<const Entry> table) noexcept {
    CounterInt count = 0;
    for (const Entry &e : table) {
      if (!e.is_null()) {
        ++count;
      }
    }
    return count;
  }

private:
  CounterInt count_{0};
};

} // namespace structo::arch
