<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# Page decay guide: ACTIVE -> INACTIVE aging in a page daemon

Header: `structo/page_decay.hpp` (pure `constexpr` policy; no lists, locks or page-table access).

This guide shows how to wire the policy into a page daemon together with the page cache, the buddy
allocator and memory hotplug (see [memory_hotplug.md](memory_hotplug.md)).

## The model in one paragraph

Every ACTIVE page carries a small history word (`Bits` bits, default 4). Each daemon scan shifts it
right and puts "was the page accessed since the last scan" into the top bit. History 0 means the page
was idle for `Bits` consecutive scans: demote it to INACTIVE. INACTIVE pages that are accessed again are
promoted back with a full history; INACTIVE pages that stay idle are reclaimed from the list tail. How
many pages a scan looks at depends on memory pressure and on how small the INACTIVE list is.

## 1. Page descriptor and lists

```cpp
#include <structo/page_decay.hpp>

using decay = structo::page_decay<4>;           // 4 idle scans before a page is demoted

// The history lives in the flags word, next to the state bits, so one atomic access updates both.
// Bits 0..7 are state (PG_ACTIVE, PG_REFERENCED, PG_ISOLATED, ...); bits 8..11 hold the history.
using age_field = structo::page_age_field<8, 4>;

struct page {
  std::atomic<std::uint32_t> flags;
  // list links, refcount, mapping/index for the page cache, ...
};

// Intrusive lists, one pair per NUMA node (or per memcg). The daemon owns a per-list lock; the
// lists are never touched under the segment read guard.
struct lru {
  page_list active, inactive;
  std::uint64_t nr_active, nr_inactive;
  spinlock lock;
};
```

## 2. Harvesting the access bit

`accessed` must mean "some mapping or user touched the page since the last harvest", and the harvest
must clear the hardware bit so the next scan measures new activity.

```cpp
// Called by the daemon with the page's rmap. Returns true if any mapping was accessed.
bool page_harvest_accessed(page &p) {
  bool accessed = false;

  // 1. Mapped pages: walk every PTE that maps the page (reverse mapping) and test-and-clear its
  //    access bit. x86: PTE.A, RISC-V: PTE.A, AArch64: PTE.AF (needs FEAT_HAFDBS or the kernel
  //    must emulate it by clearing AF and handling the access fault). Clearing the bit requires a
  //    TLB invalidation of that entry (batch it: flush once per scan batch, not per page).
  for_each_mapping(p, [&](pte_ref pte) { accessed |= pte_test_and_clear_young(pte); });

  // 2. Unmapped page-cache pages (read()/write() path) have no PTE: the I/O path sets PG_REFERENCED
  //    instead (mark_page_accessed()); consume it here.
  if (p.flags.fetch_and(~PG_REFERENCED) & PG_REFERENCED) accessed = true;

  return accessed;
}
```

Kernel-owned mappings should keep AF set from the start (see
[recursive_remapper.md](recursive_remapper.md)); kernel pages are normally unevictable and excluded
from the ACTIVE/INACTIVE lists anyway.

## 3. The daemon loop

```cpp
// Runs per node when free memory drops below the high watermark, or periodically (e.g. every 1 s)
// for background balancing. 'wm_low'/'wm_high' are the node's watermarks in pages.
void page_daemon_scan(lru &l, std::uint64_t free_pages, std::uint64_t wm_low, std::uint64_t wm_high) {
  structo::decay_pressure_config cfg{};   // defaults: INACTIVE >= 50 % of the LRU, 16/256 background rate

  // How many ACTIVE pages to age this round. 0 means "nothing to do": plenty of free memory and the
  // lists are balanced. Under pressure it grows linearly up to the whole ACTIVE list.
  std::uint64_t budget = structo::decay_scan_count(cfg, l.nr_active, l.nr_inactive,
                                                   free_pages, wm_low, wm_high);

  while (budget > 0) {
    // Take a small batch off the ACTIVE tail under the lock, then work on it unlocked. Batching keeps
    // the lock hold time short and lets the TLB flush be amortized.
    page_batch batch = l.take_from_active_tail(std::min<std::uint64_t>(budget, 32));
    if (batch.empty()) break;
    budget -= batch.size();

    for (page &p : batch) {
      // Pages isolated for hot-remove or migration, or pinned for I/O, are skipped and put back.
      if (page_is_busy(p)) { batch.keep(p); continue; }

      const bool accessed = page_harvest_accessed(p);

      // The whole policy: update the history and ask what to do.
      std::uint32_t f = p.flags.load();
      auto r = decay::step(age_field::get(f), accessed);
      p.flags.store(age_field::set(f, r.age));        // single writer here: the daemon owns the batch

      if (r.action == structo::decay_action::demote) {
        p.flags.fetch_and(~PG_ACTIVE);
        batch.move_to_inactive(p);                    // goes to the INACTIVE head
      } else {
        batch.keep(p);                                // back to the ACTIVE head: it has been aged
      }
    }
    tlb_flush_batch(batch);                           // one flush for all access bits cleared above
    l.return_batch(batch);                            // splice back under the lock, fix nr_* counters
  }
}
```

