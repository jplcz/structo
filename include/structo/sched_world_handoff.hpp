// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file sched_world_handoff.hpp
 * @brief `structo::sched_world_handoff<Entry, Sched, Traits>`: per-CPU
 * state tracking for handing a CPU back and forth between this kernel
 * and another, cooperating execution context ("the other world" --
 * e.g. a hypervisor's other guest, a TEE's normal-world OS, or any
 * other scheme where two independent kernels take turns owning a core)
 * via one dedicated per-CPU "world thread", built on top of
 * `sched.hpp`'s `force_next`.
 *
 * ## The model: a world thread is a first-class, per-CPU-bound task
 *
 * Exactly one `Entry` per CPU -- the "world thread" -- represents "this
 * CPU is currently executing the other world". Handing the CPU to the
 * other world is not a special, scheduler-invisible context switch: it
 * is an ordinary dispatch of the world thread, through the exact same
 * `enqueue`/`pick_next`/`on_block`/`on_wake` calls every other task on
 * this CPU goes through. This keeps every other scheduler-level
 * property (SMP work-stealing over `work_steal.hpp`, per-CPU load
 * accounting over `cpu_load.hpp`, priority ordering, ...) working
 * unmodified: the world thread is just another runnable entry, not a
 * parallel, out-of-band execution mode the rest of the scheduler has
 * to know about.
 *
 * What *is* special about the world thread, and what this class
 * exists to track, is a small amount of per-CPU bookkeeping no other
 * task needs:
 * - **Forcing a return to the other world, unconditionally.** Whatever
 *   is runnable locally, once the other world must be given the CPU
 *   back (e.g. an interrupt belonging to it needs routing there), the
 *   world thread must be dispatched *next*, regardless of priority.
 *   This is exactly `sched.hpp`'s `force_next` primitive --
 *   `request_force_switch()` below calls it for the caller.
 * - **Tracking how long the other world has held the CPU.** Unlike an
 *   ordinary task, the world thread has no scheduler-visible quantum:
 *   it can run for as long as the other world chooses, cooperatively.
 *   This class measures that dwell time itself (`dwell_time()`) so a
 *   caller can both enforce a bounded budget (`should_switch_to_world()`
 *   forces a pseudo-IRQ-style reentry once `Traits::max_dwell()` is
 *   exceeded) and detect a CPU that looks stuck (`stuck_warning_due()`).
 * - **Distinguishing a cooperative entry from a forced one.** The other
 *   world is normally entered via its own accessor call (e.g. a
 *   hypercall) -- `world_entry_kind::cooperative`. It can also be
 *   entered by a forced kernel entry that happens without that
 *   cooperation at all (e.g. this kernel's own interrupt handler firing
 *   while the other world was already running) -- `world_entry_kind::
 *   critical`. This class only records which kind is in effect
 *   (`entry_kind()`); deciding which concrete work is safe to run
 *   during a critical entry is the caller's own policy, built on top of
 *   that flag.
 *
 * ## What this class does *not* do
 *
 * It does not own a runqueue, does not implement a `schedule()` loop,
 * and does not decide *what* to run -- it is a thin, per-CPU state
 * object a caller's own scheduler-dispatch code consults and updates at
 * a handful of well-defined checkpoints, alongside its ordinary
 * `Sched` calls (`enqueue`/`pick_next`/`on_block`/`on_wake`/...). See
 * "Usage" below for exactly which checkpoints.
 *
 * ## `Traits`
 *
 * @code
 * struct my_world_handoff_traits {
 *   // Budget before a cooperatively-entered other world is forced to
 *   // give the CPU back, even without an explicit switch request.
 *   static constexpr reloco::duration max_dwell() noexcept { return reloco::duration::from_millis(50); }
 *   // Threshold (normally >= max_dwell()) beyond which `stuck_warning_due()`
 *   // starts reporting true, e.g. to drive a one-shot diagnostic log line.
 *   static constexpr reloco::duration stuck_warning_threshold() noexcept {
 *     return reloco::duration::from_millis(500);
 *   }
 * };
 * @endcode
 *
 * ## Usage
 *
 * One `sched_world_handoff` instance per CPU, constructed with a
 * reference to that CPU's dedicated world-thread `Entry` (which must
 * outlive it) -- `Sched` itself is a type parameter, matching
 * `sched.hpp`'s own stateless, all-`static`-method policies, so no
 * `Sched` instance is needed:
 * @code
 * // Call sites, alongside the CPU's ordinary scheduler-dispatch code:
 *
 * // The kernel's own cross-world entry glue, right after this CPU has
 * // started (or resumed) executing the other world:
 * handoff.on_world_entered(now, structo::world_entry_kind::cooperative);
 *
 * // ...and right before actually returning control to the other world
 * // (the matching exit of the above):
 * handoff.on_world_exited(now);
 *
 * // Anywhere that discovers the other world must be given the CPU
 * // back (e.g. an IRQ routed to it fires while this kernel is running):
 * handoff.request_force_switch(now);
 *
 * // In the dispatch loop, before/instead of an ordinary `pick_next()`,
 * // once this CPU is about to run the other world cooperatively:
 * if (handoff.should_switch_to_world(now)) {
 *   sched.force_next(world_thread); // idempotent if already pinned
 * }
 *
 * // Whenever the dispatch loop calls `sched.on_block(entry, now)` for
 * // any entry, also tell the handoff object (cheap, no-op unless
 * // `entry` happened to be an outstanding resume hint -- see
 * // `on_world_entered`'s `resume_hint` parameter):
 * handoff.on_block(entry);
 *
 * // Whenever the dispatch loop calls `sched.on_wake(entry, now)` for
 * // any entry, also tell the handoff object (cheap, no-op unless
 * // `entry` is this CPU's own world thread with a switch still owed):
 * handoff.on_wake(entry);
 * @endcode
 */

