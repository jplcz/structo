// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file mmu_regs.hpp
 * @brief Parsers/builders for AArch64 (VMSAv8-64) EL1 MMU control
 * registers: `SCTLR_EL1`, `TCR_EL1`, `TTBR0_EL1`/`TTBR1_EL1`, and
 * `MAIR_EL1`.
 *
 * ## Scope and split
 *
 * Each register gets a small value type wrapping the raw 64-bit
 * register image, with named, compile-time-checked bit accessors built
 * on `structo::arch::pte_bit_field` (the same primitive the per-
 * architecture page-table-entry headers use -- a control register is,
 * structurally, just another flat integer with named fixed-position
 * fields). This parse/build logic is pure bit arithmetic over an
 * in-memory value: it has no architecture dependency and is fully
 * host-testable.
 *
 * The `read()`/`write()` static methods that round-trip a value type
 * through the *real* system register are a separate, thin layer on top,
 * compiled only on a genuine AArch64 target (`__aarch64__`) -- on any
 * other host this header still defines every value type and its
 * accessors (so cross-compiled header checks stay clean), it just omits
 * `read()`/`write()`, matching the gating convention established by
 * `structo/arch/arm64/irq_guard.hpp`. `MRS`/`MSR` to these registers is
 * privileged (EL1), so `read()`/`write()` cannot be exercised from an
 * unprivileged test process; only the pure parse/build logic is unit
 * tested.
 *
 * ## Example
 *
 * @code
 * using namespace structo::arch::arm64;
 *
 * tcr_el1 tcr{};
 * tcr.set_t0sz(16).set_t1sz(16).set_tg0(0).set_tg1(2).set_ips(0b101);
 *
 * sctlr_el1 sctlr{};
 * sctlr.set_mmu_enabled(true).set_dcache_enabled(true).set_icache_enabled(true);
 *
 * #if defined(__aarch64__)
 * tcr.write();
 * sctlr.write();
 * #endif
 * @endcode
 */

#include <structo/arch/pte_field.hpp>

#include <cstdint>

namespace structo::arch::arm64 {

/** @brief Named bit-field accessors for `SCTLR_EL1`. */
struct sctlr_el1_bits {
  using m = pte_bit_field<0, 1>;    //!< Bit 0: MMU enable.
  using a = pte_bit_field<1, 1>;    //!< Bit 1: alignment check enable.
  using c = pte_bit_field<2, 1>;    //!< Bit 2: (data) cache enable.
  using sa = pte_bit_field<3, 1>;   //!< Bit 3: SP alignment check enable.
  using i = pte_bit_field<12, 1>;   //!< Bit 12: instruction cache enable.
  using wxn = pte_bit_field<19, 1>; //!< Bit 19: write permission implies execute-never.
};

/**
 * @brief Parsed/built view of `SCTLR_EL1`, the AArch64 EL1 System
 * Control Register. Only the handful of bits most relevant to bringing
 * up or tearing down a stage-1 translation are modeled; every other bit
 * of `raw` is preserved untouched by the named accessors below.
 */
struct sctlr_el1 {
  std::uint64_t raw{0};

  [[nodiscard]] static constexpr sctlr_el1 from_raw(std::uint64_t value) noexcept { return sctlr_el1{value}; }