## 4. Promotion and reclaim (outside the decay policy)

The policy only decides ACTIVE -> INACTIVE. The other transitions are ordinary list code:

```cpp
// Page fault / read() hit on a page that is currently INACTIVE: promote with a full history.
void page_touched_inactive(lru &l, page &p) {
  std::uint32_t f = p.flags.load();
  p.flags.store(age_field::set(f | PG_ACTIVE, decay::fresh()));
  l.move_inactive_to_active(p);
}

// Page already ACTIVE and hit by the I/O path (no PTE): record it without waiting for the scan.
void page_touched_active(page &p) {
  std::uint32_t f = p.flags.load();
  p.flags.store(age_field::set(f, decay::touch(age_field::get(f))));
}

// Reclaim scans the INACTIVE tail: pages whose access bit is clear are written back if dirty,
// removed from the page cache and returned with buddy.free(). Accessed ones are promoted.
```

## 5. Interplay with the segment map and hotplug

- **Pinning rule:** the LRU lists hold `page *` across sleeps. That is safe because a segment is
  only removed after its pages left every list (hot-remove step "migrate"). Do not hold a segment read
  guard in the daemon; it only needs it when resolving a bare PFN.
- **Hot-remove isolation:** `PG_ISOLATED` pages must be skipped by the daemon and by reclaim (see
  `page_is_busy` above). The disconnector takes them off the LRU itself before migrating.
- **Migration keeps the age.** When a page is migrated, copy the history word into the new
  descriptor (`age_field::set(new_flags, age_field::get(old_flags))`) and put the new page on the same
  list, so migration does not reset the working-set estimate.
- **Hot-add:** new segments start with empty lists; their pages enter the buddy allocator and reach
  the LRU only when first allocated and used.
- **Per-node lists:** the segment tag (e.g. NUMA node) selects which `lru` and which watermarks a page
  belongs to: `mm::tag_of(pfn)->node`.

## 6. Tuning

- **History length (`Bits`):** longer history tolerates bursty workloads (a page used once every 8 scans
  survives with `Bits = 8`) but delays reclaim. 4 is a reasonable default for a 1 s scan period.
- **Scan period and batch:** with `balance_urgency = 16` and balanced lists the daemon looks at about 6 %
  of ACTIVE per round when INACTIVE is too small; raise `min_batch` if the lists are small and rounds
  are cheap.
- **`inactive_target_percent`:** larger values reclaim sooner and protect fewer pages; smaller values
  keep a larger working set at the cost of reclaim scanning more.
- **Watermarks:** `decay_scan_count` returns 0 above `wm_high` while the lists are balanced, so wake
  the daemon from the allocator when free pages fall below `wm_high`, and also on a slow timer for
  background balancing.
- **Concurrency:** step results depend only on the history and one boolean, so the policy is trivially
  thread-safe; protect list membership and the flags write with the list lock or a single-writer rule.

See also [hotplug_decay_integration.md](hotplug_decay_integration.md) for a queue-based, cache-centric VM, and
[page_queue_scan.md](page_queue_scan.md) for walking the active/inactive queue with the lock dropped per page.

See also: [page_queue.md](page_queue.md) for the intrusive queues and `splice`.
