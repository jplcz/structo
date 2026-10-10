// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file memory_window.hpp
 * @brief Walks a segment's page-descriptor array in fixed-size windows, for offlining memory a window at a time.
 *
 * `page_window_walker<Page>` is a `reloco::iterator_adaptor`: it yields one `page_window<Page>` per step (and
 * works in a range-for and with `.take()`, `.map()`, ...). Window boundaries are aligned to multiples of
 * `window_pages` in PFN space, so windows line up with buddy blocks / page blocks; only the first and last
 * windows of a segment can be short. A `page_window` owns no memory: it is a bounds-checked `reloco::span`
 * plus the PFN of its first page, and all page access goes through that span (no pointer arithmetic).
 *
 * @code
 * // 'seg' is a memory_segment_map segment: {first, page_count, tag, pages}. 8192 pages = 32 MiB at 4 KiB.
 * auto windows = structo::page_window_walker<page>(seg, 8192);
 * for (auto &w : windows) {                      // one window at a time; the rest of the segment stays in use
 *   isolate(w.first_pfn(), w.end_pfn());         // PFN range of this window, [first, end)
 *   for (auto &wp : w.walk()) {                  // every descriptor of the window, with its PFN
 *     if (wp.page().flags & PG_FREE) take_from_buddy(wp.pfn);
 *     else migrate(wp.page());
 *   }
 * }
 * @endcode
 */

#include <structo/page_index_range.hpp>

#include <reloco/array.hpp>
#include <reloco/iterator.hpp>
#include <reloco/optional.hpp>
#include <reloco/span.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>

namespace structo {

using pfn_range = page_index_range<std::uint64_t>;

/**
 * Log of PFN runs that are already dealt with (for example the runs `buddy_allocator::claim_range` handed to its
 * sink). Touching or overlapping runs are merged, so a window usually needs only a few entries.
 *
 * The log does not own its storage: it works inside a caller-provided `reloco::span<pfn_range>`, so the memory can
 * live in a per-disconnector object, a static, or an early allocation instead of on a (small) kernel stack.
 * `add` returns false when the storage is full; the run is then simply not remembered and the walker will look
 * at those pages again.
 */
class pfn_range_log {
public:
  explicit pfn_range_log(reloco::span<pfn_range> storage) noexcept : storage_(storage) {}

  bool add(pfn_range r) noexcept {
    if (r.empty()) {
      return true;
    }
    for (std::size_t i = 0; i < count_; ++i) {
      if (storage_[i].mergeable(r)) {
        r = storage_[i].merge(r);
        storage_[i] = storage_[--count_];
        i = static_cast<std::size_t>(-1); // restart: the merged run may now touch others
      }
    }
    if (count_ == storage_.size()) {
      return false;
    }
    storage_[count_++] = r;
    return true;
  }

  [[nodiscard]] bool covers(std::uint64_t pfn) const noexcept {
    for (std::size_t i = 0; i < count_; ++i) {
      if (storage_[i].contains(pfn)) {
        return true;
      }
    }
    return false;
  }

  /** The remembered runs (unordered). Valid until the next `add`/`clear`. */
  [[nodiscard]] reloco::span<const pfn_range> runs() const noexcept {
    // span::first() is lifetime-bound to the span member itself; build the view over the storage directly.
    return reloco::span<const pfn_range>(storage_.data(), count_);
  }
  [[nodiscard]] std::size_t size() const noexcept { return count_; }
  void clear() noexcept { count_ = 0; }

private:
  reloco::span<pfn_range> storage_;
  std::size_t count_{0};
};

/** Yields the sub-ranges of `window` not covered by any of `done` (the pages still to inspect). */
class pending_pfn_iterator : public reloco::iterator_adaptor<pending_pfn_iterator, pfn_range> {
public:
  using item_type = pfn_range;

  pending_pfn_iterator(pfn_range window, reloco::span<const pfn_range> done) noexcept
      : cursor_(window.first()), end_(window.end()), done_(done) {}

  [[nodiscard]] reloco::optional<pfn_range> next_impl() noexcept {
    // Step over done runs that cover the cursor (repeat: a run may end inside another).
    for (bool moved = true; moved;) {
      moved = false;
      for (const auto &d : done_) {
        if (d.contains(cursor_)) {
          cursor_ = d.end();
          moved = true;
        }
      }
    }
    if (cursor_ >= end_) {
      return reloco::nullopt;
    }
    std::uint64_t stop = end_;
    for (const auto &d : done_) {
      if (!d.empty() && d.first() > cursor_ && d.first() < stop) {
        stop = d.first();
      }
    }
    const pfn_range piece{cursor_, stop};
    cursor_ = stop;
    return piece;
  }

private:
  std::uint64_t cursor_;
  std::uint64_t end_;
  reloco::span<const pfn_range> done_;
};

/** One descriptor of a window together with its PFN. */
template <typename Page> struct window_page {
  std::uint64_t pfn;
  std::reference_wrapper<Page> ref;

  [[nodiscard]] Page &page() const noexcept { return ref.get(); }
};

/** Iterates the descriptors of one window as `window_page<Page>`. */
template <typename Page>
class window_page_iterator : public reloco::iterator_adaptor<window_page_iterator<Page>, window_page<Page>> {
public:
  using item_type = window_page<Page>;

