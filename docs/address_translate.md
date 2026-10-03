<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::arch::address_translate_traits<Arch>` / `address_translator<Arch>`

`include/structo/arch/address_translate.hpp`

A per-architecture, `Space`-tagged customization point for *hardware
address translation* -- modeling instructions like ARM's `AT`
(AArch64)/CP15 `ATS1*`/`ATS12NSO*` (AArch32) System instructions, which
ask the MMU "what would happen if I accessed this virtual address for
{read, write} right now, in this `Space`?" without actually touching
memory or raising a fault. The CPU performs the full privilege/
permission check exactly as a real access would, and reports either the
resulting output address plus its attributes, or why the access would
fault -- without the caller having to reimplement the page-table-walk
logic (short/long descriptor format, `FEAT_LPA2`, stage-2 combination,
...) in software itself.

Reuses the same `Space` tags as [`tlb_flush.md`](tlb_flush.md)
(`process_tlb_space`, `hypervisor_tlb_space`, `guest_tlb_space`, ...) --
translating an address and flushing its TLB entry target exactly the
same hardware translation regime, so one shared set of tags serves both
customization points.

## Result: `translated_address`, or a `reloco::error`

`address_translator<Arch>::translate<Space>(vaddr, access)` returns
`reloco::result<translated_address>` -- `reloco::unexpected(err)` if the
CPU reports the access would fault (including, notably, a security/
permission-domain violation -- e.g. a Non-secure access hitting a
Secure-only Granule Protection Table region under `FEAT_RME` --
reported as `reloco::error::security_violation`, not folded into the
generic `page_fault`), the decoded `translated_address` otherwise:

- `physical_address` -- page-aligned output address.
- `mem_attr` -- raw `MAIR_ELx`-encoded memory-attribute byte.
- `shareability` -- `0`/`2`/`3` = Non-shareable/Outer/Inner Shareable.
- `output_non_secure` -- best-effort decode of the `NS` bit; regime- and
  instruction-dependent, informational only, not a security boundary
  decision by itself.

## No fallback chain, unlike `tlb_flush.hpp`

`address_translator<Arch>::translate<Space>()` has no precision-fallback
at all -- if `Arch` doesn't implement `Space`, it is a hard
`static_assert`. There is no "coarser" translation query to fall back
to: either the hardware can answer this exact question or it cannot.

## Fault decode: one shared `FST`-to-`reloco::error` mapping

Both ARM backends share one `detail::fault_status_to_error(unsigned
fst)` helper (ARM32's `PAR` and AArch64's `PAR_EL1` report the same
6-bit fault-status encoding):

- Address-size / translation fault, access flag fault -> `page_fault`.
- Permission fault -> `permission_denied`.
- Granule Protection Fault (`FEAT_RME`, a Security/Realm/Root PAS
  boundary violation) -> `security_violation` -- deliberately not
  folded into `permission_denied`, since it is a trust-boundary
  violation, not an ordinary page-permission check.
- Synchronous External abort / parity-ECC error on the table walk ->
  `io_error`.
- TLB conflict abort (transient) -> `try_again`.
- Everything else (unsupported atomic HW update, AArch32
  short-descriptor domain faults, any reserved encoding) ->
  `unsupported_operation`.

This is necessarily a many-to-one bucketing, not a lossless decode; a
caller needing the exact fault level/cause must keep the raw
`PAR`/`PAR_EL1` value around itself.

## Per-architecture backends

- `arch/arm64/address_translate.hpp` (AArch64) -- `arm64::at_tag`:
  `process_tlb_space` (`AT S1E1R`/`S1E1W`), `hypervisor_tlb_space`
  (`AT S1E2R`/`S1E2W`, also Secure EL2's own stage-1 under `FEAT_SEL2`
  when the current security state selects Secure), `guest_tlb_space`
  (`AT S12E1R`/`S12E1W`, current-VMID-only, also the Secure guest's
  combined stage-1+2 under `FEAT_SEL2`). No dedicated AArch64
  instruction overrides the current security state the way AArch32's
  `ATS12NSO**` does, so `nonsecure_tlb_space`/`secure_tlb_space` are not
  modeled here.
- `arch/arm/address_translate.hpp` (ARMv7-A/AArch32) -- `arm::at_tag`:
  `process_tlb_space` (`ATS1CPR`/`ATS1CPW`), `hypervisor_tlb_space`
  (`ATS1HR`/`ATS1HW`), and **both** `guest_tlb_space` *and*
  `nonsecure_tlb_space` wired to the same `ATS12NSOPR`/`ATS12NSOPW`
  encoding -- these are AArch32's dedicated "Non-secure state only"
  instructions, which always walk the Non-secure world's tables
  regardless of the executing core's current Security state: issued
  from Hyp mode they answer "what would my current guest's stage-1+2
  access resolve to?" (`guest_tlb_space`); issued from Secure Monitor
  mode they answer "what would the Non-secure world's current guest
  resolve this to?" (`nonsecure_tlb_space`). Assumes the LPAE
  (long-descriptor) `PAR` format; the legacy short-descriptor format is
  out of scope, consistent with `arm/pte_short.hpp` vs.
  `arm/pte_stage1.hpp` elsewhere in this library.

Both backends intentionally skip the unprivileged (`S1E0*`/`ATS1CU*`/
`ATS12NSOU*`) and PAN-override (`S1E1*P`) instruction variants -- same
conservative-scope precedent as `tlb_flush.hpp` skipping `TLBI`'s
last-level hint variants.

## Example

```cpp
using namespace structo::arch;

auto result = address_translator<arm64::at_tag>::translate<process_tlb_space>(
    user_vaddr, translate_access::write);
if (!result) {
  // result.error() is e.g. reloco::error::permission_denied,
  // reloco::error::page_fault, or reloco::error::security_violation.
  return reloco::unexpected(result.error());
}
// result->physical_address is page-aligned; add the VA's own low-order
// page offset back in if a byte-exact PA is needed.
```

See also: [`tlb_flush.md`](tlb_flush.md) (the sibling TLB-maintenance
customization point, sharing the same `Space` tags),
[`domain_space_traits.md`](domain_space_traits.md) (translating
`execution_domain` into one of this header's `Space` tags, and back).
