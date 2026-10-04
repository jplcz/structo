// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file vm_exit_dispatcher.hpp
 * @brief `structo::hypervisor::vm_exit_dispatcher<Handlers, MaxReasons>`:
 * a thin, allocation-free dispatch point from a raw hardware VM-exit/
 * trap reason code (Intel VMX `VM_EXIT_REASON`, AMD SVM `#VMEXIT` code,
 * Arm `ESR_EL2.EC`, RISC-V `scause`, ...) to architecture- and guest-
 * specific handling code.
 *
 * ## Same shape as `arch::ipi_dispatcher`'s `Handlers`, for the same reason
 *
 * This follows the exact "`Handlers` trait resolved once at compile
 * time, no runtime registration table" shape `arch/ipi_dispatcher.hpp`
 * documents in full under "IPIs are boot-time-fixed, never configured
 * dynamically" -- a VM-exit's handler set is just as fixed for the life
 * of a hypervisor build, and runs in exactly the kind of context
 * (synchronously, on the vCPU's own core, often non-reentrant, possibly
 * with interrupts still masked) where a mutable handler table protected
 * by a lock would be actively dangerous: a nested trap taking that same
 * lock, or a handler blocking indefinitely while the rest of the table
 * is briefly unavailable, is not something a trap path can tolerate.
 *
 * `vm_exit_dispatcher` itself carries no handling logic at all --
 * `Handlers::invoke(reason, ctx)` is the entire dispatch, written as a
 * plain `switch` (or whatever the caller prefers) over the
 * architecture's own reason enum; this header only adds the one thing
 * worth sharing across every architecture's own trap path: optional,
 * lock-free per-reason exit counters (`count()`/`try_count()`), useful
 * for profiling which exit reason actually dominates a workload's
 * VM-exit rate, without `Handlers::invoke` needing to instrument every
 * `case` by hand.
 *
 * `Handlers` must provide:
 * - `using context_type = ...;` -- whatever per-exit state the handler
 *   set needs (typically at least the owning vCPU and any exit-
 *   qualification/instruction-length fields the architecture reports
 *   alongside the raw reason code).
 * - `static reloco::result<void> invoke(std::uint32_t reason, context_type &ctx) noexcept;`
 *
 * @code
 * enum vmx_exit_reason : std::uint32_t { exit_hlt = 12, exit_cpuid = 10, exit_io_instruction = 30 };
 *
 * struct my_vcpu_exit_context {
 *   my_vcpu &vcpu;
 *   std::uint64_t exit_qualification;
 * };
 *
 * struct my_vm_exit_handlers {
 *   using context_type = my_vcpu_exit_context;
 *
 *   static reloco::result<void> invoke(std::uint32_t reason, context_type &ctx) noexcept {
 *     switch (reason) {
 *     case exit_hlt: return handle_hlt(ctx.vcpu);
 *     case exit_cpuid: return handle_cpuid(ctx.vcpu);
 *     case exit_io_instruction: return handle_io(ctx.vcpu, ctx.exit_qualification);
 *     default: return reloco::unexpected(reloco::error::unsupported_operation);
 *     }
 *   }
 * };
 *
 * using my_vm_exits = structo::hypervisor::vm_exit_dispatcher<my_vm_exit_handlers, 64>;
 *
 * // In the actual VM-exit path, right after reading the hardware's raw reason code:
 * my_vcpu_exit_context ctx{vcpu, read_exit_qualification()};
 * const reloco::result<void> outcome = my_vm_exits::dispatch(read_exit_reason(), ctx);
 * @endcode
 */

#include <reloco/array.hpp>
#include <reloco/error.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace structo::hypervisor {

/**
 * @brief Boot-time-fixed VM-exit dispatch table: forwards every call to
 * `Handlers::invoke`, additionally maintaining a lock-free per-reason
 * hit counter for `reason < MaxReasons`.
 * @tparam Handlers Resolves the fixed, boot-time-configured handler for
 * a given reason; see the file-level docs above for the full contract
 * and an example.
 * @tparam MaxReasons Size of the per-reason counter table. An out-of-
 * range @p reason passed to `dispatch()` is still forwarded to
 * `Handlers::invoke` -- it is simply not counted -- so this only needs
 * to cover the reasons a caller actually wants profiled, not every
 * value the hardware could theoretically report.
 */
template <typename Handlers, std::size_t MaxReasons> class vm_exit_dispatcher {
public:
  using handlers_type = Handlers;
  using context_type = typename Handlers::context_type;

  static inline constexpr std::size_t max_reasons = MaxReasons;

  /**
   * @brief Bumps this reason's exit counter (if `reason < max_reasons`)
   * then forwards unconditionally to `Handlers::invoke(reason, ctx)` --
   * an out-of-range reason is still dispatched, just not counted, since
   * `Handlers::invoke` (not this bookkeeping) is what decides whether an
   * unrecognized reason is actually an error.
   */
  static reloco::result<void> dispatch(std::uint32_t reason, context_type &ctx) noexcept {
    if (reason < MaxReasons) {
      s_counts[reason].fetch_add(1, std::memory_order_relaxed);
    }
    return Handlers::invoke(reason, ctx);
  }

  /** @brief Number of times `dispatch()` has observed this exact @p reason. Checked: traps if out of range. */
  [[nodiscard]] static std::uint64_t count(std::uint32_t reason) noexcept {
    return s_counts[reason].load(std::memory_order_relaxed);
  }

  /** @brief Fallible `count()`: `error::out_of_bounds` instead of trapping when `reason >= max_reasons`. */
  [[nodiscard]] static reloco::result<std::uint64_t> try_count(std::uint32_t reason) noexcept {
    auto found = s_counts.try_at(reason);
    if (!found) {
      return reloco::unexpected(found.error());
    }
    return found->get().load(std::memory_order_relaxed);
  }

private:
  static inline reloco::array<std::atomic<std::uint64_t>, MaxReasons> s_counts{};
};

} // namespace structo::hypervisor
