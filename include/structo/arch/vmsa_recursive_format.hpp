// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file vmsa_recursive_format.hpp
 * @brief `vmsa_recursive_format<...>`: stage-1 last-level (page descriptor) format shared by AArch64
 * (4/16/64 KiB granules) and ARMv7 LPAE, for `recursive_remapper`. Use the aliases in
 * `arm64/recursive_format.hpp` and `arm/recursive_format.hpp`.
 *
 * Stage 2 is not covered: its tables are walked in the IPA space, which the CPU cannot use to reach them.
 *
 * ## Regimes (`vmsa_regime`)
 *
 *  - `el1_el0`: AArch64 EL1&0 / EL2&0. AP[2:1], PXN, UXN.
 *  - `flat`: AArch64 EL2 without VHE / EL3. One XN bit (54), AP[1] is RES1, no user access (`invalid_argument`).
 *  - `lpae`: ARMv7 LPAE. AP[2:1], PXN, and a single XN (bit 54) that blocks both privilege levels.
 *
 * ## Protection legalization
 *
 *  - kernel read is always implied; no user access: kernel RW -> AP=00, kernel RO -> AP=10;
 *  - user read-only -> AP=11 (kernel RO too: "kernel RW + user RO" does not exist);
 *  - user write -> AP=01 (kernel RW); downgraded to user read when the kernel is read-only;
 *  - user-visible pages are non-global (nG=1); otherwise nG = !global;
 *  - execute: PXN = !kexec, UXN = !uexec. `lpae`: XN = !(user_visible ? uexec : kexec), so on a user page
 *    the kernel can only execute what user may execute (kernel-only execute is dropped);
 *  - `Policy` (`mmu_policy<Wxn, Uwxn>`) is applied first, so executable bits that SCTLR.WXN/UWXN would ignore
 *    are reported as cleared;
 *  - AF is always set and writable pages are born dirty (AP[2]=0, DBM=0): no Access/Dirty faults and no
 *    hardware A/D updates for these mappings, with or without TCR.HA/HD (kernels set AF/D on their own mappings);
 *  - shareability: inner for normal memory, outer for device;
 *  - memory type through `Mair` (default `default_mair`; program `MAIR_ELx` with `default_mair::value`; build custom ones with `mair.hpp`);
 *    `write_combining` and `uncached` both map to Normal Non-cacheable;
 *  - for secure tags, NS follows `security_state`; `secure` on a non-secure tag is `unsupported_operation`.
 *
 * @code
 * // AArch64, 16 KiB granule, SCTLR.WXN=1, non-secure: 2048 entries per last-level table.
 * using format = structo::arch::arm64::recursive_stage1_format<
 *     structo::page_16k, structo::arch::arm64::stage1_ns_tag<structo::page_16k>,
 *     structo::arch::vmsa_regime::el1_el0, structo::arch::mmu_policy<true>>;
 * @endcode
 */

#include <structo/arch/mair.hpp>
#include <structo/arch/page_table_traits.hpp>
#include <structo/arch/protection.hpp>
#include <structo/arch/vmsa_pte_fields.hpp>
#include <structo/phys_page.hpp>

#include <reloco/error.hpp>
#include <reloco/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace structo::arch {

enum class vmsa_regime : std::uint8_t { el1_el0, flat, lpae };

template <typename Granule, typename Tag, vmsa_regime Regime = vmsa_regime::el1_el0,
          typename Policy = default_mmu_policy, typename Mair = default_mair>
struct vmsa_recursive_format {
  using word = std::uint64_t;
  using phys_type = typename page_table_entry_traits<Tag>::phys_type;

  static constexpr std::uint64_t page_size = Granule::page_size;
  static constexpr std::size_t entry_count = Granule::page_size / 8;
  static constexpr bool flush_on_map = false;

  [[nodiscard]] static constexpr bool is_present(word raw) noexcept { return bits::valid::test(raw); }
  [[nodiscard]] static constexpr phys_type frame_addr(word raw) noexcept {
    return phys_type{bits::template output_address<Granule::page_shift>(raw)};
  }

  [[nodiscard]] static reloco::result<word> make_leaf(phys_type frame, protection req) noexcept {
    if (req.is_none()) {
      return reloco::unexpected(reloco::error::invalid_argument);
    }
    const protection p = req.template enforce_policy<Policy>();
    if (!secure_world && p.security() == security_state::secure) {
      return reloco::unexpected(reloco::error::unsupported_operation);
    }
    if (Regime == vmsa_regime::flat && p.is_user_visible()) {
      return reloco::unexpected(reloco::error::invalid_argument);
    }

    std::uint64_t raw = bits::valid::set(0, 1);
    raw = bits::table_or_page::set(raw, 1); // page descriptor at the last level
    raw = bits::attr_indx::set(raw, Mair::index_of(p.cache()));
    raw = bits::sh::set(raw, p.is_device() ? 0b10 : 0b11);
    raw = bits::af::set(raw, 1);
    if constexpr (secure_world) {
      raw = bits::ns::set_bit(raw, p.security() == security_state::non_secure);
    }

    if constexpr (Regime == vmsa_regime::flat) {
      raw = bits::ap::set(raw, p.kernel_write() ? 0b01 : 0b11);
      raw = bits::uxn::set_bit(raw, !p.kernel_exec());
    } else {
      const bool user = p.is_user_visible();
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
      if constexpr (Regime == vmsa_regime::lpae) {
        raw = bits::uxn::set_bit(raw, !(user ? p.user_exec() : p.kernel_exec()));
      } else {
        raw = bits::uxn::set_bit(raw, !p.user_exec());
      }
      raw = bits::ng::set_bit(raw, user || !p.is_global());
    }
    return bits::template with_output_address<Granule::page_shift>(raw, frame.value);
  }

  [[nodiscard]] static protection attrs(word raw) noexcept {
    protection p;
    const std::uint64_t ap = bits::ap::get(raw);
    if constexpr (Regime == vmsa_regime::flat) {
      const bool ro = (ap & 0b10) != 0;
      const bool x = !bits::uxn::test(raw);
      p = p.with_kernel(static_cast<kprot>((ro ? 1 : 2) | (x ? 4 : 0)));
    } else {
      const bool kwrite = (ap == 0b00 || ap == 0b01);
      const bool xn = Regime == vmsa_regime::lpae && bits::uxn::test(raw);
      const bool kexec = !bits::pxn::test(raw) && !xn;
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
    return p;
  }

  /** Break-before-make is required unless only AP, PXN/UXN or AF change. */
  [[nodiscard]] static constexpr bool needs_bbm(word old_raw, word new_raw) noexcept {
    constexpr std::uint64_t relaxed =
        (std::uint64_t{3} << 6) | (std::uint64_t{1} << 10) | (std::uint64_t{1} << 53) | (std::uint64_t{1} << 54);
    return ((old_raw ^ new_raw) & ~relaxed) != 0;
  }

private:
  using bits = detail::vmsa::stage1_bits;
  static constexpr bool secure_world = !std::is_same_v<phys_type, phys_addr<void, nonsecure_phys_space>>;
};

} // namespace structo::arch
