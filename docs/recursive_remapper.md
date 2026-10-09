# Early remapper with the self-referencing page-table trick

Headers: `arch/recursive_remapper.hpp`, `arch/protection.hpp`, `arch/page_table_memory.hpp`,
`arch/x86/recursive_format.hpp`, `arch/arm64/recursive_format.hpp`.

A bootstrap tool: it edits the *live* page tables without a direct map of physical memory,
by reaching every table through one root slot that points back at the root.

```cpp
using namespace structo::arch;

// Page-table format: x86-64, 4 levels. (AArch64: arm64::recursive_stage1_format<>.)
using format = x86::recursive_pte_format<>;
using phys = format::phys_type;

// Bootstrap table pool: caller storage + the physical address of its first word
// (4 KiB aligned). Only for early boot; a running kernel supplies its own allocator
// with the same three members (try_allocate_table / free_table / table).
alignas(4096) static std::uint64_t pool[512 * 64];
table_arena<phys> arena(reloco::span<std::uint64_t>(pool, 512 * 64), phys{0x8010'0000});

// TLB hook: invalidates by page through the trait layer; `complete()` is the barrier
// (ARM: dsb ish; isb). Use no_tlb while the MMU is still off.
using tlb = tlb_binding<x86::tlb_tag>;

// Window: converts a window virtual address into a pointer. identity_window
// reinterprets the address, which is right once the new root is live.
using remapper = recursive_remapper<format, table_arena<phys>, tlb, identity_window>;

// Creates the root and points its slot 510 at itself. Slot 510 (and the whole 512 GiB
// it covers) is now the window and cannot be mapped for anything else.
auto m = remapper::try_create(arena, 510);

// ... load m->root() into CR3 ...

// Map 4 MiB read/execute for the kernel. The remapper picks 2 MiB blocks where the
// address and size allow, 4 KiB pages otherwise. `no_huge` avoids 1 GiB blocks on CPUs
// without Page1GB; `replace` would overwrite existing leaves; `no_large` forces 4 KiB.
(void)m->try_map(0xFFFF'8000'0000'0000, phys{0x20'0000}, 4 << 20,
                 protection::kernel_text(), map_flags::no_huge);

// Flip data to read-only (frames are kept). Fails with not_found over holes.
(void)m->try_protect(0xFFFF'8000'0000'0000, 4 << 20, protection::kernel_rodata());

// Query: physical address, effective (legalized) protection, block size, level.
auto q = m->query(0xFFFF'8000'0000'1000);
```

## Protection flags

`protection` combines `kprot` (privileged access), `uprot` (unprivileged access), `scope`
(global / per address space), `cache_mode` and `security_state`. Only cross-category
`operator|` exists, so `kprot::write | uprot::read | cache_mode::uncached` compiles and
`kprot::read | kprot::write` does not.

```cpp
// Kernel RW with user read-only, device memory type.
protection p = protection{}.with_kernel(kprot::write).with_user(uprot::read).with_cache(cache_mode::device);

// SCTLR.WXN: report what the hardware will really enforce (exec dropped where writable).
protection q = protection::kernel_data().with_kernel(kprot::write_exec).enforce_policy<mmu_policy<true>>();
```

Each format legalizes the request internally to the nearest encoding that is **never more
permissive**. Kernel and user attributes need not be representable independently:

| Architecture | Rule |
|---|---|
| x86-64 | One US, RW and XD bit. User-visible: write needs user *and* kernel write, exec follows `uexec`; otherwise kernel bits. XD set whenever not executable (`EFER.NXE`). |
| AArch64 EL1&0 | AP[2:1] + PXN + UXN. Kernel read implied; "kernel RW + user RO" becomes RO/RO; user write is downgraded to read when the kernel is read-only; user pages are non-global. `mmu_policy<Wxn, Uwxn>` applies WXN/UWXN. |
| AArch64 flat (EL2/EL3) | Single XN bit, AP[1]=RES1, no user access (`invalid_argument`). |

Accessed/dirty (`A`, `AF`, `D`) are always set. Unsupported requests fail instead of being
silently weakened: `write_combining` on x86 with the reset PAT, `secure` on a non-secure format.

## Notes

- Every table descriptor is also a valid page descriptor (it is used as the last-level entry
  of a window walk). The formats guarantee this: on AArch64 `make_table` carries AF=1,
  write-back, kernel-only, PXN/UXN. On x86 the self entry is supervisor-only and XD, which
  makes the whole window kernel-only and non-executable.
- Each table must be one granule long (`static_assert`), so the 16K/64K granules and enlarged
  roots are not supported by this remapper.
- **RISC-V cannot use the trick** (a pointer PTE at the last level is invalid); use a direct map.
- No demotion: `try_unmap`/`try_protect` need ranges aligned to existing blocks. Empty tables
  are not reclaimed. A failed `try_map` is rolled back.
- AArch64 changes that alter anything other than AP/PXN/UXN/AF use break-before-make
  (`flush` + `complete` between clearing and rewriting the entry).
- Not thread safe. On SMP, x86 and RISC-V need an IPI shootdown in the `Tlb` type you pass in.
