<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `fdt_writer`

`include/structo/fdt_writer.hpp`

> Moved from `jplcz_reloco` into `jplcz_structo`'s own `include/structo/`
> and re-homed into the `structo` namespace (the header adds `using
> namespace reloco;` so unmoved reloco types remain reachable unqualified).
> Documented here as extracted from reloco's own reference, with some
> prose still to be reconciled with the new namespace split.

Move-only, single-pass streaming writer for Flattened Device Tree (DTB,
`/dts-v1/`) blobs, written directly into a caller-owned `span<std::byte>`
with no allocation. `try_create(span<std::byte>)` is the only way to obtain
one -- it validates that the span has room for the fixed header, the
`mem_rsvmap` terminator, and a trailing `FDT_END` marker, returning
`error::allocation_failed` instead of asserting if it doesn't (memory
safety over convenience: buffer sizing is often caller/config-derived, not
a compile-time constant, on the embedded/kernel targets this library
serves):

```cpp
std::byte storage[4096];
auto made = structo::fdt::fdt_writer::try_create(structo::span<std::byte>(storage, sizeof(storage)));
if (!made)
  return made.error();
auto w = std::move(made).value();
(void)w.add_mem_reserve(0, 0);
(void)w.begin_node("");
(void)w.property_u32("#address-cells", 2);
(void)w.begin_node("cpus");
(void)w.end_node();
(void)w.end_node();
auto blob = w.finish(); // result<span<std::byte>>: the written prefix of `storage`
```

`add_mem_reserve` appends a `/memreserve/`-style entry (only before the
first node/property); `begin_node`/`end_node` bracket a node; `property*`
appends a named property (`property_empty`, `property_u32`, `property_u64`,
`property_u32_array`, `property_string`, or the raw-bytes `property`).
Property names are deduped into a single shared string-table entry.
Every mutating method returns `result<void>` and is `&`-ref-qualified
(rejecting calls on rvalues); the first failure is latched internally, so
a caller who ignores one failed `result<void>` and keeps writing (or jumps
straight to `finish()`) still gets that same error back from `finish()`
instead of a truncated blob. `finish()` is also `RELOCO_CONSUMABLE`-tracked
(`-Wconsumed`): calling any writer method again after `finish()`, or on a
moved-from writer, is rejected both at compile time (where Clang's
consumed-state analysis can prove it) and at runtime (`error`-returning,
via the same sticky-error latch). Copying is not allowed -- only moving --
matching every other reloco handle that mutates memory in place.

See also: [`fdt_reader.md`](fdt_reader.md).
