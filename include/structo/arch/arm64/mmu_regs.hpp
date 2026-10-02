// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file mmu_regs.hpp
 * @brief Parsers/builders for AArch64 (VMSAv8-64) MMU control registers
 * across three exception levels: EL1 stage-1 (`SCTLR_EL1`, `TCR_EL1`,
 * `TTBR0_EL1`/`TTBR1_EL1`, `MAIR_EL1`), EL2 stage-2/virtualization
 * (`HCR_EL2`, `VTCR_EL2`, `VTTBR_EL2`), and the EL3 TrustZone world
 * switch (`SCR_EL3`).
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
 * privileged (EL1 for the stage-1 group, EL2 for the stage-2/
 * virtualization group, EL3 for `SCR_EL3`), so `read()`/`write()` cannot
 * be exercised from an unprivileged test process; only the pure parse/
 * build logic is unit tested.
 *
 * ## TrustZone: `SCR_EL3`
 *
 * `SCR_EL3.NS` selects which world (Secure or Non-secure) the next
 * lower exception level runs in -- the AArch64 analog of ARMv7-A's
 * `SCR.NS` (see `structo::arch::arm::scr`) -- and is the register
 * Secure/EL3 ("Monitor") software uses to configure that world switch,
 * plus which lower-EL exception classes trap to EL3 and whether `SMC`/
 * `HVC` are available.
 *
 * ## Hypervisor stage-2 translation: `HCR_EL2`/`VTCR_EL2`/`VTTBR_EL2`
 *
 * These three configure EL2's *second* translation stage -- guest
 * ("intermediate") physical address to real physical address -- the
 * same two-stage scheme `structo::arch::arm64::stage2_tag` entries (see
 * `pte_stage2.hpp`) are walked under: `HCR_EL2.VM` turns stage-2
 * translation on, `VTCR_EL2` configures its input/output address sizes
 * and granule exactly like `TCR_EL1` does for stage 1, and `VTTBR_EL2`
 * points at the root of the guest's stage-2 table, tagged with a VMID
 * so TLB entries from different guests don't collide. Only the subset
 * of `HCR_EL2` bits most relevant to bringing up or tearing down this
 * translation (plus the handful of MMU-adjacent trap-to-EL2 controls a
 * basic hypervisor needs, e.g. trapping EL1 writes to its own stage-1
 * MMU registers via `TVM`) is modeled; the many interrupt-virtualization
 * bits (`VF`/`VI`/`VSE`/`FB`/...) are out of scope here.
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
 * hcr_el2 hcr{};
 * hcr.set_vm(true).set_rw(true).set_tvm(true); // enable stage 2, EL1 is AArch64, trap EL1 VM-config writes
 *
 * #if defined(__aarch64__)
 * tcr.write();
 * sctlr.write();
 * hcr.write();
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
  using cnp = pte_bit_field<0, 1>;    //!< Bit 0: Common not Private (`FEAT_TTCNP`).
  using baddr = pte_bit_field<1, 47>; //!< Bits [47:1]: translation table base address.
  using asid = pte_bit_field<48, 16>; //!< Bits [63:48]: Address Space ID.
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

/**
 * @brief Named bit-field accessors for `SCR_EL3` (Secure Configuration
 * Register). Only accessible from EL3 ("Monitor" state).
 */
