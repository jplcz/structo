// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file address_translate.hpp
 * @brief `structo::arch::address_translate_traits<structo::arch::arm64::at_tag>`
 * for AArch64 -- wiring `structo::arch::address_translator<arm64::at_tag>`
 * up to the real `AT <op>, Xt` System instructions and decoding their
 * result from `PAR_EL1`.
 *
 * See `arch/address_translate.hpp` for the full architecture-agnostic
 * dispatcher design this specializes; only the AArch64-specific
 * instruction selection and `PAR_EL1` decode are documented here.
 * Encodings verified against the Armv8-A Architecture Reference Manual
 * (DDI0487), sections C5.3 (`AT`) and D24.2.136 (`PAR_EL1`).
 *
 * ## Space mapping
 *
 * - `process_tlb_space` -- `AT S1E1R`/`AT S1E1W`: stage-1 only, EL1&0
 *   regime, as if executed with the current PSTATE's privilege (EL1).
 *   The unprivileged `AT S1E0R`/`AT S1E0W` variants (translate as if
 *   from EL0) and the PAN-override `AT S1E1RP`/`AT S1E1WP` variants are
 *   intentionally out of scope -- a caller needing those specific
 *   privilege nuances issues the raw instruction directly; this header
 *   only wires up the one most common case per `Space`, same
 *   conservative-scope precedent as `arch/arm64/tlb_flush.hpp` skipping
 *   `TLBI`'s last-level (`L`-suffixed) hint variants.
 * - `hypervisor_tlb_space` -- `AT S1E2R`/`AT S1E2W`: EL2's own stage-1
 *   translation, untagged (no ASID concept at EL2, same as
 *   `arch/arm64/tlb_flush.hpp`'s `hypervisor_tlb_space` mapping). When
 *   `FEAT_SEL2` (Secure EL2) is implemented and the current Effective
 *   `SCR_EL3.{NSE,NS}` selects the Secure state, this is instead the
 *   Secure EL2 translation regime -- same instruction, no separate
 *   mnemonic exists for it (see the "not modeled" paragraph below).
 * - `guest_tlb_space` -- `AT S12E1R`/`AT S12E1W`: combined stage-1 +
 *   stage-2 translation of a guest's EL1&0 regime, for the current VMID
 *   (`VTTBR_EL2`, or `VSTTBR_EL2` under the Secure-state condition
 *   below) -- no VMID operand exists in the instruction, exactly the
 *   same ISA limitation already documented for `TLBI IPAS2E1` in
 *   `arch/arm64/tlb_flush.hpp`. The unprivileged `AT S12E0R`/
 *   `AT S12E0W` variants are out of scope for the same reason as
 *   `process_tlb_space`'s `S1E0*` above. When `FEAT_SEL2` is
 *   implemented and the current Effective `SCR_EL3.{NSE,NS}` selects
 *   the Secure state, this instruction instead walks the *Secure*
 *   guest's stage-1 + Secure-stage-2 regime (`VSTTBR_EL2`) -- it is the
 *   same encoding either way; which world it targets is decided by
 *   `SCR_EL3` at the time it executes, exactly as the manual documents
 *   for `AT S12E1R`'s translation-regime selection.
 *
 * `secure_tlb_space`/`nonsecure_tlb_space`/`root_tlb_space`/
 * `realm_tlb_space`/`gpt_tlb_space` are not modeled, same rationale as
 * `arch/arm64/tlb_flush.hpp`: no instruction above takes a world/PAS
 * selector operand at all -- which world's regime is probed is
 * determined by `SCR_EL3.{NSE,NS}` at the time the instruction
 * executes (this remains true with `FEAT_SEL2` enabled: it changes
 * *which regime* `SCR_EL3.{NSE,NS}` selects between, not whether a
 * selector operand exists), and `FEAT_RME` is out of scope
 * library-wide (see `arm64/pte_stage1.hpp`/`arm64/pte_stage2.hpp`/
 * `vmsa_pte_fields.hpp`). Unlike AArch32's `ATS12NSO**` (see
 * `arch/arm/address_translate.hpp`), AArch64 has no "Non-secure Only"
 * variant of `AT S12E1*` that overrides the current security state, so
 * there is no dedicated instruction here to wire up `nonsecure_tlb_space`
 * to in the first place.
 *
 * ## `PAR_EL1` decode (best effort)
 *
 * On success (`PAR_EL1.F == 0`): `ATTR` (bits `[63:56]`) ->
 * `translated_address::mem_attr`; `SH` (bits `[8:7]`) ->
 * `translated_address::shareability`; `NS` (bit `[9]`) ->
 * `translated_address::output_non_secure` (see that field's own docs
 * for why this is informational, not load-bearing); `PA[51:12]` (bits
 * `[51:48]`+`[47:12]`, i.e. `FEAT_LPA`'s extension bits combined with
 * the base 36-bit field) -> `translated_address::physical_address`
 * (already page-aligned, as the ISA only ever reports page granularity
 * here). `FEAT_D128`'s 128-bit `PAR_EL1` format is out of scope --
 * `PAR_EL1`'s own architecturally-mapped low 64 bits are read with a
 * plain 64-bit `MRS`, which this library's toolchain support targets.
 *
 * On failure (`PAR_EL1.F == 1`): `FST` (bits `[6:1]`) is decoded via
 * `arch::detail::fault_status_to_error()` (see `address_translate.hpp`
 * for the full fault-class-to-`reloco::error` mapping table) --
 * `S`/`PTW`/`DirtyBit`/`Overlay`/`TopLevel`/`AssuredOnly` and the
 * implementation-defined bits are not decoded, same best-effort scope
 * as the rest of this header.
 *
 * Only compiled on a real AArch64 target (`__aarch64__`); on every
 * other host this header is an intentional no-op so it stays
 * header-check-clean cross-compiled from any machine, matching the
 * gating convention established by `arm64/tlb_flush.hpp`.
 */

