<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `early_region_allocator<Capacity, PhysInt>`

`include/structo/early_region_allocator.hpp`

A minimal, allocation-free physical-range allocator seeded from a
caller-owned [`region_set`](region_set.md) -- the smallest allocator a
boot path needs before any page-granularity allocator
([`buddy_allocator`](buddy_allocator.md), a slab, ...) exists.

`region_set` *is* the boot memory map: a sorted, auto-merging set of
free physical ranges, exactly what [`boot_memory_map`](boot_memory_map.md)
exposes as `free`. That boot memory map is typically the *full*
authoritative record of installed/reserved RAM for the whole boot --
other, unrelated code may still read it later, so `early_region_allocator`
must not mutate it. Instead, `try_create()` takes an immutable snapshot
of the seed `region_set` into its own, separate internal `region_set` at
construction time; every subsequent `try_alloc`/`try_reserve`/`free` call
only ever mutates that private copy. The seed `region_set` is left
completely untouched and can keep being read (or handed to something
else) for as long as the caller likes.

```cpp
auto map = structo::boot_memory_map<32>::try_from_dtb(dtb_blob);
if (!map) { /* handle map.error() */ }

auto early = structo::early_region_allocator<32>::try_create(map->free);
if (!early) { /* handle early.error() */ }
// `map->free` itself is untouched by everything below.

// Reserve a region whose address is already known (e.g. the kernel
// image itself, loaded by the bootloader before `free` was computed):
(void)early->try_reserve(kernel_phys_base, kernel_phys_size);

// Carve out scratch memory for early page tables, 4 KiB-aligned:
auto page_table_mem = early->try_alloc(16 * 4096, 4096);
if (!page_table_mem) { /* handle page_table_mem.error() */ }

// Once the real page allocator exists, seed it from what's left:
for (auto &region : early->regions()) {
  // buddy_allocator::init() each region, or feed region_set-size chunks in
}
```

- `try_create(const region_set<Capacity, PhysInt> &seed)` copies every
  region in `seed` into a fresh allocator's own internal `region_set`
  and returns it. `seed` is only read, never mutated -- by this call or
  by anything the returned allocator does afterwards. Fails with
  `error::capacity_exceeded` only if `seed`'s own merging invariant was
  somehow violated (both sets share `Capacity`, so this shouldn't
  happen through normal `region_set` use).
- `try_alloc(size, alignment = 1)` best-fit-searches every currently-free
  region in the allocator's own set for the one that wastes the fewest
  bytes aligning `size` bytes to `alignment` (a power of two), carves it
  out via `try_subtract`, and returns the resulting `memory_region<PhysInt>`.
  Fails with `error::invalid_argument` on zero `size` or a
  non-power-of-two `alignment`, `error::allocation_failed` if nothing fits.
- `try_reserve(base, size)` reserves the exact, caller-known range
  `[base, base + size)` -- e.g. the kernel image, a DTB blob, or a
  firmware reservation discovered only after `free` was first computed.
  Fails with `error::invalid_state` unless that entire range is
  currently free (already reserved, or outside every tracked region);
  `error::invalid_argument` on zero `size`.
- `free(base, size)` returns a range to the allocator's own free set via
  `region_set::try_add`, merging with adjacent free regions.
- `empty()`, `free_bytes()`, `largest_region()` mirror
  [`boot_memory_map`](boot_memory_map.md)'s own helpers of the same
  name, forwarded straight to the internal `region_set`.
- `regions()` returns the allocator's own, private `region_set&` (not
  the seed it was built from), for handing the now-partially-carved set
  on to `buddy_allocator::init()`.

See also: [`region_set.md`](region_set.md),
[`boot_memory_map.md`](boot_memory_map.md),
[`buddy_allocator.md`](buddy_allocator.md).
