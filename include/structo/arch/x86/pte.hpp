// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file pte.hpp
 * @brief `structo::arch::page_table_entry_traits<Tag>` specializations
 * for x86/x86-64 ordinary paging table/directory/page descriptors
 * (i386, PAE, and long-mode 4-/5-level all share this bit layout),
 * shared verbatim with AMD's Nested Page Tables (NPT) hypervisor-staging
 * format -- unlike Intel's EPT (see `pte_ept.hpp`), AMD NPT reuses the
 * exact same encoding as ordinary paging, just rooted at `nCR3` instead
 * of `CR3` and walked a second time per guest access.
 *
 * ## The PS/PAT bit-position caveat
 *
 * Bit 7 means two different things depending on the level: at an
 * intermediate level that permits a huge page (PDPTE/PDE), it is `PS`
 * ("page size"; `1` means this descriptor is itself a huge-page leaf,
 * not a pointer to the next table). At the leaf-adjacent (final, 4KB)
 * level, the *same bit position* instead means `PAT` (a memory-type
 * selector), since every final-level descriptor is unconditionally a
 * leaf already. This mirrors `arm64::pte_stage1.hpp`'s page-vs-block
 * distinction: `is_leaf()`/`make_leaf_entry()`'s `final_level` parameter
 * exists for exactly the same reason, and `is_leaf()` should only be
 * queried at an intermediate, huge-page-capable level.
 *
 * ## Field layout
 *
 * - Bit 0: P (present)
 * - Bit 1: R/W (0 = read-only, 1 = read-write)
 * - Bit 2: U/S (0 = supervisor-only, 1 = user-accessible)
 * - Bit 3: PWT, Bit 4: PCD (cache control)
 * - Bit 5: A (accessed)
 * - Bit 6: D (dirty, leaf only)
 * - Bit 7: PS (intermediate level) / PAT (final level) -- see above
 * - Bit 8: G (global, leaf only)
 * - Bits [11:9]: AVL (software use)
 * - Bits [51:12]: output address (next-level table, or leaf frame)
 * - Bit 63: XD/NX (execute-disable; requires `EFER.NXE` in long mode,
 *   otherwise reserved/ignored)
 */

#include <structo/arch/page_table_traits.hpp>
#include <structo/arch/pte_field.hpp>
#include <structo/phys_addr.hpp>

#include <cstdint>

namespace structo::arch::x86 {

/** @brief Tag: ordinary OS paging (i386/PAE/long-mode, `CR3`-rooted). */
struct pte_tag {};

/** @brief Tag: AMD Nested Page Tables (NPT) hypervisor staging, `nCR3`-rooted; identical bit layout to `pte_tag`. */
struct npt_tag {};

namespace detail {

struct pte_bits {
  using present = pte_bit_field<0, 1>;
  using rw = pte_bit_field<1, 1>;
  using us = pte_bit_field<2, 1>;
  using pwt = pte_bit_field<3, 1>;
  using pcd = pte_bit_field<4, 1>;
  using accessed = pte_bit_field<5, 1>;
  using dirty = pte_bit_field<6, 1>;
  using ps_or_pat = pte_bit_field<7, 1>;
  using global = pte_bit_field<8, 1>;
  using avl = pte_bit_field<9, 3>;
  using addr = pte_bit_field<12, 40>; // bits [51:12]
  using xd = pte_bit_field<63, 1>;