#if defined(__aarch64__)

#include <structo/arch/address_translate.hpp>

#include <cstdint>
#include <type_traits>

namespace structo::arch::arm64 {

/** @brief `address_translate_traits<arm64::at_tag>`'s `Arch` tag -- `AT <op>, Xt` + `PAR_EL1`, every AArch64 core. See
 * the @file docs. */
struct at_tag {};

namespace detail {

/** @brief Decodes `PAR_EL1` (already read into `par`) into `translated_address` or a `reloco::error`, per the @file
 * docs' best-effort field mapping. */
[[nodiscard]] inline reloco::result<translated_address> decode_par_el1(std::uint64_t par) noexcept {
  if ((par & 0x1u) != 0) { // F == 1: translation aborted.
    unsigned fst = static_cast<unsigned>((par >> 1) & 0x3fu);
    return reloco::unexpected(arch::detail::fault_status_to_error(fst));
  }

  translated_address out{};
  out.physical_address = par & 0x000f'ffff'ffff'f000ull; // PA[51:12], already page-aligned.
  out.mem_attr = static_cast<std::uint8_t>((par >> 56) & 0xffu);
  out.shareability = static_cast<std::uint8_t>((par >> 7) & 0x3u);
  out.output_non_secure = ((par >> 9) & 0x1u) != 0;
  return out;
}

} // namespace detail

} // namespace structo::arch::arm64

template <> struct structo::arch::address_translate_traits<structo::arch::arm64::at_tag> {

  template <typename Space>
  static auto translate(std::uint64_t vaddr, translate_access access) noexcept
      -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>, reloco::result<translated_address>> {
    std::uint64_t par;
    if (access == translate_access::read) {
      asm volatile("at s1e1r, %0" ::"r"(vaddr));
    } else {
      asm volatile("at s1e1w, %0" ::"r"(vaddr));
    }
    asm volatile("mrs %0, par_el1" : "=r"(par));
    return arm64::detail::decode_par_el1(par);
  }

  template <typename Space>
  static auto translate(std::uint64_t vaddr, translate_access access) noexcept
      -> std::enable_if_t<std::is_same_v<Space, hypervisor_tlb_space>, reloco::result<translated_address>> {
    std::uint64_t par;
    if (access == translate_access::read) {
      asm volatile("at s1e2r, %0" ::"r"(vaddr));
    } else {
      asm volatile("at s1e2w, %0" ::"r"(vaddr));
    }
    asm volatile("mrs %0, par_el1" : "=r"(par));
    return arm64::detail::decode_par_el1(par);
  }

  template <typename Space>
  static auto translate(std::uint64_t vaddr, translate_access access) noexcept
      -> std::enable_if_t<std::is_same_v<Space, guest_tlb_space>, reloco::result<translated_address>> {
    std::uint64_t par;
    if (access == translate_access::read) {
      asm volatile("at s12e1r, %0" ::"r"(vaddr));
    } else {
      asm volatile("at s12e1w, %0" ::"r"(vaddr));
    }
    asm volatile("mrs %0, par_el1" : "=r"(par));
    return arm64::detail::decode_par_el1(par);
  }
};

#endif // defined(__aarch64__)