#include <structo/sched.hpp>

#include <reloco/duration.hpp>
#include <reloco/instant.hpp>

#include <cstdint>

namespace structo {

/** @brief How the other world's current execution on this CPU began; see
 * `sched_world_handoff::entry_kind()`. */
enum class world_entry_kind : std::uint8_t {
  /** @brief Entered via the other world's own accessor call (e.g. a hypercall) -- the common case, in which the
   * other world may run any of its own work for up to `Traits::max_dwell()`. */
  cooperative,
  /** @brief Entered by a forced kernel entry that happened without the other world's cooperation (e.g. this
   * kernel's own interrupt handler firing while the other world was already executing) -- only whatever bounded,
   * critical subset of work the caller's own policy allows should run before returning control. */
  critical,
};

/**
 * @brief Per-CPU state tracking a cross-world handoff built on top of
 * `Sched::force_next`; see the @file-level docs above for the full
 * model and usage checkpoints.
 * @tparam Entry Caller-owned task type, matching whichever `Sched`
 * policy's own `Entry` requirement (see `sched.hpp`).
 * @tparam Sched One of `sched.hpp`'s five scheduling policies (or any
 * type exposing the same `force_next(Entry&)` static method).
 * @tparam Traits Supplies `max_dwell()`/`stuck_warning_threshold()`;
 * see the @file-level docs' `Traits` example.
 */
template <typename Entry, typename Sched, typename Traits> class sched_world_handoff {
public:
  /**
   * @param world_thread This CPU's dedicated world-thread `Entry`; must outlive this object. Not copied/enqueued
   * by the constructor -- the caller is responsible for its initial scheduling, same as any other task.
   */
  explicit sched_world_handoff(Entry &world_thread) noexcept : world_thread_(&world_thread) {}

  /**
   * @brief Records that this CPU has just started (or resumed) executing the other world. Clears any previously
   * outstanding `force_switch_needed()` and starts dwell-time tracking from @p now.
   * @param now Current time.
   * @param kind Whether this entry was cooperative or a forced, critical-only entry; see `world_entry_kind`.
   * @param resume_hint If non-null, an `Entry` that must be dispatched next inside the other world (e.g. the task
   * that was running when the other world was last forced to give up the CPU) -- forced into place immediately via
   * `Sched::force_next`, same as `request_force_switch` does for the world thread itself. Left unconsumed (not
   * cleared) if it blocks before ever being dispatched; see `on_block`.
   */
  void on_world_entered(instant now, world_entry_kind kind, Entry *resume_hint = nullptr) noexcept {
    in_world_ = true;
    entry_kind_ = kind;
    entered_at_ = now;
    force_switch_needed_ = false;
    resume_hint_ = resume_hint;
    if (resume_hint_ != nullptr)
      Sched::force_next(*resume_hint_);
  }

  /**
   * @brief Records that this CPU has just handed control back to the other world (i.e. the world thread is about
   * to return there). Pure bookkeeping: no `force_next` call is needed here, since by construction the only way
   * back into the other world is through the world thread itself having already been dispatched. At most clears
   * `force_switch_needed()` and stops dwell-time tracking.
   */
  void on_world_exited(instant now) noexcept {
    (void)now;
    in_world_ = false;
    force_switch_needed_ = false;
    resume_hint_ = nullptr;
  }

  /**
   * @brief Tell the handoff object that @p entry has just blocked (i.e. right alongside the caller's own
   * `Sched::on_block(entry, now)` call). A no-op unless @p entry is the `resume_hint` most recently passed to
   * `on_world_entered` and it blocked before ever being dispatched (consumed by `pick_next`) -- in which case the
   * stale hint is released, so a later, unrelated wake of @p entry does not re-force it into place.
   */
  void on_block(Entry &entry) noexcept {
    if (resume_hint_ == &entry)
      resume_hint_ = nullptr;
  }

  /**
   * @brief Tell the handoff object that @p entry has just become runnable again (i.e. right alongside the
   * caller's own `Sched::on_wake(entry, now)` call). A no-op unless @p entry is this CPU's own world thread *and*
   * a switch is still owed (`force_switch_needed()`) -- in which case the world thread is re-forced into place,
   * since the world thread blocking partway through a forced return (e.g. needing a lock) must not cost it its
   * "runs next" guarantee once it wakes back up.
   */
  void on_wake(Entry &entry) noexcept {
    if (force_switch_needed_ && &entry == world_thread_)
      Sched::force_next(*world_thread_);
  }

  /**
   * @brief Marks that the other world must be given the CPU back as soon as possible (e.g. an interrupt
   * belonging to it just fired) and immediately forces the world thread to be dispatched next, regardless of
   * whatever else is runnable or how it compares in priority. Idempotent: calling this again while already
   * outstanding is a cheap no-op beyond re-affirming the flag.
   */
  void request_force_switch(instant now) noexcept {
    (void)now;
    force_switch_needed_ = true;
    Sched::force_next(*world_thread_);
  }

  /**
   * @brief Whether the world thread should be dispatched next, right now -- true if `request_force_switch` was
   * called and not yet satisfied by a matching `on_world_exited`, or if the other world has been running
   * cooperatively for at least `Traits::max_dwell()` without yielding on its own (a pseudo-IRQ-style forced
   * reentry budget). The latter case also marks the switch as needed and forces the world thread into place, same
   * as `request_force_switch` -- so a caller need only check this return value, not separately force anything.
   * Always false while the other world is not currently executing (`in_world()` is false).
   */
  [[nodiscard]] bool should_switch_to_world(instant now) noexcept {
    if (!in_world_)
      return false;
    if (force_switch_needed_)
      return true;
    if (entry_kind_ == world_entry_kind::cooperative && dwell_time(now) >= Traits::max_dwell()) {
      force_switch_needed_ = true;
      Sched::force_next(*world_thread_);
      return true;
    }
    return false;
  }

  /**
   * @brief Whether the other world has now been executing on this CPU for at least `Traits::
   * stuck_warning_threshold()` without returning -- intended to drive a one-shot diagnostic warning (e.g. "CPU N
   * appears stuck inside the other world"), not a hard failure: unlike `should_switch_to_world`, this performs no
   * forcing of its own. Always false while `in_world()` is false.
   */
  [[nodiscard]] bool stuck_warning_due(instant now) const noexcept {
    return in_world_ && dwell_time(now) >= Traits::stuck_warning_threshold();
  }

  /** @brief Whether the other world is currently executing on this CPU (between a matching `on_world_entered`
   * and `on_world_exited` pair). */
  [[nodiscard]] bool in_world() const noexcept { return in_world_; }

  /** @brief How the current (or, before the first `on_world_entered`, the most recent) entry into the other
   * world began; see `world_entry_kind`. Meaningless (holds the default `cooperative`) before the first
   * `on_world_entered` call. */
  [[nodiscard]] world_entry_kind entry_kind() const noexcept { return entry_kind_; }

  /** @brief Whether a forced switch back to the other world is currently outstanding (set by
   * `request_force_switch` or by `should_switch_to_world` detecting an exceeded dwell budget; cleared by
   * `on_world_exited`). */
  [[nodiscard]] bool force_switch_needed() const noexcept { return force_switch_needed_; }

  /** @brief How long the other world has been executing on this CPU since the last `on_world_entered`, as of
   * @p now. Zero while `in_world()` is false. */
  [[nodiscard]] duration dwell_time(instant now) const noexcept {
    return in_world_ ? now.saturating_duration_since(entered_at_) : duration{};
  }

private:
  Entry *world_thread_;
  Entry *resume_hint_ = nullptr;
  bool in_world_ = false;
  bool force_switch_needed_ = false;
  world_entry_kind entry_kind_ = world_entry_kind::cooperative;
  instant entered_at_{};
};

} // namespace structo
