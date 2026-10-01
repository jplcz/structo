<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::arch::page_table_occupancy<CounterInt>`

`include/structo/arch/page_table_occupancy.hpp`

An O(1) live-entry counter for one page-table level's table, so an unmap
walk can tell "is this table completely empty now, free it" without
rescanning the table.

## Why this exists

A page-table tree that only ever grows during `mmap`/fault-in and never
shrinks back during `munmap` wastes memory: every child table that once
held a mapping stays allocated forever, even once every entry it held has
since been unmapped. Releasing those tables back to the allocator
requires answering, after every unmap that touches a table, "does this
table have *any* live entry left?" -- and answering that by rescanning
the table (e.g. looping `page_table_level_range`'s `step::entry().
is_null()` across all `entry_count` entries) costs O(entry_count) **per
table, per unmap call**. For a 512-entry level that is 512 loads and
branches to recover a single bit of information, repeated at every level
an unmap cascades through, on every unmap.

Real kernels avoid this by keeping a small live-entry counter *with* the
table instead -- Linux's per-`pgtable_t` use count, FreeBSD's
page-table-page wire count -- incremented/decremented exactly once per
entry transition (null &harr; non-null) as it happens. Checking emptiness
then costs one integer read. `page_table_occupancy<CounterInt>` is that
counter.

## This is bookkeeping only -- the caller drives every transition

Consistent with every other `structo::arch` header, this type does
**not** observe entry writes, walk tables, or free memory itself. It is
a plain counter the caller:

- increments exactly once, via `increment()`, at every null &rarr;
  non-null entry transition (a new leaf mapping installed, or a new
  child table linked in);
- decrements exactly once, via `decrement()`, at every non-null &rarr;
  null entry transition (a leaf mapping removed, or a now-empty child
  table unlinked).

`decrement()` returns `true` exactly when that decrement just brought
the counter to zero -- the caller's cue to free that table and, if the
table is itself someone's child, cascade the same `decrement()` call
into the parent's own counter. Getting a transition's accounting wrong
(an `increment()` without a matching eventual `decrement()`, or vice
versa) desynchronizes the counter from reality exactly as a hand-rolled
reference count would; `increment()`/`decrement()` only guard against
the counter's own overflow/underflow (via `RELOCO_ASSERT`), not against
a caller that forgets to call them.

## Where the counter lives: embedded in the caller's own per-table-page metadata

`structo::arch` headers never own page-table memory, so this counter
cannot either. The intended usage is to embed one
`page_table_occupancy<CounterInt>` inside whatever per-table-page
descriptor/metadata struct the caller already maintains for each
page-table page -- a kernel's `struct page`/`vm_page` equivalent, or a
side array indexed by physical frame number -- right alongside fields
like the table's physical address, a lock, or free-list linkage. There is
deliberately no "attach a counter to this `reloco::span<Entry>`" API
here: this header has no opinion on how a caller indexes from a table
back to its counter, since that indexing scheme is inseparable from
whatever page-descriptor/allocator infrastructure the embedder already
has.

## Cold-start: seeding a counter for an already-populated table

The one legitimate full-table scan is a *one-time* cost: initializing a
counter for a table that already has live entries before this accounting
scheme started tracking it (e.g. attaching to a table built by
firmware/a bootloader, or recovering bookkeeping after a crash).
`count_non_null()` does that one-time scan; after seeding a counter with
its result, maintain it purely via `increment()`/`decrement()` from then
on -- never call `count_non_null()` again on the hot unmap path.

## API

- `page_table_occupancy()` -- default-constructs empty (`count() == 0`).
- `page_table_occupancy(CounterInt initial_count)` -- constructs with an
  already-known count (typically the result of `count_non_null()`).
- `count()` -- current tracked live-entry count.
- `empty()` -- `count() == 0`.
- `increment()` -- call once per null &rarr; non-null entry transition.
  Traps (`RELOCO_ASSERT`) on `CounterInt` overflow.
- `decrement()` -- call once per non-null &rarr; null entry transition;
  returns `true` iff the table just became empty. Traps
  (`RELOCO_ASSERT`) if already at zero (an accounting bug).
- `count_non_null<Entry>(reloco::span<const Entry> table)` -- static,
  one-time O(`table.size()`) scan counting already-non-null entries, for
  cold-start seeding only (see above). Requires `Entry::is_null() const`.

## Example: cascading release during a recursive unmap

```cpp
// Caller's own per-table-page metadata/descriptor, one per physical
// page backing a table (every non-leaf-adjacent level's tables need
// one tracking child-table entries; the leaf-adjacent level's tables
// need one tracking leaf-page entries instead -- same counter type
// either way).
struct table_page_descriptor {
  structo::arch::page_table_occupancy<std::uint32_t> occupancy;
  // ... physical address, lock, free-list linkage, etc.
};

// Recursive, post-order unmap: clear leaf entries first, then unwind
// back up, freeing and un-linking any table that just became empty.
// `table_descriptor_for(step)` / `free_table(...)` / `map_child_table(...)`
// are caller-supplied (resolve/release a child table's own descriptor);
// not shown here.
void unmap_level(reloco::span<entry> table, table_page_descriptor &desc, std::uint64_t base,
                  std::uint64_t start, std::uint64_t end, int level) {
  using l_range = structo::arch::page_table_level_range<levels, 0, entry>; // LevelIndex selected per `level`
  for (auto &step : l_range(table, base, start, end)) {
    if (step.entry().is_null()) {
      continue;
    }
    if (is_leaf_level(level)) {
      step.entry() = entry{}; // unmap the page
      desc.occupancy.decrement(); // caller checks desc itself after the loop
      continue;
    }
    table_page_descriptor &child_desc = table_descriptor_for(step.entry());
    unmap_level(map_child_table(step.entry()), child_desc, step.entry_base(), step.range_start(),
                step.range_end(), level + 1);
    if (child_desc.occupancy.empty()) {
      free_table(step.entry()); // return the now-unused physical page
      step.entry() = entry{};   // unlink it from this table
      desc.occupancy.decrement(); // cascades the same check into *this* table
    }
  }
}
```

Note that `child_desc.occupancy.empty()` is checked *after* recursing
into the child -- that recursive call is exactly what already drove
`child_desc.occupancy` down via its own `decrement()` calls on leaf (or
further-nested child) entries, so by the time control returns here the
counter already reflects whether anything is still mapped under that
child table. The root table is never freed by this pattern (there is no
parent entry to unlink it from or cascade into).

See also: [`page_table_range.md`](page_table_range.md) (the range
adapter this counter is designed to pair with) and
[`page_table_traits.md`](page_table_traits.md) (the per-level index
traits and opaque entry handle both headers build on).
