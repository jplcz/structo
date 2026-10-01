// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file pte.hpp
 * @brief `structo::arch::page_table_entry_traits<Tag>` specializations
 * for the RISC-V privileged architecture's PTE format, shared verbatim
 * by both ordinary (S-stage, `satp`-rooted) and hypervisor-staging
 * (G-stage, `hgatp`-rooted, the RISC-V H-extension's guest-physical-
 * address translation) walks.
 *
 * ## One format, two regimes, no TrustZone-equivalent
 *
 * Unlike ARM, RISC-V's base privileged ISA has no Secure/Non-secure
 * split baked into the page-table format itself (there is no RISC-V
 * equivalent of TrustZone's per-entry NS bit at the ISA level -- a
 * RISC-V platform wanting that draws on PMP/IOPMP or a separate
 * standard, not a page-table bit), and the G-stage format used for
 * "hypervisor staging" (translating a guest's GPA to a real PA) is
 * *bit-for-bit identical* to the S-stage format -- RISC-V deliberately
 * reused one PTE encoding for both rather than defining a second one the
 * way ARM64 (stage 1 vs. stage 2) and x86 (normal paging vs. Intel EPT)
 * do. Two tags are still provided, purely for the usual type-safety
 * reason (so a G-stage entry can't be silently fed to an S-stage-typed
 * accessor or vice versa), not because any bit differs.
 *
 * ## Field layout
 *
 * - Bit 0: V (valid)
 * - Bit 1: R (readable)
 * - Bit 2: W (writable)
 * - Bit 3: X (executable)
 * - Bit 4: U (user-mode accessible)
 * - Bit 5: G (global mapping)
 * - Bit 6: A (accessed)
 * - Bit 7: D (dirty)
 * - Bits [9:8]: RSW (reserved for supervisor/software use)
 * - Bits [53:10] (Sv39/Sv48/Sv57): PPN, the output physical page number
 *
 * `R`/`W`/`X` all being `0` means this is a pointer to the next-level
 * table (non-leaf); any of them being `1` means this is a leaf -- a
 * "leaf" and a "block" are not architecturally distinct concepts in
 * RISC-V the way they are in ARM64/x86 (see `page_table_traits.hpp`'s
 * file doc: every RISC-V level permits an early leaf), so `is_leaf()`
 * may be called at any level without the final-level caveat ARM64/x86
 * need.
 *
 * ## Example
 *
 * @code
 * using namespace structo::arch;
 * using traits = page_table_entry_traits<riscv::pte_tag>;
 * auto leaf = traits::make_leaf_entry(traits::phys_type{0x8020'0000}, true, true, true); // r, w, x
 * bool present = traits::is_present(leaf);
 * @endcode
 */

#include <structo/arch/page_table_traits.hpp>
#include <structo/arch/pte_field.hpp>
#include <structo/phys_addr.hpp>

#include <cstdint>

namespace structo::arch::riscv {

/** @brief Tag: S-stage (ordinary, `satp`-rooted) translation. */
struct pte_tag {};

/** @brief Tag: G-stage (hypervisor staging, `hgatp`-rooted) translation. */
struct pte_g_stage_tag {};

namespace detail {

struct pte_bits {
  using valid = pte_bit_field<0, 1>;
  using read = pte_bit_field<1, 1>;
  using write = pte_bit_field<2, 1>;
  using exec = pte_bit_field<3, 1>;
  using user = pte_bit_field<4, 1>;
  using global = pte_bit_field<5, 1>;
  using accessed = pte_bit_field<6, 1>;
  using dirty = pte_bit_field<7, 1>;
  using rsw = pte_bit_field<8, 2>;
  using ppn = pte_bit_field<10, 44>; // wide enough to cover Sv39/Sv48/Sv57's PPN field

  [[nodiscard]] static constexpr bool is_present(std::uint64_t raw) noexcept { return valid::test(raw); }
  [[nodiscard]] static constexpr bool is_leaf(std::uint64_t raw) noexcept {
    return (read::test(raw) || write::test(raw) || exec::test(raw));
  }
};

template <typename Tag, typename PhysSpaceTag> struct pte_traits_impl {
  using entry_type = page_table_entry<Tag>;
  using phys_type = phys_addr<void, PhysSpaceTag>;
  using bits = pte_bits;

  [[nodiscard]] static constexpr bool is_present(entry_type e) noexcept { return bits::is_present(e.value); }
  [[nodiscard]] static constexpr bool is_leaf(entry_type e) noexcept { return bits::is_leaf(e.value); }

  [[nodiscard]] static constexpr bool readable(entry_type e) noexcept { return bits::read::test(e.value); }
  [[nodiscard]] static constexpr bool writable(entry_type e) noexcept { return bits::write::test(e.value); }
  [[nodiscard]] static constexpr bool executable(entry_type e) noexcept { return bits::exec::test(e.value); }
  [[nodiscard]] static constexpr bool user_accessible(entry_type e) noexcept { return bits::user::test(e.value); }
  [[nodiscard]] static constexpr bool global_mapping(entry_type e) noexcept { return bits::global::test(e.value); }
  [[nodiscard]] static constexpr bool accessed(entry_type e) noexcept { return bits::accessed::test(e.value); }
  [[nodiscard]] static constexpr bool dirty(entry_type e) noexcept { return bits::dirty::test(e.value); }
  [[nodiscard]] static constexpr unsigned rsw(entry_type e) noexcept {
    return static_cast<unsigned>(bits::rsw::get(e.value));
  }

  [[nodiscard]] static constexpr phys_type child_table_addr(entry_type e) noexcept {
    return phys_type{bits::ppn::get(e.value) << 12};
  }
  [[nodiscard]] static constexpr phys_type leaf_frame_addr(entry_type e) noexcept { return child_table_addr(e); }

  [[nodiscard]] static constexpr entry_type make_table_entry(phys_type child_table) noexcept {
    std::uint64_t raw = bits::valid::set(0, 1);
    raw = bits::ppn::set(raw, child_table.value >> 12);
    return entry_type{raw};
  }

  [[nodiscard]] static constexpr entry_type make_leaf_entry(phys_type frame, bool readable, bool writable,
                                                             bool executable, bool user_accessible = false,
                                                             bool global_mapping = false, bool accessed = true,
                                                             bool dirty = false, unsigned rsw = 0) noexcept {
    std::uint64_t raw = bits::valid::set(0, 1);
    raw = bits::read::set_bit(raw, readable);
    raw = bits::write::set_bit(raw, writable);
    raw = bits::exec::set_bit(raw, executable);
    raw = bits::user::set_bit(raw, user_accessible);
    raw = bits::global::set_bit(raw, global_mapping);
    raw = bits::accessed::set_bit(raw, accessed);
    raw = bits::dirty::set_bit(raw, dirty);
    raw = bits::rsw::set(raw, rsw);
    raw = bits::ppn::set(raw, frame.value >> 12);
    return entry_type{raw};
  }
};

} // namespace detail

} // namespace structo::arch::riscv

namespace structo::arch {

template <> struct page_table_entry_traits<riscv::pte_tag> : riscv::detail::pte_traits_impl<riscv::pte_tag, default_phys_space> {};

template <>
struct page_table_entry_traits<riscv::pte_g_stage_tag>
    : riscv::detail::pte_traits_impl<riscv::pte_g_stage_tag, host_phys_space> {};

} // namespace structo::arch
