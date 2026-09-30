<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `device_tree` / `device_tree_storage<NodeCapacity, PhandleCapacity, StackDepth>`

`include/structo/device_tree.hpp`

```cpp
structo::device_tree_storage<128> storage; // size from a known node-count bound
auto dt = structo::device_tree::try_open(dtb_blob, storage);
if (!dt) { /* handle dt.error() */ }

auto bootargs = dt->try_bootargs();     // "/chosen"'s "bootargs" property
auto console  = dt->try_stdout_path();  // "/chosen"'s "stdout-path" property
auto prop     = dt->try_find_property("/soc/uart@9000000", "clock-frequency");
```

- `device_tree_storage` owns the `reloco::array`-backed node/phandle/
  build-scratch buffers `structo::fdt::fdt_index::try_build` requires;
  size each capacity from a known bound on the target's DTB.
- `try_open(blob, storage)` builds both the `fdt_reader` and the
  `fdt_index` over `storage` in one call.
- `try_find_property(path, name)` resolves `path` with
  `fdt_index::find_by_path` then looks up a direct property by name.
- `try_bootargs()`/`try_stdout_path()` are `try_find_property("/chosen",
  ...)` shorthands returning the property's string value.

See [`examples/device_tree_demo.cpp`](../examples/device_tree_demo.cpp)
for a runnable demo, and [`fdt_reader.md`](fdt_reader.md)/
[`fdt_index.md`](fdt_index.md) for the underlying building blocks.
