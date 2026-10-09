// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file time_manager.hpp
 * @brief `structo::hw::time_manager`: a tickless kernel-level timekeeping
 * facility ("timehands"/timecounter manager, in FreeBSD/Linux
 * terminology) that samples a free-running hardware counter
 * (`time_source_ref`, `time_source_ref.hpp`) to maintain both a
 * monotonic and a realtime (wall-clock) notion of "now", and optionally
 * republishes both into a `vdso_clock_page` (`vdso_clock_page.hpp`) for
 * lockless user-space reads.
 *
 * ## Tickless: no periodic internal work is required to keep time
 *
 * Unlike a classic `jiffies`-style kernel that must increment a counter
 * on every hardclock tick to track elapsed time, `time_manager` never
 * needs to run on a fixed schedule to stay correct *between* calls to
 * @ref resync -- `try_monotonic_now`/`try_realtime_now` always compute
 * the live answer as "the last published reference instant, plus
 * however far the hardware counter has advanced since", exactly the
 * same computation `vdso_clock_reader.hpp` performs in user space. The
 * one genuinely periodic obligation is @ref resync itself, needed only
 * to (a) keep the published reference recent enough that the next
 * `checked_cycles_to_duration` conversion/the next wraparound-detection
 * window stays valid (see @ref max_resync_interval), and (b) step the
 * published snapshot so a very long-idle reader doesn't have to convert
 * an implausibly large cycle count. `resync()` takes no arguments and
 * needs no scheduling policy of its own -- it is a plain entry point an
 * external event timer/callout calls on whatever cadence @ref
 * max_resync_interval recommends; this header does not arm or own that
 * timer itself.
 *
 * ## Mutual exclusion: writers are caller-serialized, reads stay lockless
 *
 * `start`/`set_realtime`/`resync` mutate shared state (the internal
 * `vdso_clock_slot`s, and the externally-bound `vdso_clock_page`'s
 * slots, if any) and are **not** internally synchronized against
 * concurrent calls to each other -- exactly like `vdso_clock_writer`
 * (`vdso_clock_writer.hpp`), whose own precondition this class inherits:
 * the caller must already hold whatever lock serializes concurrent
 * writers (e.g. a per-instance kernel spin lock) for the entire extent
 * of any one of these three calls. `try_monotonic_now`/
 * `try_realtime_now`, by contrast, are genuinely lock-free and safe to
 * call from any context (including concurrently with an in-progress
 * `resync()`), via the same seqlock-retry protocol
 * `vdso_clock_reader.hpp` uses.
 *
 * ## Handling an imperfect hardware counter: `is_monotonic`/`is_per_cpu`
 *
 * `time_source_capabilities` (`time_source_ref.hpp`) describes two
 * properties about the bound counter that materially change how
 * `time_manager` must behave, not just advisory metadata:
 *
 * - **`is_monotonic == false`** (the counter can genuinely stutter/step
 *   backward, not just wrap at its natural width): `resync()` tracks a
 *   high-water mark (the largest raw value ever observed) instead of
 *   blindly trusting "the new sample is smaller, so it must have
 *   wrapped". A sample at or below the high-water mark contributes zero
 *   elapsed time (clamped, counted in @ref backstep_count) and does
 *   **not** move the mark backward -- so the manager's own published
 *   monotonic clock itself still never regresses, and automatically
 *   *recovers* full accounting as soon as the counter climbs back above
 *   the mark, with no special-cased recovery step needed. An unstable
 *   counter must never feed a shared `vdso_clock_page`: user space has
 *   no way to apply this same high-water-mark recovery against a live
 *   counter read that dips below the last published reference (its
 *   `checked_sub` would simply fail), so @ref try_create refuses to
 *   bind a `vdso_clock_page` together with an `is_monotonic == false`
 *   source.
 * - **`is_per_cpu == true`** (the raw counter is not a single,
 *   globally-consistent count -- e.g. a non-invariant, unsynchronized
 *   TSC): a snapshot published by one CPU is meaningless correlated
 *   against a counter read taken on a *different* CPU, which is exactly
 *   what a shared, system-wide `vdso_clock_page` requires every reader
 *   (on any core) to do safely. `try_create` therefore also refuses to
 *   bind a `vdso_clock_page` together with an `is_per_cpu == true`
 *   source; such a source may still back a `time_manager` used purely
 *   for this-CPU-local, non-VDSO timekeeping (one instance per core).
 *
 * ## Switching the bound counter: a single-slot republish, not a timehands ring
 *
 * @ref switch_source rebinds the hardware counter in place (e.g. an
 * unstable TSC falling back to a platform timer) by finalizing elapsed
 * time against the outgoing counter and republishing a fresh reference
 * pair from the incoming one -- all under one seqlock generation bump
 * per slot, same as every other writer method. A multi-generation
 * "timehands ring" (FreeBSD's `windup`-style design, which keeps several
 * past snapshots alive so a slow reader is never forced to retry against
 * an in-progress writer) is deliberately not used here: writers are
 * already fully caller-serialized and a slot's publish is a handful of
 * plain stores, so the existing bounded seqlock retry
 * (@ref max_read_attempts) has effectively no realistic chance of being
 * exhausted by a source switch; a ring would only earn its complexity
 * under very different constraints (e.g. lock-free concurrent writers,
 * or smoothing a live frequency recalibration instead of a discrete
 * switch).
 */

