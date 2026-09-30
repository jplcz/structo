<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# jplcz_structo

`structo` is a header-only C++17 library of OS kernel/hypervisor/trusted-
boundary building blocks: Flattened Device Tree (DTB) decoding and
physical memory region management, built directly on
[`jplcz_reloco`](https://github.com/jplcz/reloco)'s allocation-free,
fallible-by-default primitives (`fdt_reader`/`fdt_index`/`fdt_memory`,
`region_set`/`phys_addr`, `expected`/`result`).

It targets the same class of code as `jplcz_reloco`/`jplcz_microfmt`:
kernel boot paths, hypervisor/VMM code, and any trusted-boundary call site
where allocation-free, exception-free, caller-owned-storage components
matter -- but specialized to the handful of things almost every such boot
path needs from firmware/bootloader-supplied data before an allocator or
MMU exists.

## What's here

See [docs/reference.md](docs/reference.md) for the full per-header
reference (one page per header, linked from there) covering every public
type. A few highlights, demonstrated in [examples/](examples):

- **[`boot_memory_map_demo.cpp`](examples/boot_memory_map_demo.cpp)** --
  decodes a DTB's `/memory`/`/reserved-memory` description into full/free
  `reloco::region_set`s with `structo::boot_memory_map::try_from_dtb()`,
  then finds the largest free region with `try_largest_free_region()` --
  the two queries a first-stage allocator needs before a heap exists.
- **[`device_tree_demo.cpp`](examples/device_tree_demo.cpp)** -- opens a
  DTB into a caller-owned-storage `structo::device_tree` and reads its
  `/chosen` node's `bootargs`/`stdout-path` properties, the first two
  things almost every kernel/hypervisor boot path looks for.

Both build on the OS-development building blocks moved in from
`jplcz_reloco` (typed physical addresses/pages, scatter-gather
translation, and the Flattened Device Tree reader/writer/index), each
documented on its own [docs/](docs) page.

## Using structo

structo is consumed exactly like `jplcz_reloco`/`jplcz_microfmt`:

```cmake
include(FetchContent)
FetchContent_Declare(
    jplcz_structo
    GIT_REPOSITORY https://github.com/jplcz/structo.git
    GIT_TAG master
)
FetchContent_MakeAvailable(jplcz_structo)

target_link_libraries(my_kernel PRIVATE jplcz_structo::structo)
```

structo's own `CMakeLists.txt` fetches `jplcz_reloco` the same way (see
`JPLCZ_STRUCTO_RELOCO_SOURCE_DIR`/`JPLCZ_STRUCTO_RELOCO_GIT_REPOSITORY`/
`JPLCZ_STRUCTO_RELOCO_GIT_TAG`), so a consumer never has to declare that
dependency itself unless it wants to pin/vendor a specific reloco
checkout. See [docs/package-managers.md](docs/package-managers.md) for
Conan/vcpkg/CPM alternatives.

## Building

```sh
cmake -S . -B build -DJPLCZ_STRUCTO_BUILD_TESTS=ON -DJPLCZ_STRUCTO_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build
```

## License

BSD-2-Clause. See [LICENSE](LICENSE).
