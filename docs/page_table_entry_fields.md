# Per-architecture PTE field descriptors

`structo/arch/page_table_traits.hpp` declares (but deliberately leaves
undefined) `page_table_entry_traits<Tag>`: an extension-point hook meant
to be specialized per architecture/translation-regime so a future
page-table walker can decode/encode a raw `page_table_entry<Tag>` value
without ever needing to know its bit layout. The headers below provide
real specializations of that hook -- one tag per hardware translation
regime -- each exposing the documented minimal contract (`is_present`,
`is_leaf`, `child_table_addr`, `leaf_frame_addr`, `make_table_entry`,
`make_leaf_entry`) plus extra named accessors for every architecturally
meaningful field (permission bits, memory-type/attribute indices,
shareability, dirty/accessed state, and so on).

Every type here is a stateless traits struct operating on an opaque
`page_table_entry<Tag>` value: like the rest of `structo`, there is no
runtime state, no allocation, and no dependency on a live page-table
walker -- these headers only remove the need to hand-assemble bit-shift
arithmetic for a real CPU's PTE encoding.

## Shared building blocks

- [`arch/pte_field.hpp`](../include/structo/arch/pte_field.hpp) --
  `pte_bit_field<LowBit, NumBits, Int>`, a single named bit-field
  primitive (`get`/`set`/`test`/`set_bit`) every per-architecture header
  below is built from.
- [`arch/vmsa_pte_fields.hpp`](../include/structo/arch/vmsa_pte_fields.hpp)
  -- internal, shared bit-position tables for the VMSA long-descriptor
  format AArch64 and ARMv7-LPAE both use (bit-for-bit compatible), so
  the bit math is defined exactly once rather than duplicated between
  `arch/arm64/` and `arch/arm/`.

## The `final_level` parameter: block/huge-page vs. page descriptors

VMSA (ARM64/ARMv7-LPAE) descriptor bit 1 and x86 descriptor bit 7 are
both **level-dependent**: at an intermediate, block/huge-page-capable
level (see `page_table_level::allows_leaf` in `page_table_traits.hpp`),
the bit distinguishes a block/huge-page leaf from a table descriptor;
at the leaf-adjacent *final* level, the same bit position means
something else entirely (VMSA: the bit is unconditionally `1`, a "page"
descriptor, and is never queried via `is_leaf()`; x86: the bit becomes
`PAT`, not `PS`). Every `make_leaf_entry()` in the headers below takes a
`final_level` boolean parameter (default `true`, the common case) so
callers explicitly choose which meaning they want instead of silently
misusing `is_leaf()` at the wrong level. Intel EPT (see below) sidesteps
this ambiguity for presence -- it has no dedicated present bit at all.

## ARM64 (AArch64 VMSAv8-64)

| Header | Tag(s) | Regime |
|---|---|---|
| [`arch/arm64/pte_stage1.hpp`](../include/structo/arch/arm64/pte_stage1.hpp) | `stage1_ns_tag<LeafPageTraits>` | Non-secure EL1&0 / EL2 / EL2&0 stage-1 |
| | `stage1_secure_tag<LeafPageTraits>` | Secure EL1&0 (TrustZone Trusted OS) / EL3 stage-1 |
| | `stage1_secure_el2_tag<LeafPageTraits>` | Secure EL2 / EL2&0 ("sEL2", `FEAT_SEL2`) stage-1 |
| [`arch/arm64/pte_stage2.hpp`](../include/structo/arch/arm64/pte_stage2.hpp) | `stage2_tag<LeafPageTraits>` | Ordinary (Non-secure) stage-2, i.e. hypervisor guest-physical-address translation |
| | `stage2_secure_tag<LeafPageTraits>` | Secure EL2 stage-2 |

All five tags are templated on `LeafPageTraits` (default `page_4k`;
`page_16k`/`page_64k` from `phys_page.hpp` also work, matching the
granules `arch/arm64/page_table_traits.hpp` supports) so the same
accessor code works across every AArch64 translation granule.

### Stage-1 NS-bit semantics

The stage-1 `NS` bit's meaning depends on which regime is walking, and
this is why `stage1_secure_tag` is the *only* one of the three stage-1
tags whose `phys_type` isn't statically fixed:

| Regime | NS bit | `phys_type` |
|---|---|---|
| Non-secure EL1&0 / EL2 / EL2&0 (`stage1_ns_tag`) | RES0 (meaningless) | fixed `nonsecure_phys_space` |
| Secure EL1&0 / EL3 (`stage1_secure_tag`) | **live**: `ns(e)` selects Non-secure (`true`) vs. Secure (`false`) output per entry | untagged `default_phys_space` -- the output space is a runtime choice, recovered via `ns()` |
| Secure EL2 / EL2&0, "sEL2" (`stage1_secure_el2_tag`) | RES0 (meaningless) | fixed `secure_phys_space` |

Stage-2 has no per-entry NS-equivalent bit at all (pre-`FEAT_RME`):
which physical space a stage-2 walk's output lands in is fixed by which
regime is walking (`stage2_tag` -> `host_phys_space`, `stage2_secure_tag`
-> `secure_phys_space`), not chosen per descriptor. Stage-2 table
descriptors also have no `NSTable`/`APTable`/`XNTable`/`PXNTable` upper
attributes -- those are stage-1-only, and are RES0 at stage 2.