#include <structo/hw/clock_cycles.hpp>
#include <structo/hw/time_source_ref.hpp>
#include <structo/hw/vdso_clock_page.hpp>
#include <structo/hw/vdso_clock_writer.hpp>

#include <reloco/duration.hpp>
#include <reloco/error.hpp>

#include <atomic>
#include <cstdint>

namespace structo {

using namespace reloco;

namespace hw {

/**
 * @brief Tickless timehands manager: samples a @ref time_source_ref to
 * maintain monotonic/realtime "now", optionally republished into a
 * @ref vdso_clock_page. See the @file-level docs for the full model,
 * the caller-owned-mutex precondition on the writer methods, and how
 * `is_monotonic`/`is_per_cpu` are handled.
 */
class time_manager {
public:
  /** @brief Upper bound on seqlock retry attempts in `try_monotonic_now`/`try_realtime_now` before giving up
   * with `error::try_again`; see `vdso_clock_reader.hpp`'s identical bound/rationale. */
  static constexpr std::uint32_t max_read_attempts = 100;

  constexpr time_manager() noexcept = default;

  /** @brief Movable (needed so `try_create` can return `result<time_manager>` by value); not copyable, since
   * `vdso_clock_slot` embeds a `std::atomic` and copying a `time_manager` would otherwise silently produce two
   * independent instances racing to publish into the same `vdso_page_`, if any. Moving is safe here because it
   * only ever happens before any reader/writer thread has observed the moved-from instance (e.g. the single
   * `result<time_manager>` returned by `try_create`); the atomic generation counters are relocated with a plain
   * (non-atomic) load/store rather than an atomic exchange. */
  time_manager(time_manager &&other) noexcept
      : counter_(other.counter_), source_(other.source_), caps_(other.caps_), vdso_page_(other.vdso_page_),
        rtc_offset_(other.rtc_offset_), backstep_count_(other.backstep_count_),
        live_backstep_count_(other.live_backstep_count_.load(std::memory_order_relaxed)), started_(other.started_) {
    move_slot(monotonic_slot_, other.monotonic_slot_);
    move_slot(realtime_slot_, other.realtime_slot_);
  }

  time_manager &operator=(time_manager &&other) noexcept {
    if (this != &other) {
      counter_ = other.counter_;
      source_ = other.source_;
      caps_ = other.caps_;
      vdso_page_ = other.vdso_page_;
      rtc_offset_ = other.rtc_offset_;
      backstep_count_ = other.backstep_count_;
      live_backstep_count_.store(other.live_backstep_count_.load(std::memory_order_relaxed), std::memory_order_relaxed);
      started_ = other.started_;
      move_slot(monotonic_slot_, other.monotonic_slot_);
      move_slot(realtime_slot_, other.realtime_slot_);
    }
    return *this;
  }

  time_manager(const time_manager &) = delete;
  time_manager &operator=(const time_manager &) = delete;

