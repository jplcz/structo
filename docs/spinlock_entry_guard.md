<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `spinlock_entry_guard<Traits>`

`include/structo/sync/spinlock_entry_guard.hpp`

An RAII guard plus the supporting `spinlock_entered_token`/
`with_spinlock_entered`/`spinlock_entered_locked<T>` building blocks --
the same shape as [`preemption_guard.md`](preemption_guard.md)'s
toolkit, but wired to FreeBSD-style `spinlock_enter(9)`/
`spinlock_exit(9)` rather than a plain preemption counter.

## What `spinlock_enter()`/`spinlock_exit()` actually do

FreeBSD's real `spinlock_enter(9)`/`spinlock_exit(9)` disable
interrupts *and* enter a critical section (preventing preemption) as
one combined, nestable unit -- the exact pair of things a true "spin"
mutex (`mtx_lock_spin(9)`) needs held around its own lock/unlock so
that neither an interrupt handler nor the scheduler can run on this
core while it is spinning for (or holding) the mutex, which would
otherwise risk a cross-core deadlock against whichever other core is
also spinning for the same lock.

Nesting and interrupt-flag saving are both handled *inside* those two
calls themselves (FreeBSD stores a per-thread nesting count and the
outermost saved interrupt flags on `struct thread`), not by whatever
calls them -- so, unlike [`irq_guard`](irq_guard.md)'s `Traits`, there
is no `flags_type` here either: `Traits::spinlock_enter()`/
`spinlock_exit()` take no arguments and return nothing, exactly
mirroring the real functions' signatures, the same way
[`preemption_guard`](preemption_guard.md)'s `Traits` mirrors a plain
nestable preempt-count.

```cpp
struct kernel_spinlock_entry_traits {
  static void spinlock_enter() noexcept { ::spinlock_enter(); }
  static void spinlock_exit() noexcept { ::spinlock_exit(); }
};

structo::sync::spinlock_entry_guard<kernel_spinlock_entry_traits> guard;
real_spin_mutex.lock();
// ... protected section, safe from both interrupts and preemption ...
real_spin_mutex.unlock();
```

- `spinlock_entry_guard<Traits>` -- move-only RAII guard: calls
  `Traits::spinlock_enter()` on construction, `Traits::spinlock_exit()`
  on destruction (or early via `unlock()`, which is safe to call more
  than once).
- `with_spinlock_entered<Traits>(callable)` -- runs `callable` with the
  section entered for its duration; `callable` may optionally accept a
  `spinlock_entered_token` or a `spinlock_entry_guard<Traits>&` if it
  needs to exit early or prove it is running with the section entered.
- `spinlock_entered_locked<T, Traits>` -- a value wrapper requiring
  proof of an active entered section (via `lock()`, `borrow(token)`, or
  `with_lock()`) before its wrapped `T` is accessible.

## Not a lock itself

This header only provides the RAII wrapper around entering/exiting the
combined interrupts-disabled/critical-section region -- it is
deliberately *not* a lock (no `Traits::current_owner()`, no queueing).
Pair it with [`kernel_spin_lock`](kernel_spin_lock.md)/
[`ticket_spin_lock`](ticket_spin_lock.md)/
[`queue_spin_lock`](queue_spin_lock.md) (or a real kernel's own spin
mutex) for the actual mutual exclusion, the same way FreeBSD's
`mtx_lock_spin()` calls `spinlock_enter()` internally before ever
touching the mutex's own lock word.

See also: [`preemption_guard.md`](preemption_guard.md), [`irq_guard.md`](irq_guard.md), [`kernel_spin_lock.md`](kernel_spin_lock.md).
