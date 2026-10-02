<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `buddy_allocator<FreeList, OsPage, MaxOrder>`

`include/structo/buddy_allocator.hpp`

> Moved from `jplcz_reloco` into `jplcz_structo`'s own `include/structo/`
> and re-homed into the `structo` namespace (the header adds `using
> namespace reloco;` so unmoved reloco types remain reachable unqualified).
> Documented here as extracted from reloco's own reference, with some
> prose still to be reconciled with the new namespace split.

Classic power-of-two buddy allocator over a caller-supplied `OsPage` type
(see [`phys_page.md`](phys_page.md)'s `page_view`/`os_traits_base`) and a
caller-supplied intrusive `FreeList` implementation (see
`free_list_archetype` in the header for the expected shape). `init()`
carves an arbitrary page count into the largest aligned power-of-two
blocks it can; `allocate()`/`allocate_n()`/`allocate_constrained()` split
blocks on demand and `free()`/`free_n()` coalesce them back, including a
`physical_constraint` path for DMA/hardware-mandated low-PFN, alignment,
and boundary-crossing rules, and an opportunistic `allocate_up_to()` for
greedy best-effort allocation. `reserve(page)` "un-frees" one specific,
caller-chosen page that the allocator currently considers free --
splitting its containing block down as needed while returning every
untouched sibling half back to the free lists -- for retroactively
marking a page in-use once it's discovered to be reserved (e.g. a
firmware/ACPI table) only *after* `init()` already assumed the whole
region was free.

See also: [`phys_page.md`](phys_page.md), [`region_set.md`](region_set.md),
[`early_region_allocator.md`](early_region_allocator.md) (the earlier-boot
allocator this one typically takes over from).
