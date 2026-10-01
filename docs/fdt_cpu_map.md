<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `try_populate_hw_id_lut_from_fdt`

`include/structo/arch/fdt_cpu_map.hpp`

Reads a devicetree's `/cpus` node straight off a single-pass `fdt_reader`
-- no `fdt_index` build step, no per-node index buffers -- registering
each enabled `cpu@N` child's `reg` (hardware ID) into a caller-provided
[`hw_id_lut`](hw_id_map.md), keyed by a sequential logical CPU index
assigned in devicetree order (0, 1, 2, ...):

```cpp
structo::arch::hw_id_lut<uint32_t, 8> lut;
auto reader = structo::fdt::fdt_reader::try_create(dtb_blob).value();
auto count = structo::arch::try_populate_hw_id_lut_from_fdt(reader, lut);
// count == 2; lut.find(0x1) resolves to logical index 1.
```

Deliberately independent of `fdt_index.hpp`, mirroring
[`fdt_memory.md`](fdt_memory.md)'s `try_extract_memory` rationale: meant
to run early enough -- e.g. right after a bootloader hands off a raw DTB
pointer, before any allocator or index scratch memory exists yet to
resolve an `hw_id_lut`/`cpu_index<Tag>::current()` against -- that
building a random-access index isn't an option, so it walks the struct
block directly instead. Once a real index (or anything else heavier)
becomes available later in boot, `fdt_index`/`device_tree` are the
better fit for repeated or more elaborate devicetree queries; this
function exists specifically for the "index isn't present yet"
early-boot window.

A `cpu@N` node is registered if it is a direct child of a `/cpus` node
(the root's child whose name, ignoring any `@unit-address` suffix, is
exactly `"cpus"`) and its own `status` property is absent or `"okay"` --
the same absent-defaults-to-active fallback every other `structo::fdt`
scanner uses. Its `reg` property is decoded with `/cpus`'s own
`#address-cells` (default 1, the devicetree spec's default for
`/cpus`); only 1- or 2-cell hardware IDs are supported, matching
`hw_id_lut`'s `HwId` footprint (pass a wider `HwId` to keep a 2-cell
ID's full width).

Fails with `error::not_found` if no `/cpus` node exists; otherwise
propagates whatever error the first malformed `reg` property, an
unsupported cell count, or the `hw_id_lut`'s capacity being exceeded
(`error::capacity_exceeded`) reports.

See also: [`hw_id_map.md`](hw_id_map.md), [`cpu_index.md`](cpu_index.md), [`fdt_memory.md`](fdt_memory.md), [`fdt_reader.md`](fdt_reader.md).
