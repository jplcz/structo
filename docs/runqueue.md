<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::fifo_runqueue` / `priority_list_runqueue` / `priority_bucket_runqueue`

`include/structo/runqueue.hpp`

Three allocation-free, intrusive runqueue policies sharing one
identical `enqueue`/`dequeue`/`peek`/`remove`/`empty`/`size` surface --
pick the one matching what a scheduler actually needs, with no call-site
changes required to swap later:

| Policy | `enqueue` | `dequeue`/`peek`/`remove` | Ordering |
|---|---|---|---|
| `fifo_runqueue<Entry, Hook>` | O(1) | O(1) | insertion order; no priority concept |
| `priority_list_runqueue<Entry, Hook, Priority>` | O(n) (sorted insert) | O(1) | ascending `Entry.*Priority`; ties FIFO |
| `priority_bucket_runqueue<Entry, Hook, Priority, NumPriorities>` | O(1) | O(1) | ascending `Entry.*Priority`; ties FIFO within a bucket |

`priority_bucket_runqueue` is the classic Linux "O(1) scheduler" design:
one intrusive list per priority level plus a fixed-size bitmap tracking
which buckets are non-empty, so the highest-priority non-empty bucket is
found with a single `__builtin_ctzll` (count-trailing-zeros) scan. It
trades a fixed `NumPriorities`-sized bucket array for O(1) `enqueue`
too, at the cost of needing a bounded priority range known at compile
time -- `priority_list_runqueue` is the better fit when the number of
distinct priority levels is unbounded or unknown ahead of time.

## Intrusive, not owning

Like `reloco::c_tailq` (used internally by all three), none of these
types own the entries they queue. `Entry` is caller-owned storage that
must outlive every runqueue operation referencing it, and must embed its
own intrusive link field, declared via the same pointer-to-member `Hook`
non-type template parameter `c_tailq` itself uses:

```cpp
struct my_task {
  struct {
    my_task *next = nullptr;
    my_task **prev = nullptr;
  } link;
  unsigned priority = 0; // lower runs first
};
```

`priority_list_runqueue`/`priority_bucket_runqueue`'s `Priority`
parameter is a pointer-to-member at an unsigned integral field on
`Entry`, read (never written) by the policy -- the caller sets it before
`enqueue`, and must not change it while the entry is queued without
`remove`ing it first (the same precondition `c_tailq::remove` itself
already has for any in-place mutation of linked state).

## Checking `remove`'s precondition: `is_linked`

`remove(entry)` requires @p entry to currently be enqueued -- just like
`reloco::c_tailq::remove` itself, and just as easy to violate by
accident. Every policy has a static `is_linked(entry)` method answering
that precondition in O(1), with no external bookkeeping: it reads
@p entry's own hook state directly (`prev` is non-null while linked, at
any position, and `nullptr` both before the first `enqueue` and after
`remove`/`dequeue`).

```cpp
if (my_runqueue::is_linked(entry))
  rq.remove(entry);
```

## Why not one `runqueue<Entry, Policy>` facade

The three policies share a method surface, but deliberately stay three
distinct class templates rather than one facade dispatching through a
`Policy` trait: there is no shared state representation to factor out
(a plain `c_tailq`, a sorted `c_tailq`, and an array of `c_tailq` plus a
bitmap are simply different objects), so a facade would only add
indirection with nothing left to share. Pick the policy type directly;
swapping it later is still a one-line `using` change.

## Example

```cpp
using my_runqueue = structo::priority_bucket_runqueue<my_task, &my_task::link,
                                                       &my_task::priority, 32>;

my_runqueue rq;
my_task t1{{}, /*priority=*/5};
my_task t2{{}, /*priority=*/0};
rq.enqueue(t1);
rq.enqueue(t2);

while (my_task *next = rq.dequeue()) {
  // `t2` first (priority 0), then `t1` (priority 5)
}
```

See [`async_kernel_object.md`](async_kernel_object.md)/
[`callout.md`](callout.md) for a potential consumer: a future
`callout_subsystem` can use any of these three in place of the
sorted-list toy scheduler in
[`examples/callout_scheduler_demo.cpp`](../examples/callout_scheduler_demo.cpp).
