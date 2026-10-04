// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file tlb_flush.hpp
 * @brief Two `structo::arch::tlb_flush_traits<Arch>` specializations
 * for ARMv7-A/AArch32 -- `structo::arch::arm::tlb_tag` (local-only,
 * every ARMv7-A/AArch32 core) and `structo::arch::arm::tlb_tag_mp`
 * (adds the Inner-Shareable broadcast `...IS` encodings, Multiprocessing
 * Extensions only) -- wiring `structo::arch::tlb_flusher<Arch>` up to
 * the real CP15 `TLBI*` System instructions (`MCR p15, <opc1>, <Rt>,
 * c8, <CRm>, <opc2>`).
 *
 * See `arch/tlb_flush.hpp` for the full architecture-agnostic
 * dispatcher/fallback design this specializes; only the AArch32-specific
 * instruction selection is documented here. Encodings verified against
 * the Armv8-A Architecture Reference Manual (DDI0487), section G8.2,
 * which also defines the AArch32 state's CP15 TLB maintenance
 * instructions as inherited unchanged from ARMv7-A.
 *
 * ## Why two separate `Arch` tags, not one
 *
 * Exactly as for x86 (`arch/x86/tlb_flush.hpp`): whether a given core
 * actually implements the Inner-Shareable broadcast domain (the
 * Multiprocessing Extensions) is a runtime/implementation fact, not
 * something this header can reliably test at compile time. **The
 * caller states which hardware feature it has already verified** by
 * picking `tlb_tag` or `tlb_tag_mp`; executing an `...IS` encoding on a
 * core that doesn't implement the Multiprocessing Extensions is
 * CONSTRAINED UNPREDICTABLE, the same way any other
 * implementation-gated instruction would be.
 *
 * ## `process_tlb_space`: ASID-tagged stage-1, PL1&0
 *
 * - `flush_all()` -- `TLBIALL` (`opc1=0, CRm=c7, opc2=0`), `Rt`
 *   ignored.
 * - `flush_tag(asid)` -- `TLBIASID` (`opc1=0, CRm=c7, opc2=2`), `Rt`
 *   bits `[7:0]` = ASID.
 * - `flush_page(addr)` -- `TLBIMVAA` ("by VA, **all** ASID", `opc1=0,
 *   CRm=c7, opc2=3`), `Rt` bits `[31:12]` = VA -- deliberately not
 *   `TLBIMVA` (see below), since `TLBIMVAA` is the one encoding whose
 *   architectural effect does not depend on an ASID at all.
 * - `flush_page_tag(addr, asid)` -- `TLBIMVA` (`opc1=0, CRm=c7,
 *   opc2=1`), `Rt` bits `[31:12]` = VA, bits `[7:0]` = ASID.
 *
 * `TLBIMVA`'s ASID field is not optional the way, say, ARM64's `TLBI
 * VAE1` can be read as "this ASID, or match globally" -- it specifically
 * also still hits *global* entries matching that VA regardless of the
 * ASID field, but does **not** reach every non-global entry at that VA
 * the way `TLBIMVAA` does. `TLBIMVAA` is therefore the correct, safe
 * choice for this header's address-only, every-tag `flush_page` (never
 * under-invalidating), while `TLBIMVA` is reserved for the precise,
 * single-ASID `flush_page_tag`.
 *
 * ## `hypervisor_tlb_space`: untagged, Hyp mode (PL2)
 *
 * - `flush_all()` -- `TLBIALLH` (`opc1=4, CRm=c7, opc2=0`).
 * - `flush_page(addr)` -- `TLBIMVAH` (`opc1=4, CRm=c7, opc2=1`), `Rt`
 *   bits `[31:12]` = VA. Hyp mode's own stage-1 translation has no ASID
 *   concept at all, so there is no tagged twin to wire up.
 *
 * ## `guest_tlb_space`: stage-2, by IPA, current VMID only
 *
 * `TLBIIPAS2` takes an Intermediate Physical Address but **no VMID
 * operand at all** -- it always invalidates for whichever VMID the
 * current `VTTBR` already holds (see `mmu_regs.hpp`'s `vttbr`). There is
 * therefore no ISA-level way to implement a `flush_tag`/`flush_page_tag`
 * that targets an arbitrary, caller-chosen VMID without also
 * reprogramming `VTTBR` as a side effect this header has no business
 * performing silently -- so only the untagged members are provided
 * here:
 *
 * - `flush_all()` -- `TLBIALLNSNH` ("All, Non-Secure Non-Hyp",
 *   `opc1=4, CRm=c7, opc2=4`). This invalidates the current VMID's
 *   combined stage-1+stage-2 entries, slightly more than a pure
 *   stage-2-only flush -- always safe, per the precision-fallback
 *   philosophy in `arch/tlb_flush.hpp`'s file docs, since there is no
 *   separate "stage 2 only, every IPA, current VMID" encoding to prefer
 *   instead.
 * - `flush_page(ipa)` -- `TLBIIPAS2` (`opc1=4, CRm=c4, opc2=1`), `Rt`
 *   bits `[27:0]` = `IPA[39:12]` (i.e. `ipa >> 12`).
 *
 * `tlb_flusher::flush_tag`/`flush_page_tag` for `guest_tlb_space` on
 * either tag below therefore fall back to the untagged `flush_all`/
 * `flush_page` automatically, exactly as `arch/tlb_flush.hpp`
 * documents for any `Space`/operation a trait doesn't implement --
 * correct (the caller's own `VTTBR` write already scoped the "tag"),
 * just not literally a hardware tag match.
 *
 * ## `secure_tlb_space`/`nonsecure_tlb_space`: not modeled here
 *
 * Every instruction above has **no Secure/Non-secure selector operand**
 * at all -- which world `TLBIALL`/`TLBIASID`/`TLBIMVA`/`TLBIMVAA` affect
 * is determined entirely by the PL1 security state the core is
 * executing in *when the instruction runs*, exactly like `mmu_regs.hpp`'s
 * banked `SCTLR`/`TTBCR`/`TTBR0`/`TTBR1`/`CONTEXTIDR` registers (see
 * that header's "TrustZone and MMU register banking" docs). A caller
 * needing to flush a specific world's `process_tlb_space` entries
 * simply executes `tlb_tag`'s ops while actually running in that world
 * (e.g. from Monitor mode after an `SCR.NS` switch) -- this header
 * cannot and does not offer a `Space` selector the ISA itself has no
 * operand for.
 *
 * ## Broadcast (`tlb_tag_mp` only)
 *
 * Every local op above has an Inner-Shareable-broadcast twin, same
 * `Rt` encoding, `opc1`/`CRm` swapped to the shared "IS" group:
 * `TLBIALLIS` (`opc1=0, CRm=c3, opc2=0`), `TLBIASIDIS` (`CRm=c3,
 * opc2=2`), `TLBIMVAAIS` (`CRm=c3, opc2=3`), `TLBIMVAIS` (`CRm=c3,
 * opc2=1`), `TLBIALLHIS` (`opc1=4, CRm=c3, opc2=0`), `TLBIMVAHIS`
 * (`opc1=4, CRm=c3, opc2=1`), `TLBIALLNSNHIS` (`opc1=4, CRm=c3,
 * opc2=4`), `TLBIIPAS2IS` (`opc1=4, CRm=c0, opc2=1`).
 * `tlb_tag_mp::supports_broadcast = true`; `tlb_tag::supports_broadcast
 * = false` and defines no `..._broadcast` member at all, so
 * `tlb_flusher<arm::tlb_tag>`'s `..._broadcast()` calls are a
 * compile-time `static_assert`, exactly as `arch/tlb_flush.hpp`
 * documents for any local-only architecture.
 *
 * ## Range flushes: not wired up, falls back automatically
 *
 * ARMv7-A/AArch32 has no range-invalidate instruction (nothing like
 * ARM64's `FEAT_TLBIRANGE`), so neither tag defines `flush_range`/
 * `flush_range_tag` -- `tlb_flusher` already falls back to
 * `flush_tag`/`flush_all` for them automatically.
 *
 * Only compiled on a real 32-bit ARM target (`__arm__`, and not
 * AArch64); on every other host this header is an intentional no-op so
 * it stays header-check-clean cross-compiled from any machine.
 */

#if defined(__arm__) && !defined(__aarch64__)

#include <structo/arch/tlb_flush.hpp>

#include <cstdint>
#include <type_traits>

namespace structo::arch::arm {

/** @brief `tlb_flush_traits<arm::tlb_tag>`'s `Arch` tag -- local-only CP15 `TLBI*` ops, every ARMv7-A/AArch32 core. See
 * the @file docs. */
struct tlb_tag {};

/** @brief `tlb_flush_traits<arm::tlb_tag_mp>`'s `Arch` tag -- adds the Inner-Shareable `...IS` broadcast ops. Requires
 * the Multiprocessing Extensions. See the @file docs. */
struct tlb_tag_mp {};

} // namespace structo::arch::arm

// ============================================================================
// arm::tlb_tag -- local-only (opc1=0 for PL1&0, opc1=4 for Hyp/stage-2)
// ============================================================================

template <> struct structo::arch::tlb_flush_traits<structo::arch::arm::tlb_tag> {
  static constexpr bool supports_broadcast = false;

  // --- process_tlb_space: TLBIALL / TLBIASID / TLBIMVA / TLBIMVAA ---------

  template <typename Space>
  static auto flush_all() noexcept -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    std::uint32_t rt = 0;
    asm volatile("mcr p15, 0, %0, c8, c7, 0" ::"r"(rt) : "memory"); // TLBIALL
  }

  template <typename Space>
  static auto flush_tag(std::uint64_t asid) noexcept -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    std::uint32_t rt = static_cast<std::uint32_t>(asid) & 0xffu;
    asm volatile("mcr p15, 0, %0, c8, c7, 2" ::"r"(rt) : "memory"); // TLBIASID
  }

  template <typename Space>
  static auto flush_page(std::uint64_t vaddr) noexcept -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    std::uint32_t rt = static_cast<std::uint32_t>(vaddr) & ~0xfffu;
    asm volatile("mcr p15, 0, %0, c8, c7, 3" ::"r"(rt) : "memory"); // TLBIMVAA
  }

  template <typename Space>
  static auto flush_page_tag(std::uint64_t vaddr, std::uint64_t asid) noexcept
      -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    std::uint32_t rt = (static_cast<std::uint32_t>(vaddr) & ~0xfffu) | (static_cast<std::uint32_t>(asid) & 0xffu);
    asm volatile("mcr p15, 0, %0, c8, c7, 1" ::"r"(rt) : "memory"); // TLBIMVA
  }

  // --- hypervisor_tlb_space: TLBIALLH / TLBIMVAH --------------------------

  template <typename Space>
  static auto flush_all() noexcept -> std::enable_if_t<std::is_same_v<Space, hypervisor_tlb_space>> {
    std::uint32_t rt = 0;
    asm volatile("mcr p15, 4, %0, c8, c7, 0" ::"r"(rt) : "memory"); // TLBIALLH
  }

  template <typename Space>
  static auto flush_page(std::uint64_t vaddr) noexcept
      -> std::enable_if_t<std::is_same_v<Space, hypervisor_tlb_space>> {
    std::uint32_t rt = static_cast<std::uint32_t>(vaddr) & ~0xfffu;
    asm volatile("mcr p15, 4, %0, c8, c7, 1" ::"r"(rt) : "memory"); // TLBIMVAH
  }

  // --- guest_tlb_space: TLBIALLNSNH / TLBIIPAS2 (current VMID only) ------

  template <typename Space>
  static auto flush_all() noexcept -> std::enable_if_t<std::is_same_v<Space, guest_tlb_space>> {
    std::uint32_t rt = 0;
    asm volatile("mcr p15, 4, %0, c8, c7, 4" ::"r"(rt) : "memory"); // TLBIALLNSNH
  }

  template <typename Space>
  static auto flush_page(std::uint64_t ipa) noexcept -> std::enable_if_t<std::is_same_v<Space, guest_tlb_space>> {
    std::uint32_t rt = static_cast<std::uint32_t>(ipa >> 12);       // IPA[39:12]
    asm volatile("mcr p15, 4, %0, c8, c4, 1" ::"r"(rt) : "memory"); // TLBIIPAS2
  }
};

