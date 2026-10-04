// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file hypercall_dispatcher.hpp
 * @brief `structo::hypervisor::hypercall_dispatcher<Handlers, MaxCalls>`:
 * a thin, allocation-free dispatch point from a guest-issued hypercall
 * number (decoded from whatever register the platform's calling
 * convention places it in -- `X0` under Arm's SMC Calling Convention
 * (SMCCC), `RAX`/`EAX` for a KVM- or Xen-style `VMCALL`/`VMMCALL`-based
 * paravirtual ABI, ...) to guest- and platform-specific handling code.
 *
 * ## Same shape as `vm_exit_dispatcher`, for a guest-initiated trap
 * instead of a hardware-initiated one
 *
 * A hypercall is, mechanically, just another VM-exit (`VMCALL`/
 * `VMMCALL`/`HVC`/`SMC` all trap exactly like any other instruction a
 * VMCS/VMCB/`HCR_EL2` configuration routes to the hypervisor) --
 * `hypercall_dispatcher` deliberately mirrors `vm_exit_dispatcher`'s
 * shape (a `Handlers` trait resolved once at compile time, no runtime
 * registration table -- see that header's docs for the full rationale,
 * which applies unchanged here) rather than inventing a different one,
 * but is kept as its own header/type because the two dispatch keys mean
 * different things to a caller: a VM-exit reason is hardware-decoded and
 * architecture-fixed, while a hypercall number is guest-chosen and ABI-
 * defined (SMCCC's function-ID encoding, a hypervisor-specific PV ABI,
 * ...) -- distinct enough namespaces that collapsing them into one
 * template parameterized by "which kind of number" would only make call
 * sites harder to read for no shared implementation benefit beyond what
 * both already get from sharing the same shape.
 *
 * `Handlers` must provide:
 * - `using context_type = ...;` -- whatever per-call state the handler
 *   set needs (typically at least the owning vCPU and the guest's own
 *   argument registers, plus a place to stash the return value(s) the
 *   calling convention expects back in specific registers on return).
 * - `static reloco::result<void> invoke(std::uint64_t call_number, context_type &ctx) noexcept;`
 *
 * @code
 * // Arm SMCCC-style: function ID already masked/shifted by the caller
 * // out of X0 before reaching this dispatcher; args in X1-X6, result in X0-X3.
 * enum smccc_function_id : std::uint64_t { psci_cpu_suspend = 0xC4000001, psci_cpu_off = 0x84000002 };
 *
 * struct my_hypercall_context {
 *   my_vcpu &vcpu;
 *   std::uint64_t args[6];
 *   std::uint64_t result[4];
 * };
 *
 * struct my_hypercall_handlers {
 *   using context_type = my_hypercall_context;
 *
 *   static reloco::result<void> invoke(std::uint64_t call_number, context_type &ctx) noexcept {
 *     switch (call_number) {
 *     case psci_cpu_suspend: return handle_psci_cpu_suspend(ctx.vcpu, ctx.args, ctx.result);
 *     case psci_cpu_off: return handle_psci_cpu_off(ctx.vcpu, ctx.result);
 *     default: return reloco::unexpected(reloco::error::unsupported_operation);
 *     }
 *   }
 * };
 *
 * using my_hypercalls = structo::hypervisor::hypercall_dispatcher<my_hypercall_handlers, 256>;
 *
 * // In the actual HVC/SMC trap path, right after decoding the function ID out of X0:
 * my_hypercall_context ctx{vcpu, {x1, x2, x3, x4, x5, x6}, {}};
 * const reloco::result<void> outcome = my_hypercalls::dispatch(function_id, ctx);
 * @endcode
 */

#include <reloco/array.hpp>
#include <reloco/detail/assert.hpp>
#include <reloco/error.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace structo::hypervisor {

/**
 * @brief Boot-time-fixed hypercall dispatch table: forwards every call
 * to `Handlers::invoke`, additionally maintaining a lock-free per-call-
 * number hit counter for `call_number < MaxCalls`.
 * @tparam Handlers Resolves the fixed, boot-time-configured handler for
 * a given call number; see the file-level docs above for the full
 * contract and an example.
 * @tparam MaxCalls Size of the per-call-number counter table. An out-of-
 * range @p call_number passed to `dispatch()` is still forwarded to
 * `Handlers::invoke` -- it is simply not counted -- so this only needs
 * to cover the call numbers a caller actually wants profiled, not the
 * entire range the ABI could theoretically carry (SMCCC function IDs
 * span the full 32/64-bit register width; no table could size that).
 */
template <typename Handlers, std::size_t MaxCalls> class hypercall_dispatcher {
public:
  using handlers_type = Handlers;
  using context_type = typename Handlers::context_type;

  static inline constexpr std::size_t max_calls = MaxCalls;

  /**
   * @brief Bumps this call number's counter (if `call_number < max_calls`)
   * then forwards unconditionally to `Handlers::invoke(call_number, ctx)`
   * -- an out-of-range or unrecognized call number is still dispatched,
   * just not counted, since `Handlers::invoke` (not this bookkeeping) is
   * what decides whether it is actually an error (and fills in whatever
   * error-return convention the guest ABI expects, e.g. SMCCC's
   * `SMCCC_RET_NOT_SUPPORTED` in `ctx.result[0]`).
   */
  static reloco::result<void> dispatch(std::uint64_t call_number, context_type &ctx) noexcept {
    // Bounds-checked and narrowed to `std::size_t` by hand, rather than
    // handing the raw `std::uint64_t` straight to `array::operator[]`,
    // so a 32-bit host (where `std::size_t` is narrower than
    // `std::uint64_t`) never silently truncates an out-of-range call
    // number -- the `< MaxCalls` comparison below always widens
    // `MaxCalls` up to `std::uint64_t`, never narrows `call_number` down.
    if (call_number < MaxCalls) {
      s_counts[static_cast<std::size_t>(call_number)].fetch_add(1, std::memory_order_relaxed);
    }
    return Handlers::invoke(call_number, ctx);
  }

  /** @brief Number of times `dispatch()` has observed this exact @p call_number. Checked: traps if out of range. */
  [[nodiscard]] static std::uint64_t count(std::uint64_t call_number) noexcept {
    RELOCO_ASSERT(call_number < MaxCalls, "hypercall_dispatcher: call_number out of range");
    return s_counts[static_cast<std::size_t>(call_number)].load(std::memory_order_relaxed);
  }

  /** @brief Fallible `count()`: `error::out_of_bounds` instead of trapping when `call_number >= max_calls`. */
  [[nodiscard]] static reloco::result<std::uint64_t> try_count(std::uint64_t call_number) noexcept {
    if (call_number >= MaxCalls) {
      return reloco::unexpected(reloco::error::out_of_bounds);
    }
    return s_counts[static_cast<std::size_t>(call_number)].load(std::memory_order_relaxed);
  }

private:
  static inline reloco::array<std::atomic<std::uint64_t>, MaxCalls> s_counts{};
};

} // namespace structo::hypervisor
