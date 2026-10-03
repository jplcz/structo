# Architecture MMU control-register parsers/builders

`structo/arch/{arm,arm64,riscv,x86}/mmu_regs.hpp` give you small, named
value types for the handful of hardware control registers that
configure a CPU's translation hardware (enabling/disabling the MMU,
selecting input/output address sizes and granules, pointing at the root
translation table, and -- on ARM -- selecting the Secure/Non-secure
world). Each register is a flat integer where fixed-position bit ranges
each mean something different -- the same problem `arch/pte_field.hpp`
solves for page-table entries, just applied to control registers
instead, and built on the identical `structo::arch::pte_bit_field`
primitive.

## Split: pure parse/build vs. real register access

Every value type below is usable entirely as pure, portable bit
arithmetic over an in-memory `raw` value -- `from_raw()`, the named
`foo()`/`set_foo(...)` accessor pairs, and `raw` itself have no
architecture dependency and compile (and are unit tested) on any host.

```cpp
using namespace structo::arch::arm64;

tcr_el1 tcr{};
tcr.set_t0sz(16).set_t1sz(16).set_tg0(0).set_tg1(2).set_ips(0b101);

sctlr_el1 sctlr{};
sctlr.set_mmu_enabled(true).set_dcache_enabled(true).set_icache_enabled(true);
```

`read()`/`write()` round-trip a value type through the *real* system
register via inline `MRS`/`MSR` (AArch64), `MRC`/`MCR`/`MRRC`/`MCRR`
(ARMv7-A), `csrr`/`csrw` (RISC-V), or `mov %crN`/`rdmsr`/`wrmsr`
(x86/x86-64). They are only compiled on a genuine target of the matching
architecture (`__aarch64__`, `__arm__` and not AArch64, `__riscv`, or
`__i386__`/`__x86_64__` respectively) -- on any other host, the header
still defines every value type and its pure accessors (so cross-compiled
header checks stay clean), it just omits `read()`/`write()`. This access
is privileged (`EL1`+ on ARM, S-mode+ on RISC-V, Secure state on
`SCR`/`NSACR`, CPL0 on x86) and cannot be exercised from an unprivileged
test process, so only the pure parse/build logic has unit tests --
mirroring the established convention in `arch/<arch>/irq_guard.hpp`.

```cpp
#if defined(__aarch64__)
tcr.write();
sctlr.write();
#endif
```

## `structo/arch/arm64/mmu_regs.hpp` -- AArch64 EL1, EL2, EL3

- `sctlr_el1` -- MMU enable, D/I-cache enable, alignment checks, WXN.
- `tcr_el1` -- T0SZ/T1SZ, TG0/TG1 granule, cacheability/shareability for
  both ranges, IPS, ASID size/source (`A1`), TBI0/TBI1, HA/HD.
- `ttbr0_el1`/`ttbr1_el1` -- translation table base address + ASID
  (`CnP` too); both share one `ttbr_el1<IsTtbr1>` template since the bit
  layout is identical.
- `mair_el1` -- eight independent 8-bit memory-attribute encodings
  (`Attr0..Attr7`), selected per PTE by `tcr_el1`'s `AttrIndx` field (see
  [`page_table_entry_fields.md`](page_table_entry_fields.md)'s
  `structo::arch::detail::vmsa::stage1_bits::attr_indx`).

### TrustZone: `scr_el3`

- `scr_el3` (Secure Configuration Register, EL3-only) -- `NS` (which
  world the next lower exception level enters), `IRQ`/`FIQ`/`EA`
  exception-routing-to-EL3 bits, `SMD` (disable `SMC`), `HCE` (enable
  `HVC`), `SIF` (forbid Secure instruction fetch from Non-secure
  memory), `RW` (next lower EL's execution state: AArch64 or AArch32),
  `ST` (Secure EL1 access to the generic timer), and `TWI`/`TWE` (trap
  `WFI`/`WFE` to EL3). This is the AArch64 analogue of ARMv7-A's `SCR`;
  unlike `SCR`, it is not itself banked (EL3 has no "other world" copy
  of its own state). Unlike ARMv7-A's `SCTLR`/`TTBR0`/`TTBR1`/
  `CONTEXTIDR`, however, AArch64's EL1 registers (`sctlr_el1`,
  `ttbr0_el1`/`ttbr1_el1`, `tcr_el1`, `mair_el1`, `contextidr_el1`,
  `vbar_el1`, ...) have **no hardware-banked per-world copy** at all --
  `NS` only selects which world the next lower EL runs as, never which
  physical register bank a plain `MRS`/`MSR` reaches. EL3 firmware must
  explicitly save/restore them across every world switch -- see
  [`world_switch_guard.md`](world_switch_guard.md) and Arm Trusted
  Firmware-A's `cm_el1_sysregs_context_save()`/`_restore()`.