**Out of scope**: `FEAT_RME` (Realm Management Extension, i.e. the
Granule Protection / PAS-selecting NSE+NS encoding and `realm_phys_space`
output) is not modeled -- explicitly skipped for this feature. 52-bit
output addresses (`FEAT_LPA2`) and split stage-2 execute-never
(`FEAT_TTS2UXN`) are likewise out of scope, consistent with
`vmsa_pte_fields.hpp`'s documented limitations.

## ARMv7 with LPAE (`structo::arch::arm::lpae`)

| Header | Tag(s) | Regime |
|---|---|---|
| [`arch/arm/pte_stage1.hpp`](../include/structo/arch/arm/pte_stage1.hpp) | `stage1_ns_tag` | Non-secure PL1&0 / Hyp stage-1 |
| | `stage1_secure_tag` | Secure PL1&0 (TrustZone Trusted OS) stage-1 |
| [`arch/arm/pte_stage2.hpp`](../include/structo/arch/arm/pte_stage2.hpp) | `stage2_tag` | Hyp mode stage-2 (hypervisor guest-physical-address translation) |

LPAE's long-descriptor format is bit-for-bit compatible with AArch64's,
so these headers reuse `vmsa_pte_fields.hpp` directly with a hardcoded
4KB granule shift (LPAE's only granule). Unlike AArch64, ARMv7 has only
two stage-1 tags (no sEL2 equivalent -- `FEAT_SEL2` is AArch64-only) and
only one stage-2 tag (the Virtualization Extensions' Hyp mode has no
Secure-world counterpart).

## RISC-V

| Header | Tag(s) | Regime |
|---|---|---|
| [`arch/riscv/pte.hpp`](../include/structo/arch/riscv/pte.hpp) | `pte_tag` | S-stage (ordinary supervisor) translation |
| | `pte_g_stage_tag` | G-stage (hypervisor guest-physical-address, H-extension) translation |

RISC-V's privileged architecture deliberately reuses the exact same PTE
bit layout (`V`/`R`/`W`/`X`/`U`/`G`/`A`/`D` + `RSW` + `PPN`) for both
S-stage and G-stage translation -- unlike ARM64 (distinct stage-1/
stage-2 formats) or x86 (distinct paging/EPT formats), RISC-V did not
define a second encoding for hypervisor staging. There is no
TrustZone-equivalent at the base ISA level, so no secure-world tag
exists.

## x86 / x86-64

| Header | Tag(s) | Regime |
|---|---|---|
| [`arch/x86/pte.hpp`](../include/structo/arch/x86/pte.hpp) | `pte_tag` | Ordinary x86/x86-64 paging |
| | `npt_tag` | AMD Nested Page Tables (NPT) hypervisor staging |
| [`arch/x86/pte_ept.hpp`](../include/structo/arch/x86/pte_ept.hpp) | `ept_tag` | Intel Extended Page Tables (EPT) hypervisor staging |

AMD's NPT reuses x86-64 long-mode paging's bit layout verbatim (just a
second walk rooted at `nCR3`), so `npt_tag` shares `pte.hpp`'s traits
implementation rather than needing its own file. Intel EPT, by contrast,
is a genuinely different encoding -- `R`/`W`/`X` at bits `[2:0]`, an EPT
memory-type field instead of `PWT`/`PCD`, and **no dedicated present
bit** (`is_present()` is instead derived as `R || W || X` being
nonzero) -- so it gets its own file, per this library's "split into a
file per logical mode" convention for genuinely distinct encodings.

**Out of scope**: EPT's mode-based execute control (a separate
user-mode execute bit) is not modeled; a caller needing it can read/
write the raw `page_table_entry<ept_tag>::value` directly.

## Example

```cpp
#include <structo/arch/arm64/pte_stage1.hpp>

using namespace structo::arch;

using traits = page_table_entry_traits<arm64::stage1_ns_tag<>>;

// A 4KB page leaf at the final level.
auto leaf = traits::make_leaf_entry(traits::phys_type{0x4000'0000}, /*ap=*/0b01,
                                     /*sh=*/0b11, /*attr_indx=*/0);
assert(traits::is_present(leaf));
assert(traits::leaf_frame_addr(leaf).value == 0x4000'0000);

// A 1GB block leaf at an intermediate (AllowsLeaf) level.
auto block = traits::make_leaf_entry(traits::phys_type{0x8000'0000}, 0, 0, 0,
                                      /*final_level=*/false);
assert(traits::is_leaf(block)); // only valid to query at this level
```

See also: [`page_table_traits.md`](page_table_traits.md) (the
`page_table_entry_traits<Tag>` hook these headers specialize),
[`page_table_arch_configs.md`](page_table_arch_configs.md) (the
`page_table_levels<...>` configs these PTE tags pair with),
[`phys_addr.md`](phys_addr.md) (the `secure_phys_space`/
`nonsecure_phys_space`/`host_phys_space`/`guest_phys_space`/
`default_phys_space` tags used as each tag's `phys_type`).
