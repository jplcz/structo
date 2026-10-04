// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file tlb_flush.hpp
 * @brief `structo::arch::tlb_flush_traits<structo::arch::arm64::tlb_tag>`
 * for AArch64 -- wiring `structo::arch::tlb_flusher<arm64::tlb_tag>` up
 * to the real `TLBI` System instructions (`TLBI <op>{, <Xt>}`).
 *
 * See `arch/tlb_flush.hpp` for the full architecture-agnostic
 * dispatcher/fallback design this specializes; only the AArch64-specific
 * instruction selection is documented here. Encodings verified against
 * the Armv8-A Architecture Reference Manual (DDI0487), section C5.5.
 *
 * ## Only one `Arch` tag here, unlike AArch32/x86
 *
 * `arch/arm/tlb_flush.hpp` (AArch32) and `arch/x86/tlb_flush.hpp` each
 * define multiple `Arch` tags because whether a core actually
 * implements broadcast invalidation is an optional, runtime-detected
 * feature there (the Multiprocessing Extensions; VMX/`INVPCID`/
 * `INVVPID`/`INVEPT` support). On AArch64, the Inner-Shareable
 * broadcast `TLBI <op>IS` encodings used below are **mandatory baseline
 * A64 instructions since Armv8.0** -- present unconditionally on every
 * multi-PE AArch64 implementation, nothing to runtime-detect -- so a
 * single tag with `supports_broadcast = true` is both correct and
 * sufficient; there is no separate "might not have broadcast" tag to
 * offer.
 *
 * ## `process_tlb_space`: ASID-tagged stage-1, EL1&0 regime
 *
 * - `flush_all()` -- `TLBI VMALLE1` (`Xt` ignored) -- every stage-1
 *   entry for the current VMID, all ASIDs.
 * - `flush_tag(asid)` -- `TLBI ASIDE1, Xt` (`Xt[63:48]` = ASID).
 * - `flush_page(addr)` -- `TLBI VAAE1, Xt` ("by VA, **all** ASID"),
 *   `Xt[43:0]` = `VA[55:12]` (i.e. `addr >> 12`) -- deliberately not
 *   `VAE1` (see below), since `VAAE1` is the one encoding whose
 *   architectural effect does not depend on an ASID at all.
 * - `flush_page_tag(addr, asid)` -- `TLBI VAE1, Xt`, `Xt[63:48]` =
 *   ASID, `Xt[43:0]` = `VA[55:12]`.
 *
 * `VAE1`'s ASID field is not optional the way a reader might assume --
 * it also still hits *global* entries matching that VA regardless of
 * the ASID field, but does **not** reach every non-global entry at
 * that VA the way `VAAE1` does. `VAAE1` is therefore the correct, safe
 * choice for this header's address-only, every-tag `flush_page` (never
 * under-invalidating), while `VAE1` is reserved for the precise,
 * single-ASID `flush_page_tag`.
 *
 * ## `hypervisor_tlb_space`: untagged, EL2 (non-VHE)
 *
 * - `flush_all()` -- `TLBI ALLE2` (`Xt` ignored).
 * - `flush_page(addr)` -- `TLBI VAE2, Xt`, `Xt[43:0]` = `VA[55:12]`.
 *   EL2's own stage-1 translation has no ASID concept at all, so there
 *   is no tagged twin to wire up.
 *
 * `FEAT_VHE`'s `E2H=1` host-OS mode (where the `*E1*` instructions
 * issued from EL2 instead address the EL2&0 regime) is intentionally
 * out of scope here, same conservative-scope precedent as
 * `arm64/mmu_regs.hpp`'s `hcr_el2::e2h()` accessor being read-only
 * documentation, not a behavior switch baked into this header.
 *
 * ## `guest_tlb_space`: stage-2, current VMID only
 *
 * `IPAS2E1` takes an Intermediate Physical Address but **no VMID
 * operand at all** -- like AArch32's `TLBIIPAS2`, it always invalidates
 * for whichever VMID the current `VTTBR_EL2` already holds (see
 * `arm64/hyp_vm_regs.hpp`'s `vm_hyp_el2_state::vttbr_el2`). There is
 * therefore no ISA-level way to implement a `flush_tag`/`flush_page_tag`
 * that targets an arbitrary, caller-chosen VMID without also
 * reprogramming `VTTBR_EL2` as a side effect this header has no
 * business performing silently -- so only the untagged members are
 * provided here:
 *
 * - `flush_all()` -- `TLBI VMALLS12E1` ("All, stage 1 and 2, EL1"):
 *   invalidates the current VMID's combined stage-1+stage-2 entries,
 *   slightly more than a pure stage-2-only flush -- always safe, per
 *   the precision-fallback philosophy in `arch/tlb_flush.hpp`'s file
 *   docs, since there is no separate "stage 2 only, every IPA, current
 *   VMID" encoding to prefer instead.
 * - `flush_page(ipa)` -- `TLBI IPAS2E1, Xt`, `Xt[39:0]` = `IPA[51:12]`
 *   (i.e. `ipa >> 12`); the `NS`/`TTL`/`IPA[55:52]` fields (`FEAT_RME`/
 *   `FEAT_SEL2`/`FEAT_TTL`/`FEAT_LPA2`) are left `0` (their "no
 *   information supplied"/current-security-state-implied reset value),
 *   same scope decision as everywhere else in this library that skips
 *   `FEAT_RME` (see `arm64/pte_stage1.hpp`, `arm64/pte_stage2.hpp`,
 *   `vmsa_pte_fields.hpp`).
 *
 * `tlb_flusher::flush_tag`/`flush_page_tag` for `guest_tlb_space`
 * therefore fall back to the untagged `flush_all`/`flush_page`
 * automatically, exactly as `arch/tlb_flush.hpp` documents for any
 * `Space`/operation a trait doesn't implement -- correct (the caller's
 * own `VTTBR_EL2` write already scoped the "tag"), just not literally a
 * hardware tag match.
 *
 * ## Not modeled here: `secure_tlb_space`/`nonsecure_tlb_space`, RME, EL3
 *
 * Every instruction above has **no Secure/Non-secure/Realm selector
 * operand** -- which world they affect is determined entirely by
 * `SCR_EL3.NS` (or `SCR_EL3.{NSE,NS}` with `FEAT_RME`) at the time the
 * instruction executes, exactly like `arm64/mmu_regs.hpp`'s `scr_el3`
 * world switch. A caller needing to flush a specific world's
 * `process_tlb_space` entries simply executes `tlb_tag`'s ops while
 * actually running in that world -- this header cannot and does not
 * offer a `Space` selector the ISA itself has no operand for.
 * `FEAT_RME`'s Root/Realm worlds (`TLBI ... EL3` variants, Granule
 * Protection Table invalidation) and EL3/Monitor-mode TLB maintenance
 * in general are out of scope entirely, matching the `FEAT_RME`
 * exclusion already documented in `arm64/pte_stage1.hpp`,
 * `arm64/pte_stage2.hpp`, and `vmsa_pte_fields.hpp`; `root_tlb_space`/
 * `realm_tlb_space`/`gpt_tlb_space` are therefore left unimplemented
 * for `arm64::tlb_tag` (dispatcher calls for them are a compile-time
 * `static_assert`).
 *
 * ## Range flushes (`FEAT_TLBIRANGE`): not wired up, falls back automatically
 *
 * `TLBI RVAE1`/`RIPAS2E1` and friends (`FEAT_TLBIRANGE`, Armv8.4+) are
 * an optional feature needing its own runtime check, and their `Xt`
 * `BaseADDR`/`SCALE`/`NUM`/`TG` range-descriptor encoding is
 * substantially more involved than a single address/ASID pair --
 * deliberately left out of scope for this header, same conservative
 * stance as `arch/riscv/tlb_flush.hpp`/`arch/x86/tlb_flush.hpp` taking
 * re: range ops. `tlb_flusher` already falls back to `flush_tag`/
 * `flush_all` for `flush_range`/`flush_range_tag` automatically.
 *
 * Only compiled on a real AArch64 target (`__aarch64__`); on every
 * other host this header is an intentional no-op so it stays
 * header-check-clean cross-compiled from any machine, matching the
 * gating convention established by `arm64/irq_guard.hpp`.
 */

