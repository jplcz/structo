<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# API reference

Per-type reference for every public structo header, grouped by
subsystem. See the [README](../README.md) for the rationale behind the
library as a whole. Every header also has a one-line entry below; most
have a linked, standalone page going into more depth.

## Contents

- [Boot & devicetree discovery](#boot-devicetree-discovery)
- [Physical/virtual addressing & scatter-gather](#physicalvirtual-addressing-scatter-gather)
- [Device & hardware abstraction (`hw/`)](#device-hardware-abstraction-hw)
- [Async objects & scheduling](#async-objects-scheduling)
- [Cross-process IPC](#cross-process-ipc)
- [CPU identity & per-CPU/thread/domain storage](#cpu-identity-per-cputhreaddomain-storage)
- [ASID/VMID allocation & MMU context tracking](#asidvmid-allocation-mmu-context-tracking)
- [TLB maintenance & hardware address translation](#tlb-maintenance-hardware-address-translation)
- [Page tables & PTE field decoding](#page-tables-pte-field-decoding)
- [Execution domain & build-time configuration](#execution-domain-build-time-configuration)
- [Architecture control registers & hardware RNG](#architecture-control-registers-hardware-rng)
- [Debugging utilities](#debugging-utilities)
- [Synchronization & interrupt/preemption guards](#synchronization-interruptpreemption-guards)

## Boot & devicetree discovery

| Header | Type(s) | One-line summary |
|---|---|---|
| [`boot_memory_map.hpp`](boot_memory_map.md) | `boot_memory_map<Capacity, PhysInt>` | Fixed-capacity physical memory map (`full`/`free` `reloco::region_set`s) decoded from a devicetree blob via `structo::fdt::try_extract_memory`; `try_from_dtb()` builds it in one call, `try_largest_free_region()`/`free_bytes()` answer the two queries a first-stage allocator needs |
| [`early_region_allocator.hpp`](early_region_allocator.md) | `early_region_allocator<Capacity, PhysInt>` | Allocation-free physical-range allocator `try_create()`-seeded from a caller's `region_set` (e.g. `boot_memory_map::free`) by taking an immutable snapshot into its own private `region_set` -- the seed is never mutated; `try_alloc()` best-fit-searches and carves out a request, `try_reserve()` punches out a caller-known exact range, `free()` heals a range back in, all against the allocator's own copy, which can later seed `buddy_allocator::init()` |
| [`device_tree.hpp`](device_tree.md) | `device_tree`, `device_tree_storage<NodeCapacity, PhandleCapacity, StackDepth>` | `structo::fdt::fdt_reader` + `structo::fdt::fdt_index` bundle over caller-owned `device_tree_storage`; `try_open()` builds both from a blob, `try_find_property()`/`try_bootargs()`/`try_stdout_path()` cover the `/chosen`-node lookups a boot path reaches for first |
| [`fdt_reader.hpp`](fdt_reader.md) | `fdt_reader`, `mem_reserve_iterator` | Bounds-checked, `iterator_adaptor`-based read-only view over a caller-owned Flattened Device Tree (DTB) span, yielding `result<fdt_event>` per struct-block token |
| [`fdt_writer.hpp`](fdt_writer.md) | `fdt_writer` | Move-only, fallibly-constructed streaming writer for Flattened Device Tree (DTB, `/dts-v1/`) blobs into a caller-owned span, with sticky error propagation |
| [`fdt_index.hpp`](fdt_index.md) | `fdt_index<Container>`, `fdt_index_node`, `fdt_index_phandle_entry`, `fdt_index_child_iterator<NodeContainer>`, `fdt_index_property_iterator` | Random-access index over an `fdt_reader` blob, built once (iteratively, never recursively) into caller-supplied `Container<T>` buffers, giving `O(1)` parent lookup and child iteration without descending into subtrees, plus `O(log n)` phandle-to-node lookup |
| [`fdt_memory.hpp`](fdt_memory.md) | `try_extract_memory` | Extracts a devicetree's physical memory description straight off `fdt_reader`'s single-pass streaming API (no `fdt_index`, safe to call very early in boot) into caller-provided `region_set`s |
| [`arch/fdt_cpu_map.hpp`](fdt_cpu_map.md) | `try_populate_hw_id_lut_from_fdt` | Decodes a devicetree's `/cpus` node straight off `fdt_reader`'s single-pass streaming API (no `fdt_index`, safe to call before any CPU index exists) into a caller-provided `hw_id_lut`, assigning sequential logical CPU indices in devicetree order |

## Physical/virtual addressing & scatter-gather

| Header | Type(s) | One-line summary |
|---|---|---|
| [`phys_addr.hpp`](phys_addr.md) | `default_phys_space`, `host_phys_space`, `guest_phys_space`, `dma_bus_space`, `secure_phys_space`, `nonsecure_phys_space`, `root_phys_space`, `realm_phys_space`, `phys_addr<T, SpaceTag, PhysInt>`, `dmap_mapper<VirtBase, PhysSize, ExpectedSpace, PhysBase>`, `dmap_ptr<T, Mapper, PhysInt>` | Typed physical addresses, address-space tags, and direct-map pointer conversion |
| [`target_ptr.hpp`](target_ptr.md) | `user_space`, `kernel_space`, `guest_vm_space`, `realm_space`, `secure_world_space`, `target_ptr_space_traits<SpaceTag>`, `target_ptr<T, SpaceTag, PtrType>` | Typed pointer into another *virtual* address space (user/kernel/Realm/guest VM/...), valid only in the current execution context; fallible, page-fault-aware materialize/store accessors (with `_nofault` variants) and Rust-style checked/wrapping/saturating pointer arithmetic |
| [`io_address.hpp`](io_address.md) | `default_io_space`, `port_io_space`, `device_io_space`, `secure_io_space`, `nonsecure_io_space`, `realm_io_space`, `hypervisor_io_space`, `io_address<T, SpaceTag, IoInt>`, `reg_traits<Size>`, `io_math` | Typed, tagged address into a device-register space (port I/O, MMIO, ARM device memory, ...); pure address tagging and compile-time-checked offset arithmetic, plus register-width-aware offset math, no I/O access itself |
| [`pfn_translator.hpp`](pfn_translator.md) | `phys_pfn<SpaceTag, PageTraits, PhysInt>` | Typed physical page-frame number and address conversion |
| [`phys_page.hpp`](phys_page.md) | `page_traits<Size, Shift>`, `os_traits_base<Derived, OsPage>`, `page_view<PageTraits, OsTraits>` | Page-size traits and an OS-page-backed physical page view |
| [`phys_translator.hpp`](phys_translator.md) | `phys_translator<Policy>` | Policy-based virtual/physical address translator |
| [`region_set.hpp`](region_set.md) | `memory_region<PhysInt>`, `region_set<Capacity, PhysInt>` | Fixed-capacity collection of physical memory regions |
| [`sg_list.hpp`](sg_list.md) | `sg_entry<SpaceTag, PhysInt>`, `sg_list<Container>`, `sg_list_cursor<PhysInt>` | Scatter-gather entries and container, plus a type-erased single-pass cursor splitting entries into caller-sized chunks (e.g. for remapping memory) |
| [`sg_translator.hpp`](sg_translator.md) | `sg_translator` | Policy-based scatter-gather translator |
| [`compat_sg.hpp`](compat_sg.md) | `sg_descriptor_layout<StorageType, PfnField, OffsetField, LengthField, LastFlagField, HeaderSize, LengthUnit>`, `chained_sg_layout<StorageType, PfnField, OffsetField, LengthField, LastFlagField, ChainFlagField, HeaderSize>`, `length_unit_bytes`, `length_unit_pages`, `compact_sg_codec<Layout, PageTraits, SpaceTag>`, `chained_sg_codec<Layout, PageTraits, SpaceTag>`, `two_level_sg_codec<L1Layout, L2Layout, PageTraits, SpaceTag>` | Compile-time scatter-gather descriptor layouts and codecs for compact, chained, and two-level representations; `LengthUnit` selects whether `LengthField` is a literal byte count or a whole-page count |
| [`buddy_allocator.hpp`](buddy_allocator.md) | `buddy_allocator<FreeList, OsPage, MaxOrder>` | Power-of-two buddy page allocator over a caller-supplied intrusive free list and `OsPage` type, with DMA/hardware-constrained and greedy allocation variants, plus `reserve(page)` to retroactively "un-free" one specific already-free page |

## Device & hardware abstraction (`hw/`)

| Header | Type(s) | One-line summary |
|---|---|---|
| [`io_space_ref.hpp`](io_space_ref.md) | `io_space_ref<SpaceTag>`, `io_space_traits<Backend>` | Type-erased, non-owning handle performing fixed-width loads/stores and `rep insb`/`outsb`-style string I/O over an `io_address`, via a customizable backend |
| [`hw/uart_ref.hpp`](uart_ref.md) | `structo::hw::uart_ref`, `structo::hw::uart_traits<Backend>`, `structo::hw::uart_config` | Type-erased, non-owning handle over a basic (interrupt-free, polled) UART's operations and settings -- configure/tx_ready/rx_ready/put_byte/get_byte/write/read_available -- via a customizable backend |
| [`hw/rng.hpp`](hw_rng.md) | `structo::hw::hw_rng_ref`, `structo::hw::hw_rng_traits<Backend>` | Type-erased, non-owning handle over a hardware random/entropy source -- `try_generate64`/`try_generate32`/`try_fill`, with bounded retry on a backend's transient "not ready yet" condition -- via a customizable backend |
| [`hw/rng_combinator.hpp`](hw_rng.md) | `structo::hw::hw_rng_combinator` | Combines several `hw_rng_ref` sources (e.g. a real hardware RNG plus weak fallback jitter sources) by `XOR`-folding every successful draw and avalanche-mixing the result; itself bindable through another `hw_rng_ref` |
| [`hw/otp_storage.hpp`](otp_storage.md) | `structo::hw::otp_storage_ref`, `structo::hw::otp_storage_traits<Backend>` | Type-erased, non-owning handle over hardware one-time-programmable (OTP) storage -- an eFuse/antifuse array or a flash-sector-backed OTP partition -- offering bounds-checked `try_read`/OR-only irreversible `try_program`, optional permanent region `try_lock`/`is_locked`, and a `try_program_verify` convenience, via a customizable backend |
| [`hw/timer_ref.hpp`](timer_ref.md) | `structo::hw::timer_ref`, `structo::hw::timer_traits<Backend>`, `structo::hw::timer_mode`, `structo::hw::timer_capabilities` | Type-erased, non-owning handle over a hardware countdown/interval timer -- arm one-shot/periodic, cancel, is_active, poll for expiry, query remaining time/capabilities -- via a customizable backend; `try_wait`/`wait` follow Rust `embedded-hal`'s `nb::Result` non-blocking-poll shape and periods/readback use `reloco::duration` |
| [`hw/irqc_ref.hpp`](irqc_ref.md) | `structo::hw::irqc_ref`, `structo::hw::irqc_traits<Backend>`, `structo::hw::irq_source`, `structo::hw::irq_config`, `structo::hw::irq_trigger`, `structo::hw::irq_polarity`, `structo::hw::irq_map_data`, `structo::hw::irq_map_kind`, `structo::hw::irq_security_domain`, `structo::hw::irqc_capabilities` | Type-erased, non-owning handle over a hardware interrupt controller (PIC), modeled after FreeBSD's `INTRNG` -- enable/disable/setup/teardown a source, `map_intr` a bus-agnostic FDT/MSI/GSI specifier onto a registered `irq_source`, optional affinity (`assign_cpu`), ARM TrustZone-inspired security-domain switching (`assign_security_domain`) and domain-targeted, CPU-mask-batched IPIs (`send_ipi`), and `pre_ithread`/`post_ithread`/`post_filter` mask/EOI hooks -- via a customizable backend; `mask_scoped` is a `scoped_mask` RAII guard masking one source for its lifetime |
| [`hw/framebuffer.hpp`](framebuffer.md) | `structo::hw::framebuffer<PixelFormat>`, `structo::hw::rgb_color`, `structo::hw::rgb888`/`bgr888`/`xrgb8888`/`rgba8888`/`rgb565`/`gray8` | Non-owning wrapper over a linear pixel buffer plus bounds-checked `put_pixel`/`get_pixel` and clipped `fill_rect`/`clear`/`draw_hline`/`draw_vline`/`draw_rect`/`draw_line`/`blit` rendering helpers; the pixel encoding is the only backend-specific axis, captured by `PixelFormat` rather than a type-erased trait |
| [`hw/console_ref.hpp`](console_ref.md) | `structo::hw::console_ref`, `structo::hw::console_traits<Backend>`, `structo::hw::console_color`, `structo::hw::console_cell` | Type-erased, non-owning handle over a character-cell, 16-color text console -- `put_char`/`get_char`, clipped `fill_rect`/`clear`/`scroll_up`, cursor-driven `write_char`/`put`/`write` -- via a customizable backend; unlike `uart_ref`/`timer_ref`, owns the logical cursor position/current colors itself and forwards cursor moves to an optional `move_cursor` backend trait |
| [`hw/vga_text_console.hpp`](vga_text_console.md) | `structo::hw::vga_text_console` | `console_ref`-adaptable backend for the classic 2-bytes-per-cell PC VGA/CGA text-mode framebuffer; optional caller-supplied `reloco::function_ref` callback forwards cursor moves to real CRTC registers without this header depending on port I/O |
| [`hw/framebuffer_console.hpp`](framebuffer_console.md) | `structo::hw::framebuffer_console<PixelFormat, Font>`, `structo::hw::block_font_8x8`, `structo::hw::console_color_to_rgb` | `console_ref`-adaptable backend rasterizing console cells onto a `framebuffer<PixelFormat>` via a pluggable `Font` glyph trait, backed by a caller-supplied shadow `console_cell` grid (pixels are write-only); ships one honestly-documented placeholder font, `block_font_8x8`, to avoid embedding a traced/copyrighted bitmap font |
| [`hw/vt100.hpp`](vt100.md) | `structo::hw::vt100_terminal` | VT100/ANSI escape-sequence interpreter built entirely on `console_ref` -- cursor movement (CUU/CUD/CUF/CUB/CUP), erase (ED/EL), SGR (16-color + bold-as-bright + reset), raw CR/LF/BS/TAB -- usable with any adapted backend; LF is strict VT100 (down-only, no column reset), unlike `console_ref::put()`'s friendlier convenience handling |

## Async objects & scheduling

| Header | Type(s) | One-line summary |
|---|---|---|
| [`async_kernel_object.hpp`](async_kernel_object.md) | `async_kernel_object<Traits, Capacity>` | Memory-safe, owning base for any kernel object whose completion fires asynchronously (interrupt, other CPU, softirq, timer-wheel sweep, ...) -- `try_submit`/`cancel`/`drain`/`deactivate` plus `is_pending`/`is_active`/`is_firing`, atomic `callout(9)`-style cancel/drain semantics, via a compile-time `Traits::submit`/`cancel` policy; subsystem-agnostic base for a future `callout`-like timer object |
| [`callout.hpp`](callout.md) | `callout<Subsystem, Capacity>` | FreeBSD-`callout(9)`-like one-shot/periodic deferred callback built on `async_kernel_object` -- `reset`/`reset_periodic`/`stop`/`drain`/`deactivate`/`pending`/`active`/`firing`, callback invoked as `void(callout &)`, scheduling policy supplied by a `Subsystem` ("callout subsystem") implementing `submit(Obj &, reloco::duration)`/`cancel(Obj &)` |
| [`runqueue.hpp`](runqueue.md) | `fifo_runqueue<Entry, Hook>`, `priority_list_runqueue<Entry, Hook, Priority>`, `priority_bucket_runqueue<Entry, Hook, Priority, NumPriorities>` | Three allocation-free, intrusive runqueue policies sharing one `enqueue`/`dequeue`/`peek`/`remove`/`empty`/`size` surface: plain FIFO, a priority-sorted intrusive list, and a Linux-"O(1) scheduler"-style per-priority bucket array plus bitmap (`__builtin_ctzll`-scanned) for O(1) highest-priority lookup |
| [`sched.hpp`](sched.md) | `noop_sched<Entry, Hook, PerCpu>`, `fixed_priority_sched<Entry, Hook, Priority, NumPriorities, PerCpu>`, `edf_sched<Entry, Hook, Deadline, PerCpu>`, `sched_ule<Entry, Hook, Priority, State, NumPriorities, PerCpu>`, `sched_4bsd<Entry, Hook, Priority, State, NumPriorities, PerCpu>` | Five stateless, tickless scheduling policies built on `runqueue.hpp`: plain FIFO, static-priority `SCHED_FIFO`/`SCHED_RR`-like, Earliest Deadline First, and simplified FreeBSD-`SCHED_ULE`/`SCHED_4BSD`-like; per-task bookkeeping lives in caller-owned `Entry` fields, per-CPU state is resolved through a caller-supplied `PerCpu` trait (`structo::arch::per_cpu_ptr` fits directly) |
| [`load_average.hpp`](load_average.md) | `load_average<Rep, FracBits>`, `unix_load_average<Rep, FracBits>` | Linux-`calc_load()`-style exponentially-decayed moving average of an "active count", sampled by the caller at arbitrary (tickless) intervals via `reloco::fixed_point::exp()`; `unix_load_average` is the classic `/proc/loadavg` 1/5/15-minute triple built on three of these -- a systemwide quantity, so keep exactly one instance and serialize `sample()` to a single caller on SMP (see the linked page) |

## Cross-process IPC

| Header | Type(s) | One-line summary |
|---|---|---|
| [`reloco_ipc_ring.h`](reloco_ipc_ring.md) / [`reloco_ipc_ring.hpp`](reloco_ipc_ring.md) | `reloco_ipc_spsc_page`, `reloco_ipc_producer`, `reloco_ipc_consumer` (C ABI); `reloco::ipc_producer`, `reloco::ipc_consumer` (C++ wrapper) | Cross-process, allocation-free single-producer/single-consumer byte-stream ring buffer over a shared-memory page; usable standalone from a Linux/FreeBSD kernel module, with a zero-copy typestate-checked C++ transaction API |

## CPU identity & per-CPU/thread/domain storage

| Header | Type(s) | One-line summary |
|---|---|---|
| [`arch/cpu_index.hpp`](cpu_index.md) | `cpu_index<Tag>`, `uniprocessor_tag` | Zero-overhead CRTP-style "which logical CPU is this" wrapper plus core operations (yield, WFE/SEV, BSP detection, hardware ID), delegated to a caller-provided `Tag` policy |
| [`arch/hw_id_map.hpp`](hw_id_map.md) | `hw_id_lut<HwId, MaxCpus, L1Size, Hash>`, `default_hw_id_hash` | Allocation-free, two-tier (hash-filtered L1 + sorted L2 fallback) lookup table mapping a hardware ID (e.g. MPIDR_EL1) to a logical CPU index |
| [`arch/cpu_mask.hpp`](cpu_mask.md) | `cpu_mask<Tag, MaxCpus>`, `physical_cpu_tag`, `vcpu_tag` | Fixed-capacity, compile-time-sized, tagged CPU/vCPU bitmask with Rust `bitflags`-style set algebra, intrinsic-backed bit-scanning, a lock-free GCC/Clang `__atomic_*`-builtin subset (including atomic find-first-set and a CAS-based find-and-set bitmap allocator), and a `words()` accessor that decays the mask to a type-erased `reloco::span<const std::uint64_t>` for APIs (e.g. `hw::irqc_ref::send_ipi`) that should not be templated on `Tag`/`MaxCpus` |
| [`arch/lazy_context.hpp`](lazy_context.md) | `lazy_context<Traits, CpuId>`, `lazy_context_switcher<Traits, CpuId>`, `static_per_cpu_storage<MaxCpus, Context, CpuId>` | Lazy/deferred per-thread coprocessor-state (FPU, vector registers, ...) context-switching framework, saving/restoring hardware state only on first trapped use |
| [`arch/per_cpu_ptr.hpp`](per_cpu_ptr.md) | `per_cpu_ptr<Tag, T>` | Storage-free, type-safe resolver for a per-CPU `T*`, keyed by `Tag` and backed entirely by `Tag`'s own OS-specific per-CPU mechanism |
| [`arch/per_thread_ptr.hpp`](per_thread_ptr.md) | `per_thread_ptr<Tag, T>` | Storage-free, type-safe resolver for the *running thread's own* `T*`, keyed by `Tag`; exposes no explicit-thread accessor by design |
| [`arch/per_domain_ptr.hpp`](per_domain_ptr.md) | `per_domain_ptr<Tag, T>` | Storage-free, type-safe resolver for a per-domain `T*` (e.g. Arm RME World ID), keyed by `Tag` and backed entirely by `Tag`'s own per-domain mechanism |

## ASID/VMID allocation & MMU context tracking

| Header | Type(s) | One-line summary |
|---|---|---|
| [`arch/asid_allocator.hpp`](asid_allocator.md) | `asid_allocator<Tag, MaxActive>`, `tagged_asid<Tag>`, `process_asid_tag`, `vmid_tag` | Generation-counted bitmap ASID/VMID/PCID allocator (runtime ASID width, allocator-backed bitmap storage), returning an opaque, per-Tag `tagged_asid<Tag>` handle that cannot be confused between a process ASID and a VM's VMID |
| [`arch/fixed_asid_allocator.hpp`](fixed_asid_allocator.md) | `fixed_asid_allocator<Tag, Capacity, SeqGroupSize>`, `fixed_asid<Tag>` | No-allocation, rollover-free ASID allocator for a compile-time-bounded, power-of-two task/VM count (`Capacity`) that never exceeds the hardware ASID space -- every task gets one permanent ASID slot for its lifetime off a fixed, static `std::uint64_t[(Capacity + 63) / 64]` bitmap, with no rollover/`flush_required` machinery; spare hardware ASID bits above `log2(Capacity)` carry a cosmetic sequence counter (shared per `SeqGroupSize` neighboring slots) so reused slots look different in hardware/log dumps without affecting correctness; `slot_of()` recovers the stable slot index while `asid_of()` returns the full raw value; `fixed_asid<Tag>` is deliberately a distinct type from `tagged_asid<Tag>` even for the same `Tag` |
| [`arch/mm_asid_context.hpp`](mm_asid_context.md) | `mm_asid_context<AsidTag, MaxCpus, CpuTag>` | Reusable per-address-space ASID/cpu-residency tracker composing `asid_allocator` + `cpu_mask`: a cached `tagged_asid<AsidTag>` plus a lock-free cpu mask of every core that may still hold a stale, tagged TLB entry for it |

## TLB maintenance & hardware address translation

| Header | Type(s) | One-line summary |
|---|---|---|
| [`arch/tlb_flush.hpp`](tlb_flush.md) | `tlb_flush_traits<Arch>`, `tlb_flusher<Arch>`, `{untagged,process,guest,hypervisor,secure,nonsecure,root,realm,gpt}_tlb_space` | Per-architecture TLB-maintenance customization point plus the `Space`-tagged dispatcher routing every flush call through it, with automatic precision fallback (tag/page/range -> whole-`Space`) and an explicit, never-synthesized broadcast axis |
| [`arch/arm/tlb_flush.hpp`](tlb_flush.md), [`arch/arm64/tlb_flush.hpp`](tlb_flush.md), [`arch/riscv/tlb_flush.hpp`](tlb_flush.md), [`arch/x86/tlb_flush.hpp`](tlb_flush.md) | `structo::arch::{arm,arm64}::{tlb_tag, tlb_tag_mp}`, `structo::arch::riscv::tlb_tag`, `structo::arch::x86::tlb_tag` | Real `tlb_flush_traits<Tag>` specializations wiring up ARMv7-A `TLBIALL`/`TLBIASID`/`TLBIMVA(A)` (+ `...IS` broadcast), AArch64 `TLBI VAE1IS`/`ASIDE1IS`/`VMALLE1IS`/`ALLE2IS`/`IPAS2E1IS`/`RVAE1IS`, RISC-V `sfence.vma`, and x86 `INVLPG`/`INVPCID`/full `CR3` reload respectively |
| [`arch/address_translate.hpp`](address_translate.md) | `address_translate_traits<Arch>`, `address_translator<Arch>`, `translated_address`, `translate_access` | Per-architecture, `Space`-tagged customization point modeling hardware address-translation-query instructions (e.g. ARM `AT`/`ATS1*`), returning `reloco::result<translated_address>` with fault status decoded into `reloco::error` (`page_fault`/`permission_denied`/`security_violation`/`io_error`/`try_again`/`unsupported_operation`) |
| [`arch/arm/address_translate.hpp`](address_translate.md), [`arch/arm64/address_translate.hpp`](address_translate.md) | `structo::arch::arm::at_tag`, `structo::arch::arm64::at_tag` | Real `address_translate_traits<Tag>` specializations wiring up AArch32 `ATS1CPR`/`ATS1CPW`/`ATS1HR`/`ATS1HW`/`ATS12NSOPR`/`ATS12NSOPW` and AArch64 `AT S1E1R`/`S1E1W`/`S1E2R`/`S1E2W`/`S12E1R`/`S12E1W` (including Secure EL2/`FEAT_SEL2` regimes), decoding `PAR`/`PAR_EL1` |

## Page tables & PTE field decoding

| Header | Type(s) | One-line summary |
|---|---|---|
| [`arch/page_table_traits.hpp`](page_table_traits.md) | `page_table_level<IndexBits, Shift, AllowsLeaf>`, `page_table_levels<LeafPageTraits, VaBits, Levels...>`, `page_table_entry<Tag, Int>`, `page_table_entry_traits<Tag>` | Compile-time, self-consistency-checked per-level index-decoding traits for a multi-level hardware page table (ARM64/x86-64/RISC-V-shaped), plus an opaque tagged raw PTE handle and a declared-but-undefined entry-encode/decode extension point for a future walker layer |
| [`arch/page_table_range.hpp`](page_table_range.md) | `page_table_level_range<Levels, LevelIndex, Entry, AddrInt>`, `make_level_range<Levels, LevelIndex>(...)` | Forward-iterable (and Rust-style `.iter()`-able) adapter over exactly the entries of one caller-supplied page-table level's span that a `[start, end)` virtual-address range touches, built on `page_table_traits.hpp`; performs no mapping/allocation of its own |
| [`arch/page_table_occupancy.hpp`](page_table_occupancy.md) | `page_table_occupancy<CounterInt>` | O(1) live-entry counter for one page-table level's table (increment/decrement per entry transition), so a `page_table_range.hpp`-based unmap walk can free now-empty tables without an O(entry_count) rescan per table per unmap |
| [`arch/arm64/page_table_traits.hpp`](page_table_arch_configs.md) | `structo::arch::arm64::{level1, level2, level1_16k, level2_16k, level1_64k, level2_64k}` | Ready-made `page_table_levels<...>` configs for ARMv8-A/AArch64 (VMSAv8-64), covering all three translation granules (4KB/16KB/64KB) at the two most common translation-table starting levels |
| [`arch/arm/page_table_traits.hpp`](page_table_arch_configs.md) | `structo::arch::arm::lpae::{level1, level2}` | Ready-made `page_table_levels<...>` configs for ARMv7 with the Large Physical Address Extension (LPAE), 4KB granule (LPAE's only granule) |
| [`arch/riscv/page_table_traits.hpp`](page_table_arch_configs.md) | `structo::arch::riscv::{sv39, sv48, sv57}` | Ready-made `page_table_levels<...>` configs for RISC-V Sv39/Sv48/Sv57, where every level (including the root) allows an early-terminating leaf PTE |
| [`arch/x86/page_table_traits.hpp`](page_table_arch_configs.md) | `structo::arch::x86::{i386, pae, long_mode_4level, long_mode_5level}` | Ready-made `page_table_levels<...>` configs for x86/x86-64 paging modes, with huge-page-capable levels expressed via `AllowsLeaf` |
| [`arch/pte_field.hpp`](page_table_entry_fields.md) | `pte_bit_field<LowBit, NumBits, Int>` | Generic named-bit-field primitive (`get`/`set`/`test`/`set_bit`) every per-architecture PTE field descriptor header below is built from |
| [`arch/arm64/pte_stage1.hpp`](page_table_entry_fields.md), [`arch/arm64/pte_stage2.hpp`](page_table_entry_fields.md) | `structo::arch::arm64::{stage1_ns_tag, stage1_secure_tag, stage1_secure_el2_tag, stage2_tag, stage2_secure_tag}` (+ `page_table_entry_traits<>` specializations) | Real `page_table_entry_traits<Tag>` specializations for AArch64 stage-1/stage-2 VMSA descriptors, covering Non-secure, Secure (TrustZone), Secure EL2 (sEL2), and hypervisor-staging (stage-2) regimes, with leaf/block `final_level` selection |
| [`arch/arm/pte_stage1.hpp`](page_table_entry_fields.md), [`arch/arm/pte_stage2.hpp`](page_table_entry_fields.md) | `structo::arch::arm::lpae::{stage1_ns_tag, stage1_secure_tag, stage2_tag}` | ARMv7-LPAE equivalents of the above (bit-for-bit compatible format, 4KB-only granule, no sEL2 equivalent) |
| [`arch/riscv/pte.hpp`](page_table_entry_fields.md) | `structo::arch::riscv::{pte_tag, pte_g_stage_tag}` | `page_table_entry_traits<Tag>` specializations for RISC-V's single shared S-stage/G-stage PTE bit layout |
| [`arch/x86/pte.hpp`](page_table_entry_fields.md) | `structo::arch::x86::{pte_tag, npt_tag}` | `page_table_entry_traits<Tag>` specializations for ordinary x86/x86-64 paging and AMD NPT (identical bit layout), including the PS-vs-PAT bit-7 `final_level` caveat |
| [`arch/x86/pte_ept.hpp`](page_table_entry_fields.md) | `structo::arch::x86::ept_tag` | `page_table_entry_traits<Tag>` specialization for Intel EPT's distinct encoding (no dedicated present bit; derived from R/W/X) |

## Execution domain & build-time configuration

| Header | Type(s) | One-line summary |
|---|---|---|
| [`structo_config.hpp`](execution_domain.md) | `STRUCTO_DOMAIN_{SECURE,NONSECURE,MONITOR,HYPERVISOR,SECURE_HYPERVISOR}` | Single build-time customization entry point (reloco-style user-override hook plus `#ifndef`-guarded macro defaults) backing `arch/execution_domain.hpp`'s domain selection; defining more than one `STRUCTO_DOMAIN_*` macro is a build-time `#error` |
| [`arch/execution_domain.hpp`](execution_domain.md) | `execution_domain`, `current_execution_domain` | The privilege world/mode (`secure`/`nonsecure`/`monitor`/`hypervisor`/`secure_hypervisor`/`unspecified`) a translation unit is compiled to run as, resolved from `structo_config.hpp`'s `STRUCTO_DOMAIN_*` macros; pure scaffolding today, not yet consulted by `tlb_flusher<Arch>`/`address_translator<Arch>` |
| [`arch/domain_space_traits.hpp`](domain_space_traits.md) | `domain_space_traits<Domain>`, `space_domain_of<SpaceTag>`, `same_domain_v<TagA, TagB>`, `current_domain_spaces` | Correspondence table translating an `execution_domain` into the matching tag across four independent tag families (`tlb_flush.hpp`'s `tlb_space`, `phys_addr.hpp`'s `phys_space`, `io_address.hpp`'s `io_space`, `target_ptr.hpp`'s `virt_space`), and a reverse lookup/equality helper |

## Architecture control registers & hardware RNG

| Header | Type(s) | One-line summary |
|---|---|---|
| [`arch/arm64/mmu_regs.hpp`](mmu_regs.md) | `structo::arch::arm64::{sctlr_el1, tcr_el1, ttbr0_el1, ttbr1_el1, mair_el1, scr_el3, hcr_el2, vtcr_el2, vttbr_el2}` | Parsed/built views of AArch64 EL1 MMU control registers, the EL3 TrustZone `SCR_EL3` register, and the EL2 hypervisor stage-2 registers `HCR_EL2`/`VTCR_EL2`/`VTTBR_EL2`; pure bit-field parse/build logic is portable and tested, `read()`/`write()` (real `MRS`/`MSR`) only compile for `__aarch64__` |
| [`arch/arm/mmu_regs.hpp`](mmu_regs.md) | `structo::arch::arm::{sctlr, ttbcr_short, ttbcr_lpae, ttbr0_short, ttbr1_short, ttbr0_lpae, ttbr1_lpae, contextidr, scr, nsacr, hcr, vtcr, vttbr}` | Parsed/built views of ARMv7-A MMU control registers (short-descriptor and LPAE), the TrustZone `SCR`/`NSACR` security-state configuration registers that select which banked copy is live, and the Virtualization Extensions' hypervisor stage-2 registers `HCR`/`VTCR`/`VTTBR`; `read()`/`write()` only compile for `__arm__` (not AArch64) |
| [`arch/riscv/mmu_regs.hpp`](mmu_regs.md) | `structo::arch::riscv::{satp, satp_mode}` | Parsed/built view of the RV64 `satp` CSR (Sv39/Sv48/Sv57 mode, ASID, root PPN); `read()`/`write()` only compile for `__riscv` |
| [`arch/x86/mmu_regs.hpp`](mmu_regs.md) | `structo::arch::x86::{cr0, cr3, cr4, efer}` | Parsed/built views of x86/x86-64 `CR0`/`CR3`/`CR4` and the `EFER` MSR; `read()`/`write()` only compile for `__i386__`/`__x86_64__` |
| [`arch/x86/hw_rng.hpp`](hw_rng.md) | `structo::arch::x86::{rdrand_rng, rdseed_rng}` | `hw_rng_traits` backends for `RDRAND`/`RDSEED`, with `CPUID`-based `is_available()`; only compile their real asm for `__i386__`/`__x86_64__` |
| [`arch/arm64/hw_rng.hpp`](hw_rng.md) | `structo::arch::arm64::{rndr_rng, rndrrs_rng, cntpct_rng, cntvct_rng}` | `hw_rng_traits` backends for `FEAT_RNG`'s `RNDR`/`RNDRRS` (`ID_AA64ISAR0_EL1`-based `is_available()`), plus weak `CNTPCT_EL0`/`CNTVCT_EL0` jitter-combiner fallbacks for cores without `FEAT_RNG`; only compile their real asm for `__aarch64__` |
| [`arch/arm/hw_rng.hpp`](hw_rng.md) | `structo::arch::arm::{cntpct_rng, cntvct_rng}` | Weak `CNTPCT`/`CNTVCT` jitter-combiner fallback backends (ARMv7-A has no baseline hardware-RNG instruction); `ID_PFR1.GenTimer`-based `is_available()`; only compile their real asm for `__arm__` |
| [`arch/riscv/hw_rng.hpp`](hw_rng.md) | `structo::arch::riscv::seed_rng` | `hw_rng_traits` backend for the RISC-V Zkr `seed` CSR, accumulating four 16-bit `ES16` samples per 64-bit draw per the architecture's `csrrw`-swap-with-zero protocol; only compiles its real asm for `__riscv` |
| [`prng.hpp`](prng.md) | `structo::prng::{splitmix64, xoshiro256ss, pcg32}` | Small, fast, deterministic pseudo-random generators, each directly seedable and each with a `from_hw_rng(hw_rng_ref, ...)` factory drawing its initial state from a hardware entropy source |

## Debugging utilities

| Header | Type(s) | One-line summary |
|---|---|---|
| [`debug_symtab.hpp`](debug_symtab.md), [`debug_symtab_resolver.hpp`](debug_symtab.md) | `debug_symtab_view`, `debug_symtab_resolver_tag` | Allocation-free decoder for the compressed `DSYM` debug symbol table blob format; `try_resolve()` recovers symbol names a release `strip` removed from the shipped binary, from a blob built offline by `scripts/elf_symtab_to_blob.py` and loaded via a caller-owned external memory block; `debug_symtab_resolver.hpp` adapts it to microfmt's `symbol_resolver_traits<Tag>` |

## Synchronization & interrupt/preemption guards

| Header | Type(s) | One-line summary |
|---|---|---|
| [`sync/irq_guard.hpp`](irq_guard.md) | `irq_guard<Traits>`, `irq_locked<T, Traits>`, `critical_section_token`, `with_irq_disabled` | RAII interrupt-disable guard, proof-token-gated data wrapper, and functional helper for interrupt-safe kernel code |
| [`arch/arm/irq_guard.hpp`](irq_guard.md), [`arch/arm64/irq_guard.hpp`](irq_guard.md), [`arch/riscv/irq_guard.hpp`](irq_guard.md), [`arch/x86/irq_guard.hpp`](irq_guard.md) | `structo::arch::{arm,arm64,riscv,x86}::irq_traits` | Concrete `irq_guard<Traits>` policies masking ARM CPSR `AIF`, AArch64 `DAIF`, RISC-V `sstatus.SIE`, and x86 `RFLAGS.IF` respectively; each only compiles on its own target architecture |
| [`sync/preemption_guard.hpp`](preemption_guard.md) | `preemption_guard<Traits>`, `preempt_locked<T, Traits>`, `preemption_disabled_token`, `with_preemption_disabled` | RAII preemption-disable guard, proof-token-gated data wrapper, and functional helper, mirroring `irq_guard` for scheduler preemption instead of interrupts |
| [`sync/core_pin_guard.hpp`](core_pin_guard.md) | `core_pin_guard<Traits>`, `with_cpu_pinned` | RAII guard pinning the calling thread to its current CPU core for its lifetime (migration prevention, not interrupt/preemption exclusion), exposing which CPU it pinned to |
| [`arch/world_switch_guard.hpp`](world_switch_guard.md) | `world_switch_guard<Traits>`, `with_world_switch` | RAII guard saving a `Traits`-defined group of non-banked registers on entry and restoring them on exit, bracketing a security-domain/world switch (e.g. ARM TrustZone Secure/Non-secure) that may clobber shared, unbanked state such as AArch64's EL1 system registers |
| [`sync/core_rendezvous_barrier.hpp`](core_rendezvous_barrier.md) | `core_rendezvous_barrier<Traits>` | Reusable, spin-only SMP rendezvous point for exactly `num_cores` participants, invoking a caller-supplied callback once per spin iteration on every non-leader core while it waits |
| [`sync/early_rendezvous_barrier.h`](early_rendezvous_barrier.md) / [`.hpp`](early_rendezvous_barrier.md) | `structo_early_rendezvous_wait`, `early_rendezvous_wait<Traits>` | Trivial, GCC/Clang-only, purely-static-storage SMP rendezvous barrier (plain C struct + `__atomic_*` builtins) usable before `.init_array`/global constructors have run |
| [`sync/lock_striping.hpp`](lock_striping.md) | `lock_striping<N, LockT>` | FreeBSD-`pa_lock[]`-style shared, fixed-size array of `N` locks selected by hashing an address, so many protected objects can share a small pool of locks instead of each embedding its own; exclusive (`lock_for`/`try_lock_for`), shared (`shared_lock_for`/`try_shared_lock_for`, requires `LockT::lock_shared`), and seqlock-style optimistic-read (`sequence_for`/`validate_for`) access to the same stripe table |
| [`backref_ptr.hpp`](backref_ptr.md) | `backref_ptr<T, Cell>`, `embedded_mutex_cell<T, MutexT>`, `embedded_rw_cell<T, SharedMutexT>`, `embedded_seqlock_cell<T>`, `striped_mutex_cell<T, N, LockT, Tag>`, `striped_rw_cell<T, N, SharedLockT, Tag>`, `striped_seqlock_cell<T, N, LockT, Tag>` | Non-owning, safely-invalidated backpointer (e.g. FreeBSD's `vm_page->object`); the pointer is only ever read/written through a `guard` returned by `lock()`/`try_lock()`/`shared_lock()`/`read_unlocked()`, which composes one of `reloco`'s existing `guarded_mutex`/`rw_lock`/`guarded_seqlock` types (embedded per instance) or `lock_striping` (looked up in a shared static table, zero lock bytes per instance) |

