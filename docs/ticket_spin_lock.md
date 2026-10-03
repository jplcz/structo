<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `ticket_spin_lock<Traits>`

`include/structo/sync/ticket_spin_lock.hpp`

A FIFO-fair busy-wait lock -- "take a ticket, spin until it's your
number" -- in the same spirit as
[`kernel_spin_lock<Traits>`](kernel_spin_lock.md) (owner tracking,
[`backoff`](backoff.md), [`softlock_detector`](softlock_detector.md)
wiring, identical `Traits` shape), but trading its bare
test-and-test-and-set `lock()` for strict first-come-first-served
acquisition order:

```cpp
structo::sync::ticket_spin_lock<kernel_lock_traits> lock;
lock.lock();
// ... protected section, released strictly in arrival order ...
lock.unlock();
```

`Traits` supplies the exact same two hooks as
[`kernel_spin_lock`](kernel_spin_lock.md)'s own `Traits` (plus the same
optional `softlock_limit`) -- both lock types can share one `Traits`
policy.

## Why fairness, and why it costs something

`kernel_spin_lock`/`reloco::spin_lock` are both a simple compare-exchange
race: whichever contender's `compare_exchange` happens to land first
wins, with no memory of who asked first. Under heavy, sustained
contention that can let one hot core keep re-winning the race
indefinitely while a less "lucky" one starves -- acceptable for a lock
held briefly and rarely contended, but a real correctness/fairness risk
for one many cores hammer constantly (e.g. a global runqueue lock). A
ticket lock removes that risk entirely, at the cost of one extra atomic
fetch-add per `lock()` and a second cache line every waiter polls -- the
same trade-off Linux's own `ticket_spinlock_t` (the pre-`qspinlock`
default) made.

Two monotonically increasing counters implement it: the next ticket
`lock()` will hand out, and the ticket currently being served. `lock()`
atomically takes the next ticket, then spins until it is the one being
served; `unlock()` advances the served counter by one, releasing exactly
the next-in-line waiter.

- `lock()` -- takes the next ticket, then spins (with
  `backoff`-throttled polling, and a `softlock_detector` tripwire
  against a true deadlock) until it is served, then stamps the owner
  slot with `Traits::current_owner()`. Traps (`RELOCO_ASSERT`) if the
  calling context already owns the lock.
- `try_lock()` -- attempts to acquire without spinning; only succeeds
  when the lock is free *and* no other waiter is already queued ahead of
  this attempt (matching `lock()`'s FIFO guarantee rather than letting
  `try_lock()` itself jump the queue).
- `unlock()` -- releases the lock, admitting the next-in-line waiter (if
  any). Traps (`RELOCO_ASSERT`) if the calling context is not the
  current owner.
- `is_locked()` / `is_locked_by_current()` -- same semantics as
  `kernel_spin_lock`'s own accessors.
- Destructor traps (`RELOCO_ASSERT`) if the lock is still held *or* a
  waiter is still queued for it.

Like `kernel_spin_lock`, never appropriate outside contexts where
spinning is known to be short (IRQ/exception handlers,
pre-scheduler-init code, data shared with an interrupt handler on
another core) -- a waiter here can never jump ahead of an earlier one
that is itself stalled, so an unexpectedly long critical section delays
strictly more waiters than the unfair `kernel_spin_lock` would.

See also: [`kernel_spin_lock.md`](kernel_spin_lock.md), [`queue_spin_lock.md`](queue_spin_lock.md), [`backoff.md`](backoff.md), [`softlock_detector.md`](softlock_detector.md).
