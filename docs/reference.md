<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# API reference

Per-type reference for every public structo header. See the
[README](../README.md) for the rationale; this page is a map of what
exists and where.

| Header | Type(s) | One-line summary |
|---|---|---|
| `boot_memory_map.hpp` | `boot_memory_map<Capacity, PhysInt>` | Fixed-capacity physical memory map (`full`/`free` `reloco::region_set`s) decoded from a devicetree blob via `reloco::fdt::try_extract_memory`; `try_from_dtb()` builds it in one call, `try_largest_free_region()`/`free_bytes()` answer the two queries a first-stage allocator needs |
| `device_tree.hpp` | `device_tree`, `device_tree_storage<NodeCapacity, PhandleCapacity, StackDepth>` | `reloco::fdt::fdt_reader` + `reloco::fdt::fdt_index` bundle over caller-owned `device_tree_storage`; `try_open()` builds both from a blob, `try_find_property()`/`try_bootargs()`/`try_stdout_path()` cover the `/chosen`-node lookups a boot path reaches for first |

## `boot_memory_map<Capacity, PhysInt>`

```cpp
auto map = structo::boot_memory_map<32>::try_from_dtb(dtb_blob);
if (!map) { /* handle map.error() */ }

for (auto &region : map->free) { /* hand region.base/region.size to the allocator */ }

auto seed = map->try_largest_free_region();
```

- `try_from_dtb(reloco::span<const std::byte>)` parses the blob with
  `reloco::fdt::fdt_reader::try_create` and extracts its memory
  description with `reloco::fdt::try_extract_memory` in one call. Fails
  with `error::not_found` if no `/memory`-class node exists, or whatever
  error a malformed `reg` property or a too-small `Capacity` reports.
- `try_largest_free_region()` returns the single biggest `free` region
  (`error::not_found` if `free` is empty).
- `free_bytes()` sums every `free` region's size.

## `device_tree` / `device_tree_storage<NodeCapacity, PhandleCapacity, StackDepth>`

```cpp
structo::device_tree_storage<128> storage; // size from a known node-count bound
auto dt = structo::device_tree::try_open(dtb_blob, storage);
if (!dt) { /* handle dt.error() */ }

auto bootargs = dt->try_bootargs();     // "/chosen"'s "bootargs" property
auto console  = dt->try_stdout_path();  // "/chosen"'s "stdout-path" property
auto prop     = dt->try_find_property("/soc/uart@9000000", "clock-frequency");
```

- `device_tree_storage` owns the `reloco::array`-backed node/phandle/
  build-scratch buffers `reloco::fdt::fdt_index::try_build` requires;
  size each capacity from a known bound on the target's DTB.
- `try_open(blob, storage)` builds both the `fdt_reader` and the
  `fdt_index` over `storage` in one call.
- `try_find_property(path, name)` resolves `path` with
  `fdt_index::find_by_path` then looks up a direct property by name.
- `try_bootargs()`/`try_stdout_path()` are `try_find_property("/chosen",
  ...)` shorthands returning the property's string value.
