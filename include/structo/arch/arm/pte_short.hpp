// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file pte_short.hpp
 * @brief `structo::arch::page_table_entry_traits<Tag>` specializations
 * for ARMv7-A's classic "short-descriptor" (32-bit, non-LPAE)
 * translation table format: a 4096-entry L1 table of 1MB sections (or
 * pointers to an L2 "coarse" table) and a 256-entry L2 table of 4KB
 * small pages. Pairs with
 * `structo::arch::arm::short_descriptor::level1` in
 * `arch/arm/page_table_traits.hpp`.
 *
 * ## L1 and L2 need separate tags, unlike LPAE/AArch64/x86
 *
 * Every other format in this library (VMSA long-descriptor, x86 paging,
 * RISC-V) keeps the *same* bit layout at every table level -- only one
 * bit's meaning changes between an intermediate (block-capable) level
 * and the final level (see the `final_level` parameter elsewhere in
 * this library). The short-descriptor format does not have this
 * property: its L1 (section / coarse-page-table-pointer) descriptor and
 * L2 (small-page) descriptor have genuinely different, incompatible
 * field positions (domain, AP, TEX, and XN all live at different bit
 * offsets at each level; a glance at this file's `detail::l1_bits` vs
 * `detail::l2_bits` makes this obvious). Consequently, this header
 * provides **two separate `page_table_entry_traits<Tag>`
 * specializations** for the one 2-level table:
 * `stage1_ns_tag`/`stage1_secure_tag` for L1 entries (used with
 * `make_table_entry`/`make_leaf_entry` exactly as elsewhere), and the
 * single `stage1_l2_tag` for L2 entries (leaf-only -- L2 is always the
 * final level in this table shape, so it has no `make_table_entry`).
 *
 * ## The NS bit exists only at L1, not at L2
 *
 * Unlike LPAE/AArch64, where every leaf and table descriptor at every
 * level can carry its own NS bit, the short-descriptor format's
 * Security state is fixed per-L1-entry: the Section descriptor's NS bit
 * (for a 1MB leaf) or the coarse-page-table descriptor's NS bit (for an
 * L1-to-L2 pointer) governs the *entire* 1MB region or entire L2 table
 * respectively -- small-page (L2) descriptors have no NS field of their
 * own. This is why `stage1_l2_tag`'s `phys_type` is deliberately
 * untagged (`default_phys_space`): L2 small-page output space is
 * whatever the L1 entry that pointed here already decided, not
 * something `stage1_l2_tag`'s traits can determine on their own.
 *
 * ## Out of scope
 *
 * 16MB supersections and 64KB large pages are not modeled (both
 * replicate their leaf descriptor across 16 consecutive table slots,
 * which this library's one-index-one-slot `page_table_levels<...>`
 * model does not represent -- the same reason AArch64/LPAE large-block
 * variants are scoped out elsewhere). "Fine" page tables (deprecated
 * well before ARMv7) are not modeled. The Virtualization Extensions'
 * PXN bit on L1 page-table descriptors (`bit[2]`, meaningful only when
 * the Virtualization Extensions are implemented) is modeled as an
 * optional, default-`false` parameter since it is architecturally RES0
 * otherwise and therefore always safe to leave at its default.
 */

#include <structo/arch/page_table_traits.hpp>
#include <structo/arch/pte_field.hpp>
#include <structo/phys_addr.hpp>

#include <cstdint>

