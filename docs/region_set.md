<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `memory_region<PhysInt>` / `region_set<Capacity, PhysInt>`

`include/structo/region_set.hpp`

> Moved from `jplcz_reloco` into `jplcz_structo`'s own `include/structo/`
> and re-homed into the `structo` namespace (the header adds `using
> namespace reloco;` so unmoved reloco types remain reachable unqualified).
> Documented here as extracted from reloco's own reference, with some
> prose still to be reconciled with the new namespace split.

`memory_region` represents a single physical address range. `region_set`
is a fixed-capacity collection of merged, non-overlapping `memory_region`s,
used by [`boot_memory_map`](reference.md) and
[`try_extract_memory`](fdt_memory.md) to describe installed/available RAM.

See also: [`fdt_memory.md`](fdt_memory.md), [`phys_page.md`](phys_page.md).
