// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file memory_segment_os_traits.hpp
 * @brief `structo::memory_segment_os_traits<Derived, Provider, Page, OsPage>`: the `from_pfn` / `to_pfn` /
 * `is_same_zone` half of an `os_traits_base` implementation, backed by a `memory_segment_map`. Plug the
 * result into `page_view` (page math: `try_add`, `try_sub`, `try_get_buddy`, `phys()`) and `buddy_allocator`.
 *
 * `OsPage` (the handle stored in `page_view` and in the buddy free lists) is either
 *  - a pointer to the page descriptor (`Page *`): `from_pfn` is a map lookup, `to_pfn` a reverse lookup;
 *  - an unsigned integer: the handle is the PFN itself (compressed handles, no reverse lookup needed).
 *
 * `is_same_zone` is true when both pages are in the same segment, so page math and buddy merging never
 * cross a hole or a hot-plug boundary. `Provider::map()` supplies the snapshot to consult (typically the
 * currently published one) -- every call looks it up again, so it is safe across snapshot swaps.
 *
 * @code
 * struct page { std::uint64_t next, prev; std::uint16_t order; bool free; };
 * struct node_tag { std::uint8_t node; };
 * using segments = structo::fixed_memory_segment_map<page, node_tag, 8, 64>;
 *
 * // Where the current snapshot lives (e.g. the pointer published with the grace-period mechanism).
 * struct current_segments {
 *   static segments::map_type map() noexcept { return g_segments.view(); }
 * };
 *
 * // Buddy bookkeeping stays yours: page 'free'/'order' are plain fields of struct page.
 * // The handle is the PFN (OsPage = uint64_t), so descriptors are reached through the map.
 * struct os_traits : structo::memory_segment_os_traits<os_traits, current_segments, page, std::uint64_t> {
 *   // deref(handle) comes from the base class: handle -> struct page through the segment map.
 *   static std::uint16_t buddy_order(os_page_type p) noexcept { return deref(p).order; }
 *   static void set_buddy_order(os_page_type p, std::uint16_t o) noexcept { deref(p).order = o; }
 *   static bool is_buddy_free(os_page_type p) noexcept { return deref(p).free; }
 *   static void set_buddy_free(os_page_type p, bool f) noexcept { deref(p).free = f; }
 * };
 * using page_t = structo::page_view<structo::page_4k, os_traits>;
 * @endcode
 */

#include <structo/memory_segment_map.hpp>
#include <structo/phys_page.hpp>

#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/optional.hpp>

#include <cstdint>
#include <type_traits>

namespace structo {

template <typename Derived, typename Provider, typename Page, typename OsPage>
struct memory_segment_os_traits : os_traits_base<Derived, OsPage> {
  using os_page_type = OsPage;
  static_assert(std::is_pointer_v<OsPage> || std::is_unsigned_v<OsPage>,
                "OsPage must be a pointer to the descriptor or an unsigned PFN handle");

  [[nodiscard]] static constexpr os_page_type null_page() noexcept {
    if constexpr (std::is_pointer_v<OsPage>) {
      return nullptr;
    } else {
      return ~OsPage{0};
    }
  }
  [[nodiscard]] static constexpr bool is_null(os_page_type p) noexcept { return p == null_page(); }

  /** Handle for `pfn`, or `out_of_range` when no segment covers it. */
  [[nodiscard]] static reloco::result<os_page_type> from_pfn(std::uint64_t pfn) noexcept {
    const auto map = Provider::map();
    using pfn_type = typename decltype(map)::pfn_type;
    auto h = map.find(pfn_type{pfn});
    if (!h) {
      return reloco::unexpected(reloco::error::out_of_range);
    }
    if constexpr (std::is_pointer_v<OsPage>) {
      return &map.page_at(*h);
    } else {
      if (pfn >= static_cast<std::uint64_t>(null_page())) {
        return reloco::unexpected(reloco::error::out_of_range);
      }
      return static_cast<os_page_type>(pfn);
    }
  }

  /** PFN of a handle; 0 for a descriptor that is not in the map. */
  [[nodiscard]] static std::uint64_t to_pfn(os_page_type p) noexcept {
    if constexpr (std::is_pointer_v<OsPage>) {
      const auto map = Provider::map();
      auto h = map.find_page(*p);
      return h ? map.pfn_of(*h).value : 0;
    } else {
      return p;
    }
  }

  /** Both pages must be in the same segment. */
  [[nodiscard]] static bool is_same_zone(os_page_type a, os_page_type b) noexcept {
    const auto map = Provider::map();
    using pfn_type = typename decltype(map)::pfn_type;
    reloco::optional<typename decltype(map)::hit> ha;
    reloco::optional<typename decltype(map)::hit> hb;
    if constexpr (std::is_pointer_v<OsPage>) {
      ha = map.find_page(*a);
      hb = map.find_page(*b);
    } else {
      ha = map.find(pfn_type{a});
      hb = map.find(pfn_type{b});
    }
    return ha && hb && ha->segment_index == hb->segment_index;
  }

  /** Descriptor for a handle (`p` must be in the map). */
  [[nodiscard]] static Page &deref(os_page_type p) noexcept {
    const auto map = Provider::map();
    if constexpr (std::is_pointer_v<OsPage>) {
      return *p;
    } else {
      using pfn_type = typename decltype(map)::pfn_type;
      return map.page_at(*map.find(pfn_type{p}));
    }
  }
};

} // namespace structo
