<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `rw_spin_lock<Traits>`

`include/structo/sync/rw_spin_lock.hpp`

A busy-wait reader-writer lock rounding out the
[`kernel_spin_lock`](kernel_spin_lock.md) /
[`ticket_spin_lock`](ticket_spin_lock.md) /
[`queue_spin_lock`](queue_spin_lock.md) family with the one thing none
of those three provide: any number of concurrent *readers*, with
writers still fully exclusive against both readers and other writers.

```cpp
structo::sync::rw_spin_lock<kernel_lock_traits> lock;

lock.read_lock();
// ... any number of concurrent readers here ...
lock.read_unlock();

lock.write_lock();
// ... exclusive access here ...
lock.write_unlock();
```

`Traits` supplies the exact same two hooks as
[`kernel_spin_lock`](kernel_spin_lock.md)'s own `Traits` (plus the same
optional `softlock_limit`) -- all four lock types in this family can
share one `Traits` policy. Readers are not individually tracked by
owner (any number may hold the lock at once, from any context), so only
the writer side is stamped/asserted against `Traits::current_owner()`.

## State packed into one word

A single `std::atomic<std::uint32_t>` encodes everything:

- bit 31 (`writer_bit`) -- a writer currently holds the lock.
- bit 30 (`writer_waiting_bit`) -- at least one writer is waiting;
  sampled by `read_lock()` to refuse *new* readers.
- bits 0-29 (`reader_mask`) -- the number of readers currently holding
  the lock.

`read_lock()` spins (incrementing the reader count via CAS) so long as
neither bit is set; `write_lock()` first sets the waiting bit (so every
*new* reader, and any other writer, backs off), then spins until the
word reads back as exactly the waiting bit alone (no readers left, no
other writer), at which point it CAS's the whole word to exactly the
writer bit.

## Why the "writer waiting" bit exists

A naive reader-writer spinlock (CAS the reader count up whenever no
writer currently holds it) lets a steady stream of readers starve a
waiting writer indefinitely, since a new reader can always slip in
between two others' `read_unlock()`s. Announcing writer intent *before*
spinning for the drain -- the same idea as glibc's
`PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP` -- closes that window:
once a writer is waiting, no further reader is admitted, so the
existing readers are guaranteed to drain to zero in bounded time.

## API

- `read_lock()` / `read_unlock()` -- shared access; any number of
  holders at once. `read_unlock()` traps (`RELOCO_ASSERT`) on a
  double-unlock (no active readers) or corrupted state.
- `try_read_lock()` -- single CAS attempt; may spuriously fail under
  pure reader-vs-reader contention even when no writer is involved.
- `write_lock()` / `write_unlock()` -- exclusive access, with owner
  tracking identical to `kernel_spin_lock`. Traps on self-deadlock
  (recursive `write_lock()`) and on unlock by a non-owner.
- `try_write_lock()` -- single CAS attempt; only succeeds when the lock
  is completely free. Unlike `write_lock()`, never announces intent, so
  it cannot itself starve readers already racing it.
- `reader_count()`, `is_read_locked()`, `is_write_locked()`,
  `is_locked()`, `is_write_locked_by_current()` -- best-effort
  diagnostic snapshots, as with the rest of this family.
- The destructor traps (`RELOCO_ASSERT`) if the lock is still held (by
  any reader or the writer) or a writer is still waiting for it.

## Caveats

As with the rest of this family, never appropriate outside contexts
where spinning is known to be short (IRQ/exception handlers,
pre-scheduler-init code, data shared with an interrupt handler on
another core).

Recursively taking a second `read_lock()` from a context that already
holds one is **not** tracked or guarded against (there is no per-reader
identity to check) -- as with real kernels' own reader-writer locks
(e.g. FreeBSD's `rwlock(9)`), doing so while a writer is also waiting
can self-deadlock, because the pending writer bit blocks the recursive
`read_lock()` call exactly like it would any other new reader.

See also: [`kernel_spin_lock.md`](kernel_spin_lock.md), [`ticket_spin_lock.md`](ticket_spin_lock.md), [`queue_spin_lock.md`](queue_spin_lock.md), [`backoff.md`](backoff.md), [`softlock_detector.md`](softlock_detector.md).