  window_page_iterator(std::uint64_t first_pfn, reloco::span<Page> pages) noexcept
      : first_pfn_(first_pfn), pages_(pages) {}

  [[nodiscard]] reloco::optional<item_type> next_impl() noexcept {
    if (pos_ >= pages_.size()) {
      return reloco::nullopt;
    }
    const std::size_t i = pos_++;
    return reloco::optional<item_type>(item_type{first_pfn_ + i, std::ref(pages_[i])});
  }

private:
  std::uint64_t first_pfn_;
  reloco::span<Page> pages_;
  std::size_t pos_{0};
};

/** A PFN range `[first_pfn, first_pfn + size)` and the descriptors that belong to it. */
template <typename Page> class page_window {
public:
  page_window() noexcept = default;
  page_window(std::uint64_t first_pfn, reloco::span<Page> pages) noexcept : first_pfn_(first_pfn), pages_(pages) {}

  [[nodiscard]] std::uint64_t first_pfn() const noexcept { return first_pfn_; }
  [[nodiscard]] std::uint64_t end_pfn() const noexcept { return first_pfn_ + pages_.size(); }
  [[nodiscard]] std::size_t size() const noexcept { return pages_.size(); }
  [[nodiscard]] reloco::span<Page> pages() const noexcept { return pages_; }

  [[nodiscard]] bool contains(std::uint64_t pfn) const noexcept { return pfn >= first_pfn_ && pfn < end_pfn(); }

  /** Index of `pfn` inside `pages()`, or nullopt when outside the window. */
  [[nodiscard]] reloco::optional<std::size_t> index_of(std::uint64_t pfn) const noexcept {
    if (!contains(pfn)) {
      return reloco::nullopt;
    }
    return static_cast<std::size_t>(pfn - first_pfn_);
  }

  /** Bounds-checked access by PFN; empty when `pfn` is outside the window. */
  [[nodiscard]] reloco::optional<std::reference_wrapper<Page>> page_at(std::uint64_t pfn) const noexcept {
    auto i = index_of(pfn);
    if (!i) {
      return reloco::nullopt;
    }
    return reloco::optional<std::reference_wrapper<Page>>(std::ref(pages_[*i]));
  }

  /** PFN range `[first_pfn, end_pfn)` of the window. */
  [[nodiscard]] pfn_range range() const noexcept { return {first_pfn_, end_pfn()}; }

  /** Iterator over every descriptor of the window with its PFN (a `reloco::iterator_adaptor`). */
  [[nodiscard]] window_page_iterator<Page> walk() const noexcept { return {first_pfn_, pages_}; }

  /** Like `walk()` but only for `r` (which must lie inside the window; otherwise the iterator is empty). */
  [[nodiscard]] window_page_iterator<Page> walk(pfn_range r) const noexcept {
    auto part = r.slice(pages_, first_pfn_);
    if (!part) {
      return {first_pfn_, reloco::span<Page>{}};
    }
    return {r.first(), *part};
  }

  /** PFN sub-ranges of this window that are not in `done`; feed each to `walk(range)`. */
  [[nodiscard]] pending_pfn_iterator pending(reloco::span<const pfn_range> done) const noexcept {
    return {range(), done};
  }

private:
  std::uint64_t first_pfn_{0};
  reloco::span<Page> pages_{};
};

/** Yields successive `page_window<Page>` over a descriptor array; windows are aligned to `window_pages` in PFN. */
template <typename Page>
class page_window_walker : public reloco::iterator_adaptor<page_window_walker<Page>, page_window<Page>> {
public:
  using item_type = page_window<Page>;

  /**
   * @param first_pfn   PFN of `pages[0]`.
   * @param pages       Descriptor array of the segment.
   * @param window_pages Window size in pages (0 = a single window covering everything).
   */
  page_window_walker(std::uint64_t first_pfn, reloco::span<Page> pages, std::uint64_t window_pages) noexcept
      : first_pfn_(first_pfn), pages_(pages), window_(window_pages) {}

  /** From anything segment-shaped: `first.value` and `pages` (a `memory_segment_map` segment). */
  template <typename Segment>
  page_window_walker(const Segment &seg, std::uint64_t window_pages) noexcept
      : page_window_walker(seg.first.value, seg.pages, window_pages) {}

  [[nodiscard]] reloco::optional<item_type> next_impl() noexcept {
    if (pos_ >= pages_.size()) {
      return reloco::nullopt;
    }
    const std::uint64_t remaining = pages_.size() - pos_;
    std::uint64_t count = remaining;
    if (window_ != 0) {
      const std::uint64_t pfn = first_pfn_ + pos_;
      const std::uint64_t to_boundary = window_ - pfn % window_; // 1..window_
      count = to_boundary < remaining ? to_boundary : remaining;
    }
    const std::size_t at = pos_;
    pos_ += static_cast<std::size_t>(count);
    return reloco::optional<item_type>(
        item_type{first_pfn_ + at, pages_.subspan(at, static_cast<std::size_t>(count))});
  }

  /** PFN of the next window's first page (the "walk mark"); `end` when finished. */
  [[nodiscard]] std::uint64_t next_pfn() const noexcept { return first_pfn_ + pos_; }

private:
  std::uint64_t first_pfn_;
  reloco::span<Page> pages_;
  std::uint64_t window_;
  std::size_t pos_{0};
};

} // namespace structo