```cpp
using namespace structo::arch::arm64;

scr_el3 s{};
s.set_ns(true).set_rw(true).set_hyp_call_enabled(true); // next EL: Non-secure, AArch64, HVC usable
```

### Hypervisor stage-2 translation: `hcr_el2`, `vtcr_el2`, `vttbr_el2`

The Armv8 Virtualization Extensions add a second, EL2-controlled
translation stage -- guest ("intermediate") physical address to real
physical address -- that every stage-1 (EL1/EL0) translation also
passes through once enabled. These are the EL2 registers that bring
that stage-2 translation up:

- `hcr_el2` (Hypervisor Configuration Register) -- `VM` enables stage-2
  translation; `SWIO`/`PTW`/`DC`/`BSU` adjust cache/barrier behavior
  seen by EL1/EL0; `FMO`/`IMO`/`AMO` route physical interrupts/aborts to
  EL2; `TWI`/`TWE`/`TSC`/`TTLB`/`TVM`/`TGE`/`TDZ`/`TRVM` trap assorted
  EL1/EL0 operations (including EL1's own stage-1 MMU register writes,
  via `TVM`/`TRVM`) to EL2; `HCD` disables `HVC`; `RW` picks EL1's
  execution state; `CD`/`ID` (FEAT_VHE, `E2H=1` only) disable stage-1
  cacheability for the EL2&0 translation regime; `E2H` (FEAT_VHE)
  switches EL2 into the "EL2&0" regime. Only this MMU/trap-focused
  subset is modeled -- the register also has many virtual-interrupt
  -pending-state bits (`VF`/`VI`/`VSE`/`FB`) that are out of scope here.
- `vtcr_el2` (Virtualization Translation Control Register) -- mirrors
  `tcr_el1`'s shape for the single stage-2 range: `T0SZ`, `SL0`
  (starting level), `IRGN0`/`ORGN0`/`SH0`, `TG0` (granule), `PS`
  (physical address size), `VS` (16-bit VMID), `HA`/`HD`.
