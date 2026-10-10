<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# Memory hotplug guide: segment map, page cache and buddy allocator together

Headers: `structo/memory_segment_map.hpp`, `structo/memory_segment_os_traits.hpp`,
`structo/snapshot_domain.hpp`, `structo/memory_hotplug.hpp`, `structo/buddy_allocator.hpp`,
`structo/phys_page.hpp`.

This guide shows how the pieces fit in a general-purpose OS: the physical-memory map that every
subsystem uses to get from a physical address or PFN to its page descriptor (`struct page`), the
buddy allocator that hands pages out, a page cache that keeps pages for a long time, and the two
hotplug operations on top.

## The pieces and who owns what

- **`fixed_memory_segment_map`**: an immutable snapshot "PFN range -> {tag, descriptor array}". It
  never changes; a change builds a new snapshot (copy-on-write). The descriptor arrays are *not*
  copied: a new snapshot points at the same arrays, so a `page *` stays valid across any number of
  map updates for as long as its segment exists.
- **`memory_segment_os_traits`**: makes `page_view` and `buddy_allocator` use the map for
  `from_pfn`/`to_pfn`/`is_same_zone`. Buddy merging and `page_view::try_add` never cross a segment.
- **`snapshot_domain` / `memory_hotplug`**: publish the current map. Readers pin it with a
  `read_guard`; a removal waits until no guard can still see the old map.

### What a read guard protects (and what it does not)

A guard protects a *lookup*: while it lives, the descriptors reachable through the map exist.
It is meant for short sections (PFN -> page, or page -> PFN). It does **not** keep a page alive for
longer, and nothing may sleep on I/O while holding one: a removal waits for every guard.

Long-lived references (page cache entries, buddy free lists, mappings, DMA pins) are not covered by
guards. They are handled by the removal protocol below, which empties the segment of all such
references *before* it is unpublished. Hot-add needs no such protocol.

## Wiring it up

```cpp
#include <structo/buddy_allocator.hpp>
#include <structo/memory_hotplug.hpp>
#include <structo/memory_segment_os_traits.hpp>

using namespace structo;

// The kernel's page descriptor. State bits tell every subsystem who owns the page; next/prev are the
// buddy free-list links (as PFN handles); 'order'/'free' are the buddy bookkeeping.
struct page {
  std::uint64_t next{~0ull};
  std::uint64_t prev{~0ull};
  std::uint16_t order{0};
  bool free{false};
  std::atomic<std::uint32_t> flags{0}; // PG_ISOLATED, PG_CACHE, PG_UNMOVABLE, ...
};

// Per-segment tag, copied by value into every snapshot. Keep it immutable: put things that change
// (like "going offline") in page flags or in your own per-node structure.
struct seg_tag {
  std::uint8_t node;     // NUMA node
  bool removable;        // ZONE_MOVABLE-like: only movable pages are ever placed here
};

// 8 segments, 64 table sections (128 MiB each at 4 KiB pages) => up to 8 GiB of physical span.
using segments = fixed_memory_segment_map<page, seg_tag, 8, 64>;

// Traits for the domain (kernel mutex, per-CPU shards, futex-style wait/wake for the remover):
//   shards            number of reader counters (power of two), spreads cache-line traffic
//   current_shard()   e.g. current CPU number
//   mutex_type        sleeping mutex that serializes add/remove
//   wait()/wake_all() block the remover until the last reader leaves
struct hotplug_traits { /* see snapshot_domain.hpp */ };

// The one global published map.
inline memory_hotplug<segments, hotplug_traits> g_hotplug;

// Provider for the OS traits: "which snapshot do I consult". Every call re-reads the published one.
struct current_segments {
  static segments::map_type map() noexcept;  // returns the snapshot of g_hotplug (see below)
};

// OS traits: handle = PFN (compressed), descriptor access through the segment map.
// deref() comes from the base class.
struct os_traits : memory_segment_os_traits<os_traits, current_segments, page, std::uint64_t> {
  static std::uint16_t buddy_order(os_page_type p) noexcept { return deref(p).order; }
  static void set_buddy_order(os_page_type p, std::uint16_t o) noexcept { deref(p).order = o; }
  static bool is_buddy_free(os_page_type p) noexcept { return deref(p).free; }
  static void set_buddy_free(os_page_type p, bool f) noexcept { deref(p).free = f; }
};
using page_t = page_view<page_4k, os_traits>;
```

