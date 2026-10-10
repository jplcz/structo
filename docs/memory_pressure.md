<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# memory_pressure

`include/structo/memory_pressure.hpp` – integer, constexpr helpers that measure memory pressure and pace
the page daemon. Pressure is an *urgency* in `0..256` (0 = plenty of memory, 256 = out of memory). Nothing
here allocates or locks. Time is a `reloco::duration` (wall-clock), so the policy does not depend on the
kernel's tick rate.

## Usage

```cpp
// Watermarks for a pool of 1M pages: min = 0.5 % (floor 64 pages), low = min*5/4, high = min*3/2.
auto wm = structo::memory_watermarks::for_total(1'000'000, 5, 64);

structo::page_daemon_backoff_config cfg{};   // min_interval 1 ms, max_interval 1 s, futile doublings up to 8x
structo::page_daemon_backoff backoff{cfg};   // remembers consecutive rounds that reclaimed nothing
structo::pressure_average avg;               // fast attack (rises at once), slow decay (1/4 per update)

for (;;) {
  // Free pages plus half of the clean reclaimable ones: those can be dropped without I/O.
  auto eff = structo::effective_free_pages(free_pages(), clean_inactive_pages());

  // 0 at/above wm.high, 256 at/below wm.min, linear between. Smooth it so one lucky round
  // does not end the alarm.
  avg.update(structo::pressure_urgency(eff, wm));

  // Do one scan round sized by pressure; it reports pages scanned and pages actually freed.
  auto [scanned, reclaimed] = run_one_round(avg.value());

  // Scanning a lot but freeing little means pressure is really higher than the watermarks say.
  auto pressure = structo::combine_pressure(avg.value(), structo::reclaim_inefficiency(scanned, reclaimed));

  // Sleep: short under pressure, long when idle, exponentially longer while rounds are futile.
  // next_interval() returns a reloco::duration; pass it to the kernel's timed wait on the daemon's
  // wakeup object (condvar / event / callout). A critical allocation signals that object to cut the sleep short.
  timed_wait(daemon_wakeup, backoff.next_interval(pressure, scanned, reclaimed));
}
```

| Helper | Meaning |
| --- | --- |
| `memory_watermarks{min, low, high}` | Free-page thresholds; `for_total()` derives them, `operator+` sums per-zone sets |
| `pressure_level_of(free, wm)` | `none / low / medium / critical` for allocator slow paths |
| `reclaim_inefficiency(scanned, reclaimed)` | 0 = all reclaimed, 256 = nothing reclaimed |
| `page_daemon_interval(cfg, urgency)` | Stateless sleep (`reloco::duration`), linear from `max_interval` to `min_interval` |

## Integration

- **Page daemon / decay:** `decay_urgency()` ([page_decay.md](page_decay.md)) is `pressure_urgency` over a
  `(low, high)` pair; pass `avg.value()` into the scan sizing and the daemon loop above around
  `page_queue_scan` ([page_queue_scan.md](page_queue_scan.md)).
- **Allocators:** check `pressure_level_of()` on the slow path. `critical` should wake the daemon at once
  (not wait for its sleep); convert the duration for your timer with `reloco::duration_converter<T>`; `low` is the "start background reclaim" level.
- **Hotplug / balloon:** after `buddy_allocator::claim_range` or onlining a segment, rebuild the watermarks
  with `for_total()` from the new total or add per-segment sets ([memory_hotplug.md](memory_hotplug.md)).
