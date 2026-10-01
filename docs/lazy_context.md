<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `lazy_context<Traits, CpuId>` / `lazy_context_switcher<Traits, CpuId>`

`include/structo/arch/lazy_context.hpp`

A lazy/deferred hardware-context switching framework for per-thread
coprocessor/extension state (FPU, vector-register files, debug
registers, ...) that is expensive to save/restore on every context
switch and so should only be saved/restored when a thread actually traps
into using it.

`lazy_context` is the small per-thread/per-vcpu state block (residency,
initialized/dirty/constructed flags, the raw `Traits::state_type`);
`lazy_context_switcher` is the stateless dispatcher driving the state
machine over it (`UNINITIALIZED` -> `ACTIVE_SILICON` <->
`CACHED_SILICON` -> `EVICTED_SYNC`/`EVICTED_DIRTY`; see the header's
ASCII-art diagram for the full transition map) across `on_trap()`,
`on_thread_enter()`/`on_thread_leave()`, `on_thread_local_sync()`/
`on_thread_remote_sync()`, and `on_thread_construct()`/`on_thread_exit()`.
`static_per_cpu_storage` is the bundled zero-allocation
`*_active_context()` residency-tracking implementation (a flat
`Context*[MaxCpus]` array) most `Traits` types can simply inherit from.

`Traits` must provide a `state_type` plus `is_enabled()`, `enable()`,
`disable()`, `save_context(state_type&)`, `restore_context(state_type&)`,
and the three `*_active_context()` residency hooks (inherit
`static_per_cpu_storage`, or implement them directly). Everything else --
`matches_trap()` (trap cascading to the correct extension handler),
`init_context()` (distinct first-touch initialization vs. ordinary
restore), `on_migrate()` (cross-CPU migration notification),
`on_thread_construct()`/`on_lazy_construct()` (eager vs. deferred
allocation), and both `destroy_context()` overloads -- is optional and
individually SFINAE-detected, so a `Traits` implementing only the
required subset still compiles. See the header's `@file` block for a
complete `toy_fpu_traits` example.

See also: [`cpu_index.md`](cpu_index.md), [`irq_guard.md`](irq_guard.md).