`current_segments::map()` must return the snapshot of the published map. `memory_hotplug` hands out
snapshots through `read()`; for the buddy path, which does many tiny lookups inside its own lock,
the simple choice is for the allocator-side lock holder to take one guard for the whole operation
(see "Allocation path" below) and have the provider return that guard's map from a per-CPU slot.
Pick whichever your kernel prefers; the contract is only that the returned snapshot stays valid
for the whole buddy call.

## Boot: building the first map

```cpp
// At boot the memory map comes from the bootloader (boot_memory_map / FDT / UEFI).
// For every usable RAM range allocate its descriptor array (from early memory), initialize it
// to "reserved, not free", and publish the segment. Nothing needs to wait: no reader exists yet.
void boot_add_ram(std::uint64_t first_pfn, std::uint64_t count, seg_tag tag) {
  std::span<page> descs = early_alloc_descriptors(count);   // zero-initialized: flags=0, free=false
  (void)g_hotplug.add({segments::pfn_type{first_pfn}, count, tag, descs});
}

// Then give the usable pages to the buddy allocator. init() is for the very first range only:
// it clears all free lists.
```

## Allocation path (buddy)

The buddy allocator works on `page_t` handles. `os_traits` resolves handles to descriptors through
the map; `is_same_zone` keeps blocks inside one segment.

```cpp
using buddy_t = buddy_allocator<pfn_free_list, page_t, 10>;
buddy_t g_buddy;

// First range: init() carves it into maximal aligned blocks and clears the lists.
auto first = os_traits::from_pfn(first_pfn);                     // handle for the first page
(void)g_buddy.init(page_t::from_os_page(*first), count);

// Normal operation: allocate/free pages, page math through page_t.
auto blk = g_buddy.allocate(2);                                   // 4 pages
if (blk) {
  std::uint64_t pfn = blk->pfn();                                 // PFN
  auto pa = blk->phys();                                          // physical address
  auto next = blk->try_add(1);                                    // page math; fails at the segment end
  g_buddy.free(*blk, 2);
}
```

Rule: take a read guard (or ensure the provider holds one) around each buddy call, and never keep
a `page *` or a guard beyond what you actually own. Pages you *own* (allocated, referenced by the page
cache, pinned) stay valid because the removal protocol will not unpublish their segment while you
own them.

## Hot-add (cheap)

Existing descriptor pointers are untouched by hot-add, so the sequence is short and needs no
draining.

```cpp
// firmware/hypervisor has already made [first_pfn, first_pfn + count) usable and the direct map covers it.
void memory_hot_add(std::uint64_t first_pfn, std::uint64_t count, seg_tag tag) {
  // 1. Descriptors first. They may live in the new memory itself ("memmap on memory"):
  //    place the array at the start of the range and treat those pages as reserved.
  std::span<page> descs = alloc_descriptors_for(first_pfn, count);
  for (page &p : descs) { p.flags.store(PG_RESERVED); p.free = false; p.order = 0; }

  // 2. Publish. One atomic store: readers see none of the segment or all of it, never a half state.
  //    check_insert() inside add() rejects overlap, a shared 128 MiB section, or no table room.
  auto r = g_hotplug.add({segments::pfn_type{first_pfn}, count, tag, descs});
  if (!r) { free_descriptors(descs); return; }

  // 3. Hand the usable pages to the buddy allocator. init() is not for this (it clears the lists);
  //    free_n() "frees" an arbitrary range into maximal aligned blocks. It never merges across
  //    segments, so a hot-added segment adjacent to an old one stays separate: expected.
  auto start = os_traits::from_pfn(first_pfn + reserved_prefix_pages);
  g_buddy.free_n(page_t::from_os_page(*start), count - reserved_prefix_pages);

  // 4. Update watermarks/per-node counters and notify subscribers.
}
```

## Hot-remove (the expensive direction)

Removing a segment means: stop using it, move everything out of it, prove that nothing can still
reach it, then free it. Every subsystem that keeps long-lived page references takes part.

Migration takes time (dirty page write-back, pinned pages that unpin later, a busy system), so the
disconnector never works on the whole segment at once. It walks the segment with a **sliding
window** (for example 32 MiB, or one buddy `MaxOrder` block): only pages inside the window are
isolated and migrated, so the rest of the segment keeps serving allocations and the system is not
starved of memory while the disconnect runs.

