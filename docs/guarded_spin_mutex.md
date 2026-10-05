<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `guarded_spin_mutex<T, Lock, IrqLocker>`

`include/structo/sync/guarded_spin_mutex.hpp`

The spin-lock counterpart of reloco's `guarded_mutex<T, MutexT>` (Rust's
`std::sync::Mutex<T>`): the protected `T` lives *inside* the mutex
itself, reachable only through the RAII `guard` returned by `lock()`/
`try_lock()`, instead of pairing a bare lock with a separately declared
variable nothing stops code from touching without holding it.

## Why this exists, and how it differs from `irq_spin_lock`

`reloco::guarded_mutex<T, MutexT>` already works unmodified with a
*plain* spin lock from this family
([`kernel_spin_lock`](kernel_spin_lock.md)/[`ticket_spin_lock`](ticket_spin_lock.md))
as its `MutexT`, since their `lock()`/`try_lock()`/`unlock()` already
match `reloco::mutex`'s own no-argument shape exactly. `guarded_spin_mutex`
exists for the two things plain substitution cannot do:

1. **[`queue_spin_lock`](queue_spin_lock.md)-shaped locks.** Its
   `lock(node&)`/`try_lock(node&)`/`unlock(node&)` take a caller-supplied
   `node` (the MCS wait-queue entry, which must outlive the time spent
   queued and holding the lock). `reloco::guarded_mutex` has no way to
   thread that extra argument through its fixed `lock()`/`try_lock()`
   signature; `guarded_spin_mutex` detects this shape (via the presence
   of a nested `Lock::node` type, the same mechanism
   [`irq_spin_lock`](irq_spin_lock.md) uses) and exposes a matching
   `lock(node&)`/`try_lock(node&)` overload instead.
2. **Pairing with an IRQ/preemption-exclusion locker.** `IrqLocker` is an
   extra template parameter alongside `Lock`, defaulting to a zero-cost
   no-op for callers who need none. When given a real
   [`irq_guard<Traits>`](irq_guard.md) or
   [`spinlock_entry_guard<Traits>`](spinlock_entry_guard.md), `lock()`/
   `try_lock()` engage it *before* the spin lock and release it *after*,
   exactly like `irq_spin_lock` -- but `guarded_spin_mutex` is a
   **separate, standalone implementation**: it does not wrap or depend
   on `irq_spin_lock`/`irq_rw_spin_lock`, because the two headers serve
   different purposes. `irq_spin_lock<Lock, IrqLocker>` is a bare,
   value-less wrapper around a `Lock` the caller still owns and passes
   in; `guarded_spin_mutex<T, Lock, IrqLocker>` *owns* both `Lock` and
   the protected value `T` itself.

```cpp
struct kernel_lock_traits {
  using owner_type = std::uintptr_t;
  static owner_type current_owner() noexcept { return get_current_thread_id(); }
};

// A plain spin lock, no IRQ masking (IrqLocker defaults to a no-op):
structo::sync::guarded_spin_mutex<int, structo::sync::kernel_spin_lock<kernel_lock_traits>> counter;
{
  auto g = counter.lock();
  *g += 1;
}

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

// IRQ-safe: interrupts disabled for the duration the lock is held.
structo::sync::guarded_spin_mutex<int, structo::sync::kernel_spin_lock<kernel_lock_traits>,
                                   structo::sync::irq_guard<arm_irq_traits>>
    irq_safe_counter;
{
  auto g = irq_safe_counter.lock();
  *g += 1;
}

// queue_spin_lock-shaped: lock()/try_lock() take a caller-supplied node.
structo::sync::guarded_spin_mutex<int, structo::sync::queue_spin_lock<kernel_lock_traits>,
                                   structo::sync::irq_guard<arm_irq_traits>>
    queued_counter;
decltype(queued_counter)::node n;
{
  auto g = queued_counter.lock(n);
  *g += 1;
}
```

## API

- `guarded_spin_mutex()` / `guarded_spin_mutex(T)` -- default- or
  value-constructs the protected `T`.
- `lock()` / `lock(node&)` -- engages `IrqLocker`, then blocks (per
  `Lock::lock()`'s own semantics) until the underlying lock is acquired;
  returns a move-only `guard`.
- `try_lock()` / `try_lock(node&)` -- engages `IrqLocker`, then attempts
  the underlying `Lock::try_lock()` without spinning; returns
  `reloco::optional<guard>`. If the underlying attempt fails, `IrqLocker`
  is released immediately before returning, so a failed attempt never
  leaves interrupts/preemption disabled.
- `guard::operator*` / `guard::operator->` -- access to the protected
  value while the guard is held.
- `guard::get()` / `guard::get_mut()` -- named equivalents of
  `operator*`, returning `const T&`/`T&` respectively.
- `guard::unlock()` -- early, idempotent release: releases the
  underlying `Lock` first, then `IrqLocker`.
- `guard::is_locked()` -- whether the guard still holds the lock (false
  after `unlock()` or being moved from).
- `guard::irq_locker()` -- direct access to the underlying `IrqLocker`.
- `unsafe_get_mut()` -- direct, unguarded mutable access, sound exactly
  when the caller already holds an exclusive `guarded_spin_mutex&`
  (mirroring `reloco::guarded_mutex::unsafe_get_mut()`, which borrows
  `&mut self` at compile time instead of taking the lock at runtime). C++
  has no borrow checker to enforce that exclusivity, hence the `unsafe_`
  name/annotation: nothing stops a caller from also holding a `guard`
  live at the same time.
- `uses_node` -- `static constexpr bool`, true when `Lock` is
  `queue_spin_lock`-shaped and the `node&`-taking overloads apply.

See also: [`guarded_rw_spin_mutex.md`](guarded_rw_spin_mutex.md), [`irq_spin_lock.md`](irq_spin_lock.md),
[`irq_guard.md`](irq_guard.md), [`spinlock_entry_guard.md`](spinlock_entry_guard.md),
[`kernel_spin_lock.md`](kernel_spin_lock.md), [`ticket_spin_lock.md`](ticket_spin_lock.md),
[`queue_spin_lock.md`](queue_spin_lock.md).
