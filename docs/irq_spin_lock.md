<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `irq_spin_lock<Lock, IrqLocker>`

`include/structo/sync/irq_spin_lock.hpp`

Wraps any non-reader/writer lock from this family --
[`kernel_spin_lock`](kernel_spin_lock.md), [`ticket_spin_lock`](ticket_spin_lock.md),
[`queue_spin_lock`](queue_spin_lock.md) -- together with an IRQ/
preemption-exclusion locker ([`irq_guard`](irq_guard.md) or
[`spinlock_entry_guard`](spinlock_entry_guard.md)) so that acquiring the
lock *also* disables interrupts (and/or enters a critical section) for
exactly as long as the lock is held -- the same pairing FreeBSD's
`mtx_lock_spin(9)` performs internally (`spinlock_enter()` then the
mutex's own lock word) and Linux's `spin_lock_irqsave()` performs via
`local_irq_save()` plus `spin_lock()`.

## Why this exists

Without this wrapper, callers already have to pair the two building
blocks by hand, and -- critically -- get the *order* right: the IRQ/
preemption locker must be acquired *before* the spin lock and released
*after* it, or an interrupt handler on the same core could itself try to
take the same lock while this core is still spinning for it, deadlocking
against itself. `irq_spin_lock` bakes that ordering into one RAII
`guard` so it can never be gotten backwards: `lock()` engages
`IrqLocker` first, then the underlying `Lock`; the `guard`'s destructor
(or early `unlock()`) releases the underlying `Lock` first, then
`IrqLocker`.

```cpp
struct arm_irq_traits {
  using flags_type = std::uint32_t;

  static flags_type hw_save_irqs() noexcept {
    std::uint32_t cpsr;
    asm volatile("mrs %0, cpsr" : "=r"(cpsr));
    asm volatile("cpsid i" ::: "memory");
    return cpsr;
  }

  static void hw_restore_irqs(flags_type cpsr) noexcept {
    asm volatile("msr cpsr_c, %0" : : "r"(cpsr) : "memory");
  }
};

struct kernel_lock_traits {
  using owner_type = std::uintptr_t;
  static owner_type current_owner() noexcept { return get_current_thread_id(); }
};

structo::sync::irq_spin_lock<structo::sync::kernel_spin_lock<kernel_lock_traits>,
                              structo::sync::irq_guard<arm_irq_traits>>
    lock;
{
  auto g = lock.lock(); // interrupts disabled, then the spinlock acquired
  // ... protected section, safe from this core's own interrupt handler too ...
} // spinlock released, then interrupts restored
```

`IrqLocker` may just as well be
[`spinlock_entry_guard<Traits>`](spinlock_entry_guard.md) (FreeBSD-style
`spinlock_enter()`/`spinlock_exit()`) instead of `irq_guard<Traits>` --
both satisfy the exact same shape, by design.

## Shape detection: plain vs. `queue_spin_lock`-shaped locks

`kernel_spin_lock<Traits>`/`ticket_spin_lock<Traits>`'s `lock()`/
`try_lock()`/`unlock()` take no arguments; `queue_spin_lock<Traits>`'s
take a caller-supplied `node&` for its MCS wait queue.
`irq_spin_lock<Lock, IrqLocker>` detects which shape `Lock` has (via the
presence of a nested `Lock::node` type) and exposes the matching
`lock()`/`try_lock()` overload automatically:

```cpp
structo::sync::irq_spin_lock<structo::sync::queue_spin_lock<kernel_lock_traits>,
                              structo::sync::irq_guard<arm_irq_traits>>
    lock;

structo::sync::irq_spin_lock<structo::sync::queue_spin_lock<kernel_lock_traits>,
                              structo::sync::irq_guard<arm_irq_traits>>::node qnode;
auto g = lock.lock(qnode);
```

## API

- `lock()` / `lock(node&)` -- engages `IrqLocker`, then blocks until the
  underlying `Lock` is acquired; returns a move-only `guard`.
- `try_lock()` / `try_lock(node&)` -- engages `IrqLocker`, then attempts
  the underlying `Lock::try_lock()` without spinning; returns
  `reloco::optional<guard>`. If the underlying attempt fails,
  `IrqLocker` is released immediately before returning, so a failed
  attempt never leaves interrupts/preemption disabled.
- `guard::unlock()` -- early, idempotent release: releases the
  underlying `Lock` first, then `IrqLocker`.
- `is_locked()` / `is_locked_by_current()` -- forwarded to the
  underlying `Lock`.
- `underlying()` -- direct access to the wrapped `Lock`, for diagnostics
  that don't fit this wrapper's API.

See also: [`irq_rw_spin_lock.md`](irq_rw_spin_lock.md), [`irq_guard.md`](irq_guard.md),
[`spinlock_entry_guard.md`](spinlock_entry_guard.md), [`kernel_spin_lock.md`](kernel_spin_lock.md),
[`ticket_spin_lock.md`](ticket_spin_lock.md), [`queue_spin_lock.md`](queue_spin_lock.md).
