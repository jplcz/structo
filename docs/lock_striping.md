<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::sync::lock_striping`

`include/structo/sync/lock_striping.hpp`

A shared, fixed-size array of `N` locks selected by hashing an address,
so a large number of protected objects can share a small, fixed pool of
locks instead of each embedding its own -- mirroring FreeBSD's
`vm_page_lockptr()`/`pa_lock[]` hashed page-lock array: every
`struct vm_page` in the system shares one of a handful of global
mutexes, chosen by hashing the page's own address, rather than each
page carrying a full lock of its own.

This is purely a memory-density trade-off versus an embedded lock per
instance: two unrelated objects that happen to hash to the same stripe
will needlessly contend (or, for the seqlock-style reader below,
needlessly retry) against each other, so `N` should be picked
comfortably larger than the expected number of *concurrently
contended* objects (not the total object count) -- exactly the same
trade-off FreeBSD's own `PA_LOCK_COUNT` makes.

`lock_striping` is the backing table for
[`backref_ptr.hpp`](backref_ptr.md)'s `striped_mutex_cell`/
`striped_rw_cell`/`striped_seqlock_cell`, but is independently usable
any time a dense array of objects needs *some* mutual exclusion without
paying a lock's worth of memory per element.

## Usage

```cpp
// One shared table of 64 spinlocks, chosen by hashing each `vm_page`'s
// own address -- no lock lives inside `vm_page` itself.
structo::sync::lock_striping<64> page_locks;

void detach_page_owner(vm_page *page) {
  auto guard = page_locks.lock_for(page); // picks (and locks) one of the 64 stripes
  page->owner = nullptr;
} // stripe unlocked here
```

## Access modes

Every stripe carries both a `LockT` and a sequence counter bumped
around every exclusive critical section -- the same counter
`reloco::guarded_seqlock` embeds per protected value, just here shared
across whichever keys hash to that stripe. One table therefore supports
three access modes simultaneously:

| Method | Model | Requires |
|---|---|---|
| `lock_for(key)` / `try_lock_for(key)` | Exclusive, blocking/non-blocking. Returns a move-only `guard`. | `LockT::lock()`/`unlock()`/`try_lock()` (e.g. `reloco::spin_lock`, the default) |
| `shared_lock_for(key)` / `try_shared_lock_for(key)` | Shared (read), any number concurrently. Returns a move-only `shared_guard`. | Additionally `LockT::lock_shared()`/`unlock_shared()`/`try_lock_shared()` (e.g. `reloco::shared_mutex`) |
| `sequence_for(key)` / `validate_for(key, start_seq)` | Lock-free, seqlock-style optimistic read: snapshot the sequence, copy out whatever data you're reading, then validate. | None beyond the default `LockT` |

`shared_lock_for`/`try_shared_lock_for` are plain (non-template) member
functions, so a `LockT` lacking `lock_shared()` (like the default
`reloco::spin_lock`) remains a perfectly valid template argument as
long as those two methods are never actually called -- exactly as if
they did not exist for that `LockT`.

### Seqlock-style optimistic reads

`sequence_for`/`validate_for` mirror `reloco::guarded_seqlock`'s own
read/write protocol exactly: `lock_for`/`try_lock_for` bump the
stripe's sequence counter odd on acquire and back to even on release;
a reader snapshots the sequence, copies out its data, then calls
`validate_for` with the snapshot -- which fails (meaning: discard the
snapshot and retry) if the sequence was odd when the read started, or
changed by the time `validate_for` is called:

```cpp
structo::sync::lock_striping<64> table;

int read_something(const void *key, const int &value) {
  for (;;) {
    const auto start = table.sequence_for(key);
    if (start % 2 != 0) {
      reloco::hint::spin_loop();
      continue; // a writer is active on this stripe; retry
    }
    int snapshot = value; // only safe for a trivially-copyable value
    if (table.validate_for(key, start))
      return snapshot;
    reloco::hint::spin_loop();
  }
}
```

`lock_striping` itself does not own the protected value (unlike
`reloco::guarded_seqlock`) -- it only owns the lock and sequence
counter for a given stripe, since it is designed to be shared across
many externally-stored values. The data being read must be
trivially copyable and the caller is responsible for the snapshot/
validate loop above; see [`backref_ptr.hpp`](backref_ptr.md)'s
`striped_seqlock_cell` for a worked example built on top of this.

## See also

- [`backref_ptr.hpp`](backref_ptr.md) -- the primary consumer of this table.
- `reloco::guarded_mutex`/`reloco::rw_lock`/`reloco::guarded_seqlock` --
  the *embedded*-lock equivalents (one lock per protected value instead
  of a shared table).
