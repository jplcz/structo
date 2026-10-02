// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file pte_ept.hpp
 * @brief `structo::arch::page_table_entry_traits<Tag>` specialization
 * for Intel VT-x's Extended Page Table (EPT) hypervisor-staging format
 * -- a genuinely different bit layout from ordinary x86 paging (see
 * `pte.hpp`), unlike AMD's NPT which reuses the ordinary format
 * verbatim. This is exactly the "split into a file per logical mode"
 * case: EPT earns its own file because its encoding, not just its root
 * register, differs from `x86::pte_tag`.
 *
 * ## No dedicated "present" bit
 *
 * EPT has no `P` bit at all: an entry with `R == W == X == 0` is
 * (treated as) not present and causes an EPT violation on access, so
 * `is_present()` here is derived from those three bits rather than a
 * standalone flag -- this is different from every other format in this
 * library, which all have an explicit present/valid bit.
 *
 * ## Field layout
 *
 * - Bit 0: R (read), Bit 1: W (write), Bit 2: X (supervisor-mode execute)
 * - Bits [5:3]: EPT memory type (leaf only)
 * - Bit 6: Ignore PAT (leaf only)
 * - Bit 7: PS (huge-page leaf, intermediate levels only -- see `pte.hpp`'s
 *   PS/PAT-position caveat; EPT has the same dual-meaning-by-level
 *   wrinkle, except the final level's bit 7 is simply unused rather than
 *   repurposed)
 * - Bit 8: Accessed, Bit 9: Dirty (leaf only) -- both require EPT
 *   accessed/dirty-flags support to be enabled in `EPTP`
 * - Bits [51:12]: output address (next-level table, or leaf frame)
 * - Bit 63: Suppress `#VE` (requires `#VE` support to be enabled)
 *
 * Mode-based execute control (a separate user-mode-execute bit, bit 10)
 * is not modeled here -- out of scope for the same reason RME/FEAT_SEL2
 * nuances are elsewhere in this library: it's an optional extension a
 * caller needing it should read/write directly via `page_table_entry<Tag>::value`.
 */

#include <structo/arch/page_table_traits.hpp>
#include <structo/arch/pte_field.hpp>
#include <structo/phys_addr.hpp>

#include <cstdint>

namespace structo::arch::x86 {

/** @brief Tag: Intel EPT hypervisor-staging translation (`EPTP`-rooted). */
struct ept_tag {};

namespace detail {

struct ept_bits {
  using read = pte_bit_field<0, 1>;
  using write = pte_bit_field<1, 1>;
  using exec = pte_bit_field<2, 1>;
  using mem_type = pte_bit_field<3, 3>;
  using ignore_pat = pte_bit_field<6, 1>;
  using ps = pte_bit_field<7, 1>;
  using accessed = pte_bit_field<8, 1>;
  using dirty = pte_bit_field<9, 1>;
  using addr = pte_bit_field<12, 40>; // bits [51:12]
  using suppress_ve = pte_bit_field<63, 1>;
};

} // namespace detail

} // namespace structo::arch::x86

namespace structo::arch {

template <> struct page_table_entry_traits<x86::ept_tag> {
  using entry_type = page_table_entry<x86::ept_tag>;
  using phys_type = phys_addr<void, host_phys_space>;
  using bits = x86::detail::ept_bits;

  [[nodiscard]] static constexpr bool is_present(entry_type e) noexcept {
    return bits::read::test(e.value) || bits::write::test(e.value) || bits::exec::test(e.value);
  }
  /** @brief `true` for a huge-page leaf. Only meaningful at an intermediate, huge-page-capable level. */
  [[nodiscard]] static constexpr bool is_leaf(entry_type e) noexcept { return bits::ps::test(e.value); }

  [[nodiscard]] static constexpr bool readable(entry_type e) noexcept { return bits::read::test(e.value); }
  [[nodiscard]] static constexpr bool writable(entry_type e) noexcept { return bits::write::test(e.value); }
  [[nodiscard]] static constexpr bool executable(entry_type e) noexcept { return bits::exec::test(e.value); }
  [[nodiscard]] static constexpr unsigned mem_type(entry_type e) noexcept {
    return static_cast<unsigned>(bits::mem_type::get(e.value));
  }
  [[nodiscard]] static constexpr bool ignore_pat(entry_type e) noexcept { return bits::ignore_pat::test(e.value); }
  [[nodiscard]] static constexpr bool accessed(entry_type e) noexcept { return bits::accessed::test(e.value); }
  [[nodiscard]] static constexpr bool dirty(entry_type e) noexcept { return bits::dirty::test(e.value); }
  [[nodiscard]] static constexpr bool suppress_ve(entry_type e) noexcept { return bits::suppress_ve::test(e.value); }

  [[nodiscard]] static constexpr phys_type child_table_addr(entry_type e) noexcept {
    return phys_type{bits::addr::get(e.value) << 12};
  }
  [[nodiscard]] static constexpr phys_type leaf_frame_addr(entry_type e) noexcept { return child_table_addr(e); }

  [[nodiscard]] static constexpr entry_type make_table_entry(phys_type child_table, bool writable = true,
                                                             bool executable = true) noexcept {
    std::uint64_t raw = bits::read::set_bit(0, true);
    raw = bits::write::set_bit(raw, writable);
    raw = bits::exec::set_bit(raw, executable);
    raw = bits::addr::set(raw, child_table.value >> 12);
    return entry_type{raw};
  }

  /** @brief Builds a leaf entry; `final_level = false` sets `PS` for an intermediate-level huge-page leaf. */
  [[nodiscard]] static constexpr entry_type make_leaf_entry(phys_type frame, bool readable, bool writable,
                                                            bool executable, unsigned mem_type, bool final_level = true,
                                                            bool ignore_pat = false, bool accessed = true,
                                                            bool dirty = false, bool suppress_ve = false) noexcept {
    std::uint64_t raw = bits::read::set_bit(0, readable);
    raw = bits::write::set_bit(raw, writable);
    raw = bits::exec::set_bit(raw, executable);
    raw = bits::mem_type::set(raw, mem_type);
    raw = bits::ignore_pat::set_bit(raw, ignore_pat);
    raw = bits::ps::set_bit(raw, !final_level);
    raw = bits::accessed::set_bit(raw, accessed);
    raw = bits::dirty::set_bit(raw, dirty);
    raw = bits::suppress_ve::set_bit(raw, suppress_ve);
    raw = bits::addr::set(raw, frame.value >> 12);
    return entry_type{raw};
  }
};

} // namespace structo::arch
