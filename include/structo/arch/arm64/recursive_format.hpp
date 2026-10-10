// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file arm64/recursive_format.hpp
 * @brief `arm64::recursive_stage1_format<...>`: AArch64 stage-1, 4 KiB granule format for `recursive_remapper`.
 *
 * ## Why the recursive trick works on AArch64
 *
 * A table descriptor ignores bits [58:2] and a page descriptor (last level) is "valid + bit 1".
 * `make_table` therefore also fills in the *page view*: AF=1, normal write-back memory, inner
 * shareable, kernel-only RW, never executable. The same 64-bit value is a table when walked
 * at an upper level and a page when the window walk reaches it at the last level.
 *
 * ## Protection legalization
 *
 * Regime `Flat = false` (EL1&0 or EL2&0, AP[2:1], PXN and UXN):
 *  - `kernel` read is always implied; PXN = !kexec, UXN = !uexec;
 *  - no user access: kernel RW -> AP=00, kernel RO -> AP=10;
 *  - user read-only: AP=11 (kernel becomes RO too: "kernel RW + user RO" does not exist);
 *  - user write: AP=01 (kernel RW). If the kernel is requested read-only, user write is
 *    downgraded to read (never more permissive than asked);
 *  - user-visible pages are forced non-global (nG=1);
 *  - the `Policy` (`mmu_policy<Wxn, Uwxn>`) is applied first, so executable bits that
 *    SCTLR.WXN/UWXN would ignore are reported as cleared.
 *
 * Regime `Flat = true` (EL2 without VHE, EL3): one XN bit (bit 54), AP[1] is RES1,
 * no unprivileged access. User access in the request is `invalid_argument`.
 *
 * Common: AF=1 (no hardware flag management needed), shareability inner (normal) / outer
 * (device), memory type through the MAIR layout `Mair` (default: `default_mair`, program
 * `MAIR_ELx` with `default_mair::value`). `write_combining` and `uncached` both map to
 * Normal Non-cacheable. For `stage1_secure_tag`, the NS bit follows `security_state`.
 * `HighHalf = true` selects the TTBR1 VA prefix (all ones above `va_bits`).
 *
 * @code
 * // TTBR1 kernel half, 48-bit VA, 4 KiB granule, SCTLR.WXN=1, non-secure EL1.
 * using format = structo::arch::arm64::recursive_stage1_format<
 *     structo::arch::arm64::levels_4k_48bit,                  // 4-level walk, 4 KiB granule
 *     structo::arch::arm64::stage1_ns_tag<>,                  // non-secure output addresses
 *     true,                                                   // HighHalf: VAs start with 0xFFFF
 *     false,                                                  // Flat: EL1&0 style AP/PXN/UXN
 *     structo::arch::mmu_policy<true>>;                       // WXN enabled
 * @endcode
 */

#include <structo/arch/arm64/page_table_traits.hpp>
#include <structo/arch/arm64/pte_stage1.hpp>
#include <structo/arch/page_table_traits.hpp>
#include <structo/arch/protection.hpp>
#include <structo/arch/vmsa_pte_fields.hpp>

#include <reloco/error.hpp>
#include <reloco/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace structo::arch::arm64 {

/** 4 KiB granule, 48-bit VA, four levels (L0 table-only, L1 1 GiB, L2 2 MiB, L3 4 KiB). */
using levels_4k_48bit =
    page_table_levels<structo::page_4k, 48, page_table_level<9, 39, false>, page_table_level<9, 30, true>,
                      page_table_level<9, 21, true>, page_table_level<9, 12, true>>;

/** MAIR layout that `recursive_stage1_format` assumes by default. Program `MAIR_ELx` with `value`. */
struct default_mair {
  static constexpr std::uint64_t value = 0x00ull | (0x04ull << 8) | (0x44ull << 16) | (0xbbull << 24) | (0xffull << 32);

  /** Attribute index for a memory type. */
  [[nodiscard]] static constexpr std::uint64_t index_of(cache_mode c) noexcept {
    switch (c) {
    case cache_mode::device_ordered:
      return 0; // Device-nGnRnE
    case cache_mode::device:
      return 1; // Device-nGnRE
    case cache_mode::uncached:
    case cache_mode::write_combining:
      return 2; // Normal Non-cacheable
    case cache_mode::write_through:
      return 3; // Normal write-through
    case cache_mode::write_back:
      break;
    }
    return 4; // Normal write-back
  }

  [[nodiscard]] static constexpr cache_mode cache_of(std::uint64_t index) noexcept {
    switch (index) {
    case 0:
      return cache_mode::device_ordered;
    case 1:
      return cache_mode::device;
    case 2:
      return cache_mode::uncached;
    case 3:
      return cache_mode::write_through;
    default:
      return cache_mode::write_back;
    }
  }
};

template <typename Levels = levels_4k_48bit, typename Tag = stage1_ns_tag<structo::page_4k>, bool HighHalf = false,
          bool Flat = false, typename Policy = default_mmu_policy, typename Mair = default_mair>
struct recursive_stage1_format {
  using levels = Levels;
  using phys_type = typename page_table_entry_traits<Tag>::phys_type;

  static_assert(Levels::leaf_page_traits::page_shift == 12, "only the 4 KiB granule is supported");
  static constexpr bool secure_world = !std::is_same_v<phys_type, phys_addr<void, nonsecure_phys_space>>;

