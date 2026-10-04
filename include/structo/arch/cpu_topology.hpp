// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file cpu_topology.hpp
 * @brief `structo::arch::cpu_topology<MaxCpus, MaxLevels, LevelId>`: a
 * caller-owned, allocation-free table recording each logical CPU's
 * position in a hierarchical scheduling/affinity topology (thread ->
 * core -> cluster -> package/socket, or however many levels a platform
 * actually has) -- purely a scheduler-topology fact table, unrelated to
 * `cpu_mask.hpp`/IPI routing.
 *
 * Levels are indexed finest-first: level 0 is the tightest-sharing group
 * (e.g. SMT siblings on the same core), increasing level indices walk
 * outward to coarser groups (core -> cluster -> package). Every CPU is
 * recorded with one `LevelId` per level in `[0, level_count())`; two
 * CPUs share a given level iff their recorded id at that level compares
 * equal -- whatever populates the table (see e.g.
 * `fdt_cpu_topology_decoder` in `fdt_cpu_topology.hpp`, or the bundled
 * `flat_cpu_topology_decoder`/`passive_cpu_topology_decoder` fallbacks
 * below) must assign level ids so this holds: an id must never be
 * reused across two different branches of the hierarchy at the same
 * level.
 *
 * `cpu_topology` itself is a plain data table -- it has no trait/policy
 * parameter and makes no attempt to derive ids on its own. Populating it
 * is a separate, explicit "decoder" step so the table's storage shape
 * stays decoupled from any particular source of topology information:
 *
 * @code
 * structo::arch::cpu_topology<8, 4> topo;
 * // ... populate via structo::arch::flat_cpu_topology_decoder::decode(topo, 4)
 * // or structo::arch::fdt_cpu_topology_decoder::decode(index, topo) ...
 * if (topo.shares_level(0, 1, 0)) {
 *   // logical CPUs 0 and 1 are SMT siblings on the same core
 * }
 * @endcode
 */

#include <cstddef>
#include <cstdint>

#include <reloco/detail/assert.hpp>
#include <reloco/error.hpp>
#include <reloco/optional.hpp>

