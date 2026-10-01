<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::arch::page_table_level_range<Levels, LevelIndex, Entry, AddrInt>`

`include/structo/arch/page_table_range.hpp`

A forward-iterable adapter over exactly the entries of one caller-
supplied page-table level that a `[start, end)` virtual-address range
touches, built on top of [`page_table_traits.md`](page_table_traits.md)'s
compile-time level decomposition.

## Why this exists

`page_table_traits.hpp` can tell you *which* index a single address
falls at for a given level; walking a whole *range* of addresses (e.g.
`munmap(addr, length)`, or building a mapping that spans many entries)
means repeating that computation for every address the range touches --
wasteful, and easy to get subtly wrong at the range's first/last,
partially-covered entry. `page_table_level_range` does this arithmetic
once: given one level's table (as a caller-supplied span) and a
`[start, end)` range, it yields exactly the entries that range touches
at that level, each annotated with the VA sub-window (clipped to
`[start, end)`) that entry is responsible for.

## Caller must provide memory mappings via span -- no mapping, no allocation

Consistent with every other `structo::arch` header, this type performs
**no** physical-to-virtual mapping and **no** allocation of its own. The
`table` span handed to the constructor must already be a directly
dereferenceable view of one page table's backing physical page -- exactly
as a real kernel already has its own page tables mapped (identity map,
linear/direct map, recursive mapping, whatever that kernel uses) before
it ever walks them. Resolving a *child* table's physical address into
another such span (after inspecting an entry this type yields) is the
caller's job, using whatever `page_table_entry_traits<Tag>`
specialization (see [`page_table_traits.md`](page_table_traits.md)) and
address-space mapping mechanism it already has.

A span whose size does not equal the level's `entry_count` is a caller
contract violation and triggers `RELOCO_ASSERT` (not a silently truncated
walk).

## API

- `entry_count` -- number of entries in this level's table (`1 <<
  index_bits`, same as `page_table_levels::entry_count<LevelIndex>()`).
- `entry_span` -- virtual-address span one entry at this level covers
  (`1 << shift`).
- `step` -- one visited entry, a plain value type with checked-tier
  accessors: `index()` (table index), `entry()` (reference into the
  caller-supplied span -- traps via `RELOCO_ASSERT` if called on a
  default-constructed/unbound `step`), `entry_base()` (full, level-aligned
  VA this entry's window starts at), `range_start()`/`range_end()`
  (intersection of that window with the queried `[start, end)`).
- Constructor `(table, table_base_va, start, end)` -- `table_base_va` is
  the VA corresponding to index 0 of `table`. Entries whose window does
  not intersect `[start, end)` -- including every index outside this
  table's own coverage -- are skipped automatically; the caller does not
  need to pre-clip the range to this table.
- `begin()`/`end()` -- forward iterator yielding `step` by `const`
  reference, ascending index. Ref-qualified `const &` (an rvalue
  `page_table_level_range` cannot have `begin()`/`end()` called on it
  directly, matching every other `reloco`/`structo` range type) --
  always bind the range to a named variable (or a range-for loop, which
  does this implicitly) before iterating.
- `.iter()` -- Rust-style iterator entry point (see below).
- `empty()` -- `true` if `[start, end)` touches no entry of this table at
  all (including an inverted or zero-length range).
- `make_level_range<Levels, LevelIndex>(table, table_base_va, start,
  end)` -- deduces `Entry`/`AddrInt` from the call so `Levels`/
  `LevelIndex` are the only template arguments a caller must spell out.

## Rust-style iteration via `.iter()`

`page_table_level_range` follows `reloco`'s `contiguous_iterator.hpp`
convention: `.iter()` is a thin, lvalue-only wrapper around
`reloco::iter(*this)`, itself built directly on the real `begin()`/
`end()` above (not a separate, parallel adaptor hierarchy). This means
the full `reloco/iterator.hpp` adaptor chain (`.map()`, `.filter()`,
`.enumerate()`, `.take()`, `.skip()`, `.fuse()`, ...) works on it for
free:

```cpp
l0_range r(root_table, root_base_va, start, end);
for (auto index : r.iter()
                       .filter([](const l0_range::step &s) { return !s.entry().is_null(); })
                       .map([](const l0_range::step &s) { return s.index(); })) {
  // index is every populated entry's table index in [start, end).
}
```

As with every other `reloco` adaptor chain, `.iter()` (like `begin()`/
`end()`) can only be called on an lvalue -- bind the range to a named
variable first, as above, rather than chaining off a temporary.

## Example: walking two levels of a `munmap`-style range

```cpp
using levels = structo::arch::page_table_levels<
    structo::page_4k, 48,
    structo::arch::page_table_level<9, 39, false>,
    structo::arch::page_table_level<9, 30, true>,
    structo::arch::page_table_level<9, 21, true>,
    structo::arch::page_table_level<9, 12, true>>;

struct my_pte_tag {};
using entry = structo::arch::page_table_entry<my_pte_tag>;

// `root_table` is already a dereferenceable span over the root table's
// 512 entries (e.g. obtained from the kernel's own direct map of the
// physical page `ttbr0` names). `root_base_va` is 0 for a full-range
// root table (index 0 always starts at VA 0 at the root level).
void unmap_range(reloco::span<entry> root_table, std::uint64_t root_base_va, std::uint64_t start,
                  std::uint64_t end) {
  using l0_range = structo::arch::page_table_level_range<levels, 0, entry>;
  for (auto &step : l0_range(root_table, root_base_va, start, end)) {
    if (step.entry().is_null()) {
      continue; // nothing mapped under this entry at all
    }
    // Decode via the caller's own page_table_entry_traits<my_pte_tag>
    // specialization (not shown): resolve the child table's physical
    // address, map it into a dereferenceable span the same way
    // `root_table` was obtained, then recurse with the *clipped*
    // sub-range this entry is responsible for.
    reloco::span<entry> child_table = map_child_table(step.entry()); // caller-supplied
    using l1_range = structo::arch::page_table_level_range<levels, 1, entry>;
    for (auto &child_step : l1_range(child_table, step.entry_base(), step.range_start(), step.range_end())) {
      // ... continue recursing into level 2, then level 3 (the leaf
      // table), where child_step.entry() is finally unmapped/flushed.
    }
  }
}
```

See also: [`page_table_traits.md`](page_table_traits.md) (the per-level
index traits and opaque entry handle this range adapter is built on).