  [[nodiscard]] static constexpr bool is_present(std::uint64_t raw) noexcept { return present::test(raw); }
  [[nodiscard]] static constexpr bool is_leaf(std::uint64_t raw) noexcept { return ps_or_pat::test(raw); }
};

template <typename Tag, typename PhysSpaceTag> struct pte_traits_impl {
  using entry_type = page_table_entry<Tag>;
  using phys_type = phys_addr<void, PhysSpaceTag>;
  using bits = pte_bits;

  [[nodiscard]] static constexpr bool is_present(entry_type e) noexcept { return bits::is_present(e.value); }
  /** @brief `true` for a huge-page ("PS") leaf. Only meaningful at an intermediate, huge-page-capable level. */
  [[nodiscard]] static constexpr bool is_leaf(entry_type e) noexcept { return bits::is_leaf(e.value); }

  [[nodiscard]] static constexpr bool writable(entry_type e) noexcept { return bits::rw::test(e.value); }
  [[nodiscard]] static constexpr bool user_accessible(entry_type e) noexcept { return bits::us::test(e.value); }
  [[nodiscard]] static constexpr bool write_through(entry_type e) noexcept { return bits::pwt::test(e.value); }
  [[nodiscard]] static constexpr bool cache_disabled(entry_type e) noexcept { return bits::pcd::test(e.value); }
  [[nodiscard]] static constexpr bool accessed(entry_type e) noexcept { return bits::accessed::test(e.value); }
  [[nodiscard]] static constexpr bool dirty(entry_type e) noexcept { return bits::dirty::test(e.value); }
  /** @brief The raw bit-7 value: huge-page leaf (intermediate level) or PAT (final level) -- see the file doc. */
  [[nodiscard]] static constexpr bool ps_or_pat(entry_type e) noexcept { return bits::ps_or_pat::test(e.value); }
  [[nodiscard]] static constexpr bool global_mapping(entry_type e) noexcept { return bits::global::test(e.value); }
  [[nodiscard]] static constexpr unsigned avl(entry_type e) noexcept { return static_cast<unsigned>(bits::avl::get(e.value)); }
  [[nodiscard]] static constexpr bool execute_disabled(entry_type e) noexcept { return bits::xd::test(e.value); }

  [[nodiscard]] static constexpr phys_type child_table_addr(entry_type e) noexcept {
    return phys_type{bits::addr::get(e.value) << 12};
  }
  [[nodiscard]] static constexpr phys_type leaf_frame_addr(entry_type e) noexcept { return child_table_addr(e); }

  [[nodiscard]] static constexpr entry_type make_table_entry(phys_type child_table, bool writable = true,
                                                              bool user_accessible = false) noexcept {
    std::uint64_t raw = bits::present::set(0, 1);
    raw = bits::rw::set_bit(raw, writable);
    raw = bits::us::set_bit(raw, user_accessible);
    raw = bits::addr::set(raw, child_table.value >> 12);
    return entry_type{raw};
  }

  /**
   * @brief Builds a leaf entry. `final_level = false` (the default is
   * `true`, the common 4KB-page case) sets bit 7 as `PS` for an
   * intermediate-level huge-page leaf instead of `PAT`.
   */
  [[nodiscard]] static constexpr entry_type make_leaf_entry(phys_type frame, bool writable, bool user_accessible,
                                                             bool final_level = true, bool pwt = false,
                                                             bool pcd = false, bool accessed = true,
                                                             bool dirty = false, bool global_mapping = false,
                                                             bool execute_disabled = false, unsigned avl = 0) noexcept {
    std::uint64_t raw = bits::present::set(0, 1);
    raw = bits::rw::set_bit(raw, writable);
    raw = bits::us::set_bit(raw, user_accessible);
    raw = bits::pwt::set_bit(raw, pwt);
    raw = bits::pcd::set_bit(raw, pcd);
    raw = bits::accessed::set_bit(raw, accessed);
    raw = bits::dirty::set_bit(raw, dirty);
    raw = bits::ps_or_pat::set_bit(raw, !final_level); // PS=1 for a huge leaf; final-level PAT left at 0
    raw = bits::global::set_bit(raw, global_mapping);
    raw = bits::avl::set(raw, avl);
    raw = bits::xd::set_bit(raw, execute_disabled);
    raw = bits::addr::set(raw, frame.value >> 12);
    return entry_type{raw};
  }
};

} // namespace detail

} // namespace structo::arch::x86

namespace structo::arch {

template <> struct page_table_entry_traits<x86::pte_tag> : x86::detail::pte_traits_impl<x86::pte_tag, default_phys_space> {};

template <> struct page_table_entry_traits<x86::npt_tag> : x86::detail::pte_traits_impl<x86::npt_tag, host_phys_space> {};

} // namespace structo::arch
