// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file page_table_traits.hpp
 * @brief `structo::arch::page_table_level<IndexBits, Shift, AllowsLeaf>`
 * (one level's compile-time index-decoding traits) and
 * `structo::arch::page_table_levels<LeafPageTraits, VaBits, Levels...>`
 * (a self-consistency-checked aggregate of them), plus
 * `structo::arch::page_table_entry<Tag, Int>` -- an opaque, per-
 * architecture-tagged raw page-table-entry handle -- and the
 * `page_table_entry_traits<Tag>` extension point a future entry-
 * encode/decode/walker layer will specialize.
 *
 * ## Why this exists
 *
 * Every hardware multi-level page-table format (ARM64's 4KB/16KB/64KB
 * granules, x86-64's 4-level and 5-level long-mode paging, RISC-V's
 * Sv39/Sv48/Sv57) carves a virtual address into the same *shape* --
 * a handful of fixed-width index fields, most-significant first, each
 * selecting one entry out of one level's table, followed by a page
 * offset -- but every architecture (and sometimes every page-table
 * *level* within one architecture) disagrees on the field widths, shift
 * amounts, and which levels are even allowed to terminate the walk early
 * with a large/huge/block mapping instead of pointing at the next
 * level's table. `asid_allocator.hpp`'s architecture-agnostic-bookkeeping
 * philosophy applies here too: this header supplies the *shape* --
 * compile-time index decomposition, validated once at compile time
 * against the declared level widths -- and leaves every bit of the
 * actual entry encoding (which bit means "present", how a physical frame
 * number is packed, permission bits, ...) to a per-architecture trait the
 * embedder supplies.
 *
 * ## Compile-time, not runtime, level traits
 *
 * `MaxCpus`-style runtime flexibility (as in `cpu_mask`/`hw_id_lut`) does
 * not apply here: a page-table format's level count, field widths, and
 * shift amounts are fixed by the architecture and page-granule choice a
 * kernel is built for, never a runtime-probed value. Making every level
 * a compile-time template parameter lets `index_of()` compile down to a
 * single shift-and-mask with a compile-time-constant shift/mask pair --
 * no runtime branch, no table of per-level widths to index into -- and
 * lets `page_table_levels` catch a mis-specified level (one that doesn't
 * tile the virtual address contiguously down to the leaf page's shift)
 * as a `static_assert` failure, not a boot-time page-fault storm.
 *
 * ## What this header intentionally does NOT add (yet)
 *
 * This is the *decomposition* layer only: turning a virtual address into
 * a `(level_0_index, level_1_index, ..., page_offset)` tuple, validated
 * for internal consistency at compile time. It deliberately does **not**
 * provide a page-table walker, entry allocation, or any interpretation
 * of what bits inside a `page_table_entry<Tag>` mean (present? leaf?
 * which physical frame?) -- that is `page_table_entry_traits<Tag>`'s
 * job, declared below as an extension point (mirroring `target_ptr.hpp`'s
 * `target_ptr_space_traits<SpaceTag>` pattern) for a future walker layer
 * to specialize and consume. Keeping this header's scope to "traits plus
 * decomposition" means it has zero opinions about allocation strategy,
 * locking, or TLB invalidation -- those belong with whatever consumes
 * `page_table_entry_traits<Tag>` later, exactly as `asid_allocator.hpp`
 * stays agnostic about which lock (if any) serializes its callers.
 *
 * ## Example: ARM64, 4KB granule, 4-level (48-bit VA)
 *
 * @code
 * // One index field per level, most-significant (closest to the root)
 * // first, ending at the leaf-adjacent table whose entries point
 * // directly at (or, for levels with AllowsLeaf, immediately terminate
 * // the walk at) a physical page.
 * using arm64_4k_4level = structo::arch::page_table_levels<
 *     structo::page_4k, 48,
 *     structo::arch::page_table_level<9, 39, false>, // L0 (root): no block mappings on this granule
 *     structo::arch::page_table_level<9, 30, true>,   // L1: may terminate early with a 1GB block
 *     structo::arch::page_table_level<9, 21, true>,   // L2: may terminate early with a 2MB block
 *     structo::arch::page_table_level<9, 12, true>    // L3 (leaf-adjacent): always a 4KB page
 * >;
 *
 * void decompose(std::uintptr_t va) {
 *   auto l0 = arm64_4k_4level::index_of<0>(va); // bits [47:39]
 *   auto l1 = arm64_4k_4level::index_of<1>(va); // bits [38:30]
 *   auto l2 = arm64_4k_4level::index_of<2>(va); // bits [29:21]
 *   auto l3 = arm64_4k_4level::index_of<3>(va); // bits [20:12]
 *   auto offset = arm64_4k_4level::page_offset(va); // bits [11:0]
 *   (void)l0; (void)l1; (void)l2; (void)l3; (void)offset;
 * }
 * @endcode
 */

#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>

namespace structo::arch {

// ============================================================================
// Per-Level Index Traits
// ============================================================================

/**
 * @brief Compile-time index-decoding traits for one page-table level.
 * @tparam IndexBits Number of virtual-address bits forming this level's
 * table index; the table this level indexes into therefore has
 * `1 << IndexBits` entries.
 * @tparam Shift Bit position in the virtual address where this level's
 * index field starts (i.e. this level's index is `(va >> Shift) &
 * ((1 << IndexBits) - 1)`).
 * @tparam AllowsLeaf Whether a walker may legally terminate the walk
 * early at this level with a large/huge/block mapping instead of
 * continuing to the next level's table. Meaningless (always effectively
 * `true`) for whichever level is last in a `page_table_levels` list --
 * there is no "next level" to continue to there, so that level's entries
 * always address a page, never a further table.
 */
template <std::size_t IndexBits, std::size_t Shift, bool AllowsLeaf = true> struct page_table_level {
  static_assert(IndexBits >= 1 && IndexBits <= 32, "IndexBits must be in [1, 32]");
  static_assert(Shift + IndexBits <= 64, "Shift + IndexBits must fit in a 64-bit address");

  static constexpr std::size_t index_bits = IndexBits;
  static constexpr std::size_t shift = Shift;
  static constexpr std::size_t entry_count = std::size_t(1) << IndexBits;
  static constexpr std::size_t index_mask = entry_count - 1;
  static constexpr bool allows_leaf = AllowsLeaf;

  /** @brief Extracts this level's table index out of a raw virtual address. */
  template <typename AddrInt> [[nodiscard]] static constexpr std::size_t index_of(AddrInt va) noexcept {
    return static_cast<std::size_t>((static_cast<std::uint64_t>(va) >> Shift) & std::uint64_t(index_mask));
  }
};

namespace detail {

// Recursively checks that a level pack, read root-first, tiles the
// virtual address contiguously down to `ExpectedLeafShift` with no gaps
// or overlaps: each level's shift must equal the next (finer) level's
// shift plus that next level's index_bits, and the last (leaf-adjacent)
// level's shift must equal the leaf page's shift exactly.
template <std::size_t ExpectedLeafShift, typename... Levels> struct levels_tile_contiguously;

template <std::size_t ExpectedLeafShift, typename Last> struct levels_tile_contiguously<ExpectedLeafShift, Last> {
  static constexpr bool value = (Last::shift == ExpectedLeafShift);
};

template <std::size_t ExpectedLeafShift, typename First, typename Second, typename... Rest>
struct levels_tile_contiguously<ExpectedLeafShift, First, Second, Rest...> {
  static constexpr bool value = (First::shift == Second::shift + Second::index_bits) &&
                                levels_tile_contiguously<ExpectedLeafShift, Second, Rest...>::value;
};

} // namespace detail

// ============================================================================
// Multi-Level Aggregate
// ============================================================================

/**
 * @brief Self-consistency-checked aggregate of a multi-level page
 * table's per-level index traits, root level first.
 * @tparam LeafPageTraits `structo::page_traits<Size, Shift>`-shaped leaf
 * page-size traits (e.g. `structo::page_4k`); the last level in `Levels`
 * must have `shift == LeafPageTraits::page_shift`.
 * @tparam VaBits Total number of significant virtual-address bits this
 * configuration decodes (e.g. 48 for ARM64/x86-64 4-level paging, 39 for
 * RISC-V Sv39); the root (first) level must satisfy `shift + index_bits
 * == VaBits`.
 * @tparam Levels One `page_table_level<...>` per page-table level, most-
 * significant (root) first, leaf-adjacent table last.
 */
template <typename LeafPageTraits, std::size_t VaBits, typename... Levels> struct page_table_levels {
  static_assert(sizeof...(Levels) >= 1, "page_table_levels requires at least one level");
  static_assert(detail::levels_tile_contiguously<LeafPageTraits::page_shift, Levels...>::value,
                "levels must tile the virtual address contiguously down to the leaf page shift, with no "
                "gaps or overlaps");

  using leaf_page_traits = LeafPageTraits;
  static constexpr std::size_t level_count = sizeof...(Levels);
  static constexpr std::size_t va_bits = VaBits;

private:
  using level_tuple = std::tuple<Levels...>;
  using root_level = std::tuple_element_t<0, level_tuple>;
  static_assert(root_level::shift + root_level::index_bits == VaBits,
                "the root level must cover exactly VaBits significant virtual-address bits");

public:
  /** @brief The `page_table_level<...>` traits for level `LevelIndex` (0 == root). */
  template <std::size_t LevelIndex> using level = std::tuple_element_t<LevelIndex, level_tuple>;

  /** @brief Entry count of level `LevelIndex`'s table (`1 << index_bits`). */
  template <std::size_t LevelIndex> [[nodiscard]] static constexpr std::size_t entry_count() noexcept {
    return level<LevelIndex>::entry_count;
  }

  /** @brief Whether level `LevelIndex` may terminate the walk early with a block/huge mapping. */
  template <std::size_t LevelIndex> [[nodiscard]] static constexpr bool allows_leaf() noexcept {
    return level<LevelIndex>::allows_leaf;
  }

  /** @brief Extracts level `LevelIndex`'s table index out of a raw virtual address. */
  template <std::size_t LevelIndex, typename AddrInt>
  [[nodiscard]] static constexpr std::size_t index_of(AddrInt va) noexcept {
    return level<LevelIndex>::index_of(va);
  }

  /** @brief The byte offset within the final, leaf-level page. */
  template <typename AddrInt> [[nodiscard]] static constexpr AddrInt page_offset(AddrInt va) noexcept {
    return static_cast<AddrInt>(static_cast<std::uint64_t>(va) & std::uint64_t(LeafPageTraits::alignment_mask));
  }
};

// ============================================================================
// Opaque Page-Table Entry Handle
// ============================================================================

/**
 * @brief Opaque, architecture-tagged raw page-table-entry value.
 * @tparam Tag Phantom tag identifying which architecture's/format's
 * entry-bit layout this value's raw bits follow (e.g. a caller-defined
 * `arm64_stage1_pte_tag`); entries from unrelated tags are different,
 * non-interconvertible types, exactly as `tagged_asid<Tag>` keeps a
 * process ASID and a VMID apart.
 * @tparam Int Underlying raw integer storage (typically `std::uint64_t`).
 *
 * This type is deliberately just storage plus equality/null-check --
 * `value == 0` is a reasonable, near-universal "not present" convention
 * (every architecture this library targets treats an all-zero entry as
 * not-present), but interpretation of every other bit is entirely up to
 * the `page_table_entry_traits<Tag>` specialization below; this type
 * itself never inspects any bit beyond that.
 */
template <typename Tag, typename Int = std::uint64_t> struct page_table_entry {
  using tag_type = Tag;
  using raw_type = Int;

  Int value{Int(0)};

  constexpr page_table_entry() noexcept = default;
  constexpr page_table_entry(std::nullptr_t) noexcept {}
  constexpr explicit page_table_entry(Int raw) noexcept : value(raw) {}

  /** @brief `true` for an all-zero ("not present", by near-universal architectural convention) entry. */
  [[nodiscard]] constexpr bool is_null() const noexcept { return value == Int(0); }
  constexpr explicit operator bool() const noexcept { return !is_null(); }

  [[nodiscard]] friend constexpr bool operator==(page_table_entry lhs, page_table_entry rhs) noexcept {
    return lhs.value == rhs.value;
  }
  [[nodiscard]] friend constexpr bool operator!=(page_table_entry lhs, page_table_entry rhs) noexcept {
    return lhs.value != rhs.value;
  }
};

/**
 * @brief Per-`Tag` hook a future page-table-walker layer routes every
 * entry encode/decode operation through. Deliberately left undefined by
 * default (matching `target_ptr.hpp`'s `target_ptr_space_traits<SpaceTag>`
 * pattern): declared here purely to document the extension point this
 * header's `page_table_entry<Tag>` is designed to plug into, not
 * consumed by anything in this header. A future walker would expect a
 * full specialization shaped like:
 *
 * @code
 * template <> struct structo::arch::page_table_entry_traits<my_arm64_stage1_tag> {
 *   using entry_type = structo::arch::page_table_entry<my_arm64_stage1_tag>;
 *   using phys_type = structo::phys_addr<void, my_phys_space_tag>;
 *
 *   static constexpr bool is_present(entry_type e) noexcept { return (e.value & (1ull << 0)) != 0; }
 *   // True only at a level where AllowsLeaf permits it; the walker, not
 *   // this trait, is responsible for only asking at such a level.
 *   static constexpr bool is_leaf(entry_type e) noexcept { return (e.value & (1ull << 1)) == 0; }
 *
 *   static phys_type child_table_addr(entry_type e) noexcept; // valid when !is_leaf(e)
 *   static phys_type leaf_frame_addr(entry_type e) noexcept;  // valid when is_leaf(e)
 *
 *   static entry_type make_table_entry(phys_type child_table) noexcept;
 *   static entry_type make_leaf_entry(phys_type frame, unsigned perm_bits) noexcept; // perm_bits: arch-specific
 * };
 * @endcode
 */
template <typename Tag> struct page_table_entry_traits;

} // namespace structo::arch