Everything the disconnector wins goes to its **private list** (a `pfn_free_list` owned by the
disconnector, not linked into the buddy). Pages parked there are not free, not in the page cache
and not mapped: nobody else can reach them. When the window is fully in the private list it slides
on. Only when the whole segment is in the private list is it unpublished.

1. **Slide**: pick the next window; set `PG_ISOLATED` on its pages (allocator free path diverts them).
2. **Claim free pages**: pull the window's free blocks out of the buddy into the private list.
3. **Migrate** the in-use pages of the window; each old page, once emptied, goes to the private list.
   This step is slow and retryable: transient failures (page locked, writeback, extra reference)
   retry the window later; only permanent ones (slab, pinned DMA, kernel stack) abort.
4. Repeat until all windows are done, then **unpublish and wait** (`remove()`).
5. **Release**: free the descriptor array and unmap/return the memory.

On abort or timeout, walk the private list back with `free_n()`, clear `PG_ISOLATED` on the
windows done so far, and return `busy`: the memory returns to normal use. Because the work is
incremental, the disconnector should be a cancellable background task, not a syscall that blocks
the caller for the whole time.

```cpp
std::error_code memory_hot_remove(std::uint64_t first_pfn) {
  // Find the segment (short lookup under a guard; copy what you need, do not keep the guard).
  segments::segment seg;
  {
    auto g = g_hotplug.read();
    auto h = g->find(segments::pfn_type{first_pfn});
    if (!h) return not_found;
    seg = g->segment_at(*h);                 // {first, page_count, tag, descriptor span}
  }
  if (!seg.tag.removable) return not_removable;

  // The disconnector's private list: pages that were won from the window. They are neither free
  // nor owned by anyone else, so no subsystem can touch them. (Same list type as the buddy uses.)
  pfn_free_list mine;
  constexpr std::uint64_t window_pages = 8192;                      // 32 MiB at 4 KiB pages

  // Slide a window over the segment. Only the window is isolated at a time, so the rest of the
  // segment keeps serving allocations while this (slow) loop runs.
  for (std::uint64_t lo = seg.first.value; lo < seg.end_value(); lo += window_pages) {
    const std::uint64_t hi = std::min(lo + window_pages, seg.end_value()) - 1;   // inclusive

    // 1. Isolate the window: the allocator free path must divert PG_ISOLATED pages (and never
    //    put them back on a free list); they end up on 'mine' instead.
    for (std::uint64_t pfn = lo; pfn <= hi; ++pfn)
      seg.pages[pfn - seg.first.value].flags.fetch_or(PG_ISOLATED);

    // 2. Claim the free blocks of the window out of the buddy, largest first, into 'mine'.
    for (std::size_t order = 10; ; --order) {
      buddy_t::physical_constraint c{lo, hi, std::uint64_t{1} << order, 0};
      while (auto b = g_buddy.allocate_constrained(std::size_t{1} << order, c))
        mine.push_back(b->get_os_page());
      if (order == 0) break;
    }

    // 3. Migrate what is still in use. This is the slow part: dirty pages need write-back, a page may
    //    be locked or temporarily pinned. Retry the pass with back-off until the window is empty or
    //    the deadline passes; cancel/timeout rolls everything back.
    while (!window_empty(seg, lo, hi)) {
      for (std::uint64_t pfn = lo; pfn <= hi; ++pfn) {
        page &p = seg.pages[pfn - seg.first.value];
        switch (migrate_page(p)) {                                  // see below
        case migrate_result::moved:     mine.push_back(os_handle(p)); break;  // old page is empty now
        case migrate_result::retry:     break;                      // look again next pass
        case migrate_result::permanent: rollback(seg, mine, lo, hi); return busy;
        }
      }
      if (cancelled() || deadline_passed()) { rollback(seg, mine, lo, hi); return busy; }
      sleep_backoff();
    }
  }
  // Every page of the segment is now on 'mine'.

  // 4. Unpublish and wait for readers. The lambda runs only after every read guard that could have
  //    seen the old map has been released.
  auto r = g_hotplug.remove(seg.first, [&mine](const segments::segment &s) {
    // 5. Nothing can hold a descriptor pointer into 's' any more: release the memory.
    mine.clear();                                                   // drop the private list first
    free_descriptors(s.pages);
    unmap_direct_map(s.first.value, s.page_count);
    firmware_offline(s.first.value, s.page_count);
  });
  return r ? ok : busy;
}
```

