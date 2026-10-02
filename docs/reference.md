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
| `boot_memory_map.hpp` | `boot_memory_map<Capacity, PhysInt>` | Fixed-capacity physical memory map (`full`/`free` `reloco::region_set`s) decoded from a devicetree blob via `structo::fdt::try_extract_memory`; `try_from_dtb()` builds it in one call, `try_largest_free_region()`/`free_bytes()` answer the two queries a first-stage allocator needs |
| `device_tree.hpp` | `device_tree`, `device_tree_storage<NodeCapacity, PhandleCapacity, StackDepth>` | `structo::fdt::fdt_reader` + `structo::fdt::fdt_index` bundle over caller-owned `device_tree_storage`; `try_open()` builds both from a blob, `try_find_property()`/`try_bootargs()`/`try_stdout_path()` cover the `/chosen`-node lookups a boot path reaches for first |
| `compat_sg.hpp` | `sg_descriptor_layout<StorageType, PfnField, OffsetField, LengthField, LastFlagField, HeaderSize, LengthUnit>`, `chained_sg_layout<StorageType, PfnField, OffsetField, LengthField, LastFlagField, ChainFlagField, HeaderSize>`, `length_unit_bytes`, `length_unit_pages`, `compact_sg_codec<Layout, PageTraits, SpaceTag>`, `chained_sg_codec<Layout, PageTraits, SpaceTag>`, `two_level_sg_codec<L1Layout, L2Layout, PageTraits, SpaceTag>` | Compile-time scatter-gather descriptor layouts and codecs for compact, chained, and two-level representations; `LengthUnit` selects whether `LengthField` is a literal byte count or a whole-page count |
| `phys_addr.hpp` | `default_phys_space`, `host_phys_space`, `guest_phys_space`, `dma_bus_space`, `secure_phys_space`, `nonsecure_phys_space`, `root_phys_space`, `realm_phys_space`, `phys_addr<T, SpaceTag, PhysInt>`, `dmap_mapper<VirtBase, PhysSize, ExpectedSpace, PhysBase>`, `dmap_ptr<T, Mapper, PhysInt>` | Typed physical addresses, address-space tags, and direct-map pointer conversion |
| `target_ptr.hpp` | `user_space`, `kernel_space`, `guest_vm_space`, `realm_space`, `secure_world_space`, `target_ptr_space_traits<SpaceTag>`, `target_ptr<T, SpaceTag, PtrType>` | Typed pointer into another *virtual* address space (user/kernel/Realm/guest VM/...), valid only in the current execution context; fallible, page-fault-aware materialize/store accessors (with `_nofault` variants) and Rust-style checked/wrapping/saturating pointer arithmetic |
| `io_address.hpp` | `default_io_space`, `port_io_space`, `device_io_space`, `secure_io_space`, `nonsecure_io_space`, `realm_io_space`, `hypervisor_io_space`, `io_address<T, SpaceTag, IoInt>`, `reg_traits<Size>`, `io_math` | Typed, tagged address into a device-register space (port I/O, MMIO, ARM device memory, ...); pure address tagging and compile-time-checked offset arithmetic, plus register-width-aware offset math, no I/O access itself |
| `io_space_ref.hpp` | `io_space_ref<SpaceTag>`, `io_space_traits<Backend>` | Type-erased, non-owning handle performing fixed-width loads/stores and `rep insb`/`outsb`-style string I/O over an `io_address`, via a customizable backend |
| `hw/uart_ref.hpp` | `structo::hw::uart_ref`, `structo::hw::uart_traits<Backend>`, `structo::hw::uart_config` | Type-erased, non-owning handle over a basic (interrupt-free, polled) UART's operations and settings -- configure/tx_ready/rx_ready/put_byte/get_byte/write/read_available -- via a customizable backend |
| `hw/rng.hpp` | `structo::hw::hw_rng_ref`, `structo::hw::hw_rng_traits<Backend>` | Type-erased, non-owning handle over a hardware random/entropy source -- `try_generate64`/`try_generate32`/`try_fill`, with bounded retry on a backend's transient "not ready yet" condition -- via a customizable backend |
| `hw/rng_combinator.hpp` | `structo::hw::hw_rng_combinator` | Combines several `hw_rng_ref` sources (e.g. a real hardware RNG plus weak fallback jitter sources) by `XOR`-folding every successful draw and avalanche-mixing the result; itself bindable through another `hw_rng_ref` |
| `hw/timer_ref.hpp` | `structo::hw::timer_ref`, `structo::hw::timer_traits<Backend>`, `structo::hw::timer_mode`, `structo::hw::timer_capabilities` | Type-erased, non-owning handle over a hardware countdown/interval timer -- arm one-shot/periodic, cancel, is_active, poll for expiry, query remaining time/capabilities -- via a customizable backend; `try_wait`/`wait` follow Rust `embedded-hal`'s `nb::Result` non-blocking-poll shape and periods/readback use `reloco::duration` |
| `async_kernel_object.hpp` | `async_kernel_object<Traits, Capacity>` | Memory-safe, owning base for any kernel object whose completion fires asynchronously (interrupt, other CPU, softirq, timer-wheel sweep, ...) -- `try_submit`/`cancel`/`drain`/`deactivate` plus `is_pending`/`is_active`/`is_firing`, atomic `callout(9)`-style cancel/drain semantics, via a compile-time `Traits::submit`/`cancel` policy; subsystem-agnostic base for a future `callout`-like timer object |
| `callout.hpp` | `callout<Subsystem, Capacity>` | FreeBSD-`callout(9)`-like one-shot/periodic deferred callback built on `async_kernel_object` -- `reset`/`reset_periodic`/`stop`/`drain`/`deactivate`/`pending`/`active`/`firing`, callback invoked as `void(callout &)`, scheduling policy supplied by a `Subsystem` ("callout subsystem") implementing `submit(Obj &, reloco::duration)`/`cancel(Obj &)` |
| `runqueue.hpp` | `fifo_runqueue<Entry, Hook>`, `priority_list_runqueue<Entry, Hook, Priority>`, `priority_bucket_runqueue<Entry, Hook, Priority, NumPriorities>` | Three allocation-free, intrusive runqueue policies sharing one `enqueue`/`dequeue`/`peek`/`remove`/`empty`/`size` surface: plain FIFO, a priority-sorted intrusive list, and a Linux-"O(1) scheduler"-style per-priority bucket array plus bitmap (`__builtin_ctzll`-scanned) for O(1) highest-priority lookup |
| `sched.hpp` | `noop_sched<Entry, Hook, PerCpu>`, `fixed_priority_sched<Entry, Hook, Priority, NumPriorities, PerCpu>`, `sched_ule<Entry, Hook, Priority, State, NumPriorities, PerCpu>`, `sched_4bsd<Entry, Hook, Priority, State, NumPriorities, PerCpu>` | Four stateless, tickless scheduling policies built on `runqueue.hpp`: plain FIFO, static-priority `SCHED_FIFO`/`SCHED_RR`-like, and simplified FreeBSD-`SCHED_ULE`/`SCHED_4BSD`-like; per-task bookkeeping lives in caller-owned `Entry` fields, per-CPU state is resolved through a caller-supplied `PerCpu` trait (`structo::arch::per_cpu_ptr` fits directly) |
| `pfn_translator.hpp` | `phys_pfn<SpaceTag, PageTraits, PhysInt>` | Typed physical page-frame number and address conversion |
| `phys_page.hpp` | `page_traits<Size, Shift>`, `os_traits_base<Derived, OsPage>`, `page_view<PageTraits, OsTraits>` | Page-size traits and an OS-page-backed physical page view |
| `phys_translator.hpp` | `phys_translator<Policy>` | Policy-based virtual/physical address translator |
| `region_set.hpp` | `memory_region<PhysInt>`, `region_set<Capacity, PhysInt>` | Fixed-capacity collection of physical memory regions |
| `sg_list.hpp` | `sg_entry<SpaceTag, PhysInt>`, `sg_list<Container>`, `sg_list_cursor<PhysInt>` | Scatter-gather entries and container, plus a type-erased single-pass cursor splitting entries into caller-sized chunks (e.g. for remapping memory) |
| `sg_translator.hpp` | `sg_translator` | Policy-based scatter-gather translator |
| `fdt_reader.hpp` | `fdt_reader`, `mem_reserve_iterator` | Bounds-checked, `iterator_adaptor`-based read-only view over a caller-owned Flattened Device Tree (DTB) span, yielding `result<fdt_event>` per struct-block token |
| `fdt_writer.hpp` | `fdt_writer` | Move-only, fallibly-constructed streaming writer for Flattened Device Tree (DTB, `/dts-v1/`) blobs into a caller-owned span, with sticky error propagation |
| `fdt_index.hpp` | `fdt_index<Container>`, `fdt_index_node`, `fdt_index_phandle_entry`, `fdt_index_child_iterator<NodeContainer>`, `fdt_index_property_iterator` | Random-access index over an `fdt_reader` blob, built once (iteratively, never recursively) into caller-supplied `Container<T>` buffers, giving `O(1)` parent lookup and child iteration without descending into subtrees, plus `O(log n)` phandle-to-node lookup |
| `fdt_memory.hpp` | `try_extract_memory` | Extracts a devicetree's physical memory description straight off `fdt_reader`'s single-pass streaming API (no `fdt_index`, safe to call very early in boot) into caller-provided `region_set`s |
| `arch/fdt_cpu_map.hpp` | `try_populate_hw_id_lut_from_fdt` | Decodes a devicetree's `/cpus` node straight off `fdt_reader`'s single-pass streaming API (no `fdt_index`, safe to call before any CPU index exists) into a caller-provided `hw_id_lut`, assigning sequential logical CPU indices in devicetree order |
| `buddy_allocator.hpp` | `buddy_allocator<FreeList, OsPage, MaxOrder>` | Power-of-two buddy page allocator over a caller-supplied intrusive free list and `OsPage` type, with DMA/hardware-constrained and greedy allocation variants |
| `reloco_ipc_ring.h` / `reloco_ipc_ring.hpp` | `reloco_ipc_spsc_page`, `reloco_ipc_producer`, `reloco_ipc_consumer` (C ABI); `reloco::ipc_producer`, `reloco::ipc_consumer` (C++ wrapper) | Cross-process, allocation-free single-producer/single-consumer byte-stream ring buffer over a shared-memory page; usable standalone from a Linux/FreeBSD kernel module, with a zero-copy typestate-checked C++ transaction API |
| `arch/cpu_index.hpp` | `cpu_index<Tag>`, `uniprocessor_tag` | Zero-overhead CRTP-style "which logical CPU is this" wrapper plus core operations (yield, WFE/SEV, BSP detection, hardware ID), delegated to a caller-provided `Tag` policy |
| `arch/hw_id_map.hpp` | `hw_id_lut<HwId, MaxCpus, L1Size, Hash>`, `default_hw_id_hash` | Allocation-free, two-tier (hash-filtered L1 + sorted L2 fallback) lookup table mapping a hardware ID (e.g. MPIDR_EL1) to a logical CPU index |
| `arch/asid_allocator.hpp` | `asid_allocator<Tag, MaxActive>`, `tagged_asid<Tag>`, `process_asid_tag`, `vmid_tag` | Generation-counted bitmap ASID/VMID/PCID allocator (runtime ASID width, allocator-backed bitmap storage), returning an opaque, per-Tag `tagged_asid<Tag>` handle that cannot be confused between a process ASID and a VM's VMID |
| `arch/cpu_mask.hpp` | `cpu_mask<Tag, MaxCpus>`, `physical_cpu_tag`, `vcpu_tag` | Fixed-capacity, compile-time-sized, tagged CPU/vCPU bitmask with Rust `bitflags`-style set algebra, intrinsic-backed bit-scanning, and a lock-free GCC/Clang `__atomic_*`-builtin subset (including atomic find-first-set and a CAS-based find-and-set bitmap allocator) |
| `arch/mm_asid_context.hpp` | `mm_asid_context<AsidTag, MaxCpus, CpuTag>` | Reusable per-address-space ASID/cpu-residency tracker composing `asid_allocator` + `cpu_mask`: a cached `tagged_asid<AsidTag>` plus a lock-free cpu mask of every core that may still hold a stale, tagged TLB entry for it |
| `arch/page_table_traits.hpp` | `page_table_level<IndexBits, Shift, AllowsLeaf>`, `page_table_levels<LeafPageTraits, VaBits, Levels...>`, `page_table_entry<Tag, Int>`, `page_table_entry_traits<Tag>` | Compile-time, self-consistency-checked per-level index-decoding traits for a multi-level hardware page table (ARM64/x86-64/RISC-V-shaped), plus an opaque tagged raw PTE handle and a declared-but-undefined entry-encode/decode extension point for a future walker layer |
| `arch/page_table_range.hpp` | `page_table_level_range<Levels, LevelIndex, Entry, AddrInt>`, `make_level_range<Levels, LevelIndex>(...)` | Forward-iterable (and Rust-style `.iter()`-able) adapter over exactly the entries of one caller-supplied page-table level's span that a `[start, end)` virtual-address range touches, built on `page_table_traits.hpp`; performs no mapping/allocation of its own |
| `arch/page_table_occupancy.hpp` | `page_table_occupancy<CounterInt>` | O(1) live-entry counter for one page-table level's table (increment/decrement per entry transition), so a `page_table_range.hpp`-based unmap walk can free now-empty tables without an O(entry_count) rescan per table per unmap |
| `arch/arm64/page_table_traits.hpp` | `structo::arch::arm64::{level1, level2, level1_16k, level2_16k, level1_64k, level2_64k}` | Ready-made `page_table_levels<...>` configs for ARMv8-A/AArch64 (VMSAv8-64), covering all three translation granules (4KB/16KB/64KB) at the two most common translation-table starting levels |
| `arch/arm/page_table_traits.hpp` | `structo::arch::arm::lpae::{level1, level2}` | Ready-made `page_table_levels<...>` configs for ARMv7 with the Large Physical Address Extension (LPAE), 4KB granule (LPAE's only granule) |
| `arch/riscv/page_table_traits.hpp` | `structo::arch::riscv::{sv39, sv48, sv57}` | Ready-made `page_table_levels<...>` configs for RISC-V Sv39/Sv48/Sv57, where every level (including the root) allows an early-terminating leaf PTE |
| `arch/x86/page_table_traits.hpp` | `structo::arch::x86::{i386, pae, long_mode_4level, long_mode_5level}` | Ready-made `page_table_levels<...>` configs for x86/x86-64 paging modes, with huge-page-capable levels expressed via `AllowsLeaf` |
| `arch/pte_field.hpp` | `pte_bit_field<LowBit, NumBits, Int>` | Generic named-bit-field primitive (`get`/`set`/`test`/`set_bit`) every per-architecture PTE field descriptor header below is built from |
| `arch/arm64/pte_stage1.hpp`, `arch/arm64/pte_stage2.hpp` | `structo::arch::arm64::{stage1_ns_tag, stage1_secure_tag, stage1_secure_el2_tag, stage2_tag, stage2_secure_tag}` (+ `page_table_entry_traits<>` specializations) | Real `page_table_entry_traits<Tag>` specializations for AArch64 stage-1/stage-2 VMSA descriptors, covering Non-secure, Secure (TrustZone), Secure EL2 (sEL2), and hypervisor-staging (stage-2) regimes, with leaf/block `final_level` selection |
| `arch/arm/pte_stage1.hpp`, `arch/arm/pte_stage2.hpp` | `structo::arch::arm::lpae::{stage1_ns_tag, stage1_secure_tag, stage2_tag}` | ARMv7-LPAE equivalents of the above (bit-for-bit compatible format, 4KB-only granule, no sEL2 equivalent) |
| `arch/riscv/pte.hpp` | `structo::arch::riscv::{pte_tag, pte_g_stage_tag}` | `page_table_entry_traits<Tag>` specializations for RISC-V's single shared S-stage/G-stage PTE bit layout |
| `arch/x86/pte.hpp` | `structo::arch::x86::{pte_tag, npt_tag}` | `page_table_entry_traits<Tag>` specializations for ordinary x86/x86-64 paging and AMD NPT (identical bit layout), including the PS-vs-PAT bit-7 `final_level` caveat |
| `arch/x86/pte_ept.hpp` | `structo::arch::x86::ept_tag` | `page_table_entry_traits<Tag>` specialization for Intel EPT's distinct encoding (no dedicated present bit; derived from R/W/X) |
| `arch/lazy_context.hpp` | `lazy_context<Traits, CpuId>`, `lazy_context_switcher<Traits, CpuId>`, `static_per_cpu_storage<MaxCpus, Context, CpuId>` | Lazy/deferred per-thread coprocessor-state (FPU, vector registers, ...) context-switching framework, saving/restoring hardware state only on first trapped use |
| `arch/per_cpu_ptr.hpp` | `per_cpu_ptr<Tag, T>` | Storage-free, type-safe resolver for a per-CPU `T*`, keyed by `Tag` and backed entirely by `Tag`'s own OS-specific per-CPU mechanism |
| `arch/per_thread_ptr.hpp` | `per_thread_ptr<Tag, T>` | Storage-free, type-safe resolver for the *running thread's own* `T*`, keyed by `Tag`; exposes no explicit-thread accessor by design |
| `arch/per_domain_ptr.hpp` | `per_domain_ptr<Tag, T>` | Storage-free, type-safe resolver for a per-domain `T*` (e.g. Arm RME World ID), keyed by `Tag` and backed entirely by `Tag`'s own per-domain mechanism |
| `sync/irq_guard.hpp` | `irq_guard<Traits>`, `irq_locked<T, Traits>`, `critical_section_token`, `with_irq_disabled` | RAII interrupt-disable guard, proof-token-gated data wrapper, and functional helper for interrupt-safe kernel code |
| `arch/arm/irq_guard.hpp`, `arch/arm64/irq_guard.hpp`, `arch/riscv/irq_guard.hpp`, `arch/x86/irq_guard.hpp` | `structo::arch::{arm,arm64,riscv,x86}::irq_traits` | Concrete `irq_guard<Traits>` policies masking ARM CPSR `AIF`, AArch64 `DAIF`, RISC-V `sstatus.SIE`, and x86 `RFLAGS.IF` respectively; each only compiles on its own target architecture |
| `arch/arm64/mmu_regs.hpp` | `structo::arch::arm64::{sctlr_el1, tcr_el1, ttbr0_el1, ttbr1_el1, mair_el1, scr_el3, hcr_el2, vtcr_el2, vttbr_el2}` | Parsed/built views of AArch64 EL1 MMU control registers, the EL3 TrustZone `SCR_EL3` register, and the EL2 hypervisor stage-2 registers `HCR_EL2`/`VTCR_EL2`/`VTTBR_EL2`; pure bit-field parse/build logic is portable and tested, `read()`/`write()` (real `MRS`/`MSR`) only compile for `__aarch64__` |
| `arch/arm/mmu_regs.hpp` | `structo::arch::arm::{sctlr, ttbcr_short, ttbcr_lpae, ttbr0_short, ttbr1_short, ttbr0_lpae, ttbr1_lpae, contextidr, scr, nsacr, hcr, vtcr, vttbr}` | Parsed/built views of ARMv7-A MMU control registers (short-descriptor and LPAE), the TrustZone `SCR`/`NSACR` security-state configuration registers that select which banked copy is live, and the Virtualization Extensions' hypervisor stage-2 registers `HCR`/`VTCR`/`VTTBR`; `read()`/`write()` only compile for `__arm__` (not AArch64) |
| `arch/riscv/mmu_regs.hpp` | `structo::arch::riscv::{satp, satp_mode}` | Parsed/built view of the RV64 `satp` CSR (Sv39/Sv48/Sv57 mode, ASID, root PPN); `read()`/`write()` only compile for `__riscv` |
| `arch/x86/mmu_regs.hpp` | `structo::arch::x86::{cr0, cr3, cr4, efer}` | Parsed/built views of x86/x86-64 `CR0`/`CR3`/`CR4` and the `EFER` MSR; `read()`/`write()` only compile for `__i386__`/`__x86_64__` |
| `arch/x86/hw_rng.hpp` | `structo::arch::x86::{rdrand_rng, rdseed_rng}` | `hw_rng_traits` backends for `RDRAND`/`RDSEED`, with `CPUID`-based `is_available()`; only compile their real asm for `__i386__`/`__x86_64__` |
| `arch/arm64/hw_rng.hpp` | `structo::arch::arm64::{rndr_rng, rndrrs_rng, cntpct_rng, cntvct_rng}` | `hw_rng_traits` backends for `FEAT_RNG`'s `RNDR`/`RNDRRS` (`ID_AA64ISAR0_EL1`-based `is_available()`), plus weak `CNTPCT_EL0`/`CNTVCT_EL0` jitter-combiner fallbacks for cores without `FEAT_RNG`; only compile their real asm for `__aarch64__` |
| `arch/arm/hw_rng.hpp` | `structo::arch::arm::{cntpct_rng, cntvct_rng}` | Weak `CNTPCT`/`CNTVCT` jitter-combiner fallback backends (ARMv7-A has no baseline hardware-RNG instruction); `ID_PFR1.GenTimer`-based `is_available()`; only compile their real asm for `__arm__` |
| `arch/riscv/hw_rng.hpp` | `structo::arch::riscv::seed_rng` | `hw_rng_traits` backend for the RISC-V Zkr `seed` CSR, accumulating four 16-bit `ES16` samples per 64-bit draw per the architecture's `csrrw`-swap-with-zero protocol; only compiles its real asm for `__riscv` |
| `prng.hpp` | `structo::prng::{splitmix64, xoshiro256ss, pcg32}` | Small, fast, deterministic pseudo-random generators, each directly seedable and each with a `from_hw_rng(hw_rng_ref, ...)` factory drawing its initial state from a hardware entropy source |
| `sync/preemption_guard.hpp` | `preemption_guard<Traits>`, `preempt_locked<T, Traits>`, `preemption_disabled_token`, `with_preemption_disabled` | RAII preemption-disable guard, proof-token-gated data wrapper, and functional helper, mirroring `irq_guard` for scheduler preemption instead of interrupts |
| `sync/core_pin_guard.hpp` | `core_pin_guard<Traits>`, `with_cpu_pinned` | RAII guard pinning the calling thread to its current CPU core for its lifetime (migration prevention, not interrupt/preemption exclusion), exposing which CPU it pinned to |
| `sync/core_rendezvous_barrier.hpp` | `core_rendezvous_barrier<Traits>` | Reusable, spin-only SMP rendezvous point for exactly `num_cores` participants, invoking a caller-supplied callback once per spin iteration on every non-leader core while it waits |
| `sync/early_rendezvous_barrier.h` / `.hpp` | `structo_early_rendezvous_wait`, `early_rendezvous_wait<Traits>` | Trivial, GCC/Clang-only, purely-static-storage SMP rendezvous barrier (plain C struct + `__atomic_*` builtins) usable before `.init_array`/global constructors have run |

## structo's own headers

- [`boot_memory_map.md`](boot_memory_map.md) -- DTB-derived physical memory map
- [`device_tree.md`](device_tree.md) -- `fdt_reader`/`fdt_index` bundle with `/chosen` helpers
- [`target_ptr.md`](target_ptr.md) -- typed pointers into another virtual address space (user/kernel/Realm/VM)
- [`io_address.md`](io_address.md) -- typed, tagged device-register addresses (port I/O, MMIO, ARM device memory)
- [`io_space_ref.md`](io_space_ref.md) -- type-erased device-register read/write access over an `io_address`
- [`uart_ref.md`](uart_ref.md) -- type-erased basic (polled) UART operations and settings, in `include/structo/hw/`
- [`hw_rng.md`](hw_rng.md) -- type-erased hardware RNG handle, per-architecture backends, and the entropy-source combinator
- [`timer_ref.md`](timer_ref.md) -- type-erased hardware countdown/interval timer, in `include/structo/hw/`
- [`async_kernel_object.md`](async_kernel_object.md) -- memory-safe, owning base for any asynchronously-completing kernel object (subsystem-agnostic, timer-independent)
- [`callout.md`](callout.md) -- FreeBSD-`callout(9)`-like one-shot/periodic deferred callback built on `async_kernel_object`
- [`runqueue.md`](runqueue.md) -- `fifo_runqueue`/`priority_list_runqueue`/`priority_bucket_runqueue`: pluggable, allocation-free intrusive runqueue policies
- [`sched.md`](sched.md) -- `noop_sched`/`fixed_priority_sched`/`sched_ule`/`sched_4bsd`: tickless, per-CPU-trait-driven scheduling policies built on `runqueue.hpp`
- [`prng.md`](prng.md) -- `splitmix64`/`xoshiro256ss`/`pcg32` pseudo-random generators, seedable from a hardware RNG

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

## Arch/sync building blocks (native to `jplcz_structo`)

The headers below are native to `jplcz_structo` (not moved from reloco)
and live under `include/structo/arch/` and `include/structo/sync/`:

- [`cpu_index.md`](cpu_index.md) -- logical-CPU resolution and core operations
- [`hw_id_map.md`](hw_id_map.md) -- hardware-ID-to-CPU-index lookup table
- [`asid_allocator.md`](asid_allocator.md) -- generation-counted bitmap ASID/VMID allocator with a tagged, opaque context-ID handle
- [`cpu_mask.md`](cpu_mask.md) -- fixed-capacity, tagged CPU/vCPU bitmask with Rust `bitflags` set algebra and lock-free atomics
- [`mm_asid_context.md`](mm_asid_context.md) -- per-address-space ASID + lock-free cpu-residency tracker composing `asid_allocator` and `cpu_mask`
- [`page_table_traits.md`](page_table_traits.md) -- compile-time, validated per-level page-table index-decoding traits and an opaque tagged PTE handle
- [`page_table_range.md`](page_table_range.md) -- forward-/Rust-iterable adapter walking one page-table level's caller-supplied span over a virtual-address range
- [`page_table_occupancy.md`](page_table_occupancy.md) -- O(1) live-entry counter enabling release of now-empty page tables during an unmap walk
- [`page_table_arch_configs.md`](page_table_arch_configs.md) -- ready-made `page_table_levels<...>` configs for ARM64, ARMv7-LPAE, RISC-V, and x86/x86-64, across every translation granule each architecture supports
- [`page_table_entry_fields.md`](page_table_entry_fields.md) -- per-architecture `page_table_entry_traits<Tag>` specializations with named field accessors, covering TrustZone/Secure, sEL2, and hypervisor-staging (stage-2/NPT/EPT) regimes
- [`fdt_cpu_map.md`](fdt_cpu_map.md) -- devicetree `/cpus` early CPU-index resolution into an `hw_id_lut`
- [`lazy_context.md`](lazy_context.md) -- lazy per-thread coprocessor-state context switching
- [`per_cpu_ptr.md`](per_cpu_ptr.md) -- storage-free, Tag-keyed per-CPU typed-pointer resolver
- [`per_thread_ptr.md`](per_thread_ptr.md) -- storage-free, Tag-keyed current-thread-only typed-pointer resolver
- [`per_domain_ptr.md`](per_domain_ptr.md) -- storage-free, Tag-keyed per-domain (e.g. Arm RME World ID) typed-pointer resolver
- [`irq_guard.md`](irq_guard.md) -- RAII interrupt-disable guard and critical-section helpers
- [`preemption_guard.md`](preemption_guard.md) -- RAII preemption-disable guard and critical-section helpers
- [`core_pin_guard.md`](core_pin_guard.md) -- RAII CPU-core pinning guard preventing migration
- [`core_rendezvous_barrier.md`](core_rendezvous_barrier.md) -- spin-only SMP rendezvous barrier with on-spin callback
- [`early_rendezvous_barrier.md`](early_rendezvous_barrier.md) -- purely-static-storage boot-phase SMP rendezvous barrier (C struct + compiler atomic builtins)