  /**
   * @brief Constructs a `time_manager` bound to @p counter, following `reloco`'s fallible-construction
   * convention (see `docs/fallible-construction.md`): `time_manager` has real preconditions on its counter
   * (a known-nonzero frequency; `vdso_page` binding additionally requires a monotonic, globally-consistent
   * counter) that cannot be satisfied by a default constructor.
   * @param counter The free-running hardware counter to sample. Must be bound (see `time_source_ref::operator
   * bool`); copied into the returned `time_manager` (a `time_source_ref` is itself just a two-word, non-owning
   * handle, same as every other `structo` `*_ref`).
   * @param source Which concrete hardware counter @p counter reads from, published into every slot's
   * informational `vdso_clock_source` field (`vdso_clock_page.hpp`) -- `time_source_ref` itself is
   * backend-agnostic and does not know this.
   * @param vdso_page If non-null, every `start`/`set_realtime`/`resync` call also republishes both clocks into
   * this page's `realtime`/`monotonic` slots for lockless user-space reads (`vdso_clock_reader.hpp`). Must
   * outlive this `time_manager`. Left null for a this-CPU-local, non-VDSO-backed manager.
   * @return `error::unsupported_operation` if @p counter is unbound; `error::invalid_argument` if its
   * capabilities report a zero `clock_hz`, or if @p vdso_page is non-null while the counter is not both
   * `is_monotonic` and *not* `is_per_cpu` (see the @file-level docs' `is_monotonic`/`is_per_cpu` section);
   * otherwise whatever error `time_source_ref::capabilities()` itself reports.
   */
  [[nodiscard]] static result<time_manager> try_create(time_source_ref counter, vdso_clock_source source,
                                                       vdso_clock_page *vdso_page = nullptr) noexcept {
    if (!counter)
      return unexpected(error::unsupported_operation);

    auto caps = counter.capabilities();
    if (!caps)
      return unexpected(caps.error());
    if (caps->clock_hz == 0)
      return unexpected(error::invalid_argument);
    if (vdso_page != nullptr && (!caps->is_monotonic || caps->is_per_cpu))
      return unexpected(error::invalid_argument);

    return time_manager(counter, source, *caps, vdso_page);
  }

  /**
   * @brief Establishes the initial monotonic baseline. Must be called exactly once before `resync`/
   * `set_realtime`/`try_monotonic_now`/`try_realtime_now` are used. Caller must hold whatever lock serializes
   * concurrent writers (see the @file-level docs).
   * @param initial_monotonic The monotonic clock's value as of right now (e.g. `duration{}` to start counting
   * from zero at boot, matching "seconds since boot" convention).
   */
  [[nodiscard]] result<void> start(duration initial_monotonic = {}) noexcept {
    auto raw = counter_.try_now();
    if (!raw)
      return unexpected(raw.error());

    publish(monotonic_slot_, raw.value(), initial_monotonic);
    publish(realtime_slot_, raw.value(), initial_monotonic + rtc_offset_);
    if (vdso_page_ != nullptr) {
      publish(vdso_page_->slot(vdso_clock_id::monotonic), raw.value(), initial_monotonic);
      publish(vdso_page_->slot(vdso_clock_id::realtime), raw.value(), initial_monotonic + rtc_offset_);
    }
    started_ = true;
    return {};
  }

  /**
   * @brief Sets the realtime (wall-clock) offset relative to the monotonic clock -- e.g. after reading an RTC
   * chip at boot or completing an NTP sync -- and immediately republishes the realtime clock (it does not wait
   * for the next `resync()`). Does not affect monotonic timekeeping. Caller-locked, same as `resync()`.
   * @param wall_clock_now The current wall-clock instant (a duration since the Unix epoch).
   * @return `error::not_initialized` if `start()` has not been called yet; `error::integer_overflow` if
   * @p wall_clock_now is somehow before the current monotonic reading (never true for a real Unix-epoch
   * timestamp against a since-boot monotonic clock, but checked rather than assumed).
   */
  [[nodiscard]] result<void> set_realtime(duration wall_clock_now) noexcept {
    if (!started_)
      return unexpected(error::not_initialized);

    const duration current_monotonic = reference_instant(monotonic_slot_);
    auto offset = wall_clock_now.checked_sub(current_monotonic);
    if (!offset)
      return unexpected(offset.error());
    rtc_offset_ = offset.value();

    const cycles ref_counter{monotonic_slot_.reference_counter};
    const duration new_realtime = current_monotonic + rtc_offset_;
    publish(realtime_slot_, ref_counter, new_realtime);
    if (vdso_page_ != nullptr)
      publish(vdso_page_->slot(vdso_clock_id::realtime), ref_counter, new_realtime);
    return {};
  }

