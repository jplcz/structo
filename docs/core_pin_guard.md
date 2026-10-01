<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `core_pin_guard<Traits>`

`include/structo/sync/core_pin_guard.hpp`

An RAII guard that pins the calling thread/task to its current CPU core
for its lifetime -- the same shape as [`irq_guard.md`](irq_guard.md)'s
and [`preemption_guard.md`](preemption_guard.md)'s guards, but
preventing migration rather than interrupts or preemption:

```cpp
structo::sync::core_pin_guard<my_kernel_pin_traits> guard;
// guard.pinned_cpu() is stable until the guard is destroyed/unlock()ed:
// safe to repeatedly touch that CPU's per_cpu_ptr slot across several
// operations without the scheduler migrating us mid-sequence.
```

`Traits` supplies two scheduler hooks:

- `static cpu_id_type pin() noexcept;` -- pins the calling thread to its
  current CPU and returns which CPU that is.
- `static void unpin() noexcept;` -- releases one pin.

plus an optional `cpu_id_type` (defaults to `std::size_t`, the same
convention `per_cpu_ptr`/`cpu_index` use for their own `Tag`s).

- `core_pin_guard<Traits>` -- move-only RAII guard: pins on construction,
  unpins on destruction (or early via `unlock()`, which is safe to call
  more than once); `pinned_cpu()` exposes the CPU the guard pinned to.
- `with_cpu_pinned<Traits>(callable)` -- runs `callable` pinned to the
  calling thread's current CPU; `callable` may optionally accept the
  pinned `cpu_id_type` or a `core_pin_guard<Traits>&`.

Pinning (Linux's `get_cpu()`/`put_cpu()`, FreeBSD's
`sched_pin()`/`sched_unpin()`) stops the scheduler from migrating the
calling context to a different core while a sequence of operations
needs to stay bound to one core's identity or its per-CPU state (see
[`per_cpu_ptr.md`](per_cpu_ptr.md)) -- it does **not** by itself disable
preemption or interrupts; a pinned thread can still be preempted and
later resumed, just always on the same core. Combine with
[`preemption_guard`](preemption_guard.md)/[`irq_guard`](irq_guard.md) if
stronger exclusion is also required.

See the header's `@file` block for a complete `kernel_pin_traits`
example (a per-thread nesting counter guarding a scheduler pin).

See also: [`irq_guard.md`](irq_guard.md), [`preemption_guard.md`](preemption_guard.md), [`per_cpu_ptr.md`](per_cpu_ptr.md).