struct scr_el3_bits {
  using ns = pte_bit_field<0, 1>;  //!< Bit 0: Non-secure -- the world the next lower exception level enters.
  using irq = pte_bit_field<1, 1>; //!< Bit 1: physical IRQ routed to EL3 regardless of `PSTATE.I`.
  using fiq = pte_bit_field<2, 1>; //!< Bit 2: physical FIQ routed to EL3 regardless of `PSTATE.F`.
  using ea = pte_bit_field<3, 1>;  //!< Bit 3: External Abort / SError routed to EL3.
  using smd =
      pte_bit_field<7, 1>; //!< Bit 7: Secure Monitor Call Disable (traps `SMC` wherever it would otherwise be usable).
  using hce = pte_bit_field<8, 1>; //!< Bit 8: Hypervisor Call Enable (`HVC` usable).
  using sif = pte_bit_field<9, 1>; //!< Bit 9: Secure Instruction Fetch (forbids Secure fetch from Non-secure memory).
  using rw =
      pte_bit_field<10, 1>; //!< Bit 10: next lower Exception level's execution state is AArch64 (1) or AArch32 (0).
  using st =
      pte_bit_field<11, 1>; //!< Bit 11: Secure EL1 access to the Secure physical timer registers not trapped to EL3.
  using twi = pte_bit_field<12, 1>; //!< Bit 12: `WFI` from EL2/EL1/EL0 traps to EL3.
  using twe = pte_bit_field<13, 1>; //!< Bit 13: `WFE` from EL2/EL1/EL0 traps to EL3.
};

/**
 * @brief Parsed/built view of `SCR_EL3`: the AArch64 analog of ARMv7-A's
 * `SCR` (see `structo::arch::arm::scr`) -- `NS` selects which world
 * (Secure or Non-secure) the next lower exception level runs in, which
 * in turn selects which banked copy of the lower-EL MMU registers
 * (`SCTLR_EL1`, `TTBR0_EL1`/`TTBR1_EL1`, etc.) is live, plus the handful
 * of related exception-routing and `SMC`/`HVC` availability bits.
 */
struct scr_el3 {
  std::uint64_t raw{0};

  [[nodiscard]] static constexpr scr_el3 from_raw(std::uint64_t value) noexcept { return scr_el3{value}; }