  /**
   * @brief The periodic entry point an external event timer/callout calls to resynchronize both clocks against
   * the live hardware counter and republish them. Takes no arguments and schedules nothing itself -- see
   * @ref max_resync_interval for how often the caller should arrange to call it, and the @file-level docs for
   * exactly how an `is_monotonic == false` counter's backward steps are handled. Caller-locked, same as
   * `start()`/`set_realtime()`.
   */
  [[nodiscard]] result<void> resync() noexcept {
    if (!started_)
      return unexpected(error::not_initialized);

    auto raw = counter_.try_now();
    if (!raw)
      return unexpected(raw.error());

    const elapsed_sample sample = compute_elapsed(raw.value());
    if (sample.backstep)
      ++backstep_count_;

    auto elapsed = checked_cycles_to_duration(sample.delta, caps_.clock_hz);
    if (!elapsed)
      return unexpected(elapsed.error());

    const duration new_monotonic = reference_instant(monotonic_slot_) + elapsed.value();
    const duration new_realtime = new_monotonic + rtc_offset_;

    publish(monotonic_slot_, sample.new_high_water, new_monotonic);
    publish(realtime_slot_, sample.new_high_water, new_realtime);
    if (vdso_page_ != nullptr) {
      publish(vdso_page_->slot(vdso_clock_id::monotonic), sample.new_high_water, new_monotonic);
      publish(vdso_page_->slot(vdso_clock_id::realtime), sample.new_high_water, new_realtime);
    }
    return {};
  }

  /**
   * @brief Live-switches the bound hardware counter (e.g. falling back from an unstable TSC to the platform's
   * HPET/ACPI PM timer, or picking up a newly-calibrated clock source) without losing monotonic continuity or
   * requiring a multi-slot "timehands ring" -- see the @file-level docs' switching-rationale note. Caller-locked,
   * same as `start()`/`set_realtime()`/`resync()`.
   *
   * This first finalizes elapsed time against the *outgoing* counter exactly like `resync()` would (so no time
   * is lost or double-counted across the switch), then samples the *incoming* counter once and republishes both
   * clocks correlated against that same finalized monotonic instant -- i.e. the switch itself behaves as one
   * atomic combination of "resync, then rebind". Because every field of a slot (including `source`/`counter_hz`)
   * is republished together under one seqlock generation bump (`vdso_clock_writer.hpp`), a concurrent lockless
   * reader can never observe a torn mix of the old and new source's fields; it just sees either the pre-switch or
   * post-switch snapshot.
   * @param new_counter The replacement free-running counter. Must be bound; its own capabilities are validated
   * exactly like `try_create`'s (nonzero `clock_hz`; `is_monotonic`/`is_per_cpu` compatible with `vdso_page_`, if
   * bound).
   * @param new_source Which concrete hardware counter @p new_counter reads from (see `try_create`'s matching
   * parameter).
   * @return `error::not_initialized` if `start()` has not been called yet; `error::unsupported_operation` if
   * @p new_counter is unbound; `error::invalid_argument` if its capabilities report a zero `clock_hz`, or are
   * incompatible with an already-bound `vdso_page_`; otherwise whatever error sampling either counter or
   * converting elapsed cycles reports. On any error, the manager is left bound to the *outgoing* counter,
   * unmodified.
   */
  [[nodiscard]] result<void> switch_source(time_source_ref new_counter, vdso_clock_source new_source) noexcept {
    if (!started_)
      return unexpected(error::not_initialized);
    if (!new_counter)
      return unexpected(error::unsupported_operation);

    auto new_caps = new_counter.capabilities();
    if (!new_caps)
      return unexpected(new_caps.error());
    if (new_caps->clock_hz == 0)
      return unexpected(error::invalid_argument);
    if (vdso_page_ != nullptr && (!new_caps->is_monotonic || new_caps->is_per_cpu))
      return unexpected(error::invalid_argument);

    // Finalize elapsed time against the outgoing counter/capabilities first, so the switch neither loses nor
    // double-counts the interval since the last resync()/start().
    auto raw_old = counter_.try_now();
    if (!raw_old)
      return unexpected(raw_old.error());
    const elapsed_sample sample = compute_elapsed(raw_old.value());
    if (sample.backstep)
      ++backstep_count_;
    auto elapsed = checked_cycles_to_duration(sample.delta, caps_.clock_hz);
    if (!elapsed)
      return unexpected(elapsed.error());
    const duration switch_monotonic = reference_instant(monotonic_slot_) + elapsed.value();

    // Sample the incoming counter once, to correlate it with switch_monotonic as the new reference pair; any
    // sub-microsecond gap between this read and the one above becomes the (negligible, one-time) switch error.
    auto raw_new = new_counter.try_now();
    if (!raw_new)
      return unexpected(raw_new.error());

    counter_ = new_counter;
    source_ = new_source;
    caps_ = new_caps.value();

    const duration switch_realtime = switch_monotonic + rtc_offset_;
    publish(monotonic_slot_, raw_new.value(), switch_monotonic);
    publish(realtime_slot_, raw_new.value(), switch_realtime);
    if (vdso_page_ != nullptr) {
      publish(vdso_page_->slot(vdso_clock_id::monotonic), raw_new.value(), switch_monotonic);
      publish(vdso_page_->slot(vdso_clock_id::realtime), raw_new.value(), switch_realtime);
    }
    return {};
  }

