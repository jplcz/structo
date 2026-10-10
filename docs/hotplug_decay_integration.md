<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# Integrating segment-map hotplug and page decay into a cache-centric VM

This guide targets operating systems whose virtual memory subsystem looks like this:

- One global **page array** indexed by page frame number, built at boot from the firmware memory map.
- Each page has an explicit **state** (`free`, `clear` (zeroed), `active`, `inactive`, `modified`,
  `cached`, `wired`, `unused`) and sits on at most one global **queue** protected by a queue lock.
- Pages that hold data belong to a **cache object** (file, anonymous region, device) with its own lock;
  the object owns the page-to-offset mapping.
- Every page keeps a list of its **mappings** (reverse mappings) in address spaces.
- A **page daemon** ages mapped pages with an access counter and moves them between queues.
- Free pages are handed out through a **reservation** layer (callers reserve N pages first, then take them).

It builds on [memory_hotplug.md](memory_hotplug.md) (segment map, grace period, removal protocol) and
[page_decay.md](page_decay.md) (aging policy). Read those first; this document only shows how the pieces
map onto such a VM.

## 1. Replace the flat page array with the segment map

The flat array becomes a `fixed_memory_segment_map` with one segment per firmware RAM range. Because
the page descriptor in such systems already stores its physical page number, `page_to_pfn` needs no scan.

```cpp
#include <structo/memory_hotplug.hpp>
#include <structo/memory_segment_lookup.hpp>
#include <structo/page_decay.hpp>

struct page {
  std::uint64_t physical_page_number;   // stored: page_to_pfn becomes a field read
  std::uint8_t state;                   // free, clear, active, inactive, modified, cached, wired, unused
  std::uint8_t flags;                   // busy, isolated, referenced, ...
  std::uint16_t age;                    // decay history (see page_decay.hpp), 4 bits used
  // queue links, owning cache object, offset in the cache, mapping list, wire count ...
};

// Segment tag: NUMA node and whether the range can ever be removed (firmware-reported hotpluggable).
struct range_tag { std::uint8_t node; bool removable; };

using ranges = structo::fixed_memory_segment_map<page, range_tag, 16, 128>;
inline structo::memory_hotplug<ranges, hotplug_traits> g_ranges;   // traits: see snapshot_domain.hpp

struct provider { static auto &hotplug() noexcept { return g_ranges; } };
struct pfn_hook { static std::uint64_t pfn(const page &p) noexcept { return p.physical_page_number; } };
using mm = structo::memory_segment_lookup<structo::hotplug_segment_source<provider>, pfn_hook>;

// The names the rest of the VM already uses keep their meaning; only their implementation changes.
inline page *lookup_page(std::uint64_t pfn) { return mm::pfn_to_page(pfn); }       // caller owns/pins the page
inline std::uint64_t page_number(const page &p) { return *mm::page_to_pfn(p); }    // O(1) with the hook
```

Pinning rules for the old code, unchanged in practice:

- A page you reached through its **cache object** (cache lock held) is pinned: the segment cannot be
  removed while any cache entry refers to it. Use `lookup_page` freely.
- A page on a **queue** (queue lock held) is pinned for the same reason: removal drains queues first.
- A bare physical page number from a device, a dump tool or a debug command is *unpinned*: use
  `mm::with_page` / `mm::try_get_page` and never keep the pointer.
- On systems that never hot-plug, use `static_segment_source` instead; nothing else changes.

## 2. Page states and where decay fits

Decay only applies to pages that are **mapped and in use**: states `active` (recently used) and `inactive`
(candidate for reclaim). It does not touch `free`, `clear`, `wired`, `unused` or pages being written back.