namespace structo::arch::arm::short_descriptor {

/** @brief Tag: Non-secure PL1&0 short-descriptor stage-1 L1 entries. */
struct stage1_ns_tag {};

/** @brief Tag: Secure PL1&0 (TrustZone Trusted OS) short-descriptor stage-1 L1 entries. */
struct stage1_secure_tag {};

/** @brief Tag: short-descriptor stage-1 L2 (small-page) entries. Security state is inherited from the L1 entry. */
struct stage1_l2_tag {};

namespace detail {

/** @brief L1 descriptor bit positions, shared by Section leaves and coarse-page-table pointers. */
struct l1_bits {
  using type = pte_bit_field<0, 2, std::uint32_t>;      // 0b00 fault, 0b01 coarse table, 0b10/0b11 section
  using pxn_table = pte_bit_field<2, 1, std::uint32_t>; // page-table descriptor only; Virtualization Extensions
  using ns_table = pte_bit_field<3, 1, std::uint32_t>;  // page-table descriptor only
  using b = pte_bit_field<2, 1, std::uint32_t>;         // section only; overlaps pxn_table (different descriptor kind)
  using c = pte_bit_field<3, 1, std::uint32_t>;         // section only; overlaps ns_table (different descriptor kind)
  using xn = pte_bit_field<4, 1, std::uint32_t>;        // section only
  using domain = pte_bit_field<5, 4, std::uint32_t>;    // both coarse-table and section descriptors
  using ap01 = pte_bit_field<10, 2, std::uint32_t>;     // section only: AP[1:0]
  using tex = pte_bit_field<12, 3, std::uint32_t>;      // section only
  using apx = pte_bit_field<15, 1, std::uint32_t>;      // section only
  using s = pte_bit_field<16, 1, std::uint32_t>;        // section only
  using ng = pte_bit_field<17, 1, std::uint32_t>;       // section only
  using ns_section = pte_bit_field<19, 1, std::uint32_t>;    // section only
  using table_addr = pte_bit_field<10, 22, std::uint32_t>;   // coarse-page-table base, 1KB-aligned
  using section_addr = pte_bit_field<20, 12, std::uint32_t>; // section base, 1MB-aligned

  [[nodiscard]] static constexpr bool is_present(std::uint32_t raw) noexcept { return type::get(raw) != 0; }
  [[nodiscard]] static constexpr bool is_table(std::uint32_t raw) noexcept { return type::get(raw) == 0b01; }
};

/** @brief L2 ("coarse") small-page descriptor bit positions -- a different layout from `l1_bits`. */
struct l2_bits {
  using present = pte_bit_field<1, 1, std::uint32_t>; // small page: bit[1] fixed 1; bit[0] is XN, not part of "type"
  using xn = pte_bit_field<0, 1, std::uint32_t>;
  using b = pte_bit_field<2, 1, std::uint32_t>;
  using c = pte_bit_field<3, 1, std::uint32_t>;
  using ap01 = pte_bit_field<4, 2, std::uint32_t>;
  using tex = pte_bit_field<6, 3, std::uint32_t>;
  using apx = pte_bit_field<9, 1, std::uint32_t>;
  using s = pte_bit_field<10, 1, std::uint32_t>;
  using ng = pte_bit_field<11, 1, std::uint32_t>;
  using addr = pte_bit_field<12, 20, std::uint32_t>; // small-page base, 4KB-aligned
};

/** @brief Accessors common to both L1 tags (Section-descriptor fields). */
template <typename Entry> struct stage1_l1_common_accessors {
  [[nodiscard]] static constexpr bool is_present(Entry e) noexcept { return l1_bits::is_present(e.value); }
  /** @brief `true` for a 1MB Section leaf; `false` for a coarse-page-table pointer. Query only at L1. */
  [[nodiscard]] static constexpr bool is_leaf(Entry e) noexcept { return !l1_bits::is_table(e.value); }

  [[nodiscard]] static constexpr bool bufferable(Entry e) noexcept { return l1_bits::b::test(e.value); }
  [[nodiscard]] static constexpr bool cacheable(Entry e) noexcept { return l1_bits::c::test(e.value); }
  [[nodiscard]] static constexpr bool execute_never(Entry e) noexcept { return l1_bits::xn::test(e.value); }
  [[nodiscard]] static constexpr unsigned domain(Entry e) noexcept {
    return static_cast<unsigned>(l1_bits::domain::get(e.value));
  }
  [[nodiscard]] static constexpr unsigned ap(Entry e) noexcept {
    return static_cast<unsigned>(l1_bits::ap01::get(e.value));
  }
  [[nodiscard]] static constexpr bool apx(Entry e) noexcept { return l1_bits::apx::test(e.value); }
  [[nodiscard]] static constexpr unsigned tex(Entry e) noexcept {
    return static_cast<unsigned>(l1_bits::tex::get(e.value));
  }
  [[nodiscard]] static constexpr bool shareable(Entry e) noexcept { return l1_bits::s::test(e.value); }
  [[nodiscard]] static constexpr bool not_global(Entry e) noexcept { return l1_bits::ng::test(e.value); }
};

} // namespace detail

} // namespace structo::arch::arm::short_descriptor

