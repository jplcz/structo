// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file pte_stage2.hpp
 * @brief `structo::arch::page_table_entry_traits<Tag>` specializations
 * for AArch64 (VMSAv8-64) stage-2 ("hypervisor staging": intermediate-
 * physical-address-to-physical-address) table/block/page descriptors,
 * for both ordinary (Non-secure) stage-2 and Secure EL2 ("sEL2")
 * stage-2 translation regimes.
 *
 * ## Stage 2 vs. stage 1
 *
 * Stage 2 is what a hypervisor (EL2) uses to translate a guest's own
 * stage-1 output -- an Intermediate Physical Address (IPA) -- into a
 * real physical address; `VTTBR_EL2`/`VTCR_EL2` (ordinary, Non-secure
 * guests) or `VSTTBR_EL2`/`VSTCR_EL2` (`FEAT_SEL2`, Secure guests) hold
 * the walk's root, instead of `TTBR0_EL1`. The descriptor format is
 * closely related to stage 1's (see `pte_stage1.hpp`) but meaningfully
 * different: there is no NSTable/APTable/XNTable/PXNTable on stage-2
 * table descriptors (those bits are RES0 at stage 2), access permission
 * is `S2AP[1:0]` (an explicit read/write bit pair, not the stage-1
 * AP[2:1] read-only/EL0-access encoding), and the memory type is a
 * direct 4-bit `MemAttr` field rather than a `MAIR_ELx` index.
 *
 * ## Two tags, no per-entry choice
 *
 * Unlike stage 1's `stage1_secure_tag`, stage 2 has no per-entry
 * NS-equivalent bit in the baseline (pre-`FEAT_RME`) architecture --
 * which physical address space a stage-2 walk's output lands in is
 * fixed by which translation regime is doing the walk, not chosen per
 * descriptor:
 *
 * - **`stage2_tag`** -- ordinary (Non-secure) stage 2, used by a
 *   Non-secure hypervisor for its Non-secure guests. Output is always
 *   Non-secure physical memory.
 * - **`stage2_secure_tag`** -- Secure EL2 ("sEL2") stage 2, used to run
 *   Secure guests (Secure Partitions / Protected VMs). Output is always
 *   Secure physical memory.
 *
 * `FEAT_RME` (which adds a per-entry PAS selector here, same as at stage
 * 1) is out of scope, same as elsewhere in this library.
 *
 * ## Example
 *
 * @code
 * using namespace structo::arch;
 * using traits = page_table_entry_traits<arm64::stage2_tag<>>;
 * auto e = traits::make_leaf_entry(traits::phys_type{0x8000'0000}, 0b11, 0b11, 0xF); // s2ap, sh, mem_attr
 * bool writable = (traits::s2ap(e) & 0b10) != 0;
 * @endcode
 */

#include <structo/arch/page_table_traits.hpp>
#include <structo/arch/vmsa_pte_fields.hpp>
#include <structo/phys_addr.hpp>
#include <structo/phys_page.hpp>

namespace structo::arch::arm64 {

/** @brief Tag: ordinary (Non-secure) stage-2 ("hypervisor staging") translation regime. */
template <typename LeafPageTraits = structo::page_4k> struct stage2_tag {};

/** @brief Tag: Secure EL2 ("sEL2") stage-2 translation regime. */
template <typename LeafPageTraits = structo::page_4k> struct stage2_secure_tag {};

namespace detail {

template <typename Entry> struct stage2_common_accessors {
  using bits = structo::arch::detail::vmsa::stage2_bits;

  [[nodiscard]] static constexpr bool is_present(Entry e) noexcept { return bits::is_present(e.value); }
  /** @brief `true` for a "block" descriptor; see the stage-1 `is_leaf()` doc for the same final-level caveat. */
  [[nodiscard]] static constexpr bool is_leaf(Entry e) noexcept { return !bits::is_table(e.value); }

  [[nodiscard]] static constexpr unsigned mem_attr(Entry e) noexcept {
    return static_cast<unsigned>(bits::mem_attr::get(e.value));
  }
  /** @brief Bits [1:0] = {write, read} (bit0 = read enable, bit1 = write enable). */
  [[nodiscard]] static constexpr unsigned s2ap(Entry e) noexcept {
    return static_cast<unsigned>(bits::s2ap::get(e.value));
  }
  [[nodiscard]] static constexpr bool readable(Entry e) noexcept { return (s2ap(e) & 0b01u) != 0; }
  [[nodiscard]] static constexpr bool writable(Entry e) noexcept { return (s2ap(e) & 0b10u) != 0; }
  [[nodiscard]] static constexpr unsigned sh(Entry e) noexcept { return static_cast<unsigned>(bits::sh::get(e.value)); }
  [[nodiscard]] static constexpr bool af(Entry e) noexcept { return bits::af::test(e.value); }
  [[nodiscard]] static constexpr bool dbm(Entry e) noexcept { return bits::dbm::test(e.value); }
  [[nodiscard]] static constexpr bool contiguous(Entry e) noexcept { return bits::contiguous::test(e.value); }
  [[nodiscard]] static constexpr bool xn(Entry e) noexcept { return bits::xn::test(e.value); }
};

template <typename Tag, typename PhysSpaceTag, typename LeafPageTraits> struct stage2_traits_impl {
  using entry_type = page_table_entry<Tag>;
  using phys_type = phys_addr<void, PhysSpaceTag>;
  using bits = structo::arch::detail::vmsa::stage2_bits;

  [[nodiscard]] static constexpr phys_type child_table_addr(entry_type e) noexcept {
    return phys_type{bits::output_address<LeafPageTraits::page_shift>(e.value)};
  }
  [[nodiscard]] static constexpr phys_type leaf_frame_addr(entry_type e) noexcept { return child_table_addr(e); }

  [[nodiscard]] static constexpr entry_type make_table_entry(phys_type child_table) noexcept {
    std::uint64_t raw = bits::valid::set(0, 1);
    raw = bits::page_or_block::set(raw, 1);
    raw = bits::with_output_address<LeafPageTraits::page_shift>(raw, child_table.value);
    return entry_type{raw};
  }

  /**
   * @brief Builds a leaf entry: a "page" descriptor at the leaf-adjacent
   * (final) level when `final_level` is `true` (the default), or a
   * "block" descriptor at an intermediate level that permits one when
   * `false`. See `is_leaf()`'s doc for why it only applies to the latter.
   */
  [[nodiscard]] static constexpr entry_type make_leaf_entry(phys_type frame, unsigned s2ap, unsigned sh,
                                                            unsigned mem_attr, bool final_level = true, bool af = true,
                                                            bool contiguous = false, bool xn = false,
                                                            bool dbm = false) noexcept {
    std::uint64_t raw = bits::valid::set(0, 1);
    raw = bits::page_or_block::set(raw, final_level ? 1 : 0);
    raw = bits::s2ap::set(raw, s2ap);
    raw = bits::sh::set(raw, sh);
    raw = bits::mem_attr::set(raw, mem_attr);
    raw = bits::af::set_bit(raw, af);
    raw = bits::contiguous::set_bit(raw, contiguous);
    raw = bits::xn::set_bit(raw, xn);
    raw = bits::dbm::set_bit(raw, dbm);
    raw = bits::with_output_address<LeafPageTraits::page_shift>(raw, frame.value);
    return entry_type{raw};
  }
};

} // namespace detail

} // namespace structo::arch::arm64

namespace structo::arch {

/** @brief Ordinary (Non-secure) stage-2 entry traits. */
template <typename LeafPageTraits>
struct page_table_entry_traits<arm64::stage2_tag<LeafPageTraits>>
    : arm64::detail::stage2_common_accessors<page_table_entry<arm64::stage2_tag<LeafPageTraits>>>,
      arm64::detail::stage2_traits_impl<arm64::stage2_tag<LeafPageTraits>, host_phys_space, LeafPageTraits> {};

/** @brief Secure EL2 ("sEL2") stage-2 entry traits. */
template <typename LeafPageTraits>
struct page_table_entry_traits<arm64::stage2_secure_tag<LeafPageTraits>>
    : arm64::detail::stage2_common_accessors<page_table_entry<arm64::stage2_secure_tag<LeafPageTraits>>>,
      arm64::detail::stage2_traits_impl<arm64::stage2_secure_tag<LeafPageTraits>, secure_phys_space, LeafPageTraits> {};

} // namespace structo::arch