namespace structo::arch {

/**
 * @brief Fixed-capacity table of per-CPU, per-level affinity group ids.
 * @tparam MaxCpus   Maximum logical CPU count this table can describe.
 * @tparam MaxLevels Maximum number of hierarchy levels this table can
 * describe (e.g. 4 for thread/core/cluster/package); defaults to 4.
 * @tparam LevelId   Integer type wide enough to uniquely identify every
 * distinct group at any single level across the whole table; defaults
 * to `uint16_t`.
 */
template <std::size_t MaxCpus, std::size_t MaxLevels = 4, typename LevelId = uint16_t> class cpu_topology {
  static_assert(MaxCpus >= 1, "cpu_topology requires at least one CPU");
  static_assert(MaxLevels >= 1, "cpu_topology requires at least one level");

public:
  using level_id_type = LevelId;

  static inline constexpr std::size_t max_cpus = MaxCpus;
  static inline constexpr std::size_t max_levels = MaxLevels;

  constexpr cpu_topology() noexcept { clear(); }

  /** @brief Resets every CPU's level ids to `LevelId{}` and `level_count()` to 0. */
  constexpr void clear() noexcept {
    for (std::size_t cpu = 0; cpu < max_cpus; ++cpu) {
      for (std::size_t level = 0; level < max_levels; ++level) {
        m_ids[cpu][level] = LevelId{};
      }
    }
    m_level_count = 0;
  }

  /** @brief Number of levels currently populated, `[0, max_levels]`. */
  [[nodiscard]] constexpr std::size_t level_count() const noexcept { return m_level_count; }

  /**
   * @brief Sets `level_count()`.
   * Traps (`RELOCO_ASSERT`) if `count > max_levels`; use
   * `try_set_level_count` to handle an out-of-range count as data instead.
   */
  constexpr void set_level_count(std::size_t count) noexcept {
    RELOCO_ASSERT(count <= max_levels, "cpu_topology: level count exceeds max_levels");
    m_level_count = count;
  }

  /** @brief Fallible variant of `set_level_count`. */
  RELOCO_CONSTEXPR20 reloco::result<void> try_set_level_count(std::size_t count) noexcept {
    if (count > max_levels)
      return reloco::unexpected(reloco::error::out_of_range);
    m_level_count = count;
    return {};
  }

  /**
   * @brief Records `cpu`'s group id at `level`.
   * Traps (`RELOCO_ASSERT`) if `cpu >= max_cpus` or `level >= max_levels`;
   * use `try_set_level_id` to handle an out-of-range index as data instead.
   */
  constexpr void set_level_id(std::size_t cpu, std::size_t level, LevelId id) noexcept {
    RELOCO_ASSERT(cpu < max_cpus, "cpu_topology: cpu index out of range");
    RELOCO_ASSERT(level < max_levels, "cpu_topology: level index out of range");
    m_ids[cpu][level] = id;
  }

  /** @brief Fallible variant of `set_level_id`. */
  RELOCO_CONSTEXPR20 reloco::result<void> try_set_level_id(std::size_t cpu, std::size_t level, LevelId id) noexcept {
    if (cpu >= max_cpus || level >= max_levels)
      return reloco::unexpected(reloco::error::out_of_range);
    m_ids[cpu][level] = id;
    return {};
  }

  /**
   * @brief `cpu`'s recorded group id at `level`.
   * Traps (`RELOCO_ASSERT`) if `cpu >= max_cpus` or `level >= max_levels`;
   * use `try_level_id` to handle an out-of-range index as data instead.
   */
  [[nodiscard]] constexpr LevelId level_id(std::size_t cpu, std::size_t level) const noexcept {
    RELOCO_ASSERT(cpu < max_cpus, "cpu_topology: cpu index out of range");
    RELOCO_ASSERT(level < max_levels, "cpu_topology: level index out of range");
    return m_ids[cpu][level];
  }

  /** @brief Fallible variant of `level_id`. */
  [[nodiscard]] constexpr reloco::result<LevelId> try_level_id(std::size_t cpu, std::size_t level) const noexcept {
    if (cpu >= max_cpus || level >= max_levels)
      return reloco::unexpected(reloco::error::out_of_range);
    return m_ids[cpu][level];
  }

  /**
   * @brief Whether `cpu_a` and `cpu_b` share the same group at `level`
   * (e.g. `level == 0` -> "are SMT siblings on the same core").
   * Traps (`RELOCO_ASSERT`) if either cpu index is out of range or
   * `level >= level_count()`.
   */
  [[nodiscard]] constexpr bool shares_level(std::size_t cpu_a, std::size_t cpu_b, std::size_t level) const noexcept {
    RELOCO_ASSERT(cpu_a < max_cpus && cpu_b < max_cpus, "cpu_topology: cpu index out of range");
    RELOCO_ASSERT(level < m_level_count, "cpu_topology: level index out of range");
    return m_ids[cpu_a][level] == m_ids[cpu_b][level];
  }

  /**
   * @brief The finest (lowest-index) level `cpu_a` and `cpu_b` share, or
   * an empty `optional` if they share none of the populated levels (e.g.
   * distinct, entirely unrelated CPUs, or an empty/not-yet-populated
   * table).
   * Traps (`RELOCO_ASSERT`) if either cpu index is out of range.
   */
  [[nodiscard]] constexpr reloco::optional<std::size_t> lowest_shared_level(std::size_t cpu_a,
                                                                            std::size_t cpu_b) const noexcept {
    RELOCO_ASSERT(cpu_a < max_cpus && cpu_b < max_cpus, "cpu_topology: cpu index out of range");
    for (std::size_t level = 0; level < m_level_count; ++level) {
      if (m_ids[cpu_a][level] == m_ids[cpu_b][level])
        return level;
    }
    return reloco::nullopt;
  }

private:
  LevelId m_ids[max_cpus][max_levels]{};
  std::size_t m_level_count{0};
};

/**
 * @brief Fallback topology decoder for when no real hierarchy is known:
 * populates a single level (level 0) whose id is simply each CPU's own
 * logical index, so no two distinct CPUs ever compare as sharing
 * anything -- the safe, maximally-pessimistic default a scheduler can
 * fall back to on hardware nothing more specific has been wired up for.
 */
struct flat_cpu_topology_decoder {
  /**
   * @brief Populates `topo` with `cpu_count` singleton groups.
   * @param topo      Table to populate; any prior contents are overwritten.
   * @param cpu_count Number of logical CPUs to record, clamped to
   * `topo.max_cpus` if larger.
   * @return The number of CPUs actually recorded.
   */
  template <std::size_t MaxCpus, std::size_t MaxLevels, typename LevelId>
  static constexpr std::size_t decode(cpu_topology<MaxCpus, MaxLevels, LevelId> &topo,
                                      std::size_t cpu_count) noexcept {
    const std::size_t count = (cpu_count < MaxCpus) ? cpu_count : MaxCpus;
    topo.clear();
    topo.set_level_count(1);
    for (std::size_t cpu = 0; cpu < count; ++cpu) {
      topo.set_level_id(cpu, 0, static_cast<LevelId>(cpu));
    }
    return count;
  }
};

/**
 * @brief Fallback topology decoder for passively-scheduled
 * environments -- a TEE/TrustZone secure-world OS (or any kernel that
 * is itself only ever entered on demand by another world via an SMC/
 * hypercall, rather than independently scheduling work across its own
 * cores) has no independent per-core scheduling domain to speak of: the
 * normal world/secure monitor decides which core runs secure-world code
 * and when, so every logical CPU this side ever observes belongs to the
 * same single group -- a scheduler built on top of this `cpu_topology`
 * sees one shared runqueue rather than `cpu_count` independent ones.
 * The exact opposite grouping of `flat_cpu_topology_decoder` (which
 * makes every CPU its own singleton group); use this one instead of
 * that whenever "treat the whole system as one scheduling domain" is
 * the correct passive-side default.
 */
struct passive_cpu_topology_decoder {
  /**
   * @brief Populates `topo` with one level (level 0) whose id is the
   * same for every recorded CPU, so `shares_level(a, b, 0)` is `true`
   * for any `a`/`b` pair.
   * @param topo      Table to populate; any prior contents are overwritten.
   * @param cpu_count Number of logical CPUs to record, clamped to
   * `topo.max_cpus` if larger.
   * @return The number of CPUs actually recorded.
   */
  template <std::size_t MaxCpus, std::size_t MaxLevels, typename LevelId>
  static constexpr std::size_t decode(cpu_topology<MaxCpus, MaxLevels, LevelId> &topo,
                                      std::size_t cpu_count) noexcept {
    const std::size_t count = (cpu_count < MaxCpus) ? cpu_count : MaxCpus;
    topo.clear();
    topo.set_level_count(1);
    for (std::size_t cpu = 0; cpu < count; ++cpu) {
      topo.set_level_id(cpu, 0, LevelId{0});
    }
    return count;
  }
};

} // namespace structo::arch
