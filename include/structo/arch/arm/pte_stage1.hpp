// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file pte_stage1.hpp
 * @brief `structo::arch::page_table_entry_traits<Tag>` specializations
 * for ARMv7-A LPAE stage-1 (virtual-address-to-physical-address) table/
 * block/page descriptors, for both Non-secure and Secure (TrustZone)
 * PL1&0 translation regimes.
 *
 * LPAE's long-descriptor format is bit-for-bit compatible with AArch64's
 * (see `vmsa_pte_fields.hpp`), so this header is a thin wrapper around
 * the same shared bit tables `arm64::pte_stage1.hpp` uses, differing
 * only in the output address width (LPAE: up to 40 bits; the upper,
 * always-zero bits of the 48-bit field this header still decodes
 * through are simply unused) and in **not** providing a Secure-EL2-
 * equivalent tag: ARMv7's Virtualization Extensions Hyp mode has no
 * Secure counterpart (`FEAT_SEL2`/"sEL2" is an AArch64-only concept), so
 * there are only two stage-1 tags here, not three.
 *
 * - **`stage1_ns_tag`** -- Non-secure PL1&0, or Hyp mode (always
 *   Non-secure on ARMv7). NS bit RES0; output always Non-secure.
 * - **`stage1_secure_tag`** -- Secure PL1&0 (the classic TrustZone
 *   Trusted OS). The NS bit is live, exactly as for AArch64's
 *   `arm64::stage1_secure_tag`.
 */

#include <structo/arch/page_table_traits.hpp>
#include <structo/arch/vmsa_pte_fields.hpp>
#include <structo/phys_addr.hpp>
#include <structo/phys_page.hpp>

namespace structo::arch::arm::lpae {

/** @brief Tag: Non-secure PL1&0, or Hyp mode, stage-1 translation regime. */
struct stage1_ns_tag {};

/** @brief Tag: Secure PL1&0 (TrustZone Trusted OS) stage-1 translation regime. */
struct stage1_secure_tag {};

} // namespace structo::arch::arm::lpae

