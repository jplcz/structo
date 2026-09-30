<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `try_extract_memory`

`include/structo/fdt_memory.hpp`

> Moved from `jplcz_reloco` into `jplcz_structo`'s own `include/structo/`
> and re-homed into the `structo` namespace (the header adds `using
> namespace reloco;` so unmoved reloco types remain reachable unqualified).
> Documented here as extracted from reloco's own reference, with some
> prose still to be reconciled with the new namespace split.

Reads a devicetree's physical memory description straight off a
single-pass `fdt_reader` -- deliberately independent of `fdt_index.hpp`,
since building a random-access index isn't an option before an
allocator/index scratch buffer exists, which is exactly when this is
meant to run -- into two caller-provided `structo::region_set`s: every
byte of installed RAM (`full`), and what's actually available to hand out
(`free`, i.e. `full` minus every reservation):

```cpp
structo::region_set<8> full;
structo::region_set<8> free;
if (auto extracted = structo::fdt::try_extract_memory(reader, full, free); !extracted)
  return extracted.error();
// full/free now hold merged, non-overlapping physical ranges.
```

A node is treated as describing physical RAM ("`/memory`-class") if it's
a direct child of the root with `device_type == "memory"`, or whose name
-- ignoring any `@unit-address` suffix -- is exactly `"memory"`,
`"secure-memory"`, or `"secure_memory"` (covers DTBs describing a
secure/TEE-world RAM carve-out without a `device_type`). Regular
`/memory` nodes are gated by the ordinary `status` property (active
unless present and not `"okay"`); `/secure-memory` nodes are instead
gated by `secure-status` -- the OP-TEE/TF-A convention where a TEE only
claims a secure-memory node once its own `secure-status` is `"okay"`,
independent of `status` -- matching the split where the REE consumes
`/memory` nodes gated by `status` and the TEE consumes `/secure-memory`
nodes gated by `secure-status`. Either property being absent defaults the
node to active, per the devicetree spec's fallback for an omitted
`status`.

Since the struct block doesn't guarantee `/memory` appears before
`/reserved-memory` (or vice versa), and `fdt_reader` is forward-only,
`try_extract_memory` runs two independent passes over two independent
copies of the same reader (`fdt_reader` is a cheap, non-owning cursor
over caller-owned bytes -- copying it just forks the cursor) rather than
one interleaved pass with a temporary exclusions buffer: pass one
collects every active memory-class node's `reg` ranges into `full`
(decoded with root's `#address-cells`/`#size-cells`, default 2/1, since a
node's `reg` uses its *parent's* declared cell counts); `free` starts as
a copy of `full`, has every legacy `/memreserve/`-table entry
(`reader.mem_reserves()`) subtracted, and then pass two subtracts every
non-disabled `/reserved-memory` child's `reg` range (decoded with
`/reserved-memory`'s *own* `#address-cells`/`#size-cells`, since that's
the node declaring them for its children). Fails with `error::not_found`
if no memory-class node exists; otherwise propagates whatever error the
first malformed `reg` property, unsupported cell count (`try_extract_memory`
only supports 1- or 2-cell address/size fields), or `region_set` capacity
overflow reports.

See also: [`fdt_reader.md`](fdt_reader.md), [`region_set.md`](region_set.md).
