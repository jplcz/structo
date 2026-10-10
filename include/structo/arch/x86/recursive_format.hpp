// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file x86/recursive_format.hpp
 * @brief Last-level (PTE) formats of x86 paging for `recursive_remapper`.
 *
 * - `x86::recursive_pte_format`: 64-bit entries, 512 per table. Identical for PAE, 4-level and 5-level
 *   long mode (the PTE layout does not change).
 * - `x86::recursive_pte32_format`: classic 32-bit non-PAE paging, 32-bit entries, 1024 per table.
 *
 * Both map 4 KiB pages. Nested paging (NPT/EPT) is not covered: its tables are walked in the guest-physical
 * space, which the CPU cannot use to reach them.
 *
 * ## Protection legalization (one US, one RW, one XD bit)
 *
 *  - user-visible (`user != none`): US=1; writable only if the user may write *and* the kernel may write
 *    (or has no stated access); executable follows `uexec`;
 *  - otherwise US=0, RW follows `kwrite`, executable follows `kexec`;
 *  - read is always implied; XD is set whenever not executable (needs `EFER.NXE`);
 *  - accessed and dirty are always set; `scope::global` sets G (needs `CR4.PGE`);
 *  - memory types use the reset PAT: write-back, write-through, UC- (`uncached`), UC (`device*`);
 *    `write_combining` and `security_state::secure` fail with `unsupported_operation`.
 *
 * **32-bit non-PAE exception:** the CPU has no NX bit, so every readable page is executable; the request's
 * execute bits are ignored and `attrs()` reports execute as allowed. Physical addresses must be below 4 GiB.
 *
 * @code
 * using format = structo::arch::x86::recursive_pte_format;   // PAE / x86-64
 * using format32 = structo::arch::x86::recursive_pte32_format; // i386 without PAE
 * @endcode
 */

#include <structo/arch/protection.hpp>
#include <structo/arch/pte_field.hpp>
#include <structo/arch/x86/pte.hpp>

#include <reloco/error.hpp>
#include <reloco/expected.hpp>

#include <cstddef>
#include <cstdint>

namespace structo::arch::x86 {

namespace detail {

// Shared encode/decode on the 64-bit view of an x86 PTE. `Nx` tells whether XD exists.
template <bool Nx> struct recursive_pte_codec {
  using bits = pte_bits;

  [[nodiscard]] static reloco::result<std::uint64_t> encode(std::uint64_t frame, protection p) noexcept {
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
    raw = bits::pwt::set_bit(raw, p.cache() == cache_mode::write_through || p.is_device());
    raw = bits::pcd::set_bit(raw, p.cache() == cache_mode::uncached || p.is_device());
    raw = bits::accessed::set(raw, 1);
    raw = bits::dirty::set(raw, 1);
    raw = bits::global::set_bit(raw, p.is_global());
    if constexpr (Nx) {
      raw = bits::xd::set_bit(raw, !exec);
    }
    return bits::addr::set(raw, frame >> 12);
  }

  [[nodiscard]] static protection decode(std::uint64_t raw) noexcept {
    const bool write = bits::rw::test(raw);
    const bool exec = !Nx || !bits::xd::test(raw);
    protection p;
    p = p.with_kernel(write ? kprot::write : kprot::read);
    if (bits::us::test(raw)) {
      p = p.with_user(write ? (exec ? uprot::write_exec : uprot::write) : (exec ? uprot::read_exec : uprot::read));
    } else if (exec) {
      p = p.with_kernel(write ? kprot::write_exec : kprot::read_exec);
    }
    p = p.with_scope(bits::global::test(raw) ? scope::global : scope::per_address_space);
    const bool wt = bits::pwt::test(raw);
    const bool cd = bits::pcd::test(raw);
    return p.with_cache(cd ? (wt ? cache_mode::device : cache_mode::uncached)
                           : (wt ? cache_mode::write_through : cache_mode::write_back));
  }
};

} // namespace detail

/** 64-bit PTE (PAE, long mode 4/5-level). */
struct recursive_pte_format {
  using word = std::uint64_t;
  using phys_type = typename page_table_entry_traits<pte_tag>::phys_type;
  static constexpr std::size_t entry_count = 512;
  static constexpr std::uint64_t page_size = 4096;
  static constexpr bool flush_on_map = false;

  [[nodiscard]] static constexpr bool is_present(word raw) noexcept { return detail::pte_bits::is_present(raw); }
  [[nodiscard]] static constexpr phys_type frame_addr(word raw) noexcept {
    return phys_type{detail::pte_bits::addr::get(raw) << 12};
  }
  [[nodiscard]] static reloco::result<word> make_leaf(phys_type frame, protection p) noexcept {
    return detail::recursive_pte_codec<true>::encode(frame.value, p);
  }
  [[nodiscard]] static protection attrs(word raw) noexcept { return detail::recursive_pte_codec<true>::decode(raw); }
  /** x86 permits changing a live entry (permissions, memory type, frame) without break-before-make. */
  [[nodiscard]] static constexpr bool needs_bbm(word, word) noexcept { return false; }
};

/** 32-bit non-PAE PTE. */
struct recursive_pte32_format {
  using word = std::uint32_t;
  using phys_type = typename page_table_entry_traits<pte_tag>::phys_type;
  static constexpr std::size_t entry_count = 1024;
  static constexpr std::uint64_t page_size = 4096;
  static constexpr bool flush_on_map = false;

  [[nodiscard]] static constexpr bool is_present(word raw) noexcept { return (raw & 1u) != 0; }
  [[nodiscard]] static constexpr phys_type frame_addr(word raw) noexcept {
    return phys_type{static_cast<std::uint64_t>(raw & 0xFFFF'F000u)};
  }
  [[nodiscard]] static reloco::result<word> make_leaf(phys_type frame, protection p) noexcept {
    if (frame.value >> 32 != 0) {
      return reloco::unexpected(reloco::error::out_of_range);
    }
    auto r = detail::recursive_pte_codec<false>::encode(frame.value, p);
    if (!r) {
      return reloco::unexpected(r.error());
    }
    const std::uint64_t raw = *r;
    return static_cast<word>(raw);
  }
  [[nodiscard]] static protection attrs(word raw) noexcept { return detail::recursive_pte_codec<false>::decode(raw); }
  [[nodiscard]] static constexpr bool needs_bbm(word, word) noexcept { return false; }
};

} // namespace structo::arch::x86
