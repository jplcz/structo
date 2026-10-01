<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `per_cpu_ptr<Tag, T>`

`include/structo/arch/per_cpu_ptr.hpp`

A thin, storage-free, type-safe resolver for a per-CPU pointer, keyed by
a caller-defined `Tag` type so unrelated per-CPU pointers of different
`T` never collide even if the underlying kernel only has one real
backing mechanism to offer.

This class owns no memory and implements no per-CPU storage itself:
`Tag` supplies the real per-CPU container -- a linear array indexed by
CPU, FreeBSD's `PCPU_GET`/`PCPU_SET` macros, Linux's `this_cpu_ptr()`/
`per_cpu_ptr()`, or anything else a concrete kernel port wants -- via
`get_ptr(cpu)`/`set_ptr(cpu, ptr)`; `per_cpu_ptr<Tag, T>` only
`static_cast`s the `Tag`'s type-erased `void*` to/from `T*`. `max_cpus`
and the CPU-id type (`Tag::cpu_id_type`, defaulting to `std::size_t`) are
both pulled from `Tag`, consistent with [`cpu_index<Tag>`](cpu_index.md)'s
contract.

`get()`/`set(ptr)` (no explicit CPU) resolve the *running* CPU's slot:
if `Tag` provides a dedicated `get_current_ptr()`/`set_current_ptr(ptr)`
fast path it is used directly; otherwise they fall back to resolving
`Tag::current()` and delegating to the explicit-CPU `get(cpu)`/
`set(cpu, ptr)` overloads. How "current" vs. an explicit foreign CPU's
slot is actually resolved is entirely `Tag`'s decision -- `per_cpu_ptr`
itself never guesses. See the header's `@file` block for a complete
example `Tag`.

See also: [`cpu_index.md`](cpu_index.md), [`lazy_context.md`](lazy_context.md).