  [[nodiscard]] constexpr bool mmu_enabled() const noexcept { return sctlr_el1_bits::m::test(raw); }
  constexpr sctlr_el1 &set_mmu_enabled(bool value) noexcept {
    raw = sctlr_el1_bits::m::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool alignment_check_enabled() const noexcept { return sctlr_el1_bits::a::test(raw); }
  constexpr sctlr_el1 &set_alignment_check_enabled(bool value) noexcept {
    raw = sctlr_el1_bits::a::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool dcache_enabled() const noexcept { return sctlr_el1_bits::c::test(raw); }
  constexpr sctlr_el1 &set_dcache_enabled(bool value) noexcept {
    raw = sctlr_el1_bits::c::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool sp_alignment_check_enabled() const noexcept { return sctlr_el1_bits::sa::test(raw); }
  constexpr sctlr_el1 &set_sp_alignment_check_enabled(bool value) noexcept {
    raw = sctlr_el1_bits::sa::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool icache_enabled() const noexcept { return sctlr_el1_bits::i::test(raw); }
  constexpr sctlr_el1 &set_icache_enabled(bool value) noexcept {
    raw = sctlr_el1_bits::i::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool write_xn() const noexcept { return sctlr_el1_bits::wxn::test(raw); }
  constexpr sctlr_el1 &set_write_xn(bool value) noexcept {
    raw = sctlr_el1_bits::wxn::set_bit(raw, value);
    return *this;
  }

#if defined(__aarch64__)
  /** @brief Reads the live `SCTLR_EL1` register. Privileged (EL1); not unit tested. */
  [[nodiscard]] static sctlr_el1 read() noexcept {
    std::uint64_t value;
    asm volatile("mrs %0, sctlr_el1" : "=r"(value));
    return sctlr_el1{value};
  }

  /** @brief Writes `raw` to the live `SCTLR_EL1` register. Privileged (EL1); not unit tested. */
  void write() const noexcept { asm volatile("msr sctlr_el1, %0" ::"r"(raw) : "memory"); }
#endif // defined(__aarch64__)
};

/** @brief Named bit-field accessors for `TCR_EL1`. */
struct tcr_el1_bits {
  using t0sz = pte_bit_field<0, 6>;   //!< Bits [5:0]: TTBR0_EL1 input-address size, `64 - T0SZ`.
  using epd0 = pte_bit_field<7, 1>;   //!< Bit 7: disable TTBR0_EL1 table walks.
  using irgn0 = pte_bit_field<8, 2>;  //!< Bits [9:8]: TTBR0_EL1 inner cacheability.
  using orgn0 = pte_bit_field<10, 2>; //!< Bits [11:10]: TTBR0_EL1 outer cacheability.
  using sh0 = pte_bit_field<12, 2>;   //!< Bits [13:12]: TTBR0_EL1 shareability.
  using tg0 = pte_bit_field<14, 2>;   //!< Bits [15:14]: TTBR0_EL1 granule (0=4KB, 2=16KB, 1=64KB).
  using t1sz = pte_bit_field<16, 6>;  //!< Bits [21:16]: TTBR1_EL1 input-address size, `64 - T1SZ`.
  using a1 = pte_bit_field<22, 1>;    //!< Bit 22: which TTBR supplies the ASID (0=TTBR0, 1=TTBR1).
  using epd1 = pte_bit_field<23, 1>;  //!< Bit 23: disable TTBR1_EL1 table walks.
  using irgn1 = pte_bit_field<24, 2>; //!< Bits [25:24]: TTBR1_EL1 inner cacheability.
  using orgn1 = pte_bit_field<26, 2>; //!< Bits [27:26]: TTBR1_EL1 outer cacheability.
  using sh1 = pte_bit_field<28, 2>;   //!< Bits [29:28]: TTBR1_EL1 shareability.
  using tg1 = pte_bit_field<30, 2>;   //!< Bits [31:30]: TTBR1_EL1 granule (1=16KB, 2=4KB, 3=64KB).
  using ips = pte_bit_field<32, 3>;   //!< Bits [34:32]: intermediate physical address size.
  using as = pte_bit_field<36, 1>;    //!< Bit 36: ASID size (0=8-bit, 1=16-bit).
  using tbi0 = pte_bit_field<37, 1>;  //!< Bit 37: top-byte ignored for TTBR0_EL1 addresses.
  using tbi1 = pte_bit_field<38, 1>;  //!< Bit 38: top-byte ignored for TTBR1_EL1 addresses.
  using ha = pte_bit_field<39, 1>;    //!< Bit 39: hardware management of the Access flag.
  using hd = pte_bit_field<40, 1>;    //!< Bit 40: hardware management of dirty state.
};

/**
 * @brief Parsed/built view of `TCR_EL1`, the AArch64 EL1 Translation
 * Control Register: input-address sizes, granule sizes, cacheability/
 * shareability attributes, and the intermediate physical address size
 * for both the TTBR0_EL1-rooted (low) and TTBR1_EL1-rooted (high)
 * translation ranges.
 */
struct tcr_el1 {
  std::uint64_t raw{0};

  [[nodiscard]] static constexpr tcr_el1 from_raw(std::uint64_t value) noexcept { return tcr_el1{value}; }

  [[nodiscard]] constexpr unsigned t0sz() const noexcept { return static_cast<unsigned>(tcr_el1_bits::t0sz::get(raw)); }
  constexpr tcr_el1 &set_t0sz(unsigned value) noexcept {
    raw = tcr_el1_bits::t0sz::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned t1sz() const noexcept { return static_cast<unsigned>(tcr_el1_bits::t1sz::get(raw)); }
  constexpr tcr_el1 &set_t1sz(unsigned value) noexcept {
    raw = tcr_el1_bits::t1sz::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool epd0() const noexcept { return tcr_el1_bits::epd0::test(raw); }
  constexpr tcr_el1 &set_epd0(bool value) noexcept {
    raw = tcr_el1_bits::epd0::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool epd1() const noexcept { return tcr_el1_bits::epd1::test(raw); }
  constexpr tcr_el1 &set_epd1(bool value) noexcept {
    raw = tcr_el1_bits::epd1::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned irgn0() const noexcept {
    return static_cast<unsigned>(tcr_el1_bits::irgn0::get(raw));
  }
  constexpr tcr_el1 &set_irgn0(unsigned value) noexcept {
    raw = tcr_el1_bits::irgn0::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned orgn0() const noexcept {
    return static_cast<unsigned>(tcr_el1_bits::orgn0::get(raw));
  }
  constexpr tcr_el1 &set_orgn0(unsigned value) noexcept {
    raw = tcr_el1_bits::orgn0::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned irgn1() const noexcept {
    return static_cast<unsigned>(tcr_el1_bits::irgn1::get(raw));
  }
  constexpr tcr_el1 &set_irgn1(unsigned value) noexcept {
    raw = tcr_el1_bits::irgn1::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned orgn1() const noexcept {
    return static_cast<unsigned>(tcr_el1_bits::orgn1::get(raw));
  }
  constexpr tcr_el1 &set_orgn1(unsigned value) noexcept {
    raw = tcr_el1_bits::orgn1::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned sh0() const noexcept { return static_cast<unsigned>(tcr_el1_bits::sh0::get(raw)); }
  constexpr tcr_el1 &set_sh0(unsigned value) noexcept {
    raw = tcr_el1_bits::sh0::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned sh1() const noexcept { return static_cast<unsigned>(tcr_el1_bits::sh1::get(raw)); }
  constexpr tcr_el1 &set_sh1(unsigned value) noexcept {
    raw = tcr_el1_bits::sh1::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned tg0() const noexcept { return static_cast<unsigned>(tcr_el1_bits::tg0::get(raw)); }
  constexpr tcr_el1 &set_tg0(unsigned value) noexcept {
    raw = tcr_el1_bits::tg0::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned tg1() const noexcept { return static_cast<unsigned>(tcr_el1_bits::tg1::get(raw)); }
  constexpr tcr_el1 &set_tg1(unsigned value) noexcept {
    raw = tcr_el1_bits::tg1::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned ips() const noexcept { return static_cast<unsigned>(tcr_el1_bits::ips::get(raw)); }
  constexpr tcr_el1 &set_ips(unsigned value) noexcept {
    raw = tcr_el1_bits::ips::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool asid_16bit() const noexcept { return tcr_el1_bits::as::test(raw); }
  constexpr tcr_el1 &set_asid_16bit(bool value) noexcept {
    raw = tcr_el1_bits::as::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool asid_from_ttbr1() const noexcept { return tcr_el1_bits::a1::test(raw); }
  constexpr tcr_el1 &set_asid_from_ttbr1(bool value) noexcept {
    raw = tcr_el1_bits::a1::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool tbi0() const noexcept { return tcr_el1_bits::tbi0::test(raw); }
  constexpr tcr_el1 &set_tbi0(bool value) noexcept {
    raw = tcr_el1_bits::tbi0::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool tbi1() const noexcept { return tcr_el1_bits::tbi1::test(raw); }
  constexpr tcr_el1 &set_tbi1(bool value) noexcept {
    raw = tcr_el1_bits::tbi1::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool hw_access_flag() const noexcept { return tcr_el1_bits::ha::test(raw); }
  constexpr tcr_el1 &set_hw_access_flag(bool value) noexcept {
    raw = tcr_el1_bits::ha::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool hw_dirty_state() const noexcept { return tcr_el1_bits::hd::test(raw); }
  constexpr tcr_el1 &set_hw_dirty_state(bool value) noexcept {
    raw = tcr_el1_bits::hd::set_bit(raw, value);
    return *this;
  }

#if defined(__aarch64__)
  /** @brief Reads the live `TCR_EL1` register. Privileged (EL1); not unit tested. */
  [[nodiscard]] static tcr_el1 read() noexcept {
    std::uint64_t value;
    asm volatile("mrs %0, tcr_el1" : "=r"(value));
    return tcr_el1{value};
  }

  /** @brief Writes `raw` to the live `TCR_EL1` register. Privileged (EL1); not unit tested. */
  void write() const noexcept { asm volatile("msr tcr_el1, %0" ::"r"(raw) : "memory"); }
#endif // defined(__aarch64__)
};

/** @brief Named bit-field accessors shared by `TTBR0_EL1`/`TTBR1_EL1` (identical layout). */
struct ttbr_el1_bits {
  using cnp = pte_bit_field<0, 1>;     //!< Bit 0: Common not Private (`FEAT_TTCNP`).
  using baddr = pte_bit_field<1, 47>;  //!< Bits [47:1]: translation table base address.
  using asid = pte_bit_field<48, 16>;  //!< Bits [63:48]: Address Space ID.
};

/**
 * @brief Parsed/built view shared by `TTBR0_EL1` and `TTBR1_EL1`: the
 * translation table base address and ASID. `Which` only selects which
 * `MRS`/`MSR` mnemonic `read()`/`write()` use; the bit layout itself is
 * identical for both registers.
 */
template <bool IsTtbr1> struct ttbr_el1 {
  std::uint64_t raw{0};

  [[nodiscard]] static constexpr ttbr_el1 from_raw(std::uint64_t value) noexcept { return ttbr_el1{value}; }

  [[nodiscard]] constexpr bool common_not_private() const noexcept { return ttbr_el1_bits::cnp::test(raw); }
  constexpr ttbr_el1 &set_common_not_private(bool value) noexcept {
    raw = ttbr_el1_bits::cnp::set_bit(raw, value);
    return *this;
  }

  /** @brief The translation table base address, already shifted into place (low bit is `CnP`, bit 0). */
  [[nodiscard]] constexpr std::uint64_t base_addr() const noexcept { return ttbr_el1_bits::baddr::get(raw) << 1; }
  /** @brief Sets the translation table base address. `addr`'s bit 0 must be `0` (it is masked off regardless). */
  constexpr ttbr_el1 &set_base_addr(std::uint64_t addr) noexcept {
    raw = ttbr_el1_bits::baddr::set(raw, addr >> 1);
    return *this;
  }

  [[nodiscard]] constexpr std::uint16_t asid() const noexcept {
    return static_cast<std::uint16_t>(ttbr_el1_bits::asid::get(raw));
  }
  constexpr ttbr_el1 &set_asid(std::uint16_t value) noexcept {
    raw = ttbr_el1_bits::asid::set(raw, value);
    return *this;
  }

#if defined(__aarch64__)
  /** @brief Reads the live register. Privileged (EL1); not unit tested. */
  [[nodiscard]] static ttbr_el1 read() noexcept {
    std::uint64_t value;
    if constexpr (IsTtbr1) {
      asm volatile("mrs %0, ttbr1_el1" : "=r"(value));
    } else {
      asm volatile("mrs %0, ttbr0_el1" : "=r"(value));
    }
    return ttbr_el1{value};
  }

  /** @brief Writes `raw` to the live register. Privileged (EL1); not unit tested. */
  void write() const noexcept {
    if constexpr (IsTtbr1) {
      asm volatile("msr ttbr1_el1, %0" ::"r"(raw) : "memory");
    } else {
      asm volatile("msr ttbr0_el1, %0" ::"r"(raw) : "memory");
    }
  }
#endif // defined(__aarch64__)
};

/** @brief `TTBR0_EL1`: the low (user-range) translation table base register. */
using ttbr0_el1 = ttbr_el1<false>;
/** @brief `TTBR1_EL1`: the high (kernel-range) translation table base register. */
using ttbr1_el1 = ttbr_el1<true>;

/**
 * @brief Parsed/built view of `MAIR_EL1`, the Memory Attribute Indirection
 * Register: eight independent 8-bit memory-attribute encodings
 * (`Attr0`..`Attr7`), selected per page-table entry by `TCR_EL1`'s
 * `AttrIndx` field (see `structo::arch::detail::vmsa::stage1_bits::attr_indx`).
 */
struct mair_el1 {
  std::uint64_t raw{0};

  [[nodiscard]] static constexpr mair_el1 from_raw(std::uint64_t value) noexcept { return mair_el1{value}; }

  /** @brief Gets the 8-bit attribute encoding at index `index` (0..7). */
  [[nodiscard]] constexpr std::uint8_t attr(unsigned index) const noexcept {
    return static_cast<std::uint8_t>((raw >> (index * 8)) & 0xffU);
  }

  /** @brief Sets the 8-bit attribute encoding at index `index` (0..7). */
  constexpr mair_el1 &set_attr(unsigned index, std::uint8_t value) noexcept {
    std::uint64_t shift = static_cast<std::uint64_t>(index) * 8;
    raw = (raw & ~(std::uint64_t(0xff) << shift)) | (static_cast<std::uint64_t>(value) << shift);
    return *this;
  }

#if defined(__aarch64__)
  /** @brief Reads the live `MAIR_EL1` register. Privileged (EL1); not unit tested. */
  [[nodiscard]] static mair_el1 read() noexcept {
    std::uint64_t value;
    asm volatile("mrs %0, mair_el1" : "=r"(value));
    return mair_el1{value};
  }

  /** @brief Writes `raw` to the live `MAIR_EL1` register. Privileged (EL1); not unit tested. */
  void write() const noexcept { asm volatile("msr mair_el1, %0" ::"r"(raw) : "memory"); }
#endif // defined(__aarch64__)
};

} // namespace structo::arch::arm64
