// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file mmu_regs.hpp
 * @brief Parsers/builders for x86/x86-64 MMU control registers: `CR0`,
 * `CR3`, `CR4`, and the `EFER` MSR.
 *
 * ## Scope and split
 *
 * As with the ARM/RISC-V counterparts, each register gets a small value
 * type wrapping the raw register image, with named bit accessors built
 * on `structo::arch::pte_bit_field`. This parse/build logic is pure bit
 * arithmetic, fully host-testable.
 *
 * `read()`/`write()` round-trip a value type through the real register
 * via `mov %crN`/`rdmsr`/`wrmsr`, compiled only for a genuine x86/x86-64
 * target (`__i386__`/`__x86_64__`) -- on any other host this header
 * still defines every value type (cross-compiled header checks stay
 * clean), it just omits `read()`/`write()`. Control-register and MSR
 * access is privileged (CPL 0) and cannot be exercised from an
 * unprivileged test process; only the pure parse/build logic is unit
 * tested.
 *
 * `CR3`'s base-address field width assumes a (generous, current-
 * hardware-superset) 52-bit maximum physical address; a narrower
 * `MAXPHYADDR` simply means the extra high bits of `base_addr()` always
 * read back as `0`.
 *
 * ## Example
 *
 * @code
 * using namespace structo::arch::x86;
 *
 * cr4 c4{};
 * c4.set_pae(true).set_pcide(true);
 *
 * cr3 c3{};
 * c3.set_base_addr(pml4_phys_addr).set_pcid(7);
 *
 * cr0 c0{};
 * c0.set_paging_enabled(true).set_write_protect(true);
 *
 * #if defined(__i386__) || defined(__x86_64__)
 * c4.write();
 * c3.write();
 * c0.write();
 * #endif
 * @endcode
 */

#include <structo/arch/pte_field.hpp>

#include <cstdint>

namespace structo::arch::x86 {

/** @brief Named bit-field accessors for `CR0`. */
struct cr0_bits {
  using pe = pte_bit_field<0, 1, std::uint64_t>;  //!< Bit 0: Protected Mode Enable.
  using mp = pte_bit_field<1, 1, std::uint64_t>;  //!< Bit 1: Monitor Co-Processor.
  using em = pte_bit_field<2, 1, std::uint64_t>;  //!< Bit 2: Emulation (no x87 FPU present).
  using ts = pte_bit_field<3, 1, std::uint64_t>;  //!< Bit 3: Task Switched.
  using ne = pte_bit_field<5, 1, std::uint64_t>;  //!< Bit 5: Numeric Error (native x87 exception reporting).
  using wp = pte_bit_field<16, 1, std::uint64_t>; //!< Bit 16: Write Protect (CPL0 honors read-only PTEs too).
  using am = pte_bit_field<18, 1, std::uint64_t>; //!< Bit 18: Alignment Mask.
  using nw = pte_bit_field<29, 1, std::uint64_t>; //!< Bit 29: Not Write-through.
  using cd = pte_bit_field<30, 1, std::uint64_t>; //!< Bit 30: Cache Disable.
  using pg = pte_bit_field<31, 1, std::uint64_t>; //!< Bit 31: Paging Enable.
};

/**
 * @brief Parsed/built view of `CR0`. Only the bits most relevant to
 * bringing up or tearing down paging are named; every other bit of
 * `raw` is preserved untouched by the accessors below.
 */
struct cr0 {
  std::uint64_t raw{0};

  [[nodiscard]] static constexpr cr0 from_raw(std::uint64_t value) noexcept { return cr0{value}; }

