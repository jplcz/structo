// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file riscv/recursive_format.hpp
 * @brief `riscv::recursive_pte_format<Svpbmt>`: RISC-V last-level PTE for `recursive_remapper`. The 4 KiB
 * leaf PTE is identical in the 64-bit modes Sv39, Sv48 and Sv57 (512 entries, 64-bit).
 *
 * The G-stage (`hgatp`) is not covered: its tables are walked in the guest-physical space.
 *
 * ## Protection legalization
 *
 *  - read is always implied (R=1; W without R is reserved); a page is *either* user (U=1, user bits apply)
 *    or supervisor (U=0, kernel bits apply);
 *  - user pages: write = user write && (kernel has no stated access || kernel write); exec = `uexec`.
 *    The kernel's own access to a U page additionally needs `sstatus.SUM` (read/write) — it never executes it;
 *  - A and D are always set; `scope::global` sets G;
 *  - memory types: `write_back` is the default PMA. With `Svpbmt = true`: `uncached` / `write_combining` ->
 *    PBMT=NC, `device*` -> PBMT=IO. Without it every other type is `unsupported_operation`;
 *    `write_through` and `security_state::secure` are always unsupported.
 *
 * RISC-V needs an `sfence.vma` after validating a PTE (`flush_on_map = true`), which the remapper issues
 * through `Tlb::flush`.
 *
 * @code
 * using format = structo::arch::riscv::recursive_pte_format<true>; // Svpbmt available
 * @endcode
 */

#include <structo/arch/protection.hpp>
#include <structo/arch/pte_field.hpp>
#include <structo/arch/riscv/pte.hpp>

#include <reloco/error.hpp>
#include <reloco/expected.hpp>

#include <cstddef>
#include <cstdint>

namespace structo::arch::riscv {

template <bool Svpbmt = false> struct recursive_pte_format {
  using word = std::uint64_t;
  using phys_type = typename page_table_entry_traits<pte_tag>::phys_type;
  static constexpr std::size_t entry_count = 512;
  static constexpr std::uint64_t page_size = 4096;
  static constexpr bool flush_on_map = true;

  [[nodiscard]] static constexpr bool is_present(word raw) noexcept { return bits::valid::test(raw); }
  [[nodiscard]] static constexpr phys_type frame_addr(word raw) noexcept {
    return phys_type{bits::ppn::get(raw) << 12};
  }

  [[nodiscard]] static reloco::result<word> make_leaf(phys_type frame, protection p) noexcept {
    if (p.is_none()) {
      return reloco::unexpected(reloco::error::invalid_argument);
    }
    if (p.security() == security_state::secure || p.cache() == cache_mode::write_through) {
      return reloco::unexpected(reloco::error::unsupported_operation);
    }
    std::uint64_t pbmt = 0;
    if (p.cache() != cache_mode::write_back) {
      if (!Svpbmt) {
        return reloco::unexpected(reloco::error::unsupported_operation);
      }
      pbmt = p.is_device() ? 2 : 1;
    }
    const bool user = p.is_user_visible();
    const bool write = user ? (p.user_write() && (!p.kernel_read() || p.kernel_write())) : p.kernel_write();
    const bool exec = user ? p.user_exec() : p.kernel_exec();

    std::uint64_t raw = bits::valid::set(0, 1);
    raw = bits::read::set(raw, 1);
    raw = bits::write::set_bit(raw, write);
    raw = bits::exec::set_bit(raw, exec);
    raw = bits::user::set_bit(raw, user);
    raw = bits::global::set_bit(raw, p.is_global());
    raw = bits::accessed::set(raw, 1);
    raw = bits::dirty::set(raw, 1);
    raw = bits::ppn::set(raw, frame.value >> 12);
    return raw | (pbmt << 61);
  }

  [[nodiscard]] static protection attrs(word raw) noexcept {
    const bool write = bits::write::test(raw);
    const bool exec = bits::exec::test(raw);
    protection p;
    if (bits::user::test(raw)) {
      p = p.with_kernel(kprot::read).with_user(static_cast<uprot>((write ? 2 : 1) | (exec ? 4 : 0)));
    } else {
      p = p.with_kernel(static_cast<kprot>((write ? 2 : 1) | (exec ? 4 : 0)));
    }
    p = p.with_scope(bits::global::test(raw) ? scope::global : scope::per_address_space);
    switch ((raw >> 61) & 3) {
    case 1:
      return p.with_cache(cache_mode::uncached);
    case 2:
      return p.with_cache(cache_mode::device);
    default:
      return p.with_cache(cache_mode::write_back);
    }
  }

  /** RISC-V allows changing a live valid PTE; the caller issues `sfence.vma` (done through `Tlb::flush`). */
  [[nodiscard]] static constexpr bool needs_bbm(word, word) noexcept { return false; }

private:
  using bits = detail::pte_bits;
};

} // namespace structo::arch::riscv
