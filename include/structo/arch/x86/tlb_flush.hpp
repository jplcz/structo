// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file tlb_flush.hpp
 * @brief Three `structo::arch::tlb_flush_traits<Arch>` specializations
 * for x86/x86-64 -- `structo::arch::x86::tlb_tag` (plain, every CPU),
 * `structo::arch::x86::pcid_tlb_tag` (Process-Context ID), and
 * `structo::arch::x86::vpid_tlb_tag`/`structo::arch::x86::eptp_tlb_tag`
 * (VMX guest-linear/EPT invalidation) -- wiring
 * `structo::arch::tlb_flusher<Arch>` up to the real `INVLPG`/
 * `INVPCID`/`INVVPID`/`INVEPT` instructions.
 *
 * See `arch/tlb_flush.hpp` for the full architecture-agnostic
 * dispatcher/fallback design this specializes; only the x86-specific
 * instruction selection is documented here.
 *
 * ## Why four separate `Arch` tags, not one
 *
 * Unlike RISC-V (`arch/riscv/tlb_flush.hpp`), x86's extra TLB-tagging
 * instructions are each gated by a CPU feature that is a *runtime* fact
 * (`CPUID` bits, and for PCID also `CR4.PCIDE`), never a compile-time
 * one -- there is no preprocessor macro this header could reliably test
 * that means "`INVPCID`/VMX is actually usable here". Rather than guess,
 * every optional capability gets its own `Arch` tag, so **the caller
 * states which hardware feature it has already verified/enabled**
 * (typically once, at boot, via `CPUID`) by picking which tag to
 * instantiate `tlb_flusher<...>` with -- exactly like choosing which
 * `tlb_flush_traits` to write in the first place, just one level up.
 * Picking a tag whose instruction the running CPU doesn't actually
 * support is a `#UD` at runtime, the same way calling any other
 * CPU-feature-gated instruction without checking `CPUID` first would
 * be; this header cannot and does not check that for you.
 *
 * - `x86::tlb_tag` -- `INVLPG`/`CR4.PGE` toggle. Always safe: every
 *   x86/x86-64 CPU since the 486 (`INVLPG`) / Pentium Pro (`CR4.PGE`)
 *   supports this. Maps `untagged_tlb_space` and `hypervisor_tlb_space`
 *   (VMX-root's own translations are plain, PCID-less paging from the
 *   hardware's point of view, identical to `untagged_tlb_space`).
 * - `x86::pcid_tlb_tag` -- `INVPCID`. Requires `CPUID.(EAX=07H,
 *   ECX=0):EBX.INVPCID[bit 10]` and, to have any PCID actually tagging
 *   entries in the first place, `CR4.PCIDE=1`. Maps `process_tlb_space`,
 *   tag = PCID.
 * - `x86::vpid_tlb_tag` -- `INVVPID`, for guest-linear-address (combined
 *   stage-1+2) translation cache entries. Requires VMX operation and
 *   `IA32_VMX_PROCBASED_CTLS2.VPID[bit 5]`. Maps `guest_tlb_space`,
 *   tag = VPID.
 * - `x86::eptp_tlb_tag` -- `INVEPT`, for EPT (stage-2) paging-structure
 *   cache entries. Requires VMX operation and
 *   `IA32_VMX_EPT_VPID_CAP`'s `INVEPT`-supported bit. Also maps
 *   `guest_tlb_space`, tag = EPTP (the full EPT pointer value, not just
 *   its physical-address bits) -- a *different* tag space than
 *   `vpid_tlb_tag`'s VPID, so never mix the two tags for the same
 *   `guest_tlb_space` call site.
 *
 * `INVEPT`/`INVVPID` have no per-page (individual-address) type for a
 * plain `flush_page`/`flush_all`-without-a-tag call -- Intel's
 * architecture only ever invalidates EPT/VPID entries per-context or
 * globally, which is exactly why `eptp_tlb_tag` defines no
 * `flush_page`/`flush_range` at all (`tlb_flusher` already falls back
 * to `flush_tag`/`flush_all` for those), and `vpid_tlb_tag` only has the
 * tagged `flush_page_tag`, not a plain `flush_page`.
 *
 * ## Broadcast: none -- every one of these is a single-core instruction
 *
 * `supports_broadcast = false` for every tag above: there is no x86
 * instruction that invalidates another core's TLB. Cross-core
 * consistency is always the caller's job via an IPI-driven shootdown
 * (the classic x86 "TLB shootdown"), exactly as `arch/tlb_flush.hpp`'s
 * file docs describe for any architecture with local-only hardware.
 *
 * ## Range flushes: not wired up, falls back automatically
 *
 * x86 has no range-invalidate instruction (nothing like ARM
 * `FEAT_TLBIRANGE`), so none of these tags define `flush_range`/
 * `flush_range_tag` -- `tlb_flusher` already falls back to
 * `flush_tag`/`flush_all` for them automatically.
 *
 * Only compiled on a real x86/x86-64 target (`__i386__`/`__x86_64__`);
 * on every other host this header is an intentional no-op so it stays
 * header-check-clean cross-compiled from any machine.
 */

#if defined(__i386__) || defined(__x86_64__)

#include <structo/arch/tlb_flush.hpp>

#include <cstdint>
#include <type_traits>

namespace structo::arch::x86 {

/** @brief `tlb_flush_traits<x86::tlb_tag>`'s `Arch` tag -- plain `INVLPG`/`CR4.PGE`, every x86/x86-64 CPU. See the @file docs. */
struct tlb_tag {};

/** @brief `tlb_flush_traits<x86::pcid_tlb_tag>`'s `Arch` tag -- `INVPCID`. Requires `CPUID` support and `CR4.PCIDE=1`. See the @file docs. */
struct pcid_tlb_tag {};

/** @brief `tlb_flush_traits<x86::vpid_tlb_tag>`'s `Arch` tag -- `INVVPID`. Requires VMX operation with VPID enabled. See the @file docs. */
struct vpid_tlb_tag {};

/** @brief `tlb_flush_traits<x86::eptp_tlb_tag>`'s `Arch` tag -- `INVEPT`. Requires VMX operation with EPT enabled. See the @file docs. */
struct eptp_tlb_tag {};

namespace detail {

/** @brief A 128-bit, 16-byte-aligned `{pcid, address}`/`{vpid, reserved, address}`/`{eptp, reserved}` descriptor, exactly as `INVPCID`/`INVVPID`/`INVEPT` require their memory operand shaped. */
struct alignas(16) invpcid_descriptor {
  std::uint64_t context_id; // PCID (bits [11:0]) or VPID (bits [15:0]) or EPTP, rest reserved/0
  std::uint64_t address; // linear address (INVPCID/INVVPID individual-address type) or reserved/0
};

} // namespace detail

} // namespace structo::arch::x86

// ============================================================================
// x86::tlb_tag -- INVLPG / CR4.PGE toggle, every x86/x86-64 CPU
// ============================================================================

template <> struct structo::arch::tlb_flush_traits<structo::arch::x86::tlb_tag> {
  static constexpr bool supports_broadcast = false;

  template <typename Space>
  static auto flush_all() noexcept
      -> std::enable_if_t<std::is_same_v<Space, untagged_tlb_space> || std::is_same_v<Space, hypervisor_tlb_space>> {
    // No single instruction flushes global pages too without INVPCID --
    // toggling CR4.PGE off and back on is the standard portable
    // technique: disabling it (per the SDM) invalidates all TLB entries
    // including global ones, and re-enabling it does not reinstate them.
    std::uintptr_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    asm volatile("mov %0, %%cr4" ::"r"(cr4 & ~std::uintptr_t{1u << 7}) : "memory");
    asm volatile("mov %0, %%cr4" ::"r"(cr4) : "memory");
  }

  template <typename Space>
  static auto flush_page(std::uint64_t vaddr) noexcept
      -> std::enable_if_t<std::is_same_v<Space, untagged_tlb_space> || std::is_same_v<Space, hypervisor_tlb_space>> {
    asm volatile("invlpg (%0)" ::"r"(static_cast<std::uintptr_t>(vaddr)) : "memory");
  }
};

// ============================================================================
// x86::pcid_tlb_tag -- INVPCID, process_tlb_space tagged by PCID
// ============================================================================

template <> struct structo::arch::tlb_flush_traits<structo::arch::x86::pcid_tlb_tag> {
  static constexpr bool supports_broadcast = false;

  /** @brief `INVPCID` type 2: invalidate every mapping for every PCID, including global pages. */
  template <typename Space>
  static auto flush_all() noexcept -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    x86::detail::invpcid_descriptor desc{0, 0};
    asm volatile("invpcid %1, %0" ::"r"(std::uint64_t{2}), "m"(desc) : "memory");
  }

  /** @brief `INVPCID` type 1: invalidate every mapping for one PCID, retaining global pages. */
  template <typename Space>
  static auto flush_tag(std::uint64_t pcid) noexcept -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    x86::detail::invpcid_descriptor desc{pcid, 0};
    asm volatile("invpcid %1, %0" ::"r"(std::uint64_t{1}), "m"(desc) : "memory");
  }

  /** @brief `INVLPG`, untagged by PCID -- always invalidates for the current PCID regardless of whether PCID is enabled. */
  template <typename Space>
  static auto flush_page(std::uint64_t vaddr) noexcept -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    asm volatile("invlpg (%0)" ::"r"(static_cast<std::uintptr_t>(vaddr)) : "memory");
  }

  /** @brief `INVPCID` type 0: invalidate a single address for a single PCID. */
  template <typename Space>
  static auto flush_page_tag(std::uint64_t vaddr, std::uint64_t pcid) noexcept
      -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    x86::detail::invpcid_descriptor desc{pcid, vaddr};
    asm volatile("invpcid %1, %0" ::"r"(std::uint64_t{0}), "m"(desc) : "memory");
  }
};

