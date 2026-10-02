// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file pte_stage2.hpp
 * @brief `structo::arch::page_table_entry_traits<Tag>` specialization
 * for ARMv7-A LPAE stage-2 ("hypervisor staging") table/block/page
 * descriptors -- the Virtualization Extensions' `HTTBR`/`VTTBR`-rooted
 * walk translating a guest's intermediate physical address (IPA) to a
 * real physical address.
 *
 * Only one tag is provided here (`stage2_tag`): ARMv7's Virtualization
 * Extensions have no Secure-world hypervisor mode (no ARMv7 equivalent
 * of AArch64's "sEL2"), so there is no secure stage-2 counterpart to
 * `arm64::stage2_secure_tag`.
 */

#include <structo/arch/page_table_traits.hpp>
#include <structo/arch/vmsa_pte_fields.hpp>
#include <structo/phys_addr.hpp>
#include <structo/phys_page.hpp>

namespace structo::arch::arm::lpae {

/** @brief Tag: stage-2 ("hypervisor staging") translation regime. */
struct stage2_tag {};

} // namespace structo::arch::arm::lpae

namespace structo::arch {

template <> struct page_table_entry_traits<arm::lpae::stage2_tag> {
  using entry_type = page_table_entry<arm::lpae::stage2_tag>;
  using phys_type = phys_addr<void, host_phys_space>;
  using bits = structo::arch::detail::vmsa::stage2_bits;

  [[nodiscard]] static constexpr bool is_present(entry_type e) noexcept { return bits::is_present(e.value); }
  [[nodiscard]] static constexpr bool is_leaf(entry_type e) noexcept { return !bits::is_table(e.value); }

  [[nodiscard]] static constexpr unsigned mem_attr(entry_type e) noexcept {
    return static_cast<unsigned>(bits::mem_attr::get(e.value));
  }
  [[nodiscard]] static constexpr unsigned s2ap(entry_type e) noexcept {
    return static_cast<unsigned>(bits::s2ap::get(e.value));
  }
  [[nodiscard]] static constexpr bool readable(entry_type e) noexcept { return (s2ap(e) & 0b01u) != 0; }
  [[nodiscard]] static constexpr bool writable(entry_type e) noexcept { return (s2ap(e) & 0b10u) != 0; }
  [[nodiscard]] static constexpr unsigned sh(entry_type e) noexcept {
    return static_cast<unsigned>(bits::sh::get(e.value));
  }
  [[nodiscard]] static constexpr bool af(entry_type e) noexcept { return bits::af::test(e.value); }
  [[nodiscard]] static constexpr bool xn(entry_type e) noexcept { return bits::xn::test(e.value); }

  [[nodiscard]] static constexpr phys_type child_table_addr(entry_type e) noexcept {
    return phys_type{bits::output_address<12>(e.value)};
  }
  [[nodiscard]] static constexpr phys_type leaf_frame_addr(entry_type e) noexcept { return child_table_addr(e); }

  [[nodiscard]] static constexpr entry_type make_table_entry(phys_type child_table) noexcept {
    std::uint64_t raw = bits::valid::set(0, 1);
    raw = bits::page_or_block::set(raw, 1);
    raw = bits::with_output_address<12>(raw, child_table.value);
    return entry_type{raw};
  }

  /** @brief Builds a leaf entry; `final_level = false` builds a block descriptor instead of a page descriptor. */
  [[nodiscard]] static constexpr entry_type make_leaf_entry(phys_type frame, unsigned s2ap, unsigned sh,
                                                            unsigned mem_attr, bool final_level = true, bool af = true,
                                                            bool xn = false) noexcept {
    std::uint64_t raw = bits::valid::set(0, 1);
    raw = bits::page_or_block::set(raw, final_level ? 1 : 0);
    raw = bits::s2ap::set(raw, s2ap);
    raw = bits::sh::set(raw, sh);
    raw = bits::mem_attr::set(raw, mem_attr);
    raw = bits::af::set_bit(raw, af);
    raw = bits::xn::set_bit(raw, xn);
    raw = bits::with_output_address<12>(raw, frame.value);
    return entry_type{raw};
  }
};

} // namespace structo::arch