  [[nodiscard]] static constexpr bool is_present(std::uint64_t raw) noexcept { return bits::valid::test(raw); }
  [[nodiscard]] static constexpr bool is_leaf(std::uint64_t raw, std::size_t) noexcept {
    return !bits::table_or_page::test(raw);
  }
  [[nodiscard]] static constexpr phys_type table_addr(std::uint64_t raw) noexcept {
    return phys_type{bits::template output_address<12>(raw)};
  }
  [[nodiscard]] static constexpr phys_type frame_addr(std::uint64_t raw, std::size_t) noexcept {
    return table_addr(raw);
  }

  /** Table descriptor that is simultaneously a valid kernel-only, non-executable write-back page descriptor. */
  [[nodiscard]] static constexpr std::uint64_t make_table(phys_type child) noexcept {
    std::uint64_t raw = bits::valid::set(0, 1);
    raw = bits::table_or_page::set(raw, 1);
    raw = bits::attr_indx::set(raw, Mair::index_of(cache_mode::write_back));
    raw = bits::ap::set(raw, Flat ? 1 : 0);
    raw = bits::sh::set(raw, 0b11);
    raw = bits::af::set(raw, 1);
    raw = bits::uxn::set(raw, 1);
    if constexpr (!Flat) {
      raw = bits::pxn::set(raw, 1);
    }
    return bits::template with_output_address<12>(raw, child.value);
  }

  [[nodiscard]] static constexpr std::uint64_t make_self(phys_type root) noexcept { return make_table(root); }

  [[nodiscard]] static reloco::result<std::uint64_t> make_leaf(phys_type frame, protection req,
                                                               std::size_t level) noexcept {
    const protection p = req.template enforce_policy<Policy>();
    if (req.is_none()) {
      return reloco::unexpected(reloco::error::invalid_argument);
    }
    if (!secure_world && p.security() == security_state::secure) {
      return reloco::unexpected(reloco::error::unsupported_operation);
    }
    if (Flat && p.is_user_visible()) {
      return reloco::unexpected(reloco::error::invalid_argument);
    }

    std::uint64_t raw = bits::valid::set(0, 1);
    raw = bits::table_or_page::set_bit(raw, level == Levels::level_count - 1);
    raw = bits::attr_indx::set(raw, Mair::index_of(p.cache()));
    raw = bits::sh::set(raw, p.is_device() ? 0b10 : 0b11);
    raw = bits::af::set(raw, 1);
    if constexpr (secure_world) {
      raw = bits::ns::set_bit(raw, p.security() == security_state::non_secure);
    }

    if constexpr (Flat) {
      raw = bits::ap::set(raw, p.kernel_write() ? 0b01 : 0b11);
      raw = bits::uxn::set_bit(raw, !p.kernel_exec());
    } else {
      bool user = p.is_user_visible();
      std::uint64_t ap = 0;
      if (!user) {
        ap = p.kernel_write() ? 0b00 : 0b10;
      } else if (p.user_write() && (!p.kernel_read() || p.kernel_write())) {
        ap = 0b01;
      } else {
        ap = 0b11;
      }
      raw = bits::ap::set(raw, ap);
      raw = bits::pxn::set_bit(raw, !p.kernel_exec());
      raw = bits::uxn::set_bit(raw, !p.user_exec());
      raw = bits::ng::set_bit(raw, user || !p.is_global());
    }
    return bits::template with_output_address<12>(raw, frame.value);
  }

  /** Effective protection of a leaf. */
  [[nodiscard]] static constexpr protection attrs(std::uint64_t raw, std::size_t) noexcept {
    protection p;
    const std::uint64_t ap = bits::ap::get(raw);
    if constexpr (Flat) {
      p = p.with_kernel((ap & 0b10) ? kprot::read : kprot::write);
      if (!bits::uxn::test(raw)) {
        p = p.with_kernel((ap & 0b10) ? kprot::read_exec : kprot::write_exec);
      }
    } else {
      const bool kwrite = (ap == 0b00 || ap == 0b01);
      const bool kexec = !bits::pxn::test(raw);
      p = p.with_kernel(static_cast<kprot>((kwrite ? 2 : 1) | (kexec ? 4 : 0)));
      if (ap == 0b01 || ap == 0b11) {
        p = p.with_user(static_cast<uprot>((ap == 0b01 ? 2 : 1) | (bits::uxn::test(raw) ? 0 : 4)));
      }
      p = p.with_scope(bits::ng::test(raw) ? scope::per_address_space : scope::global);
    }
    p = p.with_cache(Mair::cache_of(bits::attr_indx::get(raw)));
    if constexpr (secure_world) {
      p = p.with_security(bits::ns::test(raw) ? security_state::non_secure : security_state::secure);
    }
    return p.template enforce_policy<Policy>();
  }

  /** Break-before-make is required unless only AP, PXN/UXN or AF change. */
  [[nodiscard]] static constexpr bool needs_bbm(std::uint64_t old_raw, std::uint64_t new_raw) noexcept {
    constexpr std::uint64_t relaxed =
        (std::uint64_t{3} << 6) | (std::uint64_t{1} << 10) | (std::uint64_t{1} << 53) | (std::uint64_t{1} << 54);
    return ((old_raw ^ new_raw) & ~relaxed) != 0;
  }

  /** TTBR0 half: zero-extended; TTBR1 half (`HighHalf`): ones above `va_bits`. */
  [[nodiscard]] static constexpr std::uint64_t canonicalize(std::uint64_t low) noexcept {
    constexpr std::uint64_t mask = (std::uint64_t{1} << Levels::va_bits) - 1;
    return HighHalf ? ((low & mask) | ~mask) : (low & mask);
  }

private:
  using bits = structo::arch::detail::vmsa::stage1_bits;
};

} // namespace structo::arch::arm64
