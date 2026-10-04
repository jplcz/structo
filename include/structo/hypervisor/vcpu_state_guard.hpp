// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file vcpu_state_guard.hpp
 * @brief `structo::hypervisor::vcpu_entry_guard<Traits>`: an RAII guard that
 * loads a persistent, caller-owned vCPU register snapshot into the live
 * registers on construction, saves whatever those registers hold *back*
 * into that same snapshot on destruction, and restores the host's own
 * register values in between -- bracketing exactly the window a VM-entry
 * instruction (`VMLAUNCH`/`VMRESUME`, `VMRUN`, `ERET` into a lower,
 * non-secure EL1) and its matching VM-exit trap cover.
 *
 * ## Why this is not just another `world_switch_guard`
 *
 * `arch/world_switch_guard.hpp` already solves "save a group of live
 * registers, hand control elsewhere, restore exactly what was here
 * before" -- but it is symmetric: one snapshot, captured from whichever
 * side is currently live, written back to that same side on return. A
 * vCPU entry is deliberately *not* symmetric: the registers loaded on
 * entry (the guest's) are not the registers restored on exit (the
 * host's) -- two distinct, independently-owned snapshots are involved,
 * and the guest's snapshot must *outlive* any single entry/exit pair
 * (it is the vCPU's persistent architectural state, read again the next
 * time this same vCPU is entered, possibly after other vCPUs or the
 * host itself has run on this same core). `vcpu_entry_guard` is that
 * asymmetric, two-snapshot shape; reach for `world_switch_guard` instead
 * for the plain symmetric case (e.g. saving EL1 sysregs across a
 * TrustZone world switch where neither side keeps a separate persistent
 * copy).
 *
 * ## Why hardware needs this at all
 *
 * No mainstream virtualization extension swaps the *entire* register
 * file on a VM-entry/VM-exit:
 *
 * - **Intel VMX**: `VMLAUNCH`/`VMRESUME` load the guest's `RIP`, `RSP`,
 *   `RFLAGS`, and control/segment state from the VMCS guest-state area
 *   automatically, and a VM-exit reloads the equivalent host fields from
 *   the VMCS host-state area -- but **general-purpose registers (`RAX`-
 *   `R15`, aside from `RSP`) are not part of the VMCS at all**. Software
 *   must save the host's GPRs and load the guest's immediately around
 *   `VMLAUNCH`/`VMRESUME`, and reverse that on the VM-exit that follows
 *   -- exactly what KVM's `__vmx_vcpu_run` assembly stub does by hand
 *   for every entry.
 * - **AMD SVM**: `VMRUN` is broader -- its VMCB save-state area does
 *   cover `RAX`, `RSP`, `RIP`, `RFLAGS`, and control/segment state -- but
 *   the *other* GPRs (`RBX`, `RCX`, `RDX`, `RSI`, `RDI`, `RBP`, `R8`-
 *   `R15`) are, again, not part of the VMCB and still need manual
 *   save/restore around `VMRUN`, for the same reason.
 * - **Arm EL2**: entering a guest's EL1/EL0 (`ERET`) touches only the
 *   handful of banked/system registers EL2 itself manages explicitly
 *   (see `arch/world_switch_guard.hpp`'s own EL1-sysregs example) --
 *   the general-purpose register file `X0`-`X30` is shared, unbanked
 *   state a hypervisor's own trap-entry/trap-exit assembly must save
 *   and restore by hand on every world switch, identically in spirit to
 *   VMX/SVM's GPR handling above.
 *
 * `vcpu_entry_guard<Traits>` is a generic, reusable customization point
 * for exactly that recurring obligation, parameterized so any of the
 * three (or a port's own register subset -- GPRs only, GPRs plus FPU/
 * vector state, ...) can share the same guard shape.
 *
 * ## The customization point
 *
 * `Traits` supplies an opaque `state_type` plus the same two static
 * hooks `world_switch_guard<Traits>` uses -- `save()` (capture the live
 * register group into a fresh `state_type`) and `restore(const
 * state_type &)` (write a snapshot back into the live registers):
 * @code
 * struct vmx_gpr_traits {
 *   struct state_type {
 *     std::uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
 *     std::uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
 *   };
 *
 *   static state_type save() noexcept {
 *     state_type s{};
 *     // Real code: read each GPR out of whatever the trap entry stub
 *     // already pushed onto the stack (or a dedicated save area), not
 *     // a live register read -- by the time C++ runs, the compiler, not
 *     // this trait, owns the physical registers.
 *     return s;
 *   }
 *
 *   static void restore(const state_type &s) noexcept {
 *     // Real code: write each field back into that same save area, so
 *     // the assembly stub's epilogue (VMLAUNCH/VMRESUME, or the final
 *     // `iret`/`sysret` back to host context) picks it up from there.
 *   }
 * };
 * @endcode
 *
 * ## Usage: bracket exactly one entry/exit pair, with the guest's
 * persistent snapshot owned elsewhere
 *
 * @code
 * struct my_vcpu {
 *   vmx_gpr_traits::state_type guest_gprs{}; // persists across entries
 *   // ... VMCS/VMCB handle, per-vCPU id, etc.
 * };
 *
 * void run_one_entry(my_vcpu &vcpu) {
 *   structo::hypervisor::vcpu_entry_guard<vmx_gpr_traits> guard(vcpu.guest_gprs);
 *   vmlaunch_or_vmresume(); // traps back in on the matching VM-exit
 * } // destructor: guest_gprs updated with whatever the guest left behind,
 *   // host's own GPRs restored exactly as this function found them
 * @endcode
 */

#include <type_traits>
#include <utility>

namespace structo::hypervisor {

// Forward declaration
template <typename Traits> class vcpu_entry_guard;

// -----------------------------------------------------------------------------
// Scoped RAII Guard
// -----------------------------------------------------------------------------
/**
 * @brief RAII guard bracketing one vCPU VM-entry/VM-exit pair: loads a
 * caller-owned, persistent guest register snapshot into the live
 * registers on construction (after first capturing the host's own live
 * values), then writes the live registers back into that same guest
 * snapshot on destruction and restores the host's captured values.
 * @tparam Traits Register-group policy providing `state_type`, `save()`
 * (returns a fresh snapshot of the live registers), and `restore(const
 * state_type &)` (writes a snapshot back to the live registers). See
 * the file-level docs above for the full contract and an example.
 */
template <typename Traits> class [[nodiscard]] vcpu_entry_guard {
public:
  using traits_type = Traits;
  using state_type = typename Traits::state_type;

  /**
   * @brief Captures the host's live register group, then loads
   * @p guest_state into the live registers. @p guest_state must outlive
   * this guard -- it is written back into on destruction/`unlock()`, so
   * it is typically a field inside the owning vCPU's own long-lived
   * object, not a temporary.
   */
  explicit vcpu_entry_guard(state_type &guest_state) noexcept
      : m_guest_state(&guest_state), m_host_state(Traits::save()), m_armed(true) {
    Traits::restore(guest_state);
  }

  /** @brief Saves the live (guest) registers back into the persistent snapshot, then restores the host's own. */
  ~vcpu_entry_guard() noexcept {
    if (m_armed) {
      *m_guest_state = Traits::save();
      Traits::restore(m_host_state);
    }
  }

  vcpu_entry_guard(const vcpu_entry_guard &) = delete;
  vcpu_entry_guard &operator=(const vcpu_entry_guard &) = delete;

  /** @brief Transfers ownership of the captured host snapshot and guest-state binding; `other` is left disarmed. */
  vcpu_entry_guard(vcpu_entry_guard &&other) noexcept
      : m_guest_state(other.m_guest_state), m_host_state(std::move(other.m_host_state)),
        m_armed(std::exchange(other.m_armed, false)) {}

  /** @brief Restores this guard's own state first, then takes over `other`'s; `other` is left disarmed. */
  vcpu_entry_guard &operator=(vcpu_entry_guard &&other) noexcept {
    if (this != &other) {
      if (m_armed) {
        *m_guest_state = Traits::save();
        Traits::restore(m_host_state);
      }
      m_guest_state = other.m_guest_state;
      m_host_state = std::move(other.m_host_state);
      m_armed = std::exchange(other.m_armed, false);
    }
    return *this;
  }

  /**
   * @brief Early explicit save-guest/restore-host before scope exit.
   * Idempotent: a second call (or destruction afterwards) is a no-op.
   */
  void unlock() noexcept {
    if (m_armed) {
      *m_guest_state = Traits::save();
      Traits::restore(m_host_state);
      m_armed = false;
    }
  }

  /** @brief Whether this guard still owns an un-saved-back entry (i.e. not yet `unlock()`ed or moved-from). */
  [[nodiscard]] constexpr bool is_armed() const noexcept { return m_armed; }

  /** @brief The host's register snapshot, captured on construction and pending restoration. */
  [[nodiscard]] constexpr const state_type &host_state() const noexcept { return m_host_state; }

private:
  state_type *m_guest_state;
  state_type m_host_state{};
  bool m_armed{false};
};

// -----------------------------------------------------------------------------
// Functional Entry Executor (`with_vcpu_entry`)
// -----------------------------------------------------------------------------
/**
 * @brief Executes a callable with @p guest_state loaded live for its
 * duration, saving it back and restoring the host's own registers on
 * return.
 *
 * Automatically inspects the invocable:
 *   - `[](auto &guard) { ... }` -> receives the `vcpu_entry_guard<Traits>&`
 *   - `[]() { ... }`            -> receives no args
 *
 * @tparam Traits Register-group policy forwarded to the underlying `vcpu_entry_guard<Traits>`.
 * @tparam F      Callable type; invoked with whichever of the two forms above it accepts.
 * @param guest_state Persistent per-vCPU snapshot; must outlive this call.
 * @param f Callable to invoke while the guest's registers are loaded live.
 * @return Whatever `f` returns, forwarded unchanged.
 */
template <typename Traits, typename F>
decltype(auto) with_vcpu_entry(typename Traits::state_type &guest_state, F &&f) noexcept {
  vcpu_entry_guard<Traits> guard(guest_state);

  if constexpr (std::is_invocable_v<F, vcpu_entry_guard<Traits> &>) {
    return f(guard);
  } else {
    return f();
  }
}

} // namespace structo::hypervisor
