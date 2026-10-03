<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `queue_spin_lock<Traits>`

`include/structo/sync/queue_spin_lock.hpp`

An MCS-style queue lock -- the same local-spinning idea behind Linux's
`qspinlock` -- rounding out the
[`kernel_spin_lock`](kernel_spin_lock.md) /
[`ticket_spin_lock`](ticket_spin_lock.md) family (owner tracking,
[`backoff`](backoff.md), [`softlock_detector`](softlock_detector.md)
wiring, matching `Traits` shape) with the one property neither of those
two have: no two waiters ever spin on the same cache line.

```cpp
structo::sync::queue_spin_lock<kernel_lock_traits> lock;

structo::sync::queue_spin_lock<kernel_lock_traits>::node qnode;
lock.lock(qnode);
// ... protected section ...
lock.unlock(qnode);
```

`Traits` supplies the exact same two hooks as
[`kernel_spin_lock`](kernel_spin_lock.md)'s own `Traits` (plus the same
optional `softlock_limit`) -- all three lock types in this family can
share one `Traits` policy.

## Why a queue of local spin sites

`kernel_spin_lock` has every waiter re-poll one shared atomic;
`ticket_spin_lock` fixes *fairness* but still has every waiter re-poll
one shared counter. Either way, *N* contending cores keep bouncing the
same cache line between themselves even though only one waiter's read
actually matters at any given moment (the one currently first in line).
An MCS lock instead has each waiter enqueue a small node -- supplied by
the caller, typically stack-allocated for the duration of the critical
section -- onto a singly linked list via one atomic exchange of a
shared tail pointer, then spin only on a flag *inside its own node*.
`unlock()` hands off by writing directly into the next-in-line node's
local flag, so no two cores ever contend the same cache line while
waiting. This is the same trick Linux's own `qspinlock` scales to
hundreds of cores with -- that implementation additionally packs
everything into one machine word and uses small per-CPU node arrays
indexed by interrupt-nesting depth so callers never have to supply a
node explicitly; `queue_spin_lock` keeps the caller-supplied node
instead, trading that transparency for staying free of any per-CPU or
nesting-depth infrastructure dependency.

## Why the node is caller-supplied

Unlike `kernel_spin_lock`/`ticket_spin_lock`, `lock()`/`try_lock()`/
`unlock()` all take an explicit `node &` parameter -- the very thing
that makes an MCS lock scale (each waiter's spin site is its own private
memory) means that memory has to live somewhere, and the straightforward
place is a local on the same stack frame that holds the critical
section itself. It must stay alive and untouched for the entire time
the lock is held (and while queued waiting for it); passing the same
`node` instance to a lock's `lock()`/`try_lock()` and its matching
`unlock()` is required, and reusing one `node` across *unrelated*,
concurrently-queued acquisitions of different locks is unsafe (a node
tracks its membership in exactly one queue at a time).

## API

- `lock(node &n)` -- enqueues `n`, then spins (with `backoff`-throttled
  polling, and a `softlock_detector` tripwire against a true deadlock)
  on `n`'s own local flag until handed the lock, then stamps the owner
  slot with `Traits::current_owner()`. Traps (`RELOCO_ASSERT`) if the
  calling context already owns the lock.
- `try_lock(node &n)` -- attempts to acquire without spinning; only
  succeeds when the queue is genuinely empty (matching `lock()`'s FIFO
  guarantee rather than letting `try_lock()` itself jump the queue).
- `unlock(node &n)` -- releases the lock, handing off directly to the
  next-in-line waiter's local flag if one is already linked. Traps
  (`RELOCO_ASSERT`) if the calling context is not the current owner.
- `is_locked()` / `is_locked_by_current()` -- same semantics as
  `kernel_spin_lock`'s/`ticket_spin_lock`'s own accessors.
- The lock's destructor traps (`RELOCO_ASSERT`) if it is still held or a
  waiter is still queued; a `node`'s own destructor traps if destroyed
  while still linked into some lock's queue.

Like the rest of this family, never appropriate outside contexts where
spinning is known to be short (IRQ/exception handlers,
pre-scheduler-init code, data shared with an interrupt handler on
another core).

See also: [`kernel_spin_lock.md`](kernel_spin_lock.md), [`ticket_spin_lock.md`](ticket_spin_lock.md), [`rw_spin_lock.md`](rw_spin_lock.md), [`backoff.md`](backoff.md), [`softlock_detector.md`](softlock_detector.md).
