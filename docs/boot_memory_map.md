<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `boot_memory_map<Capacity, PhysInt>`

`include/structo/boot_memory_map.hpp`

```cpp
auto map = structo::boot_memory_map<32>::try_from_dtb(dtb_blob);
if (!map) { /* handle map.error() */ }

for (auto &region : map->free) { /* hand region.base/region.size to the allocator */ }

auto seed = map->try_largest_free_region();
```

- `try_from_dtb(reloco::span<const std::byte>)` parses the blob with
  `structo::fdt::fdt_reader::try_create` and extracts its memory
  description with `structo::fdt::try_extract_memory` in one call. Fails
  with `error::not_found` if no `/memory`-class node exists, or whatever
  error a malformed `reg` property or a too-small `Capacity` reports.
- `try_largest_free_region()` returns the single biggest `free` region
  (`error::not_found` if `free` is empty).
- `free_bytes()` sums every `free` region's size.

See [`examples/boot_memory_map_demo.cpp`](../examples/boot_memory_map_demo.cpp)
for a runnable demo, and [`fdt_memory.md`](fdt_memory.md)/
[`region_set.md`](region_set.md) for the underlying building blocks.
Once you have `free`, [`early_region_allocator.md`](early_region_allocator.md)
is the allocator that bootstraps directly from it.
