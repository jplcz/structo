<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# Memory window walker: offlining memory one window at a time

Header: `structo/memory_window.hpp`.

Offlining a segment (see [memory_hotplug.md](memory_hotplug.md)) is slow: pages must be isolated, free
blocks claimed, in-use pages migrated. Doing the whole segment at once would starve the system of memory,
so the disconnector works on a **window** (for example 32 MiB) at a time. `page_window_walker` produces those
windows from a segment's page-descriptor array, and every access to a descriptor goes through a bounds-checked
`reloco::span` (no raw pointers, no manual index arithmetic).

## Types

- `page_window_walker<Page>`: a `reloco::iterator_adaptor` yielding one `page_window<Page>` per step. It is
  usable in a range-for and composes with `.take(n)`, `.map(f)`, ... from `reloco/iterator.hpp`.
- `page_window<Page>`: PFN range `[first_pfn(), end_pfn())` plus the descriptor `pages()` span for it, with
  `contains(pfn)`, `index_of(pfn)`, `page_at(pfn)` (empty when outside), `range()` (a `pfn_range`), `walk()`,
  `walk(range)` (only a sub-range) and `pending(done)` (the sub-ranges not covered by `done`).
- `window_page_iterator<Page>` (from `walk()`): yields `window_page<Page>` = `{pfn, page()}` for every
  descriptor of the window.

Window boundaries are aligned to multiples of `window_pages` in **PFN space**, not relative to the segment
start. Windows therefore line up with buddy blocks and page blocks, and `allocate_constrained` windows never
straddle an aligned block. Only the first and last window of a segment can be shorter than `window_pages`.

## Basic use

```cpp
#include <structo/memory_window.hpp>

// 'seg' is a segment of the segment map (first PFN, page count, tag, descriptor span), copied out of a
// read guard as shown in memory_hotplug.md. 8192 pages is 32 MiB with 4 KiB pages. It should be a
// multiple of the buddy allocator's largest block so a window never splits one.
structo::page_window_walker<page> walker(seg, 8192);

// Range-for: each element is one window. Only this window is being offlined; the rest of the
// segment keeps serving allocations.
for (auto &w : walker) {
  // PFN range of the window: [first, end). Use it for allocate_constrained limits and for isolation.
  isolate_range(w.first_pfn(), w.end_pfn());

  // Every descriptor of the window together with its PFN; the span is bounds-checked, so a bug cannot
  // touch a neighbouring segment's descriptors.
  for (auto &wp : w.walk()) {
    if (wp.page().flags & PG_FREE) claim_free_page(wp.pfn);
    else                           queue_for_migration(wp.page());
  }

  // Direct access by PFN (e.g. from an I/O completion): empty when the PFN is outside this window.
  if (auto p = w.page_at(some_pfn)) p->get().flags |= PG_RETRY;
}
```

The walker can also be driven by hand, which is useful for a cancellable background task that must resume
after each retry pass:

```cpp
// A pull iterator: next() returns an empty optional once the segment is finished, and keeps doing so.
if (auto w = walker.next()) {
  process(*w);
}
// PFN where the next window starts (the "walk mark"). Persist it to resume after a pause.
std::uint64_t mark = walker.next_pfn();
```

To resume, rebuild the walker over the remaining part of the descriptor array:
`page_window_walker<page>(mark, seg.pages.subspan(mark - seg.first.value), window_pages)`.

## Integration with other subsystems

**Segment map and hotplug.** Build the walker from a `memory_segment_map::segment` copied out of a read
guard. The disconnector only needs the descriptor span, which stays valid until `memory_hotplug::remove()`
runs its quiesce callback; do not keep the guard while migrating. See the removal sequence in
[memory_hotplug.md](memory_hotplug.md).

**Buddy allocator.** Drain the window with `buddy_allocator::claim_range` and skip what it took with
`pfn_range_log` + `pending()`:

```cpp
// 'done' remembers PFN runs that are already dealt with; touching runs merge, so a few entries suffice.
// It does not own memory: give it storage that is not on the kernel stack (a member of the offline job
// object, a static, or an early allocation). 64 entries = 1 KiB; add() returns false when full and that
// run is just revisited by the walker.
structo::pfn_range_log done{reloco::span<structo::pfn_range>(job.run_storage)};
// The sink must not free into the buddy (the pages would be claimed again); recording runs is fine.
// claim_range(low, high_inclusive, sink): takes every page that is free right now; never fails.
g_buddy.claim_range(w.first_pfn(), w.end_pfn() - 1, [&](page_t first, std::size_t count) {
  done.add(structo::pfn_range::from_count(first.pfn(), count));                       // remember the run
  disconnector_list.push(first, count);                                               // now owned by us
});

// Only inspect pages that were NOT claimed: pending() yields the window minus 'done', walk(r) visits them.
auto todo = w.pending(done.runs());
for (auto r : todo) for (auto &wp : w.walk(r)) migrate_or_retry(wp.page());
```

Opportunistic frees later (a retry pass that finds more free pages) call `claim_range` again over the same
window and `done.add()` the new runs; the next `pending()` shrinks accordingly. Wrap ranges in
[`page_index_range`](page_index_range.md) operations (`subtract`, `merge`) for anything more elaborate. A virtio
balloon can use the same call with the `max_pages` argument to take exactly N pages.

**Page cache and mappings.** Migration of each in-use page happens under the page cache / address-space locks as
described in the hotplug guide; the walker only tells you which pages to visit. A retry pass is another
`w.walk()` over the same window, so keep the `page_window` while retrying and drop it when the window is empty.

**Page daemon (decay).** Isolated pages must be skipped by the daemon (see [page_decay.md](page_decay.md)).
Set the isolation flag on the whole window first (`for (auto &wp : w.walk()) ...`) before claiming free blocks,
so a concurrent free cannot put a page back on a free list.

**Memory accounting.** Adjust totals and watermarks per finished window if you want free-memory accounting to
follow the offlining progress, or once for the whole segment before unpublishing (see
[hotplug_decay_integration.md](hotplug_decay_integration.md)).

**Rollback on abort.** Windows already finished hold pages on the private list with the isolation flag set.
Walk the finished part again (a second walker over `[seg.first, mark)`) to clear the flag and return the pages
to the allocator with `free_n`.

## Notes

- Windows are by value and cheap: a PFN plus one span. Copy them freely.
- `window_pages == 0` means a single window covering the whole segment.
- The walker never allocates and holds no locks; it is safe in a kernel thread that sleeps between windows,
  provided the descriptor array stays alive (it does until `remove()` quiesces).
