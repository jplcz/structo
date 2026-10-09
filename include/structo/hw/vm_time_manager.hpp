// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file vm_time_manager.hpp
 * @brief `structo::hw::vm_time_manager<Traits>`: a hypervisor-side
 * helper that computes and applies the raw hardware offset a guest's
 * virtual free-running counter needs, relative to the host's own
 * counter (`time_source_ref`, `time_source_ref.hpp`), so a vCPU's
 * timer baseline can be set at reset, preserved across a scheduling
 * pause, or re-established after live migration.
 *
 * ## What this is not
 *
 * This is unrelated to (though it composes with) @ref time_manager
 * (`time_manager.hpp`): `time_manager` samples a free-running counter to
 * maintain the *host's own* monotonic/realtime "now", optionally
 * published into a `vdso_clock_page` for host user-space readers.
 * `vm_time_manager` instead programs the architecture register that
 * determines what a *guest* vCPU's own counter reads when it executes
 * the same "read the virtual counter" instruction a native kernel
 * would -- it never reads or republishes a clock of its own, and it
 * does not run inside the guest.
 *
 * ## The core primitive: one offset, computed the same way on every architecture
 *
 * Every architecture `structo` targets exposes a guest-visible virtual
 * counter whose value is some function of the host's physical counter
 * and one hypervisor-programmed per-vCPU offset register:
 *
 * | Architecture | Guest-visible register | Offset register | Native relationship |
 * |---|---|---|---|
 * | ARMv8-A/ARMv9-A | `CNTVCT_EL0` | `CNTVOFF_EL2` | `guest = host - CNTVOFF_EL2` |
 * | x86-64 (Intel VMX) | `RDTSC`/`RDTSCP` | VMCS `TSC_OFFSET` | `guest = host + TSC_OFFSET` (ignoring the optional VMCS
 * `TSC_MULTIPLIER` scaling field -- see the note below) | | RISC-V (H-extension) | `time` CSR (VS-level) |
 * `htimedelta`/`htimedeltah` | `guest = host + htimedelta` |
 *
 * Two of the three add the offset; ARM subtracts it. Rather than push
 * that sign difference onto every call site, `vm_time_manager` fixes
 * one canonical convention -- **`guest = host + offset`** -- for the
 * `cycles` value it hands to @ref vm_timer_traits::apply_offset /
 * expects back from @ref vm_timer_traits::try_read_offset; a backend
 * whose native register uses the opposite convention (ARM) negates
 * once, at the point it touches the register, via `cycles`'s own
 * `wrapping_sub` (see the ARM example below) -- the arithmetic
 * `vm_time_manager` itself performs (`cycles::wrapping_add`/
 * `wrapping_sub`) is therefore the same modulo-2^64, never-fails
 * arithmetic the hardware register itself implements, with no
 * intermediate `reloco::duration` conversion and no possibility of a
 * spurious `error::integer_overflow`.
 *
 * @note VMX's `TSC_MULTIPLIER` (and the analogous per-vCPU frequency
 * scaling some ARM/RISC-V hypervisors implement in software by trapping
 * the counter read) lets a guest's virtual counter also run at a
 * *different rate* than the host's, not just a different baseline. This
 * header deliberately does not model that: @ref vm_timer_traits is an
 * offset-only customization point. A hypervisor presenting a scaled
 * guest frequency must apply that scaling itself (e.g. before/after
 * calling into this header, or by not using it at all for that vCPU) and
 * should keep `CNTFRQ_EL0`/`capabilities().clock_hz`/equivalent
 * consistent with whatever rate it actually presents.
 *
 * ## Multi-vCPU VMs: one `vm_time_manager`, one host time source, many `vcpu_handle`s
 *
 * A `vm_time_manager<Traits>` instance is bound to exactly one host counter (its constructor argument),
 * but every method takes a `vcpu_handle` identifying *which* vCPU's offset register to touch -- so the
 * right granularity is **one `vm_time_manager` per VM** (constructed once, e.g. alongside the VM itself),
 * reused across calls for every vCPU that VM owns, never one instance per vCPU. This mirrors @ref
 * time_manager's own `is_per_cpu` restriction (see `time_manager.hpp`'s @file-level docs) for exactly the
 * same reason: a guest SMP kernel assumes its vCPUs' virtual counters are mutually consistent -- a read on
 * one vCPU and a read on another, taken "at the same time", must agree (modulo normal cross-core skew),
 * exactly like native SMP hardware's `CNTVCT_EL0`/invariant TSC/`mtime` already guarantee across physical
 * cores. That guarantee only holds if every vCPU's offset was computed from the *same* globally-consistent
 * host counter (@ref time_source_capabilities::is_per_cpu `== false` -- see `time_source_ref.hpp`'s own
 * docs); binding two different `vm_time_manager` instances to two different, not-mutually-synchronized
 * counters (e.g. two per-core, non-invariant TSCs) for two vCPUs of the *same* VM would silently reintroduce
 * the exact cross-core skew problem a single, shared host counter exists to avoid.
 *
 * This does not mean every vCPU must share one offset register, or even be bound to the same baseline at
 * the same wall-clock moment -- @ref vcpu_handle is per-vCPU precisely so each vCPU's own register can be
 * programmed independently (they are, after all, physically separate `CNTVOFF_EL2`/VMCS/`htimedelta`
 * instances, one per vCPU). What must be shared is only the *host counter being read*, so that "vCPU A's
 * offset, computed from host reading X" and "vCPU B's offset, computed from host reading Y" both describe
 * offsets from the same underlying timeline. In practice this also makes "align every vCPU to the same
 * guest-visible baseline at VM boot" straightforward -- call @ref reset (or @ref rebind) once per vCPU, in
 * a tight loop, all through the one shared `vm_time_manager`:
 *
 * @code
 * structo::hw::vm_time_manager<my_vm_timer_traits> vm_clock(host_counter); // once per VM
 * for (my_vcpu_handle vcpu : vm.vcpus())
 *   (void)vm_clock.reset(vcpu); // every vCPU reads ~cycles{0} at (approximately) the same host instant
 * @endcode
 *
 * (The loop body's own execution time means later vCPUs in the loop are bound a handful of host cycles
 * later than earlier ones -- exactly the same bounded skew native multi-core boot/reset sequencing already
 * has to tolerate, not a gap this header introduces.)
 *
 * ## Customization point: `vm_timer_traits<Traits>`
 *
 * `Traits` is supplied by the embedding hypervisor and must define:
 *
 * @code
 * struct my_vm_timer_traits {
 *   // Opaque, cheap-to-copy handle identifying one vCPU's register context (a raw pointer, an index into a
 *   // table the hypervisor already owns, ...). Never dereferenced by vm_time_manager itself.
 *   using vcpu_handle = my_vcpu_handle;
 *
 *   // Programs the offset register so the vCPU's virtual counter reads `host + offset` (see the table/
 *   // convention above) from this point on. Writing an inactive vCPU's saved register context (rather than a
 *   // live system register) is fine and expected; vm_time_manager never assumes @p vcpu is the one currently
 *   // executing.
 *   static void apply_offset(vcpu_handle vcpu, structo::hw::cycles offset) noexcept;
 *
 *   // Reads back whatever apply_offset most recently programmed for @p vcpu, decoded into the same
 *   // `guest = host + offset` convention (an ARM backend must undo its own negation here).
 *   static reloco::result<structo::hw::cycles> try_read_offset(vcpu_handle vcpu) noexcept;
 * };
 * @endcode
 *
 * Neither function is ever called with a `Traits` that hasn't been
 * supplied -- there is no default/undefined primary template, unlike
 * `time_source_traits`/`hw_rng_traits`: `vm_time_manager<Traits>`
 * simply will not compile against an incomplete `Traits`, mirroring
 * `sync::irq_guard<Traits>`/`arch::lazy_context<Traits>`'s own
 * direct-duck-typing convention for a single-architecture-at-a-time
 * policy parameter (there is no type-erased `vm_timer_ref` -- a given
 * hypervisor build targets exactly one architecture's register layout).
 *
 * ## Example `Traits` (ARMv8-A/ARMv9-A, `CNTVOFF_EL2`)
 *
 * @code
 * // Hypervisor-defined: identifies one vCPU's saved EL2 system-register context.
 * struct arm_vcpu_handle { arm_vcpu_context *ctx; };
 *
 * struct arm_vm_timer_traits {
 *   using vcpu_handle = arm_vcpu_handle;
 *
 *   static void apply_offset(vcpu_handle vcpu, structo::hw::cycles offset) noexcept {
 *     // CNTVOFF_EL2 natively means "guest = host - CNTVOFF_EL2"; negate once here to match this header's
 *     // "guest = host + offset" convention.
 *     auto cntvoff = structo::hw::cycles{0}.wrapping_sub(offset);
 *     vcpu.ctx->cntvoff_el2 = cntvoff.raw(); // restored to the real CNTVOFF_EL2 register on the next vCPU entry
 *   }
 *
 *   static reloco::result<structo::hw::cycles> try_read_offset(vcpu_handle vcpu) noexcept {
 *     auto cntvoff = structo::hw::cycles{vcpu.ctx->cntvoff_el2};
 *     return structo::hw::cycles{0}.wrapping_sub(cntvoff);
 *   }
 * };
 * @endcode
 *
 * ## Example `Traits` (x86-64 Intel VMX, VMCS `TSC_OFFSET`)
 *
 * @code
 * // Hypervisor-defined: identifies one vCPU's VMCS (e.g. wraps the physical address VMPTRLD expects).
 * struct vmx_vcpu_handle { vmcs_region *vmcs; };
 *
 * struct vmx_vm_timer_traits {
 *   using vcpu_handle = vmx_vcpu_handle;
 *
 *   static void apply_offset(vcpu_handle vcpu, structo::hw::cycles offset) noexcept {
 *     // VMCS TSC_OFFSET natively means "guest = host + TSC_OFFSET" -- already this header's convention, no
 *     // sign flip needed. vmcs_field_write64 is hypervisor-supplied (VMPTRLD + VMWRITE, or a direct write
 *     // into the VMCS's cached shadow if this vCPU is not currently loaded).
 *     vmcs_field_write64(vcpu.vmcs, VMCS_TSC_OFFSET, offset.raw());
 *   }
 *
 *   static reloco::result<structo::hw::cycles> try_read_offset(vcpu_handle vcpu) noexcept {
 *     return structo::hw::cycles{vmcs_field_read64(vcpu.vmcs, VMCS_TSC_OFFSET)};
 *   }
 * };
 * @endcode
 *
 * ## Example `Traits` (RISC-V H-extension, `htimedelta`/`htimedeltah`)
 *
 * @code
 * // Hypervisor-defined: identifies one vCPU's saved HS-level CSR context.
 * struct riscv_vcpu_handle { riscv_vcpu_context *ctx; };
 *
 * struct riscv_vm_timer_traits {
 *   using vcpu_handle = riscv_vcpu_handle;
 *
 *   static void apply_offset(vcpu_handle vcpu, structo::hw::cycles offset) noexcept {
 *     // htimedelta natively means "guest = host + htimedelta" -- already this header's convention.
 *     vcpu.ctx->htimedelta = offset.raw(); // restored via csrw htimedelta on the next hart's VS-mode entry
 *   }
 *
 *   static reloco::result<structo::hw::cycles> try_read_offset(vcpu_handle vcpu) noexcept {
 *     return structo::hw::cycles{vcpu.ctx->htimedelta};
 *   }
 * };
 * @endcode
 */

#include <structo/hw/clock_cycles.hpp>
#include <structo/hw/time_source_ref.hpp>

#include <reloco/error.hpp>
#include <reloco/span.hpp>

#include <cstddef>
#include <iterator>

namespace structo {
namespace hw {

/**
 * @brief Hypervisor-side helper: computes and applies the `guest = host + offset` register @ref
 * vm_timer_traits "Traits" needs so one vCPU's virtual counter reads a chosen baseline, relative to the
 * host's own free-running counter. See the @file-level docs for the full architecture table, the
 * `Traits` contract, and worked ARM/x86/RISC-V examples.
 * @tparam Traits Supplies `vcpu_handle`, `apply_offset(vcpu_handle, cycles)`, and
 * `try_read_offset(vcpu_handle) -> result<cycles>`.
 */
template <typename Traits> class vm_time_manager {
public:
  /** @brief Opaque, hypervisor-defined per-vCPU handle type -- see the @file-level docs. */
  using vcpu_handle = typename Traits::vcpu_handle;

  /**
   * @brief Binds to the host's free-running counter every operation below samples.
   * @param host_counter The same (or an equally globally-consistent) counter the host's own @ref
   * time_manager is tracking. Must outlive this `vm_time_manager` and every copy of it, matching @ref
   * time_source_ref's own non-owning-handle convention. Marked `explicit`: binding a host counter is
   * always a deliberate step, never an implicit conversion.
   */
  constexpr explicit vm_time_manager(time_source_ref host_counter) noexcept : host_counter_(host_counter) {}

  /**
   * @brief Programs @p vcpu's offset register so its virtual counter reads @p guest_target *now* (at
   * whatever host instant this call samples) -- the single primitive every scenario in the @file-level
   * docs reduces to: a vCPU reset (`guest_target = cycles{0}`, or see @ref reset), resuming a
   * previously-paused vCPU without exposing the pause gap (`guest_target` = whatever @ref try_guest_now
   * returned just before descheduling it), or re-establishing a migrated vCPU's clock on a new host
   * (`guest_target` = the value captured on the old host, @p this bound to the new host's counter).
   * Fails only if sampling the host counter itself fails (see @ref time_source_ref::try_now); the
   * register write itself (@ref vm_timer_traits::apply_offset) is unconditional.
   */
  [[nodiscard]] result<void> rebind(vcpu_handle vcpu, cycles guest_target) const noexcept {
    auto host_now = host_counter_.try_now();
    if (!host_now)
      return unexpected(host_now.error());
    Traits::apply_offset(vcpu, guest_target.wrapping_sub(host_now.value()));
    return {};
  }

  /** @brief Convenience for the vCPU-reset case: @ref rebind with `guest_target = cycles{0}`. */
  [[nodiscard]] result<void> reset(vcpu_handle vcpu) const noexcept { return rebind(vcpu, cycles{0}); }

  /**
   * @brief What @p vcpu's virtual counter reads *right now*, computed from the host's current counter
   * reading plus whatever offset @ref rebind most recently programmed for it (via @ref
   * vm_timer_traits::try_read_offset) -- the snapshot a caller takes immediately before descheduling/
   * migrating @p vcpu, to later hand back to @ref rebind as `guest_target`. Fails if either the host
   * counter read or the offset read-back fails.
   */
  [[nodiscard]] result<cycles> try_guest_now(vcpu_handle vcpu) const noexcept {
    auto host_now = host_counter_.try_now();
    if (!host_now)
      return unexpected(host_now.error());
    auto offset = Traits::try_read_offset(vcpu);
    if (!offset)
      return unexpected(offset.error());
    return host_now.value().wrapping_add(offset.value());
  }

  /** @brief The bound host counter, e.g. to query `capabilities().clock_hz` for a `cycles`<->`duration`
   * conversion (`clock_cycles.hpp`) the caller needs to translate a wall-clock pause duration into the
   * `cycles` domain @ref rebind/@ref try_guest_now operate in. */
  [[nodiscard]] constexpr time_source_ref host_counter() const noexcept { return host_counter_; }

  /**
   * @brief Snapshots every vCPU in @p vcpus (see @ref try_guest_now) into the matching slot of @p
   * out_snapshots, for a true whole-VM pause (checkpoint, the stop-the-world gap during live migration,
   * ...) where **no vCPU of this VM is executing anywhere** -- pair with @ref resume_all so the pause is
   * invisible to the guest. See the @file-level docs' "paused"/"resumed" section for why this must *not*
   * be used for an individual vCPU's ordinary scheduling gaps (host-scheduler preemption of one vCPU
   * thread while the VM otherwise keeps running) -- only when every vCPU is simultaneously not running.
   * @tparam VCPUs Any range of @ref vcpu_handle usable in a range-`for` and with `std::size` (e.g.
   * `reloco::span<const vcpu_handle>`, a plain C array, or a hypervisor-owned intrusive/linked vCPU list
   * that tracks its own count -- no random access required, only `begin()`/`end()`/`size()`). Deduced,
   * forwarded, never copied/owned.
   * @param out_snapshots Caller-owned storage (never allocated here), one `cycles` slot per entry in @p
   * vcpus, in the same order.
   * @return `error::invalid_argument` if @p vcpus and @p out_snapshots differ in length; otherwise
   * whatever the first failing @ref try_guest_now call reports.
   */
  template <typename VCPUs>
  [[nodiscard]] result<void> pause_all(VCPUs &&vcpus, span<cycles> out_snapshots) const noexcept {
    if (std::size(vcpus) != out_snapshots.size())
      return unexpected(error::invalid_argument);
    std::size_t i = 0;
    for (auto &&vcpu : vcpus) {
      auto guest_now = try_guest_now(vcpu);
      if (!guest_now)
        return unexpected(guest_now.error());
      out_snapshots[i++] = guest_now.value();
    }
    return {};
  }

  /**
   * @brief The inverse of @ref pause_all: re-binds every vCPU in @p vcpus (see @ref rebind) to its
   * matching slot in @p snapshots, so collectively none of them observe any time having passed since @ref
   * pause_all captured that snapshot -- call this through a `vm_time_manager` bound to the *resuming*
   * host's counter, which may be a different instance than the one @ref pause_all was called through (the
   * live-migration case).
   * @tparam VCPUs See @ref pause_all.
   * @param snapshots The exact values @ref pause_all produced, in the same order as @p vcpus.
   * @return `error::invalid_argument` if @p vcpus and @p snapshots differ in length; otherwise whatever
   * the first failing @ref rebind call reports.
   */
  template <typename VCPUs>
  [[nodiscard]] result<void> resume_all(VCPUs &&vcpus, span<const cycles> snapshots) const noexcept {
    if (std::size(vcpus) != snapshots.size())
      return unexpected(error::invalid_argument);
    std::size_t i = 0;
    for (auto &&vcpu : vcpus) {
      auto rebound = rebind(vcpu, snapshots[i++]);
      if (!rebound)
        return unexpected(rebound.error());
    }
    return {};
  }

private:
  time_source_ref host_counter_;
};

} // namespace hw
} // namespace structo
