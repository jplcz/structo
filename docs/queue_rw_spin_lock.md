<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `queue_rw_spin_lock<Traits>`

`include/structo/sync/queue_rw_spin_lock.hpp`

A reader-writer spinlock in the same spirit as
[`rw_spin_lock`](rw_spin_lock.md), but with writer-vs-writer contention
resolved through an MCS-style admission queue
([`queue_spin_lock`](queue_spin_lock.md)) instead of every waiting
writer re-CAS-ing one shared word -- the same relationship Linux's own
queued rwlock (`qrwlock`) has to the older, plain `rwlock_t`.

```cpp
structo::sync::queue_rw_spin_lock<kernel_lock_traits> lock;

lock.read_lock();
// ... any number of concurrent readers here ...
lock.read_unlock();

structo::sync::queue_rw_spin_lock<kernel_lock_traits>::node qnode;
lock.write_lock(qnode);
// ... exclusive access here ...
lock.write_unlock(qnode);
```

`Traits` supplies the exact same two hooks as
[`kernel_spin_lock`](kernel_spin_lock.md)'s own `Traits` (plus the same
optional `softlock_limit`) -- every lock type in this family can share
one `Traits` policy.

## Why queue only the writer side

`rw_spin_lock` already solves writer starvation (via its "writer
waiting" bit), but under *heavy multi-writer* contention every waiting
writer still independently spins trying to CAS the same shared state
word -- exactly the cache-line-bouncing problem
[`queue_spin_lock`](queue_spin_lock.md) solves for an ordinary mutex.
`queue_rw_spin_lock` fixes just that part: writers first queue up via an
internal `queue_spin_lock` (local spinning, one waiter per node, no
shared cache line to contend), and only the writer that has *already*
won admission touches the shared reader/writer state word to announce
intent and wait for readers to drain. Readers remain simple CAS-spinners
against that shared word, same as `rw_spin_lock` -- reads are expected
to be brief and frequent, so queueing them individually would add
overhead without a matching benefit (this matches `qrwlock`'s own
design: only the writer path goes through a queue/wait-lock, not the
reader path).

## API

Reader side is identical to [`rw_spin_lock`](rw_spin_lock.md)'s own:

- `read_lock()` / `read_unlock()` / `try_read_lock()` -- same semantics
  and caveats (a `try_read_lock()` may spuriously fail under pure
  reader-vs-reader contention).

Writer side takes a caller-supplied node, exactly like
[`queue_spin_lock`](queue_spin_lock.md) (see that header for the full
rationale on why):

- `write_lock(node &n)` -- enqueues `n` onto the writer admission queue
  (spinning there, MCS-style, only on `n`'s own local flag against other
  writers), then -- once admitted -- announces intent and spins (with
  `backoff`-throttled polling and a `softlock_detector` tripwire) until
  every existing reader has drained. Traps (`RELOCO_ASSERT`, via the
  underlying admission queue) if the calling context already holds this
  as a writer.
- `try_write_lock(node &n)` -- non-blocking; only succeeds when
  admission is immediately free *and* the lock itself is completely
  free (no readers, no writer). Never announces intent, so it cannot
  itself starve readers already racing it.
- `write_unlock(node &n)` -- releases the lock, then admits the next
  queued writer (if any). Traps (`RELOCO_ASSERT`) if the calling context
  is not the current writer.
- `reader_count()`, `is_read_locked()`, `is_write_locked()`,
  `is_locked()`, `is_write_locked_by_current()` -- same diagnostic
  accessors as `rw_spin_lock`.

The destructor traps (`RELOCO_ASSERT`) if the lock is still held (by
any reader or the writer) or a writer is still waiting for readers to
drain; the internal admission queue's own destructor (run immediately
afterwards) additionally traps if a writer is still queued for it.

## Caveats

Same as [`rw_spin_lock`](rw_spin_lock.md): never appropriate outside
contexts where spinning is known to be short, and recursively taking a
second `read_lock()` from a context that already holds one is not
tracked or guarded against.

See also: [`rw_spin_lock.md`](rw_spin_lock.md), [`queue_spin_lock.md`](queue_spin_lock.md), [`kernel_spin_lock.md`](kernel_spin_lock.md), [`backoff.md`](backoff.md), [`softlock_detector.md`](softlock_detector.md).