```cpp
using decay = structo::page_decay<4>;

// The daemon visits pages from the active queue under the queue lock, but it needs the cache lock to
// change a page's mapping state. Use try-lock: a busy cache is skipped this round (same rule the
// disconnector in section 4 follows, so the two never deadlock).
void age_one_page(page &p) {
  cache_object *c = p.cache;
  if (!c || !try_lock(c->lock)) return;                     // page is being used elsewhere: skip

  if (page_is_busy(p) || (p.flags & PAGE_ISOLATED)) { unlock(c->lock); return; }

  // Harvest: test-and-clear the access bit in every mapping of the page; unmapped cache pages
  // (file I/O path) set a software "referenced" flag instead. Either one counts as "accessed".
  bool accessed = harvest_and_clear_accessed(p);

  auto r = decay::step(static_cast<std::uint8_t>(p.age), accessed);
  p.age = r.age;                                            // cache lock protects the field

  if (r.action == structo::decay_action::demote) {
    // Demotion keeps the mappings (a later access just sets the bit again); the page moves from
    // the active to the inactive queue. Reclaim later unmaps it and, if modified, writes it back.
    queue_move(p, state::inactive);                         // takes the queue lock briefly
  }
  unlock(c->lock);
}
```

Where the older usage-counter scheme incremented a counter on every access and decremented it per scan,
the history word records *which of the last scans* saw an access. Pages with bursty use are not reclaimed
just because the counter happened to reach zero between two bursts. If you want to keep boosting pages on
explicit access, call `decay::touch(age)` from the access path; it only sets the newest bit.

Promotion (a mapped `inactive` page being accessed again, found by the daemon or by a fault) sets
`age = decay::fresh()` and moves the page to the `active` queue.

Run the scan size through `decay_scan_count` using the node's free-page count (from the reservation
layer) and its low/high watermarks (the same numbers the low-memory handlers use).

## 3. Hot-add

New RAM is cheap to add because existing page pointers never move.

```cpp
// firmware/hypervisor has made [first_pfn, first_pfn + count) usable and the kernel's physical map covers it.
status memory_add(std::uint64_t first_pfn, std::uint64_t count, range_tag tag) {
  // 1. Descriptor array, initialized to "unused": nobody may allocate these pages yet. It can live
  //    at the start of the new memory itself; those pages are then marked wired/unused forever.
  std::span<page> descs = allocate_descriptors(first_pfn, count);
  for (std::uint64_t i = 0; i < descs.size(); ++i) {
    descs[i] = page{};
    descs[i].physical_page_number = first_pfn + i;
    descs[i].state = state::unused;
  }

  // 2. Publish the range: lookups now find it. One atomic store; no waiting for readers.
  auto r = g_ranges.add({ranges::pfn_type{first_pfn}, count, tag, descs});
  if (!r) { free_descriptors(descs); return status_from(r.error()); }

  // 3. Make the pages allocatable: mark them 'free' and put them on the free queue under the queue
  //    lock, then raise the totals and the watermarks the reservation layer and daemon use.
  with_queue_lock([&] {
    for (page &p : descs.subspan(reserved_prefix_pages(descs))) {
      p.state = state::free;
      free_queue_push(p);
    }
    total_pages += count;
    free_pages += count - reserved_prefix_pages(descs);
  });
  recompute_watermarks();            // keeps the daemon's low/high thresholds proportional to RAM
  wake_waiters_for_memory();         // threads blocked in page reservation can proceed
  return ok;
}
```

If your free/clear queues are per size class or per NUMA node, push each page onto the queue selected by
`range_tag::node`.

## 4. Hot-remove: a disconnector that cooperates with the daemon

Removal follows the sliding-window protocol from [memory_hotplug.md](memory_hotplug.md); here is how each
state is handled in this kind of VM. The disconnector works on a window of the range at a time, parks what
it wins on a **private offline queue** and never lets other subsystems reach those pages.

