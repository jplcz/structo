// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file page_table_range.hpp
 * @brief `structo::arch::page_table_level_range<Levels, LevelIndex,
 * Entry, AddrInt>`: a forward-iterable adapter over exactly the entries
 * of one caller-supplied page-table level that a virtual-address range
 * touches, built on top of `page_table_traits.hpp`'s compile-time level
 * decomposition.
 *
 * ## Why this exists
 *
 * `page_table_traits.hpp` can tell you *which* index a single address
 * falls at for a given level; walking a whole *range* of addresses
 * (e.g. `munmap(addr, length)`, or building a mapping that spans many
 * entries) means repeating that computation for every address the range
 * touches, which is both wasteful (most of those addresses share the
 * same index at the coarser levels) and easy to get subtly wrong at the
 * range's first/last, partially-covered entry. `page_table_level_range`
 * does this arithmetic once: given one level's table (as a caller-
 * supplied span -- see below) and a `[start, end)` virtual-address
 * range, it yields exactly the entries that range touches at that
 * level, each annotated with the VA sub-window (clipped to `[start,
 * end)`) that entry is responsible for.
 *
 * ## Caller must provide memory mappings via span -- no mapping, no allocation
 *
 * Consistent with every other `structo::arch` header, this type performs
 * **no** physical-to-virtual mapping and **no** allocation of its own.
 * The `table` span handed to its constructor must already be a directly
 * dereferenceable view of one page table's backing physical page --
 * exactly as a real kernel already has its own page tables mapped
 * (identity map, linear/direct map, recursive mapping, whatever that
 * kernel uses) before it ever walks them. Resolving a *child* table's
 * physical address into another such span (after inspecting an entry
 * this type yields) is the caller's job, using whatever `page_table_
 * entry_traits<Tag>` specialization and address-space mapping mechanism
 * (e.g. `target_ptr`/`phys_addr`'s direct-map helpers) it already has.
 *
 * ## Example: walking two levels of a `munmap`-style range
 *
 * @code
 * using levels = structo::arch::page_table_levels<
 *     structo::page_4k, 48,
 *     structo::arch::page_table_level<9, 39, false>,
 *     structo::arch::page_table_level<9, 30, true>,
 *     structo::arch::page_table_level<9, 21, true>,
 *     structo::arch::page_table_level<9, 12, true>>;
 *
 * struct my_pte_tag {};
 * using entry = structo::arch::page_table_entry<my_pte_tag>;
 *
 * // `root_table` is already a dereferenceable span over the root table's
 * // 512 entries (e.g. obtained from the kernel's own direct map of the
 * // physical page `ttbr0` names). `root_base_va` is 0 for a full-range
 * // root table (index 0 always starts at VA 0 at the root level).
 * void unmap_range(reloco::span<entry> root_table, std::uint64_t root_base_va, std::uint64_t start,
 *                   std::uint64_t end) {
 *   using l0_range = structo::arch::page_table_level_range<levels, 0, entry>;
 *   for (auto &step : l0_range(root_table, root_base_va, start, end)) {
 *     if (step.entry().is_null()) {
 *       continue; // nothing mapped under this entry at all
 *     }
 *     // Decode via the caller's own page_table_entry_traits<my_pte_tag>
 *     // specialization (not shown): resolve the child table's physical
 *     // address, map it into a dereferenceable span the same way
 *     // `root_table` was obtained, then recurse with the *clipped*
 *     // sub-range this entry is responsible for.
 *     reloco::span<entry> child_table = map_child_table(step.entry()); // caller-supplied
 *     using l1_range = structo::arch::page_table_level_range<levels, 1, entry>;
 *     for (auto &child_step : l1_range(child_table, step.entry_base(), step.range_start(), step.range_end())) {
 *       // ... continue recursing into level 2, then level 3 (the leaf
 *       // table), where child_step.entry() is finally unmapped/flushed.
 *       (void)child_step;
 *     }
 *   }
 * }
 * @endcode
 */

#include "page_table_traits.hpp"
#include <reloco/contiguous_iterator.hpp>
#include <reloco/detail/assert.hpp>
#include <reloco/iterator.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/span.hpp>
#include <cstddef>
#include <cstdint>
#include <iterator>

namespace structo::arch {

/**
 * @brief Forward-iterable adapter over the entries of one page-table
 * level's table that a `[start, end)` virtual-address range touches.
 * @tparam Levels A `page_table_levels<...>` configuration.
 * @tparam LevelIndex Which level (0 == root) of `Levels` this range walks.
 * @tparam Entry The page-table-entry type stored in the caller-supplied
 * table span (typically a `page_table_entry<Tag>`).
 * @tparam AddrInt Virtual-address integer type (default `std::uint64_t`).
 */
template <typename Levels, std::size_t LevelIndex, typename Entry, typename AddrInt = std::uint64_t>
class page_table_level_range {
  using level_traits = typename Levels::template level<LevelIndex>;

public:
  /** @brief Number of entries in this level's table (`1 << index_bits`). */
  static constexpr std::size_t entry_count = level_traits::entry_count;
  /** @brief Virtual-address span one entry at this level covers (`1 << shift`). */
  static constexpr AddrInt entry_span = AddrInt(1) << level_traits::shift;

  /**
   * @brief One visited entry: its table index, an accessor into the
   * caller-supplied span, and the virtual-address window it is
   * responsible for.
   *
   * A plain, default-constructible, assignable value type (storing a
   * pointer rather than a reference to the entry) so that `iterator` can
   * hold one as a recomputed-in-place cached member and `operator*()`
   * can return a genuine `const step &`, which is required for this
   * range to be usable with `reloco::iter()`/`.iter()` (see below).
   */
  class step {
  public:
    constexpr step() noexcept = default;

    /** @brief Index into the caller-supplied table span, `[0, entry_count)`. */
    [[nodiscard]] constexpr std::size_t index() const noexcept { return index_; }
    /**
     * @brief Reference to this entry inside the caller-supplied span.
     * Traps (via `RELOCO_ASSERT`) on a default-constructed/end-iterator
     * `step` that was never bound to a real table slot.
     */
    [[nodiscard]] constexpr Entry &entry() const noexcept RELOCO_LIFETIMEBOUND {
      RELOCO_ASSERT(entry_ != nullptr, "page_table_level_range::step::entry(): step is not bound to a table slot");
      return *entry_;
    }
    /** @brief The full, level-aligned VA this entry's window starts at (`table_base_va + index * entry_span`). */
    [[nodiscard]] constexpr AddrInt entry_base() const noexcept { return entry_base_; }
    /** @brief Intersection of this entry's full window with the queried `[start, end)`: `max(entry_base, start)`. */
    [[nodiscard]] constexpr AddrInt range_start() const noexcept { return range_start_; }
    /** @brief Intersection of this entry's full window with the queried `[start, end)`: `min(entry_base + entry_span, end)`. */
    [[nodiscard]] constexpr AddrInt range_end() const noexcept { return range_end_; }

  private:
    friend class page_table_level_range;
    constexpr step(std::size_t index, Entry &entry, AddrInt entry_base, AddrInt range_start,
                    AddrInt range_end) noexcept
        : index_(index), entry_(&entry), entry_base_(entry_base), range_start_(range_start),
          range_end_(range_end) {}

    std::size_t index_{0};
    Entry *entry_{nullptr};
    AddrInt entry_base_{0};
    AddrInt range_start_{0};
    AddrInt range_end_{0};
  };

  /**
   * @brief Forward iterator yielding a `step` for each touched table
   * index, ascending.
   *
   * Caches the current `step` as a member (recomputed on construction
   * and on every increment) so `operator*()` can return a real `const
   * step &` rather than a fresh by-value object, which plain C++
   * `begin()`/`end()`-range consumers such as `reloco::iter()` require.
   */
  class iterator {
  public:
    using value_type = step;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;

    constexpr iterator() noexcept = default;

    /**
     * @brief Checked tier: traps (via `RELOCO_ASSERT`) on a default-constructed or past-the-end iterator.
     * Recomputes `cached_` for the current index on every call (lazily,
     * not eagerly on `operator++`) -- see `recompute()`'s doc comment
     * for why this ordering matters for `reloco::iter()` interop.
     */
    [[nodiscard]] constexpr const step &operator*() const noexcept RELOCO_LIFETIMEBOUND {
      RELOCO_ASSERT(owner_ != nullptr && idx_ < entry_count, "page_table_level_range::iterator: dereferencing an invalid or past-the-end iterator");
      recompute();
      return cached_;
    }
    /** @copydoc operator*() */
    [[nodiscard]] constexpr const step *operator->() const noexcept RELOCO_LIFETIMEBOUND {
      RELOCO_ASSERT(owner_ != nullptr && idx_ < entry_count, "page_table_level_range::iterator: dereferencing an invalid or past-the-end iterator");
      recompute();
      return &cached_;
    }

    constexpr iterator &operator++() noexcept {
      ++idx_;
      return *this;
    }

    constexpr iterator operator++(int) noexcept {
      iterator tmp = *this;
      ++(*this);
      return tmp;
    }

    [[nodiscard]] friend constexpr bool operator==(const iterator &a, const iterator &b) noexcept {
      return a.idx_ == b.idx_;
    }
    [[nodiscard]] friend constexpr bool operator!=(const iterator &a, const iterator &b) noexcept {
      return !(a == b);
    }

  private:
    friend class page_table_level_range;
    constexpr iterator(const page_table_level_range *owner, std::size_t idx) noexcept : owner_(owner), idx_(idx) {}

    /**
     * @brief Recomputes `cached_` for `idx_`, called only from
     * `operator*()`/`operator->()`, never from `operator++()`.
     *
     * This ordering is required for safe use with `reloco::iter()`:
     * its `range_iterator::next_impl()` does `item_type ref(*current_);
     * ++current_; return ref;` -- i.e. it takes a reference via
     * `operator*()`, *then* advances, *then* hands the still-live
     * reference to the caller. If `operator++()` recomputed `cached_`
     * in place, that post-increment recompute would silently overwrite
     * the very value `ref` still points at before the caller ever reads
     * it. Confining the recompute to dereference time means advancing
     * never touches already-vended data.
     */
    constexpr void recompute() const noexcept {
      if (owner_ == nullptr || idx_ >= entry_count) {
        return;
      }
      AddrInt base = owner_->table_base_ + static_cast<AddrInt>(idx_) * entry_span;
      AddrInt window_end = base + entry_span;
      AddrInt clipped_start = base > owner_->start_ ? base : owner_->start_;
      AddrInt clipped_end = window_end < owner_->end_ ? window_end : owner_->end_;
      cached_ = step(idx_, owner_->table_[idx_], base, clipped_start, clipped_end);
    }

    const page_table_level_range *owner_{nullptr};
    std::size_t idx_{0};
    mutable step cached_{};
  };

  /**
   * @param table Caller-owned span over exactly this level's
   * `entry_count` entries, already dereferenceable (see the file-level
   * "Caller must provide memory mappings via span" section).
   * @param table_base_va The virtual address corresponding to index 0 of
   * `table` (i.e. this table's own window starts here).
   * @param start Start (inclusive) of the virtual-address range of interest.
   * @param end End (exclusive) of the virtual-address range of interest.
   * Entries whose window does not intersect `[start, end)` -- including
   * every index outside this table's own `[table_base_va, table_base_va
   * + entry_count * entry_span)` coverage -- are skipped automatically;
   * the caller does not need to pre-clip the range to this table.
   */
  constexpr page_table_level_range(reloco::span<Entry> table, AddrInt table_base_va, AddrInt start,
                                    AddrInt end) noexcept
      : table_(table), table_base_(table_base_va), start_(start), end_(end) {
    RELOCO_ASSERT(table.size() == entry_count,
                  "page_table_level_range: table span size must equal this level's entry_count");
    if (start_ >= end_) {
      begin_idx_ = end_idx_ = entry_count;
      return;
    }
    AddrInt table_end = table_base_ + static_cast<AddrInt>(entry_count) * entry_span;
    AddrInt clipped_start = start_ > table_base_ ? start_ : table_base_;
    AddrInt clipped_end = end_ < table_end ? end_ : table_end;
    if (clipped_start >= clipped_end) {
      begin_idx_ = end_idx_ = entry_count;
      return;
    }
    begin_idx_ = static_cast<std::size_t>((clipped_start - table_base_) / entry_span);
    end_idx_ = static_cast<std::size_t>((clipped_end - table_base_ + entry_span - 1) / entry_span);
  }

  [[nodiscard]] constexpr iterator begin() const & noexcept RELOCO_LIFETIMEBOUND { return iterator(this, begin_idx_); }
  [[nodiscard]] constexpr iterator end() const & noexcept RELOCO_LIFETIMEBOUND { return iterator(this, end_idx_); }
  constexpr iterator begin() const && noexcept = delete;
  constexpr iterator end() const && noexcept = delete;

  /**
   * @brief Rust-style `.iter()`, following `reloco`'s
   * `contiguous_iterator.hpp` convention: a thin, lvalue-only wrapper
   * delegating to `reloco::iter(*this)`, itself built on the real
   * `begin()`/`end()` above (not a parallel iterator hierarchy).
   */
  RELOCO_GENERATE_ITER()

  /** @brief `true` if `[start, end)` touches no entry of this table at all. */
  [[nodiscard]] constexpr bool empty() const noexcept { return begin_idx_ == end_idx_; }

private:
  reloco::span<Entry> table_;
  AddrInt table_base_{0};
  AddrInt start_{0};
  AddrInt end_{0};
  std::size_t begin_idx_{0};
  std::size_t end_idx_{0};
};

/**
 * @brief Deduces `Entry`/`AddrInt` and constructs a `page_table_level_range<Levels, LevelIndex, Entry, AddrInt>`.
 * @tparam Levels A `page_table_levels<...>` configuration.
 * @tparam LevelIndex Which level (0 == root) of `Levels` this range walks.
 */
template <typename Levels, std::size_t LevelIndex, typename Entry, typename AddrInt>
[[nodiscard]] constexpr page_table_level_range<Levels, LevelIndex, Entry, AddrInt>
make_level_range(reloco::span<Entry> table, AddrInt table_base_va, AddrInt start, AddrInt end) noexcept {
  return page_table_level_range<Levels, LevelIndex, Entry, AddrInt>(table, table_base_va, start, end);
}

} // namespace structo::arch
