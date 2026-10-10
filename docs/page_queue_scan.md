<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# page_queue_scan

`structo::page_queue_scan<Ops, Lockable>` (`<structo/page_queue_scan.hpp>`) walks an intrusive page queue
for a **page daemon** while the queue lock is dropped during the loop body. It is a
`reloco::iterator_adaptor` (range-for, `.take()`, `.map()`, ...).

A pass cannot hold the queue lock across the work: the body needs the cache lock, may sleep, and other CPUs
keep adding, removing and re-queueing pages. A normal iterator would end up on a page that was freed or moved.
The scan stores its position in a **marker**, a dummy descriptor linked *into the queue* right after the page
last returned. The marker, not a page, is the cursor, so any page may leave the queue while the lock is dropped.

Each step takes the lock, moves the marker past the next real page and releases the lock before returning the
page handle. The returned page is only a hint: re-take the lock with `locked()` and verify the page is still on
this queue before acting. Leaving the loop in any way (break, return, exception) removes the marker.

## Usage

```cpp
// Ops adapts YOUR intrusive queue. All four functions are called with the queue lock held.
struct active_queue_ops {
  using handle_type = page_handle;                          // copyable page handle (pointer-like or index)
  // Head of the queue skipping marker descriptors (of any scan); nullopt when empty.
  reloco::optional<handle_type> first() noexcept;
  // Page after 'pos' skipping markers; 'pos' can be the scan's own marker. nullopt at the tail.
  reloco::optional<handle_type> next_after(handle_type pos) noexcept;
  // Link / unlink the marker descriptor. Your queue code must treat marker descriptors as "not a page".
  void insert_after(handle_type pos, handle_type marker) noexcept;
  void remove(handle_type marker) noexcept;
};

active_queue_ops ops;                                       // wraps the active queue
queue_lock_t queue_lock;                                    // BasicLockable: lock() / unlock()
page_handle marker = daemon_marker_descriptor();            // allocated once per daemon and queue, never freed

// 4th argument: max pages to visit in this pass (0 = to the tail). Use it to bound passes: a page that is
// re-queued at the tail while being scanned would otherwise be visited again.
std::size_t budget = structo::decay_scan_count(/* ... */);
structo::page_queue_scan scan(ops, queue_lock, marker, budget);

for (auto page : scan) {                                    // queue lock is NOT held in the body
  if (!try_lock_cache_of(page)) continue;                   // other locks are taken here, in lock order
  scan.locked([&] {                                         // queue lock again, to validate and to move
    if (!still_on_active_queue(page)) return;               // freed / isolated / moved meanwhile: skip
    age_or_demote(page);                                    // e.g. page_decay<>::step, then queue_move
  });
  unlock_cache_of(page);
}
// 'scan' going out of scope unlinks the marker; scan.finish() does it earlier.
```

## Markers are probe-only

A marker is not a page. The only valid operation on it is the `is_marker()` probe (`page_view::is_marker()`,
backed by `os_traits::is_marker()`, default `false`; hide it in your traits to recognise your marker
descriptors). Everything else (`pfn()`, `phys()`, `try_add`/`try_sub`/`try_get_buddy`, buddy metadata)
fails a fatal `RELOCO_ASSERT` (a kernel panic under `RELOCO_KERNEL`) because using a marker as a page is
always a kernel bug. Your queue code must probe and skip markers *before* touching a descriptor as a page:
`Ops::first`/`next_after` do exactly that, so the scan never hands one out.

## Several scans on one queue

Any number of scans may be in progress on the same queue at once (for example the active-queue aging daemon
and a reclaim pass, or one scan per CPU/NUMA node worker). Rules:

- **One marker per scan.** Never share a marker descriptor between scans; allocate one per scan object
  (a small per-daemon array is typical).
- **Steps are atomic.** Each step runs fully under the queue lock, so interleaved scans never observe a
  half-moved marker. The scan object itself is single-threaded: do not call one scan from two threads.
- **Scans are invisible to each other.** `Ops::first`/`next_after` skip every marker, so each scan still
  sees every real page. Two scans can have their markers on the same page, and a scan can finish, `break`
  or be destroyed while others continue.
- **Removals are shared.** A page unlinked by one scan's body (or by anyone else) simply disappears for all
  of them; no scan holds a page reference, only its marker.

## Integration

- **Page decay** ([page_decay.md](page_decay.md), [hotplug_decay_integration.md](hotplug_decay_integration.md)):
  pass `decay_scan_count()` as the budget and run `page_decay::step()` per page. Keep the documented lock
  order (cache lock, then queue lock); `locked()` takes only the queue lock.
- **Hotplug / offline** ([memory_hotplug.md](memory_hotplug.md)): the disconnector removes isolated pages from
  queues under the queue lock, which the scan tolerates by design. Re-check the isolation flag in
  `still_on_active_queue`. Marker descriptors must not live in a segment that can be offlined (allocate them
  statically).
- **Several daemons or queues:** see "Several scans on one queue" above; the queue's other users (reclaim,
  isolation, free paths) must also skip marker descriptors.
- **Lock type:** any object with `lock()`/`unlock()`; a try-lock based daemon can pass a wrapper whose
  `lock()` spins or sleeps as your kernel requires.
