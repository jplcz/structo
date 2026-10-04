// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file address_translate.hpp
 * @brief `structo::arch::address_translate_traits<structo::arch::arm::at_tag>`
 * for ARMv7-A/AArch32 -- wiring `structo::arch::address_translator<arm::at_tag>`
 * up to the real CP15 `ATS1*`/`ATS12NSO*` System instructions
 * (`MCR p15, <opc1>, <Rt>, c7, c8, <opc2>`) and decoding their result
 * from the 64-bit `PAR` register (`MRRC p15, 0, <Rt>, <Rt2>, c7`).
 *
 * See `arch/address_translate.hpp` for the full architecture-agnostic
 * dispatcher design this specializes; only the AArch32-specific
 * instruction selection and `PAR` decode are documented here. Encodings
 * verified against the Armv8-A Architecture Reference Manual (DDI0487),
 * section G8.2.
 *
 * ## LPAE (long-descriptor) `PAR` format assumed
 *
 * `PAR`'s bits `[63:0]` are architecturally mapped onto exactly the
 * same layout as AArch64's `PAR_EL1[63:0]` *only when the translation
 * system uses the LPAE (long-descriptor) format* -- i.e. `TTBCR.EAE ==
 * 1`. The legacy VMSAv7 short-descriptor format reports a different,
 * 32-bit-oriented `PAR` layout this header does not decode. Since every
 * other PTE-shaped header in this library already assumes LPAE for
 * ARMv7-A (`arm/pte_stage1.hpp`/`arm/pte_stage2.hpp`, not
 * `arm/pte_short.hpp`'s short-descriptor-specific format), this is a
 * consistent, not a new, scope decision -- a system still running
 * short-descriptor tables should not use this header to decode `PAR`.
 *
 * ## Space mapping
 *
 * - `process_tlb_space` -- `ATS1CPR`/`ATS1CPW` (`opc1=0, CRm=c8,
 *   opc2=0/1`): stage-1 only, current Security state, PL1 (current
 *   privilege). The unprivileged `ATS1CUR`/`ATS1CUW` and PAN-override
 *   `ATS1CPRP`/`ATS1CPWP` variants are out of scope, same reasoning as
 *   `arch/arm64/address_translate.hpp` skipping `AT S1E0*`/`AT S1E1*P`.
 * - `hypervisor_tlb_space` -- `ATS1HR`/`ATS1HW` (`opc1=4, CRm=c8,
 *   opc2=0/1`): Hyp mode's own stage-1 translation, untagged (no ASID
 *   concept at Hyp mode, same as `arch/arm/tlb_flush.hpp`'s
 *   `hypervisor_tlb_space` mapping).
 * - `guest_tlb_space` / `nonsecure_tlb_space` -- both map to the *same*
 *   `ATS12NSOPR`/`ATS12NSOPW` encoding (`opc1=0, CRm=c8, opc2=4/5`):
 *   combined stage-1 + stage-2 translation of the Non-secure EL1&0
 *   (PL1) regime, for whichever VMID `HTTBR`/`VTTBR` currently holds --
 *   no VMID operand exists in the instruction, exactly the same ISA
 *   limitation already documented for `TLBIIPAS2`/`TLBI IPAS2E1` in
 *   `arch/arm/tlb_flush.hpp`/`arch/arm64/tlb_flush.hpp`. Unlike every
 *   other `AT*`/`TLBI*` instruction in this library, `ATS12NSOPR`/
 *   `ATS12NSOPW` are explicitly documented ("Non-secure state only",
 *   ARM DDI0487 G8.2.9/G8.2.10) to **always** walk the Non-secure
 *   world's tables regardless of the executing core's current Security
 *   state -- they are issuable from either Non-secure Hyp mode (where
 *   they answer "what would my current guest's stage-1+2 access
 *   resolve to?", i.e. `guest_tlb_space`) or Secure Monitor mode (where
 *   they answer "what would the Non-secure world's current guest
 *   resolve this to?", i.e. `nonsecure_tlb_space`) -- the instruction
 *   encoding, and this trait's implementation, is identical either way;
 *   only the calling context changes which question is actually being
 *   asked. Both `Space` tags are provided so callers can name whichever
 *   question matches their own calling context. The unprivileged
 *   `ATS12NSOUR`/`ATS12NSOUW` variants are out of scope for the same
 *   reason as `process_tlb_space`'s unprivileged variants above. Only
 *   issuable from Hyp mode or Monitor mode (see the instructions' own
 *   `PSTATE.EL` preconditions) -- executing `address_translator<
 *   arm::at_tag>::translate<guest_tlb_space|nonsecure_tlb_space>(...)`
 *   from PL1 is the caller's own precondition violation, not something
 *   this header can check.
 *
 * `secure_tlb_space` is not modeled: no instruction above takes a
 * Security-state selector that forces the *Secure* world's regime the
 * way `ATS12NSOPR`/`ATS12NSOPW` force the Non-secure one -- `ATS1CPR`/
 * `ATS1CPW` (`process_tlb_space`) already probe whichever state is
 * current, so a caller needing the Secure world's translation simply
 * calls `process_tlb_space` while actually executing in Secure PL1,
 * same rationale as `arch/arm/tlb_flush.hpp`'s identical exclusion.
 * AArch32 has no Secure-EL2 concept (`FEAT_SEL2` requires EL2 to use
 * AArch64, see `arch/arm64/address_translate.hpp`), so there is no
 * further Secure-regime nuance to consider here.
 *
 * ## `PAR` decode (best effort)
 *
 * Identical bit layout and decode logic to
 * `arch/arm64/address_translate.hpp`'s `PAR_EL1` decode (see that
 * header's docs for the full field table and fault-code mapping) --
 * reused directly via the shared `arch::detail::fault_status_to_error()`
 * helper in `address_translate.hpp`.
 *
 * Only compiled on a real 32-bit ARM target (`__arm__`, and not
 * AArch64); on every other host this header is an intentional no-op so
 * it stays header-check-clean cross-compiled from any machine, matching
 * the gating convention established by `arm/tlb_flush.hpp`.
 */

#if defined(__arm__) && !defined(__aarch64__)

#include <structo/arch/address_translate.hpp>

#include <cstdint>
#include <type_traits>

namespace structo::arch::arm {

/** @brief `address_translate_traits<arm::at_tag>`'s `Arch` tag -- CP15 `ATS1*`/`ATS12NSO*` ops + 64-bit `PAR` (LPAE
 * format), every ARMv7-A/AArch32 core. See the @file docs. */
struct at_tag {};

namespace detail {

/** @brief Decodes a 64-bit LPAE-format `PAR` (already read into `par`) into `translated_address` or a `reloco::error`
 * -- identical layout/logic to `arm64::detail::decode_par_el1`, see the @file docs. */
[[nodiscard]] inline reloco::result<translated_address> decode_par(std::uint64_t par) noexcept {
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

/** @brief `MRRC p15, 0, Rt, Rt2, c7` -- reads the 64-bit `PAR` as `{Rt2:Rt}` (`Rt` low, `Rt2` high). */
[[nodiscard]] inline std::uint64_t read_par() noexcept {
  std::uint32_t lo, hi;
  asm volatile("mrrc p15, 0, %0, %1, c7" : "=r"(lo), "=r"(hi));
  return static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
}

} // namespace detail

} // namespace structo::arch::arm

template <> struct structo::arch::address_translate_traits<structo::arch::arm::at_tag> {

  template <typename Space>
  static auto translate(std::uint64_t vaddr, translate_access access) noexcept
      -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>, reloco::result<translated_address>> {
    std::uint32_t ia = static_cast<std::uint32_t>(vaddr);
    if (access == translate_access::read) {
      asm volatile("mcr p15, 0, %0, c7, c8, 0" ::"r"(ia)); // ATS1CPR
    } else {
      asm volatile("mcr p15, 0, %0, c7, c8, 1" ::"r"(ia)); // ATS1CPW
    }
    return arm::detail::decode_par(arm::detail::read_par());
  }

  template <typename Space>
  static auto translate(std::uint64_t vaddr, translate_access access) noexcept
      -> std::enable_if_t<std::is_same_v<Space, hypervisor_tlb_space>, reloco::result<translated_address>> {
    std::uint32_t ia = static_cast<std::uint32_t>(vaddr);
    if (access == translate_access::read) {
      asm volatile("mcr p15, 4, %0, c7, c8, 0" ::"r"(ia)); // ATS1HR
    } else {
      asm volatile("mcr p15, 4, %0, c7, c8, 1" ::"r"(ia)); // ATS1HW
    }
    return arm::detail::decode_par(arm::detail::read_par());
  }

  // guest_tlb_space and nonsecure_tlb_space both wire up to the exact
  // same ATS12NSOPR/ATS12NSOPW encoding -- see the @file docs for why
  // these two logically distinct questions share one instruction.
  template <typename Space>
  static auto translate(std::uint64_t vaddr, translate_access access) noexcept
      -> std::enable_if_t<std::is_same_v<Space, guest_tlb_space> || std::is_same_v<Space, nonsecure_tlb_space>,
                          reloco::result<translated_address>> {
    std::uint32_t ia = static_cast<std::uint32_t>(vaddr);
    if (access == translate_access::read) {
      asm volatile("mcr p15, 0, %0, c7, c8, 4" ::"r"(ia)); // ATS12NSOPR
    } else {
      asm volatile("mcr p15, 0, %0, c7, c8, 5" ::"r"(ia)); // ATS12NSOPW
    }
    return arm::detail::decode_par(arm::detail::read_par());
  }
};

#endif // defined(__arm__) && !defined(__aarch64__)