namespace structo::arch {

/** @brief Fixed-Non-secure-output short-descriptor stage-1 L1 entry traits. */
template <>
struct page_table_entry_traits<arm::short_descriptor::stage1_ns_tag>
    : arm::short_descriptor::detail::stage1_l1_common_accessors<
          page_table_entry<arm::short_descriptor::stage1_ns_tag>> {
  using entry_type = page_table_entry<arm::short_descriptor::stage1_ns_tag>;
  using phys_type = phys_addr<void, nonsecure_phys_space>;
  using bits = arm::short_descriptor::detail::l1_bits;

  [[nodiscard]] static constexpr phys_type child_table_addr(entry_type e) noexcept {
    return phys_type{static_cast<std::uint64_t>(bits::table_addr::get(e.value)) << 10};
  }
  [[nodiscard]] static constexpr phys_type leaf_frame_addr(entry_type e) noexcept {
    return phys_type{static_cast<std::uint64_t>(bits::section_addr::get(e.value)) << 20};
  }

  [[nodiscard]] static constexpr entry_type make_table_entry(phys_type child_table, unsigned domain = 0,
                                                             bool pxn_table = false) noexcept {
    std::uint32_t raw = bits::type::set(0, 0b01);
    raw = bits::domain::set(raw, domain);
    raw = bits::pxn_table::set_bit(raw, pxn_table);
    raw = bits::table_addr::set(raw, static_cast<std::uint32_t>(child_table.value >> 10));
    return entry_type{raw};
  }

  /** @brief Builds a 1MB Section leaf entry. */
  [[nodiscard]] static constexpr entry_type make_leaf_entry(phys_type frame, unsigned ap, unsigned tex,
                                                            unsigned domain = 0, bool bufferable = false,
                                                            bool cacheable = false, bool execute_never = false,
                                                            bool apx = false, bool shareable = false,
                                                            bool not_global = false) noexcept {
    std::uint32_t raw = bits::type::set(0, 0b10);
    raw = bits::b::set_bit(raw, bufferable);
    raw = bits::c::set_bit(raw, cacheable);
    raw = bits::xn::set_bit(raw, execute_never);
    raw = bits::domain::set(raw, domain);
    raw = bits::ap01::set(raw, ap);
    raw = bits::tex::set(raw, tex);
    raw = bits::apx::set_bit(raw, apx);
    raw = bits::s::set_bit(raw, shareable);
    raw = bits::ng::set_bit(raw, not_global);
    raw = bits::section_addr::set(raw, static_cast<std::uint32_t>(frame.value >> 20));
    return entry_type{raw};
  }
};

/** @brief Secure PL1&0 (TrustZone Trusted OS) short-descriptor stage-1 L1 entry traits; `ns()` is live. */
template <>
struct page_table_entry_traits<arm::short_descriptor::stage1_secure_tag>
    : arm::short_descriptor::detail::stage1_l1_common_accessors<
          page_table_entry<arm::short_descriptor::stage1_secure_tag>> {
  using entry_type = page_table_entry<arm::short_descriptor::stage1_secure_tag>;
  // The output space is a per-entry runtime choice (the NS bit), not static type
  // information -- callers recover the tagged address via `ns()` + `cast_space()`.
  using phys_type = phys_addr<void, default_phys_space>;
  using bits = arm::short_descriptor::detail::l1_bits;

  /** @brief `true`: this entry's output region (1MB section, or everything behind this L2 table) is Non-secure. */
  [[nodiscard]] static constexpr bool ns(entry_type e) noexcept {
    return is_leaf(e) ? bits::ns_section::test(e.value) : bits::ns_table::test(e.value);
  }

  [[nodiscard]] static constexpr phys_type child_table_addr(entry_type e) noexcept {
    return phys_type{static_cast<std::uint64_t>(bits::table_addr::get(e.value)) << 10};
  }
  [[nodiscard]] static constexpr phys_type leaf_frame_addr(entry_type e) noexcept {
    return phys_type{static_cast<std::uint64_t>(bits::section_addr::get(e.value)) << 20};
  }

  [[nodiscard]] static constexpr entry_type make_table_entry(phys_type child_table, bool ns_table = false,
                                                             unsigned domain = 0, bool pxn_table = false) noexcept {
    std::uint32_t raw = bits::type::set(0, 0b01);
    raw = bits::ns_table::set_bit(raw, ns_table);
    raw = bits::domain::set(raw, domain);
    raw = bits::pxn_table::set_bit(raw, pxn_table);
    raw = bits::table_addr::set(raw, static_cast<std::uint32_t>(child_table.value >> 10));
    return entry_type{raw};
  }

  /** @brief Builds a 1MB Section leaf entry; `ns = true` makes this region's output Non-secure. */
  [[nodiscard]] static constexpr entry_type make_leaf_entry(phys_type frame, unsigned ap, unsigned tex, bool ns = false,
                                                            unsigned domain = 0, bool bufferable = false,
                                                            bool cacheable = false, bool execute_never = false,
                                                            bool apx = false, bool shareable = false,
                                                            bool not_global = false) noexcept {
    std::uint32_t raw = bits::type::set(0, 0b10);
    raw = bits::ns_section::set_bit(raw, ns);
    raw = bits::b::set_bit(raw, bufferable);
    raw = bits::c::set_bit(raw, cacheable);
    raw = bits::xn::set_bit(raw, execute_never);
    raw = bits::domain::set(raw, domain);
    raw = bits::ap01::set(raw, ap);
    raw = bits::tex::set(raw, tex);
    raw = bits::apx::set_bit(raw, apx);
    raw = bits::s::set_bit(raw, shareable);
    raw = bits::ng::set_bit(raw, not_global);
    raw = bits::section_addr::set(raw, static_cast<std::uint32_t>(frame.value >> 20));
    return entry_type{raw};
  }
};

/** @brief Short-descriptor stage-1 L2 (4KB small-page) entry traits. Always a leaf; security is inherited from L1. */
template <> struct page_table_entry_traits<arm::short_descriptor::stage1_l2_tag> {
  using entry_type = page_table_entry<arm::short_descriptor::stage1_l2_tag>;
  using phys_type = phys_addr<void, default_phys_space>;
  using bits = arm::short_descriptor::detail::l2_bits;

  [[nodiscard]] static constexpr bool is_present(entry_type e) noexcept { return bits::present::test(e.value); }
  /** @brief Always `true`: this table has no further levels, so every present entry here is a leaf. */
  [[nodiscard]] static constexpr bool is_leaf(entry_type e) noexcept { return is_present(e); }

  [[nodiscard]] static constexpr bool execute_never(entry_type e) noexcept { return bits::xn::test(e.value); }
  [[nodiscard]] static constexpr bool bufferable(entry_type e) noexcept { return bits::b::test(e.value); }
  [[nodiscard]] static constexpr bool cacheable(entry_type e) noexcept { return bits::c::test(e.value); }
  [[nodiscard]] static constexpr unsigned ap(entry_type e) noexcept {
    return static_cast<unsigned>(bits::ap01::get(e.value));
  }
  [[nodiscard]] static constexpr unsigned tex(entry_type e) noexcept {
    return static_cast<unsigned>(bits::tex::get(e.value));
  }
  [[nodiscard]] static constexpr bool apx(entry_type e) noexcept { return bits::apx::test(e.value); }
  [[nodiscard]] static constexpr bool shareable(entry_type e) noexcept { return bits::s::test(e.value); }
  [[nodiscard]] static constexpr bool not_global(entry_type e) noexcept { return bits::ng::test(e.value); }

  [[nodiscard]] static constexpr phys_type child_table_addr(entry_type) noexcept = delete; // L2 is always a leaf here
  [[nodiscard]] static constexpr phys_type leaf_frame_addr(entry_type e) noexcept {
    return phys_type{static_cast<std::uint64_t>(bits::addr::get(e.value)) << 12};
  }

  static constexpr entry_type make_table_entry(phys_type) noexcept = delete; // L2 is always a leaf here

  /** @brief Builds a 4KB small-page leaf entry. */
  [[nodiscard]] static constexpr entry_type make_leaf_entry(phys_type frame, unsigned ap, unsigned tex,
                                                            bool execute_never = false, bool bufferable = false,
                                                            bool cacheable = false, bool apx = false,
                                                            bool shareable = false, bool not_global = false) noexcept {
    std::uint32_t raw = bits::present::set_bit(0, true);
    raw = bits::xn::set_bit(raw, execute_never);
    raw = bits::b::set_bit(raw, bufferable);
    raw = bits::c::set_bit(raw, cacheable);
    raw = bits::ap01::set(raw, ap);
    raw = bits::tex::set(raw, tex);
    raw = bits::apx::set_bit(raw, apx);
    raw = bits::s::set_bit(raw, shareable);
    raw = bits::ng::set_bit(raw, not_global);
    raw = bits::addr::set(raw, static_cast<std::uint32_t>(frame.value >> 12));
    return entry_type{raw};
  }
};

} // namespace structo::arch
