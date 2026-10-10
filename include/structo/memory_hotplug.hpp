// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file memory_hotplug.hpp
 * @brief `structo::memory_hotplug<Map, Traits>`: sequences memory hot-add / hot-remove on top of a
 * `fixed_memory_segment_map` published through a `snapshot_domain`. Every thread always sees one complete,
 * valid map; removal additionally guarantees that when the caller's `on_quiesced` runs, no thread can still
 * hold a descriptor pointer from the removed segment.
 *
 * ## Sequencing
 *
 * Hot-add: (1) the caller initializes the segment's page descriptors, (2) `add()` validates, builds and
 * publishes the new map with a single atomic store, so readers either see none of the segment or all of it
 * with initialized descriptors. No wait is needed; the previous map is drained lazily before the next update.
 *
 * Hot-remove: (1) the caller has already taken the pages out of use (offlined, migrated, off the buddy
 * lists), (2) `remove()` publishes a map without the segment, so new lookups no longer find it, (3) waits
 * until every reader of the previous map has left, and (4) only then calls `on_quiesced(segment)`, where the
 * caller frees or unmaps the page-descriptor array and removes the memory.
 *
 * @code
 * // 'traits' provides shards, current_shard(), mutex_type, wait() and wake_all(); see snapshot_domain.hpp.
 * using segments = structo::fixed_memory_segment_map<page, numa_tag, 8, 64>;
 * structo::memory_hotplug<segments, traits> hotplug;
 *
 * // Lookup anywhere, any thread, any context that may hold a guard: descriptor valid while 'g' lives.
 * {
 *   auto g = hotplug.read();
 *   if (auto h = g->find(segments::phys_type{pa})) {
 *     page &p = g->page_at(*h);
 *     touch(p);
 *   }
 * }
 *
 * // Hot-add: descriptors first, then publish.
 * init_descriptors(new_seg.pages);
 * (void)hotplug.add(new_seg);
 *
 * // Hot-remove: segment starts at 'first_pfn'. Called after the pages were taken out of use.
 * // The lambda runs once nothing can hold a descriptor pointer from the segment anymore.
 * (void)hotplug.remove(first_pfn, [](const segments::segment &s) { free_descriptors(s.pages); });
 * @endcode
 */

#include <structo/snapshot_domain.hpp>

#include <reloco/error.hpp>
#include <reloco/expected.hpp>

#include <reloco/optional.hpp>
#include <utility>

namespace structo {

template <typename Map, typename Traits> class memory_hotplug {
public:
  using map_type = Map;
  using segment = typename Map::segment;
  using pfn_type = typename Map::pfn_type;
  using domain_type = snapshot_domain<Map, Traits>;
  using read_guard = typename domain_type::read_guard;

  memory_hotplug() = default;
  explicit memory_hotplug(Map initial) : domain_(std::move(initial)) {}

  /** Pins the current map (see `snapshot_domain::read`). */
  [[nodiscard]] read_guard read() noexcept { return domain_.read(); }

  /** Publishes `seg`. Descriptors must already be initialized. Does not wait for readers. */
  [[nodiscard]] reloco::result<void> add(const segment &seg) {
    return domain_.update_async([&](const Map &cur) -> reloco::result<Map> {
      auto c = cur.check_insert(seg);
      if (!c) {
        return reloco::unexpected(c.error());
      }
      return cur.commit(*c);
    });
  }

  /**
   * Unpublishes the segment starting at `first`, waits until no reader can hold its descriptors, then calls
   * `on_quiesced(const segment &)`. If the segment does not exist, nothing is changed and `not_found` is
   * returned without calling `on_quiesced`.
   */
  template <typename F> [[nodiscard]] reloco::result<void> remove(pfn_type first, F &&on_quiesced) {
    reloco::optional<segment> removed;
    auto r = domain_.update([&](const Map &cur) -> reloco::result<Map> {
      auto c = cur.check_remove(first);
      if (!c) {
        return reloco::unexpected(c.error());
      }
      for (const segment &s : cur.view().segments()) {
        if (s.first == first) {
          removed = s;
        }
      }
      return cur.commit(*c);
    });
    if (!r) {
      return r;
    }
    on_quiesced(static_cast<const segment &>(*removed));
    return {};
  }

  /** Waits until no reader holds anything older than the current map (after `add`). */
  void synchronize() { domain_.synchronize(); }

private:
  domain_type domain_;
};

} // namespace structo
