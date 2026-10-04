// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file asid_allocator.hpp
 * @brief `structo::arch::asid_allocator<Tag, MaxActive>`: a software ASID
 * (Address Space IDentifier) allocator -- the bitmap-plus-generation
 * scheme real kernels (Linux's arm64 `mm/context.c`, FreeBSD's
 * `pmap_pinit`/TLB-shootdown ASID path) use to hand out a small, hardware
 * ASID/VMID/PCID register value to tasks/VMs and safely reuse it once the
 * hardware ID space is exhausted -- plus `structo::arch::tagged_asid<Tag>`,
 * an opaque 32-bit handle distinguishing what *kind* of address-space
 * context an ID belongs to (a process/task vs a virtual machine) purely at
 * the type level.
 *
 * ## Why this exists
 *
 * Hardware ASID-style fields are small (ARM's `TTBR0_EL1.ASID` is 8 or 16
 * bits depending on `ID_AA64MMFR0_EL1.ASIDBits`; x86-64 PCID is 12 bits;
 * ARM VMID and Intel VPID are narrower still) -- far too small to assign
 * one permanently to every task/VM that ever existed. The standard
 * solution, used by every production kernel that has hardware ASIDs, is a
 * *generation-counted* bitmap: hand out raw ASID values from a bitmap
 * while they last, and when the bitmap is exhausted, bump a software
 * "generation" counter, flush the TLB (invalidating every ASID in the
 * outgoing generation at once), and start reusing bits from zero -- except
 * for any ASID that is still the *active*, currently-resident ID on some
 * core/VM, which survives the sweep into the new generation unchanged.
 * `asid_allocator` implements exactly this scheme as architecture-agnostic
 * bookkeeping; it performs no TLB invalidation and touches no system
 * register itself (see "Division of responsibility" below).
 *
 * ## Runtime, not compile-time, ASID width
 *
 * Unlike most of `structo`'s other strongly-typed values (`phys_addr`,
 * `io_address`, `target_ptr`), the hardware ASID width is **not** a
 * compile-time constant here: it depends on the actual CPU model/mode a
 * kernel ends up booted on (e.g. `ID_AA64MMFR0_EL1.ASIDBits` is a runtime
 * MSR/system-register read, not something the compiler can know), so
 * `asid_bits` is a constructor argument supplied once the caller has
 * probed it, not a template parameter. `MaxActive` (how many simultaneous
 * "this ASID is the live one right now" slots to track -- one per core for
 * a process ASID allocator, or one per currently-scheduled VM for a VMID
 * allocator) remains a compile-time template parameter, matching
 * `hw_id_lut<HwId, MaxCpus, ...>`'s `MaxCpus`: it bounds a small, fixed-size
 * array, not the (potentially large, runtime-sized) ASID bitmap itself.
 *
 * ## Allocator-backed bitmap storage
 *
 * The ASID bitmap is sized from the runtime `asid_bits` (anywhere from a
 * handful of bits to `max_asid_bits`), so it is **not** a fixed-size
 * in-class array the way `hw_id_lut`'s L1/L2 tables are -- it is a
 * `reloco::vector<std::uint64_t>` obtained through a caller-supplied
 * `reloco::allocator_ref` at construction time (`try_allocate`/
 * `try_create`, following `reloco`'s fallible-construction convention;
 * see `docs/fallible-construction.md`), exactly as `reloco::vector<T>`
 * itself obtains its own backing storage. Once constructed, no further
 * allocation ever occurs -- `allocate()`/`release()` only flip bits in the
 * already-sized bitmap.
 *
 * ## `tagged_asid<Tag>`: distinguishing ASID from VMID at the type level
 *
 * The same bitmap-plus-generation scheme applies equally to a per-task
 * ASID allocator and a per-VM VMID allocator -- they are the same
 * algorithm over a differently-sized ID space. To stop a VMID from ever
 * being accidentally passed where a process ASID was expected (or vice
 * versa), the ID type returned by `allocate()`/accepted by `release()` is
 * `tagged_asid<Tag>`, parameterized on a phantom `Tag` (`process_asid_tag`/
 * `vmid_tag` are bundled; a caller with further subdivisions -- e.g. a
 * secure-world ASID space distinct from a normal-world one -- can define
 * its own tag struct). `tagged_asid<TagA>` and `tagged_asid<TagB>` are
 * unrelated types with no implicit or explicit conversion between them:
 * unlike `phys_addr`/`io_address`'s `cast_space()`, there is deliberately
 * **no** cross-tag cast here, because (unlike a physical address, which
 * genuinely may need reinterpreting across an address-space boundary) an
 * ASID and a VMID are never legitimately the same identifier wearing a
 * different hat -- if code needs to go from one to the other, that is a
 * bug, not a cast. `tagged_asid<Tag>` is otherwise a fully opaque 32-bit
 * handle: its packed generation/ASID bit layout is a private implementation
 * detail of whichever `asid_allocator<Tag, MaxActive>` produced it, not
 * something caller code should ever manually decode. The single
 * exception is `asid_allocator::asid_of()`, the one sanctioned way to
 * extract the raw hardware-loadable ASID value a caller needs to actually
 * program a system register with.
 *
 * ## Division of responsibility: bookkeeping here, hardware effects at the caller
 *
 * `asid_allocator` never touches a TLB, system register, or any other
 * hardware state -- it is pure bookkeeping, like every other header in
 * this library. `allocate()` returns an `allocation` with a
 * `flush_required` flag: `true` exactly when a generation rollover just
 * happened, meaning the caller **must** perform at least a local TLB
 * invalidation (a global/all-core shootdown, in a multi-core system, since
 * other cores' still-resident ASIDs from the outgoing generation are
 * exactly what a rollover is reclaiming bits out from under) before the
 * newly allocated ASID can be safely loaded into hardware. Exactly which
 * invalidation instruction/hypercall to issue is architecture-specific and
 * deliberately out of scope here, matching `io_space_ref.hpp`'s divide
 * between address-tagging/bookkeeping and the backend-specific access
 * itself.
 *
 * ## Thread safety
 *
 * `asid_allocator` has NO internal synchronization: `allocate()`,
 * `release()`, and the generation rollover they may trigger all mutate
 * shared state (the bitmap, the generation counter, and the `active[]`
 * tracking array) without any locking or atomics, exactly like every
 * other bookkeeping header in this library. A single instance is only
 * safe to use from one logical thread of execution at a time; concurrent
 * callers (e.g. multiple cores context-switching concurrently, or an
 * interrupt handler reentering a context switch in progress) MUST
 * serialize their own access externally -- for example by guarding the
 * instance with `structo::sync::irq_locked<asid_allocator<Tag, MaxActive>>`
 * (interrupt exclusion on a single core) composed with a caller-supplied
 * cross-core spinlock where the allocator is shared across cores, which
 * is exactly how real kernels guard their own ASID allocator state (e.g.
 * Linux arm64's `cpu_asid_lock`). This is a deliberate design choice, not
 * an oversight: baking a specific locking policy into this header would
 * force every caller -- including single-core, uniprocessor-only
 * embedded targets -- to pay for synchronization they may not need.
 *
 * ## Bridging to an `mm_context`-like structure
 *
 * @note The pattern below (one cached `context_id` plus a per-mm
 * cpu-residency mask) is implemented as a ready-made, tested type in
 * `structo/arch/mm_asid_context.hpp` (`mm_asid_context<AsidTag, MaxCpus>`)
 * -- prefer it over hand-rolling the fields described here, unless its
 * design (see its own Doxygen block for the reasoning) doesn't fit.
 *
 * A real kernel's per-address-space structure (Linux's `mm_context_t`,
 * a hypervisor's per-VM `vmid` field, etc.) should cache exactly one
 * `context_id` across its whole lifetime and drive it through three
 * distinct events -- *activation* (context switch in), *deactivation*
 * (context switch out), and *destruction* (the address space itself goes
 * away) -- which are NOT symmetric with `allocate()`/`release()`:
 *
 *   - **Activation** (`switch_mm`/`vcpu_load`-equivalent) is the ONLY time
 *     `allocate()` is called, passing the context's own cached ID back in
 *     as `prev` so an ID still valid in the current generation is reused
 *     for free. `flush_required` tells you whether a rollover just
 *     invalidated every outstanding ASID hardware-side; if so, a
 *     **global, non-tagged** TLB invalidation is mandatory before loading
 *     the (possibly numerically-recycled) ASID -- a rollover means some
 *     *other* address space may already have been handed the very same
 *     raw ASID bits in the new generation, so any stale entries still
 *     tagged with that number from the old generation would otherwise be
 *     wrongly treated as belonging to the new owner.
 *   - **Deactivation** (switching away to run something else) requires NO
 *     allocator call and NO TLB action at all -- this is the entire point
 *     of a tagged TLB: entries stay cached, tagged with this context's
 *     ASID, ready for an instant, flush-free reactivation later. Simply
 *     leave the cached `context_id` as-is.
 *   - **Destruction** (process/VM exit) is the ONLY time `release()` is
 *     called, returning the bit to circulation; do not call it on a plain
 *     deactivation; and after a `release()`, a **tagged** (this-ASID-only)
 *     TLB invalidation should be issued so the now-reusable hardware ASID
 *     doesn't serve stale translations to whichever context is handed
 *     that number next.
 *
 * A context that merely *changes its own mappings* in place (e.g.
 * `munmap`/`mprotect`) without being deactivated or destroyed needs
 * neither `allocate()` nor `release()` -- only a **tagged** flush of its
 * own ASID, which (unlike the rollover's global flush) leaves every other
 * address space's cached TLB entries untouched.
 *
 * @code
 * // Probed once at boot, e.g. from ID_AA64MMFR0_EL1.ASIDBits.
 * std::size_t hw_asid_bits = probe_asid_bits();
 *
 * using allocator_type = structo::arch::asid_allocator<structo::arch::process_asid_tag, 8>;
 * auto maker = allocator_type::try_create(hw_asid_bits);
 * if (!maker) { panic("out of memory sizing the ASID bitmap"); }
 * auto allocator = std::move(maker.value());
 *
 * struct mm_context {
 *   allocator_type::context_id asid{}; // default-constructed: invalid, never activated
 *   // ... page tables, VMAs, etc.
 * };
 *
 * // Activation: context switch INTO `mm` on logical core `core_id`.
 * void activate_mm(mm_context &mm, std::size_t core_id) {
 *   auto alloc_res = allocator.allocate(core_id, mm.asid);
 *   if (!alloc_res) { panic("ASID space exhausted"); } // unreachable in practice
 *   mm.asid = alloc_res.value().id;
 *   if (alloc_res.value().flush_required) {
 *     arch_flush_tlb_all(); // global, non-tagged: a rollover just happened
 *   }
 *   arch_write_ttbr0_asid(mm.page_table_base, allocator.asid_of(mm.asid));
 * }
 *
 * // Deactivation: switching away to run something else. No allocator
 * // call, no TLB flush -- `mm.asid` simply stays cached as-is.
 * void deactivate_mm(mm_context &) {}
 *
 * // Destruction: the address space itself is being torn down.
 * void mm_exit(mm_context &mm) {
 *   auto asid = allocator.asid_of(mm.asid); // decode before releasing
 *   allocator.release(mm.asid);
 *   mm.asid = {};
 *   arch_flush_tlb_asid(asid); // tagged: only this now-reusable ASID's entries
 * }
 *
 * // In-place mapping change (e.g. munmap) while `mm` stays resident.
 * void flush_mm_mappings(mm_context &mm) {
 *   arch_flush_tlb_asid(allocator.asid_of(mm.asid)); // tagged, this ASID only
 * }
 * @endcode
 *
 * ## Finding which CPUs to flush: `mm_context` may be active on none, one, or many cores
 *
 * `flush_mm_mappings()` above is only correct on a strictly single-core
 * target. On SMP, a page-table mutation must reach the TLB of **every**
 * core that could be holding a stale, tagged translation for `mm` -- which
 * is not just "the core currently running it": `active(slot)` reports the
 * context most recently loaded into tracking slot `slot` and is left
 * untouched by deactivation (deactivation is a pure no-op, by design --
 * see "Bridging to an `mm_context`-like structure" above), so a slot whose
 * owner was merely switched away from (not released) still correctly
 * reports `mm`'s ID here, flagging that core's TLB as still potentially
 * carrying `mm`'s tagged entries from its last residency. Iterating every
 * slot and comparing against `mm.asid` (via `tagged_asid`'s `operator==`)
 * is therefore the right -- and only -- way to compute the shootdown
 * target set; there is no separate "is this mm active anywhere" query
 * because this iteration already answers it (an empty target set means
 * no core's TLB can contain `mm`'s entries, so nothing need be flushed at
 * all, now or later, until `mm` is activated again, which revalidates
 * through `allocate()` that happens to re-share the same unflushed ASID
 * only when the mapping did not change since -- callers that mutate
 * mappings must always flush below, since `allocate()` performs no
 * flush of its own on a fast-path reuse).
 *
 * Once the target set is known, delivering the flush itself is
 * architecture-specific: some ISAs provide a broadcast, tagged
 * invalidation instruction that every core in a shareability domain
 * observes without software help (e.g. ARM's inner-shareable `TLBI
 * ...IS` forms) -- there, a single instruction on any one core suffices
 * and the loop below only needs to decide *whether* to flush at all, not
 * which cores to IPI. Architectures without a broadcast form (plain
 * `INVLPG`/`INVPCID` on x86-64, or non-`IS` ARM forms) require the
 * classic TLB-shootdown pattern: flush locally in-line if the local core
 * is in the target set, and send an inter-processor interrupt to every
 * *other* target core asking it to run the same local, tagged flush on
 * itself, waiting for all of them to acknowledge before returning (so the
 * caller can safely assume the stale mapping is gone everywhere once the
 * function returns). This is exactly the same shootdown machinery a
 * generation rollover's `arch_flush_tlb_all()` (see "Division of
 * responsibility" above) needs too -- a rollover's stale entries can be
 * resident on *any* core, not just the one that happened to observe
 * `flush_required`, so that flush must also reach every core, typically
 * by unconditionally targeting the whole cpu mask rather than computing
 * one from `active()`.
 *
 * @code
 * // Returns a bitmask of logical core indices that may be holding a
 * // stale, tagged TLB entry for `mm` -- empty if `mm` has never been
 * // resident on any core, or was fully flushed since its last residency.
 * std::uint64_t cpu_mask_for_mm(const mm_context &mm) {
 *   std::uint64_t mask = 0;
 *   if (!mm.asid.is_valid()) { return mask; } // never activated: nothing to flush
 *   for (std::size_t slot = 0; slot < allocator_type::max_active; ++slot) {
 *     if (allocator.active(slot) == mm.asid) {
 *       mask |= (std::uint64_t{1} << slot); // assumes slot == logical core index
 *     }
 *   }
 *   return mask;
 * }
 *
 * // Call after mutating `mm`'s page tables (munmap/mprotect/etc.) while
 * // it may be resident -- current or past -- on any number of cores.
 * void flush_mm_mappings_smp(mm_context &mm) {
 *   std::uint64_t targets = cpu_mask_for_mm(mm);
 *   if (targets == 0) { return; } // not resident anywhere: nothing can be stale
 *
 *   auto raw_asid = allocator.asid_of(mm.asid);
 *   if constexpr (arch_has_broadcast_tlbi) {
 *     arch_flush_tlb_asid_broadcast(raw_asid); // e.g. ARM TLBI ...IS: one instruction, every core
 *   } else {
 *     if (targets & (std::uint64_t{1} << this_cpu())) {
 *       arch_flush_tlb_asid(raw_asid); // local, tagged
 *     }
 *     arch_send_tlb_shootdown_ipi(targets & ~(std::uint64_t{1} << this_cpu()), raw_asid);
 *     arch_wait_for_shootdown_acks(targets); // block until every remote core has flushed
 *   }
 * }
 * @endcode
 *
 * ## Fine-grained flushes after unmapping a single page
 *
 * `flush_mm_mappings_smp()` above invalidates **every** TLB entry tagged
 * with `mm`'s ASID -- correct, but wasteful after e.g. a single `munmap()`
 * of one page: every other still-mapped page's cached translation is
 * thrown away too, only to be refetched by a page-table walk on its next
 * access. Most ISAs provide a by-address (optionally still ASID-tagged)
 * invalidation form precisely for this case (ARM's `TLBI VAE1IS`, x86's
 * single-address `INVLPG`/`INVPCID` type 0) -- use it instead of the
 * whole-ASID form whenever the set of unmapped pages is small, following
 * exactly the same target-cpu-mask computation as above (the *scope* of
 * what gets invalidated changes; *where* it needs to be invalidated does
 * not).
 *
 * A single-page (or short run of pages) unmap should therefore issue one
 * by-address flush per page, to the same `cpu_mask_for_mm()` target set,
 * instead of a full-ASID flush:
 *
 * @code
 * // Call after unmapping exactly one page at `vaddr` from `mm`.
 * void flush_mm_page(mm_context &mm, std::uintptr_t vaddr) {
 *   std::uint64_t targets = cpu_mask_for_mm(mm);
 *   if (targets == 0) { return; }
 *
 *   auto raw_asid = allocator.asid_of(mm.asid);
 *   if constexpr (arch_has_broadcast_tlbi) {
 *     arch_flush_tlb_page_asid_broadcast(raw_asid, vaddr); // by-address, tagged, one instruction
 *   } else {
 *     if (targets & (std::uint64_t{1} << this_cpu())) {
 *       arch_flush_tlb_page_asid(raw_asid, vaddr); // local, by-address, tagged
 *     }
 *     arch_send_tlb_shootdown_ipi_page(targets & ~(std::uint64_t{1} << this_cpu()), raw_asid, vaddr);
 *     arch_wait_for_shootdown_acks(targets);
 *   }
 * }
 * @endcode
 *
 * Unmapping a short *run* of pages (e.g. a small `munmap()` range) extends
 * naturally: loop `flush_mm_page()`-style over each page in the range,
 * reusing one `cpu_mask_for_mm()` computation and one shootdown IPI/ack
 * round-trip for the whole range rather than per page, issuing a
 * by-address invalidation (or, on ISAs that provide one, a single
 * hardware range-invalidation instruction, e.g. ARM's `TLBI RVAE1IS`) for
 * each page in the loop. Past some range-size threshold, though, the
 * per-page loop's cumulative cost exceeds a single full-ASID flush --
 * real kernels (e.g. Linux's `tlb_flush_mmu()`) fall back to the
 * whole-ASID form above once the unmapped range spans more than a
 * small, architecture-tuned number of pages, rather than looping
 * indefinitely:
 *
 * @code
 * // Call after unmapping [start, end) from `mm` (end exclusive, both
 * // page-aligned). Falls back to a full-ASID flush past a small
 * // range-size threshold, matching real kernels' amortization heuristic.
 * constexpr std::size_t max_pages_for_fine_grained_flush = 33; // architecture-tuned, e.g. Linux arm64's default
 *
 * void flush_mm_range(mm_context &mm, std::uintptr_t start, std::uintptr_t end) {
 *   std::size_t num_pages = (end - start) / page_size;
 *   if (num_pages > max_pages_for_fine_grained_flush) {
 *     flush_mm_mappings_smp(mm); // cheaper than num_pages individual invalidations
 *     return;
 *   }
 *
 *   std::uint64_t targets = cpu_mask_for_mm(mm);
 *   if (targets == 0) { return; }
 *   auto raw_asid = allocator.asid_of(mm.asid);
 *   for (std::uintptr_t vaddr = start; vaddr < end; vaddr += page_size) {
 *     if constexpr (arch_has_broadcast_tlbi) {
 *       arch_flush_tlb_page_asid_broadcast(raw_asid, vaddr);
 *     } else {
 *       if (targets & (std::uint64_t{1} << this_cpu())) { arch_flush_tlb_page_asid(raw_asid, vaddr); }
 *       arch_send_tlb_shootdown_ipi_page(targets & ~(std::uint64_t{1} << this_cpu()), raw_asid, vaddr);
 *     }
 *   }
 *   if constexpr (!arch_has_broadcast_tlbi) { arch_wait_for_shootdown_acks(targets); } // once, for the whole range
 * }
 * @endcode
 *
 * @note Refactor candidate: the ASID bitmap below (`bitmap_`,
 * `test_bit`/`mark_used`/`clear_bit`) is a hand-rolled
 * `vector<std::uint64_t>` bit-twiddler that predates
 * `bitmap_utils.hpp`/`bitmap_ops.hpp`/`dynamic_bitmap.hpp`. It could be
 * rebased onto `dynamic_bitmap` (same `allocator_ref`-backed, runtime-
 * sized storage) to drop the duplicated bit-scan logic, but that is left
 * for a future pass rather than bundled into this change.
 */

#include <reloco/allocator.hpp>
#include <reloco/default_allocator.hpp>
#include <reloco/detail/assert.hpp>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/optional.hpp>
#include <reloco/vector.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace structo::arch {

using namespace reloco;

// -------------------------------------------------------------------------
// Address-Space Context Tags
// -------------------------------------------------------------------------

/** @brief Tag for a per-task/process hardware ASID (ARM ASID, x86-64 PCID). */
struct process_asid_tag {};

/** @brief Tag for a per-VM, stage-2/EPT hardware ASID (ARM VMID, Intel VPID). */
struct vmid_tag {};

// -------------------------------------------------------------------------
// Opaque Tagged ASID Handle
// -------------------------------------------------------------------------

/**
 * @brief Opaque, tagged 32-bit hardware address-space-context handle.
 * @tparam Tag Phantom type distinguishing what kind of context this ID
 * belongs to (e.g. `process_asid_tag` vs `vmid_tag`); never implicitly or
 * explicitly convertible to a `tagged_asid<OtherTag>`.
 */
template <typename Tag> class tagged_asid {
public:
  using tag_type = Tag;
  using raw_type = std::uint32_t;

  /** @brief Constructs an invalid (unassigned) handle. */
  constexpr tagged_asid() noexcept = default;

  /** @brief `true` if this handle was produced by a successful `asid_allocator::allocate()`. */
  [[nodiscard]] constexpr bool is_valid() const noexcept { return value_ != invalid_value; }
  constexpr explicit operator bool() const noexcept { return is_valid(); }

  /**
   * @brief The raw packed 32-bit value, for logging/storage/equality only.
   * Its bit layout is a private implementation detail of whichever
   * `asid_allocator` produced it -- use `asid_allocator::asid_of()` to
   * extract the hardware-loadable ASID value, never this directly.
   */
  [[nodiscard]] constexpr raw_type raw() const noexcept { return value_; }

  [[nodiscard]] friend constexpr bool operator==(tagged_asid lhs, tagged_asid rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }
  [[nodiscard]] friend constexpr bool operator!=(tagged_asid lhs, tagged_asid rhs) noexcept {
    return lhs.value_ != rhs.value_;
  }

private:
  template <typename, std::size_t> friend class asid_allocator;

  static inline constexpr raw_type invalid_value = ~raw_type{0};

  explicit constexpr tagged_asid(raw_type v) noexcept : value_(v) {}

  raw_type value_{invalid_value};
};

// -------------------------------------------------------------------------
// ASID Allocator
// -------------------------------------------------------------------------

/**
 * @brief Generation-counted bitmap ASID allocator.
 * @tparam Tag Phantom tag for the `tagged_asid<Tag>` handles this allocator
 * produces (e.g. `process_asid_tag`, `vmid_tag`).
 * @tparam MaxActive Number of simultaneous "currently resident" slots to
 * track across a generation rollover (typically the core count for a
 * process-ASID allocator, or the max concurrently-scheduled VM count for a
 * VMID allocator).
 */
template <typename Tag, std::size_t MaxActive = 8> class asid_allocator {
  static_assert(MaxActive >= 1 && MaxActive <= 254, "MaxActive must be in [1, 254]");

public:
  using context_id = tagged_asid<Tag>;
  using raw_type = typename context_id::raw_type;

  static inline constexpr std::size_t max_active = MaxActive;
  /** @brief Smallest ASID width this allocator accepts. */
  static inline constexpr std::size_t min_asid_bits = 1;
  /**
   * @brief Largest ASID width this allocator accepts.
   * Caps the packed 32-bit raw value at >= 8 generation bits regardless of
   * `asid_bits`, so a generation rollover remains an astronomically rare
   * event even at the widest supported ASID space.
   */
  static inline constexpr std::size_t max_asid_bits = 24;

  /** @brief Result of a successful `allocate()`. */
  struct allocation {
    /** @brief The (possibly just-allocated, possibly reused) context ID. */
    context_id id;
    /**
     * @brief `true` if a generation rollover occurred to satisfy this
     * allocation; the caller must invalidate the TLB (architecture-specific)
     * before `id` may be loaded into hardware.
     */
    bool flush_required{false};
  };

  asid_allocator(asid_allocator &&) noexcept = default;
  asid_allocator &operator=(asid_allocator &&) noexcept = default;
  asid_allocator(const asid_allocator &) = delete;
  asid_allocator &operator=(const asid_allocator &) = delete;

  /**
   * @brief Fallible allocation and reservation factory.
   * @param alloc Allocator backing the ASID bitmap's storage.
   * @param asid_bits Hardware ASID width (e.g. probed from
   * `ID_AA64MMFR0_EL1.ASIDBits`), in `[min_asid_bits, max_asid_bits]`.
   * @return The allocator, or `error::invalid_argument` if `asid_bits` is
   * out of range (or too narrow to ever outnumber `MaxActive` live
   * contexts), or `error::allocation_failed` if `alloc` could not provide
   * the bitmap's backing storage.
   */
  [[nodiscard]] static result<asid_allocator> try_allocate(allocator_ref alloc, std::size_t asid_bits) noexcept {
    if (asid_bits < min_asid_bits || asid_bits > max_asid_bits) {
      return unexpected(error::invalid_argument);
    }

    const raw_type num_asids = raw_type{1} << asid_bits;
    if (static_cast<std::size_t>(num_asids) <= MaxActive) {
      // Rollover could never free enough bits to make progress: every ASID
      // would always be pinned by some active() slot.
      return unexpected(error::invalid_argument);
    }

    const std::size_t words = (static_cast<std::size_t>(num_asids) + 63) / 64;
    auto vec_res = vector<std::uint64_t>::try_allocate(alloc, words);
    if (!vec_res) {
      return unexpected(vec_res.error());
    }
    auto bitmap = std::move(vec_res.value());
    if (auto resize_res = bitmap.try_resize(words, std::uint64_t{0}); !resize_res) {
      return unexpected(resize_res.error());
    }

    asid_allocator result(asid_bits, num_asids, std::move(bitmap));
    result.mark_tail_padding();
    return result;
  }

  /** @brief `try_allocate()` using `reloco::default_allocator()`. */
  [[nodiscard]] static result<asid_allocator> try_create(std::size_t asid_bits) noexcept {
    return try_allocate(default_allocator(), asid_bits);
  }

  /**
   * @brief Permanently removes `asid` from circulation (e.g. an ASID a
   * particular architecture reserves for identity/global mappings).
   * Must be called, if at all, before any `allocate()`.
   */
  [[nodiscard]] result<void> reserve(raw_type asid) noexcept {
    if (asid >= num_asids_) {
      return unexpected(error::invalid_argument);
    }
    mark_used(asid);
    return {};
  }

  /**
   * @brief Allocates (or revalidates) the ASID for a task/VM about to
   * become resident in slot `slot`.
   * @param slot Which "currently resident" tracking slot this context is
   * becoming active in (e.g. the logical core index); `< MaxActive`.
   * @param prev The context's previously-held ID (a default-constructed,
   * invalid `context_id{}` if it has never held one). If `prev` is still
   * valid in the current generation, it is returned unchanged (the common,
   * fast-path case of resuming a task that already owns a live ASID).
   * @return The (possibly reused) `allocation` on success;
   * `error::invalid_argument` if `slot >= MaxActive`.
   */
  [[nodiscard]] result<allocation> allocate(std::size_t slot, context_id prev = context_id{}) noexcept {
    if (slot >= MaxActive) {
      return unexpected(error::invalid_argument);
    }

    if (prev.is_valid() && generation_of(prev) == generation_) {
      active_[slot] = prev;
      return allocation{prev, false};
    }

    bool flush_required = false;
    auto found = try_alloc_bit();
    if (!found) {
      rollover(slot);
      flush_required = true;
      found = try_alloc_bit();
      if (!found) {
        return unexpected(error::allocation_failed); // unreachable: num_asids_ > MaxActive is enforced at construction
      }
    }

    context_id id(pack(generation_, *found));
    active_[slot] = id;
    return allocation{id, flush_required};
  }

  /**
   * @brief Returns `id`'s ASID bit to the free pool, if it is still live
   * in the current generation (a no-op for an invalid or already-stale
   * `id` -- the latter's bit was already reclaimed by a prior rollover).
   */
  void release(context_id id) noexcept {
    if (!id.is_valid() || generation_of(id) != generation_) {
      return;
    }
    clear_bit(asid_of(id));
  }

  /** @brief The ASID width this allocator was constructed with. */
  [[nodiscard]] constexpr std::size_t asid_bits() const noexcept { return asid_bits_; }

  /** @brief `1 << asid_bits()`, the total size of the hardware ASID space. */
  [[nodiscard]] constexpr raw_type num_asids() const noexcept { return num_asids_; }

  /** @brief Current software generation counter (starts at 1). */
  [[nodiscard]] constexpr raw_type generation() const noexcept { return generation_; }

  /** @brief Number of ASIDs currently marked in-use (excludes unreachable tail padding bits). */
  [[nodiscard]] std::size_t used_count() const noexcept {
    std::size_t count = 0;
    for (std::uint64_t word : bitmap_) {
      std::uint64_t v = word;
      while (v != 0) {
        v &= (v - 1);
        ++count;
      }
    }
    const raw_type tail = num_asids_ % 64;
    if (tail != 0) {
      count -= (64 - tail);
    }
    return count;
  }

  /**
   * @brief The context ID currently resident in `slot`.
   * Traps (`RELOCO_ASSERT`) if `slot >= MaxActive`; use `try_active()` for
   * a fallible variant.
   */
  [[nodiscard]] context_id active(std::size_t slot) const noexcept {
    RELOCO_ASSERT(slot < MaxActive, "asid_allocator: active() slot out of range");
    return active_[slot];
  }

  /** @brief Fallible variant of `active()`. */
  [[nodiscard]] result<context_id> try_active(std::size_t slot) const noexcept {
    if (slot >= MaxActive) {
      return unexpected(error::invalid_argument);
    }
    return active_[slot];
  }

  /**
   * @brief The one sanctioned way to extract the raw, hardware-loadable
   * ASID bits out of an opaque `context_id` -- never `id.raw()` directly,
   * which includes this allocator's private generation bits too.
   */
  [[nodiscard]] constexpr raw_type asid_of(context_id id) const noexcept { return id.value_ & asid_mask_; }

  /** @brief The software generation `id` was allocated in (diagnostics only). */
  [[nodiscard]] constexpr raw_type generation_of(context_id id) const noexcept { return id.value_ >> asid_bits_; }

private:
  asid_allocator(std::size_t asid_bits, raw_type num_asids, vector<std::uint64_t> bitmap) noexcept
      : asid_bits_(asid_bits), num_asids_(num_asids), asid_mask_(num_asids - 1), bitmap_(std::move(bitmap)) {}

  [[nodiscard]] constexpr raw_type pack(raw_type gen, raw_type asid) const noexcept {
    return static_cast<raw_type>(gen << asid_bits_) | asid;
  }

  void mark_tail_padding() noexcept {
    const raw_type tail = num_asids_ % 64;
    if (tail != 0) {
      const std::size_t last = bitmap_.size() - 1;
      bitmap_[last] |= (~std::uint64_t{0} << tail);
    }
  }

  [[nodiscard]] bool test_bit(raw_type asid) const noexcept { return ((bitmap_[asid / 64] >> (asid % 64)) & 1u) != 0; }
  void mark_used(raw_type asid) noexcept { bitmap_[asid / 64] |= (std::uint64_t{1} << (asid % 64)); }
  void clear_bit(raw_type asid) noexcept { bitmap_[asid / 64] &= ~(std::uint64_t{1} << (asid % 64)); }

  [[nodiscard]] static std::size_t lowest_clear_bit(std::uint64_t word) noexcept {
    std::uint64_t inverted = ~word;
    std::size_t idx = 0;
    while ((inverted & 1u) == 0) {
      inverted >>= 1;
      ++idx;
    }
    return idx;
  }

  [[nodiscard]] optional<raw_type> try_alloc_bit() noexcept {
    const std::size_t word_count = bitmap_.size();
    const std::size_t start_word = static_cast<std::size_t>(cursor_ / 64);
    for (std::size_t offset = 0; offset < word_count; ++offset) {
      const std::size_t w = (start_word + offset) % word_count;
      const std::uint64_t word = bitmap_[w];
      if (word != ~std::uint64_t{0}) {
        const std::size_t bit = lowest_clear_bit(word);
        const raw_type asid = static_cast<raw_type>((w * 64) + bit);
        mark_used(asid);
        cursor_ = asid + 1;
        if (cursor_ >= num_asids_) {
          cursor_ = 0;
        }
        return asid;
      }
    }
    return nullopt;
  }

  void rollover(std::size_t requesting_slot) noexcept {
    ++generation_;
    for (auto &w : bitmap_) {
      w = 0;
    }
    mark_tail_padding();
    cursor_ = 0;

    for (std::size_t i = 0; i < MaxActive; ++i) {
      if (i == requesting_slot) {
        continue; // about to receive a freshly allocated ASID of its own
      }
      context_id &a = active_[i];
      if (a.is_valid()) {
        mark_used(asid_of(a));
        a = context_id(pack(generation_, asid_of(a)));
      }
    }
  }

  std::size_t asid_bits_{0};
  raw_type num_asids_{0};
  raw_type asid_mask_{0};
  raw_type generation_{1};
  raw_type cursor_{0};
  vector<std::uint64_t> bitmap_;
  context_id active_[MaxActive]{};
};

} // namespace structo::arch
