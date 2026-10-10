// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file arm/recursive_format.hpp
 * @brief ARMv7 last-level formats for `recursive_remapper`.
 *
 * - `arm::lpae::recursive_stage1_format<Tag, Policy, Mair>`: LPAE, 64-bit entries, 512 per table, 4 KiB pages
 *   (see `vmsa_recursive_format.hpp`; single XN bit + PXN).
 * - `arm::short_descriptor::recursive_small_page_format<Tag>`: short-descriptor small pages, 32-bit entries,
 *   256 per coarse table (1 KiB). The caller must own the **whole 4 KiB page** containing the table (the table
 *   at offset 0, page aligned): the self entry maps that page, so the remaining 3 KiB are visible in the window.
 *
 * ## Short-descriptor legalization
 *
 *  - AP/APX: no user access -> kernel RW (APX=0, AP=01) or kernel RO (APX=1, AP=01); user read-only ->
 *    kernel RW (APX=0, AP=10) or RO (APX=1, AP=11); user write -> AP=11 (needs kernel write, else downgraded
 *    to read-only for both);
 *  - XN is shared by both privilege levels and small pages have no PXN: execute = `kexec` on kernel-only pages
 *    and `uexec` on user pages (kernel-only execute of a user page is dropped, and a user-executable page is
 *    also kernel-executable: the hardware cannot prevent it — **documented exception**);
 *  - memory types use the TEX remap-off encodings: write-back (TEX=001,C=1,B=1), write-through (000,1,0),
 *    non-cacheable (001,0,0), device (000,0,1), strongly-ordered (000,0,0); S=1 (shareable) for normal memory;
 *  - nG = user-visible || !global; there is no access flag (needs SCTLR.AFE=0); physical addresses < 4 GiB;
 *  - `Policy` WXN/UWXN (SCTLR.WXN) is applied as in the other ARM formats.
 */

#include <structo/arch/arm/pte_stage1.hpp>
#include <structo/arch/protection.hpp>
#include <structo/arch/pte_field.hpp>
#include <structo/arch/vmsa_recursive_format.hpp>

#include <reloco/error.hpp>
#include <reloco/expected.hpp>

#include <cstddef>
#include <cstdint>

namespace structo::arch::arm::lpae {

template <typename Tag = stage1_ns_tag, typename Policy = default_mmu_policy, typename Mair = default_mair>
using recursive_stage1_format = vmsa_recursive_format<structo::page_4k, Tag, vmsa_regime::lpae, Policy, Mair>;

} // namespace structo::arch::arm::lpae

