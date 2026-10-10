// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file memory_segment_lookup.hpp
 * @brief Ready-made `pfn_to_page` / `phys_to_page` / `page_to_pfn` / `page_to_phys` helpers on top of a
 * memory segment map, as static functions an OS can call from its own `pfn_to_page()` macros.
 *
 * Two kinds of helpers (see docs/memory_hotplug.md):
 *
 *  - *Pinned* (`pfn_to_page`, `phys_to_page`, `page_to_pfn`, ...): the caller already owns something that keeps
 *    the segment published (a page reference, a page-cache lock, a buddy allocation). The read guard lives only
 *    for the lookup; the returned descriptor pointer is valid afterwards because the segment cannot be removed
 *    while the caller's ownership exists. Pointer returns are part of the OS-facing contract.
 *  - *Unpinned* (`with_page`, `try_get_page`, `for_each_page`): the caller only has a number. The work runs
 *    inside the read guard and the descriptor never escapes; `try_get_page` takes a reference through the
 *    caller's `try_get` before leaving the guard, which turns the page into a pinned one.
 *
 * `Source` selects where the map comes from:
 *  - `hotplug_segment_source<Provider>`: `Provider::hotplug()` returns the global `memory_hotplug&`.
 *  - `static_segment_source<Provider>`: `Provider::map()` returns the (immutable) map by value; no hotplug.
 *
 * @code
 * struct page { std::uint32_t flags; std::atomic<std::int32_t> refs; };
 * using segments = structo::fixed_memory_segment_map<page, numa_tag, 8, 64>;
 * inline structo::memory_hotplug<segments, hotplug_traits> g_hotplug;
 *
 * // Tells the helpers which published map to use (any thread, any time).
 * struct provider { static auto &hotplug() noexcept { return g_hotplug; } };
 * using mm = structo::memory_segment_lookup<structo::hotplug_segment_source<provider>>;
 *
 * // The OS-facing names, one line each.
 * inline page *pfn_to_page(std::uint64_t pfn) { return mm::pfn_to_page(pfn); }
 * inline page *phys_to_page(std::uint64_t pa) { return mm::phys_to_page(pa); }
 *
 * // From a bare PFN (e.g. a device or /proc/kpageflags): take a reference inside the guard.
 * page *p = mm::try_get_page(pfn, [](page &pg) {
 *   return pg.refs.fetch_add(1) > 0; // fail for free pages (refcount 0)
 * });
 * @endcode
 */

#include <structo/memory_hotplug.hpp>
#include <structo/memory_segment_map.hpp>

#include <cstddef>
#include <cstdint>
#include <reloco/optional.hpp>
#include <type_traits>
#include <utility>

