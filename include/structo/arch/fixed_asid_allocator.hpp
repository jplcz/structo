// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file fixed_asid_allocator.hpp
 * @brief `structo::arch::fixed_asid_allocator<Tag, Capacity, SeqGroupSize>`:
 * a no-allocation, rollover-free ASID allocator for systems where the
 * number of tasks/VMs that will ever be simultaneously live is a
 * compile-time-known, power-of-two `Capacity` that never exceeds the
 * (also power-of-two) hardware ASID space -- every task is handed one
 * permanent ASID *slot* for its whole lifetime, so none of
 * `asid_allocator<Tag, MaxActive>`'s generation *rollover* machinery
 * (the TLB-exhaustion escape hatch) is needed at all. `fixed_asid<Tag>`
 * is the opaque handle it hands back, deliberately a distinct type from
 * `tagged_asid<Tag>` (see "Why not just reuse `tagged_asid`" below).
 *
 * ## Why this exists
 *
 * `asid_allocator<Tag, MaxActive>` earns its generation-counted bitmap
 * complexity by handling the hard case: a hardware ASID space (e.g. an
 * 8-bit PCID) far too small to assign one permanently to every task
 * that ever existed, so IDs must be recycled, under hardware still
 * actively using some of them, via a software generation counter and a
 * global TLB flush on rollover. Plenty of real systems never hit that
 * hard case -- a small embedded RTOS with a fixed, compile-time-bounded
 * task count that is itself smaller than the hardware ASID space (e.g.
 * <= 256 tasks against a 16-bit ASID register) never needs to reclaim an
 * ASID out from under a still-resident task, because there are always
 * enough raw ASID values to hand every concurrently-live task its own,
 * forever. For that system, `asid_allocator`'s rollover scan and
 * per-slot `active[]` residency tracking are pure overhead paid for a
 * case (hardware ASID exhaustion) that can provably never occur.
 * `fixed_asid_allocator<Tag, Capacity, SeqGroupSize>` is the allocator
 * for that system: a plain fixed-capacity free-slot pool, no rollover,
 * no `flush_required` flag -- `Capacity` is simply how many tasks/VMs
 * may hold a live ASID at once, and that many bits (not `2^asid_bits`
 * bits) is all the bitmap ever needs.
 *
 * ## Fixed, static bitmap sized by task count, not by ASID width
 *
 * `asid_allocator`'s bitmap is sized from the runtime-probed `asid_bits`
 * (e.g. `ID_AA64MMFR0_EL1.ASIDBits`) because it must be able to track
 * every raw ASID value the hardware could ever produce -- a
 * `reloco::vector<std::uint64_t>` obtained through a caller-supplied
 * allocator. `fixed_asid_allocator` does not have that problem: since
 * `Capacity` (the task count) is always `<=` the hardware ASID space,
 * it is only ever necessary to track which of the `Capacity` *tasks*
 * currently hold a live slot, not which of the (potentially much
 * larger) `2^asid_bits` raw values are in use. The free-slot bitmap is
 * therefore a fixed-size `std::uint64_t[(Capacity + 63) / 64]` array
 * embedded directly in the object -- no allocator, no `reloco::vector`,
 * safe to use in interrupt/trap context or before an allocator even
 * exists, exactly like `cpu_mask<Tag, MaxCpus>`'s own fixed-size
 * storage. `asid_bits` is still a constructor argument -- `try_create()`
 * rejects it unless the runtime-probed hardware ASID space is actually
 * wide enough to hold every slot index, so an undersized hardware ASID
 * register is caught at startup, not silently truncated into a
 * colliding value later.
 *
 * ## Both `Capacity` and the hardware ASID space are powers of two
 *
 * `Capacity` must be a power of two (`static_assert`-enforced), matching
 * how a task/VM table is sized in practice (and how the hardware ASID
 * space itself is always shaped: `2^asid_bits`), so the number of bits
 * needed to address a slot, `slot_bits = log2(Capacity)`, is always
 * exact -- no rounding, no wasted slot-index codespace, computed with a
 * single `__builtin_ctzll(Capacity)` (this codebase targets GCC/Clang
 * exclusively, so the builtin is preferred over a portable fallback).
 *
 * ## Spare hardware ASID bits become a cosmetic sequence counter
 *
 * A real hardware ASID register is frequently wider than `log2(Capacity)`
 * -- e.g. a 16-bit ASID register (`asid_bits == 16`) against a `Capacity`
 * of 256 tasks (`slot_bits == 8`) leaves 8 bits of the register
 * permanently unused by the slot index alone. Rather than waste that
 * codespace, `fixed_asid_allocator` packs a small rolling sequence
 * counter into those spare top bits
 * (`seq_bits = asid_bits - slot_bits`): every `allocate()` bumps the
 * counter for the slot's group and packs `(seq << slot_bits) | slot`
 * into the returned handle, so the *same* task slot visibly produces a
 * *different* raw hardware ASID value each time it is reallocated --
 * useful when staring at a register dump, a TLB-entry trace, or a log
 * line to tell "this is a fresh occupant of slot N" apart from "this is
 * the same occupant as before" at a glance.
 *
 * This counter is **purely a diagnostic aid, not a correctness
 * mechanism**: `release()`/`allocate()` already require the caller to
 * flush that one ASID's tagged TLB entries on every reuse regardless
 * (see "Division of responsibility" below), so two allocations of the
 * same slot that happened to produce the exact same raw value (e.g.
 * because `seq_bits == 0`, the common case when `asid_bits == slot_bits`
 * and there are no spare bits at all) would be just as correct, merely
 * harder to tell apart by eye. Because of this, the sequence counter is
 * deliberately allowed to be coarser than "one independent counter per
 * slot": `SeqGroupSize` (a power-of-two template parameter, default 32)
 * lets a single counter be shared across a whole block of `SeqGroupSize`
 * neighboring slots, bumped on *any* allocation within the block --
 * trading a little extra, harmless "two different slots momentarily
 * show the same generation prefix" ambiguity for `Capacity /
 * SeqGroupSize` counters instead of `Capacity` of them. `slot_of()`
 * recovers the stable per-task slot index from a handle regardless of
 * which sequence value happens to be packed alongside it.
 *
 * ## Why not just reuse `tagged_asid<Tag>`
 *
 * `asid_allocator`'s `tagged_asid<Tag>` packs a software generation
 * counter that `asid_allocator::allocate()`'s `prev`/fast-path and
 * rollover logic read back out via `generation_of()` to decide whether a
 * cached ID is still valid -- machinery that only makes sense paired
 * with the allocator that maintains and *interprets* that counter.
 * `fixed_asid_allocator`'s sequence counter, by contrast, is written but
 * never read back by anything (see above -- it is cosmetic only), so
 * its handle, `fixed_asid<Tag>`, is deliberately a separate, unrelated
 * type: reusing `tagged_asid<Tag>` here would let a handle produced by
 * one allocator kind be silently accepted by the other's
 * `release()`/`asid_of()`/`generation_of()` (which would then
 * misinterpret this header's cosmetic sequence bits as a real,
 * rollover-tracked generation) -- exactly the kind of cross-space mixup
 * `tagged_asid<Tag>`'s own per-`Tag` phantom typing exists to rule out
 * between a process ASID and a VMID. `fixed_asid<Tag>` and
 * `tagged_asid<Tag>` are therefore unrelated types with no conversion
 * between them, even when instantiated on the exact same `Tag`
 * (`process_asid_tag`/`vmid_tag`, bundled in `asid_allocator.hpp`, are
 * reused here purely for their phantom-tagging meaning, not for any
 * shared handle representation).
 *
 * ## Division of responsibility: bookkeeping here, hardware effects at the caller
 *
 * Like `asid_allocator`, `fixed_asid_allocator` never touches a TLB,
 * system register, or any other hardware state -- it is pure
 * bookkeeping. `allocate()` never requires a TLB flush of its own (there
 * is no generation rollover to flush after); `release()` hands a slot
 * back to the free pool, after which the caller must issue a **tagged**
 * (this-ASID-only) TLB invalidation before that slot's raw ASID value is
 * handed to a different task, so the new owner never observes the old
 * owner's stale translations -- exactly `asid_allocator`'s own
 * destruction-time flush rule (see `asid_allocator.hpp`'s "Bridging to an
 * `mm_context`-like structure" table), minus the generation-rollover row,
 * which never applies here.
 *
 * ## Thread safety
 *
 * `fixed_asid_allocator` performs **no internal synchronization** --
 * `allocate()` and `release()` mutate the shared bitmap (and sequence
 * counters) with no locking or atomics whatsoever, exactly like
 * `asid_allocator` and every other bookkeeping header in this library.
 * A single instance may only be driven from one logical thread of
 * execution at a time; concurrent callers must serialize access
 * themselves (e.g.
 * `structo::sync::irq_locked<fixed_asid_allocator<Tag, Capacity>>`
 * composed with a caller-supplied cross-core spinlock when shared across
 * cores).
 *
 * @code
 * // Capacity known at compile time, a power of two: this kernel never
 * // schedules more than 64 tasks concurrently, far inside the 16-bit
 * // hardware ASID space probed below.
 * using allocator_type = structo::arch::fixed_asid_allocator<structo::arch::process_asid_tag, 64>;
 *
 * std::size_t hw_asid_bits = probe_asid_bits(); // e.g. 16
 * auto maker = allocator_type::try_create(hw_asid_bits);
 * if (!maker) { panic("hardware ASID space too narrow for Capacity tasks"); }
 * auto allocator = std::move(maker.value());
 *
 * // Task creation: hand it a permanent ASID for its whole lifetime.
 * auto alloc_res = allocator.allocate();
 * if (!alloc_res) { panic("fixed_asid_allocator exhausted: Capacity too small"); }
 * task.asid = alloc_res.value();
 * arch_write_ttbr0_asid(task.page_table_base, allocator.asid_of(task.asid));
 *
 * // Task exit: return the slot to the pool and flush just that one tag.
 * auto raw_asid = allocator.asid_of(task.asid);
 * allocator.release(task.asid);
 * task.asid = {};
 * arch_flush_tlb_asid(raw_asid); // tagged: only this now-reusable ASID's entries
 * @endcode
 *
 * See also: [`asid_allocator.md`](asid_allocator.md) (the
 * generation-rollover allocator this header's "never needs to reclaim"
 * case trades away), [`cpu_mask.md`](cpu_mask.md) (the fixed-size,
 * allocation-free bitmask storage convention this header follows).
 */

#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/optional.hpp>

#include <structo/arch/asid_allocator.hpp> // process_asid_tag / vmid_tag reused for phantom tagging

#include <cstddef>
#include <cstdint>

namespace structo::arch {

using namespace reloco;

// -------------------------------------------------------------------------
// Opaque Fixed ASID Handle
// -------------------------------------------------------------------------

/**
 * @brief Opaque, tagged 32-bit hardware address-space-context handle
 * produced by `fixed_asid_allocator<Tag, Capacity, SeqGroupSize>`.
 * @tparam Tag Phantom type distinguishing what kind of context this ID
 * belongs to (e.g. `process_asid_tag` vs `vmid_tag`); never implicitly or
 * explicitly convertible to a `fixed_asid<OtherTag>`, nor to a
 * `tagged_asid<Tag>` even for the same `Tag` (see
 * `fixed_asid_allocator.hpp`'s "Why not just reuse `tagged_asid`").
 */
template <typename Tag> class fixed_asid {
public:
  using tag_type = Tag;
  using raw_type = std::uint32_t;

  /** @brief Constructs an invalid (unassigned) handle. */
  constexpr fixed_asid() noexcept = default;

  /** @brief `true` if this handle was produced by a successful `fixed_asid_allocator::allocate()`. */
  [[nodiscard]] constexpr bool is_valid() const noexcept { return value_ != invalid_value; }
  constexpr explicit operator bool() const noexcept { return is_valid(); }

  /**
   * @brief The raw, hardware-loadable ASID value, including whichever
   * cosmetic sequence bits `fixed_asid_allocator` packed into its spare
   * top bits -- its private bit layout (slot index in the low bits,
   * sequence counter in the high bits) is otherwise an implementation
   * detail of whichever `fixed_asid_allocator` produced it; use
   * `fixed_asid_allocator::slot_of()` to recover the stable per-task
   * slot index instead of decoding this value by hand.
   */
  [[nodiscard]] constexpr raw_type raw() const noexcept { return value_; }

  [[nodiscard]] friend constexpr bool operator==(fixed_asid lhs, fixed_asid rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }
  [[nodiscard]] friend constexpr bool operator!=(fixed_asid lhs, fixed_asid rhs) noexcept {
    return lhs.value_ != rhs.value_;
  }

private:
  template <typename, std::size_t, std::size_t> friend class fixed_asid_allocator;

  static inline constexpr raw_type invalid_value = ~raw_type{0};

  explicit constexpr fixed_asid(raw_type v) noexcept : value_(v) {}

  raw_type value_{invalid_value};
};

// -------------------------------------------------------------------------
// Fixed-Capacity ASID Allocator
// -------------------------------------------------------------------------

/**
 * @brief No-allocation, rollover-free ASID allocator for a
 * compile-time-bounded, power-of-two task/VM count that never exceeds
 * the (also power-of-two) hardware ASID space.
 * @tparam Tag Phantom tag for the `fixed_asid<Tag>` handles this
 * allocator produces (e.g. `process_asid_tag`, `vmid_tag`).
 * @tparam Capacity Maximum number of simultaneously-live tasks/VMs this
 * allocator will ever hand an ASID to; a power of two, bounding the
 * fixed-size bitmap and the number of slot-index bits packed into each
 * handle.
 * @tparam SeqGroupSize How many neighboring slots share one cosmetic
 * sequence counter (see the file-level "Spare hardware ASID bits become
 * a cosmetic sequence counter" section); a power of two, default 32.
 */
template <typename Tag, std::size_t Capacity, std::size_t SeqGroupSize = 32> class fixed_asid_allocator {
  static_assert(Capacity >= 1 && Capacity <= 65536 && (Capacity & (Capacity - 1)) == 0,
                "Capacity must be a power of two in [1, 65536]");
  static_assert(SeqGroupSize >= 1 && (SeqGroupSize & (SeqGroupSize - 1)) == 0, "SeqGroupSize must be a power of two");

public:
  using context_id = fixed_asid<Tag>;
  using raw_type = typename context_id::raw_type;

  /** @brief Maximum number of simultaneously-live ASIDs this allocator can hand out. */
  static inline constexpr std::size_t capacity = Capacity;
  /** @brief Number of low bits of a raw ASID value dedicated to the stable per-task slot index. */
  static inline constexpr std::size_t slot_bits = static_cast<std::size_t>(__builtin_ctzll(Capacity));
  /** @brief Smallest hardware ASID width this allocator accepts (must at least address every slot). */
  static inline constexpr std::size_t min_asid_bits = slot_bits == 0 ? 1 : slot_bits;
  /**
   * @brief Largest hardware ASID width this allocator accepts, capped
   * well below `raw_type`'s 32 bits to keep `raw_type{1} << asid_bits`
   * free of overflow/shift-UB for every accepted value.
   */
  static inline constexpr std::size_t max_asid_bits = 31;

  fixed_asid_allocator(fixed_asid_allocator &&) noexcept = default;
  fixed_asid_allocator &operator=(fixed_asid_allocator &&) noexcept = default;
  fixed_asid_allocator(const fixed_asid_allocator &) = delete;
  fixed_asid_allocator &operator=(const fixed_asid_allocator &) = delete;

  /**
   * @brief Fallible construction and validation factory. No allocator is
   * involved -- the bitmap is a fixed-size in-object array -- `asid_bits`
   * only needs validating, never sizing anything.
   * @param asid_bits Hardware ASID width (e.g. probed from
   * `ID_AA64MMFR0_EL1.ASIDBits`), in `[min_asid_bits, max_asid_bits]`.
   * @return The allocator, or `error::invalid_argument` if `asid_bits` is
   * out of range, or narrower than `slot_bits` (the hardware ASID space
   * is too narrow to give every one of `Capacity` tasks a distinct
   * permanent slot).
   */
  [[nodiscard]] static result<fixed_asid_allocator> try_create(std::size_t asid_bits) noexcept {
    if (asid_bits < min_asid_bits || asid_bits > max_asid_bits || asid_bits < slot_bits) {
      return unexpected(error::invalid_argument);
    }
    return fixed_asid_allocator(asid_bits);
  }

  /**
   * @brief Permanently removes slot `slot` from circulation (e.g. an
   * architecturally reserved identity-mapping ASID). Must be called, if
   * at all, before any `allocate()`.
   */
  [[nodiscard]] result<void> reserve(raw_type slot) noexcept {
    if (slot >= Capacity) {
      return unexpected(error::invalid_argument);
    }
    mark_used(slot);
    return {};
  }

  /**
   * @brief Hands out a fresh, permanent ASID for a task/VM that is about
   * to start existing. Unlike `asid_allocator::allocate()`, there is no
   * `prev`/fast-path-reuse parameter and no `flush_required` flag -- the
   * returned ASID was never in use before (or was fully flushed by the
   * caller after a prior `release()`), so nothing hardware-side needs
   * invalidating before it is loaded. The handle's raw value packs a
   * cosmetic sequence counter into any spare hardware ASID bits above
   * `slot_bits` (see the file-level docs); the stable slot identity is
   * always recoverable via `slot_of()`.
   * @return The freshly allocated `context_id`, or
   * `error::allocation_failed` if every one of `Capacity` slots is
   * already in use.
   */
  [[nodiscard]] result<context_id> allocate() noexcept {
    auto found = try_alloc_bit();
    if (!found) {
      return unexpected(error::allocation_failed);
    }
    const raw_type slot = *found;
    const raw_type seq = bump_seq(slot);
    return context_id(static_cast<raw_type>((seq << slot_bits) | slot));
  }

  /**
   * @brief Returns `id`'s slot to the free pool (a no-op for an invalid
   * `id`). The caller must issue a tagged (this-ASID-only) TLB
   * invalidation before this slot's raw ASID value is handed to a
   * different task.
   */
  void release(context_id id) noexcept {
    if (!id.is_valid()) {
      return;
    }
    clear_bit(slot_of(id));
  }

  /** @brief The ASID width this allocator was constructed with. */
  [[nodiscard]] constexpr std::size_t asid_bits() const noexcept { return asid_bits_; }

  /** @brief Number of ASIDs currently in use. */
  [[nodiscard]] std::size_t used_count() const noexcept {
    std::size_t count = 0;
    for (std::uint64_t word : bitmap_) {
      count += static_cast<std::size_t>(__builtin_popcountll(word));
    }
    return count;
  }

  /**
   * @brief The one sanctioned way to extract the raw, hardware-loadable
   * ASID bits out of an opaque `context_id` -- includes the cosmetic
   * sequence bits, exactly the value to load into a hardware ASID
   * register.
   */
  [[nodiscard]] constexpr raw_type asid_of(context_id id) const noexcept { return id.raw(); }

  /**
   * @brief The stable per-task slot index packed into `id`'s low
   * `slot_bits` bits -- unlike `asid_of()`, this strips the cosmetic
   * sequence prefix, so it is the same value across every reallocation
   * of the same task's slot.
   */
  [[nodiscard]] constexpr raw_type slot_of(context_id id) const noexcept {
    return id.raw() & slot_mask_;
  }

private:
  explicit fixed_asid_allocator(std::size_t asid_bits) noexcept : asid_bits_(asid_bits) {}

  static inline constexpr std::size_t word_count = (Capacity + 63) / 64;
  static inline constexpr std::size_t tail_bits = Capacity % 64;
  static inline constexpr std::size_t group_count = (Capacity + SeqGroupSize - 1) / SeqGroupSize;
  static inline constexpr std::size_t group_shift = static_cast<std::size_t>(__builtin_ctzll(SeqGroupSize));
  static inline constexpr raw_type slot_mask_ = (slot_bits == 0) ? raw_type{0} : ((raw_type{1} << slot_bits) - 1);

  void mark_used(raw_type slot) noexcept { bitmap_[slot / 64] |= (std::uint64_t{1} << (slot % 64)); }
  void clear_bit(raw_type slot) noexcept { bitmap_[slot / 64] &= ~(std::uint64_t{1} << (slot % 64)); }

  [[nodiscard]] optional<raw_type> try_alloc_bit() noexcept {
    for (std::size_t w = 0; w < word_count; ++w) {
      std::uint64_t avail = ~bitmap_[w];
      if (w == word_count - 1 && tail_bits != 0) {
        avail &= (std::uint64_t{1} << tail_bits) - 1; // mask off unreachable tail padding bits
      }
      if (avail != 0) {
        const auto bit = static_cast<std::size_t>(__builtin_ctzll(avail));
        const auto slot = static_cast<raw_type>((w * 64) + bit);
        mark_used(slot);
        return slot;
      }
    }
    return nullopt;
  }

  /** @brief Bumps and returns the cosmetic sequence counter shared by `slot`'s group, masked to the spare ASID bits. */
  [[nodiscard]] raw_type bump_seq(raw_type slot) noexcept {
    const std::size_t seq_bits = asid_bits_ - slot_bits;
    if (seq_bits == 0) {
      return 0; // no spare hardware bits: nothing to pack
    }
    const raw_type seq_mask = (raw_type{1} << seq_bits) - 1;
    raw_type &counter = seq_[slot >> group_shift];
    counter = (counter + 1) & seq_mask;
    return counter;
  }

  std::size_t asid_bits_{0};
  std::uint64_t bitmap_[word_count]{};
  raw_type seq_[group_count]{};
};

} // namespace structo::arch
