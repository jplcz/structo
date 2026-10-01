<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `cpu_index<Tag>`

`include/structo/arch/cpu_index.hpp`

A zero-overhead CRTP-style wrapper resolving "which logical CPU is this"
plus a handful of common kernel/hypervisor core operations (yield,
WFE/SEV, BSP detection, hardware ID), all delegated to a caller-provided
`Tag` policy type.

`Tag` only needs to supply `static constexpr std::size_t max_cpus` plus
whichever of `current()`, `from_context(ctx)`, `hardware_id()`,
`is_bsp()`, `yield()`, `wait_for_event()`, `send_event()` it can actually
implement -- every method is individually SFINAE-gated, so a `Tag` that
only implements a subset still compiles; unsupported operations simply
never appear as callable members of `cpu_index<Tag>`. `current()`/
`from_context()` clamp any `Tag`-reported index that is out of
`[0, max_cpus)` back to `0` rather than propagating an unchecked value.
`uniprocessor_tag` is the bundled zero-cost single-core implementation
(always core 0, always BSP, every operation a no-op) and doubles as a
minimal conforming `Tag` example; see the header's `@file` block for a
fuller multi-core `Tag` example.

See also: [`hw_id_map.md`](hw_id_map.md), [`lazy_context.md`](lazy_context.md).