### Migrating a page: what each owner has to do

`migrate_page(p)` classifies the page by its state and acts, always against a destination page
taken from *outside* the segment (allocate with the buddy, constrained to other PFNs):

```cpp
// Returns moved (page is empty, may go to the private list), retry (try again later) or permanent.
migrate_result migrate_page(page &p) {
  switch (page_state(p)) {
  case state::free:
  case state::in_private_list:
    return migrate_result::moved;                   // nothing to do / already ours

  case state::page_cache: {
    // Clean page: simply drop it from the cache (it can be re-read). Dirty: write back first, or
    // copy into a new page. Either way the cache entry for (inode, index) is replaced under the
    // cache's own lock, so concurrent lookups see either the old or the new page, never neither.
    page *dst = alloc_outside_window();             // never from the isolated window/segment
    if (!dst) return migrate_result::retry;         // memory pressure: back off
    copy_page(dst, &p);
    if (!page_cache_replace(p, *dst)) {             // locked, under writeback, extra reference...
      free_page(dst);
      return migrate_result::retry;
    }
    return migrate_result::moved;
  }

  case state::anonymous:
    // Unmap all mappings (rmap), copy, remap to the new page, then drop the old one.
    return migrate_anonymous(p);                    // moved or retry

  case state::unmovable:                            // slab, pinned for DMA, kernel stacks, ...
    return migrate_result::permanent;               // the whole removal fails with busy
  }
  return migrate_result::permanent;
}
```

