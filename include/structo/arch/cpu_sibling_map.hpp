// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file cpu_sibling_map.hpp
 * @brief `structo::arch::cpu_sibling_map<MaxCpus, MaxLevels, LevelId>`: a
 * precomputed, closest-first ordering of every other CPU, derived once
 * from a `cpu_topology<MaxCpus, MaxLevels, LevelId>` (see
 * `cpu_topology.hpp`) -- the lookup table a work-stealing/load-balancing
 * search walks to decide *which order* to probe other CPUs in, without
 * re-deriving `shares_level`/`lowest_shared_level` comparisons on every
 * single steal attempt.
 *
 * ## Why precompute this instead of calling `lowest_shared_level` at steal time
 *
 * `cpu_topology::lowest_shared_level(a, b)` is already O(`level_count()`)
 * per pair, which is cheap for a *single* comparison -- but a
 * work-stealing search that wants "walk upward through the topology,
 * nearest CPUs first" (same core, then cluster, then package/NUMA node,
 * ...) needs the *entire* candidate set sorted by distance from the
 * stealing CPU, every time it looks for work. Doing that sort on every
 * steal attempt (likely to be the scheduler's hottest cold-queue path)
 * would repeat the same O(`cpu_count` * `level_count()`) work on every
 * single call for a result that only actually changes when the topology
 * itself changes (CPU hotplug, or never at all on most targets).
 * `cpu_sibling_map::build()` does that sort exactly once (boot time, or
 * after a hotplug event) and `sibling(cpu, index)` thereafter is a
 * single array load -- the same "pay the cost once, read it forever"
 * tradeoff `fdt_cpu_topology_decoder` itself already makes relative to
 * calling `lowest_shared_level` ad hoc (see that header's own docs for
 * why deriving topology itself is not considered early-boot-critical).
 *
 * ## Storage cost: `O(MaxCpus^2)`, by design
 *
 * Every CPU gets its own full ordering of every *other* CPU, so this
 * table is quadratic in `MaxCpus` (`MaxCpus * (MaxCpus - 1)`
 * `std::size_t` sibling slots, plus one count per CPU) -- unlike
 * `cpu_topology`'s own `O(MaxCpus * MaxLevels)` storage. For any `MaxCpus`
 * a real SMP/NUMA system would actually have (tens to a few hundred),
 * this is a small, boot-time-allocated (not dynamically allocated --
 * this is still a fixed-size, caller-owned value type) table; pick a
 * narrower `MaxCpus` (or simply don't build this table, and fall back to
 * `cpu_topology::lowest_shared_level` directly) if `MaxCpus` would make
 * the quadratic cost actually matter for a given target.
 *
 * ## Distance ordering: nearest first, undefined-topology CPUs last
 *
 * For a given `cpu`, `build()` orders every other CPU `b` in
 * `[0, cpu_count)` ascending by `topo.lowest_shared_level(cpu, b)` --
 * level 0 (SMT siblings on the same core) sorts before level 1 (same
 * cluster), before level 2 (same package/socket), and so on; a `b` that
 * shares *no* populated level with `cpu` (e.g. a different, disjoint
 * NUMA node/tree, or simply an unpopulated table) sorts after every CPU
 * that does. Ties (two CPUs equally "close") are broken by ascending CPU
 * index, purely for deterministic, reproducible output -- real hardware
 * never actually ties within one physical topology branch. Walking
 * `sibling(cpu, 0)`, `sibling(cpu, 1)`, ... in order is therefore exactly
 * "walk upward through the topology, nearest first", which is what a
 * NUMA-aware work-steal search wants: exhaust same-core/same-cluster
 * candidates before ever considering a cross-node migration.
 *
 * @code
 * structo::arch::cpu_topology<8, 4> topo;
 * // ... populate via flat_cpu_topology_decoder / fdt_cpu_topology_decoder / passive_cpu_topology_decoder ...
 *
 * structo::arch::cpu_sibling_map<8, 4> siblings;
 * siblings.build(topo, 8); // once, at boot (or again after a hotplug event)
 *
 * // Work-stealing search from CPU 2, nearest candidates first:
 * for (std::size_t i = 0; i < siblings.sibling_count(2); ++i) {
 *   std::size_t candidate = siblings.sibling(2, i);
 *   if (looks_worth_stealing_from(candidate)) {
 *     steal_from(candidate);
 *     break;
 *   }
 * }
 * @endcode
 */

#include <cstddef>

#include <reloco/detail/assert.hpp>
#include <reloco/error.hpp>
#include <structo/arch/cpu_topology.hpp>

namespace structo::arch {

/**
 * @brief Precomputed, closest-first ordering of every other CPU, built
 * once from a `cpu_topology<MaxCpus, MaxLevels, LevelId>`. See the
 * @file-level docs above for the distance ordering and storage-cost
 * rationale.
 * @tparam MaxCpus   Maximum logical CPU count this map can describe;
 * must match the `cpu_topology` it is built from.
 * @tparam MaxLevels Maximum number of hierarchy levels, matching the
 * `cpu_topology` it is built from; defaults to 4.
 * @tparam LevelId   `cpu_topology`'s own level-id type; defaults to
 * `uint16_t`. Only used to name the exact `cpu_topology` type `build()`
 * accepts -- never stored here.
 */
template <std::size_t MaxCpus, std::size_t MaxLevels = 4, typename LevelId = uint16_t> class cpu_sibling_map {
  static_assert(MaxCpus >= 1, "cpu_sibling_map requires at least one CPU");

public:
  static inline constexpr std::size_t max_cpus = MaxCpus;

  constexpr cpu_sibling_map() noexcept { clear(); }

  /** @brief Resets every CPU's sibling ordering to empty. */
  constexpr void clear() noexcept {
    for (std::size_t cpu = 0; cpu < MaxCpus; ++cpu) {
      count_[cpu] = 0;
      for (std::size_t slot = 0; slot < MaxCpus; ++slot) {
        order_[cpu][slot] = 0;
      }
    }
  }

  /**
   * @brief Builds, for every CPU in `[0, cpu_count)`, the closest-first
   * ordering of every *other* CPU in `[0, cpu_count)`, derived from
   * @p topo (see the @file-level docs for the exact ordering rule).
   * `cpu_count` is clamped to `MaxCpus` if larger. O(`cpu_count^2`) --
   * call once at boot (or again after a hotplug event changes @p topo),
   * never on a steal-attempt hot path.
   */
  constexpr void build(const cpu_topology<MaxCpus, MaxLevels, LevelId> &topo, std::size_t cpu_count) noexcept {
    const std::size_t n = (cpu_count < MaxCpus) ? cpu_count : MaxCpus;
    clear();
    for (std::size_t cpu = 0; cpu < n; ++cpu) {
      std::size_t k = 0;
      for (std::size_t other = 0; other < n; ++other) {
        if (other == cpu)
          continue;
        order_[cpu][k] = other;
        ++k;
      }
      count_[cpu] = k;
      insertion_sort_by_distance(topo, cpu, k);
    }
  }

  /** @brief Number of other CPUs recorded for `cpu` (i.e. `cpu_count - 1` as of the last `build()`). */
  [[nodiscard]] constexpr std::size_t sibling_count(std::size_t cpu) const noexcept {
    RELOCO_ASSERT(cpu < MaxCpus, "cpu_sibling_map: cpu index out of range");
    return count_[cpu];
  }

  /**
   * @brief `cpu`'s `index`-th closest other CPU (`index == 0` is the
   * single closest). Traps (`RELOCO_ASSERT`) if `cpu >= MaxCpus` or
   * `index >= sibling_count(cpu)`; use `try_sibling` to handle an
   * out-of-range index as data instead.
   */
  [[nodiscard]] constexpr std::size_t sibling(std::size_t cpu, std::size_t index) const noexcept {
    RELOCO_ASSERT(cpu < MaxCpus, "cpu_sibling_map: cpu index out of range");
    RELOCO_ASSERT(index < count_[cpu], "cpu_sibling_map: sibling index out of range");
    return order_[cpu][index];
  }

  /** @brief Fallible variant of `sibling`. */
  [[nodiscard]] RELOCO_CONSTEXPR20 reloco::result<std::size_t> try_sibling(std::size_t cpu,
                                                                           std::size_t index) const noexcept {
    if (cpu >= MaxCpus || index >= count_[cpu])
      return reloco::unexpected(reloco::error::out_of_range);
    return order_[cpu][index];
  }

private:
  // A CPU with no level shared with `cpu` at all (disjoint subtree, or an
  // unpopulated table) is given a distance one past the last real level,
  // so it always sorts after every CPU that *does* share some level.
  [[nodiscard]] static RELOCO_CONSTEXPR20 std::size_t distance_of(const cpu_topology<MaxCpus, MaxLevels, LevelId> &topo,
                                                                  std::size_t cpu, std::size_t other) noexcept {
    reloco::optional<std::size_t> level = topo.lowest_shared_level(cpu, other);
    return level.has_value() ? *level : topo.level_count();
  }

  // Plain insertion sort: `k` is bounded by MaxCpus (expected small --
  // tens to a few hundred), and this only ever runs from build(), never
  // a hot path, so O(k^2) here is the right tradeoff against pulling in
  // <algorithm> for a one-shot, boot-time sort.
  constexpr void insertion_sort_by_distance(const cpu_topology<MaxCpus, MaxLevels, LevelId> &topo, std::size_t cpu,
                                            std::size_t k) noexcept {
    for (std::size_t i = 1; i < k; ++i) {
      const std::size_t candidate = order_[cpu][i];
      const std::size_t candidate_distance = distance_of(topo, cpu, candidate);
      std::size_t j = i;
      while (j > 0 && is_after(topo, cpu, order_[cpu][j - 1], candidate, candidate_distance)) {
        order_[cpu][j] = order_[cpu][j - 1];
        --j;
      }
      order_[cpu][j] = candidate;
    }
  }

  // Whether `existing` (already placed) must sort strictly after
  // `candidate` -- i.e. whether `candidate` needs to move earlier than
  // `existing`'s current slot.
  [[nodiscard]] static constexpr bool is_after(const cpu_topology<MaxCpus, MaxLevels, LevelId> &topo, std::size_t cpu,
                                               std::size_t existing, std::size_t candidate,
                                               std::size_t candidate_distance) noexcept {
    const std::size_t existing_distance = distance_of(topo, cpu, existing);
    if (existing_distance != candidate_distance)
      return existing_distance > candidate_distance;
    return existing > candidate; // deterministic tie-break: ascending CPU index
  }

  std::size_t order_[MaxCpus][MaxCpus]{};
  std::size_t count_[MaxCpus]{};
};

} // namespace structo::arch
