// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file tlb_flush.hpp
 * @brief `structo::arch::riscv::tlb_tag`: the RISC-V
 * `structo::arch::tlb_flush_traits<Arch>` specialization, wiring
 * `structo::arch::tlb_flusher<riscv::tlb_tag>` up to the real
 * `SFENCE.VMA`/`HFENCE.GVMA` instructions.
 *
 * See `arch/tlb_flush.hpp` for the full architecture-agnostic
 * dispatcher/fallback design this specializes; only the RISC-V-specific
 * instruction selection is documented here.
 *
 * ## Scope: one tag, two Spaces
 *
 * Every operation below is on the single `riscv::tlb_tag` -- unlike
 * x86 (`arch/x86/tlb_flush.hpp`), RISC-V needs no separate tag per
 * optional CPU feature, since `SFENCE.VMA`/`HFENCE.GVMA` are both part
 * of the base privileged ISA (the latter gated only by the `H`
 * (Hypervisor) extension being present at all, which is an
 * all-or-nothing ISA choice a kernel already has to know about, not a
 * runtime-probed feature like x86's PCID/VMX):
 *
 * - `structo::arch::process_tlb_space`, tag = ASID -- ordinary
 *   stage-1 (or HS-mode, non-virtualized) address-translation cache
 *   entries, via `SFENCE.VMA rs1, rs2` (`rs1` = virtual address, `rs2`
 *   = ASID; either operand being `x0` means "for all" of that axis).
 * - `structo::arch::guest_tlb_space`, tag = VMID -- stage-2 (guest-
 *   physical-address) translation cache entries, via
 *   `HFENCE.GVMA rs1, rs2` (`rs1` = **guest physical address shifted
 *   right by 2 bits**, `rs2` = VMID; same "`x0` means all" convention).
 *   The right-shift-by-2 is not a typo: the RISC-V Hypervisor
 *   extension's `hgatp`-relative guest-physical address space is up to
 *   2 bits wider than `XLEN`, so the ISA defines `HFENCE.GVMA`'s address
 *   operand as the GPA already divided by 4, not the raw byte address
 *   `SFENCE.VMA` takes.
 *
 * `hfence.vvma` (guest-*virtual*-address translations, i.e. a guest
 * OS's own stage-1 entries as cached while running under `hgatp`) is
 * deliberately not wired up here: its ASID tag and implicit current-VMID
 * context do not cleanly map onto either `process_tlb_space` (that tag
 * means "no virtualization context" elsewhere in this vocabulary) or
 * `guest_tlb_space` (that tag means VMID, not ASID) without overloading
 * one of them to mean two different things depending on execution mode.
 * Add a dedicated Space tag first if this is ever needed, rather than
 * force a mismatched fit here.
 *
 * ## Broadcast: none -- RISC-V TLB fences are hart-local only
 *
 * Neither instruction has a broadcast/shareability-domain-wide form in
 * the base ISA: `supports_broadcast = false`, and no `..._broadcast`
 * member is defined at all. Invalidating a translation on every hart
 * that might have cached it is the caller's job, via the SBI
 * `sbi_remote_sfence_vma[_asid]`/`sbi_remote_hfence_gvma[_vmid]` calls
 * (software-mediated IPI-driven shootdown, not a hardware broadcast) --
 * exactly the caller obligation `arch/tlb_flush.hpp`'s file docs
 * describe for any architecture with local-only hardware.
 *
 * ## Range flushes: not wired up, falls back automatically
 *
 * RISC-V's base privileged ISA has no range-invalidate instruction
 * (nothing like ARM `FEAT_TLBIRANGE`), so `flush_range`/
 * `flush_range_tag` are left undefined here -- `tlb_flusher` already
 * falls back to `flush_tag`/`flush_all` for them automatically.
 *
 * Only compiled on a real RISC-V target (`__riscv`); on every other
 * host this header is an intentional no-op so it stays
 * header-check-clean cross-compiled from any machine.
 */

#if defined(__riscv)

#include <structo/arch/tlb_flush.hpp>

#include <cstdint>
#include <type_traits>

namespace structo::arch::riscv {

/**
 * @brief `tlb_flush_traits<riscv::tlb_tag>`'s `Arch` tag -- see the
 * @file docs for exactly which `Space`s/instructions it wires up.
 */
struct tlb_tag {};

} // namespace structo::arch::riscv

template <> struct structo::arch::tlb_flush_traits<structo::arch::riscv::tlb_tag> {
  static constexpr bool supports_broadcast = false;

  // --- process_tlb_space: SFENCE.VMA rs1=vaddr, rs2=asid ------------------

  template <typename Space>
  static auto flush_all() noexcept -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    asm volatile("sfence.vma zero, zero" ::: "memory");
  }

  template <typename Space>
  static auto flush_tag(std::uint64_t asid) noexcept -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    asm volatile("sfence.vma zero, %0" ::"r"(asid) : "memory");
  }

  template <typename Space>
  static auto flush_page(std::uint64_t vaddr) noexcept -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    asm volatile("sfence.vma %0, zero" ::"r"(vaddr) : "memory");
  }

  template <typename Space>
  static auto flush_page_tag(std::uint64_t vaddr, std::uint64_t asid) noexcept
      -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    asm volatile("sfence.vma %0, %1" ::"r"(vaddr), "r"(asid) : "memory");
  }

  // --- guest_tlb_space: HFENCE.GVMA rs1=gpa>>2, rs2=vmid ------------------

  template <typename Space>
  static auto flush_all() noexcept -> std::enable_if_t<std::is_same_v<Space, guest_tlb_space>> {
    asm volatile("hfence.gvma zero, zero" ::: "memory");
  }

  template <typename Space>
  static auto flush_tag(std::uint64_t vmid) noexcept -> std::enable_if_t<std::is_same_v<Space, guest_tlb_space>> {
    asm volatile("hfence.gvma zero, %0" ::"r"(vmid) : "memory");
  }

  template <typename Space>
  static auto flush_page(std::uint64_t gpa) noexcept -> std::enable_if_t<std::is_same_v<Space, guest_tlb_space>> {
    asm volatile("hfence.gvma %0, zero" ::"r"(gpa >> 2) : "memory");
  }

  template <typename Space>
  static auto flush_page_tag(std::uint64_t gpa, std::uint64_t vmid) noexcept
      -> std::enable_if_t<std::is_same_v<Space, guest_tlb_space>> {
    asm volatile("hfence.gvma %0, %1" ::"r"(gpa >> 2), "r"(vmid) : "memory");
  }
};

#endif // defined(__riscv)