namespace structo::arch::arm::short_descriptor {

namespace detail {
// Small-page (L2) descriptor fields (ARMv7 short-descriptor format).
struct small_page_bits {
  using present = pte_bit_field<1, 1, std::uint32_t>; // bit 1 fixed to 1; bit 0 is XN
  using xn = pte_bit_field<0, 1, std::uint32_t>;
  using b = pte_bit_field<2, 1, std::uint32_t>;
  using c = pte_bit_field<3, 1, std::uint32_t>;
  using ap01 = pte_bit_field<4, 2, std::uint32_t>;
  using tex = pte_bit_field<6, 3, std::uint32_t>;
  using apx = pte_bit_field<9, 1, std::uint32_t>;
  using s = pte_bit_field<10, 1, std::uint32_t>;
  using ng = pte_bit_field<11, 1, std::uint32_t>;
  using addr = pte_bit_field<12, 20, std::uint32_t>;
};
} // namespace detail

template <typename Policy = default_mmu_policy> struct recursive_small_page_format {
  using word = std::uint32_t;
  using phys_type = phys_addr<void, default_phys_space>;
  static constexpr std::size_t entry_count = 256;
  static constexpr std::uint64_t page_size = 4096;
  static constexpr bool flush_on_map = false;

  [[nodiscard]] static constexpr bool is_present(word raw) noexcept { return bits::present::test(raw); }
  [[nodiscard]] static constexpr phys_type frame_addr(word raw) noexcept {
    return phys_type{static_cast<std::uint64_t>(bits::addr::get(raw)) << 12};
  }

  [[nodiscard]] static reloco::result<word> make_leaf(phys_type frame, protection req) noexcept {
    if (req.is_none()) {
      return reloco::unexpected(reloco::error::invalid_argument);
    }
    if (frame.value >> 32 != 0) {
      return reloco::unexpected(reloco::error::out_of_range);
    }
    const protection p = req.template enforce_policy<Policy>();
    if (p.security() == security_state::secure) {
      return reloco::unexpected(reloco::error::unsupported_operation);
    }
    const bool user = p.is_user_visible();
    const bool uwrite = user && p.user_write() && (!p.kernel_read() || p.kernel_write());
    std::uint32_t apx = 0;
    std::uint32_t ap = 0;
    if (!user) {
      apx = p.kernel_write() ? 0 : 1;
      ap = 0b01;
    } else if (uwrite) {
      ap = 0b11;
    } else {
      apx = p.kernel_write() ? 0 : 1;
      ap = apx ? 0b11 : 0b10;
    }
    std::uint32_t tex = 0;
    std::uint32_t c = 0;
    std::uint32_t b = 0;
    switch (p.cache()) {
    case cache_mode::write_back:
      tex = 1, c = 1, b = 1;
      break;
    case cache_mode::write_through:
      c = 1;
      break;
    case cache_mode::uncached:
    case cache_mode::write_combining:
      tex = 1;
      break;
    case cache_mode::device:
      b = 1;
      break;
    case cache_mode::device_ordered:
      break;
    }
    const bool exec = user ? p.user_exec() : p.kernel_exec();

    word raw = bits::present::set(0, 1);
    raw = bits::xn::set_bit(raw, !exec);
    raw = bits::b::set(raw, b);
    raw = bits::c::set(raw, c);
    raw = bits::ap01::set(raw, ap);
    raw = bits::tex::set(raw, tex);
    raw = bits::apx::set(raw, apx);
    raw = bits::s::set_bit(raw, !p.is_device());
    raw = bits::ng::set_bit(raw, user || !p.is_global());
    return bits::addr::set(raw, static_cast<std::uint32_t>(frame.value >> 12));
  }

  [[nodiscard]] static protection attrs(word raw) noexcept {
    const std::uint32_t ap = bits::ap01::get(raw);
    const bool apx = bits::apx::test(raw);
    const bool user = !(ap == 0b01 || ap == 0b00);
    const bool kwrite = !apx && ap != 0b00;
    const bool exec = !bits::xn::test(raw);
    protection p;
    p = p.with_kernel(static_cast<kprot>((kwrite ? 2 : 1) | (exec ? 4 : 0)));
    if (user) {
      const bool uw = !apx && ap == 0b11;
      p = p.with_user(static_cast<uprot>((uw ? 2 : 1) | (exec ? 4 : 0)));
    }
    p = p.with_scope(bits::ng::test(raw) ? scope::per_address_space : scope::global);
    const std::uint32_t tc = (bits::tex::get(raw) << 2) | (bits::c::get(raw) << 1) | bits::b::get(raw);
    switch (tc) {
    case 0b001'1'1:
      return p.with_cache(cache_mode::write_back);
    case 0b000'1'0:
      return p.with_cache(cache_mode::write_through);
    case 0b001'0'0:
      return p.with_cache(cache_mode::uncached);
    case 0b000'0'1:
      return p.with_cache(cache_mode::device);
    default:
      return p.with_cache(cache_mode::device_ordered);
    }
  }

  /** Break-before-make unless only AP/APX or XN change. */
  [[nodiscard]] static constexpr bool needs_bbm(word old_raw, word new_raw) noexcept {
    constexpr word relaxed = 1u | (3u << 4) | (1u << 9);
    return ((old_raw ^ new_raw) & ~relaxed) != 0;
  }

private:
  using bits = detail::small_page_bits;
};

} // namespace structo::arch::arm::short_descriptor
