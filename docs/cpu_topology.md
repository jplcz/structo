<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `cpu_topology<MaxCpus, MaxLevels, LevelId>`

`include/structo/arch/cpu_topology.hpp`

A caller-owned, allocation-free table recording each logical CPU's
position in a hierarchical scheduling/affinity topology (thread -> core
-> cluster -> package/socket, or however many levels a platform actually
has) -- purely a scheduler-topology fact table, unrelated to
`cpu_mask.hpp`/IPI routing.

Levels are indexed finest-first: level 0 is the tightest-sharing group
(e.g. SMT siblings on the same core), increasing level indices walk
outward to coarser groups (core -> cluster -> package). Every CPU is
recorded with one `LevelId` per level in `[0, level_count())`; two CPUs
share a given level iff their recorded id at that level compares equal
-- whatever populates the table must assign ids so this holds (never
reuse an id across two different branches of the hierarchy at the same
level).

`cpu_topology` itself is a plain data table: it has no trait/policy
parameter and makes no attempt to derive ids on its own. Populating it
is a separate, explicit "decoder" step so the table's storage shape
stays decoupled from any particular source of topology information --
see [`fdt_cpu_topology.md`](fdt_cpu_topology.md)'s
`fdt_cpu_topology_decoder` for deriving a table from a devicetree's
`/cpus/cpu-map` node, or the bundled `flat_cpu_topology_decoder` below
for when no real topology is known.

```cpp
structo::arch::cpu_topology<8, 4> topo;
structo::arch::flat_cpu_topology_decoder::decode(topo, 4);
if (topo.shares_level(0, 1, 0)) {
  // logical CPUs 0 and 1 are in the same finest-level group
}
```

## API

- `cpu_topology()` -- default-constructs with `level_count() == 0` and
  every CPU's level ids zeroed.
- `clear()` -- resets to the default-constructed state.
- `level_count()` / `set_level_count(count)` / `try_set_level_count(count)`
  -- the number of populated levels, `[0, max_levels]`. `set_level_count`
  traps (`RELOCO_ASSERT`) if `count > max_levels`; `try_set_level_count`
  reports `error::out_of_range` instead.
- `level_id(cpu, level)` / `set_level_id(cpu, level, id)` and their
  `try_`-prefixed fallible counterparts -- per-CPU, per-level group id
  accessors. The checked tier traps on an out-of-range `cpu`/`level`;
  the `try_` tier reports `error::out_of_range`.
- `shares_level(cpu_a, cpu_b, level)` -- whether `cpu_a` and `cpu_b`
  compare equal at `level` (e.g. `level == 0` -> "are SMT siblings").
  Traps if either cpu index is out of range or `level >= level_count()`.
- `lowest_shared_level(cpu_a, cpu_b)` -- the finest (lowest-index) level
  the two CPUs share, or an empty `reloco::optional` if they share none
  of the populated levels.

## `flat_cpu_topology_decoder`: the pessimistic fallback

When no real topology information is available (no devicetree
`cpu-map`, an architecture with no equivalent concept, or simply before
one has been wired up), `flat_cpu_topology_decoder::decode(topo,
cpu_count)` populates a single level (level 0) whose id is each CPU's
own logical index -- so no two distinct CPUs ever compare as sharing
anything. This is the safe default a scheduler can fall back to: it
never claims a false affinity relationship, it just can't express a true
one either.

## `passive_cpu_topology_decoder`: the single-runqueue fallback

A TEE/TrustZone secure-world OS (or any kernel only ever entered on
demand by another world via an SMC/hypercall, never independently
scheduling work across its own cores) has no real per-core scheduling
domain: the normal world/secure monitor decides which core runs
secure-world code and when. `passive_cpu_topology_decoder::decode(topo,
cpu_count)` populates a single level whose id is the *same* for every
CPU, so `shares_level(a, b, 0)` is `true` for every pair -- a scheduler
built on top sees one shared runqueue rather than `cpu_count`
independent ones. This is the exact opposite grouping of
`flat_cpu_topology_decoder`; use it instead whenever "treat the whole
system as one scheduling domain" is the correct passive-side default.

See also: [`fdt_cpu_topology.md`](fdt_cpu_topology.md), [`cpu_index.md`](cpu_index.md), [`cpu_mask.md`](cpu_mask.md).