Why this is safe with guards: a thread doing a page cache lookup holds a read guard only while it
resolves PFN to `page *`, then takes a reference or locks the cache entry. Migration replaces the
entry under the cache lock, so a late lookup either finds the old page *before* the replacement
(and is then handled by the entry's own locking/refcount) or the new one *after*. Once every page of
the segment has been migrated, no cache entry, mapping or list refers to it, and `remove()` only has
to wait for transient lookups, which is what the grace period covers.

### Checklist for the isolation flag

- The page allocator front end (`allocate*`) must not return a page of an isolated window: window
  free blocks are claimed (step 2) and `free()` diverts `PG_ISOLATED` pages to the disconnector's
  private list instead of the buddy lists.
- The page cache must not insert *new* pages from isolated segments: allocate from the buddy, which no
  longer returns them, so nothing extra is needed there beyond the free-path check.
- Kernel code that obtains pages outside the buddy (boot-time reservations, device memory) must be
  excluded from removable segments; mark such pages `PG_UNMOVABLE` at boot so migration fails early.

## Writing `pfn_to_page`, `phys_to_page`, `page_to_pfn`

`structo/memory_segment_lookup.hpp` ships these as static functions of
`memory_segment_lookup<Source>` (`pfn_valid`, `pfn_to_page`, `phys_to_page`, `page_to_pfn`,
`page_to_phys`, `tag_of`, `segment_of`, `with_page`, `try_get_page`, `for_each_page`). Plug them into the
OS with one-liners; the hand-written versions below show what they do.

```cpp
// Provider: which published map to use. hotplug_segment_source reads it through memory_hotplug;
// static_segment_source (Provider::map() by value) is the no-hotplug variant.
struct provider { static auto &hotplug() noexcept { return g_hotplug; } };
using mm = structo::memory_segment_lookup<structo::hotplug_segment_source<provider>>;

inline page *pfn_to_page(std::uint64_t pfn) { return mm::pfn_to_page(pfn); }   // pinned callers only

// page_to_pfn scans the segments by address (O(segments), <= 32/64) when the descriptor does not know
// its PFN. If your struct page stores the PFN (or a segment id + index), pass a hook instead: no guard,
// no scan, but valid only for pages the caller really owns.
struct pfn_hook { static std::uint64_t pfn(const page &p) noexcept { return p.pfn; } };
using mm_fast = structo::memory_segment_lookup<structo::hotplug_segment_source<provider>, pfn_hook>;
```

Without hotplug these are a flat array index. With a segment map they are a lookup that can fail
(a hole, or memory that was just removed), and the returned `page *` is only as stable as the
segment. Provide two tiers of helpers and be explicit about which one a caller may use.

```cpp
// Tier 1: "pinned" helpers. The caller ALREADY owns something that keeps the segment published:
// a reference on a page of that segment, the page cache lock of an entry in it, an allocation
// from the buddy, or a mapping the disconnector must tear down first. The segment cannot be
// removed while that holds, so a short guard is enough to resolve, and the result stays valid
// afterwards. (The guard is needed only to read the snapshot consistently, not to keep the page.)
page *pfn_to_page_pinned(std::uint64_t pfn) {
  auto g = g_hotplug.read();                                 // pins the snapshot for this lookup
  auto h = g->find(segments::pfn_type{pfn});                 // O(1) table lookup + range check
  return h ? &g->page_at(*h) : nullptr;                      // nullptr: hole / not present
}

page *phys_to_page_pinned(std::uint64_t phys) {
  auto g = g_hotplug.read();
  auto h = g->find(segments::phys_type{phys});               // same, from a physical address
  return h ? &g->page_at(*h) : nullptr;
}

// Reverse direction needs no guard of its own if you already hold the page (it is pinned):
// the page's segment cannot disappear, so only the snapshot read is needed.
std::uint64_t page_to_pfn_pinned(const page &p) {
  auto g = g_hotplug.read();
  auto h = g->find_page(p);                                  // O(segments); <= 32/64, so cheap
  return h ? g->pfn_of(*h).value : ~0ull;
}

// Tier 2: "unpinned" helpers for code that only has a number (a PFN from a device, /proc/kpageflags,
// a crash dump walker, memory-failure handler). The page may vanish the moment the guard is gone,
// so the work happens INSIDE the callback and the pointer never escapes. Never block in 'fn'.
template <class Fn>
bool with_page(std::uint64_t pfn, Fn &&fn) {
  auto g = g_hotplug.read();
  auto h = g->find(segments::pfn_type{pfn});
  if (!h) return false;
  fn(g->page_at(*h), g->tag_at(*h));                         // e.g. read flags, try-get a reference
  return true;
}
```

The usual pattern for unpinned code is `with_page` + *try-get a reference* inside the callback
(`page_try_get(p)`). After a successful try-get the page is pinned (tier 1) and the disconnector
will see the extra reference and report `retry` for it, so the pointer is safe to use after the
guard is dropped. If try-get fails (free page, isolated, being migrated) treat it as "not there".

Which helper to use:

- **Page cache / mappings / buddy / slab code** (owns a reference or lock): tier 1. These are the
  hot paths; the guard is a sharded-counter increment, no atomics on shared lines beyond that.
- **Anything starting from a bare number**: tier 2 and take a reference before leaving the guard.
- **Pages already in the page cache** need nothing special. The cache entry's lock is held while
  the disconnector replaces the page (`page_cache_replace`), so a holder of that lock sees either
  the old page or the new one, never a page that is mid-migration or gone. Resolve once under the
  lock and use the result; do not cache the `page *` past the lock.
- **Loops over many pages** (`for pfn in range`): take one guard per loop iteration batch, not per
  page, and never across blocking work; or re-resolve from the segment `pages` span you got from
  one `segment_at()` call, which is plain array indexing.

Fast path for code that knows its segment (the buddy allocator and `page_view` page math):
resolve the segment once, then index directly.

```cpp
// Walk consecutive PFNs inside one segment: one lookup, then pointer-free array indexing.
{
  auto g = g_hotplug.read();
  if (auto h = g->find(segments::pfn_type{start})) {
    const auto &seg = g->segment_at(*h);
    for (std::uint64_t i = h->page_index; i < seg.page_count; ++i)
      touch(seg.pages[i]);                                   // 'pages' is a span, bounds checked
  }
}
```

If the system is *not* hotplug-aware (fixed RAM), skip `memory_hotplug` and keep a plain
`fixed_memory_segment_map` in a global: `find()`/`page_at()` need no guard at all and tier 1 and
tier 2 collapse into the same function. The map is immutable, so reads are free of synchronization.

## Systems without runtime hotplug (fixed RAM)

If memory never changes after boot, drop the hotplug machinery entirely: no `snapshot_domain`, no
`memory_hotplug`, no traits for shards/mutex/wait, no second map copy, no guards. The map is built
once at boot and is immutable afterwards, so any CPU can read it without synchronization.

```cpp
#include <structo/memory_segment_lookup.hpp>
#include <structo/memory_segment_os_traits.hpp>

using segments = structo::fixed_memory_segment_map<page, seg_tag, 8, 64>;

// The one global map. Written only during boot, before other CPUs or threads can read it.
inline segments g_map;

// Boot, single CPU: add every usable RAM range from the bootloader memory map. Each step builds a
// new map from the old one (copy-on-write); check_insert() rejects overlaps and shared sections.
bool boot_add_ram(std::uint64_t first_pfn, std::uint64_t count, seg_tag tag) {
  std::span<page> descs = early_alloc_descriptors(count);        // from early boot memory, never freed
  auto next = g_map.check_insert({segments::pfn_type{first_pfn}, count, tag, descs});
  if (!next) return false;
  auto committed = g_map.commit(*next);
  if (!committed) return false;
  g_map = *committed;
  return true;
}
// Start the other CPUs only after the last boot_add_ram(): the thread-start barrier publishes g_map.

// Provider: view() is a cheap non-owning snapshot over g_map's storage (two spans), so lookups do not
// copy the arrays. Valid for the lifetime of g_map, i.e. forever.
struct provider { static auto map() noexcept { return g_map.view(); } };

// Same helper API as the hotplug case, only the Source differs. The "guard" is a trivial holder.
using mm = structo::memory_segment_lookup<structo::static_segment_source<provider>>;

// Buddy / page_view integration is identical; the same provider works as the OS traits Provider.
struct os_traits : structo::memory_segment_os_traits<os_traits, provider, page, std::uint64_t> { /* ... */ };
```

What changes compared to the hotplug guide:

- **No removal protocol.** There is no isolate/migrate/drain sequence, and descriptors are never
  freed, so every `page *` stays valid forever. The pinned/unpinned distinction disappears:
  `pfn_to_page`, `with_page` and `try_get_page` all behave the same, and you may cache descriptor
  pointers anywhere (page cache, mappings, DMA lists) without any pinning rule.
- **Page cache and buddy allocator** need nothing extra. Boot: `buddy.init()` for the first range and
  `free_n()` for the rest; `is_same_zone` still keeps blocks inside one segment.
- **Hot-add later?** If you may need it on some configurations, keep the code written against
  `memory_segment_lookup<Source>` and change only the Source (`static_segment_source` ->
  `hotplug_segment_source`). Nothing else in the OS has to change; only pointer-lifetime rules
  tighten (see the pinning rules above).
- **Table size** still depends on the physical span (2 bytes per 128 MiB by default); size
  `MaxSections` for the highest RAM address. With few large banks, a bigger `SectionShift` shrinks it.
- Pass a `PfnHook` if `struct page` already stores its PFN, to skip the segment scan in `page_to_pfn`.

## If the OS already serializes add/remove

Most kernels run memory add and remove under one global hotplug lock (it also guards the totals,
watermarks and zone bookkeeping). The domain's own writer mutex is then redundant: set
`using mutex_type = structo::no_writer_lock;` in the traits and take your lock around every `add()`,
`remove()` and `synchronize()`. Readers never use the mutex, so lookups are unaffected.

```cpp
struct hotplug_traits {
  // ...shards, current_shard(), wait(), wake_all() as before...
  using mutex_type = structo::no_writer_lock;   // no-op: the OS lock below is the only serialization
};

status memory_add(...) {
  hotplug_lock_guard g(g_memory_hotplug_lock);  // the single global add/remove lock
  // init descriptors, g_hotplug.add(...), publish to the allocator, update totals
}
```

Do not call two writers concurrently without that lock; there is no fallback.

## Things to keep in mind

- **Table size:** the section table grows with the physical span between the lowest and highest
  segment (2 bytes per 128 MiB by default). Configure `MaxSections` for the span, not for the amount
  of RAM, or choose a larger `SectionShift`.
- **No two segments share a section** (default 128 MiB): hot-add at section-aligned boundaries.
- **Two copies:** `memory_hotplug` keeps two maps (current and previous), each with its own table.
- **Do not hold a guard across blocking work.** A removal waits for every guard; a guard held during
  a disk read stalls the removal (and any later hot-add, which waits to reuse the previous slot).
- **Updates are serialized** by the domain mutex. Do not call `add()`/`remove()` from a context that
  holds a read guard.
- `buddy_allocator::init()` clears all free lists. Use it once at boot and `free_n()` for everything
  added later.

See also [hotplug_decay_integration.md](hotplug_decay_integration.md) for a queue-based, cache-centric VM.
