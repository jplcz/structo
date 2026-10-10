// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file page_index_range.hpp
 * @brief Half-open index range `[first, end)` shared by everything that walks or trims pages by index.
 *
 * The index is *not* necessarily a PFN: a page-cache indexes its pages by offset inside the cached object, a
 * page array by position in the segment, a VM cache by page number in an address space. `page_index_range`
 * only does the arithmetic (size, overlap, intersect, subtract, split, aligned chunking, bounds-checked span
 * slicing) so each of those users does not reimplement it. It owns nothing and is a trivially copyable value.
 *
 * Iterators are `reloco::iterator_adaptor`s, so they work in range-for and with `.take()`, `.map()`, ...
 *
 * @code
 * using range = structo::page_index_range<std::uint64_t>;
 *
 * // [first, end): 'end' is one past the last index. 100 pages starting at index 4096.
 * auto r = range::from_count(4096, 100);
 *
 * // Trim a range out of a bigger one, e.g. drop cached offsets [4100, 4120) from a file's cached span.
 * auto d = r.subtract(range{4100, 4120});          // d.lower = [4096, 4100), d.upper = [4120, 4196)
 *
 * // Walk it in aligned chunks of 64 indices (chunk edges are multiples of 64, ends may be short).
 * for (auto &chunk : r.chunks(64)) { process(chunk); }
 *
 * // Visit the page descriptors of a range; 'base' is the index of pages[0] (a segment's first PFN, or 0
 * // for a file's page array). Returns nullopt when the range does not lie inside the span.
 * if (auto part = r.slice(pages, base)) { for (auto &p : *part) touch(p); }
 * @endcode
 *
 * Integration:
 * - **Page array / hotplug:** `page_window` (memory_window.hpp) exposes `first_pfn()`/`end_pfn()` and
 *   `buddy_allocator::claim_range` takes an inclusive PFN pair; convert with the constructor /
 *   `from_inclusive()` and use `subtract()` to skip already-offlined indices.
 * - **Page cache / VM cache trees:** use the range as the key interval for "erase everything in [a, b)" loops:
 *   `for (auto &c : r.chunks(batch)) { lock; erase_range(c.first(), c.end()); unlock; }` keeps lock hold
 *   times bounded without caring whether the keys are offsets or PFNs.
 */

#include <reloco/detail/assert.hpp>
#include <reloco/iterator.hpp>
#include <reloco/optional.hpp>
#include <reloco/span.hpp>

#include <cstddef>
#include <limits>
#include <type_traits>

namespace structo {

template <typename Index> class page_index_range;

/** Yields every index of a range in order. */
template <typename Index>
class page_index_iterator : public reloco::iterator_adaptor<page_index_iterator<Index>, Index> {
public:
  using item_type = Index;

  constexpr page_index_iterator(Index first, Index end) noexcept : next_(first), end_(end) {}

