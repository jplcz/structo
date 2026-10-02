// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file pte_stage1.hpp
 * @brief `structo::arch::page_table_entry_traits<Tag>` specializations
 * for AArch64 (VMSAv8-64) stage-1 (virtual-address-to-physical-address)
 * table/block/page descriptors, covering every stage-1 translation
 * regime: ordinary Non-secure EL1&0/EL2(&0), Secure EL1&0 (classic
 * TrustZone Trusted OS / EL3), and Secure EL2(&0) ("sEL2").
 *
 * ## Which regime needs which tag, and why there are three
 *
 * All four stage-1 regimes share *exactly* the same bit layout -- the
 * difference between them is entirely which physical address space a
 * descriptor's output address lands in, and whether that is fixed or a
 * per-entry choice:
 *
 * - **`stage1_ns_tag`** -- Non-secure EL1&0, and any EL2/EL2&0 regime
 *   (Non-secure EL2 never has a Secure counterpart worth distinguishing
 *   here; see `stage1_secure_el2_tag` for when it does). Output is
 *   unconditionally Non-secure; the NS bit is architecturally RES0 in
 *   this regime; this header does not expose an `ns()` accessor for it.
 * - **`stage1_secure_tag`** -- Secure EL1&0 (the classic TrustZone
 *   Trusted OS) and EL3. This is the **one** regime where the NS bit is
 *   live: software running Secure can map a page as Secure *or*
 *   Non-secure output, per entry (e.g. to share a buffer with the
 *   Non-secure world). Because that choice is a runtime bit, not static
 *   type information, `leaf_frame_addr()`/`child_table_addr()` return an
 *   untagged `phys_addr<void, default_phys_space>` here -- call `ns()`
 *   and `cast_space()` to recover the concrete tagged address once the
 *   bit's value is known.
 * - **`stage1_secure_el2_tag`** -- Secure EL2/EL2&0 ("sEL2", `FEAT_SEL2`),
 *   the Secure-world hypervisor regime. Despite running in Secure state,
 *   sEL2's own stage-1 NS bit is RES0 just like ordinary EL2's -- a
 *   Secure EL2 cannot opt individual stage-1 entries into Non-secure
 *   output, only Secure EL1&0 can. Output is unconditionally Secure.
 *
 * `FEAT_RME` (Realm Management Extension) is explicitly out of scope,
 * same as elsewhere in this library: it adds a fourth, Realm, physical
 * address space and an `NSE`+`NS` 2-bit PAS selector this header does
 * not model.
 *
 * ## Example
 *
 * @code
 * using namespace structo::arch;
 * using entry = page_table_entry<arm64::stage1_ns_tag<>>;
 * using traits = page_table_entry_traits<arm64::stage1_ns_tag<>>;
 *
 * entry e = traits::make_leaf_entry(traits::phys_type{0x4000'0000}, 0b01, 0b11, 0); // ap, sh, attr_indx
 * bool present = traits::is_present(e);
 * auto frame = traits::leaf_frame_addr(e);
 * @endcode
 */

#include <structo/arch/page_table_traits.hpp>
#include <structo/arch/vmsa_pte_fields.hpp>
#include <structo/phys_addr.hpp>
#include <structo/phys_page.hpp>

namespace structo::arch::arm64 {

/** @brief Tag: Non-secure EL1&0, or any EL2/EL2&0, stage-1 translation regime. */
template <typename LeafPageTraits = structo::page_4k> struct stage1_ns_tag {};

/** @brief Tag: Secure EL1&0 (TrustZone Trusted OS) or EL3 stage-1 translation regime. */
template <typename LeafPageTraits = structo::page_4k> struct stage1_secure_tag {};

/** @brief Tag: Secure EL2/EL2&0 ("sEL2") stage-1 translation regime. */
template <typename LeafPageTraits = structo::page_4k> struct stage1_secure_el2_tag {};

namespace detail {

// Shared accessor surface for every stage-1 regime: table/leaf identity,
// AF/SH/AP/AttrIndx/nG/contiguous/PXN/UXN, and NSTable/APTable/XNTable/
// PXNTable on a table descriptor. `ns()` is deliberately not provided
// here -- only `stage1_secure_tag` exposes it, since it's RES0 (and thus
// meaningless) everywhere else.
template <typename Entry> struct stage1_common_accessors {
  using bits = structo::arch::detail::vmsa::stage1_bits;

  [[nodiscard]] static constexpr bool is_present(Entry e) noexcept { return bits::is_present(e.value); }
  /**
   * @brief `true` for an early-terminating "block" descriptor. Only
   * meaningful at an intermediate level with `allows_leaf == true`; at
   * the leaf-adjacent (final) level, bit 1 instead distinguishes a valid
   * "page" descriptor (1) from reserved/invalid (0), so the walker
   * should treat that level as unconditionally a leaf without calling
   * this.
   */
  [[nodiscard]] static constexpr bool is_leaf(Entry e) noexcept { return !bits::is_table(e.value); }

  [[nodiscard]] static constexpr unsigned af(Entry e) noexcept { return static_cast<unsigned>(bits::af::get(e.value)); }
  [[nodiscard]] static constexpr unsigned sh(Entry e) noexcept { return static_cast<unsigned>(bits::sh::get(e.value)); }
  [[nodiscard]] static constexpr unsigned ap(Entry e) noexcept { return static_cast<unsigned>(bits::ap::get(e.value)); }
  [[nodiscard]] static constexpr unsigned attr_indx(Entry e) noexcept {
    return static_cast<unsigned>(bits::attr_indx::get(e.value));
  }
  [[nodiscard]] static constexpr bool ng(Entry e) noexcept { return bits::ng::test(e.value); }
  [[nodiscard]] static constexpr bool contiguous(Entry e) noexcept { return bits::contiguous::test(e.value); }
  [[nodiscard]] static constexpr bool pxn(Entry e) noexcept { return bits::pxn::test(e.value); }
  [[nodiscard]] static constexpr bool uxn(Entry e) noexcept { return bits::uxn::test(e.value); }

  // Table-descriptor-only upper attributes (only meaningful when !is_leaf(e)).
  [[nodiscard]] static constexpr bool pxn_table(Entry e) noexcept { return bits::pxn_table::test(e.value); }
  [[nodiscard]] static constexpr bool xn_table(Entry e) noexcept { return bits::xn_table::test(e.value); }
  [[nodiscard]] static constexpr unsigned ap_table(Entry e) noexcept {
    return static_cast<unsigned>(bits::ap_table::get(e.value));
  }
  [[nodiscard]] static constexpr bool ns_table(Entry e) noexcept { return bits::ns_table::test(e.value); }
};

} // namespace detail

} // namespace structo::arch::arm64

namespace structo::arch {

/** @brief Fixed-Non-secure-output stage-1 entry traits (Non-secure EL1&0, any EL2/EL2&0). */
template <typename LeafPageTraits>
struct page_table_entry_traits<arm64::stage1_ns_tag<LeafPageTraits>>
    : arm64::detail::stage1_common_accessors<page_table_entry<arm64::stage1_ns_tag<LeafPageTraits>>> {
  using entry_type = page_table_entry<arm64::stage1_ns_tag<LeafPageTraits>>;
  using phys_type = phys_addr<void, nonsecure_phys_space>;

  [[nodiscard]] static constexpr phys_type child_table_addr(entry_type e) noexcept {
    using bits = structo::arch::detail::vmsa::stage1_bits;
    return phys_type{bits::output_address<LeafPageTraits::page_shift>(e.value)};
  }
  [[nodiscard]] static constexpr phys_type leaf_frame_addr(entry_type e) noexcept { return child_table_addr(e); }

  [[nodiscard]] static constexpr entry_type make_table_entry(phys_type child_table, bool xn_table = false,
                                                             bool pxn_table = false, unsigned ap_table = 0) noexcept {
    using bits = structo::arch::detail::vmsa::stage1_bits;
    std::uint64_t raw = bits::valid::set(0, 1);
    raw = bits::table_or_page::set(raw, 1);
    raw = bits::xn_table::set_bit(raw, xn_table);
    raw = bits::pxn_table::set_bit(raw, pxn_table);
    raw = bits::ap_table::set(raw, ap_table);
    raw = bits::with_output_address<LeafPageTraits::page_shift>(raw, child_table.value);
    return entry_type{raw};
  }

  /**
   * @brief Builds a leaf entry: a true "page" descriptor at the leaf-
   * adjacent (final) level when `final_level` is `true` (the default),
   * or an early-terminating "block" descriptor at an intermediate level
   * that permits one (see `page_table_level::allows_leaf`) when `false`.
   * Bit 1 of the raw descriptor is the only difference between the two
   * (`1` = page, `0` = block); `is_leaf()` only applies to the latter,
   * since the walker already knows the final level is unconditionally a
   * leaf without needing to ask.
   */
  [[nodiscard]] static constexpr entry_type make_leaf_entry(phys_type frame, unsigned ap, unsigned sh,
                                                            unsigned attr_indx, bool final_level = true, bool af = true,
                                                            bool ng = false, bool contiguous = false, bool pxn = false,
                                                            bool uxn = false) noexcept {
    using bits = structo::arch::detail::vmsa::stage1_bits;
    std::uint64_t raw = bits::valid::set(0, 1);
    raw = bits::table_or_page::set(raw, final_level ? 1 : 0);
    raw = bits::ap::set(raw, ap);
    raw = bits::sh::set(raw, sh);
    raw = bits::attr_indx::set(raw, attr_indx);
    raw = bits::af::set_bit(raw, af);
    raw = bits::ng::set_bit(raw, ng);
    raw = bits::contiguous::set_bit(raw, contiguous);
    raw = bits::pxn::set_bit(raw, pxn);
    raw = bits::uxn::set_bit(raw, uxn);
    raw = bits::with_output_address<LeafPageTraits::page_shift>(raw, frame.value);
    return entry_type{raw};
  }
};

/**
 * @brief Secure EL1&0 (TrustZone Trusted OS) / EL3 stage-1 entry traits.
 * The NS bit is live here: `ns(e) == true` means this entry's output
 * address is Non-secure despite the walk itself being Secure.
 */
template <typename LeafPageTraits>
struct page_table_entry_traits<arm64::stage1_secure_tag<LeafPageTraits>>
    : arm64::detail::stage1_common_accessors<page_table_entry<arm64::stage1_secure_tag<LeafPageTraits>>> {
  using entry_type = page_table_entry<arm64::stage1_secure_tag<LeafPageTraits>>;
  // The output space is a per-entry runtime choice (the NS bit), not static type
  // information -- callers recover the tagged address via `ns()` + `cast_space()`.
  using phys_type = phys_addr<void, default_phys_space>;

  /** @brief `true`: this entry's output address is Non-secure. `false`: Secure. */
  [[nodiscard]] static constexpr bool ns(entry_type e) noexcept {
    return structo::arch::detail::vmsa::stage1_bits::ns::test(e.value);
  }

  [[nodiscard]] static constexpr phys_type child_table_addr(entry_type e) noexcept {
    using bits = structo::arch::detail::vmsa::stage1_bits;
    return phys_type{bits::output_address<LeafPageTraits::page_shift>(e.value)};
  }
  [[nodiscard]] static constexpr phys_type leaf_frame_addr(entry_type e) noexcept { return child_table_addr(e); }

  [[nodiscard]] static constexpr entry_type make_table_entry(phys_type child_table, bool ns_table = false,
                                                             bool xn_table = false, bool pxn_table = false,
                                                             unsigned ap_table = 0) noexcept {
    using bits = structo::arch::detail::vmsa::stage1_bits;
    std::uint64_t raw = bits::valid::set(0, 1);
    raw = bits::table_or_page::set(raw, 1);
    raw = bits::ns_table::set_bit(raw, ns_table);
    raw = bits::xn_table::set_bit(raw, xn_table);
    raw = bits::pxn_table::set_bit(raw, pxn_table);
    raw = bits::ap_table::set(raw, ap_table);
    raw = bits::with_output_address<LeafPageTraits::page_shift>(raw, child_table.value);
    return entry_type{raw};
  }

  /** @brief Builds a leaf entry; see the overload in `stage1_ns_tag`'s traits for `final_level`'s meaning. */
  [[nodiscard]] static constexpr entry_type make_leaf_entry(phys_type frame, unsigned ap, unsigned sh,
                                                            unsigned attr_indx, bool ns = false,
                                                            bool final_level = true, bool af = true, bool ng = false,
                                                            bool contiguous = false, bool pxn = false,
                                                            bool uxn = false) noexcept {
    using bits = structo::arch::detail::vmsa::stage1_bits;
    std::uint64_t raw = bits::valid::set(0, 1);
    raw = bits::table_or_page::set(raw, final_level ? 1 : 0);
    raw = bits::ns::set_bit(raw, ns);
    raw = bits::ap::set(raw, ap);
    raw = bits::sh::set(raw, sh);
    raw = bits::attr_indx::set(raw, attr_indx);
    raw = bits::af::set_bit(raw, af);
    raw = bits::ng::set_bit(raw, ng);
    raw = bits::contiguous::set_bit(raw, contiguous);
    raw = bits::pxn::set_bit(raw, pxn);
    raw = bits::uxn::set_bit(raw, uxn);
    raw = bits::with_output_address<LeafPageTraits::page_shift>(raw, frame.value);
    return entry_type{raw};
  }
};

/** @brief Fixed-Secure-output stage-1 entry traits for Secure EL2/EL2&0 ("sEL2"). */
template <typename LeafPageTraits>
struct page_table_entry_traits<arm64::stage1_secure_el2_tag<LeafPageTraits>>
    : arm64::detail::stage1_common_accessors<page_table_entry<arm64::stage1_secure_el2_tag<LeafPageTraits>>> {
  using entry_type = page_table_entry<arm64::stage1_secure_el2_tag<LeafPageTraits>>;
  using phys_type = phys_addr<void, secure_phys_space>;

  [[nodiscard]] static constexpr phys_type child_table_addr(entry_type e) noexcept {
    using bits = structo::arch::detail::vmsa::stage1_bits;
    return phys_type{bits::output_address<LeafPageTraits::page_shift>(e.value)};
  }
  [[nodiscard]] static constexpr phys_type leaf_frame_addr(entry_type e) noexcept { return child_table_addr(e); }

  [[nodiscard]] static constexpr entry_type make_table_entry(phys_type child_table, bool xn_table = false,
                                                             bool pxn_table = false, unsigned ap_table = 0) noexcept {
    using bits = structo::arch::detail::vmsa::stage1_bits;
    std::uint64_t raw = bits::valid::set(0, 1);
    raw = bits::table_or_page::set(raw, 1);
    raw = bits::xn_table::set_bit(raw, xn_table);
    raw = bits::pxn_table::set_bit(raw, pxn_table);
    raw = bits::ap_table::set(raw, ap_table);
    raw = bits::with_output_address<LeafPageTraits::page_shift>(raw, child_table.value);
    return entry_type{raw};
  }

  /** @brief Builds a leaf entry; see the overload in `stage1_ns_tag`'s traits for `final_level`'s meaning. */
  [[nodiscard]] static constexpr entry_type make_leaf_entry(phys_type frame, unsigned ap, unsigned sh,
                                                            unsigned attr_indx, bool final_level = true, bool af = true,
                                                            bool ng = false, bool contiguous = false, bool pxn = false,
                                                            bool uxn = false) noexcept {
    using bits = structo::arch::detail::vmsa::stage1_bits;
    std::uint64_t raw = bits::valid::set(0, 1);
    raw = bits::table_or_page::set(raw, final_level ? 1 : 0);
    raw = bits::ap::set(raw, ap);
    raw = bits::sh::set(raw, sh);
    raw = bits::attr_indx::set(raw, attr_indx);
    raw = bits::af::set_bit(raw, af);
    raw = bits::ng::set_bit(raw, ng);
    raw = bits::contiguous::set_bit(raw, contiguous);
    raw = bits::pxn::set_bit(raw, pxn);
    raw = bits::uxn::set_bit(raw, uxn);
    raw = bits::with_output_address<LeafPageTraits::page_shift>(raw, frame.value);
    return entry_type{raw};
  }
};

} // namespace structo::arch
