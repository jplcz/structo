<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `sg_entry<SpaceTag, PhysInt>` / `sg_list<Container>`

`include/structo/sg_list.hpp`

> Moved from `jplcz_reloco` into `jplcz_structo`'s own `include/structo/`
> and re-homed into the `structo` namespace (the header adds `using
> namespace reloco;` so unmoved reloco types remain reachable unqualified).
> Documented here as extracted from reloco's own reference, with some
> prose still to be reconciled with the new namespace split.

`sg_entry` represents a single scatter-gather segment (a physical address
and a length). `sg_list<Container>` is a container-parameterized list of
`sg_entry`s, following the same caller-supplied-`Container` convention as
[`fdt_index`](fdt_index.md).

## `sg_list_cursor<PhysInt>`

A single-pass pull cursor that splits an `sg_list`'s entries into
caller-sized chunks on demand, driven purely by repeated
`next_up_to<SpaceTag>(max_length)` calls (no `begin()`/`end()`, no
Rust-style `.iter()`). The typical use case is remapping memory, where
the maximum chunk size accepted at the destination (e.g. how much a
particular mapper call can map as one segment) is a runtime decision --
possibly different from call to call -- rather than a single fixed limit
chosen up front. `max_length` is an upper bound, not an exact size: a
returned chunk may be shorter, either because the remainder of the
current `sg_entry` is smaller than `max_length`, or because a chunk is
never split across two different original entries (an entry boundary
always ends a chunk).

Only `PhysInt` (the raw address/length integer width) is a template
parameter; a cursor erases the `sg_list`'s `Container` and `SpaceTag` at
construction (CTAD deduces `PhysInt` from the `sg_list` passed in):

```cpp
structo::sg_list<> list; // sg_entry<dma_bus_space>
// ... populate list ...
structo::sg_list_cursor cursor(list); // deduces sg_list_cursor<std::uint64_t>
```

This lets a cursor be handed to a low-level memory mapper that is not
itself templated on the list's concrete `Container`/`SpaceTag`, while
still letting that mapper validate, at runtime, which address space the
cursor actually carries before trusting its output -- mirroring
`reloco::any`'s `is<T>()`/`try_get<T>()` type-erasure idiom:

- `holds<SpaceTag>()` -- `true` if the cursor was built from an
  `sg_list<Container>` whose `entry_type` is `sg_entry<SpaceTag, PhysInt>`.
- `entry_type_id()` -- the erased `reloco::type_id` of that `sg_entry`
  specialization, for callers that want to compare/log it directly.
- `next_up_to<SpaceTag>(max_length)` -- returns the next chunk as a typed
  `sg_entry<SpaceTag, PhysInt>`, or `error::invalid_argument` if
  `max_length == 0` or `SpaceTag` doesn't match `holds<SpaceTag>()`, or
  `error::out_of_bounds` once every entry has been fully consumed.

A cursor holds only a type-erased pointer back into its `sg_list`, a
function pointer bound to that list's concrete `Container` type, the
erased entry type's `reloco::type_id`, and small plain cursor state
(current entry index + consumed-so-far offset) -- all trivially
copyable, so a caller needing more than one independent pass over the
same list can simply copy the cursor before advancing it, rather than
re-deriving one from the list. The bound `sg_list` must outlive the
cursor (the constructor's `list` parameter is `RELOCO_LIFETIMEBOUND`).

See also: [`sg_translator.md`](sg_translator.md),
[`compat_sg.md`](compat_sg.md).
