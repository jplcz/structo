<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `irq_rw_spin_lock<RwLock, IrqLocker>`

`include/structo/sync/irq_rw_spin_lock.hpp`

The reader/writer counterpart to [`irq_spin_lock<Lock, IrqLocker>`](irq_spin_lock.md)
(see that page for the full rationale on *why* this pairing matters and
the ordering it guarantees) -- wraps [`rw_spin_lock<Traits>`](rw_spin_lock.md)
or [`queue_rw_spin_lock<Traits>`](queue_rw_spin_lock.md) together with an
IRQ/preemption-exclusion locker ([`irq_guard`](irq_guard.md) or
[`spinlock_entry_guard`](spinlock_entry_guard.md)) so that taking either
the read or write side also disables interrupts (and/or enters a
critical section) for exactly as long as that side is held.

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

structo::sync::irq_rw_spin_lock<structo::sync::rw_spin_lock<kernel_lock_traits>,
                                 structo::sync::irq_guard<arm_irq_traits>>
    lock;
{
  auto g = lock.read_lock(); // interrupts disabled, then a reader slot acquired
  // ... any number of concurrent readers, each with interrupts disabled on its own core ...
} // reader slot released, then interrupts restored
```

## Shape detection: plain vs. `queue_rw_spin_lock`-shaped writer side

`rw_spin_lock<Traits>`'s `write_lock()`/`try_write_lock()`/
`write_unlock()` take no arguments; `queue_rw_spin_lock<Traits>`'s take a
caller-supplied `node&` for the writer side's own MCS admission queue.
`irq_rw_spin_lock<RwLock, IrqLocker>` detects which shape `RwLock` has
(via the presence of a nested `RwLock::node` type) and exposes the
matching `write_lock()`/`try_write_lock()` overload automatically. The
reader side (`read_lock()`/`try_read_lock()`/`read_unlock()`) never
takes a node for either underlying lock type.

## API

- `read_lock()` -- engages `IrqLocker`, then blocks until a shared slot
  is acquired; returns a move-only `read_guard`.
- `try_read_lock()` -- engages `IrqLocker`, then attempts
  `RwLock::try_read_lock()` without spinning; returns
  `reloco::optional<read_guard>`, releasing `IrqLocker` immediately on
  failure.
- `write_lock()` / `write_lock(node&)` -- engages `IrqLocker`, then
  blocks until the exclusive side is acquired; returns a move-only
  `write_guard`.
- `try_write_lock()` / `try_write_lock(node&)` -- engages `IrqLocker`,
  then attempts the underlying `try_write_lock()` without spinning;
  returns `reloco::optional<write_guard>`, releasing `IrqLocker`
  immediately on failure.
- `read_guard::unlock()` / `write_guard::unlock()` -- early, idempotent
  release: releases the underlying reader/writer side first, then
  `IrqLocker`.
- `reader_count()`, `is_read_locked()`, `is_write_locked()`,
  `is_locked()`, `is_write_locked_by_current()` -- forwarded to the
  underlying `RwLock`.
- `underlying()` -- direct access to the wrapped `RwLock`, for
  diagnostics that don't fit this wrapper's API.

See also: [`irq_spin_lock.md`](irq_spin_lock.md), [`irq_guard.md`](irq_guard.md),
[`spinlock_entry_guard.md`](spinlock_entry_guard.md), [`rw_spin_lock.md`](rw_spin_lock.md),
[`queue_rw_spin_lock.md`](queue_rw_spin_lock.md).
