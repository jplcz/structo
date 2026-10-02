<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `early_region_allocator<Capacity, PhysInt>`

`include/structo/early_region_allocator.hpp`

A minimal, allocation-free physical-range allocator operating directly
on a caller-owned [`region_set`](region_set.md) -- the smallest
allocator a boot path needs before any page-granularity allocator
([`buddy_allocator`](buddy_allocator.md), a slab, ...) exists.

`region_set` *is* the boot memory map: a sorted, auto-merging set of
free physical ranges, exactly what [`boot_memory_map`](boot_memory_map.md)
exposes as `free`. `early_region_allocator` doesn't own a copy of it or
parse anything itself -- it's constructed directly from an existing
`region_set` by reference and does nothing more than carve ranges out
of it (`try_alloc`/`try_reserve`, both backed by `region_set::try_subtract`)
and heal them back in (`free`, backed by `region_set::try_add`). Because
it mutates the caller's `region_set` in place, the exact same free-region
accounting that fed it can hand straight to `buddy_allocator::init()`
(or be folded in region-by-region) once a real page allocator is ready
to take over -- every early allocation already punched its hole out.

```cpp
auto map = structo::boot_memory_map<32>::try_from_dtb(dtb_blob);
if (!map) { /* handle map.error() */ }

structo::early_region_allocator early(map->free);

// Reserve a region whose address is already known (e.g. the kernel
// image itself, loaded by the bootloader before `free` was computed):
(void)early.try_reserve(kernel_phys_base, kernel_phys_size);

// Carve out scratch memory for early page tables, 4 KiB-aligned:
auto page_table_mem = early.try_alloc(16 * 4096, 4096);
if (!page_table_mem) { /* handle page_table_mem.error() */ }

// Once the real page allocator exists, seed it from what's left:
for (auto &region : map->free) {
  // buddy_allocator::init() each region, or feed region_set-size chunks in
}
```

- `early_region_allocator(region_set<Capacity, PhysInt> &regions)` binds
  the allocator to `regions`, mutated in place by every subsequent call.
  `regions` must outlive the allocator.
- `try_alloc(size, alignment = 1)` best-fit-searches every currently-free
  region for the one that wastes the fewest bytes aligning `size` bytes
  to `alignment` (a power of two), carves it out via `try_subtract`, and
  returns the resulting `memory_region<PhysInt>`. Fails with
  `error::invalid_argument` on zero `size` or a non-power-of-two
  `alignment`, `error::allocation_failed` if nothing fits.
- `try_reserve(base, size)` reserves the exact, caller-known range
  `[base, base + size)` -- e.g. the kernel image, a DTB blob, or a
  firmware reservation discovered only after `free` was first computed.
  Fails with `error::invalid_state` unless that entire range is
  currently free (already reserved, or outside every tracked region);
  `error::invalid_argument` on zero `size`.
- `free(base, size)` returns a range to the free set via
  `region_set::try_add`, merging with adjacent free regions.
- `empty()`, `free_bytes()`, `largest_region()` mirror
  [`boot_memory_map`](boot_memory_map.md)'s own helpers of the same
  name, forwarded straight to the underlying `region_set`.
- `regions()` returns the bound `region_set&`, for handing the exact
  same (now partially-carved) set on to `buddy_allocator::init()`.

See also: [`region_set.md`](region_set.md),
[`boot_memory_map.md`](boot_memory_map.md),
[`buddy_allocator.md`](buddy_allocator.md).