namespace structo::arch {

/** @brief Fixed-Non-secure-output stage-1 entry traits. */
template <> struct page_table_entry_traits<arm::lpae::stage1_ns_tag> {
  using entry_type = page_table_entry<arm::lpae::stage1_ns_tag>;
  using phys_type = phys_addr<void, nonsecure_phys_space>;
  using bits = structo::arch::detail::vmsa::stage1_bits;

  [[nodiscard]] static constexpr bool is_present(entry_type e) noexcept { return bits::is_present(e.value); }
  [[nodiscard]] static constexpr bool is_leaf(entry_type e) noexcept { return !bits::is_table(e.value); }

  [[nodiscard]] static constexpr unsigned af(entry_type e) noexcept {
    return static_cast<unsigned>(bits::af::get(e.value));
  }
  [[nodiscard]] static constexpr unsigned sh(entry_type e) noexcept {
    return static_cast<unsigned>(bits::sh::get(e.value));
  }
  [[nodiscard]] static constexpr unsigned ap(entry_type e) noexcept {
    return static_cast<unsigned>(bits::ap::get(e.value));
  }
  [[nodiscard]] static constexpr unsigned attr_indx(entry_type e) noexcept {
    return static_cast<unsigned>(bits::attr_indx::get(e.value));
  }
  [[nodiscard]] static constexpr bool ng(entry_type e) noexcept { return bits::ng::test(e.value); }
  [[nodiscard]] static constexpr bool pxn(entry_type e) noexcept { return bits::pxn::test(e.value); }
  [[nodiscard]] static constexpr bool uxn(entry_type e) noexcept { return bits::uxn::test(e.value); }

  [[nodiscard]] static constexpr phys_type child_table_addr(entry_type e) noexcept {
    return phys_type{bits::output_address<12>(e.value)};
  }
  [[nodiscard]] static constexpr phys_type leaf_frame_addr(entry_type e) noexcept { return child_table_addr(e); }

  [[nodiscard]] static constexpr entry_type make_table_entry(phys_type child_table, bool xn_table = false,
                                                             bool pxn_table = false, unsigned ap_table = 0) noexcept {
    std::uint64_t raw = bits::valid::set(0, 1);
    raw = bits::table_or_page::set(raw, 1);
    raw = bits::xn_table::set_bit(raw, xn_table);
    raw = bits::pxn_table::set_bit(raw, pxn_table);
    raw = bits::ap_table::set(raw, ap_table);
    raw = bits::with_output_address<12>(raw, child_table.value);
    return entry_type{raw};
  }

  /** @brief Builds a leaf entry; `final_level = false` builds a block descriptor instead of a page descriptor. */
  [[nodiscard]] static constexpr entry_type make_leaf_entry(phys_type frame, unsigned ap, unsigned sh,
                                                            unsigned attr_indx, bool final_level = true, bool af = true,
                                                            bool ng = false, bool pxn = false,
                                                            bool uxn = false) noexcept {
    std::uint64_t raw = bits::valid::set(0, 1);
    raw = bits::table_or_page::set(raw, final_level ? 1 : 0);
    raw = bits::ap::set(raw, ap);
    raw = bits::sh::set(raw, sh);
    raw = bits::attr_indx::set(raw, attr_indx);
    raw = bits::af::set_bit(raw, af);
    raw = bits::ng::set_bit(raw, ng);
    raw = bits::pxn::set_bit(raw, pxn);
    raw = bits::uxn::set_bit(raw, uxn);
    raw = bits::with_output_address<12>(raw, frame.value);
    return entry_type{raw};
  }
};

/** @brief Secure PL1&0 (TrustZone Trusted OS) stage-1 entry traits; `ns()` chooses the per-entry output space. */
template <> struct page_table_entry_traits<arm::lpae::stage1_secure_tag> {
  using entry_type = page_table_entry<arm::lpae::stage1_secure_tag>;
  using phys_type = phys_addr<void, default_phys_space>;
  using bits = structo::arch::detail::vmsa::stage1_bits;

  [[nodiscard]] static constexpr bool is_present(entry_type e) noexcept { return bits::is_present(e.value); }
  [[nodiscard]] static constexpr bool is_leaf(entry_type e) noexcept { return !bits::is_table(e.value); }
  [[nodiscard]] static constexpr bool ns(entry_type e) noexcept { return bits::ns::test(e.value); }

  [[nodiscard]] static constexpr phys_type child_table_addr(entry_type e) noexcept {
    return phys_type{bits::output_address<12>(e.value)};
  }
  [[nodiscard]] static constexpr phys_type leaf_frame_addr(entry_type e) noexcept { return child_table_addr(e); }

  [[nodiscard]] static constexpr entry_type make_table_entry(phys_type child_table, bool ns_table = false,
                                                             bool xn_table = false, bool pxn_table = false,
                                                             unsigned ap_table = 0) noexcept {
    std::uint64_t raw = bits::valid::set(0, 1);
    raw = bits::table_or_page::set(raw, 1);
    raw = bits::ns_table::set_bit(raw, ns_table);
    raw = bits::xn_table::set_bit(raw, xn_table);
    raw = bits::pxn_table::set_bit(raw, pxn_table);
    raw = bits::ap_table::set(raw, ap_table);
    raw = bits::with_output_address<12>(raw, child_table.value);
    return entry_type{raw};
  }

  [[nodiscard]] static constexpr entry_type make_leaf_entry(phys_type frame, unsigned ap, unsigned sh,
                                                            unsigned attr_indx, bool ns = false,
                                                            bool final_level = true, bool af = true, bool ng = false,
                                                            bool pxn = false, bool uxn = false) noexcept {
    std::uint64_t raw = bits::valid::set(0, 1);
    raw = bits::table_or_page::set(raw, final_level ? 1 : 0);
    raw = bits::ns::set_bit(raw, ns);
    raw = bits::ap::set(raw, ap);
    raw = bits::sh::set(raw, sh);
    raw = bits::attr_indx::set(raw, attr_indx);
    raw = bits::af::set_bit(raw, af);
    raw = bits::ng::set_bit(raw, ng);
    raw = bits::pxn::set_bit(raw, pxn);
    raw = bits::uxn::set_bit(raw, uxn);
    raw = bits::with_output_address<12>(raw, frame.value);
    return entry_type{raw};
  }
};

} // namespace structo::arch