// ============================================================================
// arm::tlb_tag_mp -- adds the Inner-Shareable broadcast twins
// ============================================================================

template <> struct structo::arch::tlb_flush_traits<structo::arch::arm::tlb_tag_mp> {
  static constexpr bool supports_broadcast = true;

  // --- Local: identical to arm::tlb_tag -----------------------------------

  template <typename Space>
  static auto flush_all() noexcept -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    tlb_flush_traits<arm::tlb_tag>::flush_all<process_tlb_space>();
  }

  template <typename Space>
  static auto flush_tag(std::uint64_t asid) noexcept -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    tlb_flush_traits<arm::tlb_tag>::flush_tag<process_tlb_space>(asid);
  }

  template <typename Space>
  static auto flush_page(std::uint64_t vaddr) noexcept -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    tlb_flush_traits<arm::tlb_tag>::flush_page<process_tlb_space>(vaddr);
  }

  template <typename Space>
  static auto flush_page_tag(std::uint64_t vaddr, std::uint64_t asid) noexcept
      -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    tlb_flush_traits<arm::tlb_tag>::flush_page_tag<process_tlb_space>(vaddr, asid);
  }

  template <typename Space>
  static auto flush_all() noexcept -> std::enable_if_t<std::is_same_v<Space, hypervisor_tlb_space>> {
    tlb_flush_traits<arm::tlb_tag>::flush_all<hypervisor_tlb_space>();
  }

  template <typename Space>
  static auto flush_page(std::uint64_t vaddr) noexcept
      -> std::enable_if_t<std::is_same_v<Space, hypervisor_tlb_space>> {
    tlb_flush_traits<arm::tlb_tag>::flush_page<hypervisor_tlb_space>(vaddr);
  }

  template <typename Space>
  static auto flush_all() noexcept -> std::enable_if_t<std::is_same_v<Space, guest_tlb_space>> {
    tlb_flush_traits<arm::tlb_tag>::flush_all<guest_tlb_space>();
  }

  template <typename Space>
  static auto flush_page(std::uint64_t ipa) noexcept -> std::enable_if_t<std::is_same_v<Space, guest_tlb_space>> {
    tlb_flush_traits<arm::tlb_tag>::flush_page<guest_tlb_space>(ipa);
  }

  // --- Broadcast: process_tlb_space ---------------------------------------

  template <typename Space>
  static auto flush_all_broadcast() noexcept -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    std::uint32_t rt = 0;
    asm volatile("mcr p15, 0, %0, c8, c3, 0" ::"r"(rt) : "memory"); // TLBIALLIS
  }

  template <typename Space>
  static auto flush_tag_broadcast(std::uint64_t asid) noexcept
      -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    std::uint32_t rt = static_cast<std::uint32_t>(asid) & 0xffu;
    asm volatile("mcr p15, 0, %0, c8, c3, 2" ::"r"(rt) : "memory"); // TLBIASIDIS
  }

  template <typename Space>
  static auto flush_page_broadcast(std::uint64_t vaddr) noexcept
      -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    std::uint32_t rt = static_cast<std::uint32_t>(vaddr) & ~0xfffu;
    asm volatile("mcr p15, 0, %0, c8, c3, 3" ::"r"(rt) : "memory"); // TLBIMVAAIS
  }

  template <typename Space>
  static auto flush_page_tag_broadcast(std::uint64_t vaddr, std::uint64_t asid) noexcept
      -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    std::uint32_t rt = (static_cast<std::uint32_t>(vaddr) & ~0xfffu) | (static_cast<std::uint32_t>(asid) & 0xffu);
    asm volatile("mcr p15, 0, %0, c8, c3, 1" ::"r"(rt) : "memory"); // TLBIMVAIS
  }

  // --- Broadcast: hypervisor_tlb_space -------------------------------------

  template <typename Space>
  static auto flush_all_broadcast() noexcept -> std::enable_if_t<std::is_same_v<Space, hypervisor_tlb_space>> {
    std::uint32_t rt = 0;
    asm volatile("mcr p15, 4, %0, c8, c3, 0" ::"r"(rt) : "memory"); // TLBIALLHIS
  }

  template <typename Space>
  static auto flush_page_broadcast(std::uint64_t vaddr) noexcept
      -> std::enable_if_t<std::is_same_v<Space, hypervisor_tlb_space>> {
    std::uint32_t rt = static_cast<std::uint32_t>(vaddr) & ~0xfffu;
    asm volatile("mcr p15, 4, %0, c8, c3, 1" ::"r"(rt) : "memory"); // TLBIMVAHIS
  }

  // --- Broadcast: guest_tlb_space (current VMID only, see @file docs) -----

  template <typename Space>
  static auto flush_all_broadcast() noexcept -> std::enable_if_t<std::is_same_v<Space, guest_tlb_space>> {
    std::uint32_t rt = 0;
    asm volatile("mcr p15, 4, %0, c8, c3, 4" ::"r"(rt) : "memory"); // TLBIALLNSNHIS
  }

  template <typename Space>
  static auto flush_page_broadcast(std::uint64_t ipa) noexcept
      -> std::enable_if_t<std::is_same_v<Space, guest_tlb_space>> {
    std::uint32_t rt = static_cast<std::uint32_t>(ipa >> 12);
    asm volatile("mcr p15, 4, %0, c8, c0, 1" ::"r"(rt) : "memory"); // TLBIIPAS2IS
  }
};

#endif // defined(__arm__) && !defined(__aarch64__)
