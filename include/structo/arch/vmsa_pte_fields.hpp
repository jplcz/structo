// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file vmsa_pte_fields.hpp
 * @brief Shared bit-position tables for the VMSA ("Virtual Memory System
 * Architecture") long-descriptor page-table-entry format that AArch64
 * (VMSAv8-64) and ARMv7 with the Large Physical Address Extension (LPAE)
 * both use. Not meant to be included directly by embedders -- it exists
 * purely so `structo::arch::arm64::pte_stage1`/`pte_stage2` and
 * `structo::arch::arm::lpae::pte_stage1`/`pte_stage2` don't each
 * re-derive the same bit math, since AArch64's descriptor format is
 * deliberately bit-for-bit compatible with ARMv7-LPAE's (modulo output-
 * address width and level count) rather than a clean-sheet redesign.
 *
 * ## Scope
 *
 * This covers the Armv8.0 baseline encoding only. Deliberately **not**
 * modeled (same reasoning as skipping the Realm Management Extension
 * elsewhere in this library): the 52-bit output-address extension
 * (FEAT_LPA2, which relocates several of these bits), the split
 * stage-2 execute-never extension (FEAT_TTS2UXN, which reinterprets
 * stage-2 bit 53), and any RME-related physical-address-space field.
 * A caller targeting those extensions should read/write the relevant
 * raw bits directly via `page_table_entry<Tag>::value` rather than
 * through this header's named accessors.
 */

#include <structo/arch/pte_field.hpp>

#include <cstdint>

namespace structo::arch::detail::vmsa {

/**
 * @brief Bit positions common to every VMSA stage-1 table/block/page
 * descriptor (AArch64 and ARMv7-LPAE stage 1), regardless of which
 * translation regime (Non-secure EL1&0, Secure EL1&0, EL2, Secure EL2)
 * is walking it.
 */
struct stage1_bits {
  using valid = pte_bit_field<0, 1>;         //!< Bit 0: descriptor is valid (present).
  using table_or_page = pte_bit_field<1, 1>; //!< Bit 1: 1 = table/page descriptor, 0 = block descriptor.
  using attr_indx = pte_bit_field<2, 3>;     //!< Bits [4:2]: index into `MAIR_ELx`.
  using ns = pte_bit_field<5, 1>;            //!< Bit 5: Non-secure (1) vs. Secure (0) output, EL1&0/EL3 regimes only.
  using ap = pte_bit_field<6, 2>;            //!< Bits [7:6]: AP[2:1], access permission.
  using sh = pte_bit_field<8, 2>;            //!< Bits [9:8]: shareability.
  using af = pte_bit_field<10, 1>;           //!< Bit 10: access flag.
  using ng = pte_bit_field<11, 1>;           //!< Bit 11: not-global.
  using contiguous = pte_bit_field<52, 1>;   //!< Bit 52: part of a contiguous hint range.
  using pxn = pte_bit_field<53, 1>;          //!< Bit 53: privileged execute-never.
  using uxn = pte_bit_field<54, 1>;          //!< Bit 54: unprivileged execute-never (EL1&0 regimes) / XN (EL2 regimes).
  using pxn_table = pte_bit_field<59, 1>;    //!< Bit 59 (table descriptors only): PXN for the next level down.
  using xn_table = pte_bit_field<60, 1>;     //!< Bit 60 (table descriptors only): XN for the next level down.
  using ap_table =
      pte_bit_field<61, 2>; //!< Bits [62:61] (table descriptors only): AP restriction for the next level down.
  using ns_table =
      pte_bit_field<63, 1>; //!< Bit 63 (table descriptors only): forces Non-secure for the next level down.

  [[nodiscard]] static constexpr bool is_present(std::uint64_t raw) noexcept { return valid::test(raw); }
  [[nodiscard]] static constexpr bool is_table(std::uint64_t raw) noexcept { return table_or_page::test(raw); }

  /** @brief Bits `[47:GranuleShift]`: the next-level table's or leaf frame's output address. */
  template <std::size_t GranuleShift>
  [[nodiscard]] static constexpr std::uint64_t output_address(std::uint64_t raw) noexcept {
    return pte_bit_field<GranuleShift, 48 - GranuleShift>::get(raw) << GranuleShift;
  }

  /** @brief Replaces bits `[47:GranuleShift]` with `addr` (which must already be `GranuleShift`-aligned). */
  template <std::size_t GranuleShift>
  [[nodiscard]] static constexpr std::uint64_t with_output_address(std::uint64_t raw, std::uint64_t addr) noexcept {
    return pte_bit_field<GranuleShift, 48 - GranuleShift>::set(raw, addr >> GranuleShift);
  }
};

/**
 * @brief Bit positions common to every VMSA stage-2 table/block/page
 * descriptor (AArch64 and ARMv7-LPAE/VE stage 2), used for "hypervisor
 * staging" -- translating a guest's intermediate physical address (IPA)
 * to a real physical address -- by both ordinary (Non-secure) and
 * Secure EL2 (sEL2) stage-2 translation regimes alike.
 *
 * Unlike stage 1, stage-2 table descriptors carry no NSTable/APTable/
 * XNTable/PXNTable upper attributes -- those bits are RES0 (reserved,
 * must be zero) at stage 2, so this header exposes none of them.
 */
struct stage2_bits {
  using valid = pte_bit_field<0, 1>;         //!< Bit 0: descriptor is valid (present).
  using page_or_block = pte_bit_field<1, 1>; //!< Bit 1: 1 = table/page descriptor, 0 = block descriptor.
  using mem_attr = pte_bit_field<2, 4>;      //!< Bits [5:2]: stage-2 memory type/attributes (`MemAttr[3:0]`).
  using s2ap = pte_bit_field<6, 2>;          //!< Bits [7:6]: stage-2 access permission (bit 6 = read, bit 7 = write).
  using sh = pte_bit_field<8, 2>;            //!< Bits [9:8]: shareability.
  using af = pte_bit_field<10, 1>;           //!< Bit 10: access flag.
  using dbm = pte_bit_field<51, 1>;          //!< Bit 51: dirty-bit modifier (hardware dirty-state tracking).
  using contiguous = pte_bit_field<52, 1>;   //!< Bit 52: part of a contiguous hint range.
  using xn = pte_bit_field<54, 1>;           //!< Bit 54: execute-never (baseline, pre-FEAT_TTS2UXN single-bit form).

  [[nodiscard]] static constexpr bool is_present(std::uint64_t raw) noexcept { return valid::test(raw); }
  [[nodiscard]] static constexpr bool is_table(std::uint64_t raw) noexcept { return page_or_block::test(raw); }

  /** @brief Bits `[47:GranuleShift]`: the next-level table's or output frame's physical address. */
  template <std::size_t GranuleShift>
  [[nodiscard]] static constexpr std::uint64_t output_address(std::uint64_t raw) noexcept {
    return pte_bit_field<GranuleShift, 48 - GranuleShift>::get(raw) << GranuleShift;
  }

  /** @brief Replaces bits `[47:GranuleShift]` with `addr` (which must already be `GranuleShift`-aligned). */
  template <std::size_t GranuleShift>
  [[nodiscard]] static constexpr std::uint64_t with_output_address(std::uint64_t raw, std::uint64_t addr) noexcept {
    return pte_bit_field<GranuleShift, 48 - GranuleShift>::set(raw, addr >> GranuleShift);
  }
};

} // namespace structo::arch::detail::vmsa
