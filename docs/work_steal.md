<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::find_steal_candidate` / steal policies

`include/structo/work_steal.hpp`

Three stateless work-stealing policies --
[`performance_steal_policy`](#the-three-policies),
[`power_save_steal_policy`](#the-three-policies),
[`always_steal_policy`](#the-three-policies) -- plus
`find_steal_candidate<StealPolicy>`, the one function that uses them to
answer a single question: *which* CPU (if any) should an idle/
under-loaded CPU steal work from right now?

## This header only ever *decides*; it never touches a runqueue

Exactly like every other stateful type in this library (see
[`load_average.md`](load_average.md)/[`sched.md`](sched.md)'s own docs),
`find_steal_candidate` performs no locking and no actual cross-CPU
dequeue/enqueue itself -- it only reads whatever
[`cpu_sibling_map`](cpu_sibling_map.md)/load values the caller hands it
and returns, at most, a CPU index to steal from. Reading another CPU's
runqueue depth, actually removing a task from it, and migrating it onto
the local runqueue are all the caller's job, with whatever
synchronization that caller's own runqueue implementation already needs
for cross-CPU access -- this header has no opinion on any of that.

## Topology-ordered, not a flat scan: walks upward, nearest first

`find_steal_candidate` walks the sibling map's already-distance-sorted
order and returns the *first* eligible candidate the policy accepts --
never the busiest candidate overall. A same-core/same-cluster candidate
that merely clears the policy's threshold is preferred over a busier
cross-package/cross-NUMA-node candidate that would also clear it,
because migrating a task across a NUMA boundary is far more expensive
(cold caches, remote-memory latency) than the small extra imbalance left
behind by not picking the single busiest CPU in the whole system.

## Eligibility: online CPUs only, with caller-driven retry-by-exclusion

`eligible` is a `structo::arch::cpu_mask` the caller narrows to whatever
CPUs are actually valid steal targets right now -- in practice, the
CPU-hotplug "online" mask (e.g. `structo::arch::cpu_online_dispatcher::snapshot()`,
see `include/structo/arch/cpu_online_dispatcher.hpp`), since
stealing from an offline CPU's stale, possibly-being-torn-down runqueue
state is never safe. A caller that picks a candidate but then fails to
actually steal from it (the remote runqueue emptied out from under it
between the decision and the attempt, a trylock on the remote queue
failed, ...) simply clears that one bit from its own copy of `eligible`
and calls `find_steal_candidate` again -- the search resumes from the
(still topology-ordered) remaining candidates with no other state to
carry between attempts:

```cpp
auto eligible = online.snapshot(); // structo::arch::cpu_mask<Tag, MaxCpus>
for (int attempt = 0; attempt < max_steal_attempts; ++attempt) {
  auto victim = structo::find_steal_candidate<structo::performance_steal_policy>(
      siblings, this_cpu, eligible, [](std::size_t cpu) { return percpu_load[cpu].current(); });
  if (!victim.has_value())
    break; // nobody (left) worth stealing from this round
  if (try_steal_one_task_from(*victim))
    break; // success
  eligible.clear(*victim); // that one didn't pan out -- never reconsider it this round
  if (eligible.none())
    eligible = online.snapshot(); // every candidate was excluded -- refresh in case hotplug changed
}
```

`max_steal_attempts` bounds the loop: refreshing the snapshot after
`eligible` empties out is what lets a caller pick back up if hotplug
changed the online set mid-search, but without a cap a candidate that
fails deterministically (not merely a transient race) would otherwise be
retried forever once the mask resets to the same full snapshot.

## Two questions per policy: whether to look, and what to accept

Each policy answers two separate questions, both driven purely by
`local_load` vs. `candidate_load` with no hidden state:

- `should_attempt_steal(local_load)`: is it even worth walking the
  sibling map at all? Checked once, up front, by `find_steal_candidate`
  itself -- a caller never needs to inline this condition before
  deciding whether to call it.
- `should_steal(local_load, candidate_load)`: given a specific candidate
  reached while walking, is *this one* worth stealing from?

## The three policies

- **`performance_steal_policy`**: attempts whenever the local CPU isn't
  already comfortably loaded (`local_load <= 1`), and steals as soon as
  a candidate has more than one extra runnable task compared to the
  local count -- minimizing latency/idle time is worth the migration
  cost even for a small imbalance.
- **`power_save_steal_policy`**: only ever attempts once the local CPU
  is *completely* idle (`local_load == 0`), and then steals from the
  first candidate with anything runnable at all -- a
  lightly-loaded-but-not-idle CPU never even looks, let alone bothers a
  neighbor, and this policy never has any opinion on *waking* a
  sleeping/offline CPU to begin with (that's exactly what `eligible`
  already excludes); it only decides whether it's worth reaching out to
  an already-awake neighbor.
- **`always_steal_policy`**: always attempts, regardless of local load,
  and steals from the first candidate with anything runnable at all --
  the right policy for a passively-scheduled kernel (e.g. a
  TEE/TrustZone OS paired with
  [`passive_cpu_topology_decoder`](cpu_topology.md)) where there is
  effectively only one shared runqueue to begin with, so "stealing" is
  really just "picking up whatever's there" rather than a true
  cross-domain migration decision.

See also: [`cpu_sibling_map.md`](cpu_sibling_map.md), [`cpu_load.md`](cpu_load.md), [`cpu_topology.md`](cpu_topology.md), [`sched.md`](sched.md), [`cpu_mask.md`](cpu_mask.md).