  /** @brief The monotonic clock's current value (a duration since whatever instant `start()` was called with),
   * computed locklessly: safe to call concurrently with an in-progress `resync()`/`set_realtime()` from any
   * context. `error::not_initialized` if `start()` has not been called yet; see `try_now`'s own docs for every
   * other failure mode. */
  [[nodiscard]] result<duration> try_monotonic_now() const noexcept { return try_now(monotonic_slot_); }

  /** @copydoc try_monotonic_now
   * @brief The realtime (wall-clock) clock's current value (a duration since the Unix epoch, once `set_realtime`
   * has been called at least once; since an arbitrary kernel-chosen epoch otherwise, matching `start()`'s own
   * initial-monotonic convention until corrected). */
  [[nodiscard]] result<duration> try_realtime_now() const noexcept { return try_now(realtime_slot_); }

  /**
   * @brief The longest recommended interval between `resync()` calls, derived from the bound counter's
   * frequency/wraparound bound: half the time it would take the raw counter to complete one full wrap, so a
   * single wraparound between two consecutive `resync()` calls is always unambiguous (see the @file-level docs'
   * wraparound-detection rationale). Purely advisory for an `is_monotonic == false` source (which never performs
   * wraparound arithmetic -- see the @file-level docs), where a shorter interval only reduces how often/how long
   * a backward glitch is clamped, not correctness.
   * @return `error::division_by_zero`/`error::integer_overflow` propagated from the underlying conversion (not
   * expected in practice for any real hardware counter's frequency/width).
   */
  [[nodiscard]] result<duration> max_resync_interval() const noexcept {
    auto full_period = checked_cycles_to_duration(caps_.max_value, caps_.clock_hz);
    if (!full_period)
      return unexpected(full_period.error());
    return full_period.value().checked_div(2);
  }

  /** @brief The bound counter's capabilities, as cached at `try_create` time. */
  [[nodiscard]] const time_source_capabilities &capabilities() const noexcept { return caps_; }

  /** @brief How many `resync()`/`switch_source()` calls (the caller-locked writer path) have observed the
   * counter at or below the high-water mark (a backward step/stutter) and clamped their contribution to zero
   * elapsed time -- always `0` for an `is_monotonic == true` source (see the @file-level docs). Intended for
   * diagnostics/alerting on a misbehaving counter, not a correctness signal callers need to act on directly. */
  [[nodiscard]] std::uint64_t backstep_count() const noexcept { return backstep_count_; }

