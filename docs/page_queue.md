<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# page_queue

`include/structo/page_queue.hpp` – a counted, intrusive page queue (active / inactive / free / offline
lists) built over a pluggable handle-based list. The queue owns no memory: links live in the page
descriptors. It has **no internal lock**; the caller holds the queue lock for every call.

## Usage

```cpp
// Descriptor with an intrusive hook (layout required by reloco::c_tailq).
struct vm_page {
  page_link link;               // embedded reloco::c_tailq hook
  // ... other fields
};

// A page_view traits class (see phys_page.md) maps handles to descriptors.
// Here the handle (os_page_type) is vm_page*.
using lru_list  = structo::tailq_page_list<vm_page, &vm_page::link>;
using page_q    = structo::page_queue<lru_list, my_page_view>;

page_q active;                  // empty queue; size() is O(1)

active.push_back(p);            // enqueue at the tail (most recently used)
active.move_to_back(p);         // LRU "touch": unlink + push_back, size unchanged
active.remove(p);               // unlink a page that is known to be on this queue
auto victim = active.pop_front();   // reloco::optional<handle>; oldest page

for (auto h : active) { /* read-only walk; markers are skipped */ }
```

The `List` contract (implement your own for other descriptor layouts): `clear`, `front() -> optional`,
`next(h) -> optional`, `push_front`, `push_back`, `insert_after(pos, n)`, `insert_before(pos, n)`,
`remove`, `pop_front() -> optional`. Optionally `splice_back(List&)` / `splice_front(List&)`;
if present they are used (O(1)), otherwise splicing moves node by node and keeps order.

## Splicing

```cpp
page_q offline;                 // private queue built without the global lock
// ... fill `offline` ...
// Under the destination's lock: move everything, order preserved, `offline` becomes empty.
// Counts are adjusted on both queues.
free_q.splice_back(offline);    // append
free_q.splice_front(offline);   // prepend
```

Never splice a queue that has a linked scan marker; the node-by-node path asserts, the native path
cannot detect it.

## Integration

**Page daemon** – `scan_ops()` yields the `Ops` that `page_queue_scan` needs (see
[page_queue_scan.md](page_queue_scan.md)). Markers are linked through it only, are skipped by
iteration and are **not** counted in `size()`:

```cpp
auto ops = active.scan_ops();                    // adapter over this queue
structo::page_queue_scan scan(ops, queue_lock, marker_handle);
for (auto p : scan) {                            // lock dropped inside the body
  if (should_demote(p))
    scan.locked([&] { active.remove(p); inactive.push_back(p); });
}
```

**Memory hotplug / decay** – collect pages claimed by `buddy_allocator::claim_range` into a private
`page_q`, then `splice_back` them into the offline queue under its lock in one step
(see [memory_hotplug.md](memory_hotplug.md), [page_decay.md](page_decay.md)).

**Markers** – a marker descriptor is only probed (`is_marker()`) and linked via `scan_ops()`. Passing
one to any other `page_queue` operation is a fatal `RELOCO_ASSERT`.
