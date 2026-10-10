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

#include <reloco/iterator.hpp>
#include <reloco/optional.hpp>
#include <reloco/span.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>

namespace structo {

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

  /** Iterator over every descriptor of the window with its PFN (a `reloco::iterator_adaptor`). */
  [[nodiscard]] window_page_iterator<Page> walk() const noexcept { return {first_pfn_, pages_}; }

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