// ============================================================================
// x86::vpid_tlb_tag -- INVVPID, guest_tlb_space tagged by VPID
// ============================================================================

template <> struct structo::arch::tlb_flush_traits<structo::arch::x86::vpid_tlb_tag> {
  static constexpr bool supports_broadcast = false;

  /** @brief `INVVPID` type 2: invalidate every guest-linear mapping for every VPID, including global pages. */
  template <typename Space>
  static auto flush_all() noexcept -> std::enable_if_t<std::is_same_v<Space, guest_tlb_space>> {
    x86::detail::invpcid_descriptor desc{0, 0};
    asm volatile("invvpid %1, %0" ::"r"(std::uint64_t{2}), "m"(desc) : "memory");
  }

  /** @brief `INVVPID` type 1: invalidate every guest-linear mapping for one VPID. */
  template <typename Space>
  static auto flush_tag(std::uint64_t vpid) noexcept -> std::enable_if_t<std::is_same_v<Space, guest_tlb_space>> {
    x86::detail::invpcid_descriptor desc{vpid, 0};
    asm volatile("invvpid %1, %0" ::"r"(std::uint64_t{1}), "m"(desc) : "memory");
  }

  /** @brief `INVVPID` type 0: invalidate a single guest-linear address for a single VPID. */
  template <typename Space>
  static auto flush_page_tag(std::uint64_t vaddr, std::uint64_t vpid) noexcept
      -> std::enable_if_t<std::is_same_v<Space, guest_tlb_space>> {
    x86::detail::invpcid_descriptor desc{vpid, vaddr};
    asm volatile("invvpid %1, %0" ::"r"(std::uint64_t{0}), "m"(desc) : "memory");
  }
};

// ============================================================================
// x86::eptp_tlb_tag -- INVEPT, guest_tlb_space tagged by EPTP
// ============================================================================

template <> struct structo::arch::tlb_flush_traits<structo::arch::x86::eptp_tlb_tag> {
  static constexpr bool supports_broadcast = false;

  /** @brief `INVEPT` type 2: invalidate EPT paging-structure cache entries for every EPTP. */
  template <typename Space>
  static auto flush_all() noexcept -> std::enable_if_t<std::is_same_v<Space, guest_tlb_space>> {
    x86::detail::invpcid_descriptor desc{0, 0};
    asm volatile("invept %1, %0" ::"r"(std::uint64_t{2}), "m"(desc) : "memory");
  }

  /** @brief `INVEPT` type 1: invalidate EPT paging-structure cache entries associated with one EPTP. */
  template <typename Space>
  static auto flush_tag(std::uint64_t eptp) noexcept -> std::enable_if_t<std::is_same_v<Space, guest_tlb_space>> {
    x86::detail::invpcid_descriptor desc{eptp, 0};
    asm volatile("invept %1, %0" ::"r"(std::uint64_t{1}), "m"(desc) : "memory");
  }
};

#endif // defined(__i386__) || defined(__x86_64__)
