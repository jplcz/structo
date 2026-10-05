<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `guarded_rw_spin_mutex<T, RwLock, IrqLocker>`

`include/structo/sync/guarded_rw_spin_mutex.hpp`

The reader/writer counterpart to
[`guarded_spin_mutex<T, Lock, IrqLocker>`](guarded_spin_mutex.md) (see
that page for the full rationale on why a data-owning mutex matters and
how it differs from [`irq_rw_spin_lock`](irq_rw_spin_lock.md)): the
protected `T` lives *inside* the mutex itself, reachable only through
the RAII `read_guard`/`write_guard` returned by `read_lock()`/
`write_lock()`/their `try_` counterparts.

There is no reloco reader/writer mutex to substitute a plain structo
lock into, so -- like `guarded_spin_mutex` -- this header is a
**standalone implementation** that does not wrap or depend on
`irq_rw_spin_lock.hpp`; the two serve different purposes (a bare,
value-less lock wrapper the caller still owns, vs. a mutex that owns
both the lock and the value it protects).

`RwLock` is [`rw_spin_lock<Traits>`](rw_spin_lock.md) (whose
`write_lock()`/`try_write_lock()`/`write_unlock()` take no arguments) or
[`queue_rw_spin_lock<Traits>`](queue_rw_spin_lock.md) (whose
`write_lock(node&)`/`try_write_lock(node&)`/`write_unlock(node&)` take a
caller-supplied `node`, used only for the writer side's own MCS
admission queue); `guarded_rw_spin_mutex` detects which shape `RwLock`
has (via the presence of a nested `RwLock::node` type) and exposes the
matching `write_lock()`/`try_write_lock()` overload automatically. The
reader side (`read_lock()`/`try_read_lock()`) never takes a node for
either underlying lock type.

`IrqLocker` is an extra template parameter alongside `RwLock`,
defaulting to a zero-cost no-op (shared with `guarded_spin_mutex`) for
callers who need none. When given a real
[`irq_guard<Traits>`](irq_guard.md) or
[`spinlock_entry_guard<Traits>`](spinlock_entry_guard.md), both
`read_lock()` and `write_lock()` (and their `try_` counterparts) engage
it *before* the underlying lock and release it *after*, exactly like
`irq_rw_spin_lock`.

```cpp
struct kernel_lock_traits {
  using owner_type = std::uintptr_t;
  static owner_type current_owner() noexcept { return get_current_thread_id(); }
};

// A plain reader/writer spin lock, no IRQ masking (IrqLocker defaults to a no-op):
structo::sync::guarded_rw_spin_mutex<int, structo::sync::rw_spin_lock<kernel_lock_traits>> counter;
{
  auto g = counter.read_lock();
  int seen = *g;
}
{
  auto g = counter.write_lock();
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

// IRQ-safe: interrupts disabled for the duration either side is held.
structo::sync::guarded_rw_spin_mutex<int, structo::sync::rw_spin_lock<kernel_lock_traits>,
                                      structo::sync::irq_guard<arm_irq_traits>>
    irq_safe_counter;

// queue_rw_spin_lock-shaped: write_lock()/try_write_lock() take a caller-supplied node.
structo::sync::guarded_rw_spin_mutex<int, structo::sync::queue_rw_spin_lock<kernel_lock_traits>,
                                      structo::sync::irq_guard<arm_irq_traits>>
    queued_counter;
decltype(queued_counter)::node n;
{
  auto g = queued_counter.write_lock(n);
  *g += 1;
}
```

## API

- `guarded_rw_spin_mutex()` / `guarded_rw_spin_mutex(T)` -- default- or
  value-constructs the protected `T`.
- `read_lock()` -- engages `IrqLocker`, then blocks until a shared
  (read) slot is acquired; returns a move-only `read_guard` granting
  read-only access (`operator*`/`operator->` return `const T&`/`const
  T*`).
- `try_read_lock()` -- engages `IrqLocker`, then attempts a shared slot
  without spinning; returns `reloco::optional<read_guard>`, releasing
  `IrqLocker` immediately on failure.
- `write_lock()` / `write_lock(node&)` -- engages `IrqLocker`, then
  blocks until the exclusive (write) side is acquired; returns a
  move-only `write_guard` granting mutable access (`operator*`/
  `operator->` return `T&`/`T*`).
- `try_write_lock()` / `try_write_lock(node&)` -- engages `IrqLocker`,
  then attempts the exclusive side without spinning; returns
  `reloco::optional<write_guard>`, releasing `IrqLocker` immediately on
  failure.
- `read_guard::unlock()` / `write_guard::unlock()` -- early, idempotent
  release: releases the underlying lock side first, then `IrqLocker`.
- `read_guard::get()` -- named equivalent of `operator*`, returning
  `const T&`.
- `write_guard::get()` / `write_guard::get_mut()` -- named equivalents
  of `operator*`, returning `const T&`/`T&` respectively.
- `read_guard::is_locked()` / `write_guard::is_locked()` -- whether the
  guard still holds its side (false after `unlock()` or being moved
  from).
- `read_guard::irq_locker()` / `write_guard::irq_locker()` -- direct
  access to the underlying `IrqLocker`.
- `reader_count()`, `is_read_locked()`, `is_write_locked()`,
  `is_locked()`, `is_write_locked_by_current()` -- forwarded to the
  underlying `RwLock`.
- `unsafe_get_mut()` -- direct, unguarded mutable access, sound exactly
  when the caller already holds an exclusive `guarded_rw_spin_mutex&`.
  C++ has no borrow checker to enforce that exclusivity, hence the
  `unsafe_` name/annotation: nothing stops a caller from also holding a
  `read_guard`/`write_guard` live at the same time.
- `write_uses_node` -- `static constexpr bool`, true when `RwLock` is
  `queue_rw_spin_lock`-shaped and the `node&`-taking writer overloads
  apply.

See also: [`guarded_spin_mutex.md`](guarded_spin_mutex.md), [`irq_rw_spin_lock.md`](irq_rw_spin_lock.md),
[`irq_guard.md`](irq_guard.md), [`spinlock_entry_guard.md`](spinlock_entry_guard.md),
[`rw_spin_lock.md`](rw_spin_lock.md), [`queue_rw_spin_lock.md`](queue_rw_spin_lock.md).
