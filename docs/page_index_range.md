<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# page_index_range

`structo::page_index_range<Index>` (`<structo/page_index_range.hpp>`) is a half-open `[first, end)`
range over any unsigned index. It is deliberately **not** a PFN type: a page cache indexes pages by
offset in the cached object, a page array by position in a segment, a VM cache by page number. It owns
nothing and is a plain value.

## Usage

```cpp
using range = structo::page_index_range<std::uint64_t>;   // Index: any unsigned integer type

// Construction. 'end' is one past the last index; an inverted pair becomes empty.
range a{4096, 4196};                                      // 100 indices
range b = range::from_count(4096, 100);                   // same, from a count (saturates, never wraps)
range c = range::from_inclusive(4096, 4195);              // from an inclusive last index (e.g. high_pfn)

// Set-like queries; all O(1).
a.contains(4100); a.overlaps(range{4190, 5000}); a.intersect(range{4150, 9000});

// Remove part of a range: e.g. cached offsets [4100, 4120) were just invalidated.
auto d = a.subtract(range{4100, 4120});                   // d.lower = [4096,4100), d.upper = [4120,4196)

// Iterate. Chunks have edges on multiples of the argument (first/last may be short), so lock hold times
// stay bounded; chunk size 0 yields the whole range once.
auto chunks = a.chunks(64);
for (auto &chunk : chunks) { /* chunk.first(), chunk.end() */ }
auto idx = a.indices();
for (auto i : idx) { /* every index */ }

// Bounds-checked access to a span whose element 0 has index 'base'; nullopt if not fully covered.
if (auto part = a.slice(pages_span, /*base=*/4000)) { for (auto &p : *part) touch(p); }
```

## Integration

- **Page array / hotplug:** `page_window` (see [`memory_window.md`](memory_window.md)) exposes
  `first_pfn()`/`end_pfn()`; build `range{w.first_pfn(), w.end_pfn()}` and `subtract()` the runs that
  `buddy_allocator::claim_range` already took, so the walker only inspects pages still to be handled.
  `claim_range` takes an inclusive pair: use `from_inclusive()` / `end() - 1` to convert.
- **Page cache / VM cache trees:** drive "erase everything in `[a, b)`" with `chunks(batch)`: lock, erase
  the chunk's keys, unlock. The keys can be file offsets or PFNs; the range does not care.
- **Interval bookkeeping:** `mergeable()` / `merge()` coalesce touching ranges (e.g. already-offlined
  watermark runs); `split_at()` and `subtract()` carve them.

See also: [`memory_window.md`](memory_window.md), [`buddy_allocator.md`](buddy_allocator.md),
[`memory_hotplug.md`](memory_hotplug.md).