namespace structo {

/** Source backed by a `memory_hotplug`; `Provider::hotplug()` returns it by reference. */
template <typename Provider> struct hotplug_segment_source {
  using hotplug_type = std::remove_cv_t<std::remove_reference_t<decltype(Provider::hotplug())>>;
  using map_type = typename hotplug_type::map_type;
  [[nodiscard]] static auto read() noexcept { return Provider::hotplug().read(); }
};

/** Source for systems without hotplug: `Provider::map()` returns the immutable map by value. */
template <typename Provider> struct static_segment_source {
  using map_type = std::remove_cv_t<std::remove_reference_t<decltype(Provider::map())>>;

  /** Guard-shaped holder so both sources are used the same way. */
  class guard {
  public:
    explicit guard(map_type m) noexcept : map_(std::move(m)) {}
    [[nodiscard]] const map_type *operator->() const noexcept { return &map_; }
    [[nodiscard]] const map_type &operator*() const noexcept { return map_; }

  private:
    map_type map_;
  };

  [[nodiscard]] static guard read() noexcept { return guard{Provider::map()}; }
};

/** Default `PfnHook`: recover a descriptor's PFN by locating its segment (O(segments), needs the guard). */
struct scan_page_to_pfn {};

/**
 * @tparam PfnHook `scan_page_to_pfn` (default), or a type with `static std::uint64_t pfn(const Page &) noexcept`
 * for descriptors that store their own PFN (or compute it cheaply). With a hook `page_to_pfn` / `page_to_phys`
 * need no guard and no segment scan, but only valid for pinned pages, as they never consult the map.
 */
template <typename Source, typename PfnHook = scan_page_to_pfn> struct memory_segment_lookup {
  using map_type = typename Source::map_type;
  using page_type = std::remove_cv_t<std::remove_reference_t<decltype(std::declval<const map_type &>().page_at(
      std::declval<const typename map_type::hit &>()))>>;
  using segment = typename map_type::segment;
  using pfn_type = typename map_type::pfn_type;
  using phys_type = typename map_type::phys_type;
  using tag_type = std::remove_cv_t<std::remove_reference_t<decltype(std::declval<const map_type &>().tag_at(
      std::declval<const typename map_type::hit &>()))>>;

  static constexpr std::uint64_t page_shift = pfn_type::page_traits::page_shift;

  /** True when `pfn` is backed by a descriptor (the `pfn_valid()` of the OS). */
  [[nodiscard]] static bool pfn_valid(std::uint64_t pfn) noexcept {
    auto g = Source::read();
    return g->find(pfn_type{pfn}).has_value();
  }

  /** Pinned lookup: descriptor of `pfn` or null. See the file comment for the caller's obligations. */
  [[nodiscard]] static page_type *pfn_to_page(std::uint64_t pfn) noexcept {
    auto g = Source::read();
    auto h = g->find(pfn_type{pfn});
    return h ? &g->page_at(*h) : nullptr;
  }

  /** Pinned lookup from a physical address (any byte inside the page). */
  [[nodiscard]] static page_type *phys_to_page(std::uint64_t phys) noexcept {
    auto g = Source::read();
    auto h = g->find(phys_type{phys});
    return h ? &g->page_at(*h) : nullptr;
  }

  /** PFN of a descriptor, or nullopt when it is not part of any published segment. O(segments) unless a `PfnHook` is given. */
  [[nodiscard]] static reloco::optional<std::uint64_t> page_to_pfn(const page_type &p) noexcept {
    if constexpr (!std::is_same_v<PfnHook, scan_page_to_pfn>) {
      return PfnHook::pfn(p);
    } else {
      auto g = Source::read();
      auto h = g->find_page(p);
      if (!h) {
        return reloco::nullopt;
      }
      return g->pfn_of(*h).value;
    }
  }

  /** Physical address of the first byte of a descriptor's page. */
  [[nodiscard]] static reloco::optional<std::uint64_t> page_to_phys(const page_type &p) noexcept {
    auto pfn = page_to_pfn(p);
    if (!pfn) {
      return reloco::nullopt;
    }
    return *pfn << page_shift;
  }

  /** Tag (e.g. NUMA node) of the segment covering `pfn`. */
  [[nodiscard]] static reloco::optional<tag_type> tag_of(std::uint64_t pfn) noexcept {
    auto g = Source::read();
    auto h = g->find(pfn_type{pfn});
    if (!h) {
      return reloco::nullopt;
    }
    return g->tag_at(*h);
  }

  /** Copy of the segment covering `pfn` (descriptor span included; usable only while the segment is pinned). */
  [[nodiscard]] static reloco::optional<segment> segment_of(std::uint64_t pfn) noexcept {
    auto g = Source::read();
    auto h = g->find(pfn_type{pfn});
    if (!h) {
      return reloco::nullopt;
    }
    return g->segment_at(*h);
  }

  /**
   * Unpinned access: calls `fn(page_type &, const tag_type &)` inside the guard. The descriptor must not
   * escape and `fn` must not block. Returns false when no segment covers `pfn`.
   */
  template <typename Fn> static bool with_page(std::uint64_t pfn, Fn &&fn) {
    auto g = Source::read();
    auto h = g->find(pfn_type{pfn});
    if (!h) {
      return false;
    }
    fn(g->page_at(*h), g->tag_at(*h));
    return true;
  }

  /**
   * Unpinned access that ends pinned: `try_get(page_type &) -> bool` runs inside the guard and should take a
   * reference (return false for free / isolated / migrating pages). On success the descriptor is returned and
   * stays valid while the caller holds that reference; the caller's hot-remove side must see the reference
   * and retry the page.
   */
  template <typename TryGet> [[nodiscard]] static page_type *try_get_page(std::uint64_t pfn, TryGet &&try_get) {
    auto g = Source::read();
    auto h = g->find(pfn_type{pfn});
    if (!h) {
      return nullptr;
    }
    page_type &p = g->page_at(*h);
    return try_get(p) ? &p : nullptr;
  }

  /**
   * Visits every backed page in `[first_pfn, first_pfn + count)` as `fn(pfn, page_type &)`, skipping holes,
   * under one guard. `fn` must not block; split long ranges into batches to keep the grace period short.
   */
  template <typename Fn> static void for_each_page(std::uint64_t first_pfn, std::uint64_t count, Fn &&fn) {
    auto g = Source::read();
    std::uint64_t pfn = first_pfn;
    const std::uint64_t end = first_pfn + count;
    while (pfn < end) {
      auto h = g->find(pfn_type{pfn});
      if (!h) {
        ++pfn;
        continue;
      }
      const segment &seg = g->segment_at(*h);
      // Stay inside the segment and the requested range; one lookup covers the whole run.
      std::uint64_t run_end = seg.end_value() < end ? seg.end_value() : end;
      for (std::size_t i = h->page_index; pfn < run_end; ++i, ++pfn) {
        fn(pfn, seg.pages[i]);
      }
    }
  }
};

} // namespace structo