  /** @brief How many lock-free `try_monotonic_now`/`try_realtime_now` reads have observed the counter at or
   * below the slot's currently-published reference (and so returned the pinned, un-regressed instant instead of
   * advancing it) since this `time_manager` was created -- the read-path counterpart to @ref backstep_count,
   * always `0` for an `is_monotonic == true` source. Deliberately *not* surfaced as a per-call error: doing so
   * would force every `try_monotonic_now`/`try_realtime_now` caller to be prepared to take the writer lock and
   * call `resync()` before retrying, which is incompatible with those reads being safe to call from any context
   * (including ones that cannot take that lock at all, like `vdso_clock_reader.hpp`'s user-space readers --
   * though an unstable counter is never actually exposed there, since `try_create`/`switch_source` refuse to
   * bind a `vdso_clock_page` to one). Instead, this counter is a cheap, non-blocking signal a monitoring task can
   * poll and act on at its own discretion -- e.g. calling `resync()` sooner than @ref max_resync_interval would
   * otherwise require, or `switch_source()`-ing away from a persistently misbehaving counter -- without any read
   * path ever having to block or retry against the writer lock. */
  [[nodiscard]] std::uint64_t live_backstep_count() const noexcept {
    return live_backstep_count_.load(std::memory_order_relaxed);
  }

private:
  /** @brief One `compute_elapsed` result: how much time elapsed (already clamped/wrap-corrected as needed), the
   * raw value to publish as the new reference counter, and whether this sample was a clamped backward step. */
  struct elapsed_sample {
    cycles delta;
    cycles new_high_water;
    bool backstep;
  };

  time_manager(time_source_ref counter, vdso_clock_source source, time_source_capabilities caps,
               vdso_clock_page *vdso_page) noexcept
      : counter_(counter), source_(source), caps_(caps), vdso_page_(vdso_page) {}

  [[nodiscard]] static duration reference_instant(const vdso_clock_slot &slot) noexcept {
    return duration::from_secs(slot.reference_secs) + duration::from_nanos(slot.reference_subsec_nanos);
  }

  /** @brief Relocates @p from into @p to field-by-field, including a plain (non-atomic) transfer of
   * `generation`; see the move-constructor docs above for why this is safe (no concurrent reader/writer can
   * observe either slot mid-move). */
  static void move_slot(vdso_clock_slot &to, const vdso_clock_slot &from) noexcept {
    to.generation.store(from.generation.load(std::memory_order_relaxed), std::memory_order_relaxed);
    to.source = from.source;
    to.counter_hz = from.counter_hz;
    to.reference_counter = from.reference_counter;
    to.reference_secs = from.reference_secs;
    to.reference_subsec_nanos = from.reference_subsec_nanos;
  }

  void publish(vdso_clock_slot &slot, cycles counter, duration reference_time) noexcept {
    vdso_clock_writer{slot}.publish(source_, caps_.clock_hz, counter, reference_time);
  }

  /**
   * @brief The wraparound-/backstep-aware "how far has the counter advanced from `last` to `raw_now`" delta
   * shared by `resync()` (via @ref compute_elapsed) and the lockless @ref try_now read path: both need the same
   * handling of an `is_monotonic == false` source's backward dips (see the @file-level docs), and a monotonic
   * source can just as easily be observed to have wrapped (or been reset -- see below) by a live
   * `try_monotonic_now`/`try_realtime_now` call between two `resync()`s as by `resync()` itself.
   * @param backstep Set to `true` if @p raw_now was at or below @p last and was *not* trusted as a genuine
   * elapsed-time step (clamped to zero elapsed instead); left untouched otherwise. True either for an
   * `is_monotonic == false` source's backward dip, or for an `is_monotonic == true` source whose implied
   * "exactly one wrap" delta exceeds the plausibility bound below.
   */
  [[nodiscard]] cycles elapsed_delta(cycles raw_now, cycles last, bool &backstep) const noexcept {
    if (!caps_.is_monotonic) {
      // Unstable counter: never interpret a drop as a wraparound (see the @file-level docs). Clamp to "no time
      // passed" so a later sample that climbs back above `last` resumes advancing correctly with no
      // special-cased recovery step.
      if (raw_now.raw() > last.raw())
        return cycles{raw_now.raw() - last.raw()};
      backstep = true;
      return cycles{0};
    }

    if (raw_now.raw() >= last.raw())
      return cycles{raw_now.raw() - last.raw()};

    // Candidate "exactly one wrap since `last`" delta -- see max_resync_interval(). A genuine wrap and a
    // hardware reset (e.g. a per-core counter zeroed by a power-off/power-on cycle) are indistinguishable by
    // *sign* alone: both present as "raw_now < last". They are not indistinguishable by *magnitude*: a genuine
    // wrap observed at or below the max_resync_interval()-recommended cadence can imply at most half the
    // counter's full range worth of elapsed cycles, by construction. A reset, by contrast, typically drops
    // `last` from anywhere in its range (often close to its maximum, for a long-running counter) down near
    // zero, which this same arithmetic would otherwise read as an implausibly large *forward* time jump --
    // silently wrong, and considerably worse than conservatively freezing. So: trust the wrap interpretation
    // only below that half-range bound; above it, treat `raw_now` the same as an untrusted backward step (see
    // the `is_monotonic == false` branch above) rather than risk a bogus jump. A sustained reset (one that
    // never climbs back above `last`) keeps re-triggering this same clamp on every subsequent call, which is
    // exactly what makes it visible via `backstep_count()`/`live_backstep_count()` for a caller to act on (e.g.
    // by rebaselining via `switch_source()` once it independently knows the counter was reset).
    cycles candidate;
    std::uint64_t half_range;
    if (caps_.max_value.raw() == UINT64_MAX) {
      candidate = raw_now.wrapping_sub(last);
      half_range = UINT64_MAX / 2;
    } else {
      const std::uint64_t period = caps_.max_value.raw() + 1; // max_value != UINT64_MAX here; cannot overflow
      candidate = cycles{period - last.raw() + raw_now.raw()};
      half_range = period / 2;
    }
    if (candidate.raw() <= half_range)
      return candidate;

    backstep = true;
    return cycles{0};
  }