  [[nodiscard]] constexpr bool protected_mode_enabled() const noexcept { return cr0_bits::pe::test(raw); }
  constexpr cr0 &set_protected_mode_enabled(bool value) noexcept {
    raw = cr0_bits::pe::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool write_protect() const noexcept { return cr0_bits::wp::test(raw); }
  constexpr cr0 &set_write_protect(bool value) noexcept {
    raw = cr0_bits::wp::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool cache_disabled() const noexcept { return cr0_bits::cd::test(raw); }
  constexpr cr0 &set_cache_disabled(bool value) noexcept {
    raw = cr0_bits::cd::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool paging_enabled() const noexcept { return cr0_bits::pg::test(raw); }
  constexpr cr0 &set_paging_enabled(bool value) noexcept {
    raw = cr0_bits::pg::set_bit(raw, value);
    return *this;
  }

#if defined(__i386__) || defined(__x86_64__)
  /** @brief Reads the live `CR0` register. Privileged (CPL0); not unit tested. */
  [[nodiscard]] static cr0 read() noexcept {
    std::uint64_t value;
    asm volatile("mov %%cr0, %0" : "=r"(value));
    return cr0{value};
  }

  /** @brief Writes `raw` to the live `CR0` register. Privileged (CPL0); not unit tested. */
  void write() const noexcept { asm volatile("mov %0, %%cr0" ::"r"(raw) : "memory"); }
#endif // defined(__i386__) || defined(__x86_64__)
};

/** @brief Named bit-field accessors for `CR3`. */
struct cr3_bits {
  using pwt = pte_bit_field<3, 1>;     //!< Bit 3: Page-level Write-Through (only meaningful without PCID).
  using pcd = pte_bit_field<4, 1>;     //!< Bit 4: Page-level Cache Disable (only meaningful without PCID).
  using pcid = pte_bit_field<0, 12>;   //!< Bits [11:0]: Process-Context Identifier (only meaningful with `CR4.PCIDE`).
  using base_addr = pte_bit_field<12, 40>; //!< Bits [51:12]: top-level page-table physical base address.
};

/**
 * @brief Parsed/built view of `CR3`. `PWT`/`PCD` and `PCID` alias the
 * same low bits -- which interpretation applies depends on `CR4.PCIDE`,
 * same as the architecture itself, so both accessor pairs are provided
 * and it is the caller's responsibility to use the one matching the
 * active `CR4.PCIDE` setting.
 */
struct cr3 {
  std::uint64_t raw{0};

  [[nodiscard]] static constexpr cr3 from_raw(std::uint64_t value) noexcept { return cr3{value}; }

  [[nodiscard]] constexpr bool page_write_through() const noexcept { return cr3_bits::pwt::test(raw); }
  constexpr cr3 &set_page_write_through(bool value) noexcept {
    raw = cr3_bits::pwt::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool page_cache_disabled() const noexcept { return cr3_bits::pcd::test(raw); }
  constexpr cr3 &set_page_cache_disabled(bool value) noexcept {
    raw = cr3_bits::pcd::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr std::uint16_t pcid() const noexcept {
    return static_cast<std::uint16_t>(cr3_bits::pcid::get(raw));
  }
  constexpr cr3 &set_pcid(std::uint16_t value) noexcept {
    raw = cr3_bits::pcid::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr std::uint64_t base_addr() const noexcept {
    return cr3_bits::base_addr::get(raw) << 12;
  }
  /** @brief Sets the top-level page-table base address. `addr`'s low 12 bits must be `0` (masked off regardless). */
  constexpr cr3 &set_base_addr(std::uint64_t addr) noexcept {
    raw = cr3_bits::base_addr::set(raw, addr >> 12);
    return *this;
  }

#if defined(__i386__) || defined(__x86_64__)
  /** @brief Reads the live `CR3` register. Privileged (CPL0); not unit tested. */
  [[nodiscard]] static cr3 read() noexcept {
    std::uint64_t value;
    asm volatile("mov %%cr3, %0" : "=r"(value));
    return cr3{value};
  }

  /** @brief Writes `raw` to the live `CR3` register. Privileged (CPL0); not unit tested. */
  void write() const noexcept { asm volatile("mov %0, %%cr3" ::"r"(raw) : "memory"); }
#endif // defined(__i386__) || defined(__x86_64__)
};

/** @brief Named bit-field accessors for `CR4`. */
struct cr4_bits {
  using pae = pte_bit_field<5, 1, std::uint64_t>;   //!< Bit 5: Physical Address Extension.
  using pge = pte_bit_field<7, 1, std::uint64_t>;   //!< Bit 7: Page Global Enable.
  using pcide = pte_bit_field<17, 1, std::uint64_t>; //!< Bit 17: PCID Enable.
  using smep = pte_bit_field<20, 1, std::uint64_t>; //!< Bit 20: Supervisor Mode Execution Prevention.
  using smap = pte_bit_field<21, 1, std::uint64_t>; //!< Bit 21: Supervisor Mode Access Prevention.
  using la57 = pte_bit_field<12, 1, std::uint64_t>; //!< Bit 12: Enable 5-level paging (57-bit linear addresses).
};

/** @brief Parsed/built view of `CR4`. */
struct cr4 {
  std::uint64_t raw{0};

  [[nodiscard]] static constexpr cr4 from_raw(std::uint64_t value) noexcept { return cr4{value}; }

  [[nodiscard]] constexpr bool pae() const noexcept { return cr4_bits::pae::test(raw); }
  constexpr cr4 &set_pae(bool value) noexcept {
    raw = cr4_bits::pae::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool page_global_enabled() const noexcept { return cr4_bits::pge::test(raw); }
  constexpr cr4 &set_page_global_enabled(bool value) noexcept {
    raw = cr4_bits::pge::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool pcide() const noexcept { return cr4_bits::pcide::test(raw); }
  constexpr cr4 &set_pcide(bool value) noexcept {
    raw = cr4_bits::pcide::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool smep() const noexcept { return cr4_bits::smep::test(raw); }
  constexpr cr4 &set_smep(bool value) noexcept {
    raw = cr4_bits::smep::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool smap() const noexcept { return cr4_bits::smap::test(raw); }
  constexpr cr4 &set_smap(bool value) noexcept {
    raw = cr4_bits::smap::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool la57() const noexcept { return cr4_bits::la57::test(raw); }
  constexpr cr4 &set_la57(bool value) noexcept {
    raw = cr4_bits::la57::set_bit(raw, value);
    return *this;
  }

#if defined(__i386__) || defined(__x86_64__)
  /** @brief Reads the live `CR4` register. Privileged (CPL0); not unit tested. */
  [[nodiscard]] static cr4 read() noexcept {
    std::uint64_t value;
    asm volatile("mov %%cr4, %0" : "=r"(value));
    return cr4{value};
  }

  /** @brief Writes `raw` to the live `CR4` register. Privileged (CPL0); not unit tested. */
  void write() const noexcept { asm volatile("mov %0, %%cr4" ::"r"(raw) : "memory"); }
#endif // defined(__i386__) || defined(__x86_64__)
};

/** @brief Named bit-field accessors for the `EFER` MSR (address `0xC000'0080`). */
struct efer_bits {
  using sce = pte_bit_field<0, 1, std::uint64_t>;  //!< Bit 0: System Call Extensions (enables `syscall`/`sysret`).
  using lme = pte_bit_field<8, 1, std::uint64_t>;  //!< Bit 8: Long Mode Enable.
  using lma = pte_bit_field<10, 1, std::uint64_t>; //!< Bit 10: Long Mode Active (read-only status bit).
  using nxe = pte_bit_field<11, 1, std::uint64_t>; //!< Bit 11: No-Execute Enable.
};

/** @brief Parsed/built view of the `EFER` MSR. */
struct efer {
  static constexpr std::uint32_t msr_address = 0xC0000080U;

  std::uint64_t raw{0};

  [[nodiscard]] static constexpr efer from_raw(std::uint64_t value) noexcept { return efer{value}; }

  [[nodiscard]] constexpr bool syscall_enabled() const noexcept { return efer_bits::sce::test(raw); }
  constexpr efer &set_syscall_enabled(bool value) noexcept {
    raw = efer_bits::sce::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool long_mode_enabled() const noexcept { return efer_bits::lme::test(raw); }
  constexpr efer &set_long_mode_enabled(bool value) noexcept {
    raw = efer_bits::lme::set_bit(raw, value);
    return *this;
  }

  /** @brief Read-only status bit: whether the CPU is currently executing in long mode. */
  [[nodiscard]] constexpr bool long_mode_active() const noexcept { return efer_bits::lma::test(raw); }

  [[nodiscard]] constexpr bool no_execute_enabled() const noexcept { return efer_bits::nxe::test(raw); }
  constexpr efer &set_no_execute_enabled(bool value) noexcept {
    raw = efer_bits::nxe::set_bit(raw, value);
    return *this;
  }

#if defined(__i386__) || defined(__x86_64__)
  /** @brief Reads the live `EFER` MSR via `rdmsr`. Privileged (CPL0); not unit tested. */
  [[nodiscard]] static efer read() noexcept {
    std::uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr_address));
    return efer{(static_cast<std::uint64_t>(hi) << 32) | lo};
  }

  /** @brief Writes `raw` to the live `EFER` MSR via `wrmsr`. Privileged (CPL0); not unit tested. */
  void write() const noexcept {
    std::uint32_t lo = static_cast<std::uint32_t>(raw);
    std::uint32_t hi = static_cast<std::uint32_t>(raw >> 32);
    asm volatile("wrmsr" ::"a"(lo), "d"(hi), "c"(msr_address) : "memory");
  }
#endif // defined(__i386__) || defined(__x86_64__)
};

} // namespace structo::arch::x86
