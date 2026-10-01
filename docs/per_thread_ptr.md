<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `per_thread_ptr<Tag, T>`

`include/structo/arch/per_thread_ptr.hpp`

`per_cpu_ptr<Tag, T>`'s (see [`per_cpu_ptr.md`](per_cpu_ptr.md)) per-thread
sibling: a thin, storage-free, type-safe resolver for the *running
thread's own* pointer, keyed by a caller-defined `Tag` type so unrelated
per-thread pointers of different `T` never collide even if the
underlying kernel only has one real backing mechanism to offer.

Unlike `per_cpu_ptr`, it deliberately exposes **no** explicit-thread
accessor -- a CPU set is small, fixed-size, and reading/poking a
different CPU's slot from afar is a routine, well-defined kernel
operation, but reaching into an *arbitrary other thread's* private slot
without that thread's own synchronization is rarely safe and easy to get
wrong. `per_thread_ptr<Tag, T>` therefore only ever resolves the calling
thread's own slot via `get()`/`set(ptr)`.

`Tag` must provide either a dedicated `get_current_ptr()`/
`set_current_ptr(ptr)` fast path (the natural choice when the backing
storage is itself thread-local, e.g. `__thread`/TLS), or a `current()`
thread-id resolver plus `get_ptr(tid)`/`set_ptr(tid, ptr)` that
`per_thread_ptr` combines internally -- e.g. an intrusive field inside
the kernel's own task/thread control block, resolved via the scheduler's
own "current thread" lookup. See the header's `@file` block for a
complete example `Tag`.

See also: [`per_cpu_ptr.md`](per_cpu_ptr.md), [`cpu_index.md`](cpu_index.md).
