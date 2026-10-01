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

## `structo/arch/arm64/mmu_regs.hpp` -- AArch64 EL1

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