  /** @brief `true`: the next lower exception level is Non-secure. `false`: Secure. */
  [[nodiscard]] constexpr bool ns() const noexcept { return scr_el3_bits::ns::test(raw); }
  constexpr scr_el3 &set_ns(bool value) noexcept {
    raw = scr_el3_bits::ns::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool irq_to_el3() const noexcept { return scr_el3_bits::irq::test(raw); }
  constexpr scr_el3 &set_irq_to_el3(bool value) noexcept {
    raw = scr_el3_bits::irq::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool fiq_to_el3() const noexcept { return scr_el3_bits::fiq::test(raw); }
  constexpr scr_el3 &set_fiq_to_el3(bool value) noexcept {
    raw = scr_el3_bits::fiq::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool external_abort_to_el3() const noexcept { return scr_el3_bits::ea::test(raw); }
  constexpr scr_el3 &set_external_abort_to_el3(bool value) noexcept {
    raw = scr_el3_bits::ea::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool secure_monitor_call_disabled() const noexcept { return scr_el3_bits::smd::test(raw); }
  constexpr scr_el3 &set_secure_monitor_call_disabled(bool value) noexcept {
    raw = scr_el3_bits::smd::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool hyp_call_enabled() const noexcept { return scr_el3_bits::hce::test(raw); }
  constexpr scr_el3 &set_hyp_call_enabled(bool value) noexcept {
    raw = scr_el3_bits::hce::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool secure_instruction_fetch() const noexcept { return scr_el3_bits::sif::test(raw); }
  constexpr scr_el3 &set_secure_instruction_fetch(bool value) noexcept {
    raw = scr_el3_bits::sif::set_bit(raw, value);
    return *this;
  }

  /** @brief `true`: the next lower Exception level's execution state is AArch64. `false`: AArch32. */
  [[nodiscard]] constexpr bool rw() const noexcept { return scr_el3_bits::rw::test(raw); }
  constexpr scr_el3 &set_rw(bool value) noexcept {
    raw = scr_el3_bits::rw::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool secure_el1_timer_access() const noexcept { return scr_el3_bits::st::test(raw); }
  constexpr scr_el3 &set_secure_el1_timer_access(bool value) noexcept {
    raw = scr_el3_bits::st::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool trap_wfi_to_el3() const noexcept { return scr_el3_bits::twi::test(raw); }
  constexpr scr_el3 &set_trap_wfi_to_el3(bool value) noexcept {
    raw = scr_el3_bits::twi::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool trap_wfe_to_el3() const noexcept { return scr_el3_bits::twe::test(raw); }
  constexpr scr_el3 &set_trap_wfe_to_el3(bool value) noexcept {
    raw = scr_el3_bits::twe::set_bit(raw, value);
    return *this;
  }

#if defined(__aarch64__)
  /** @brief Reads the live `SCR_EL3` register. EL3-only; not unit tested. */
  [[nodiscard]] static scr_el3 read() noexcept {
    std::uint64_t value;
    asm volatile("mrs %0, scr_el3" : "=r"(value));
    return scr_el3{value};
  }

  /** @brief Writes `raw` to the live `SCR_EL3` register. EL3-only; not unit tested. */
  void write() const noexcept { asm volatile("msr scr_el3, %0" ::"r"(raw) : "memory"); }
#endif // defined(__aarch64__)
};

/**
 * @brief Named bit-field accessors for `HCR_EL2` (Hypervisor
 * Configuration Register). Only the subset most relevant to bringing up
 * or tearing down stage-2 translation and basic EL1-trapping is
 * modeled; the many interrupt-virtualization bits (`VF`/`VI`/`VSE`/
 * `FB`/...) are out of scope here.
 */
struct hcr_el2_bits {
  using vm = pte_bit_field<0, 1>; //!< Bit 0: enable stage-2 translation.
  using swio =
      pte_bit_field<1, 1>; //!< Bit 1: Set/Way Invalidation Override (EL1 set/way cache ops behave as invalidate-only).
  using ptw = pte_bit_field<2, 1>; //!< Bit 2: Protected Table Walk (a stage-1 walk stepping on a stage-2 Device mapping
                                   //!< faults).
  using fmo = pte_bit_field<3, 1>; //!< Bit 3: physical FIQ routed to EL2.
  using imo = pte_bit_field<4, 1>; //!< Bit 4: physical IRQ routed to EL2.
  using amo = pte_bit_field<5, 1>; //!< Bit 5: physical SError routed to EL2.
  using dc =
      pte_bit_field<12, 1>; //!< Bit 12: Default Cacheability (forces cacheable when EL0/EL1 stage-1 is disabled).
  using bsu = pte_bit_field<10, 2>;  //!< Bits [11:10]: Barrier Shareability Upgrade for EL1/EL0 DSB/DMB.
  using twi = pte_bit_field<13, 1>;  //!< Bit 13: `WFI` from EL1/EL0 traps to EL2.
  using twe = pte_bit_field<14, 1>;  //!< Bit 14: `WFE` from EL1/EL0 traps to EL2.
  using tsc = pte_bit_field<19, 1>;  //!< Bit 19: `SMC` from EL1 traps to EL2.
  using ttlb = pte_bit_field<25, 1>; //!< Bit 25: EL1 TLB maintenance instructions trap to EL2.
  using tvm = pte_bit_field<26, 1>;  //!< Bit 26: EL1 writes to its own stage-1 MMU control registers trap to EL2.
  using tge = pte_bit_field<27, 1>;  //!< Bit 27: Trap General Exceptions -- routes EL0 exceptions to EL2 and disables
                                     //!< EL1&0 stage-1 translation for EL0.
  using tdz = pte_bit_field<28, 1>;  //!< Bit 28: `DC ZVA` from EL1/EL0 traps to EL2.
  using hcd =
      pte_bit_field<29, 1>; //!< Bit 29: Hypervisor Call Disable (traps `HVC` wherever it would otherwise be usable).
  using trvm = pte_bit_field<30, 1>; //!< Bit 30: EL1 reads of its own stage-1 MMU control registers trap to EL2.
  using rw = pte_bit_field<31, 1>;   //!< Bit 31: EL1's execution state is AArch64 (1) or AArch32 (0).
  using cd = pte_bit_field<32, 1>;   //!< Bit 32: stage-1 Data Cacheability Disable for the EL2&0 regime (`FEAT_VHE`,
                                     //!< `E2H=1` only).
  using id = pte_bit_field<33, 1>;   //!< Bit 33: stage-1 Instruction Cacheability Disable for the EL2&0 regime
                                     //!< (`FEAT_VHE`, `E2H=1` only).
  using e2h =
      pte_bit_field<34, 1>; //!< Bit 34: Enable EL2 Host (`FEAT_VHE`) -- EL2 runs the "EL2&0" regime instead of its own.
};

/**
 * @brief Parsed/built view of `HCR_EL2`: whether stage-2 translation is
 * enabled (`VM`), which EL1 exception level's execution state and MMU-
 * configuration accesses trap to the hypervisor, and the related
 * barrier/cache-maintenance override bits a basic Type-1/Type-2
 * hypervisor needs when bringing a guest's stage-2 mapping up or down.
 */
struct hcr_el2 {
  std::uint64_t raw{0};

  [[nodiscard]] static constexpr hcr_el2 from_raw(std::uint64_t value) noexcept { return hcr_el2{value}; }

  [[nodiscard]] constexpr bool vm() const noexcept { return hcr_el2_bits::vm::test(raw); }
  constexpr hcr_el2 &set_vm(bool value) noexcept {
    raw = hcr_el2_bits::vm::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool set_way_invalidation_override() const noexcept { return hcr_el2_bits::swio::test(raw); }
  constexpr hcr_el2 &set_set_way_invalidation_override(bool value) noexcept {
    raw = hcr_el2_bits::swio::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool protected_table_walk() const noexcept { return hcr_el2_bits::ptw::test(raw); }
  constexpr hcr_el2 &set_protected_table_walk(bool value) noexcept {
    raw = hcr_el2_bits::ptw::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool fiq_to_el2() const noexcept { return hcr_el2_bits::fmo::test(raw); }
  constexpr hcr_el2 &set_fiq_to_el2(bool value) noexcept {
    raw = hcr_el2_bits::fmo::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool irq_to_el2() const noexcept { return hcr_el2_bits::imo::test(raw); }
  constexpr hcr_el2 &set_irq_to_el2(bool value) noexcept {
    raw = hcr_el2_bits::imo::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool serror_to_el2() const noexcept { return hcr_el2_bits::amo::test(raw); }
  constexpr hcr_el2 &set_serror_to_el2(bool value) noexcept {
    raw = hcr_el2_bits::amo::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool default_cacheability() const noexcept { return hcr_el2_bits::dc::test(raw); }
  constexpr hcr_el2 &set_default_cacheability(bool value) noexcept {
    raw = hcr_el2_bits::dc::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned barrier_shareability_upgrade() const noexcept {
    return static_cast<unsigned>(hcr_el2_bits::bsu::get(raw));
  }
  constexpr hcr_el2 &set_barrier_shareability_upgrade(unsigned value) noexcept {
    raw = hcr_el2_bits::bsu::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool trap_wfi() const noexcept { return hcr_el2_bits::twi::test(raw); }
  constexpr hcr_el2 &set_trap_wfi(bool value) noexcept {
    raw = hcr_el2_bits::twi::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool trap_wfe() const noexcept { return hcr_el2_bits::twe::test(raw); }
  constexpr hcr_el2 &set_trap_wfe(bool value) noexcept {
    raw = hcr_el2_bits::twe::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool trap_smc() const noexcept { return hcr_el2_bits::tsc::test(raw); }
  constexpr hcr_el2 &set_trap_smc(bool value) noexcept {
    raw = hcr_el2_bits::tsc::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool trap_tlb_maintenance() const noexcept { return hcr_el2_bits::ttlb::test(raw); }
  constexpr hcr_el2 &set_trap_tlb_maintenance(bool value) noexcept {
    raw = hcr_el2_bits::ttlb::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool tvm() const noexcept { return hcr_el2_bits::tvm::test(raw); }
  constexpr hcr_el2 &set_tvm(bool value) noexcept {
    raw = hcr_el2_bits::tvm::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool trap_general_exceptions() const noexcept { return hcr_el2_bits::tge::test(raw); }
  constexpr hcr_el2 &set_trap_general_exceptions(bool value) noexcept {
    raw = hcr_el2_bits::tge::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool trap_dc_zva() const noexcept { return hcr_el2_bits::tdz::test(raw); }
  constexpr hcr_el2 &set_trap_dc_zva(bool value) noexcept {
    raw = hcr_el2_bits::tdz::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool hyp_call_disabled() const noexcept { return hcr_el2_bits::hcd::test(raw); }
  constexpr hcr_el2 &set_hyp_call_disabled(bool value) noexcept {
    raw = hcr_el2_bits::hcd::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool trap_vm_reads() const noexcept { return hcr_el2_bits::trvm::test(raw); }
  constexpr hcr_el2 &set_trap_vm_reads(bool value) noexcept {
    raw = hcr_el2_bits::trvm::set_bit(raw, value);
    return *this;
  }

  /** @brief `true`: EL1's execution state is AArch64. `false`: AArch32. */
  [[nodiscard]] constexpr bool rw() const noexcept { return hcr_el2_bits::rw::test(raw); }
  constexpr hcr_el2 &set_rw(bool value) noexcept {
    raw = hcr_el2_bits::rw::set_bit(raw, value);
    return *this;
  }

  /** @brief `FEAT_VHE`, `E2H=1` only: stage-1 data-cacheability disable for the EL2&0 regime. */
  [[nodiscard]] constexpr bool cacheability_disabled() const noexcept { return hcr_el2_bits::cd::test(raw); }
  constexpr hcr_el2 &set_cacheability_disabled(bool value) noexcept {
    raw = hcr_el2_bits::cd::set_bit(raw, value);
    return *this;
  }

  /** @brief `FEAT_VHE`, `E2H=1` only: stage-1 instruction-cacheability disable for the EL2&0 regime. */
  [[nodiscard]] constexpr bool instruction_cacheability_disabled() const noexcept {
    return hcr_el2_bits::id::test(raw);
  }
  constexpr hcr_el2 &set_instruction_cacheability_disabled(bool value) noexcept {
    raw = hcr_el2_bits::id::set_bit(raw, value);
    return *this;
  }

  /** @brief `FEAT_VHE`: `true` makes EL2 run the "EL2&0" regime (Linux's `TTBR0_EL2`/`TTBR1_EL2` style), not plain EL2.
   */
  [[nodiscard]] constexpr bool e2h() const noexcept { return hcr_el2_bits::e2h::test(raw); }
  constexpr hcr_el2 &set_e2h(bool value) noexcept {
    raw = hcr_el2_bits::e2h::set_bit(raw, value);
    return *this;
  }

#if defined(__aarch64__)
  /** @brief Reads the live `HCR_EL2` register. Privileged (EL2); not unit tested. */
  [[nodiscard]] static hcr_el2 read() noexcept {
    std::uint64_t value;
    asm volatile("mrs %0, hcr_el2" : "=r"(value));
    return hcr_el2{value};
  }

  /** @brief Writes `raw` to the live `HCR_EL2` register. Privileged (EL2); not unit tested. */
  void write() const noexcept { asm volatile("msr hcr_el2, %0" ::"r"(raw) : "memory"); }
#endif // defined(__aarch64__)
};

/** @brief Named bit-field accessors for `VTCR_EL2` (Virtualization Translation Control Register). */
struct vtcr_el2_bits {
  using t0sz = pte_bit_field<0, 6>;   //!< Bits [5:0]: stage-2 input (guest-physical) address size, `64 - T0SZ`.
  using sl0 = pte_bit_field<6, 2>;    //!< Bits [7:6]: stage-2 starting level of translation.
  using irgn0 = pte_bit_field<8, 2>;  //!< Bits [9:8]: stage-2 table-walk inner cacheability.
  using orgn0 = pte_bit_field<10, 2>; //!< Bits [11:10]: stage-2 table-walk outer cacheability.
  using sh0 = pte_bit_field<12, 2>;   //!< Bits [13:12]: stage-2 table-walk shareability.
  using tg0 = pte_bit_field<14, 2>;   //!< Bits [15:14]: stage-2 granule (0=4KB, 2=16KB, 1=64KB).
  using ps =
      pte_bit_field<16, 3>; //!< Bits [18:16]: stage-2 output (physical) address size, same encoding as `TCR_EL1.IPS`.
  using vs = pte_bit_field<19, 1>; //!< Bit 19: VMID size (0=8-bit, 1=16-bit; `FEAT_VMID16`).
  using ha = pte_bit_field<21, 1>; //!< Bit 21: hardware management of the stage-2 Access flag (`FEAT_HAFDBS`).
  using hd = pte_bit_field<22, 1>; //!< Bit 22: hardware management of stage-2 dirty state (`FEAT_HAFDBS`).
};

/**
 * @brief Parsed/built view of `VTCR_EL2`: input/output address sizes,
 * granule, and cacheability/shareability attributes for stage-2
 * (guest-physical-to-physical) translation -- the stage-2 counterpart of
 * `TCR_EL1`.
 */
struct vtcr_el2 {
  std::uint64_t raw{0};

  [[nodiscard]] static constexpr vtcr_el2 from_raw(std::uint64_t value) noexcept { return vtcr_el2{value}; }

  [[nodiscard]] constexpr unsigned t0sz() const noexcept {
    return static_cast<unsigned>(vtcr_el2_bits::t0sz::get(raw));
  }
  constexpr vtcr_el2 &set_t0sz(unsigned value) noexcept {
    raw = vtcr_el2_bits::t0sz::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned sl0() const noexcept { return static_cast<unsigned>(vtcr_el2_bits::sl0::get(raw)); }
  constexpr vtcr_el2 &set_sl0(unsigned value) noexcept {
    raw = vtcr_el2_bits::sl0::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned irgn0() const noexcept {
    return static_cast<unsigned>(vtcr_el2_bits::irgn0::get(raw));
  }
  constexpr vtcr_el2 &set_irgn0(unsigned value) noexcept {
    raw = vtcr_el2_bits::irgn0::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned orgn0() const noexcept {
    return static_cast<unsigned>(vtcr_el2_bits::orgn0::get(raw));
  }
  constexpr vtcr_el2 &set_orgn0(unsigned value) noexcept {
    raw = vtcr_el2_bits::orgn0::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned sh0() const noexcept { return static_cast<unsigned>(vtcr_el2_bits::sh0::get(raw)); }
  constexpr vtcr_el2 &set_sh0(unsigned value) noexcept {
    raw = vtcr_el2_bits::sh0::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned tg0() const noexcept { return static_cast<unsigned>(vtcr_el2_bits::tg0::get(raw)); }
  constexpr vtcr_el2 &set_tg0(unsigned value) noexcept {
    raw = vtcr_el2_bits::tg0::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned ps() const noexcept { return static_cast<unsigned>(vtcr_el2_bits::ps::get(raw)); }
  constexpr vtcr_el2 &set_ps(unsigned value) noexcept {
    raw = vtcr_el2_bits::ps::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool vmid_16bit() const noexcept { return vtcr_el2_bits::vs::test(raw); }
  constexpr vtcr_el2 &set_vmid_16bit(bool value) noexcept {
    raw = vtcr_el2_bits::vs::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool hw_access_flag() const noexcept { return vtcr_el2_bits::ha::test(raw); }
  constexpr vtcr_el2 &set_hw_access_flag(bool value) noexcept {
    raw = vtcr_el2_bits::ha::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool hw_dirty_state() const noexcept { return vtcr_el2_bits::hd::test(raw); }
  constexpr vtcr_el2 &set_hw_dirty_state(bool value) noexcept {
    raw = vtcr_el2_bits::hd::set_bit(raw, value);
    return *this;
  }

#if defined(__aarch64__)
  /** @brief Reads the live `VTCR_EL2` register. Privileged (EL2); not unit tested. */
  [[nodiscard]] static vtcr_el2 read() noexcept {
    std::uint64_t value;
    asm volatile("mrs %0, vtcr_el2" : "=r"(value));
    return vtcr_el2{value};
  }

  /** @brief Writes `raw` to the live `VTCR_EL2` register. Privileged (EL2); not unit tested. */
  void write() const noexcept { asm volatile("msr vtcr_el2, %0" ::"r"(raw) : "memory"); }
#endif // defined(__aarch64__)
};

/** @brief Named bit-field accessors for `VTTBR_EL2` (Virtualization Translation Table Base Register). */
struct vttbr_el2_bits {
  using cnp = pte_bit_field<0, 1>;    //!< Bit 0: Common not Private (`FEAT_TTCNP`).
  using baddr = pte_bit_field<1, 47>; //!< Bits [47:1]: stage-2 translation table base address.
  using vmid = pte_bit_field<48, 16>; //!< Bits [63:48]: Virtual Machine ID (width is 8 or 16 bits per `VTCR_EL2.VS`;
                                      //!< unused high bits read as `0`).
};

/**
 * @brief Parsed/built view of `VTTBR_EL2`: the root of the current
 * guest's stage-2 translation table, tagged with a VMID so stage-2 TLB
 * entries from different guests don't collide -- the stage-2 counterpart
 * of `TTBR0_EL1`.
 */
struct vttbr_el2 {
  std::uint64_t raw{0};

  [[nodiscard]] static constexpr vttbr_el2 from_raw(std::uint64_t value) noexcept { return vttbr_el2{value}; }

  [[nodiscard]] constexpr bool common_not_private() const noexcept { return vttbr_el2_bits::cnp::test(raw); }
  constexpr vttbr_el2 &set_common_not_private(bool value) noexcept {
    raw = vttbr_el2_bits::cnp::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr std::uint64_t base_addr() const noexcept { return vttbr_el2_bits::baddr::get(raw) << 1; }
  /** @brief Sets the stage-2 translation table base address. `addr`'s bit 0 must be `0` (masked off regardless). */
  constexpr vttbr_el2 &set_base_addr(std::uint64_t addr) noexcept {
    raw = vttbr_el2_bits::baddr::set(raw, addr >> 1);
    return *this;
  }

  [[nodiscard]] constexpr std::uint16_t vmid() const noexcept {
    return static_cast<std::uint16_t>(vttbr_el2_bits::vmid::get(raw));
  }
  constexpr vttbr_el2 &set_vmid(std::uint16_t value) noexcept {
    raw = vttbr_el2_bits::vmid::set(raw, value);
    return *this;
  }

#if defined(__aarch64__)
  /** @brief Reads the live `VTTBR_EL2` register. Privileged (EL2); not unit tested. */
  [[nodiscard]] static vttbr_el2 read() noexcept {
    std::uint64_t value;
    asm volatile("mrs %0, vttbr_el2" : "=r"(value));
    return vttbr_el2{value};
  }

  /** @brief Writes `raw` to the live `VTTBR_EL2` register. Privileged (EL2); not unit tested. */
  void write() const noexcept { asm volatile("msr vttbr_el2, %0" ::"r"(raw) : "memory"); }
#endif // defined(__aarch64__)
};

} // namespace structo::arch::arm64
