// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file mmu_regs.hpp
 * @brief Parsers/builders for ARMv7-A/AArch32 MMU control registers:
 * `SCTLR`, `TTBCR` (shared between the classic short-descriptor format
 * and LPAE), `TTBR0`/`TTBR1` (both formats), `CONTEXTIDR`, the
 * TrustZone security-state configuration registers `SCR` and `NSACR`,
 * and the Virtualization Extensions' hypervisor/stage-2 registers
 * `HCR`, `VTCR`, and `VTTBR`.
 *
 * ## TrustZone and MMU register banking
 *
 * On a TrustZone-capable core, `SCTLR`, `TTBCR`, `TTBR0`, `TTBR1`, and
 * `CONTEXTIDR` are all **banked**: the Secure and Non-secure worlds each
 * have their own private copy, and a plain `MRC`/`MCR` from this header's
 * other accessors always reads/writes whichever copy belongs to the
 * world currently executing -- there is no separate "Secure TTBR0"
 * value type here, because the instruction encoding is identical and
 * only the current `SCR.NS` value (readable only from Secure state,
 * typically Monitor mode) decides which bank is touched. `scr`/`nsacr`
 * are themselves Secure-only (`MRC`/`MCR` to them from Non-secure state
 * traps), and model the two registers Secure/Monitor-mode software uses
 * to configure that world switch and what the Non-secure side is allowed
 * to touch.
 *
 * ## Hypervisor stage-2 translation: `HCR`/`VTCR`/`VTTBR`
 *
 * The (Non-secure-only) Virtualization Extensions add a *second*
 * translation stage at the Hyp (PL2) exception level -- guest
 * ("intermediate") physical address to real physical address, the same
 * two-stage scheme `structo::arch::arm::lpae::stage2_tag` entries (see
 * `pte_stage2.hpp`) are walked under. `HCR.VM` turns stage-2 translation
 * on, `VTCR` configures its input/output address size and
 * cacheability/shareability exactly like `TTBCR_lpae` does for stage 1,
 * and `VTTBR` (a 64-bit register, read/written as a pair via
 * `MRRC`/`MCRR`, like LPAE's `TTBR0`/`TTBR1`) points at the root of the
 * guest's stage-2 table, tagged with a VMID. Only the subset of `HCR`
 * bits most relevant to bringing up or tearing down this translation
 * (plus the handful of MMU-adjacent trap-to-Hyp controls a basic
 * hypervisor needs, e.g. trapping EL1/PL1 writes to its own stage-1 MMU
 * registers via `TVM`) is modeled; the many interrupt-virtualization
 * bits are out of scope here.
 *
 * ## Scope and split
 *
 * As with the AArch64 counterpart (`structo/arch/arm64/mmu_regs.hpp`),
 * each register gets a small value type wrapping the raw register
 * image, with named bit accessors built on `structo::arch::pte_bit_field`.
 * This parse/build logic is pure bit arithmetic, fully host-testable.
 *
 * `read()`/`write()` round-trip a value type through the real coprocessor
 * register via `MRC`/`MCR p15` (or, for the 64-bit LPAE `TTBR0`/`TTBR1`
 * and `VTTBR`, `MRRC`/`MCRR p15`), compiled only for a genuine 32-bit ARM
 * target (`__arm__`, and not AArch64) -- on any other host this header
 * still defines every value type (cross-compiled header checks stay
 * clean), it just omits `read()`/`write()`. This is privileged access
 * and cannot be exercised from an unprivileged test process; only the
 * pure parse/build logic is unit tested.
 *
 * ## Short-descriptor vs. LPAE
 *
 * `TTBCR.EAE` (bit 31) selects which format `TTBCR`'s lower bits (and
 * `TTBR0`/`TTBR1`'s layout) use: `0` is the classic short-descriptor
 * format (`ttbcr_short`, 32-bit `TTBR0`/`TTBR1` base address via
 * `ttbr_short`), `1` is LPAE (`ttbcr_lpae`, 64-bit `TTBR0`/`TTBR1` base
 * address + ASID via `ttbr_lpae`). Both `ttbcr_*` views are provided as
 * distinct types rather than one type exposing both interpretations,
 * since which fields are meaningful depends entirely on `EAE` and mixing
 * them in one type would make it easy to read a field that doesn't
 * apply to the active format.
 *
 * ## Example
 *
 * @code
 * using namespace structo::arch::arm;
 *
 * ttbcr_lpae tcr{};
 * tcr.set_t0sz(0).set_t1sz(0).set_eae(true);
 *
 * sctlr sc{};
 * sc.set_mmu_enabled(true).set_dcache_enabled(true).set_icache_enabled(true);
 *
 * scr s{};
 * s.set_ns(true).set_hyp_call_enabled(true); // switch the next world to Non-secure, allow HVC
 *
 * hcr h{};
 * h.set_vm(true).set_tvm(true); // enable stage 2, trap EL1 VM-config writes to Hyp mode
 *
 * #if defined(__arm__) && !defined(__aarch64__)
 * tcr.write();
 * sc.write();
 * s.write();
 * h.write();
 * #endif
 * @endcode
 */

#include <structo/arch/pte_field.hpp>

#include <cstdint>

namespace structo::arch::arm {

/** @brief Named bit-field accessors for `SCTLR`. */
struct sctlr_bits {
  using m = pte_bit_field<0, 1, std::uint32_t>;  //!< Bit 0: MMU enable.
  using a = pte_bit_field<1, 1, std::uint32_t>;  //!< Bit 1: alignment check enable.
  using c = pte_bit_field<2, 1, std::uint32_t>;  //!< Bit 2: (data) cache enable.
  using i = pte_bit_field<12, 1, std::uint32_t>; //!< Bit 12: instruction cache enable.
};

/**
 * @brief Parsed/built view of `SCTLR`, the ARMv7-A System Control
 * Register. Only the bits most relevant to bringing up or tearing down
 * a translation are modeled; every other bit of `raw` is preserved
 * untouched by the named accessors below.
 */
struct sctlr {
  std::uint32_t raw{0};

  [[nodiscard]] static constexpr sctlr from_raw(std::uint32_t value) noexcept { return sctlr{value}; }

  [[nodiscard]] constexpr bool mmu_enabled() const noexcept { return sctlr_bits::m::test(raw); }
  constexpr sctlr &set_mmu_enabled(bool value) noexcept {
    raw = sctlr_bits::m::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool alignment_check_enabled() const noexcept { return sctlr_bits::a::test(raw); }
  constexpr sctlr &set_alignment_check_enabled(bool value) noexcept {
    raw = sctlr_bits::a::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool dcache_enabled() const noexcept { return sctlr_bits::c::test(raw); }
  constexpr sctlr &set_dcache_enabled(bool value) noexcept {
    raw = sctlr_bits::c::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool icache_enabled() const noexcept { return sctlr_bits::i::test(raw); }
  constexpr sctlr &set_icache_enabled(bool value) noexcept {
    raw = sctlr_bits::i::set_bit(raw, value);
    return *this;
  }

#if defined(__arm__) && !defined(__aarch64__)
  /** @brief Reads the live `SCTLR` register. Privileged; not unit tested. */
  [[nodiscard]] static sctlr read() noexcept {
    std::uint32_t value;
    asm volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(value));
    return sctlr{value};
  }

  /** @brief Writes `raw` to the live `SCTLR` register. Privileged; not unit tested. */
  void write() const noexcept { asm volatile("mcr p15, 0, %0, c1, c0, 0" ::"r"(raw) : "memory"); }
#endif // defined(__arm__) && !defined(__aarch64__)
};

/** @brief Named bit-field accessors for `TTBCR` in short-descriptor mode (`EAE == 0`). */
struct ttbcr_short_bits {
  using n = pte_bit_field<0, 3, std::uint32_t>;   //!< Bits [2:0]: TTBR0 boundary size (0 = full 4GB).
  using pd0 = pte_bit_field<4, 1, std::uint32_t>; //!< Bit 4: disable TTBR0 table walks.
  using pd1 = pte_bit_field<5, 1, std::uint32_t>; //!< Bit 5: disable TTBR1 table walks.
  using eae = pte_bit_field<31, 1, std::uint32_t>; //!< Bit 31: Extended Address Enable (selects LPAE when set).
};

/** @brief Parsed/built view of `TTBCR` while operating in classic short-descriptor mode (`EAE == 0`). */
struct ttbcr_short {
  std::uint32_t raw{0};

  [[nodiscard]] static constexpr ttbcr_short from_raw(std::uint32_t value) noexcept { return ttbcr_short{value}; }

  [[nodiscard]] constexpr unsigned n() const noexcept { return ttbcr_short_bits::n::get(raw); }
  constexpr ttbcr_short &set_n(unsigned value) noexcept {
    raw = ttbcr_short_bits::n::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool pd0() const noexcept { return ttbcr_short_bits::pd0::test(raw); }
  constexpr ttbcr_short &set_pd0(bool value) noexcept {
    raw = ttbcr_short_bits::pd0::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool pd1() const noexcept { return ttbcr_short_bits::pd1::test(raw); }
  constexpr ttbcr_short &set_pd1(bool value) noexcept {
    raw = ttbcr_short_bits::pd1::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool eae() const noexcept { return ttbcr_short_bits::eae::test(raw); }
  constexpr ttbcr_short &set_eae(bool value) noexcept {
    raw = ttbcr_short_bits::eae::set_bit(raw, value);
    return *this;
  }

#if defined(__arm__) && !defined(__aarch64__)
  /** @brief Reads the live `TTBCR` register. Privileged; not unit tested. */
  [[nodiscard]] static ttbcr_short read() noexcept {
    std::uint32_t value;
    asm volatile("mrc p15, 0, %0, c2, c0, 2" : "=r"(value));
    return ttbcr_short{value};
  }

  /** @brief Writes `raw` to the live `TTBCR` register. Privileged; not unit tested. */
  void write() const noexcept { asm volatile("mcr p15, 0, %0, c2, c0, 2" ::"r"(raw) : "memory"); }
#endif // defined(__arm__) && !defined(__aarch64__)
};

/** @brief Named bit-field accessors for `TTBCR` in LPAE mode (`EAE == 1`). */
struct ttbcr_lpae_bits {
  using t0sz = pte_bit_field<0, 3, std::uint32_t>;   //!< Bits [2:0]: TTBR0 input-address size.
  using epd0 = pte_bit_field<7, 1, std::uint32_t>;   //!< Bit 7: disable TTBR0 table walks.
  using irgn0 = pte_bit_field<8, 2, std::uint32_t>;  //!< Bits [9:8]: TTBR0 inner cacheability.
  using orgn0 = pte_bit_field<10, 2, std::uint32_t>; //!< Bits [11:10]: TTBR0 outer cacheability.
  using sh0 = pte_bit_field<12, 2, std::uint32_t>;   //!< Bits [13:12]: TTBR0 shareability.
  using t1sz = pte_bit_field<16, 3, std::uint32_t>;  //!< Bits [18:16]: TTBR1 input-address size.
  using a1 = pte_bit_field<22, 1, std::uint32_t>;    //!< Bit 22: which TTBR supplies the ASID.
  using epd1 = pte_bit_field<23, 1, std::uint32_t>;  //!< Bit 23: disable TTBR1 table walks.
  using irgn1 = pte_bit_field<24, 2, std::uint32_t>; //!< Bits [25:24]: TTBR1 inner cacheability.
  using orgn1 = pte_bit_field<26, 2, std::uint32_t>; //!< Bits [27:26]: TTBR1 outer cacheability.
  using sh1 = pte_bit_field<28, 2, std::uint32_t>;   //!< Bits [29:28]: TTBR1 shareability.
  using eae = pte_bit_field<31, 1, std::uint32_t>;   //!< Bit 31: Extended Address Enable (must be 1 for this view).
};

/** @brief Parsed/built view of `TTBCR` while operating in LPAE mode (`EAE == 1`). */
struct ttbcr_lpae {
  std::uint32_t raw{0};

  [[nodiscard]] static constexpr ttbcr_lpae from_raw(std::uint32_t value) noexcept { return ttbcr_lpae{value}; }

  [[nodiscard]] constexpr unsigned t0sz() const noexcept { return ttbcr_lpae_bits::t0sz::get(raw); }
  constexpr ttbcr_lpae &set_t0sz(unsigned value) noexcept {
    raw = ttbcr_lpae_bits::t0sz::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned t1sz() const noexcept { return ttbcr_lpae_bits::t1sz::get(raw); }
  constexpr ttbcr_lpae &set_t1sz(unsigned value) noexcept {
    raw = ttbcr_lpae_bits::t1sz::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool epd0() const noexcept { return ttbcr_lpae_bits::epd0::test(raw); }
  constexpr ttbcr_lpae &set_epd0(bool value) noexcept {
    raw = ttbcr_lpae_bits::epd0::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool epd1() const noexcept { return ttbcr_lpae_bits::epd1::test(raw); }
  constexpr ttbcr_lpae &set_epd1(bool value) noexcept {
    raw = ttbcr_lpae_bits::epd1::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned irgn0() const noexcept { return ttbcr_lpae_bits::irgn0::get(raw); }
  constexpr ttbcr_lpae &set_irgn0(unsigned value) noexcept {
    raw = ttbcr_lpae_bits::irgn0::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned orgn0() const noexcept { return ttbcr_lpae_bits::orgn0::get(raw); }
  constexpr ttbcr_lpae &set_orgn0(unsigned value) noexcept {
    raw = ttbcr_lpae_bits::orgn0::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned sh0() const noexcept { return ttbcr_lpae_bits::sh0::get(raw); }
  constexpr ttbcr_lpae &set_sh0(unsigned value) noexcept {
    raw = ttbcr_lpae_bits::sh0::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned irgn1() const noexcept { return ttbcr_lpae_bits::irgn1::get(raw); }
  constexpr ttbcr_lpae &set_irgn1(unsigned value) noexcept {
    raw = ttbcr_lpae_bits::irgn1::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned orgn1() const noexcept { return ttbcr_lpae_bits::orgn1::get(raw); }
  constexpr ttbcr_lpae &set_orgn1(unsigned value) noexcept {
    raw = ttbcr_lpae_bits::orgn1::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned sh1() const noexcept { return ttbcr_lpae_bits::sh1::get(raw); }
  constexpr ttbcr_lpae &set_sh1(unsigned value) noexcept {
    raw = ttbcr_lpae_bits::sh1::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool asid_from_ttbr1() const noexcept { return ttbcr_lpae_bits::a1::test(raw); }
  constexpr ttbcr_lpae &set_asid_from_ttbr1(bool value) noexcept {
    raw = ttbcr_lpae_bits::a1::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool eae() const noexcept { return ttbcr_lpae_bits::eae::test(raw); }
  constexpr ttbcr_lpae &set_eae(bool value) noexcept {
    raw = ttbcr_lpae_bits::eae::set_bit(raw, value);
    return *this;
  }

#if defined(__arm__) && !defined(__aarch64__)
  /** @brief Reads the live `TTBCR` register. Privileged; not unit tested. */
  [[nodiscard]] static ttbcr_lpae read() noexcept {
    std::uint32_t value;
    asm volatile("mrc p15, 0, %0, c2, c0, 2" : "=r"(value));
    return ttbcr_lpae{value};
  }

  /** @brief Writes `raw` to the live `TTBCR` register. Privileged; not unit tested. */
  void write() const noexcept { asm volatile("mcr p15, 0, %0, c2, c0, 2" ::"r"(raw) : "memory"); }
#endif // defined(__arm__) && !defined(__aarch64__)
};

/** @brief Named bit-field accessors for `TTBR0`/`TTBR1` in classic short-descriptor mode. */
struct ttbr_short_bits {
  using baddr = pte_bit_field<14, 18, std::uint32_t>; //!< Bits [31:14]: translation table base address.
};

/**
 * @brief Parsed/built view of `TTBR0`/`TTBR1` in classic short-descriptor
 * mode: a 32-bit register holding only the base address (bits [13:0]
 * are reserved/implementation-defined cacheability hints this type does
 * not model). `Which` only selects which `MRC`/`MCR` mnemonic
 * `read()`/`write()` use.
 */
template <bool IsTtbr1> struct ttbr_short {
  std::uint32_t raw{0};

  [[nodiscard]] static constexpr ttbr_short from_raw(std::uint32_t value) noexcept { return ttbr_short{value}; }

  [[nodiscard]] constexpr std::uint32_t base_addr() const noexcept {
    return ttbr_short_bits::baddr::get(raw) << 14;
  }
  /** @brief Sets the translation table base address. `addr`'s low 14 bits must be `0` (masked off regardless). */
  constexpr ttbr_short &set_base_addr(std::uint32_t addr) noexcept {
    raw = ttbr_short_bits::baddr::set(raw, addr >> 14);
    return *this;
  }

#if defined(__arm__) && !defined(__aarch64__)
  /** @brief Reads the live register. Privileged; not unit tested. */
  [[nodiscard]] static ttbr_short read() noexcept {
    std::uint32_t value;
    if constexpr (IsTtbr1) {
      asm volatile("mrc p15, 0, %0, c2, c0, 1" : "=r"(value));
    } else {
      asm volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(value));
    }
    return ttbr_short{value};
  }

  /** @brief Writes `raw` to the live register. Privileged; not unit tested. */
  void write() const noexcept {
    if constexpr (IsTtbr1) {
      asm volatile("mcr p15, 0, %0, c2, c0, 1" ::"r"(raw) : "memory");
    } else {
      asm volatile("mcr p15, 0, %0, c2, c0, 0" ::"r"(raw) : "memory");
    }
  }
#endif // defined(__arm__) && !defined(__aarch64__)
};

/** @brief `TTBR0` in classic short-descriptor mode. */
using ttbr0_short = ttbr_short<false>;
/** @brief `TTBR1` in classic short-descriptor mode. */
using ttbr1_short = ttbr_short<true>;

/** @brief Named bit-field accessors shared by `TTBR0`/`TTBR1` in LPAE mode (64-bit, accessed via `MRRC`/`MCRR`). */
struct ttbr_lpae_bits {
  using baddr = pte_bit_field<1, 39>; //!< Bits [39:1]: translation table base address.
  using asid = pte_bit_field<48, 8>;  //!< Bits [55:48]: Address Space ID.
};

/**
 * @brief Parsed/built view shared by `TTBR0`/`TTBR1` in LPAE mode: a
 * 64-bit register (read/written as a pair via `MRRC`/`MCRR`) holding the
 * translation table base address and, conventionally, the ASID in
 * `TTBR0`. `Which` only selects which coprocessor opcode `read()`/
 * `write()` use; the bit layout is identical for both registers.
 */
template <bool IsTtbr1> struct ttbr_lpae {
  std::uint64_t raw{0};

  [[nodiscard]] static constexpr ttbr_lpae from_raw(std::uint64_t value) noexcept { return ttbr_lpae{value}; }

  [[nodiscard]] constexpr std::uint64_t base_addr() const noexcept { return ttbr_lpae_bits::baddr::get(raw) << 1; }
  /** @brief Sets the translation table base address. `addr`'s bit 0 must be `0` (masked off regardless). */
  constexpr ttbr_lpae &set_base_addr(std::uint64_t addr) noexcept {
    raw = ttbr_lpae_bits::baddr::set(raw, addr >> 1);
    return *this;
  }

  [[nodiscard]] constexpr std::uint8_t asid() const noexcept {
    return static_cast<std::uint8_t>(ttbr_lpae_bits::asid::get(raw));
  }
  constexpr ttbr_lpae &set_asid(std::uint8_t value) noexcept {
    raw = ttbr_lpae_bits::asid::set(raw, value);
    return *this;
  }

#if defined(__arm__) && !defined(__aarch64__)
  /** @brief Reads the live register pair. Privileged; not unit tested. */
  [[nodiscard]] static ttbr_lpae read() noexcept {
    std::uint32_t lo, hi;
    if constexpr (IsTtbr1) {
      asm volatile("mrrc p15, 1, %0, %1, c2" : "=r"(lo), "=r"(hi));
    } else {
      asm volatile("mrrc p15, 0, %0, %1, c2" : "=r"(lo), "=r"(hi));
    }
    return ttbr_lpae{(static_cast<std::uint64_t>(hi) << 32) | lo};
  }

  /** @brief Writes `raw` to the live register pair. Privileged; not unit tested. */
  void write() const noexcept {
    std::uint32_t lo = static_cast<std::uint32_t>(raw);
    std::uint32_t hi = static_cast<std::uint32_t>(raw >> 32);
    if constexpr (IsTtbr1) {
      asm volatile("mcrr p15, 1, %0, %1, c2" ::"r"(lo), "r"(hi) : "memory");
    } else {
      asm volatile("mcrr p15, 0, %0, %1, c2" ::"r"(lo), "r"(hi) : "memory");
    }
  }
#endif // defined(__arm__) && !defined(__aarch64__)
};

/** @brief `TTBR0` in LPAE mode. */
using ttbr0_lpae = ttbr_lpae<false>;
/** @brief `TTBR1` in LPAE mode. */
using ttbr1_lpae = ttbr_lpae<true>;

/** @brief Named bit-field accessors for `CONTEXTIDR`. */
struct contextidr_bits {
  using asid = pte_bit_field<0, 8, std::uint32_t>;    //!< Bits [7:0]: Address Space ID (classic short-descriptor mode).
  using procid = pte_bit_field<8, 24, std::uint32_t>; //!< Bits [31:8]: Process ID (used by some OS/trace tooling).
};

/**
 * @brief Parsed/built view of `CONTEXTIDR`: the ASID tag classic
 * short-descriptor-mode translations use (LPAE instead carries its ASID
 * directly in `TTBR0`/`TTBR1`, see `ttbr_lpae::asid()`), plus the
 * (orthogonal) Process ID field.
 */
struct contextidr {
  std::uint32_t raw{0};

  [[nodiscard]] static constexpr contextidr from_raw(std::uint32_t value) noexcept { return contextidr{value}; }

  [[nodiscard]] constexpr std::uint8_t asid() const noexcept {
    return static_cast<std::uint8_t>(contextidr_bits::asid::get(raw));
  }
  constexpr contextidr &set_asid(std::uint8_t value) noexcept {
    raw = contextidr_bits::asid::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr std::uint32_t procid() const noexcept { return contextidr_bits::procid::get(raw); }
  constexpr contextidr &set_procid(std::uint32_t value) noexcept {
    raw = contextidr_bits::procid::set(raw, value);
    return *this;
  }

#if defined(__arm__) && !defined(__aarch64__)
  /** @brief Reads the live `CONTEXTIDR` register. Privileged; not unit tested. */
  [[nodiscard]] static contextidr read() noexcept {
    std::uint32_t value;
    asm volatile("mrc p15, 0, %0, c13, c0, 1" : "=r"(value));
    return contextidr{value};
  }

  /** @brief Writes `raw` to the live `CONTEXTIDR` register. Privileged; not unit tested. */
  void write() const noexcept { asm volatile("mcr p15, 0, %0, c13, c0, 1" ::"r"(raw) : "memory"); }
#endif // defined(__arm__) && !defined(__aarch64__)
};

/**
 * @brief Named bit-field accessors for `SCR` (Secure Configuration
 * Register). Only accessible from Secure state (typically Monitor mode);
 * reads/writes from Non-secure state trap.
 */
struct scr_bits {
  using ns = pte_bit_field<0, 1, std::uint32_t>;  //!< Bit 0: Non-secure -- the world the next exception level enters.
  using irq = pte_bit_field<1, 1, std::uint32_t>; //!< Bit 1: IRQ routed to Monitor mode regardless of CPSR.I.
  using fiq = pte_bit_field<2, 1, std::uint32_t>; //!< Bit 2: FIQ routed to Monitor mode regardless of CPSR.F.
  using ea = pte_bit_field<3, 1, std::uint32_t>;  //!< Bit 3: External Abort routed to Monitor mode.
  using fw = pte_bit_field<4, 1, std::uint32_t>;  //!< Bit 4: F bit writable from Non-secure state.
  using aw = pte_bit_field<5, 1, std::uint32_t>;  //!< Bit 5: A bit writable from Non-secure state.
  using net = pte_bit_field<6, 1, std::uint32_t>; //!< Bit 6: Not Early Termination (reserved on some cores).
  using scd = pte_bit_field<7, 1, std::uint32_t>; //!< Bit 7: Secure Monitor Call Disable (traps `SMC` from Non-secure).
  using hce = pte_bit_field<8, 1, std::uint32_t>; //!< Bit 8: Hyp Call Enable (`HVC` usable from Non-secure state).
  using sif = pte_bit_field<9, 1, std::uint32_t>; //!< Bit 9: Secure Instruction Fetch (forbids Secure fetch from Non-secure memory).
};

/**
 * @brief Parsed/built view of `SCR`: the register Secure/Monitor-mode
 * software uses to decide which world (`NS`) the next exception level
 * runs in -- which, in turn, selects which banked copy of `SCTLR`/
 * `TTBCR`/`TTBR0`/`TTBR1`/`CONTEXTIDR` is live -- plus the handful of
 * related exception-routing and `SMC`/`HVC` availability bits.
 */
struct scr {
  std::uint32_t raw{0};

  [[nodiscard]] static constexpr scr from_raw(std::uint32_t value) noexcept { return scr{value}; }

  /** @brief `true`: the next world entered is Non-secure. `false`: Secure. */
  [[nodiscard]] constexpr bool ns() const noexcept { return scr_bits::ns::test(raw); }
  constexpr scr &set_ns(bool value) noexcept {
    raw = scr_bits::ns::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool irq_to_monitor() const noexcept { return scr_bits::irq::test(raw); }
  constexpr scr &set_irq_to_monitor(bool value) noexcept {
    raw = scr_bits::irq::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool fiq_to_monitor() const noexcept { return scr_bits::fiq::test(raw); }
  constexpr scr &set_fiq_to_monitor(bool value) noexcept {
    raw = scr_bits::fiq::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool external_abort_to_monitor() const noexcept { return scr_bits::ea::test(raw); }
  constexpr scr &set_external_abort_to_monitor(bool value) noexcept {
    raw = scr_bits::ea::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool fiq_mask_writable_from_ns() const noexcept { return scr_bits::fw::test(raw); }
  constexpr scr &set_fiq_mask_writable_from_ns(bool value) noexcept {
    raw = scr_bits::fw::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool abort_mask_writable_from_ns() const noexcept { return scr_bits::aw::test(raw); }
  constexpr scr &set_abort_mask_writable_from_ns(bool value) noexcept {
    raw = scr_bits::aw::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool secure_monitor_call_disabled() const noexcept { return scr_bits::scd::test(raw); }
  constexpr scr &set_secure_monitor_call_disabled(bool value) noexcept {
    raw = scr_bits::scd::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool hyp_call_enabled() const noexcept { return scr_bits::hce::test(raw); }
  constexpr scr &set_hyp_call_enabled(bool value) noexcept {
    raw = scr_bits::hce::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool secure_instruction_fetch() const noexcept { return scr_bits::sif::test(raw); }
  constexpr scr &set_secure_instruction_fetch(bool value) noexcept {
    raw = scr_bits::sif::set_bit(raw, value);
    return *this;
  }

#if defined(__arm__) && !defined(__aarch64__)
  /** @brief Reads the live `SCR` register. Secure-only; not unit tested. */
  [[nodiscard]] static scr read() noexcept {
    std::uint32_t value;
    asm volatile("mrc p15, 0, %0, c1, c1, 0" : "=r"(value));
    return scr{value};
  }

  /** @brief Writes `raw` to the live `SCR` register. Secure-only; not unit tested. */
  void write() const noexcept { asm volatile("mcr p15, 0, %0, c1, c1, 0" ::"r"(raw) : "memory"); }
#endif // defined(__arm__) && !defined(__aarch64__)
};

/**
 * @brief Named bit-field accessors for `NSACR` (Non-Secure Access
 * Control Register). Only the architecturally stable subset of bits is
 * modeled: the per-coprocessor Non-secure-access-enable field (bits
 * [13:0], one bit per `CP<n>`, with `cp10`/`cp11` convenience accessors
 * for the VFP/Advanced SIMD coprocessors most commonly toggled) and the
 * two Advanced SIMD/VFP lockout bits.
 */
struct nsacr_bits {
  using cp_access = pte_bit_field<0, 14, std::uint32_t>; //!< Bits [13:0]: Non-secure access enable, one bit per CP<n>.
  using nsd32dis = pte_bit_field<14, 1, std::uint32_t>;  //!< Bit 14: disables Non-secure use of the upper 16 VFP D registers.
  using nsasedis = pte_bit_field<15, 1, std::uint32_t>;  //!< Bit 15: disables Non-secure Advanced SIMD (ASIMD) functionality.
};

/**
 * @brief Parsed/built view of `NSACR`: which coprocessors (notably
 * CP10/CP11, VFP/Advanced SIMD) the Non-secure world may access, and
 * whether Non-secure Advanced SIMD use is disabled outright.
 */
struct nsacr {
  std::uint32_t raw{0};

  [[nodiscard]] static constexpr nsacr from_raw(std::uint32_t value) noexcept { return nsacr{value}; }

  /** @brief `true` if Non-secure state may access coprocessor `cp` (0..13). */
  [[nodiscard]] constexpr bool cp_accessible(unsigned cp) const noexcept {
    return ((nsacr_bits::cp_access::get(raw) >> cp) & 1u) != 0;
  }
  /** @brief Grants (`true`) or revokes (`false`) Non-secure access to coprocessor `cp` (0..13). */
  constexpr nsacr &set_cp_accessible(unsigned cp, bool value) noexcept {
    std::uint32_t field = nsacr_bits::cp_access::get(raw);
    field = value ? (field | (1u << cp)) : (field & ~(1u << cp));
    raw = nsacr_bits::cp_access::set(raw, field);
    return *this;
  }

  /** @brief Convenience: Non-secure access to CP10 (VFP). */
  [[nodiscard]] constexpr bool cp10_accessible() const noexcept { return cp_accessible(10); }
  constexpr nsacr &set_cp10_accessible(bool value) noexcept { return set_cp_accessible(10, value); }

  /** @brief Convenience: Non-secure access to CP11 (VFP/Advanced SIMD). */
  [[nodiscard]] constexpr bool cp11_accessible() const noexcept { return cp_accessible(11); }
  constexpr nsacr &set_cp11_accessible(bool value) noexcept { return set_cp_accessible(11, value); }

  [[nodiscard]] constexpr bool nsd32_disabled() const noexcept { return nsacr_bits::nsd32dis::test(raw); }
  constexpr nsacr &set_nsd32_disabled(bool value) noexcept {
    raw = nsacr_bits::nsd32dis::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool nsase_disabled() const noexcept { return nsacr_bits::nsasedis::test(raw); }
  constexpr nsacr &set_nsase_disabled(bool value) noexcept {
    raw = nsacr_bits::nsasedis::set_bit(raw, value);
    return *this;
  }

#if defined(__arm__) && !defined(__aarch64__)
  /** @brief Reads the live `NSACR` register. Secure-only; not unit tested. */
  [[nodiscard]] static nsacr read() noexcept {
    std::uint32_t value;
    asm volatile("mrc p15, 0, %0, c1, c1, 2" : "=r"(value));
    return nsacr{value};
  }

  /** @brief Writes `raw` to the live `NSACR` register. Secure-only; not unit tested. */
  void write() const noexcept { asm volatile("mcr p15, 0, %0, c1, c1, 2" ::"r"(raw) : "memory"); }
#endif // defined(__arm__) && !defined(__aarch64__)
};

/**
 * @brief Named bit-field accessors for `HCR` (Hypervisor Configuration
 * Register, Virtualization Extensions). Only accessible from Hyp (PL2)
 * mode. Only the subset most relevant to bringing up or tearing down
 * stage-2 translation and basic EL1/PL1-trapping is modeled; the many
 * interrupt-virtualization bits are out of scope here.
 */
struct hcr_bits {
  using vm = pte_bit_field<0, 1, std::uint32_t>;    //!< Bit 0: enable stage-2 translation.
  using swio = pte_bit_field<1, 1, std::uint32_t>;  //!< Bit 1: Set/Way Invalidation Override (PL1 set/way cache ops behave as invalidate-only).
  using ptw = pte_bit_field<2, 1, std::uint32_t>;   //!< Bit 2: Protected Table Walk (a stage-1 walk stepping on a stage-2 Device mapping faults).
  using fmo = pte_bit_field<3, 1, std::uint32_t>;   //!< Bit 3: physical FIQ routed to Hyp mode.
  using imo = pte_bit_field<4, 1, std::uint32_t>;   //!< Bit 4: physical IRQ routed to Hyp mode.
  using amo = pte_bit_field<5, 1, std::uint32_t>;   //!< Bit 5: physical external abort routed to Hyp mode.
  using dc = pte_bit_field<12, 1, std::uint32_t>;   //!< Bit 12: Default Cacheability (forces cacheable when PL0/PL1 stage-1 is disabled).
  using bsu = pte_bit_field<10, 2, std::uint32_t>;   //!< Bits [11:10]: Barrier Shareability Upgrade for PL1/PL0 DSB/DMB.
  using twi = pte_bit_field<13, 1, std::uint32_t>;  //!< Bit 13: `WFI` from PL1/PL0 traps to Hyp mode.
  using twe = pte_bit_field<14, 1, std::uint32_t>;  //!< Bit 14: `WFE` from PL1/PL0 traps to Hyp mode.
  using tsc = pte_bit_field<19, 1, std::uint32_t>;  //!< Bit 19: `SMC` from PL1 traps to Hyp mode.
  using ttlb = pte_bit_field<25, 1, std::uint32_t>; //!< Bit 25: PL1 TLB maintenance instructions trap to Hyp mode.
  using tvm = pte_bit_field<26, 1, std::uint32_t>;  //!< Bit 26: PL1 writes to its own stage-1 MMU control registers trap to Hyp mode.
  using tge = pte_bit_field<27, 1, std::uint32_t>;  //!< Bit 27: Trap General Exceptions -- routes PL0 exceptions to Hyp mode and disables PL1&0 stage-1 translation for PL0.
  using tdz = pte_bit_field<28, 1, std::uint32_t>;  //!< Bit 28: `DC ZVA` from PL1/PL0 traps to Hyp mode.
  using hcd = pte_bit_field<29, 1, std::uint32_t>;  //!< Bit 29: Hyp Call Disable (traps `HVC` wherever it would otherwise be usable).
  using trvm = pte_bit_field<30, 1, std::uint32_t>; //!< Bit 30: PL1 reads of its own stage-1 MMU control registers trap to Hyp mode.
};

/**
 * @brief Parsed/built view of `HCR`: whether stage-2 translation is
 * enabled (`VM`), which PL1 MMU-configuration accesses trap to Hyp
 * mode, and the related barrier/cache-maintenance override bits a basic
 * hypervisor needs when bringing a guest's stage-2 mapping up or down.
 */
struct hcr {
  std::uint32_t raw{0};

  [[nodiscard]] static constexpr hcr from_raw(std::uint32_t value) noexcept { return hcr{value}; }

  [[nodiscard]] constexpr bool vm() const noexcept { return hcr_bits::vm::test(raw); }
  constexpr hcr &set_vm(bool value) noexcept {
    raw = hcr_bits::vm::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool set_way_invalidation_override() const noexcept { return hcr_bits::swio::test(raw); }
  constexpr hcr &set_set_way_invalidation_override(bool value) noexcept {
    raw = hcr_bits::swio::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool protected_table_walk() const noexcept { return hcr_bits::ptw::test(raw); }
  constexpr hcr &set_protected_table_walk(bool value) noexcept {
    raw = hcr_bits::ptw::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool fiq_to_hyp() const noexcept { return hcr_bits::fmo::test(raw); }
  constexpr hcr &set_fiq_to_hyp(bool value) noexcept {
    raw = hcr_bits::fmo::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool irq_to_hyp() const noexcept { return hcr_bits::imo::test(raw); }
  constexpr hcr &set_irq_to_hyp(bool value) noexcept {
    raw = hcr_bits::imo::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool external_abort_to_hyp() const noexcept { return hcr_bits::amo::test(raw); }
  constexpr hcr &set_external_abort_to_hyp(bool value) noexcept {
    raw = hcr_bits::amo::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool default_cacheability() const noexcept { return hcr_bits::dc::test(raw); }
  constexpr hcr &set_default_cacheability(bool value) noexcept {
    raw = hcr_bits::dc::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned barrier_shareability_upgrade() const noexcept {
    return hcr_bits::bsu::get(raw);
  }
  constexpr hcr &set_barrier_shareability_upgrade(unsigned value) noexcept {
    raw = hcr_bits::bsu::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool trap_wfi() const noexcept { return hcr_bits::twi::test(raw); }
  constexpr hcr &set_trap_wfi(bool value) noexcept {
    raw = hcr_bits::twi::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool trap_wfe() const noexcept { return hcr_bits::twe::test(raw); }
  constexpr hcr &set_trap_wfe(bool value) noexcept {
    raw = hcr_bits::twe::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool trap_smc() const noexcept { return hcr_bits::tsc::test(raw); }
  constexpr hcr &set_trap_smc(bool value) noexcept {
    raw = hcr_bits::tsc::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool trap_tlb_maintenance() const noexcept { return hcr_bits::ttlb::test(raw); }
  constexpr hcr &set_trap_tlb_maintenance(bool value) noexcept {
    raw = hcr_bits::ttlb::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool tvm() const noexcept { return hcr_bits::tvm::test(raw); }
  constexpr hcr &set_tvm(bool value) noexcept {
    raw = hcr_bits::tvm::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool trap_general_exceptions() const noexcept { return hcr_bits::tge::test(raw); }
  constexpr hcr &set_trap_general_exceptions(bool value) noexcept {
    raw = hcr_bits::tge::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool trap_dc_zva() const noexcept { return hcr_bits::tdz::test(raw); }
  constexpr hcr &set_trap_dc_zva(bool value) noexcept {
    raw = hcr_bits::tdz::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool hyp_call_disabled() const noexcept { return hcr_bits::hcd::test(raw); }
  constexpr hcr &set_hyp_call_disabled(bool value) noexcept {
    raw = hcr_bits::hcd::set_bit(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr bool trap_vm_reads() const noexcept { return hcr_bits::trvm::test(raw); }
  constexpr hcr &set_trap_vm_reads(bool value) noexcept {
    raw = hcr_bits::trvm::set_bit(raw, value);
    return *this;
  }

#if defined(__arm__) && !defined(__aarch64__)
  /** @brief Reads the live `HCR` register. Hyp-mode-only; not unit tested. */
  [[nodiscard]] static hcr read() noexcept {
    std::uint32_t value;
    asm volatile("mrc p15, 4, %0, c1, c1, 0" : "=r"(value));
    return hcr{value};
  }

  /** @brief Writes `raw` to the live `HCR` register. Hyp-mode-only; not unit tested. */
  void write() const noexcept { asm volatile("mcr p15, 4, %0, c1, c1, 0" ::"r"(raw) : "memory"); }
#endif // defined(__arm__) && !defined(__aarch64__)
};

/**
 * @brief Named bit-field accessors for `VTCR` (Virtualization
 * Translation Control Register). Only the architecturally stable,
 * conservative subset of fields is modeled (see `vtcr_el2_bits` in the
 * AArch64 header for the analogous, more extensively documented
 * fields); ARMv7 LPAE stage-2 has no alternate granule, so there is no
 * `TG0`-equivalent field here.
 */
struct vtcr_bits {
  using t0sz = pte_bit_field<0, 4, std::uint32_t>;   //!< Bits [3:0]: stage-2 input (guest-physical) address size.
  using sl0 = pte_bit_field<6, 2, std::uint32_t>;     //!< Bits [7:6]: stage-2 starting level of translation.
  using irgn0 = pte_bit_field<8, 2, std::uint32_t>;   //!< Bits [9:8]: stage-2 table-walk inner cacheability.
  using orgn0 = pte_bit_field<10, 2, std::uint32_t>;  //!< Bits [11:10]: stage-2 table-walk outer cacheability.
  using sh0 = pte_bit_field<12, 2, std::uint32_t>;    //!< Bits [13:12]: stage-2 table-walk shareability.
};

/**
 * @brief Parsed/built view of `VTCR`: input address size, starting
 * level, and cacheability/shareability attributes for stage-2
 * (guest-physical-to-physical) translation -- the stage-2 counterpart of
 * `TTBCR` (LPAE mode).
 */
struct vtcr {
  std::uint32_t raw{0};

  [[nodiscard]] static constexpr vtcr from_raw(std::uint32_t value) noexcept { return vtcr{value}; }

  [[nodiscard]] constexpr unsigned t0sz() const noexcept { return vtcr_bits::t0sz::get(raw); }
  constexpr vtcr &set_t0sz(unsigned value) noexcept {
    raw = vtcr_bits::t0sz::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned sl0() const noexcept { return vtcr_bits::sl0::get(raw); }
  constexpr vtcr &set_sl0(unsigned value) noexcept {
    raw = vtcr_bits::sl0::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned irgn0() const noexcept { return vtcr_bits::irgn0::get(raw); }
  constexpr vtcr &set_irgn0(unsigned value) noexcept {
    raw = vtcr_bits::irgn0::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned orgn0() const noexcept { return vtcr_bits::orgn0::get(raw); }
  constexpr vtcr &set_orgn0(unsigned value) noexcept {
    raw = vtcr_bits::orgn0::set(raw, value);
    return *this;
  }

  [[nodiscard]] constexpr unsigned sh0() const noexcept { return vtcr_bits::sh0::get(raw); }
  constexpr vtcr &set_sh0(unsigned value) noexcept {
    raw = vtcr_bits::sh0::set(raw, value);
    return *this;
  }

#if defined(__arm__) && !defined(__aarch64__)
  /** @brief Reads the live `VTCR` register. Hyp-mode-only; not unit tested. */
  [[nodiscard]] static vtcr read() noexcept {
    std::uint32_t value;
    asm volatile("mrc p15, 4, %0, c2, c1, 2" : "=r"(value));
    return vtcr{value};
  }

  /** @brief Writes `raw` to the live `VTCR` register. Hyp-mode-only; not unit tested. */
  void write() const noexcept { asm volatile("mcr p15, 4, %0, c2, c1, 2" ::"r"(raw) : "memory"); }
#endif // defined(__arm__) && !defined(__aarch64__)
};

/** @brief Named bit-field accessors for `VTTBR` (Virtualization Translation Table Base Register). */
struct vttbr_bits {
  using baddr = pte_bit_field<1, 39>; //!< Bits [39:1]: stage-2 translation table base address.
  using vmid = pte_bit_field<48, 8>;  //!< Bits [55:48]: Virtual Machine ID.
};

/**
 * @brief Parsed/built view of `VTTBR`: the root of the current guest's
 * stage-2 translation table, tagged with a VMID -- the stage-2
 * counterpart of LPAE's `TTBR0`.
 */
struct vttbr {
  std::uint64_t raw{0};

  [[nodiscard]] static constexpr vttbr from_raw(std::uint64_t value) noexcept { return vttbr{value}; }

  [[nodiscard]] constexpr std::uint64_t base_addr() const noexcept { return vttbr_bits::baddr::get(raw) << 1; }
  /** @brief Sets the stage-2 translation table base address. `addr`'s bit 0 must be `0` (masked off regardless). */
  constexpr vttbr &set_base_addr(std::uint64_t addr) noexcept {
    raw = vttbr_bits::baddr::set(raw, addr >> 1);
    return *this;
  }

  [[nodiscard]] constexpr std::uint8_t vmid() const noexcept {
    return static_cast<std::uint8_t>(vttbr_bits::vmid::get(raw));
  }
  constexpr vttbr &set_vmid(std::uint8_t value) noexcept {
    raw = vttbr_bits::vmid::set(raw, value);
    return *this;
  }

#if defined(__arm__) && !defined(__aarch64__)
  /** @brief Reads the live `VTTBR` register pair. Hyp-mode-only; not unit tested. */
  [[nodiscard]] static vttbr read() noexcept {
    std::uint32_t lo, hi;
    asm volatile("mrrc p15, 6, %0, %1, c2" : "=r"(lo), "=r"(hi));
    return vttbr{(static_cast<std::uint64_t>(hi) << 32) | lo};
  }

  /** @brief Writes `raw` to the live `VTTBR` register pair. Hyp-mode-only; not unit tested. */
  void write() const noexcept {
    std::uint32_t lo = static_cast<std::uint32_t>(raw);
    std::uint32_t hi = static_cast<std::uint32_t>(raw >> 32);
    asm volatile("mcrr p15, 6, %0, %1, c2" ::"r"(lo), "r"(hi) : "memory");
  }
#endif // defined(__arm__) && !defined(__aarch64__)
};

} // namespace structo::arch::arm
