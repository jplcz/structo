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
| `compat_sg.hpp` | `sg_descriptor_layout<StorageType, PfnField, OffsetField, LengthField, LastFlagField, HeaderSize>`, `chained_sg_layout<StorageType, PfnField, OffsetField, LengthField, LastFlagField, ChainFlagField, HeaderSize>`, `compact_sg_codec<Layout, PageTraits, SpaceTag>`, `chained_sg_codec<Layout, PageTraits, SpaceTag>`, `two_level_sg_codec<L1Layout, L2Layout, PageTraits, SpaceTag>` | Compile-time scatter-gather descriptor layouts and codecs for compact, chained, and two-level representations |
| `phys_addr.hpp` | `default_phys_space`, `host_phys_space`, `guest_phys_space`, `dma_bus_space`, `secure_phys_space`, `nonsecure_phys_space`, `root_phys_space`, `realm_phys_space`, `phys_addr<T, SpaceTag, PhysInt>`, `dmap_mapper<VirtBase, PhysSize, ExpectedSpace, PhysBase>`, `dmap_ptr<T, Mapper, PhysInt>` | Typed physical addresses, address-space tags, and direct-map pointer conversion |
| `pfn_translator.hpp` | `phys_pfn<SpaceTag, PageTraits, PhysInt>` | Typed physical page-frame number and address conversion |
| `phys_page.hpp` | `page_traits<Size, Shift>`, `os_traits_base<Derived, OsPage>`, `page_view<PageTraits, OsTraits>` | Page-size traits and an OS-page-backed physical page view |
| `phys_translator.hpp` | `phys_translator<Policy>` | Policy-based virtual/physical address translator |
| `region_set.hpp` | `memory_region<PhysInt>`, `region_set<Capacity, PhysInt>` | Fixed-capacity collection of physical memory regions |
| `sg_list.hpp` | `sg_entry<SpaceTag, PhysInt>`, `sg_list<Container>` | Scatter-gather entries and container |
| `sg_translator.hpp` | `sg_translator` | Policy-based scatter-gather translator |
| `fdt_reader.hpp` | `fdt_reader`, `mem_reserve_iterator` | Bounds-checked, `iterator_adaptor`-based read-only view over a caller-owned Flattened Device Tree (DTB) span, yielding `result<fdt_event>` per struct-block token |
| `fdt_writer.hpp` | `fdt_writer` | Move-only, fallibly-constructed streaming writer for Flattened Device Tree (DTB, `/dts-v1/`) blobs into a caller-owned span, with sticky error propagation |
| `fdt_index.hpp` | `fdt_index<Container>`, `fdt_index_node`, `fdt_index_phandle_entry`, `fdt_index_child_iterator<NodeContainer>`, `fdt_index_property_iterator` | Random-access index over an `fdt_reader` blob, built once (iteratively, never recursively) into caller-supplied `Container<T>` buffers, giving `O(1)` parent lookup and child iteration without descending into subtrees, plus `O(log n)` phandle-to-node lookup |
| `fdt_memory.hpp` | `try_extract_memory` | Extracts a devicetree's physical memory description straight off `fdt_reader`'s single-pass streaming API (no `fdt_index`, safe to call very early in boot) into caller-provided `region_set`s |
| `buddy_allocator.hpp` | `buddy_allocator<FreeList, OsPage, MaxOrder>` | Power-of-two buddy page allocator over a caller-supplied intrusive free list and `OsPage` type, with DMA/hardware-constrained and greedy allocation variants |
| `reloco_ipc_ring.h` / `reloco_ipc_ring.hpp` | `reloco_ipc_spsc_page`, `reloco_ipc_producer`, `reloco_ipc_consumer` (C ABI); `reloco::ipc_producer`, `reloco::ipc_consumer` (C++ wrapper) | Cross-process, allocation-free single-producer/single-consumer byte-stream ring buffer over a shared-memory page; usable standalone from a Linux/FreeBSD kernel module, with a zero-copy typestate-checked C++ transaction API |

## structo's own headers

- [`boot_memory_map.md`](boot_memory_map.md) -- DTB-derived physical memory map
- [`device_tree.md`](device_tree.md) -- `fdt_reader`/`fdt_index` bundle with `/chosen` helpers

## OS-development building blocks (from `jplcz_reloco`)

The headers below were moved from `jplcz_reloco` into `jplcz_structo`'s
own `include/structo/`. Most were re-homed into the `structo` namespace
(each such file adds `using namespace reloco;` so unmoved reloco types
remain reachable unqualified); `reloco_ipc_ring.h`/`.hpp` kept their
original `reloco_ipc_*`/`reloco::` naming untouched, per explicit
instruction. Each has its own reference page:

- [`compat_sg.md`](compat_sg.md) -- scatter-gather compatibility codecs
- [`phys_addr.md`](phys_addr.md) -- typed physical addresses and direct-map pointers
- [`pfn_translator.md`](pfn_translator.md) -- typed physical page-frame numbers
- [`phys_page.md`](phys_page.md) -- page-size traits and OS-page views
- [`phys_translator.md`](phys_translator.md) -- policy-based address translation
- [`region_set.md`](region_set.md) -- physical memory region collections
- [`sg_list.md`](sg_list.md) -- scatter-gather entries and lists
- [`sg_translator.md`](sg_translator.md) -- policy-based scatter-gather translation
- [`fdt_reader.md`](fdt_reader.md) -- Flattened Device Tree reader
- [`fdt_writer.md`](fdt_writer.md) -- Flattened Device Tree writer
- [`fdt_index.md`](fdt_index.md) -- random-access Flattened Device Tree index
- [`fdt_memory.md`](fdt_memory.md) -- devicetree physical memory extraction
- [`buddy_allocator.md`](buddy_allocator.md) -- power-of-two buddy page allocator
- [`reloco_ipc_ring.md`](reloco_ipc_ring.md) -- cross-process SPSC IPC ring buffer