  [[nodiscard]] elapsed_sample compute_elapsed(cycles raw_now) const noexcept {
    const cycles last{monotonic_slot_.reference_counter};
    bool backstep = false;
    const cycles delta = elapsed_delta(raw_now, last, backstep);
    return {delta, backstep ? last : raw_now, backstep};
  }

  /**
   * @brief The seqlock-retry read shared by `try_monotonic_now`/`try_realtime_now`: retries @p slot's generation
   * until a consistent snapshot is observed, then samples the live counter and converts the elapsed cycles to a
   * duration -- the same algorithm `vdso_clock_reader<Traits>::try_now` uses, adapted to read through this
   * instance's own (fallible, runtime-bound) `time_source_ref` instead of a compile-time `Traits::read_counter`
   * hook.
   */
  [[nodiscard]] result<duration> try_now(const vdso_clock_slot &slot) const noexcept {
    vdso_clock_source src{};
    std::uint64_t hz = 0;
    std::uint64_t ref_counter = 0;
    std::uint64_t ref_secs = 0;
    std::uint32_t ref_nanos = 0;

    bool consistent = false;
    for (std::uint32_t attempt = 0; attempt < max_read_attempts; ++attempt) {
      const std::uint32_t gen_before = slot.generation.load(std::memory_order_seq_cst);
      if ((gen_before & 1u) != 0u)
        continue;

      src = slot.source;
      hz = slot.counter_hz;
      ref_counter = slot.reference_counter;
      ref_secs = slot.reference_secs;
      ref_nanos = slot.reference_subsec_nanos;

      const std::uint32_t gen_after = slot.generation.load(std::memory_order_seq_cst);
      if (gen_after == gen_before) {
        consistent = true;
        break;
      }
    }
    if (!consistent)
      return unexpected(error::try_again);
    if (src == vdso_clock_source::none)
      return unexpected(error::not_initialized);

    auto raw_now = counter_.try_now();
    if (!raw_now)
      return unexpected(raw_now.error());

    bool backstep = false;
    const cycles delta = elapsed_delta(raw_now.value(), cycles{ref_counter}, backstep);
    if (backstep)
      live_backstep_count_.fetch_add(1, std::memory_order_relaxed);
    auto elapsed = checked_cycles_to_duration(delta, hz);
    if (!elapsed)
      return unexpected(elapsed.error());

    return duration::from_secs(ref_secs) + duration::from_nanos(ref_nanos) + elapsed.value();
  }

  time_source_ref counter_{};
  vdso_clock_source source_{vdso_clock_source::none};
  time_source_capabilities caps_{};
  vdso_clock_page *vdso_page_ = nullptr;
  vdso_clock_slot monotonic_slot_{};
  vdso_clock_slot realtime_slot_{};
  duration rtc_offset_{};
  std::uint64_t backstep_count_ = 0;
  /** @brief Bumped (relaxed, non-blocking) by the const `try_now` read path; see @ref live_backstep_count. Marked
   * `mutable` since observing a live backstep is a read-path diagnostic event, not a logical state mutation of
   * the manager itself. */
  mutable std::atomic<std::uint64_t> live_backstep_count_{0};
  bool started_ = false;
};

} // namespace hw
} // namespace structo
