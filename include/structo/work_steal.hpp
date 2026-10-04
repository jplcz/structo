// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file work_steal.hpp
 * @brief Three stateless work-stealing policies --
 * `structo::performance_steal_policy`, `structo::power_save_steal_policy`,
 * `structo::always_steal_policy` -- plus
 * `structo::find_steal_candidate<StealPolicy>`, the one function that
 * uses them to answer a single question: *which* CPU (if any) should an
 * idle/under-loaded CPU steal work from right now?
 *
 * ## This header only ever *decides*; it never touches a runqueue
 *
 * Exactly like every other stateful type in this library (see
 * `load_average.hpp`/`sched.hpp`'s own docs), `find_steal_candidate`
 * performs no locking and no actual cross-CPU dequeue/enqueue itself --
 * it only reads whatever `cpu_sibling_map`/load values the caller hands
 * it and returns, at most, a CPU index to steal from. Reading another
 * CPU's runqueue depth, actually removing a task from it, and migrating
 * it onto the local runqueue are all the caller's job, with whatever
 * synchronization that caller's own runqueue implementation already
 * needs for cross-CPU access (a per-CPU spinlock guarding the remote
 * runqueue, a lock-free MPSC structure, ...) -- this header has no
 * opinion on any of that.
 *
 * ## Topology-ordered, not a flat scan: walks upward, nearest first
 *
 * `find_steal_candidate` walks @p siblings's already-distance-sorted
 * order (see `cpu_sibling_map.hpp`) and returns the *first* eligible
 * candidate @p policy accepts -- never the busiest candidate overall.
 * This is deliberate: a same-core/same-cluster candidate that merely
 * clears the policy's threshold is preferred over a busier
 * cross-package/cross-NUMA-node candidate that would also clear it,
 * because migrating a task across a NUMA boundary is far more expensive
 * (cold caches, remote memory latency for however long the task's
 * working set takes to migrate) than the small extra imbalance left
 * behind by not picking the single busiest CPU in the whole system.
 * "Walk upward through the topology, nearest first, stopping at the
 * first acceptable candidate" is exactly what `cpu_sibling_map`'s own
 * ordering was built to make O(1)-per-step cheap.
 *
 * ## Eligibility: online CPUs only, with caller-driven retry-by-exclusion
 *
 * @p eligible is a `structo::arch::cpu_mask` the caller narrows to
 * whatever CPUs are actually valid steal targets right now -- in
 * practice, that CPU-hotplug "online" mask (e.g.
 * `structo::arch::cpu_online_dispatcher::snapshot()`), since stealing
 * from an offline CPU's stale, possibly-being-torn-down runqueue state
 * is never safe. A caller that picks a candidate but then fails to
 * actually steal from it (the remote runqueue emptied out from under it
 * between the decision and the attempt, a trylock on the remote queue
 * failed, ...) simply clears that one bit from its own copy of
 * @p eligible and calls `find_steal_candidate` again -- the search
 * resumes from the (still topology-ordered) remaining candidates with
 * no other state to carry between attempts:
 *
 * @code
 * auto eligible = online.snapshot(); // structo::arch::cpu_mask<Tag, MaxCpus>
 * for (int attempt = 0; attempt < max_steal_attempts; ++attempt) {
 *   auto victim = structo::find_steal_candidate<structo::performance_steal_policy>(
 *       siblings, this_cpu, eligible, [](std::size_t cpu) { return percpu_load[cpu].current(); });
 *   if (!victim.has_value())
 *     break; // nobody (left) worth stealing from this round
 *   if (try_steal_one_task_from(*victim))
 *     break; // success
 *   eligible.clear(*victim); // that one didn't pan out -- never reconsider it this round
 *   if (eligible.none())
 *     eligible = online.snapshot(); // every candidate was excluded -- refresh in case hotplug changed
 * }
 * @endcode
 *
 * `max_steal_attempts` bounds the loop: refreshing the snapshot after
 * `eligible` empties out is what lets a caller pick back up if hotplug
 * changed the online set mid-search, but without a cap a candidate that
 * fails deterministically (not merely a transient race) would otherwise
 * be retried forever once the mask resets to the same full snapshot.
 *
 * ## Two questions per policy: whether to look, and what to accept
 *
 * Each policy answers two separate questions, both driven purely by
 * `local_load` vs. `candidate_load` with no hidden state:
 *
 * - `should_attempt_steal(local_load)`: is it even worth walking
 *   @p siblings at all? Checked once, up front, by
 *   `find_steal_candidate` itself -- a caller never needs to inline this
 *   condition before deciding whether to call it.
 * - `should_steal(local_load, candidate_load)`: given a specific
 *   candidate reached while walking, is *this one* worth stealing from?
 *
 * ## The three policies
 *
 * - **`performance_steal_policy`**: attempts whenever the local CPU
 *   isn't already comfortably loaded (`local_load <= 1`), and steals as
 *   soon as a candidate has more than one extra runnable task compared
 *   to the local count -- minimizing latency/idle time is worth the
 *   migration cost even for a small imbalance.
 * - **`power_save_steal_policy`**: only ever attempts once the local CPU
 *   is *completely* idle (`local_load == 0`), and then steals from the
 *   first candidate with anything runnable at all -- a
 *   lightly-loaded-but-not-idle CPU never even looks, let alone bothers
 *   a neighbor, and this policy never has any opinion on *waking* a
 *   sleeping/offline CPU to begin with (that's exactly what @p eligible
 *   already excludes); it only decides whether it's worth reaching out
 *   to an already-awake neighbor.
 * - **`always_steal_policy`**: always attempts, regardless of local
 *   load, and steals from the first candidate with anything runnable at
 *   all -- the right policy for a passively-scheduled kernel (e.g. a
 *   TEE/TrustZone OS paired with
 *   `structo::arch::passive_cpu_topology_decoder`, see
 *   `cpu_topology.hpp`) where there is effectively only one shared
 *   runqueue to begin with, so "stealing" is really just "picking up
 *   whatever's there" rather than a true cross-domain migration decision.
 */

#include <cstddef>

#include <reloco/optional.hpp>
#include <structo/arch/cpu_mask.hpp>
#include <structo/arch/cpu_sibling_map.hpp>

namespace structo {

/** @brief Latency-favoring policy: steals as soon as a candidate has more than one extra runnable task. */
struct performance_steal_policy {
  /** @brief Worth searching whenever the local CPU isn't already comfortably loaded -- catches "merely busy, could
   * still use a hand" the same way `should_steal` does, not just the fully-idle case. */
  [[nodiscard]] static constexpr bool should_attempt_steal(std::size_t local_load) noexcept {
    return local_load <= 1;
  }
  [[nodiscard]] static constexpr bool should_steal(std::size_t local_load, std::size_t candidate_load) noexcept {
    return candidate_load > local_load + 1;
  }
};

/** @brief Power-favoring policy: steals only when the local CPU is completely idle. Never wakes a sleeping/offline
 * CPU itself -- see the @file-level docs' `eligible` mask. */
struct power_save_steal_policy {
  /** @brief Only ever worth searching once the local CPU has nothing left to run -- mirrors `should_steal`'s own
   * `local_load == 0` gate, so a lightly-loaded-but-not-idle CPU never even walks the sibling order. */
  [[nodiscard]] static constexpr bool should_attempt_steal(std::size_t local_load) noexcept { return local_load == 0; }
  [[nodiscard]] static constexpr bool should_steal(std::size_t local_load, std::size_t candidate_load) noexcept {
    return local_load == 0 && candidate_load > 0;
  }
};

/** @brief Single-shared-runqueue policy for passively-scheduled kernels (e.g. TEE/TrustZone): steals whenever a
 * candidate has anything runnable, unconditionally. */
struct always_steal_policy {
  /** @brief Always worth searching, regardless of local load -- matches `should_steal`'s own unconditional accept. */
  [[nodiscard]] static constexpr bool should_attempt_steal(std::size_t /*local_load*/) noexcept { return true; }
  [[nodiscard]] static constexpr bool should_steal(std::size_t /*local_load*/, std::size_t candidate_load) noexcept {
    return candidate_load > 0;
  }
};

/**
 * @brief Walks @p siblings's distance-sorted order (nearest first) and
 * returns the first CPU @p policy accepts as worth stealing from, or an
 * empty `optional` if none qualifies. See the @file-level docs above for
 * the full topology-ordering and eligibility-mask rationale.
 *
 * Before looking at any candidate at all, this first asks
 * @p StealPolicy `should_attempt_steal(local_load)` -- the same
 * "is this even worth searching for" gate a caller would otherwise have
 * to duplicate by hand before deciding whether to call this function in
 * the first place (e.g. `power_save_steal_policy` only ever wants to
 * search once @p this_cpu is completely idle). Folding that gate in here
 * means a single policy type fully describes both "when to look"
 * (`should_attempt_steal`) and "what to accept once looking"
 * (`should_steal`), and callers that merely want "idle or not" behavior
 * never need to inline that condition themselves.
 * @tparam StealPolicy One of `performance_steal_policy`/
 * `power_save_steal_policy`/`always_steal_policy` (or any type providing
 * the same `should_attempt_steal(local_load)` and
 * `should_steal(local_load, candidate_load)` static methods).
 * @param siblings   `this_cpu`'s precomputed, closest-first candidate
 * order (see `cpu_sibling_map.hpp`).
 * @param this_cpu   The CPU looking for work.
 * @param eligible   Candidate CPUs currently valid to steal from (e.g.
 * the online mask, with any already-tried-and-failed CPU this round
 * cleared out by the caller -- see the @file-level docs' retry example).
 * @param load       Invocable as `load(std::size_t cpu) -> std::size_t`,
 * returning `cpu`'s current stealable load (e.g.
 * `structo::cpu_load::current()`, or a runqueue's own `size()`).
 * Called for `this_cpu` exactly once -- and only if
 * `should_attempt_steal` accepts that load -- then once per examined
 * candidate.
 */
template <typename StealPolicy, std::size_t MaxCpus, std::size_t MaxLevels, typename LevelId, typename Tag,
          typename LoadFn>
[[nodiscard]] constexpr reloco::optional<std::size_t>
find_steal_candidate(const arch::cpu_sibling_map<MaxCpus, MaxLevels, LevelId> &siblings, std::size_t this_cpu,
                      const arch::cpu_mask<Tag, MaxCpus> &eligible, LoadFn &&load) noexcept {
  const std::size_t local_load = load(this_cpu);
  if (!StealPolicy::should_attempt_steal(local_load))
    return reloco::nullopt;
  const std::size_t count = siblings.sibling_count(this_cpu);
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t candidate = siblings.sibling(this_cpu, i);
    if (!eligible.test(candidate))
      continue;
    const std::size_t candidate_load = load(candidate);
    if (StealPolicy::should_steal(local_load, candidate_load))
      return candidate;
  }
  return reloco::nullopt;
}

} // namespace structo