```cpp
status memory_remove(std::uint64_t first_pfn) {
  // Range must be marked removable by the firmware and must not contain wired kernel structures.
  ranges::segment seg = lookup_segment(first_pfn);
  if (!seg.tag.removable) return not_removable;

  offline_queue mine;                                    // private: not a global queue, no other user
  constexpr std::uint64_t window_pages = 8192;

  for (std::uint64_t lo = seg.first.value; lo < seg.end_value(); lo += window_pages) {
    std::uint64_t hi = std::min(lo + window_pages, seg.end_value()) - 1;
    isolate_window(seg, lo, hi);                         // set PAGE_ISOLATED: allocator and daemon skip them
    take_free_pages(seg, lo, hi, mine);                  // pull free/clear pages off the global queues

    while (!window_empty(seg, lo, hi)) {
      for (std::uint64_t pfn = lo; pfn <= hi; ++pfn) {
        page &p = seg.pages[pfn - seg.first.value];
        switch (evacuate(p, mine)) {
        case moved:     break;                           // page now sits on 'mine'
        case retry:     break;                           // busy/locked/in I/O: try next pass
        case permanent: return rollback(seg, mine, lo, hi, busy);   // wired kernel page etc.
        }
      }
      if (cancelled() || deadline_passed()) return rollback(seg, mine, lo, hi, busy);
      sleep_backoff();                                   // do not spin: I/O and pins take time
    }
  }

  // All pages are on 'mine'. Fix the totals first so reservations stop counting this memory, then
  // unpublish and wait for the grace period; only then are descriptors and memory released.
  adjust_totals_for_removal(seg.page_count);
  return g_ranges.remove(seg.first, [&](const ranges::segment &s) {
    free_descriptors(s.pages);
    unmap_physical(s.first.value, s.page_count);
    firmware_offline(s.first.value, s.page_count);
  }) ? ok : busy;
}
```

What `evacuate` does for each page state (always with a *destination page from outside the window*,
obtained through the normal reservation path with the isolated range excluded):

| State | Action |
|---|---|
| `free`, `clear` | Already taken off the global queues by `take_free_pages`; put on `mine`. |
| `cached` (clean, unmapped) | Take the cache lock (try-lock; `retry` if busy), remove the page from the cache, put on `mine`. A later lookup simply re-reads from backing store. |
| `inactive` / `active` (mapped) | Unmap from every address space (walk the mapping list, invalidate TLBs), then treat as `cached`/`modified` below. Faulting threads wait on the cache lock and re-fault into a fresh page. |
| `modified` | Write back (or copy into a destination page and replace it in the cache under the cache lock); `retry` until I/O completes, then as `cached`. |
| `wired` | Wire count > 0 for kernel use or DMA: `retry` if it is a transient wire (I/O in flight), `permanent` if it is a long-term kernel allocation. |
| `unused` | Descriptor-array pages and similar: `permanent` unless they live inside the range being removed (memory-on-memory); then they go last. |

Rules that keep the disconnector and the page daemon from fighting each other:

- **Lock order is cache lock, then queue lock**, same as the daemon, and both use *try-lock* on the cache
  when they are not allowed to sleep, so neither can block the other.
- **`PAGE_ISOLATED` pages are skipped** by the allocator, the reservation layer, the daemon and reclaim.
  A page freed while isolated goes to the disconnector's private queue, not the global free queue.
- **A migrated page keeps its age.** Copy `age` and `state` into the destination page so the working-set
  estimate survives (`dst.age = src.age`). Active pages stay active on the new node.
- **Reservations are accounted before removal.** Reduce `total_pages` and the watermarks before
  unpublishing the range, otherwise the reservation layer may promise memory that is about to disappear.
- **Abort path:** return everything on `mine` to the global free queue (`free_n` on the buddy allocator, or
  `free_queue_push` in a queue-based allocator), clear `PAGE_ISOLATED` on the processed windows, and report
  `busy`.

## 5. Checklist for a port

- [ ] Replace boot-time page array setup with one `memory_hotplug::add` per firmware range.
- [ ] Implement `lookup_page`/`page_number` through `memory_segment_lookup` with a `PfnHook`.
- [ ] Give every descriptor an `age` field and use `decay::step` in the daemon instead of the usage counter.
- [ ] Add `PAGE_ISOLATED` and teach the allocator, reservation layer, daemon and reclaim to skip it.
- [ ] Implement `evacuate` per state, and the private offline queue.
- [ ] Update totals and watermarks on hot-add and hot-remove; wake memory waiters after hot-add.
- [ ] Make sure no code path keeps an unpinned page pointer across a sleep (device lookups, debug tools).
