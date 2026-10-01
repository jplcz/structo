// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file mm_asid_context.hpp
 * @brief `structo::arch::mm_asid_context<AsidTag, MaxCpus, CpuTag>`: the
 * per-address-space tracking fields a real `mm_context_t`-like structure
 * embeds -- a cached `tagged_asid<AsidTag>` plus a lock-free `cpu_mask`
 * of every core that may still hold a stale, tagged TLB entry for it --
 * turning `asid_allocator.hpp`'s "Bridging to an `mm_context`-like
 * structure" documentation cookbook into an actual, reusable,
 * unit-tested type instead of copy-pasted example code.
 *
 * `asid_allocator<Tag, MaxActive>` already tracks, internally, which
 * context is resident in each of its `MaxActive` "slot" (one per core)
 * -- but answering "which cores might still have a stale, tagged TLB
 * entry for *this specific* address space" from that requires an O(
 * `MaxActive`) scan of every slot (`asid_allocator.hpp`'s
 * `cpu_mask_for_mm()` example), and that scan is not safe to perform
 * without the allocator's own external lock, because it reads
 * `active_` entries of *every* slot, not just the ones belonging to this
 * address space. Page-table mutations (`munmap`/`mprotect`) need to ask
 * this question far more often than `allocate()`/`release()` are ever
 * called, so forcing every mapping change to contend on the same lock
 * that guards ASID *allocation* would be needless contention. Embedding
 * a dedicated `cpu_mask<CpuTag, MaxCpus>` directly in each address
 * space's own tracking structure answers the same question in O(
 * `MaxCpus / 64`) atomic loads (see `cpu_mask::atomic_snapshot()`) with
 * no lock at all.
 *
 * ## Synchronization contract
 *
 * `activate()`/`release()` forward directly to the (deliberately
 * unsynchronized) underlying `asid_allocator`, so they inherit its exact
 * thread-safety contract verbatim (see `asid_allocator.hpp`'s "Thread
 * safety" section): calls into the *same* `asid_allocator` instance --
 * whether through this `mm_asid_context` or any other -- must be
 * externally serialized (e.g. `structo::sync::irq_locked<asid_allocator<
 * ...>>` composed with a cross-core spinlock). `cpu_targets()` is the
 * deliberate exception: it is lock-free and safe to call concurrently
 * with an in-flight `activate()`/`release()` on another core (or even
 * this same instance), by design -- recomputing the TLB-shootdown target
 * set must not force contention on the allocator's lock. A snapshot
 * observed concurrently with an in-flight `activate()` may be a
 * subset that is about to grow (the activating core's bit may not be
 * visible yet) -- always safe to under-flush *before* that core has
 * actually loaded the ASID into hardware, since it cannot yet hold a
 * stale translation; never a superset shrinking unexpectedly, since
 * `activate()` only ever sets bits, never clears them (deactivation is a
 * pure no-op, by design -- see `asid_allocator.hpp`).
 *
 * ## What this intentionally does NOT add: a second generation counter
 *
 * The "generation" that tells a cached ASID it has gone stale is already
 * fully tracked inside `tagged_asid<Tag>`'s own opaque packed value and
 * compared internally by `asid_allocator::allocate()`'s `prev` fast path
 * -- adding a second, separate generation counter here would only
 * duplicate state the allocator already owns and protects under its
 * lock. `mm_asid_context` deliberately adds exactly one new piece of
 * state beyond the cached `tagged_asid<AsidTag>` already described in
 * `asid_allocator.hpp`'s cookbook: the lock-free, embedded cpu mask.
 *
 * @code
 * using allocator_type = structo::arch::asid_allocator<structo::arch::process_asid_tag, 8>;
 * using mm_context_type = structo::arch::mm_asid_context<structo::arch::process_asid_tag, 128>;
 *
 * struct mm_context {
 *   mm_context_type asid_ctx; // replaces the raw `context_id asid` field
 *   // ... page tables, VMAs, etc.
 * };
 *
 * // Activation: context switch INTO `mm` on logical core `core_id`.
 * // Caller must hold whatever lock serializes `allocator` (see above).
 * void activate_mm(allocator_type &allocator, mm_context &mm, std::size_t core_id) {
 *   auto flush_required = mm.asid_ctx.activate(allocator, core_id);
 *   if (!flush_required) { panic("ASID space exhausted"); } // unreachable in practice
 *   if (flush_required.value()) {
 *     arch_flush_tlb_all(); // global, non-tagged: a rollover just happened
 *   }
 *   arch_write_ttbr0_asid(mm.page_table_base, allocator.asid_of(mm.asid_ctx.asid()));
 * }
 *
 * // Deactivation: switching this core away from `old_mm` to run
 * // `new_mm` instead. There is NO separate "deactivate" call on
 * // `old_mm.asid_ctx` -- no allocator call, no TLB action, and
 * // `old_mm.asid_ctx`'s cached ASID plus its `cpu_targets()` bit for
 * // `core_id` are deliberately left exactly as they are. That is what
 * // lets a tagged TLB skip a flush entirely on this path: `old_mm`'s
 * // entries stay cached, tagged with its ASID, ready for an instant,
 * // flush-free `activate_mm()` later if it is scheduled back in. All of
 * // the actual work is just `new_mm`'s own `activate_mm()` above,
 * // overwriting the allocator's per-core slot for `core_id` -- which is
 * // how `old_mm` implicitly stops being "the resident context on this
 * // core" without any explicit call back into `old_mm.asid_ctx`.
 * void switch_mm(allocator_type &allocator, mm_context &old_mm, mm_context &new_mm, std::size_t core_id) {
 *   (void)old_mm; // nothing to do here -- see comment above
 *   activate_mm(allocator, new_mm, core_id);
 * }
 *
 * // In-place mapping change (munmap/mprotect) while `mm` stays resident
 * // on any number of cores -- no allocator lock needed at all.
 * void flush_mm_mappings_smp(const allocator_type &allocator, const mm_context &mm) {
 *   auto targets = mm.asid_ctx.cpu_targets(); // lock-free snapshot
 *   if (targets.none()) { return; } // never resident anywhere: nothing can be stale
 *
 *   // NOTE: reading the raw ASID here is intentionally lock-free, NOT
 *   // protected by the allocator's lock. It is safe because this mm is,
 *   // by construction, still resident on at least one core in `targets`
 *   // (we returned early above otherwise), and a resident context's
 *   // ASID can only be invalidated by a generation rollover -- which
 *   // this same core would only perform in its own, serialized
 *   // `activate()` call, never concurrently with this flush. A caller
 *   // that cannot make that residency guarantee (e.g. computing this on
 *   // behalf of a context that might be concurrently torn down) must
 *   // take the allocator's lock here instead.
 *   auto raw_asid = allocator.asid_of(mm.asid_ctx.asid());
 *   if constexpr (arch_has_broadcast_tlbi) {
 *     arch_flush_tlb_asid_broadcast(raw_asid); // e.g. ARM TLBI ...IS: one instruction, every core
 *   } else {
 *     if (targets.test(this_cpu())) {
 *       arch_flush_tlb_asid(raw_asid); // local, tagged
 *     }
 *     for (std::size_t cpu : targets) {
 *       if (cpu != this_cpu()) { arch_send_tlb_shootdown_ipi(cpu, raw_asid); }
 *     }
 *     arch_wait_for_shootdown_acks(targets);
 *   }
 * }
 *
 * // Destruction: the address space itself is being torn down. Caller
 * // must hold the allocator's lock for `release()`; the preceding flush
 * // does not need it (same `cpu_targets()` call as above).
 * void mm_exit(allocator_type &allocator, mm_context &mm) {
 *   flush_mm_mappings_smp(allocator, mm); // tagged flush while the ASID is still valid
 *   mm.asid_ctx.release(allocator);       // requires the allocator's lock
 *   mm.asid_ctx.clear_cpu_targets();      // optional: only useful if `mm` is about to be recycled
 * }
 * @endcode
 */

#include <structo/arch/asid_allocator.hpp>
#include <structo/arch/cpu_mask.hpp>

#include <atomic>
#include <cstddef>

namespace structo::arch {

using namespace reloco;

/**
 * @brief Per-address-space ASID tracking fields: a cached
 * `tagged_asid<AsidTag>` plus a lock-free `cpu_mask<CpuTag, MaxCpus>` of
 * every core that may still hold a stale, tagged TLB entry for it.
 * @tparam AsidTag Phantom tag of the `asid_allocator<AsidTag, MaxActive>`
 * this context is driven through (e.g. `process_asid_tag`, `vmid_tag`).
 * @tparam MaxCpus Number of logical CPU/vCPU indices `cpu_targets()` can
 * represent; matches the `cpu_mask<CpuTag, MaxCpus>` it wraps.
 * @tparam CpuTag Phantom tag for the embedded `cpu_mask` (defaults to
 * `physical_cpu_tag`; use `vcpu_tag` for a `vmid_tag`-driven context
 * tracking vCPU residency instead of physical-core residency).
 */
template <typename AsidTag, std::size_t MaxCpus, typename CpuTag = physical_cpu_tag> class mm_asid_context {
public:
  using asid_tag_type = AsidTag;
  using cpu_tag_type = CpuTag;
  using context_id = tagged_asid<AsidTag>;
  using mask_type = cpu_mask<CpuTag, MaxCpus>;

  /** @brief Constructs a never-activated context: invalid ASID, empty cpu mask. */
  constexpr mm_asid_context() noexcept = default;

  /** @brief `true` if this context currently holds a live ASID (has been activated at least once). */
  [[nodiscard]] constexpr bool is_valid() const noexcept { return asid_.is_valid(); }

  /**
   * @brief The cached, opaque context ID (invalid, default-constructed
   * until the first `activate()`).
   *
   * Like `asid_allocator::active()`, reading this concurrently with an
   * `activate()`/`release()` driven through the same (unsynchronized)
   * allocator on another core is a data race unless the caller holds
   * that allocator's external lock -- see "Synchronization contract"
   * above. Use `cpu_targets()` for the one piece of state that IS safe
   * to read lock-free.
   */
  [[nodiscard]] constexpr context_id asid() const noexcept { return asid_; }

  /**
   * @brief Activates this address space on logical core/slot `cpu`:
   * allocates (or revalidates) its ASID through `allocator`, then
   * records `cpu` into the lock-free cpu mask.
   *
   * Must be called under whatever external lock serializes `allocator`
   * (see "Synchronization contract" above) -- this is exactly
   * `asid_allocator::allocate(cpu, asid())` plus bookkeeping, nothing
   * more.
   *
   * @tparam MaxActive The allocator's own `MaxActive` tracking-slot count.
   * @param allocator The shared allocator driving this address space's ASID.
   * @param cpu Logical core (or vCPU, for a `vmid_tag`/`vcpu_tag` context)
   * index becoming resident; also the `asid_allocator` tracking slot.
   * @param mask_order Memory order for the cpu-mask bit set (default
   * `memory_order_release`, pairing with `cpu_targets()`'s default
   * `memory_order_acquire`).
   * @return `true` if a generation rollover occurred and a global,
   * non-tagged TLB invalidation is now mandatory before loading the
   * returned ASID into hardware (see `asid_allocator::allocation::
   * flush_required`); `error::invalid_argument` if `cpu >= MaxActive`.
   */
  template <std::size_t MaxActive>
  [[nodiscard]] result<bool> activate(asid_allocator<AsidTag, MaxActive> &allocator, std::size_t cpu,
                                       std::memory_order mask_order = std::memory_order_release) noexcept {
    auto alloc_res = allocator.allocate(cpu, asid_);
    if (!alloc_res) {
      return unexpected(alloc_res.error());
    }
    asid_ = alloc_res.value().id;
    active_cpus_.atomic_set(cpu, mask_order);
    return alloc_res.value().flush_required;
  }

  /**
   * @brief Releases this address space's ASID back to `allocator` (the
   * address space is being destroyed).
   *
   * Must be called under whatever external lock serializes `allocator`
   * (see "Synchronization contract" above). The caller is responsible
   * for issuing a tagged TLB flush over `cpu_targets()` *before* calling
   * this (while the ASID is still known) -- `release()` itself performs
   * no TLB action, matching `asid_allocator::release()`.
   *
   * Deliberately does NOT clear `cpu_targets()` -- a torn-down address
   * space is normally about to be freed outright, in which case its
   * stale cpu mask is irrelevant; call `clear_cpu_targets()` explicitly
   * first if this instance is instead about to be recycled for a brand
   * new address space.
   *
   * @tparam MaxActive The allocator's own `MaxActive` tracking-slot count.
   */
  template <std::size_t MaxActive> void release(asid_allocator<AsidTag, MaxActive> &allocator) noexcept {
    allocator.release(asid_);
    asid_ = context_id{};
  }

  /**
   * @brief Lock-free snapshot of every core that may still hold a stale,
   * tagged TLB entry for this address space -- empty if it has never
   * been activated, or was fully flushed since its last residency.
   *
   * Safe to call concurrently with an in-flight `activate()` on another
   * core without the allocator's lock -- see "Synchronization contract"
   * above for exactly what guarantee that gives.
   */
  [[nodiscard]] mask_type cpu_targets(std::memory_order order = std::memory_order_acquire) const noexcept {
    return active_cpus_.atomic_snapshot(order);
  }

  /**
   * @brief Resets the cpu-residency mask to empty.
   *
   * Only useful when recycling this instance for a brand-new address
   * space after a full teardown (the new address space has, by
   * definition, never run anywhere yet) -- not part of the normal
   * activate/release lifecycle, which leaves this mask monotonically
   * growing by design (see `release()`).
   */
  void clear_cpu_targets() noexcept { active_cpus_ = mask_type::empty(); }

private:
  context_id asid_{};
  mask_type active_cpus_{};
};

} // namespace structo::arch
