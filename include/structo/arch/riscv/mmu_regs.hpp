// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file mmu_regs.hpp
 * @brief Parser/builder for the RISC-V `satp` (Supervisor Address
 * Translation and Protection) CSR, RV64 only (matching
 * `structo/arch/riscv/page_table_traits.hpp`'s Sv39/Sv48/Sv57-only
 * scope -- Sv32's 32-bit `satp` layout is not modeled here).
 *
 * ## Scope and split
 *
 * As with the ARM counterparts, `satp` gets a small value type wrapping
 * the raw 64-bit CSR image, with named bit accessors built on
 * `structo::arch::pte_bit_field`. This parse/build logic is pure bit
 * arithmetic, fully host-testable.
 *
 * `read()`/`write()` round-trip a value type through the real `satp` CSR
 * via `csrr`/`csrw`, compiled only for a genuine RISC-V target
 * (`__riscv`) -- on any other host this header still defines the value
 * type (cross-compiled header checks stay clean), it just omits
 * `read()`/`write()`. `satp` is only accessible from S-mode or higher, so
 * this is privileged access that cannot be exercised from an
 * unprivileged test process; only the pure parse/build logic is unit
 * tested.
 *
 * ## Example
 *
 * @code
 * using namespace structo::arch::riscv;
 *
 * satp s{};
 * s.set_mode(satp_mode::sv39).set_asid(3).set_ppn(root_table_pfn);
 *
 * #if defined(__riscv)
 * s.write();
 * #endif
 * @endcode
 */

#include <structo/arch/pte_field.hpp>

#include <cstdint>

namespace structo::arch::riscv {

/** @brief `satp.MODE` encodings selecting the active paging scheme (or none). */
enum class satp_mode : unsigned {
  bare = 0,  //!< No translation/protection; every address is physical.
  sv39 = 8,  //!< 3-level, 39-bit virtual address space.
  sv48 = 9,  //!< 4-level, 48-bit virtual address space.
  sv57 = 10, //!< 5-level, 57-bit virtual address space.
};

/** @brief Named bit-field accessors for `satp` (RV64 layout). */
struct satp_bits {
  using ppn = pte_bit_field<0, 44>;   //!< Bits [43:0]: root page-table physical page number.
  using asid = pte_bit_field<44, 16>; //!< Bits [59:44]: Address Space ID.
  using mode = pte_bit_field<60, 4>;  //!< Bits [63:60]: paging mode (`satp_mode`).
};

/**
 * @brief Parsed/built view of the RV64 `satp` CSR: paging mode, ASID,
 * and the root page-table's physical page number.
 */
struct satp {
  std::uint64_t raw{0};

  [[nodiscard]] static constexpr satp from_raw(std::uint64_t value) noexcept { return satp{value}; }

  [[nodiscard]] constexpr satp_mode mode() const noexcept { return static_cast<satp_mode>(satp_bits::mode::get(raw)); }
  constexpr satp &set_mode(satp_mode value) noexcept {
    raw = satp_bits::mode::set(raw, static_cast<std::uint64_t>(value));
    return *this;
  }

  [[nodiscard]] constexpr std::uint16_t asid() const noexcept {
    return static_cast<std::uint16_t>(satp_bits::asid::get(raw));
  }
  constexpr satp &set_asid(std::uint16_t value) noexcept {
    raw = satp_bits::asid::set(raw, value);
    return *this;
  }

  /** @brief The root page-table's physical page number (physical address `>> 12`). */
  [[nodiscard]] constexpr std::uint64_t ppn() const noexcept { return satp_bits::ppn::get(raw); }
  constexpr satp &set_ppn(std::uint64_t value) noexcept {
    raw = satp_bits::ppn::set(raw, value);
    return *this;
  }

#if defined(__riscv)
  /** @brief Reads the live `satp` CSR. Requires S-mode or higher; not unit tested. */
  [[nodiscard]] static satp read() noexcept {
    std::uint64_t value;
    asm volatile("csrr %0, satp" : "=r"(value));
    return satp{value};
  }

  /** @brief Writes `raw` to the live `satp` CSR. Requires S-mode or higher; not unit tested. */
  void write() const noexcept { asm volatile("csrw satp, %0" ::"r"(raw) : "memory"); }
#endif // defined(__riscv)
};

} // namespace structo::arch::riscv
