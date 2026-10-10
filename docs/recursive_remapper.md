# Early remapper with the self-referencing page-table trick

Headers: `arch/recursive_remapper.hpp`, `arch/protection.hpp`, `arch/page_table_memory.hpp`, and the
last-level formats `arch/{x86,arm64,arm,riscv}/recursive_format.hpp` (+ shared `arch/vmsa_recursive_format.hpp`).

The remapper owns **one last-level page table** that maps *itself* as an ordinary page. The table is
then visible at a known virtual address, so the remapper edits its own entries with plain loads and stores
- no direct map of physical memory needed. The caller builds all levels above it.

```cpp
using namespace structo::arch;

// Last-level format: x86 PAE / long mode (512 entries, 4 KiB pages). Other formats:
//   x86::recursive_pte32_format, arm64::recursive_stage1_format<Granule, Tag, Regime, Policy>,
//   arm::lpae::recursive_stage1_format<>, arm::short_descriptor::recursive_small_page_format<>,
//   riscv::recursive_pte_format<Svpbmt>.
using format = x86::recursive_pte_format;
using phys = format::phys_type;

// TLB hook: invalidates by page through the trait layer; Barrier::complete() would be
// `dsb ish; isb` on ARM. no_tlb does nothing (MMU off / tests).
using tlb = tlb_binding<x86::tlb_tag>;
using remapper = recursive_remapper<format, tlb>;

// One page that becomes the last-level table. `view` is how the CPU can write it right now
// (identity mapping); `table_phys` is its physical address (page aligned).
// 0xFFFF'FFFF'C000'0000 is the VA where this table's 2 MiB span starts (span aligned);
// 3 is the slot that maps the table itself, so the table appears at base + 3 * 4096.
auto m = remapper::try_initialize(view, table_phys, 0xFFFF'FFFF'C000'0000, 3);

// The caller links table_phys from its upper-level tables and activates them (CR3 / TTBR / satp).

// Map 16 KiB read/write, non-executable, write-back. map_flags::replace would overwrite
// existing pages instead of failing with already_exists.
(void)m->try_map(0xFFFF'FFFF'C001'0000, phys{0x20'0000}, 16 << 10, protection::kernel_data());

// Make it read/execute (frames kept), then ask what is mapped there.
(void)m->try_protect(0xFFFF'FFFF'C001'0000, 16 << 10, protection::kernel_text());
auto q = m->query(0xFFFF'FFFF'C001'1234); // physical address, effective protection, page size
```

## Supported formats

| Format | Entry | Entries/table | Page |
|---|---|---|---|
| `x86::recursive_pte_format` (PAE, 4/5-level) | 64-bit | 512 | 4 KiB |
| `x86::recursive_pte32_format` (i386) | 32-bit | 1024 | 4 KiB |
| `arm64::recursive_stage1_format<page_4k/16k/64k, Tag, Regime>` | 64-bit | 512 / 2048 / 8192 | 4 / 16 / 64 KiB |
| `arm::lpae::recursive_stage1_format<>` | 64-bit | 512 | 4 KiB |
| `arm::short_descriptor::recursive_small_page_format<>` | 32-bit | 256 (1 KiB table) | 4 KiB |
| `riscv::recursive_pte_format<Svpbmt>` (Sv39/48/57) | 64-bit | 512 | 4 KiB |

Stage 2 / NPT / EPT / G-stage are not covered: their tables are walked in the guest-physical space,
which the CPU cannot use to reach them.

## Protection flags

`protection` combines `kprot` (privileged access), `uprot` (unprivileged access), `scope`, `cache_mode` and
`security_state`. Only cross-category `operator|` exists (`kprot::write | uprot::read` compiles,
`kprot::read | kprot::write` does not). Every format *legalizes* the request into the nearest encoding that
is never more permissive; accessed/dirty (`A`/`AF`/`D`) are always set. The per-format rules and the few
documented exceptions (no NX on i386, single XN on LPAE / ARMv7 short descriptors) are at the top of each
format header. Unsupported requests (e.g. `write_combining` on x86, a non-default memory type on RISC-V
without Svpbmt) fail with `unsupported_operation` instead of being weakened.

```cpp
// Kernel RW with user read-only, device memory type.
protection p = protection{}.with_kernel(kprot::write).with_user(uprot::read).with_cache(cache_mode::device);

// SCTLR.WXN: report what the hardware will really enforce (exec dropped where writable).
protection q = protection::kernel_data().with_kernel(kprot::write_exec).enforce_policy<mmu_policy<true>>();
```

## Memory attributes (MAIR)

`arch/mair.hpp` builds `MAIR_ELx` (AArch64) and `MAIR0`/`MAIR1` (ARM LPAE) values; `mair_layout` tells the
AArch64/LPAE formats which slot holds which `cache_mode` (the default is `default_mair`).

```cpp
// Eight 8-bit slots; descriptors pick one through AttrIndx[2:0].
constexpr mair_value mair = mair_value{}
    .set<0>(mair_attr::device_nGnRnE())                                    // strongly ordered MMIO
    .set<1>(mair_attr::normal(mair_cache::non_cacheable))                  // uncached / write-combining RAM
    .set<2>(mair_attr::normal(mair_cache::write_back, mair_alloc::read_write)); // regular RAM

// Slots for: write_back, write_through, uncached, write_combining, device, device_ordered.
// Write-through has no slot of its own here, so it is pointed at the write-back slot.
using layout = mair_layout<mair.raw, 2, 2, 1, 1, 0, 0>;
using format = arm64::recursive_stage1_format<page_4k, arm64::stage1_ns_tag<page_4k>, vmsa_regime::el1_el0,
                                              default_mmu_policy, layout>;

// AArch64: write mair.raw to MAIR_EL1.  ARM LPAE: MAIR0 = mair.lo(), MAIR1 = mair.hi().
```

## Notes

- The self entry is a normal kernel read/write non-executable leaf; the slot is reserved (`invalid_argument`).
- The table must fit in one page. For ARM short-descriptor (1 KiB table) the caller owns the whole 4 KiB page.
- `try_initialize` clears the table; `recursive_remapper(table_phys, base, self, window)` adopts one that already
  holds the self entry.
- ARM changes other than AP/PXN/UXN/AF use break-before-make (clear, flush, barrier, write). RISC-V formats
  request a flush after validating an entry (`flush_on_map`).
- `Window` converts the window VA to a CPU view (default: the VA is the pointer). Not thread safe; on SMP,
  x86/RISC-V need an IPI shootdown in the `Tlb` you pass in.
- Blocks, promotion/demotion and the upper levels are the caller's job.
