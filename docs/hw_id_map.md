<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `hw_id_lut<HwId, MaxCpus, L1Size, Hash>`

`include/structo/arch/hw_id_map.hpp`

A caller-owned, allocation-free, two-tier lookup table mapping an
architectural hardware ID (e.g. ARM's MPIDR_EL1) to a logical CPU index.

Built for the SMP boot path: firmware/devicetree enumerates CPUs by raw
hardware ID, but the rest of the kernel wants a dense `[0, max_cpus)`
logical index. Lookup is a fixed-size hash filter (L1, one byte per
slot, capacity a power of two up to 64) verified against a direct
inverse map to eliminate false positives in O(1); any hash collision
demotes that slot to a tombstone and falls back to a sorted,
duplicate-free array (L2) searched in O(log `MaxCpus`). `insert()` on an
already-registered `hw_id` updates its `cpu_idx` in place (repointing the
L1 fast-path slot and clearing the stale inverse-map entry) rather than
growing the table. `default_hw_id_hash` is the bundled
multiplication-free mixer tuned for small (<=4-byte) and wide (8-byte)
hardware IDs alike, and doubles as a conforming `Hash` example.

See also: [`cpu_index.md`](cpu_index.md).
