// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file x86/recursive_format.hpp
 * @brief `x86::recursive_pte_format<Levels>`: x86-64 paging format for `recursive_remapper`.
 *
 * ## Protection legalization (x86 has one XD bit, one US bit, one RW bit)
 *
 * The request is turned into the nearest encoding that is never more permissive:
 *  - user-visible (`user != none`): US=1; writable only if user may write *and* the kernel
 *    may write (or has no stated access); executable follows `uexec`;
 *  - otherwise: US=0; RW follows `kwrite`, executable follows `kexec`;
 *  - read is always implied; XD is set whenever not executable (requires `EFER.NXE=1`);
 *  - accessed and dirty are always set (bootloader policy, avoids hardware A/D updates);
 *  - `scope::global` sets G (needs `CR4.PGE`);
 *  - memory types use the reset PAT: write-back, write-through, UC- (`uncached`), UC (`device*`);
 *    `write_combining` needs a custom PAT and fails with `unsupported_operation`;
 *  - `security_state::secure` is not an x86 concept (`unsupported_operation`).
 *
 * ## Self entry
 *
 * `make_self` is present+writable, **supervisor-only and XD**: since permissions are the AND
 * of every level, this makes the entire recursive window kernel-only and non-executable even
 * though ordinary table entries (US=1, RW=1, XD=0) allow user mappings beneath them.
 *
 * @code
 * // 4-level long mode (use long_mode_5level for LA57).
 * using format = structo::arch::x86::recursive_pte_format<structo::arch::x86::long_mode_4level>;
 * @endcode
 */

#include <structo/arch/page_table_traits.hpp>
#include <structo/arch/protection.hpp>
#include <structo/arch/pte_field.hpp>
#include <structo/arch/x86/page_table_traits.hpp>
#include <structo/arch/x86/pte.hpp>

#include <reloco/error.hpp>
#include <reloco/expected.hpp>

#include <cstddef>
#include <cstdint>

namespace structo::arch::x86 {

template <typename Levels = long_mode_4level> struct recursive_pte_format {
  using levels = Levels;
  using phys_type = typename page_table_entry_traits<pte_tag>::phys_type;

  static_assert(Levels::leaf_page_traits::page_shift == 12, "x86 pages are 4 KiB");

  [[nodiscard]] static constexpr bool is_present(std::uint64_t raw) noexcept { return bits::is_present(raw); }
  [[nodiscard]] static constexpr bool is_leaf(std::uint64_t raw, std::size_t) noexcept { return bits::is_leaf(raw); }
  [[nodiscard]] static constexpr phys_type table_addr(std::uint64_t raw) noexcept {
    return phys_type{bits::addr::get(raw) << 12};
  }
  [[nodiscard]] static constexpr phys_type frame_addr(std::uint64_t raw, std::size_t) noexcept {
    return table_addr(raw);
  }

  [[nodiscard]] static constexpr std::uint64_t make_table(phys_type child) noexcept {
    std::uint64_t raw = bits::present::set(0, 1);
    raw = bits::rw::set(raw, 1);
    raw = bits::us::set(raw, 1);
    return bits::addr::set(raw, child.value >> 12);
  }

  [[nodiscard]] static constexpr std::uint64_t make_self(phys_type root) noexcept {
    std::uint64_t raw = bits::present::set(0, 1);
    raw = bits::rw::set(raw, 1);
    raw = bits::xd::set(raw, 1);
    return bits::addr::set(raw, root.value >> 12);
  }

  [[nodiscard]] static reloco::result<std::uint64_t> make_leaf(phys_type frame, protection p,
                                                               std::size_t level) noexcept {
    if (p.is_none()) {
      return reloco::unexpected(reloco::error::invalid_argument);
    }
    if (p.security() == security_state::secure || p.cache() == cache_mode::write_combining) {
      return reloco::unexpected(reloco::error::unsupported_operation);
    }
    const bool user = p.is_user_visible();
    const bool write = user ? (p.user_write() && (!p.kernel_read() || p.kernel_write())) : p.kernel_write();
    const bool exec = user ? p.user_exec() : p.kernel_exec();

    std::uint64_t raw = bits::present::set(0, 1);
    raw = bits::rw::set_bit(raw, write);
    raw = bits::us::set_bit(raw, user);
    raw = bits::pwt::set_bit(raw, p.cache() == cache_mode::write_through || p.cache() == cache_mode::device ||
                                      p.cache() == cache_mode::device_ordered);
    raw = bits::pcd::set_bit(raw, p.cache() == cache_mode::uncached || p.is_device());
    raw = bits::accessed::set(raw, 1);
    raw = bits::dirty::set(raw, 1);
    raw = bits::ps_or_pat::set_bit(raw, level != Levels::level_count - 1);
    raw = bits::global::set_bit(raw, p.is_global());
    raw = bits::xd::set_bit(raw, !exec);
    return bits::addr::set(raw, frame.value >> 12);
  }

  /** Effective protection of a leaf (kernel mirrors the page's access; supervisor execute of user pages is reported
   * off, as with SMEP). */
  [[nodiscard]] static constexpr protection attrs(std::uint64_t raw, std::size_t) noexcept {
    const bool write = bits::rw::test(raw);
    const bool exec = !bits::xd::test(raw);
    protection p;
    p = p.with_kernel(write ? kprot::write : kprot::read);
    if (bits::us::test(raw)) {
      p = p.with_user(write ? uprot::write : uprot::read);
      if (exec) {
        p = p.with_user(write ? uprot::write_exec : uprot::read_exec);
      }
    } else if (exec) {
      p = p.with_kernel(write ? kprot::write_exec : kprot::read_exec);
    }
    p = p.with_scope(bits::global::test(raw) ? scope::global : scope::per_address_space);
    const bool wt = bits::pwt::test(raw);
    const bool cd = bits::pcd::test(raw);
    p = p.with_cache(cd ? (wt ? cache_mode::device : cache_mode::uncached)
                        : (wt ? cache_mode::write_through : cache_mode::write_back));
    return p;
  }

  /** x86 allows changing a live entry's permissions, memory type and frame without break-before-make. */
  [[nodiscard]] static constexpr bool needs_bbm(std::uint64_t, std::uint64_t) noexcept { return false; }

  /** Sign-extends bit `va_bits-1` (canonical form). */
  [[nodiscard]] static constexpr std::uint64_t canonicalize(std::uint64_t low) noexcept {
    constexpr std::uint64_t mask = (std::uint64_t{1} << Levels::va_bits) - 1;
    low &= mask;
    return (low >> (Levels::va_bits - 1)) & 1 ? (low | ~mask) : low;
  }

private:
  using bits = detail::pte_bits;
};

} // namespace structo::arch::x86
