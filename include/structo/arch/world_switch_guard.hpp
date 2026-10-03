// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file world_switch_guard.hpp
 * @brief `structo::arch::world_switch_guard<Traits>`: an RAII guard that
 * saves a group of **non-banked** registers on entry and restores them
 * on exit, bracketing a window in which control (and those same
 * registers) may be handed to another security domain/"world" --
 * ARM TrustZone Secure/Non-secure, a Realm Management Extension (RME)
 * world switch, or any other context where two privilege domains share
 * one physical register that hardware does not automatically swap for
 * them.
 *
 * ## Banked vs. non-banked: not every register survives a world switch for free
 *
 * A TrustZone-capable ARMv7-A/AArch32 core hardware-**banks** several
 * PL1 registers (`SCTLR`, `TTBR0`/`TTBR1`, `TTBCR`, `CONTEXTIDR`, ... --
 * see `structo::arch::arm::sctlr` et al. in `arch/arm/mmu_regs.hpp`):
 * the Secure and Non-secure worlds each get their own private copy, and
 * a plain `MRC`/`MCR` always reaches whichever copy belongs to the
 * world currently executing, so a world switch leaves them untouched
 * from each world's own point of view -- nothing to save.
 *
 * AArch64 draws this line very differently: its EL1 system registers
 * (`SCTLR_EL1`, `TTBR0_EL1`/`TTBR1_EL1`, `TCR_EL1`, `MAIR_EL1`,
 * `CONTEXTIDR_EL1`, `VBAR_EL1`, `ELR_EL1`, `SPSR_EL1`, `FAR_EL1`,
 * `ESR_EL1`, `TPIDR_EL0`/`TPIDR_EL1`, ... -- see
 * `structo::arch::arm64::scr_el3`'s own docs in `arch/arm64/mmu_regs.hpp`)
 * have **no hardware-banked per-world copy at all**: `SCR_EL3.NS` only
 * ever selects which world the next lower exception level *runs as*,
 * never which physical register bank a plain `MRS`/`MSR` reaches. If
 * EL3 firmware does nothing, the incoming world sees whatever the
 * outgoing world last left in these registers -- wrong, and often a
 * direct Secure-to-Non-secure information leak. This is exactly why Arm
 * Trusted Firmware-A's EL3 runtime (BL31) carries a `cm_el1_sysregs_
 * context_save()`/`cm_el1_sysregs_context_restore()` pair (see
 * `el1_sysregs_t` in `include/lib/el3_runtime/context_el1.h`), called
 * on every Secure/Non-secure transition -- and, separately, an
 * analogous save/restore pair for the FP/SIMD register file
 * (`fpregs_context_t`), which is likewise shared, unbanked state.
 * `world_switch_guard<Traits>` is a generic, reusable customization
 * point for exactly that obligation, for either register group (or any
 * other shared, non-banked register set a caller's own world-switch
 * path needs to carry across the boundary).
 *
 * ## The customization point
 *
 * `Traits` supplies an opaque `state_type` plus two static hooks:
 * `save()` (capture every register this group covers into a fresh
 * `state_type`) and `restore(const state_type &)` (write a previously
 * captured snapshot back into those same registers). Neither hook knows
 * or cares which direction the world switch is going, or which world is
 * "ours" -- the guard only ever says "whatever is live right now, keep
 * it safe until I say otherwise", which is symmetric regardless of
 * which side of the switch is doing the saving:
 * @code
 * struct el1_sysregs_traits {
 *   struct state_type {
 *     std::uint64_t sctlr_el1, tcr_el1, ttbr0_el1, ttbr1_el1, mair_el1;
 *     std::uint64_t contextidr_el1, vbar_el1, far_el1, esr_el1;
 *     std::uint64_t tpidr_el0, tpidr_el1, tpidrro_el0, elr_el1, spsr_el1;
 *     // ... the rest of Arm Trusted Firmware-A's el1_sysregs_t.
 *   };
 *
 *   static state_type save() noexcept {
 *     state_type s{};
 *     s.sctlr_el1 = read_sctlr_el1(); // real MRS reads, one per field
 *     // ...
 *     return s;
 *   }
 *
 *   static void restore(const state_type &s) noexcept {
 *     write_sctlr_el1(s.sctlr_el1); // real MSR writes, one per field
 *     // ...
 *   }
 * };
 * @endcode
 *
 * ## Usage: bracket the handoff, not the other world's whole lifetime
 *
 * The guard's constructor captures the outgoing world's register values
 * (so they survive the trip); its destructor writes them back (so the
 * outgoing world sees exactly what it left behind, regardless of
 * whatever the other world did to those same physical registers while
 * it ran). The actual world switch itself -- `SMC`/`ERET`, a Realm
 * Management Extension `root_tlb_space`-flagged transition, or whatever
 * a caller's own monitor code does -- is a separate, explicit action
 * the caller performs *inside* the guard's scope; this header never
 * issues one itself, exactly as `tlb_flush.hpp`/`address_translate.hpp`
 * never decide *when* to flush or translate, only *how*:
 * @code
 * {
 *   structo::arch::world_switch_guard<el1_sysregs_traits> guard; // save() now
 *   enter_other_world(); // SMC/ERET -- the other world may run its own
 *                        // EL1 OS and freely overwrite every one of
 *                        // these registers for its own purposes
 * } // destructor: restore() -- our own EL1 context is exactly as we left it
 * @endcode
 *
 * Composing several independent register groups (e.g. EL1 sysregs and
 * FP/SIMD) is just nesting two guards, one per `Traits`, each restoring
 * only its own group -- never one monolithic `Traits` unless a caller
 * actually wants both saved/restored atomically together.
 */

#include <type_traits>
#include <utility>

namespace structo::arch {

// Forward declaration
template <typename Traits> class world_switch_guard;

// -----------------------------------------------------------------------------
// Scoped RAII Guard
// -----------------------------------------------------------------------------
/**
 * @brief RAII guard that captures a `Traits`-defined group of
 * non-banked registers on construction and writes the captured
 * snapshot back on destruction, bracketing a window in which those
 * registers may have been handed to -- and freely modified by -- another
 * security domain/world.
 * @tparam Traits Register-group policy providing `state_type`, `save()`
 * (returns a fresh snapshot of the live registers), and `restore(const
 * state_type &)` (writes a snapshot back to the live registers).
 */
template <typename Traits> class [[nodiscard]] world_switch_guard {
public:
  using traits_type = Traits;
  using state_type = typename Traits::state_type;

  /** @brief Captures the live register group's current values. */
  world_switch_guard() noexcept : m_state(Traits::save()), m_armed(true) {}

  /** @brief Restores the captured snapshot, unless already unlocked/moved-from. */
  ~world_switch_guard() noexcept {
    if (m_armed) {
      Traits::restore(m_state);
    }
  }

  world_switch_guard(const world_switch_guard &) = delete;
  world_switch_guard &operator=(const world_switch_guard &) = delete;

  /** @brief Transfers ownership of the captured snapshot; `other` is left disarmed (no-op on destruction). */
  world_switch_guard(world_switch_guard &&other) noexcept
      : m_state(std::move(other.m_state)), m_armed(std::exchange(other.m_armed, false)) {}

  /** @brief Restores this guard's own snapshot first, then takes over `other`'s state; `other` is left disarmed. */
  world_switch_guard &operator=(world_switch_guard &&other) noexcept {
    if (this != &other) {
      if (m_armed) {
        Traits::restore(m_state);
      }
      m_state = std::move(other.m_state);
      m_armed = std::exchange(other.m_armed, false);
    }
    return *this;
  }

  /**
   * @brief Early explicit restore before scope exit.
   * Idempotent: a second call (or destruction afterwards) is a no-op.
   */
  void unlock() noexcept {
    if (m_armed) {
      Traits::restore(m_state);
      m_armed = false;
    }
  }

  /** @brief Whether this guard still owns an un-restored snapshot (i.e. not yet `unlock()`ed or moved-from). */
  [[nodiscard]] constexpr bool is_armed() const noexcept { return m_armed; }

  /** @brief The captured snapshot, e.g. for diagnostics or to hand to a different `Traits::restore()` call site. */
  [[nodiscard]] constexpr const state_type &state() const noexcept { return m_state; }

private:
  state_type m_state{};
  bool m_armed{false};
};

// -----------------------------------------------------------------------------
// Functional World-Switch Executor (`with_world_switch`)
// -----------------------------------------------------------------------------
/**
 * @brief Executes a callable with the register group captured on entry,
 * restoring it on exit.
 *
 * Automatically inspects the invocable:
 *   - `[](auto &guard) { ... }` -> receives the `world_switch_guard<Traits>&`
 *   - `[]() { ... }`            -> receives no args
 *
 * @tparam Traits Register-group policy forwarded to the underlying `world_switch_guard<Traits>`.
 * @tparam F      Callable type; invoked with whichever of the two forms above it accepts.
 * @param f Callable to invoke while the captured snapshot is pending restoration.
 * @return Whatever `f` returns, forwarded unchanged.
 */
template <typename Traits, typename F> decltype(auto) with_world_switch(F &&f) noexcept {
  world_switch_guard<Traits> guard;

  if constexpr (std::is_invocable_v<F, world_switch_guard<Traits> &>) {
    return f(guard);
  } else {
    return f();
  }
}

} // namespace structo::arch
