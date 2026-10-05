// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file vdso_clock_writer.hpp
 * @brief `structo::hw::vdso_clock_update_guard`/`vdso_clock_writer`: the
 * OS-side half of the VDSO clock page (`vdso_clock_page.hpp`) -- bumps
 * a @ref structo::hw::vdso_clock_slot "vdso_clock_slot"'s seqlock
 * generation counter to odd, publishes a new counter/reference-instant
 * snapshot, then bumps it back to even.
 *
 * ## Mutual exclusion is the caller's responsibility
 *
 * The seqlock protocol (`generation` even/odd) only protects *readers*
 * racing a single writer; it does nothing to stop two concurrent
 * writers (e.g. two CPUs' periodic timer interrupts both deciding to
 * republish the same `vdso_clock_id`) from interleaving their own odd/
 * even transitions and corrupting the sequence (or worse, publishing a
 * torn snapshot that still happens to read back as internally
 * consistent). Neither this header nor `vdso_clock_update_guard`
 * provides that exclusion -- the caller must already hold whatever lock
 * serializes updates to a given slot (e.g. a per-slot kernel spin lock,
 * see `structo/sync/guarded_spin_mutex.hpp`/`irq_spin_lock.hpp`) for the
 * entire `begin_update()`...guard-destruction span:
 *
 * @code
 * // caller-owned, e.g. one per vdso_clock_id:
 * structo::sync::irq_spin_lock<structo::sync::kernel_spin_lock<kernel_lock_traits>,
 *                               structo::sync::irq_guard<arm_irq_traits>>
 *     realtime_update_lock;
 *
 * void publish_realtime(structo::hw::vdso_clock_page &page, structo::hw::cycles counter,
 *                        reloco::duration wall_clock_now, std::uint64_t counter_hz) {
 *   auto lock_guard = realtime_update_lock.lock(); // caller-managed; outlives the update guard below
 *   structo::hw::vdso_clock_writer writer(page.slot(structo::hw::vdso_clock_id::realtime));
 *   writer.publish(structo::hw::vdso_clock_source::x86_tsc, counter_hz, counter, wall_clock_now);
 * } // lock_guard releases here, after the seqlock has already gone back to even
 * @endcode
 */

#include <structo/hw/clock_cycles.hpp>
#include <structo/hw/vdso_clock_page.hpp>

#include <reloco/duration.hpp>

#include <atomic>
#include <cstdint>

namespace structo {

using namespace reloco;

namespace hw {

/**
 * @brief RAII scope that bumps @p slot's seqlock generation to odd on
 * construction and back to even on destruction, so field writes made
 * through it (`set_source`/`set_counter_hz`/`set_reference`) are always
 * bracketed correctly even if a caller returns early/throws between
 * them. See the @file-level docs for the mutual-exclusion precondition
 * this guard does **not** provide.
 */
class [[nodiscard]] vdso_clock_update_guard {
public:
  /** @brief Begins an update: loads the current generation and stores `generation + 1` (odd) with
   * `memory_order_seq_cst`, so no field write below can be observed by a reader before this becomes visible.
   * @param slot The slot to update; must outlive this guard. The caller must already hold whatever lock
   * serializes concurrent writers to @p slot -- see the @file-level docs. */
  explicit vdso_clock_update_guard(vdso_clock_slot &slot) noexcept : slot_(&slot) {
    const std::uint32_t gen = slot_->generation.load(std::memory_order_relaxed);
    slot_->generation.store(gen + 1, std::memory_order_seq_cst);
  }

  /** @brief Ends the update: stores `generation + 1` again (back to even) with `memory_order_seq_cst`, so every
   * field write made through this guard is visible to any reader that observes the new, even generation. A
   * no-op if this guard was moved from. */
  ~vdso_clock_update_guard() noexcept {
    if (slot_ != nullptr) {
      const std::uint32_t gen = slot_->generation.load(std::memory_order_relaxed);
      slot_->generation.store(gen + 1, std::memory_order_seq_cst);
    }
  }

  vdso_clock_update_guard(const vdso_clock_update_guard &) = delete;
  vdso_clock_update_guard &operator=(const vdso_clock_update_guard &) = delete;

  /** @brief Transfers the in-progress update to the new guard; @p other becomes a no-op on destruction. */
  vdso_clock_update_guard(vdso_clock_update_guard &&other) noexcept : slot_(other.slot_) { other.slot_ = nullptr; }

  /** @brief Sets which hardware counter backs this update's `reference_counter`. */
  void set_source(vdso_clock_source source) noexcept { slot_->source = source; }

  /** @brief Sets the counter's current frequency, in Hz. */
  void set_counter_hz(std::uint64_t counter_hz) noexcept { slot_->counter_hz = counter_hz; }

  /** @brief Sets the correlated counter sample/reference-instant pair: @p counter is the raw hardware counter
   * reading taken at the same moment @p reference_time represents (a duration since the Unix epoch for
   * `vdso_clock_id::realtime`, or since an arbitrary kernel-chosen epoch for `vdso_clock_id::monotonic`). */
  void set_reference(cycles counter, duration reference_time) noexcept {
    slot_->reference_counter = counter.raw();
    slot_->reference_secs = reference_time.as_secs();
    slot_->reference_subsec_nanos = reference_time.subsec_nanos();
  }

private:
  vdso_clock_slot *slot_;
};

/**
 * @brief Thin, non-owning convenience wrapper around a single
 * @ref vdso_clock_slot that republishes a full snapshot in one call;
 * equivalent to manually driving a @ref vdso_clock_update_guard through
 * all three setters. See the @file-level docs for the mutual-exclusion
 * precondition.
 */
class vdso_clock_writer {
public:
  /** @param slot The slot this writer publishes to; must outlive it. */
  explicit vdso_clock_writer(vdso_clock_slot &slot) noexcept : slot_(&slot) {}

  /** @brief Begins a manual update (see @ref vdso_clock_update_guard) for call sites that want to set only some
   * fields, or set them conditionally. */
  [[nodiscard]] vdso_clock_update_guard begin_update() noexcept { return vdso_clock_update_guard{*slot_}; }

  /** @brief Publishes a complete new snapshot in one seqlock-bracketed update: @p source/@p counter_hz describe
   * the counter, @p counter/@p reference_time are the correlated sample pair (see
   * `vdso_clock_update_guard::set_reference`). */
  void publish(vdso_clock_source source, std::uint64_t counter_hz, cycles counter, duration reference_time) noexcept {
    auto guard = begin_update();
    guard.set_source(source);
    guard.set_counter_hz(counter_hz);
    guard.set_reference(counter, reference_time);
  }

private:
  vdso_clock_slot *slot_;
};

} // namespace hw
} // namespace structo
