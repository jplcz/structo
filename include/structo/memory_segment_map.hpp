// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file memory_segment_map.hpp
 * @brief `structo::memory_segment_map<Page, Tag, ...>`: an immutable snapshot of the physical memory segments
 * (hot-pluggable RAM blocks), each with a caller-defined tag and its array of page descriptors, with an
 * O(1) physical-address -> page descriptor lookup. `fixed_memory_segment_map` owns inline storage.
 *
 * ## Snapshot / copy-on-write model
 *
 * A snapshot is never modified. Inserting or removing a segment builds a *new* snapshot in caller-supplied
 * storage, so readers of the old one are not disturbed (retire the old storage once its readers are gone).
 * Mutation is two-phase:
 *
 *  1. `check_insert()` / `check_remove()` validate against the current snapshot (no overlap, sizes) and
 *     return a `change` that records exactly how much storage the new snapshot needs;
 *  2. `commit()` writes the new snapshot (sorted segment array + rebuilt lookup table) into storage and
 *     returns it. A `change` is bound to the snapshot generation it was checked against; committing it
 *     against a newer snapshot fails with `try_again`.
 *
 * ## Lookup table: small and flat
 *
 * The physical range is divided into *sections* of `2^SectionShift` pages (default 32768 pages = 128 MiB at
 * 4 KiB). The table has one `Index` entry (default 16-bit) per section from the lowest to the highest used
 * section, holding `segment index + 1` (0 = no memory). A lookup is one shift, one table load and one range
 * check against the segment. Memory use scales with the *span* of physical memory, not with its size:
 * 1 TiB of address space is 8192 sections = 16 KiB. A section can be touched by only one segment (segments may
 * start and end anywhere, but two segments cannot share a section); the check phase enforces this.
 *
 * @code
 * using namespace structo;
 *
 * // 'page' is the kernel's struct page; 'numa_tag' says which node a segment belongs to.
 * struct page { std::uint32_t flags; };
 * struct numa_tag { std::uint8_t node; };
 *
 * // Up to 8 segments and 64 sections (8 GiB of physical span at 128 MiB per section), inline storage.
 * using map_t = fixed_memory_segment_map<page, numa_tag, 8, 64>;
 *
 * map_t current;                          // empty snapshot
 * std::array<page, 1 << 18> pages_node0;  // descriptors for 1 GiB of RAM at 4 KiB pages
 *
 * // Segment: first page frame number, descriptor array (its size is the page count), and a tag.
 * map_t::segment seg{map_t::pfn_type{0x80000}, pages_node0.size(), numa_tag{0}, pages_node0};
 *
 * // Phase 1: validate only (no overlap, fits). Cheap; may be done without any lock held.
 * auto change = current.check_insert(seg);
 * if (change) {
 *   // Phase 2: build the new snapshot; publish it (e.g. one atomic pointer store) and retire 'current'
 *   // once its readers are done.
 *   auto next = current.commit(*change);
 *   // Hot path: physical address -> descriptor.
 *   auto hit = next->find(map_t::phys_type{0x8000'1000});
 *   if (hit) { page &p = next->page_at(*hit); (void)p; }
 * }
 * @endcode
 */

#include <structo/pfn_translator.hpp>
#include <structo/phys_addr.hpp>
#include <structo/phys_page.hpp>

#include <reloco/array.hpp>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/span.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <reloco/optional.hpp>
#include <type_traits>

namespace structo {

/**
 * @tparam Page        Page descriptor type (`struct page`).
 * @tparam Tag         Trivially copyable value attached to every segment (e.g. NUMA node, memory type).
 * @tparam PageTraits  Page size traits.
 * @tparam SpaceTag    Physical address space.
 * @tparam SectionShift log2(pages per lookup section).
 * @tparam Index       Table entry type; limits the segment count to `max(Index) - 1`.
 */
template <typename Page, typename Tag, typename PageTraits = page_4k, typename SpaceTag = default_phys_space,
          unsigned SectionShift = 15, typename Index = std::uint16_t>
class memory_segment_map {
  static_assert(std::is_trivially_copyable_v<Tag>, "segment tags are copied by value");
  static_assert(std::is_unsigned_v<Index>, "Index must be unsigned");
  static_assert(SectionShift < 63, "invalid section size");

public:
  using pfn_type = phys_pfn<SpaceTag, PageTraits, std::uint64_t>;
  using phys_type = phys_addr<void, SpaceTag, std::uint64_t>;
  static constexpr std::size_t max_segments = std::numeric_limits<Index>::max() - 1;

  /** One physical memory segment. */
  struct segment {
    pfn_type first{};                //!< First page frame of the segment.
    std::uint64_t page_count{0};     //!< Number of pages; must equal `pages.size()`.
    Tag tag{};                       //!< Caller-defined value.
    reloco::span<Page> pages;        //!< Page descriptors, one per page.

    [[nodiscard]] constexpr std::uint64_t first_value() const noexcept { return first.value; }
    [[nodiscard]] constexpr std::uint64_t end_value() const noexcept { return first.value + page_count; }
  };

  /** Result of `find`: which segment, and the page index inside it. */
  struct hit {
    std::size_t segment_index{0};
    std::size_t page_index{0};
  };

  /** Caller-provided storage for a new snapshot. Must not alias the snapshot being changed. */
  struct storage {
    reloco::span<segment> segments;
    reloco::span<Index> table;
  };

  /** Validated, not yet applied insertion or removal; see `commit`. */
  class change {
  public:
    /** Segment slots the new snapshot needs. */
    [[nodiscard]] constexpr std::size_t segment_count() const noexcept { return new_count_; }
    /** Table entries the new snapshot needs. */
    [[nodiscard]] constexpr std::size_t table_entries() const noexcept { return new_sections_; }

  private:
    friend class memory_segment_map;
    bool insert_{true};
    std::size_t index_{0}; // insert position, or index of the removed segment
    segment seg_{};
    std::size_t new_count_{0};
    std::uint64_t new_base_section_{0};
    std::size_t new_sections_{0};
    std::uint64_t generation_{0};
  };

  constexpr memory_segment_map() noexcept = default;

  /**
   * Wraps already-built contents (a sorted segment array plus its lookup table). Used by owners that copy
   * snapshots around; the arguments must come from a previous `commit`.
   */
  [[nodiscard]] static constexpr memory_segment_map attach(reloco::span<const segment> segments,
                                                           reloco::span<const Index> table,
                                                           std::uint64_t base_section,
                                                           std::uint64_t generation) noexcept {
    memory_segment_map m;
    m.segments_ = segments;
    m.table_ = table;
    m.base_section_ = base_section;
    m.generation_ = generation;
    return m;
  }

  [[nodiscard]] constexpr std::uint64_t base_section() const noexcept { return base_section_; }
  [[nodiscard]] constexpr std::size_t size() const noexcept { return segments_.size(); }
  [[nodiscard]] constexpr bool empty() const noexcept { return segments_.empty(); }
  [[nodiscard]] constexpr std::uint64_t generation() const noexcept { return generation_; }
  [[nodiscard]] constexpr reloco::span<const segment> segments() const noexcept { return segments_; }
  /** Table entries in use (sections between the lowest and highest segment). */
  [[nodiscard]] constexpr std::size_t table_entries() const noexcept { return table_.size(); }

  /** O(1): segment and page index containing `pfn`, if memory is present there. */
  [[nodiscard]] constexpr reloco::optional<hit> find(pfn_type pfn) const noexcept {
    if (pfn.is_null()) {
      return reloco::nullopt;
    }
    const std::uint64_t sec = pfn.value >> SectionShift;
    if (sec < base_section_ || sec - base_section_ >= table_.size()) {
      return reloco::nullopt;
    }
    const Index e = table_[static_cast<std::size_t>(sec - base_section_)];
    if (e == 0) {
      return reloco::nullopt;
    }
    const segment &s = segments_[static_cast<std::size_t>(e) - 1];
    if (pfn.value < s.first_value() || pfn.value >= s.end_value()) {
      return reloco::nullopt;
    }
    return hit{static_cast<std::size_t>(e) - 1, static_cast<std::size_t>(pfn.value - s.first_value())};
  }

  [[nodiscard]] constexpr reloco::optional<hit> find(phys_type addr) const noexcept {
    if (addr.is_null()) {
      return reloco::nullopt;
    }
    return find(pfn_type::from_addr(addr));
  }

  /** Reverse lookup: which segment/page index holds this descriptor (O(segments), address comparison). */
  [[nodiscard]] reloco::optional<hit> find_page(const Page &page) const noexcept {
    // Integer address comparison: descriptors of different segments are unrelated objects.
    const auto addr = reinterpret_cast<std::uintptr_t>(&page);
    for (std::size_t i = 0; i < segments_.size(); ++i) {
      const reloco::span<Page> &arr = segments_[i].pages;
      const auto base = reinterpret_cast<std::uintptr_t>(arr.data());
      if (addr >= base && addr - base < arr.size() * sizeof(Page)) {
        return hit{i, static_cast<std::size_t>((addr - base) / sizeof(Page))};
      }
    }
    return reloco::nullopt;
  }

  /** Page frame number of a lookup result. */
  [[nodiscard]] constexpr pfn_type pfn_of(const hit &h) const noexcept {
    return pfn_type{segments_[h.segment_index].first_value() + h.page_index};
  }

  [[nodiscard]] constexpr const segment &segment_at(const hit &h) const noexcept { return segments_[h.segment_index]; }
  [[nodiscard]] constexpr Page &page_at(const hit &h) const noexcept {
    return segments_[h.segment_index].pages[h.page_index];
  }
  [[nodiscard]] constexpr const Tag &tag_at(const hit &h) const noexcept { return segments_[h.segment_index].tag; }

  /** Phase 1 of insertion: validates `seg` against this snapshot without modifying anything. */
  [[nodiscard]] reloco::result<change> check_insert(const segment &seg) const noexcept {
    if (seg.first.is_null() || seg.page_count == 0 || seg.pages.size() != seg.page_count) {
      return reloco::unexpected(reloco::error::invalid_argument);
    }
    if (seg.first.value > std::numeric_limits<std::uint64_t>::max() - seg.page_count) {
      return reloco::unexpected(reloco::error::integer_overflow);
    }
    if (segments_.size() >= max_segments) {
      return reloco::unexpected(reloco::error::capacity_exceeded);
    }
    const std::uint64_t s0 = seg.first_value() >> SectionShift;
    const std::uint64_t s1 = (seg.end_value() - 1) >> SectionShift;
    std::size_t pos = segments_.size();
    for (std::size_t i = 0; i < segments_.size(); ++i) {
      const segment &o = segments_[i];
      const std::uint64_t o0 = o.first_value() >> SectionShift;
      const std::uint64_t o1 = (o.end_value() - 1) >> SectionShift;
      if (s0 <= o1 && o0 <= s1) {
        return reloco::unexpected(reloco::error::already_exists); // shares a section with an existing segment
      }
      if (pos == segments_.size() && o.first_value() > seg.first_value()) {
        pos = i;
      }
    }

    change c;
    c.insert_ = true;
    c.index_ = pos;
    c.seg_ = seg;
    c.new_count_ = segments_.size() + 1;
    c.generation_ = generation_;
    std::uint64_t lo = s0;
    std::uint64_t hi = s1;
    for (const segment &o : segments_) {
      lo = o.first_value() >> SectionShift < lo ? o.first_value() >> SectionShift : lo;
      const std::uint64_t e = (o.end_value() - 1) >> SectionShift;
      hi = e > hi ? e : hi;
    }
    c.new_base_section_ = lo;
    c.new_sections_ = static_cast<std::size_t>(hi - lo + 1);
    return c;
  }

  /** Phase 1 of removal: `first` must be the first page frame of an existing segment. */
  [[nodiscard]] reloco::result<change> check_remove(pfn_type first) const noexcept {
    std::size_t idx = segments_.size();
    for (std::size_t i = 0; i < segments_.size(); ++i) {
      if (segments_[i].first == first) {
        idx = i;
      }
    }
    if (idx == segments_.size()) {
      return reloco::unexpected(reloco::error::not_found);
    }
    change c;
    c.insert_ = false;
    c.index_ = idx;
    c.new_count_ = segments_.size() - 1;
    c.generation_ = generation_;
    bool any = false;
    std::uint64_t lo = 0;
    std::uint64_t hi = 0;
    for (std::size_t i = 0; i < segments_.size(); ++i) {
      if (i == idx) {
        continue;
      }
      const std::uint64_t a = segments_[i].first_value() >> SectionShift;
      const std::uint64_t b = (segments_[i].end_value() - 1) >> SectionShift;
      lo = (!any || a < lo) ? a : lo;
      hi = (!any || b > hi) ? b : hi;
      any = true;
    }
    c.new_base_section_ = any ? lo : 0;
    c.new_sections_ = any ? static_cast<std::size_t>(hi - lo + 1) : 0;
    return c;
  }

  /**
   * Phase 2: builds the new snapshot into `dest` (`dest.segments`/`dest.table` need at least
   * `change.segment_count()` / `change.table_entries()` elements and must not alias this snapshot's storage)
   * and returns it. Fails with `try_again` when this snapshot is not the one the change was checked against.
   */
  [[nodiscard]] reloco::result<memory_segment_map> commit(const change &c, const storage &dest) const noexcept {
    if (c.generation_ != generation_) {
      return reloco::unexpected(reloco::error::try_again);
    }
    if (dest.segments.size() < c.new_count_ || dest.table.size() < c.new_sections_) {
      return reloco::unexpected(reloco::error::capacity_exceeded);
    }

    std::size_t out = 0;
    for (std::size_t i = 0; i < segments_.size(); ++i) {
      if (c.insert_ && i == c.index_) {
        dest.segments[out++] = c.seg_;
      }
      if (!c.insert_ && i == c.index_) {
        continue;
      }
      dest.segments[out++] = segments_[i];
    }
    if (c.insert_ && c.index_ == segments_.size()) {
      dest.segments[out++] = c.seg_;
    }

    for (std::size_t i = 0; i < c.new_sections_; ++i) {
      dest.table[i] = 0;
    }
    for (std::size_t i = 0; i < out; ++i) {
      const std::uint64_t s0 = dest.segments[i].first_value() >> SectionShift;
      const std::uint64_t s1 = (dest.segments[i].end_value() - 1) >> SectionShift;
      for (std::uint64_t s = s0; s <= s1; ++s) {
        dest.table[static_cast<std::size_t>(s - c.new_base_section_)] = static_cast<Index>(i + 1);
      }
    }

    memory_segment_map next;
    next.segments_ = reloco::span<const segment>(dest.segments.data(), out);
    next.table_ = reloco::span<const Index>(dest.table.data(), c.new_sections_);
    next.base_section_ = c.new_base_section_;
    next.generation_ = generation_ + 1;
    return next;
  }

private:
  reloco::span<const segment> segments_;
  reloco::span<const Index> table_;
  std::uint64_t base_section_{0};
  std::uint64_t generation_{0};
};

/**
 * @brief `memory_segment_map` that owns inline storage for up to `MaxSegments` segments and `MaxSections`
 * table entries. `commit` returns a new, independent object (copy-on-write); the old one stays valid.
 */
template <typename Page, typename Tag, std::size_t MaxSegments, std::size_t MaxSections,
          typename PageTraits = page_4k, typename SpaceTag = default_phys_space, unsigned SectionShift = 15,
          typename Index = std::uint16_t>
class fixed_memory_segment_map {
public:
  using map_type = memory_segment_map<Page, Tag, PageTraits, SpaceTag, SectionShift, Index>;
  using segment = typename map_type::segment;
  using hit = typename map_type::hit;
  using change = typename map_type::change;
  using pfn_type = typename map_type::pfn_type;
  using phys_type = typename map_type::phys_type;

  static_assert(MaxSegments <= map_type::max_segments, "Index type too small for MaxSegments");

  fixed_memory_segment_map() noexcept = default;

  /** Read-only snapshot over this object's storage (invalidated by moving/destroying the object). */
  [[nodiscard]] map_type view() const noexcept {
    return map_type::attach(reloco::span<const segment>(segments_.data(), count_),
                            reloco::span<const Index>(table_.data(), sections_), base_section_, generation_);
  }

  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
  [[nodiscard]] reloco::optional<hit> find(pfn_type pfn) const noexcept { return view().find(pfn); }
  [[nodiscard]] reloco::optional<hit> find(phys_type addr) const noexcept { return view().find(addr); }
  [[nodiscard]] const segment &segment_at(const hit &h) const noexcept { return segments_[h.segment_index]; }
  [[nodiscard]] Page &page_at(const hit &h) const noexcept { return segments_[h.segment_index].pages[h.page_index]; }
  [[nodiscard]] const Tag &tag_at(const hit &h) const noexcept { return segments_[h.segment_index].tag; }
  [[nodiscard]] reloco::optional<hit> find_page(const Page &page) const noexcept { return view().find_page(page); }
  [[nodiscard]] pfn_type pfn_of(const hit &h) const noexcept { return view().pfn_of(h); }

  [[nodiscard]] reloco::result<change> check_insert(const segment &seg) const noexcept {
    auto c = view().check_insert(seg);
    if (c && (c->segment_count() > MaxSegments || c->table_entries() > MaxSections)) {
      return reloco::unexpected(reloco::error::capacity_exceeded);
    }
    return c;
  }
  [[nodiscard]] reloco::result<change> check_remove(pfn_type first) const noexcept {
    return view().check_remove(first);
  }

  /** Builds the changed snapshot as a new object; `*this` is untouched. */
  [[nodiscard]] reloco::result<fixed_memory_segment_map> commit(const change &c) const noexcept {
    fixed_memory_segment_map next;
    auto r = view().commit(c, typename map_type::storage{reloco::span<segment>(next.segments_.data(), MaxSegments),
                                                         reloco::span<Index>(next.table_.data(), MaxSections)});
    if (!r) {
      return reloco::unexpected(r.error());
    }
    next.count_ = r->size();
    next.sections_ = r->table_entries();
    next.base_section_ = r->base_section();
    next.generation_ = r->generation();
    return next;
  }

private:
  reloco::array<segment, MaxSegments> segments_{};
  reloco::array<Index, MaxSections> table_{};
  std::size_t count_{0};
  std::size_t sections_{0};
  std::uint64_t base_section_{0};
  std::uint64_t generation_{0};
};

} // namespace structo