- `vttbr_el2` (Virtualization Translation Table Base Register) -- stage-2
  table root (`BADDR`) + `VMID` (tags stage-2 TLB entries per guest,
  actual usable width is 8 or 16 bits per `vtcr_el2`'s `VS` bit) +
  `CnP`; the stage-2 counterpart of `ttbr0_el1`.

```cpp
using namespace structo::arch::arm64;

vtcr_el2 vtcr{};
vtcr.set_t0sz(24).set_sl0(0b01).set_tg0(0).set_ps(0b001); // 40-bit IPA, 4 KB granule

vttbr_el2 vttbr{};
vttbr.set_base_addr(guest_table_phys_addr).set_vmid(guest_vmid);

hcr_el2 hcr{};
hcr.set_vm(true).set_tvm(true).set_rw(true); // enable stage 2, trap EL1 MMU-config writes, EL1 is AArch64

#if defined(__aarch64__)
vtcr.write();
vttbr.write();
hcr.write();
#endif
```

See [`page_table_entry_fields.md`](page_table_entry_fields.md) (stage-2
PTE field layouts, `structo::arch::arm64::lpae::stage2_tag` et al.) for
the entries these registers' tables are made of.

## `structo/arch/arm/mmu_regs.hpp` -- ARMv7-A/AArch32

- `sctlr` -- MMU enable, D/I-cache enable, alignment checks.
- `ttbcr_short`/`ttbcr_lpae` -- two distinct views of `TTBCR`, since
  which fields are meaningful depends entirely on `EAE` (bit 31, shared
  by both views): `ttbcr_short` is the classic short-descriptor format
  (`N`, `PD0`/`PD1`), `ttbcr_lpae` is the LPAE format (`T0SZ`/`T1SZ`,
  cacheability/shareability, `A1`). Mixing both interpretations in one
  type would make it easy to read a field that doesn't apply to the
  active format, so they're kept separate.
- `ttbr0_short`/`ttbr1_short` -- 32-bit base address only (classic
  short-descriptor format).
- `ttbr0_lpae`/`ttbr1_lpae` -- 64-bit base address + ASID, read/written
  as a register pair via `MRRC`/`MCRR` (LPAE format).
- `contextidr` -- ASID (short-descriptor format; LPAE instead carries
  its ASID directly in `TTBR0`/`TTBR1`) + Process ID.

### TrustZone: `scr` and `nsacr`

ARMv7-A's `SCTLR`, `TTBCR`, `TTBR0`, `TTBR1`, and `CONTEXTIDR` are all
**banked** on a TrustZone-capable core: the Secure and Non-secure worlds
each have their own private copy, and a plain `MRC`/`MCR` through this
header's other types always reads/writes whichever copy belongs to the
world currently executing. There is deliberately no separate "Secure
TTBR0" value type here -- the instruction encoding is identical in both
worlds, and only the live `SCR.NS` value (itself only readable/writable
from Secure state, typically Monitor mode) decides which bank the next
`MRC`/`MCR` lands on.

- `scr` (Secure Configuration Register) -- `NS` (which world the next
  exception level enters -- the bit that actually selects the banked
  register copy), plus `IRQ`/`FIQ`/`EA` exception-routing-to-Monitor
  bits, `FW`/`AW` (whether Non-secure state may write the corresponding
  CPSR mask bit), `SCD` (disable `SMC` from Non-secure), `HCE` (enable
  `HVC`), and `SIF` (forbid Secure instruction fetch from Non-secure
  memory).
- `nsacr` (Non-Secure Access Control Register) -- per-coprocessor
  Non-secure-access-enable bits (`cp_accessible(n)`/`set_cp_accessible`,
  with `cp10_accessible()`/`cp11_accessible()` convenience accessors for
  the commonly-toggled VFP/Advanced SIMD coprocessors), plus `NSD32DIS`/
  `NSASEDIS` (outright disabling Non-secure use of the upper VFP D
  registers / Advanced SIMD).

```cpp
using namespace structo::arch::arm;

scr s{};
s.set_ns(true).set_hyp_call_enabled(true); // switch the next world to Non-secure, allow HVC

nsacr n{};
n.set_cp10_accessible(true).set_cp11_accessible(true); // let Non-secure use VFP/NEON
```

Both `scr` and `nsacr` are themselves Secure-only registers: a
`read()`/`write()` attempted from Non-secure state traps on real
hardware (not modeled here -- this header only ever talks to the
register currently selected by the executing world, same as everywhere
else).

### Hypervisor stage-2 translation: `hcr`, `vtcr`, `vttbr`

The (Non-secure-only) Virtualization Extensions add the same kind of
second, Hyp (PL2)-controlled translation stage as AArch64's
`hcr_el2`/`vtcr_el2`/`vttbr_el2` above, just with the narrower 32-bit
register encodings ARMv7-A uses:

- `hcr` (Hypervisor Configuration Register) -- the same MMU/trap-focused
  subset as `hcr_el2`, minus the AArch64-only `RW`/`CD`/`ID`/`E2H` bits
  (ARMv7-A has no stage-1 execution-state choice or `E2H` regime).
- `vtcr` (Virtualization Translation Control Register) -- `T0SZ`, `SL0`,
  `IRGN0`/`ORGN0`/`SH0`; narrower than `vtcr_el2` since ARMv7-A LPAE
  stage-2 has no alternate granule (no `TG0`-equivalent field).
- `vttbr` (Virtualization Translation Table Base Register) -- 64-bit,
  read/written as a register pair via `MRRC`/`MCRR p15, 6` (like
  `ttbr0_lpae`/`ttbr1_lpae`): a 39-bit `BADDR` and an 8-bit `VMID`.

```cpp
using namespace structo::arch::arm;

vtcr vtcr{};
vtcr.set_t0sz(8).set_sl0(0b01); // configure stage-2 input size and starting level

vttbr vttbr{};
vttbr.set_base_addr(guest_table_phys_addr).set_vmid(guest_vmid);

hcr h{};
h.set_vm(true).set_tvm(true); // enable stage 2, trap EL1/PL1 MMU-config writes to Hyp mode

#if defined(__arm__) && !defined(__aarch64__)
vtcr.write();
vttbr.write();
h.write();
#endif
```

## `structo/arch/riscv/mmu_regs.hpp` -- RISC-V (RV64 only)

- `satp` -- `MODE` (`satp_mode::{bare,sv39,sv48,sv57}`), ASID, and the
  root page-table's physical page number. Sv32's 32-bit `satp` layout is
  not modeled, matching `page_table_traits.hpp`'s RV64-only scope.

## `structo/arch/x86/mmu_regs.hpp` -- x86/x86-64

- `cr0` -- `PE`, `WP`, `CD`, `PG`.
- `cr3` -- top-level page-table base address, plus both `PWT`/`PCD` and
  `PCID` accessor pairs (they alias the same low bits; which
  interpretation applies depends on the live `CR4.PCIDE`, same as the
  architecture itself -- use whichever pair matches your configuration).
- `cr4` -- `PAE`, `PGE`, `PCIDE`, `SMEP`, `SMAP`, `LA57`.
- `efer` -- the `EFER` MSR (`SCE`, `LME`, `NXE`, and the read-only `LMA`
  status bit), read/written via `rdmsr`/`wrmsr` at address `0xC0000080`.

## See also

[`page_table_arch_configs.md`](page_table_arch_configs.md) (the
`page_table_levels<...>` shapes these registers configure),
[`page_table_entry_fields.md`](page_table_entry_fields.md) (the PTE-field
counterpart built on the same `pte_bit_field` primitive),
[`irq_guard.md`](irq_guard.md) (the established pattern for gating real
privileged register access to its own architecture while keeping a
header-check-clean no-op everywhere else).
