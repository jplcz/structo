<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::arch::tlb_flush_traits<Arch>` / `tlb_flusher<Arch>`

`include/structo/arch/tlb_flush.hpp`

The per-architecture TLB-maintenance customization point
(`tlb_flush_traits<Arch>`) plus the architecture-agnostic dispatcher
built on top of it (`tlb_flusher<Arch>`) -- the concrete, callable
realization of the `arch_flush_tlb_all()`/`arch_flush_tlb_asid()`/
`arch_flush_tlb_page_asid()`/`arch_has_broadcast_tlbi` pseudocode
[`asid_allocator.md`](asid_allocator.md)'s and
[`mm_asid_context.md`](mm_asid_context.md)'s worked examples have
always assumed existed somewhere.

## Why this exists

Every piece of ASID bookkeeping elsewhere in this library
(`asid_allocator`, `fixed_asid_allocator`, `mm_asid_context`) is
deliberately agnostic about how a TLB is actually invalidated -- each
only ever hands back a raw ASID/VMID value and documents "now go flush
it" as a caller obligation. This header is the other half: a single,
uniform customization point a concrete architecture specializes once,
turning every one of those headers' pseudocode examples into real,
callable code.

## `Space`: more than one kind of TLB to invalidate

A flush is never just "the TLB" -- which cached translations even
*exist* to invalidate depends on which translation regime produced
them, so every operation is templated on a `Space` tag as well as
`Arch`:

| `Space` | Meaning |
|---|---|
| `untagged_tlb_space` | A flat TLB with no software-visible tag at all. |
| `process_tlb_space` | The common case: a per-task address space tagged by a hardware ASID/PCID. |
| `guest_tlb_space` | A hypervisor's stage-2/nested (IPA/GPA-to-PA) translations, tagged by VMID/VPID. |
| `hypervisor_tlb_space` | The hypervisor's own EL2/VMX-root translations -- typically untagged. |
| `secure_tlb_space` / `nonsecure_tlb_space` | ARM TrustZone worlds. |
| `root_tlb_space` / `realm_tlb_space` | ARM Realm Management Extension (RME) worlds; Root is untagged, Realm is ASID-tagged. |
| `gpt_tlb_space` | RME's Granule Protection Table cache -- PA-addressed, no ASID/VMID tag, EL3/RMM-only. |

A `Space` an architecture's trait never mentions simply isn't
supported there -- `tlb_flusher<Arch>` reports that as a
`static_assert`, not a runtime error, since "which spaces exist" is
always a compile-time architectural fact. See
[`domain_space_traits.md`](domain_space_traits.md) for translating a
[`structo::arch::execution_domain`](execution_domain.md) into one of
these tags (and back) across this header and three others that define
their own independent tag families.

## The customization point

`tlb_flush_traits<Arch>` is left undefined for any `Arch` that hasn't
opted in (mirroring `io_space_traits<Backend>`/
`page_table_entry_traits<Tag>`). A real specialization supplies a
subset of twelve `static` member templates (each itself templated on
`Space`): the mandatory `flush_all`, five optional, increasingly
precise local operations (`flush_tag`, `flush_page`, `flush_page_tag`,
`flush_range`, `flush_range_tag`), and their six `..._broadcast` twins,
plus two optional trait constants: `supports_broadcast` (informational
only) and `max_range_bytes` (see "Oversized ranges" below).

## Precision fallback: coarsening is always safe, so it is automatic

A TLB flush has exactly one safety direction: invalidating *more* than
strictly necessary is always correct (merely slower), invalidating
*less* is never correct. `tlb_flusher<Arch>` exploits this: if a
`Space`'s trait doesn't implement a precise operation, it transparently
falls back to the next-coarsest one it does implement, all the way
down to `flush_all`:

```text
flush_page_tag  -> flush_tag -> flush_page -> flush_all   // first fallback found wins, left to right
flush_range_tag -> flush_tag -> flush_range -> flush_all
flush_range     -> flush_all
flush_page      -> flush_all
flush_tag       -> flush_all
```

This is how `process_tlb_space` on hardware with no ASID tagging at
all (e.g. legacy ARM) keeps working correctly through this same API: a
trait that never defines `flush_tag`/`flush_page_tag` for that `Space`
simply has every tagged call degrade to the untagged
`flush_all`/`flush_page` it does provide.

## Broadcast: an orthogonal axis, never silently faked

Unlike precision, *reach* (one core vs. the architecture's whole
shareability domain) has no safe direction to coarsen into
automatically -- silently serving a `..._broadcast()` call with a
local-only flush would make the caller believe every core is
consistent when only this one is. So broadcast support is never
synthesized: `supports_broadcast` is a plain informational constant
for the *caller* to branch on (the `arch_has_broadcast_tlbi` constant
`asid_allocator.hpp`'s own examples already assume); a `Space`/`Arch`
pair with no broadcast operation defined for it at all fails to
compile with a clear `static_assert`, not a silent local-only flush.
x86 and RISC-V (no custom extension) define no `..._broadcast` member
at all -- cross-core consistency there is the caller's own IPI-driven
shootdown.

## Range flushes are first-class

`flush_range`/`flush_range_tag` (and their broadcast twins) are
guaranteed-present operations, exactly like `flush_all`/`flush_tag`/
`flush_page` -- never an optional add-on. An architecture with a
native range-invalidate instruction (e.g. ARM `FEAT_TLBIRANGE`'s `TLBI
RVAE1IS`) wires it up directly; one without still gets correct (if
coarser) behavior for free through the same fallback chain, down to a
single `flush_tag`/`flush_all` call.

## Oversized ranges

A native range-invalidate instruction still costs roughly one hardware
operation per page -- past some crossover point, walking a huge range
one chunk at a time costs more than a single `flush_tag`/`flush_all`
that over-invalidates everything at once. An architecture states that
crossover as the optional `max_range_bytes` trait constant; whenever
`(addr_end - addr_begin)` exceeds it, `tlb_flusher` skips the range op
entirely and flushes the coarser equivalent instead. A trait with no
`max_range_bytes` at all (the default) always attempts the most
precise op it has.

## Per-architecture backends

- `arch/riscv/tlb_flush.hpp` -- `sfence.vma`, `process_tlb_space` only
  (no hardware guest/hypervisor/TrustZone concept on plain RV64).
- `arch/x86/tlb_flush.hpp` -- `INVLPG`/`INVPCID`/full `CR3` reload,
  `process_tlb_space` only; no hardware broadcast (`supports_broadcast
  == false`).
- `arch/arm/tlb_flush.hpp` (ARMv7-A/AArch32) -- `TLBIALL`/`TLBIASID`/
  `TLBIMVA`/`TLBIMVAA` family (`arm::tlb_tag`, local-only) plus the
  `...IS` broadcast twins (`arm::tlb_tag_mp`); `process_tlb_space`,
  `hypervisor_tlb_space`, `guest_tlb_space` (current-VMID-only, see
  `TLBIIPAS2`'s own docs in the header).
- `arch/arm64/tlb_flush.hpp` (AArch64) -- `TLBI VAE1IS`/`ASIDE1IS`/
  `VMALLE1IS`/`ALLE2IS`/`IPAS2E1IS`/`RVAE1IS` family (`arm64::tlb_tag`);
  single tag since the `...IS` broadcast encodings are mandatory
  baseline A64, with `FEAT_TLBIRANGE` range ops wired up when the
  precise one is available.

## Example

```cpp
using namespace structo::arch;

// Tagged, local-only flush of one ASID's entries.
tlb_flusher<arm64::tlb_tag>::flush_tag<process_tlb_space>(asid);

// Broadcast variant, only if the architecture actually has hardware for it.
if constexpr (tlb_flush_traits<arm64::tlb_tag>::supports_broadcast) {
  tlb_flusher<arm64::tlb_tag>::flush_tag_broadcast<process_tlb_space>(asid);
} else {
  tlb_flusher<arm64::tlb_tag>::flush_tag<process_tlb_space>(asid);
  arch_wait_for_shootdown_acks(targets); // caller's own IPI shootdown
}
```

See also: [`address_translate.md`](address_translate.md) (the sibling
hardware-address-translation customization point, reusing the same
`Space` tags), [`asid_allocator.md`](asid_allocator.md)/
[`fixed_asid_allocator.md`](fixed_asid_allocator.md) (the ASID
bookkeeping this header's flush calls are paired with),
[`domain_space_traits.md`](domain_space_traits.md) (translating
`execution_domain` into one of this header's `Space` tags, and back).