#if defined(__aarch64__)

#include <structo/arch/tlb_flush.hpp>

#include <cstdint>
#include <type_traits>

namespace structo::arch::arm64 {

/** @brief `tlb_flush_traits<arm64::tlb_tag>`'s `Arch` tag -- CP15-successor `TLBI` ops, every AArch64 core. Broadcast
 * is mandatory baseline A64, always available. See the @file docs. */
struct tlb_tag {};

} // namespace structo::arch::arm64

template <> struct structo::arch::tlb_flush_traits<structo::arch::arm64::tlb_tag> {
  static constexpr bool supports_broadcast = true;

  // --- process_tlb_space: VMALLE1 / ASIDE1 / VAAE1 / VAE1 -----------------

  template <typename Space>
  static auto flush_all() noexcept -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    asm volatile("tlbi vmalle1" ::: "memory");
  }

  template <typename Space>
  static auto flush_tag(std::uint64_t asid) noexcept -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    std::uint64_t xt = (asid & 0xffffull) << 48;
    asm volatile("tlbi aside1, %0" ::"r"(xt) : "memory");
  }

  template <typename Space>
  static auto flush_page(std::uint64_t vaddr) noexcept -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    std::uint64_t xt = (vaddr >> 12) & 0xfffffffffffull;
    asm volatile("tlbi vaae1, %0" ::"r"(xt) : "memory");
  }

  template <typename Space>
  static auto flush_page_tag(std::uint64_t vaddr, std::uint64_t asid) noexcept
      -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    std::uint64_t xt = ((asid & 0xffffull) << 48) | ((vaddr >> 12) & 0xfffffffffffull);
    asm volatile("tlbi vae1, %0" ::"r"(xt) : "memory");
  }

  // --- hypervisor_tlb_space: ALLE2 / VAE2 ---------------------------------

  template <typename Space>
  static auto flush_all() noexcept -> std::enable_if_t<std::is_same_v<Space, hypervisor_tlb_space>> {
    asm volatile("tlbi alle2" ::: "memory");
  }

  template <typename Space>
  static auto flush_page(std::uint64_t vaddr) noexcept
      -> std::enable_if_t<std::is_same_v<Space, hypervisor_tlb_space>> {
    std::uint64_t xt = (vaddr >> 12) & 0xfffffffffffull;
    asm volatile("tlbi vae2, %0" ::"r"(xt) : "memory");
  }

  // --- guest_tlb_space: VMALLS12E1 / IPAS2E1 (current VMID only) ----------

  template <typename Space>
  static auto flush_all() noexcept -> std::enable_if_t<std::is_same_v<Space, guest_tlb_space>> {
    asm volatile("tlbi vmalls12e1" ::: "memory");
  }

  template <typename Space>
  static auto flush_page(std::uint64_t ipa) noexcept -> std::enable_if_t<std::is_same_v<Space, guest_tlb_space>> {
    std::uint64_t xt = (ipa >> 12) & 0xffffffffffull; // IPA[51:12]
    asm volatile("tlbi ipas2e1, %0" ::"r"(xt) : "memory");
  }

  // --- Broadcast: process_tlb_space ---------------------------------------

  template <typename Space>
  static auto flush_all_broadcast() noexcept -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    asm volatile("tlbi vmalle1is" ::: "memory");
  }

  template <typename Space>
  static auto flush_tag_broadcast(std::uint64_t asid) noexcept
      -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    std::uint64_t xt = (asid & 0xffffull) << 48;
    asm volatile("tlbi aside1is, %0" ::"r"(xt) : "memory");
  }

  template <typename Space>
  static auto flush_page_broadcast(std::uint64_t vaddr) noexcept
      -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    std::uint64_t xt = (vaddr >> 12) & 0xfffffffffffull;
    asm volatile("tlbi vaae1is, %0" ::"r"(xt) : "memory");
  }

  template <typename Space>
  static auto flush_page_tag_broadcast(std::uint64_t vaddr, std::uint64_t asid) noexcept
      -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    std::uint64_t xt = ((asid & 0xffffull) << 48) | ((vaddr >> 12) & 0xfffffffffffull);
    asm volatile("tlbi vae1is, %0" ::"r"(xt) : "memory");
  }

  // --- Broadcast: hypervisor_tlb_space -------------------------------------

  template <typename Space>
  static auto flush_all_broadcast() noexcept -> std::enable_if_t<std::is_same_v<Space, hypervisor_tlb_space>> {
    asm volatile("tlbi alle2is" ::: "memory");
  }

  template <typename Space>
  static auto flush_page_broadcast(std::uint64_t vaddr) noexcept
      -> std::enable_if_t<std::is_same_v<Space, hypervisor_tlb_space>> {
    std::uint64_t xt = (vaddr >> 12) & 0xfffffffffffull;
    asm volatile("tlbi vae2is, %0" ::"r"(xt) : "memory");
  }

  // --- Broadcast: guest_tlb_space (current VMID only, see @file docs) -----

  template <typename Space>
  static auto flush_all_broadcast() noexcept -> std::enable_if_t<std::is_same_v<Space, guest_tlb_space>> {
    asm volatile("tlbi vmalls12e1is" ::: "memory");
  }

  template <typename Space>
  static auto flush_page_broadcast(std::uint64_t ipa) noexcept
      -> std::enable_if_t<std::is_same_v<Space, guest_tlb_space>> {
    std::uint64_t xt = (ipa >> 12) & 0xffffffffffull;
    asm volatile("tlbi ipas2e1is, %0" ::"r"(xt) : "memory");
  }
};

#endif // defined(__aarch64__)
