<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::arch::cpu_sibling_map`

`include/structo/arch/cpu_sibling_map.hpp`

A precomputed, closest-first ordering of every other CPU, derived once
from a [`cpu_topology<MaxCpus, MaxLevels, LevelId>`](cpu_topology.md) --
the lookup table a work-stealing/load-balancing search walks to decide
*which order* to probe other CPUs in, without re-deriving
`shares_level`/`lowest_shared_level` comparisons on every single steal
attempt.

## Why precompute this instead of calling `lowest_shared_level` at steal time

`cpu_topology::lowest_shared_level(a, b)` is already O(`level_count()`)
per pair -- cheap for a *single* comparison, but a work-stealing search
that wants "walk upward through the topology, nearest CPUs first" (same
core, then cluster, then package/NUMA node, ...) needs the *entire*
candidate set sorted by distance from the stealing CPU, every time it
looks for work. Doing that sort on every steal attempt (likely the
scheduler's hottest cold-queue path) would repeat the same
O(`cpu_count` * `level_count()`) work for a result that only actually
changes when the topology itself changes (CPU hotplug, or never at all
on most targets). `build()` does that sort exactly once (boot time, or
after a hotplug event) and `sibling(cpu, index)` thereafter is a single
array load.

## Storage cost: `O(MaxCpus^2)`, by design

Every CPU gets its own full ordering of every *other* CPU, so this table
is quadratic in `MaxCpus` (`MaxCpus * MaxCpus` `std::size_t` slots, plus
one count per CPU) -- unlike `cpu_topology`'s own `O(MaxCpus * MaxLevels)`
storage. For any CPU count a real SMP/NUMA system would actually have
(tens to a few hundred cores) this is a trivially small, fixed-size
(never dynamically allocated) table.

## Distance ordering: nearest first, undefined-topology CPUs last

For a given `cpu`, `build()` orders every other CPU `b` in
`[0, cpu_count)` ascending by `topo.lowest_shared_level(cpu, b)` -- level
0 (SMT siblings on the same core) sorts before level 1 (same cluster),
before level 2 (same package/socket), and so on; a `b` that shares *no*
populated level with `cpu` (a disjoint NUMA node/tree, or an unpopulated
table) sorts after every CPU that does. Ties are broken by ascending CPU
index purely for deterministic output. Walking `sibling(cpu, 0)`,
`sibling(cpu, 1)`, ... in order is therefore exactly "walk upward through
the topology, nearest first" -- exhaust same-core/same-cluster candidates
before ever considering a cross-node migration.

```cpp
structo::arch::cpu_topology<8, 4> topo;
// ... populate via flat_cpu_topology_decoder / fdt_cpu_topology_decoder / passive_cpu_topology_decoder ...

structo::arch::cpu_sibling_map<8, 4> siblings;
siblings.build(topo, 8); // once, at boot (or again after a hotplug event)

// Work-stealing search from CPU 2, nearest candidates first:
for (std::size_t i = 0; i < siblings.sibling_count(2); ++i) {
  std::size_t candidate = siblings.sibling(2, i);
  if (looks_worth_stealing_from(candidate)) {
    steal_from(candidate);
    break;
  }
}
```

See also: [`cpu_topology.md`](cpu_topology.md), [`fdt_cpu_topology.md`](fdt_cpu_topology.md), [`cpu_load.md`](cpu_load.md).