  [[nodiscard]] constexpr reloco::optional<Index> next_impl() noexcept {
    if (next_ >= end_) {
      return reloco::nullopt;
    }
    return next_++;
  }

private:
  Index next_;
  Index end_;
};

/** Yields sub-ranges whose edges are multiples of `chunk` (only the first and last may be short). */
template <typename Index>
class page_index_chunk_iterator
    : public reloco::iterator_adaptor<page_index_chunk_iterator<Index>, page_index_range<Index>> {
public:
  using item_type = page_index_range<Index>;

  constexpr page_index_chunk_iterator(Index first, Index end, Index chunk) noexcept
      : next_(first), end_(end), chunk_(chunk) {}

  [[nodiscard]] constexpr reloco::optional<item_type> next_impl() noexcept;

private:
  Index next_;
  Index end_;
  Index chunk_;
};

/** Result of `subtract`: what remains below and above the removed part (either may be empty). */
template <typename Index> struct page_index_difference {
  page_index_range<Index> lower;
  page_index_range<Index> upper;
};

template <typename Index> class page_index_range {
  static_assert(std::is_unsigned_v<Index>, "page_index_range needs an unsigned integer index");

public:
  using index_type = Index;

  /** Empty range. */
  constexpr page_index_range() noexcept = default;

  /** `[first, end)`; an inverted pair is normalized to empty. */
  constexpr page_index_range(Index first, Index end) noexcept : first_(first), end_(end >= first ? end : first) {}

  /** `count` indices starting at `first`; saturates at the maximum index instead of wrapping. */
  [[nodiscard]] static constexpr page_index_range from_count(Index first, Index count) noexcept {
    const Index room = std::numeric_limits<Index>::max() - first;
    return page_index_range{first, count > room ? std::numeric_limits<Index>::max() : Index(first + count)};
  }

  /** `[first, last]` with an inclusive last index (as `physical_constraint::high_pfn`). */
  [[nodiscard]] static constexpr page_index_range from_inclusive(Index first, Index last) noexcept {
    if (last < first) {
      return {};
    }
    return page_index_range{first, last == std::numeric_limits<Index>::max() ? last : Index(last + 1)};
  }

  [[nodiscard]] constexpr Index first() const noexcept { return first_; }
  [[nodiscard]] constexpr Index end() const noexcept { return end_; }
  [[nodiscard]] constexpr Index size() const noexcept { return Index(end_ - first_); }
  [[nodiscard]] constexpr bool empty() const noexcept { return first_ == end_; }

  [[nodiscard]] constexpr bool contains(Index i) const noexcept { return i >= first_ && i < end_; }
  [[nodiscard]] constexpr bool contains(const page_index_range &o) const noexcept {
    return o.empty() || (o.first_ >= first_ && o.end_ <= end_);
  }
  [[nodiscard]] constexpr bool overlaps(const page_index_range &o) const noexcept {
    return !empty() && !o.empty() && first_ < o.end_ && o.first_ < end_;
  }
  /** True when the ranges overlap or touch, i.e. `merge()` yields one contiguous range. */
  [[nodiscard]] constexpr bool mergeable(const page_index_range &o) const noexcept {
    return !empty() && !o.empty() && first_ <= o.end_ && o.first_ <= end_;
  }

  [[nodiscard]] constexpr page_index_range intersect(const page_index_range &o) const noexcept {
    const Index f = first_ > o.first_ ? first_ : o.first_;
    const Index e = end_ < o.end_ ? end_ : o.end_;
    return f < e ? page_index_range{f, e} : page_index_range{};
  }

  /** Smallest range covering both; only meaningful when `mergeable(o)` (asserted). */
  [[nodiscard]] constexpr page_index_range merge(const page_index_range &o) const noexcept {
    if (empty()) {
      return o;
    }
    if (o.empty()) {
      return *this;
    }
    RELOCO_ASSERT(mergeable(o), "page_index_range::merge: ranges are disjoint");
    return page_index_range{first_ < o.first_ ? first_ : o.first_, end_ > o.end_ ? end_ : o.end_};
  }

  /** Removes `o` from this range, keeping the part below and the part above it. */
  [[nodiscard]] constexpr page_index_difference<Index> subtract(const page_index_range &o) const noexcept {
    if (!overlaps(o)) {
      return {*this, page_index_range{}};
    }
    return {page_index_range{first_, o.first_ > first_ ? o.first_ : first_},
            page_index_range{o.end_ < end_ ? o.end_ : end_, end_}};
  }

  /** Splits at `at` (clamped into the range): `[first, at)` and `[at, end)`. */
  [[nodiscard]] constexpr page_index_difference<Index> split_at(Index at) const noexcept {
    const Index m = at < first_ ? first_ : (at > end_ ? end_ : at);
    return {page_index_range{first_, m}, page_index_range{m, end_}};
  }

  /** Every index in order. */
  [[nodiscard]] constexpr page_index_iterator<Index> indices() const noexcept { return {first_, end_}; }

  /** Sub-ranges with edges on multiples of `chunk` (0 yields the whole range as one chunk). */
  [[nodiscard]] constexpr page_index_chunk_iterator<Index> chunks(Index chunk) const noexcept {
    return {first_, end_, chunk};
  }

  /**
   * Bounds-checked view of the part of `s` covered by this range, where `s[0]` has index `base`.
   * Returns nullopt when the range is not fully inside `[base, base + s.size())`.
   */
  template <typename T>
  [[nodiscard]] constexpr reloco::optional<reloco::span<T>> slice(reloco::span<T> s, Index base) const noexcept {
    if (empty()) {
      return reloco::span<T>{};
    }
    if (first_ < base) {
      return reloco::nullopt;
    }
    const Index offset = Index(first_ - base);
    if (offset > s.size() || size() > s.size() - offset) {
      return reloco::nullopt;
    }
    return s.subspan(offset, size());
  }

  friend constexpr bool operator==(const page_index_range &a, const page_index_range &b) noexcept {
    return (a.empty() && b.empty()) || (a.first_ == b.first_ && a.end_ == b.end_);
  }
  friend constexpr bool operator!=(const page_index_range &a, const page_index_range &b) noexcept {
    return !(a == b);
  }

private:
  Index first_{};
  Index end_{};
};

template <typename Index>
constexpr reloco::optional<page_index_range<Index>> page_index_chunk_iterator<Index>::next_impl() noexcept {
  if (next_ >= end_) {
    return reloco::nullopt;
  }
  const Index start = next_;
  Index stop = end_;
  if (chunk_ != 0) {
    // Distance to the next multiple of chunk_, computed without overflowing near the maximum index.
    const Index to_edge = Index(chunk_ - start % chunk_);
    if (to_edge < end_ - start) {
      stop = Index(start + to_edge);
    }
  }
  next_ = stop;
  return page_index_range<Index>{start, stop};
}

} // namespace structo
