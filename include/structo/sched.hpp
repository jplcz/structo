// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file sched.hpp
 * @brief Five stateless, per-CPU-trait-driven scheduling policies built
 * on `runqueue.hpp`: `structo::noop_sched`, `structo::fixed_priority_sched`,
 * `structo::edf_sched` (Earliest Deadline First), `structo::sched_ule`
 * (FreeBSD `SCHED_ULE`-flavored), and `structo::sched_4bsd` (FreeBSD
 * `SCHED_4BSD`-flavored).
 *
 * ## Tickless: no periodic tick counting, anywhere
 *
 * Every policy here is purely *event-driven*: a scheduling decision is
 * made only in response to an explicit call -- `enqueue` (a task becomes
 * runnable for the first time), `pick_next` (the CPU wants to dispatch
 * something), `on_yield`/`on_block` (the running task gives up the CPU,
 * voluntarily or by sleeping), `on_wake` (a sleeping task becomes
 * runnable again) -- never by a periodic hardware timer interrupt
 * counting elapsed "ticks". Where a policy needs real elapsed time (CPU
 * usage decay, interactivity scoring), the caller passes an explicit
 * `reloco::instant now` at each such call, and the policy computes
 * elapsed *wall-clock duration* (`now - some_earlier_instant`) lazily,
 * exactly once per transition -- never by sampling a running counter on
 * a fixed period. This is the same tickless philosophy real tickless
 * kernels (`NO_HZ_FULL` Linux, FreeBSD's `nohz`) apply to the scheduler
 * specifically: only arm a timer (e.g. `structo::callout`, elsewhere in
 * this library) for the *next actual deadline* that matters (a
 * round-robin quantum boundary, a sleep timeout), never a fixed-period
 * heartbeat.
 *
 * ## One uniform interface across all five, so they're interchangeable
 *
 * Every policy exposes the exact same ten method names and signatures
 * -- `enqueue(entry, now)`, `pick_next(now)`, `on_yield(entry, now)`,
 * `on_block(entry, now)`, `on_wake(entry, now)`, `requeue(entry, now)`,
 * `remove(entry)`, `force_next(entry)`, `is_linked(entry)`, `empty()`,
 * `size()` -- so generic/templated caller code (a test harness driving
 * "whichever policy this instantiation picked" through one fixed call
 * sequence, in particular) never has to branch on *which* of the five
 * is bound. `noop_sched`/`fixed_priority_sched`/`edf_sched` accept
 * @p now as a defaulted, unused parameter on every method that takes it
 * (they have no clock/duration logic of their own) purely for this
 * signature parity -- it's `[[maybe_unused]]`/discarded, never read.
 * Where a policy has nothing extra to compute for an operation (e.g.
 * `on_block` for the three clock-free policies: `pick_next` already
 * removed the entry from the queue, so there's nothing to track until
 * the matching `on_wake`/`enqueue`), that method is a documented no-op
 * or a plain alias for whichever other method does the equivalent work
 * (e.g. `requeue` for `sched_ule`/`sched_4bsd` is exactly `on_yield`:
 * both mean "this still-runnable entry needs to be re-scored and put
 * back", whether that happened voluntarily or because a round-robin
 * quantum expired). `sched_4bsd::set_nice` is the one deliberate
 * exception to full parity -- no other policy has any notion of a
 * caller-adjustable
 * "niceness", so faking a no-op `set_nice` elsewhere would silently
 * discard a caller's intent rather than genuinely support it.
 *
 * ## `force_next`: bypassing priority order entirely for one dispatch
 *
 * All five policies also support one additional, deliberately
 * out-of-band operation: `force_next(entry)` unlinks @p entry from
 * wherever it currently sits (if linked at all -- see below) and pins
 * it in a dedicated one-entry slot that the very next `pick_next` call
 * always checks *first*, before any of the policy's own ordering --
 * i.e. "this exact task must run next, no matter what else is
 * runnable or how it compares in priority/deadline/interactivity
 * score". This exists for protocols that need to hand a CPU to one
 * specific task immediately and unconditionally -- e.g.
 * `sched_world_handoff.hpp`'s cross-world handoff, where an interrupt
 * needing urgent routing elsewhere must make a dedicated "world
 * thread" entry win over *any* locally runnable task, regardless of
 * that task's priority. Like `remove`, `force_next` takes no @p now --
 * it is a purely structural operation, never involving a policy's
 * clock-driven recompute logic (it does not re-score, decay, or touch
 * `Entry.*Priority`/`Entry.*Deadline` at all).
 *
 * Two things to know before reaching for it:
 * - **At most one entry can be pinned at a time.** Calling
 *   `force_next` again before the previous pin is consumed by
 *   `pick_next` re-enqueues the previous one through the policy's
 *   normal path first, so it is never silently lost -- but it does
 *   lose its "runs next" guarantee at that point.
 * - **`force_next` accepts an already-blocked (unlinked) @p entry**,
 *   not just a currently-queued one -- useful for resuming an entry
 *   that was deliberately `on_block`ed so this one could be forced
 *   into its place. This is only behaviorally safe when that specific
 *   block was arranged as part of the same handoff protocol; forcing
 *   an entry that is blocked for an unrelated reason (waiting on a
 *   lock, I/O, a condition variable, ...) would dispatch it before
 *   whatever it is actually waiting for has happened -- `force_next`
 *   has no way to tell the difference, so this is the caller
 *   protocol's responsibility, not something this header can enforce.
 *   Symmetrically, if a pinned-but-not-yet-dispatched @p entry is then
 *   independently `on_block`ed (blocking before `pick_next` ever
 *   consumed its pin), the stale pin is automatically cleared so
 *   `pick_next` doesn't later hand out an entry the caller now
 *   considers blocked.
 *
 * ## `Entry` owns its own per-task scheduling state
 *
 * Exactly like `runqueue.hpp`, these policies are intrusive and
 * non-owning: `Entry` is caller-owned storage (a task-control-block
 * field, typically) that must outlive every operation referencing it.
 * All per-task bookkeeping a policy needs -- the intrusive link,
 * priority, and (for `sched_ule`/`sched_4bsd`) accumulated runtime/
 * sleep-time/decay timestamps -- lives directly inside `Entry`, named
 * to the policy via pointer-to-member non-type template parameters,
 * continuing `runqueue.hpp`'s `Hook`/`Priority` convention with one
 * addition, `State`, for the extra bookkeeping `sched_ule`/`sched_4bsd`
 * need (plain structs `structo::ule_task_state`/`structo::bsd_task_state`
 * below -- embed one as a named field and point `State` at it).
 *
 * ## Per-CPU *global* data is resolved via a caller-supplied `PerCpu` trait
 *
 * None of these policies hold a runqueue (or any other per-CPU state) as
 * a data member -- every method is `static`, and every method resolves
 * "this CPU's scheduler state" through a caller-supplied `PerCpu` trait
 * parameter, exactly mirroring `structo::arch::per_cpu_ptr<Tag, T>`'s
 * own storage-free design (see `arch/per_cpu_ptr.hpp`): `PerCpu` must
 * provide
 *
 * @code
 * static state_type *get() noexcept; // current CPU's scheduler state blob
 * @endcode
 *
 * where `state_type` is the policy's own per-CPU state-blob alias (see
 * each policy below) -- `structo::arch::per_cpu_ptr<Tag, state_type>`
 * *already* implements exactly this contract (plus `get(cpu)`/
 * `set(cpu, ptr)` for explicit-CPU access during per-CPU bring-up), so
 * it is the natural, zero-glue-code choice for `PerCpu`: each CPU's real
 * backing storage (a `state_type` instance, wherever that CPU's own
 * bring-up code puts it -- static per-CPU BSS, a boot-time array indexed
 * by CPU, ...) is registered once via `per_cpu_ptr<Tag,
 * state_type>::set(cpu, &blob)`, and every policy method thereafter
 * resolves it with zero indirection beyond that one pointer load.
 *
 * @code
 * struct my_task {
 *   struct { my_task *next = nullptr; my_task **prev = nullptr; } link;
 *   unsigned priority = 0;
 * };
 *
 * using state_type = structo::fixed_priority_sched_state<my_task, &my_task::link, &my_task::priority, 32>;
 *
 * struct my_sched_percpu_tag {
 *   static inline constexpr std::size_t max_cpus = 64;
 *   static inline void *slots[max_cpus]{nullptr};
 *   static void *get_ptr(std::size_t cpu) noexcept { return slots[cpu]; }
 *   static void set_ptr(std::size_t cpu, void *ptr) noexcept { slots[cpu] = ptr; }
 *   static std::size_t current() noexcept { return hw_current_cpu_index(); }
 * };
 * using my_percpu = structo::arch::per_cpu_ptr<my_sched_percpu_tag, state_type>;
 * using my_sched = structo::fixed_priority_sched<my_task, &my_task::link, &my_task::priority, 32, my_percpu>;
 *
 * // Once per CPU, during that CPU's own bring-up:
 * static state_type g_rq_storage[my_percpu::max_cpus];
 * my_percpu::set(this_cpu, &g_rq_storage[this_cpu]);
 *
 * // Thereafter, from any context running on that CPU:
 * my_sched::enqueue(some_task);
 * my_task *next = my_sched::pick_next();
 * @endcode
 *
 * ## The five policies
 *
 * - **`noop_sched<Entry, Hook, PerCpu>`**: plain FIFO, no priority
 *   concept -- the trivial, always-correct baseline a kernel boots with
 *   before a real policy is installed, or the entirety of what a
 *   single-priority cooperative kernel needs. Built on
 *   `fifo_runqueue`.
 * - **`fixed_priority_sched<Entry, Hook, Priority, NumPriorities,
 *   PerCpu>`**: static priorities the scheduler never recomputes --
 *   `Entry.*Priority` is set once by the caller and read-only
 *   thereafter. `enqueue`/`pick_next`/`remove` give POSIX
 *   `SCHED_FIFO`-like behavior (same-priority tasks run to completion/
 *   block in FIFO order); calling `requeue` instead of `remove` when an
 *   externally-armed, tickless round-robin quantum (a one-shot
 *   `structo::callout` the caller arms for `now + quantum` at dispatch
 *   time) expires upgrades that to `SCHED_RR`-like behavior, with zero
 *   ticking inside this header. Built on `priority_bucket_runqueue`.
 * - **`edf_sched<Entry, Hook, Deadline, PerCpu>`**: Earliest Deadline
 *   First -- the classic, uniprocessor-optimal (Liu & Layland, 1973)
 *   real-time policy: always dispatches whichever runnable task has the
 *   soonest absolute `reloco::instant` deadline. `Entry.*Deadline` is
 *   plain caller-owned storage (typically set to `now + relative
 *   deadline` right before `enqueue`) -- this scheduler never reads a
 *   clock itself, it only ever compares two already-computed instants,
 *   making it (like `fixed_priority_sched`) entirely clock-free
 *   internally despite ordering by time. Built directly on
 *   `priority_list_runqueue` (sorted insertion by `Entry.*Deadline`).
 * - **`sched_ule<Entry, Hook, Priority, State, NumPriorities, PerCpu>`**:
 *   a simplified `SCHED_ULE` -- two per-CPU `priority_bucket_runqueue`s
 *   (`curr`/`next`), always dispatching from `curr` and refilling it by
 *   swapping with `next` once `curr` empties (ULE's hallmark two-queue
 *   batching), plus a per-task *interactivity score* computed from
 *   accumulated run-time vs. sleep-time (`ule_task_state`) that
 *   reclassifies a task into a reserved high-urgency priority range
 *   (`sleep`-dominant, "interactive") or a single shared batch level
 *   (`run`-dominant, "batch") every time it's requeued. See "What's
 *   simplified" below for exactly how this departs from real `SCHED_ULE`.
 * - **`sched_4bsd<Entry, Hook, Priority, State, NumPriorities, PerCpu>`**:
 *   a simplified `SCHED_4BSD` -- a single per-CPU
 *   `priority_bucket_runqueue`, with priority recomputed from a decayed
 *   `estcpu` (accumulated CPU-usage estimate) plus a caller-settable
 *   `nice` value (`bsd_task_state`), matching 4BSD's classic `priority =
 *   PUSER + estcpu/4 + 2*nice` shape. Real 4BSD decays `estcpu`
 *   geometrically on a periodic ~1s tick; this header instead decays it
 *   lazily, by right-shifting once per elapsed decay-half-life of
 *   *real* wall-clock time since the last recompute (computed from
 *   `now - state.last_decay`, never a counted tick) -- an integer
 *   approximation of the same geometric decay, with zero periodic timer
 *   dependency.
 *
 * ## What's simplified (these are *basic*, not production, schedulers)
 *
 * Both `sched_ule` and `sched_4bsd` are deliberately simplified relative
 * to their FreeBSD namesakes: no SMP load balancing/migration, no
 * priority-inversion/priority-propagation handling, `sched_ule`'s
 * interactivity score is a single `sleep / (sleep + run)` ratio (not
 * `SCHED_ULE`'s exact piecewise formula) and its "batch" range is one
 * shared FIFO level rather than a further CPU-usage-ranked sub-range,
 * and `sched_4bsd`'s decay is an integer power-of-two approximation of
 * 4BSD's real fixed-point decay constant. Both are namesaked after,
 * not verbatim ports of, their FreeBSD counterparts. `edf_sched` is
 * EDF's bare scheduling rule only -- it performs no admission-control/
 * schedulability check, so an overloaded task set (one that would miss
 * deadlines under *any* policy) is entirely the caller's concern, same
 * as real EDF implementations (e.g. Linux's `SCHED_DEADLINE`) layer
 * admission control on top of, not inside, the dispatch rule itself.
 *
 * ## Putting it together: SMP load balancing via `work_steal.hpp`
 *
 * "No SMP load balancing/migration" above describes what *this* header
 * does on its own -- a single CPU's dispatch rule. Multi-CPU balancing
 * is layered entirely on top, out of three other, independently usable
 * pieces: `cpu_load` (per-CPU runnable-task tracking, `cpu_load.hpp`),
 * `arch::cpu_sibling_map` (precomputed, distance-ordered steal
 * candidates, `arch/cpu_sibling_map.hpp`), and `find_steal_candidate`
 * plus a steal policy (the *decision* of which CPU to steal from,
 * `work_steal.hpp`) -- this header has no opinion on any of it, exactly
 * like it has no opinion on locking. The sketch below is pseudocode (it
 * elides real per-CPU storage/locking/IPI wake-up plumbing, which is
 * entirely OS-specific), showing where each piece plugs into one CPU's
 * idle-vs-dispatch path:
 *
 * @code
 * // Per-CPU state this fake OS already maintains elsewhere:
 * //   my_sched              -- e.g. structo::fixed_priority_sched<...> for this CPU
 * //   percpu_load[cpu]      -- structo::cpu_load<...>, updated on every enqueue/remove
 * //   siblings[cpu]         -- structo::arch::cpu_sibling_map<...>, built once at boot
 * //   online                -- structo::arch::cpu_online_dispatcher, hotplug-maintained
 *
 * void scheduler_tick(std::size_t this_cpu, instant now) {
 *   if (Entry *next = my_sched::pick_next(now)) {
 *     dispatch(next); // local work available -- no need to even look at other CPUs
 *     return;
 *   }
 *
 *   // Local runqueue is empty: this CPU is about to idle. Worth a last-resort
 *   // steal before halting -- `local_load == 0` here trivially satisfies every
 *   // built-in policy's `should_attempt_steal`, so this call is never skipped.
 *   auto eligible = online.snapshot(); // arch::cpu_mask<Tag, MaxCpus>
 *   for (int attempt = 0; attempt < max_steal_attempts; ++attempt) {
 *     auto victim = structo::find_steal_candidate<structo::performance_steal_policy>(
 *         siblings[this_cpu], this_cpu, eligible,
 *         [&](std::size_t cpu) { return percpu_load[cpu].current(); });
 *     if (!victim.has_value())
 *       break; // nobody (left) worth stealing from this round -- genuinely idle
 *
 *     // Caller owns all locking: lock *victim's remote runqueue, re-check it
 *     // isn't empty (it may have changed since the decision above), pop one
 *     // task, unlock, then enqueue it into my_sched on this_cpu.
 *     if (Entry *stolen = try_steal_one_task_from(*victim)) {
 *       my_sched::enqueue(*stolen, now);
 *       dispatch(my_sched::pick_next(now));
 *       return;
 *     }
 *     eligible.clear(*victim); // that one didn't pan out -- don't reconsider it this round
 *     if (eligible.none())
 *       eligible = online.snapshot(); // every candidate excluded -- refresh for hotplug changes
 *   }
 *   halt_until_next_interrupt(); // truly nothing runnable anywhere reachable
 * }
 * @endcode
 *
 * Swapping `performance_steal_policy` for `power_save_steal_policy`/
 * `always_steal_policy` (see `work_steal.hpp`) changes nothing else
 * about this loop -- the policy alone decides whether/which candidate
 * qualifies; `scheduler_tick` itself never changes based on which OS
 * power/performance policy is configured.
 */

#include <cstddef>
#include <cstdint>
#include <reloco/duration.hpp>
#include <reloco/instant.hpp>
#include <structo/arch/per_cpu_ptr.hpp>
#include <structo/runqueue.hpp>
#include <type_traits>
#include <utility>

namespace structo {

using namespace reloco;

// --------------------------------------------------------------------
// noop_sched
// --------------------------------------------------------------------

/** @brief `noop_sched<Entry, Hook, PerCpu>`'s per-CPU state-blob type: the FIFO queue plus one
 * `force_next`-pinned slot (see `noop_sched::force_next`). */
template <typename Entry, auto Hook> struct noop_sched_state {
  fifo_runqueue<Entry, Hook> queue{};
  Entry *pinned = nullptr;
};

/**
 * @brief Trivial FIFO scheduler: no priority concept, no per-task state
 * beyond the intrusive link. The baseline every other policy here is
 * compared against.
 * @tparam Entry Caller-owned task type; must embed an intrusive link
 * field matching `reloco::c_tailq`'s hook layout, named by @p Hook.
 * @tparam Hook Pointer-to-member of @p Entry's link field.
 * @tparam PerCpu Resolves the current CPU's `noop_sched_state<Entry,
 * Hook>` blob; see the file-level "Per-CPU *global* data" section.
 */
template <typename Entry, auto Hook, typename PerCpu> class noop_sched {
  using queue_type = fifo_runqueue<Entry, Hook>;

public:
  using state_type = noop_sched_state<Entry, Hook>;

  /** @brief Makes @p entry runnable, at the tail of the queue. O(1). @p now is accepted but unused -- this
   * policy has no clock/duration logic of its own -- purely so generic caller code can drive any of this
   * header's five policies through one uniform `enqueue(entry, now)` call site; see the file-level "Common
   * operations" section. */
  static void enqueue(Entry &entry, instant now = instant{}) noexcept {
    (void)now;
    auto *st = PerCpu::get();
    // A stale `force_next` pin on @p entry must be released first: a pinned entry is deliberately unlinked
    // from the queue, so enqueuing it without clearing the pin would both double-link it and leave the pin
    // dangling toward an entry `pick_next` would also hand out separately.
    if (st->pinned == &entry)
      st->pinned = nullptr;
    st->queue.enqueue(entry);
  }

  /** @brief Selects and removes the next task to run, or `nullptr` if none is runnable -- a pending
   * `force_next` pin (if any) always wins over the queue's own order. O(1). @p now is accepted but unused; see
   * `enqueue`'s doc. */
  static Entry *pick_next(instant now = instant{}) noexcept {
    (void)now;
    auto *st = PerCpu::get();
    if (Entry *pinned = st->pinned) {
      st->pinned = nullptr;
      return pinned;
    }
    return st->queue.dequeue();
  }

  /** @brief A dispatched @p entry voluntarily gives up the CPU but stays runnable; re-enqueues it at the tail,
   * same as `enqueue` -- this policy has no interactivity/decay state to recompute. O(1). */
  static void on_yield(Entry &entry, instant now = instant{}) noexcept { enqueue(entry, now); }

  /** @brief A dispatched @p entry blocks (becomes non-runnable); normally a no-op -- `pick_next` already
   * removed it from the queue, and this policy has nothing else to track until `on_wake`/`enqueue` -- except
   * that if @p entry happens to be the currently `force_next`-pinned one (blocking before ever being
   * dispatched/consumed by `pick_next`), the stale pin is cleared so `pick_next` doesn't later hand out an
   * entry the caller now considers blocked. O(1). */
  static void on_block(Entry &entry, instant now = instant{}) noexcept {
    (void)now;
    auto *st = PerCpu::get();
    if (st->pinned == &entry)
      st->pinned = nullptr;
  }

  /** @brief A previously-`on_block`ed @p entry becomes runnable again; re-enqueues it at the tail, same as
   * `enqueue`. O(1). */
  static void on_wake(Entry &entry, instant now = instant{}) noexcept { enqueue(entry, now); }

  /** @brief Re-enqueues a just-dispatched @p entry at the tail -- call this (instead of leaving it dispatched)
   * when a caller-armed, tickless round-robin quantum timer fires while @p entry is still runnable. Same as
   * `enqueue`: this policy has nothing extra to recompute on quantum expiry. O(1). */
  static void requeue(Entry &entry, instant now = instant{}) noexcept { enqueue(entry, now); }

  /** @brief Removes @p entry. O(1). Precondition: `is_linked(entry)` -- unless @p entry is the current
   * `force_next` pin, which is not linked by construction; in that case the pin is simply released instead of
   * touching the queue. */
  static void remove(Entry &entry) noexcept {
    auto *st = PerCpu::get();
    if (st->pinned == &entry) {
      st->pinned = nullptr;
      return;
    }
    st->queue.remove(entry);
  }

  /**
   * @brief Unlinks @p entry from wherever it currently sits (if linked; an already-blocked, unlinked @p entry
   * is also accepted) and pins it so the very next `pick_next` call returns it unconditionally, bypassing this
   * policy's normal ordering entirely -- e.g. "the CPU must resume this exact task right now, regardless of
   * whatever else is runnable". O(1). @p now is accepted but unused; see `enqueue`'s doc. At most one entry can
   * be pinned at a time: calling `force_next` again before the previous pin is consumed re-enqueues the
   * previous one normally first, so it is never silently lost.
   */
  static void force_next(Entry &entry) noexcept {
    auto *st = PerCpu::get();
    if (queue_type::is_linked(entry))
      st->queue.remove(entry);
    if (st->pinned != nullptr)
      st->queue.enqueue(*st->pinned);
    st->pinned = &entry;
  }

  /** @brief Whether @p entry is currently enqueued (on any CPU). O(1). */
  [[nodiscard]] static bool is_linked(const Entry &entry) noexcept { return queue_type::is_linked(entry); }

  [[nodiscard]] static bool empty() noexcept {
    auto *st = PerCpu::get();
    return st->pinned == nullptr && st->queue.empty();
  }
  [[nodiscard]] static std::size_t size() noexcept {
    auto *st = PerCpu::get();
    return st->queue.size() + (st->pinned != nullptr ? 1 : 0);
  }
};

// --------------------------------------------------------------------
// fixed_priority_sched
// --------------------------------------------------------------------

/** @brief `fixed_priority_sched<...>`'s per-CPU state-blob type: the priority-bucket queue plus one
 * `force_next`-pinned slot (see `fixed_priority_sched::force_next`). */
template <typename Entry, auto Hook, auto Priority, std::size_t NumPriorities> struct fixed_priority_sched_state {
  priority_bucket_runqueue<Entry, Hook, Priority, NumPriorities> queue{};
  Entry *pinned = nullptr;
};

/**
 * @brief Static-priority scheduler -- `Entry.*Priority` is set once by
 * the caller and never recomputed. `SCHED_FIFO`-like if the caller only
 * ever `remove`s a dispatched task when it blocks/exits; `SCHED_RR`-like
 * if the caller instead `requeue`s it when an externally-armed,
 * tickless quantum timer expires.
 * @tparam Entry Caller-owned task type; must embed an intrusive link
 * field matching `reloco::c_tailq`'s hook layout (named by @p Hook) and
 * an unsigned priority field (named by @p Priority, lower value runs
 * first).
 * @tparam Hook Pointer-to-member of @p Entry's link field.
 * @tparam Priority Pointer-to-member of @p Entry's priority field.
 * @tparam NumPriorities Number of distinct priority levels.
 * @tparam PerCpu Resolves the current CPU's `fixed_priority_sched_state<
 * Entry, Hook, Priority, NumPriorities>` blob.
 */
template <typename Entry, auto Hook, auto Priority, std::size_t NumPriorities, typename PerCpu>
class fixed_priority_sched {
  using queue_type = priority_bucket_runqueue<Entry, Hook, Priority, NumPriorities>;

public:
  using state_type = fixed_priority_sched_state<Entry, Hook, Priority, NumPriorities>;

  /** @brief Makes @p entry runnable, at the tail of its `Entry.*Priority` bucket. O(1). @p now is accepted but
   * unused -- this policy has no clock/duration logic of its own -- purely so generic caller code can drive any
   * of this header's five policies through one uniform `enqueue(entry, now)` call site; see the file-level
   * "Common operations" section. */
  static void enqueue(Entry &entry, instant now = instant{}) noexcept {
    (void)now;
    auto *st = PerCpu::get();
    // A stale `force_next` pin on @p entry must be released first: a pinned entry is deliberately unlinked
    // from the queue, so enqueuing it without clearing the pin would both double-link it and leave the pin
    // dangling toward an entry `pick_next` would also hand out separately.
    if (st->pinned == &entry)
      st->pinned = nullptr;
    st->queue.enqueue(entry);
  }

  /** @brief Selects and removes the highest-priority runnable task, or `nullptr` -- a pending `force_next` pin
   * (if any) always wins over the queue's own priority order. O(1). @p now is accepted but unused; see
   * `enqueue`'s doc. */
  static Entry *pick_next(instant now = instant{}) noexcept {
    (void)now;
    auto *st = PerCpu::get();
    if (Entry *pinned = st->pinned) {
      st->pinned = nullptr;
      return pinned;
    }
    return st->queue.dequeue();
  }

  /**
   * @brief Re-enqueues a just-dispatched @p entry at the tail of its own
   * bucket -- call this (instead of leaving it dispatched) when a
   * caller-armed, tickless round-robin quantum timer fires while
   * @p entry is still runnable, for `SCHED_RR`-like behavior. O(1).
   * @p now is accepted but unused; see `enqueue`'s doc.
   */
  static void requeue(Entry &entry, instant now = instant{}) noexcept { enqueue(entry, now); }

  /** @brief A dispatched @p entry voluntarily gives up the CPU but stays runnable; same as `requeue` -- this
   * policy has no interactivity/decay state to recompute. O(1). */
  static void on_yield(Entry &entry, instant now = instant{}) noexcept { requeue(entry, now); }

  /** @brief A dispatched @p entry blocks (becomes non-runnable); normally a no-op -- `pick_next` already
   * removed it from the queue, and this policy has nothing else to track until `on_wake`/`enqueue` -- except
   * that if @p entry happens to be the currently `force_next`-pinned one (blocking before ever being
   * dispatched/consumed by `pick_next`), the stale pin is cleared so `pick_next` doesn't later hand out an
   * entry the caller now considers blocked. O(1). */
  static void on_block(Entry &entry, instant now = instant{}) noexcept {
    (void)now;
    auto *st = PerCpu::get();
    if (st->pinned == &entry)
      st->pinned = nullptr;
  }

  /** @brief A previously-`on_block`ed @p entry becomes runnable again; same as `enqueue`. O(1). */
  static void on_wake(Entry &entry, instant now = instant{}) noexcept { enqueue(entry, now); }

  /** @brief Removes @p entry. O(1). Precondition: `is_linked(entry)` -- unless @p entry is the current
   * `force_next` pin, which is not linked by construction; in that case the pin is simply released instead of
   * touching the queue. */
  static void remove(Entry &entry) noexcept {
    auto *st = PerCpu::get();
    if (st->pinned == &entry) {
      st->pinned = nullptr;
      return;
    }
    st->queue.remove(entry);
  }

  /**
   * @brief Unlinks @p entry from wherever it currently sits (if linked; an already-blocked, unlinked @p entry
   * is also accepted) and pins it so the very next `pick_next` call returns it unconditionally, bypassing this
   * policy's normal priority order entirely. O(1). @p now is accepted but unused; see `enqueue`'s doc. At most
   * one entry can be pinned at a time: calling `force_next` again before the previous pin is consumed
   * re-enqueues the previous one normally first, so it is never silently lost.
   */
  static void force_next(Entry &entry) noexcept {
    auto *st = PerCpu::get();
    if (queue_type::is_linked(entry))
      st->queue.remove(entry);
    if (st->pinned != nullptr)
      st->queue.enqueue(*st->pinned);
    st->pinned = &entry;
  }

  /** @brief Whether @p entry is currently enqueued (on any CPU). O(1). */
  [[nodiscard]] static bool is_linked(const Entry &entry) noexcept { return queue_type::is_linked(entry); }

  [[nodiscard]] static bool empty() noexcept {
    auto *st = PerCpu::get();
    return st->pinned == nullptr && st->queue.empty();
  }
  [[nodiscard]] static std::size_t size() noexcept {
    auto *st = PerCpu::get();
    return st->queue.size() + (st->pinned != nullptr ? 1 : 0);
  }
};

// --------------------------------------------------------------------
// edf_sched
// --------------------------------------------------------------------

/** @brief `edf_sched<...>`'s per-CPU state-blob type: the deadline-ordered queue plus one
 * `force_next`-pinned slot (see `edf_sched::force_next`). */
template <typename Entry, auto Hook, auto Deadline> struct edf_sched_state {
  priority_list_runqueue<Entry, Hook, Deadline> queue{};
  Entry *pinned = nullptr;
};

/**
 * @brief Earliest Deadline First: always dispatches whichever runnable
 * task has the soonest absolute `reloco::instant` deadline --
 * uniprocessor-optimal among dynamic-priority real-time policies (Liu &
 * Layland, 1973). `Entry.*Deadline` is plain caller-owned storage: the
 * caller sets it (typically `now + relative_deadline`) before each
 * `enqueue`; this scheduler never reads a clock or recomputes a
 * deadline itself, it only ever *compares* two already-computed
 * instants via `priority_list_runqueue`'s ordinary `operator<`-ordered
 * insertion -- genuinely tickless despite ordering by time.
 * @tparam Entry Caller-owned task type; must embed an intrusive link
 * field matching `reloco::c_tailq`'s hook layout (named by @p Hook) and
 * a `reloco::instant` deadline field (named by @p Deadline, soonest
 * deadline runs first); read-only to this policy.
 * @tparam Hook Pointer-to-member of @p Entry's link field.
 * @tparam Deadline Pointer-to-member of @p Entry's deadline field.
 * @tparam PerCpu Resolves the current CPU's `edf_sched_state<Entry,
 * Hook, Deadline>` blob.
 */
template <typename Entry, auto Hook, auto Deadline, typename PerCpu> class edf_sched {
  using queue_type = priority_list_runqueue<Entry, Hook, Deadline>;

public:
  using state_type = edf_sched_state<Entry, Hook, Deadline>;

  /** @brief Makes @p entry runnable, inserted in `Entry.*Deadline` order. O(n). @p now is accepted but unused --
   * this policy never reads a clock, only compares already-computed deadlines -- purely so generic caller code
   * can drive any of this header's five policies through one uniform `enqueue(entry, now)` call site; see the
   * file-level "Common operations" section. */
  static void enqueue(Entry &entry, instant now = instant{}) noexcept {
    (void)now;
    auto *st = PerCpu::get();
    // A stale `force_next` pin on @p entry must be released first: a pinned entry is deliberately unlinked
    // from the queue, so enqueuing it without clearing the pin would both double-link it and leave the pin
    // dangling toward an entry `pick_next` would also hand out separately.
    if (st->pinned == &entry)
      st->pinned = nullptr;
    st->queue.enqueue(entry);
  }

  /** @brief Selects and removes the task with the soonest deadline, or `nullptr` if none is runnable -- a
   * pending `force_next` pin (if any) always wins over deadline order. O(1). @p now is accepted but unused; see
   * `enqueue`'s doc. */
  static Entry *pick_next(instant now = instant{}) noexcept {
    (void)now;
    auto *st = PerCpu::get();
    if (Entry *pinned = st->pinned) {
      st->pinned = nullptr;
      return pinned;
    }
    return st->queue.dequeue();
  }

  /** @brief Re-inserts a just-dispatched @p entry in `Entry.*Deadline` order -- call this (instead of leaving it
   * dispatched) when @p entry is still runnable after being dispatched (e.g. a cooperative yield). Same as
   * `enqueue`: this policy has nothing extra to recompute. O(n). */
  static void requeue(Entry &entry, instant now = instant{}) noexcept { enqueue(entry, now); }

  /** @brief A dispatched @p entry voluntarily gives up the CPU but stays runnable; same as `requeue`. O(n). */
  static void on_yield(Entry &entry, instant now = instant{}) noexcept { requeue(entry, now); }

  /** @brief A dispatched @p entry blocks (becomes non-runnable); normally a no-op -- `pick_next` already
   * removed it from the queue, and this policy has nothing else to track until `on_wake`/`enqueue` -- except
   * that if @p entry happens to be the currently `force_next`-pinned one (blocking before ever being
   * dispatched/consumed by `pick_next`), the stale pin is cleared so `pick_next` doesn't later hand out an
   * entry the caller now considers blocked. O(1). */
  static void on_block(Entry &entry, instant now = instant{}) noexcept {
    (void)now;
    auto *st = PerCpu::get();
    if (st->pinned == &entry)
      st->pinned = nullptr;
  }

  /** @brief A previously-`on_block`ed @p entry becomes runnable again; same as `enqueue`. O(n). */
  static void on_wake(Entry &entry, instant now = instant{}) noexcept { enqueue(entry, now); }

  /** @brief Removes @p entry. O(1). Precondition: `is_linked(entry)` -- unless @p entry is the current
   * `force_next` pin, which is not linked by construction; in that case the pin is simply released instead of
   * touching the queue. */
  static void remove(Entry &entry) noexcept {
    auto *st = PerCpu::get();
    if (st->pinned == &entry) {
      st->pinned = nullptr;
      return;
    }
    st->queue.remove(entry);
  }

  /**
   * @brief Unlinks @p entry from wherever it currently sits (if linked; an already-blocked, unlinked @p entry
   * is also accepted) and pins it so the very next `pick_next` call returns it unconditionally, bypassing
   * deadline order entirely. O(n) (the unlink is O(1); re-enqueuing a previously-pinned entry, if any, is
   * O(n)). No `now` parameter: like `remove`, this is a purely structural operation with no
   * wall-clock-dependent logic. At most one entry can be pinned at a time: calling `force_next` again before
   * the previous pin is consumed re-enqueues the previous one normally first, so it is never silently lost.
   */
  static void force_next(Entry &entry) noexcept {
    auto *st = PerCpu::get();
    if (queue_type::is_linked(entry))
      st->queue.remove(entry);
    if (st->pinned != nullptr)
      st->queue.enqueue(*st->pinned);
    st->pinned = &entry;
  }

  /** @brief Whether @p entry is currently enqueued (on any CPU). O(1). */
  [[nodiscard]] static bool is_linked(const Entry &entry) noexcept { return queue_type::is_linked(entry); }

  [[nodiscard]] static bool empty() noexcept {
    auto *st = PerCpu::get();
    return st->pinned == nullptr && st->queue.empty();
  }
  [[nodiscard]] static std::size_t size() noexcept {
    auto *st = PerCpu::get();
    return st->queue.size() + (st->pinned != nullptr ? 1 : 0);
  }
};

// --------------------------------------------------------------------
// sched_ule
// --------------------------------------------------------------------

/**
 * @brief Per-task bookkeeping `sched_ule` needs: embed one as a named
 * field in `Entry` and point `sched_ule`'s `State` parameter at it.
 * Every field is maintained entirely by `sched_ule` itself -- treat
 * this as opaque, caller-owned storage, not a caller-writable API.
 */
struct ule_task_state {
  instant run_started{};
  instant sleep_started{};
  std::uint64_t run_micros = 0;
  std::uint64_t sleep_micros = 0;
  bool in_next_queue = true;
};

/** @brief `sched_ule<...>`'s per-CPU state-blob type: the `curr`/`next` queue pair plus one
 * `force_next`-pinned slot (see `sched_ule::force_next`).
 *
 * @note `force_next` accepts an already-blocked (unlinked) @p entry (see its own doc), but has no way to tell
 * *why* an entry isn't linked -- it is purely a mechanical unlink-if-linked-then-pin operation. Forcing a
 * blocked entry to be dispatched next is only behaviorally safe when that specific block was arranged as part
 * of the same handoff protocol (e.g. a task deliberately `on_block`ed so another entry could be force-run in
 * its place); forcing an entry that is blocked for an unrelated reason (waiting on a lock, I/O, ...) would
 * dispatch it before whatever it is actually waiting for has happened. Callers building a protocol on top of
 * `force_next` (see `sched_world_handoff.hpp`) must track this themselves and defer until the entry's own,
 * independent `on_wake` if its block wasn't protocol-arranged. */
template <typename Entry, auto Hook, auto Priority, std::size_t NumPriorities> struct sched_ule_state {
  priority_bucket_runqueue<Entry, Hook, Priority, NumPriorities> curr{};
  priority_bucket_runqueue<Entry, Hook, Priority, NumPriorities> next{};
  Entry *pinned = nullptr;
};

/**
 * @brief Simplified `SCHED_ULE` -- see the file-level docs for the
 * two-queue/interactivity-score design and exactly what's simplified
 * relative to real `SCHED_ULE`.
 * @tparam Entry Caller-owned task type; must embed an intrusive link
 * field (named by @p Hook), an unsigned priority field (named by
 * @p Priority, recomputed by this scheduler -- caller should treat it
 * as read-only), and a `ule_task_state` field (named by @p State).
 * @tparam Hook Pointer-to-member of @p Entry's link field.
 * @tparam Priority Pointer-to-member of @p Entry's priority field.
 * @tparam State Pointer-to-member of @p Entry's `ule_task_state` field.
 * @tparam NumPriorities Number of distinct priority levels; split in
 * half between a reserved "interactive" range and the single shared
 * "batch" level (must be at least 4 to leave both ranges non-trivial).
 * @tparam PerCpu Resolves the current CPU's `sched_ule_state<Entry,
 * Hook, Priority, NumPriorities>` blob.
 */
template <typename Entry, auto Hook, auto Priority, auto State, std::size_t NumPriorities, typename PerCpu>
class sched_ule {
  static_assert(NumPriorities >= 4, "sched_ule needs a few priority levels to separate interactive/batch ranges");

  using bucket_type = priority_bucket_runqueue<Entry, Hook, Priority, NumPriorities>;
  using priority_type = std::remove_reference_t<decltype(std::declval<Entry &>().*Priority)>;

  // Interactivity score range/threshold and batch-vs-interactive split,
  // all loosely matching real `SCHED_ULE`'s `SCHED_INTERACT_HALF`(50)/
  // `SCHED_INTERACT_THRESH`(30) constants, simplified to a single
  // `sleep / (sleep + run)` ratio (see file-level docs).
  static constexpr std::uint64_t interact_max = 100;
  static constexpr std::uint64_t interact_threshold = 30;
  // Once accumulated run+sleep history exceeds this, halve both --
  // keeps the score weighted toward *recent* behavior instead of
  // converging/freezing over a long uptime.
  static constexpr std::uint64_t aging_threshold_micros = 1'000'000;
  static constexpr std::size_t interactive_range = NumPriorities / 2;

public:
  using state_type = sched_ule_state<Entry, Hook, Priority, NumPriorities>;

  /** @brief Makes a never-before-seen @p entry runnable, as fully interactive. O(1). Also releases a stale
   * `force_next` pin on @p entry, if any (see `remove`'s doc for why: a pinned entry is deliberately unlinked,
   * so inserting it again without clearing the pin would double-link it). */
  static void enqueue(Entry &entry, instant now) noexcept {
    auto &st = entry.*State;
    st.sleep_started = now;
    st.run_micros = 0;
    st.sleep_micros = 0;
    st.in_next_queue = true;
    assign_priority(entry, interact_max);
    auto *blob = PerCpu::get();
    if (blob->pinned == &entry)
      blob->pinned = nullptr;
    blob->next.enqueue(entry);
  }

  /**
   * @brief Selects and removes the next task to run from `curr`
   * (swapping `curr`/`next` first if `curr` is empty), or `nullptr` if
   * both are empty. O(1).
   */
  static Entry *pick_next(instant now) noexcept {
    auto *blob = PerCpu::get();
    if (Entry *pinned = blob->pinned) {
      blob->pinned = nullptr;
      (pinned->*State).run_started = now;
      return pinned;
    }
    if (blob->curr.empty())
      std::swap(blob->curr, blob->next);
    Entry *entry = blob->curr.dequeue();
    if (entry != nullptr)
      (entry->*State).run_started = now;
    return entry;
  }

  /** @brief A dispatched @p entry voluntarily gives up the CPU but stays runnable; re-scores and requeues it. O(1). */
  static void on_yield(Entry &entry, instant now) noexcept {
    accumulate_run(entry, now);
    requeue_after_run(entry);
  }

  /** @brief Re-scores and requeues a just-dispatched @p entry that's still runnable -- call this (instead of
   * leaving it dispatched) when a caller-armed, tickless round-robin quantum timer fires. Same operation as
   * `on_yield`: this policy always re-scores on every return-to-runnable transition, voluntary or not. O(1). */
  static void requeue(Entry &entry, instant now) noexcept { on_yield(entry, now); }

  /** @brief A dispatched @p entry blocks (becomes non-runnable); not requeued until `on_wake`. Also clears
   * @p entry's `force_next` pin, if it happened to still be the pinned entry (blocking before ever being
   * dispatched/consumed by `pick_next`) -- so `pick_next` doesn't later hand out an entry the caller now
   * considers blocked. O(1). */
  static void on_block(Entry &entry, instant now) noexcept {
    accumulate_run(entry, now);
    (entry.*State).sleep_started = now;
    auto *blob = PerCpu::get();
    if (blob->pinned == &entry)
      blob->pinned = nullptr;
  }

  /** @brief A previously-`on_block`ed @p entry becomes runnable again; re-scores and requeues it. O(1). */
  static void on_wake(Entry &entry, instant now) noexcept {
    accumulate_sleep(entry, now);
    requeue_after_run(entry);
  }

  /** @brief Removes @p entry from whichever of `curr`/`next` it currently sits in. O(1). Precondition:
   * `is_linked(entry)` -- unless @p entry is the current `force_next` pin, which is not linked by construction;
   * in that case the pin is simply released instead of touching either queue. */
  static void remove(Entry &entry) noexcept {
    auto *blob = PerCpu::get();
    if (blob->pinned == &entry) {
      blob->pinned = nullptr;
      return;
    }
    if ((entry.*State).in_next_queue)
      blob->next.remove(entry);
    else
      blob->curr.remove(entry);
  }

  /** @brief Whether @p entry is currently enqueued in `curr` or `next` (on any CPU). O(1). */
  [[nodiscard]] static bool is_linked(const Entry &entry) noexcept { return bucket_type::is_linked(entry); }

  /**
   * @brief Unlinks @p entry from whichever of `curr`/`next` it currently sits in (if linked; an already-blocked,
   * unlinked @p entry is also accepted -- see the per-CPU state blob's doc note on when that's safe) and pins
   * it so the very next `pick_next` call returns it unconditionally, bypassing interactivity scoring entirely.
   * O(1). No `now` parameter: like `remove`, this is a purely structural operation -- it deliberately does
   * *not* update @p entry's `ule_task_state` (no `run_started`/accumulated run or sleep time change); `pick_next`
   * sets `run_started` itself once @p entry is actually dispatched. At most one entry can be pinned at a time:
   * calling `force_next` again before the previous pin is consumed re-enqueues the previous one (into `next`,
   * as `on_yield`/`on_wake` would) first, so it is never silently lost.
   */
  static void force_next(Entry &entry) noexcept {
    auto *blob = PerCpu::get();
    if (bucket_type::is_linked(entry)) {
      if ((entry.*State).in_next_queue)
        blob->next.remove(entry);
      else
        blob->curr.remove(entry);
    }
    if (blob->pinned != nullptr)
      blob->next.enqueue(*blob->pinned);
    blob->pinned = &entry;
  }

  [[nodiscard]] static bool empty() noexcept {
    auto *blob = PerCpu::get();
    return blob->pinned == nullptr && blob->curr.empty() && blob->next.empty();
  }

  [[nodiscard]] static std::size_t size() noexcept {
    auto *blob = PerCpu::get();
    return blob->curr.size() + blob->next.size() + (blob->pinned != nullptr ? 1 : 0);
  }

private:
  static void accumulate_run(Entry &entry, instant now) noexcept {
    auto &st = entry.*State;
    st.run_micros += (now - st.run_started).as_micros();
    age_if_needed(st);
  }

  static void accumulate_sleep(Entry &entry, instant now) noexcept {
    auto &st = entry.*State;
    st.sleep_micros += (now - st.sleep_started).as_micros();
    age_if_needed(st);
  }

  static void age_if_needed(ule_task_state &st) noexcept {
    if (st.run_micros + st.sleep_micros > aging_threshold_micros) {
      st.run_micros /= 2;
      st.sleep_micros /= 2;
    }
  }

  /** @brief Shared `on_yield`/`requeue`/`on_wake` tail: re-scores and re-enqueues @p entry into `next`. Also
   * releases a stale `force_next` pin on @p entry, if any -- see `enqueue`'s doc for why. */
  static void requeue_after_run(Entry &entry) noexcept {
    auto &st = entry.*State;
    const std::uint64_t total = st.run_micros + st.sleep_micros;
    const std::uint64_t score = total == 0 ? interact_max : (st.sleep_micros * interact_max) / total;
    assign_priority(entry, score);
    st.in_next_queue = true;
    auto *blob = PerCpu::get();
    if (blob->pinned == &entry)
      blob->pinned = nullptr;
    blob->next.enqueue(entry);
  }

  static void assign_priority(Entry &entry, std::uint64_t score) noexcept {
    if (score >= interact_threshold) {
      // Interactive: reserved low-numbered (highest-urgency) range,
      // scaled so a higher score -> a smaller (more urgent) priority.
      const std::uint64_t inv = interact_max - score;
      entry.*Priority = static_cast<priority_type>((inv * (interactive_range - 1)) / interact_max);
    } else {
      // Batch: a single reserved level just above the interactive
      // range, kept FIFO within it -- a deliberate simplification (see
      // file-level docs) of real `SCHED_ULE`'s further CPU-usage-ranked
      // batch sub-range.
      entry.*Priority = static_cast<priority_type>(interactive_range);
    }
  }
};

// --------------------------------------------------------------------
// sched_4bsd
// --------------------------------------------------------------------

/**
 * @brief Per-task bookkeeping `sched_4bsd` needs: embed one as a named
 * field in `Entry` and point `sched_4bsd`'s `State` parameter at it.
 * `nice` is the one field the caller is meant to set directly (like
 * `setpriority(2)`); every other field is maintained by `sched_4bsd`
 * itself.
 */
struct bsd_task_state {
  instant dispatched_at{};
  instant last_decay{};
  std::uint64_t estcpu = 0;
  int nice = 0;
};

/** @brief `sched_4bsd<...>`'s per-CPU state-blob type: the priority-bucket queue plus one
 * `force_next`-pinned slot (see `sched_4bsd::force_next`).
 *
 * @note Same caveat as `sched_ule_state`: `force_next` accepts an already-blocked (unlinked) entry
 * mechanically, but forcing one to be dispatched next is only behaviorally safe when its block was arranged
 * as part of the same handoff protocol, not an unrelated wait. */
template <typename Entry, auto Hook, auto Priority, std::size_t NumPriorities> struct sched_4bsd_state {
  priority_bucket_runqueue<Entry, Hook, Priority, NumPriorities> queue{};
  Entry *pinned = nullptr;
};

/**
 * @brief Simplified `SCHED_4BSD` -- a single per-CPU runqueue, with
 * priority recomputed from a lazily-decayed `estcpu` plus `nice`,
 * matching 4BSD's classic `priority = PUSER + estcpu/4 + 2*nice` shape.
 * See the file-level docs for exactly how decay is made tickless.
 * @tparam Entry Caller-owned task type; must embed an intrusive link
 * field (named by @p Hook), an unsigned priority field (named by
 * @p Priority, recomputed by this scheduler -- caller should treat it
 * as read-only), and a `bsd_task_state` field (named by @p State).
 * @tparam Hook Pointer-to-member of @p Entry's link field.
 * @tparam Priority Pointer-to-member of @p Entry's priority field.
 * @tparam State Pointer-to-member of @p Entry's `bsd_task_state` field.
 * @tparam NumPriorities Number of distinct priority levels.
 * @tparam PerCpu Resolves the current CPU's `sched_4bsd_state<Entry,
 * Hook, Priority, NumPriorities>` blob.
 */
template <typename Entry, auto Hook, auto Priority, auto State, std::size_t NumPriorities, typename PerCpu>
class sched_4bsd {
  using priority_type = std::remove_reference_t<decltype(std::declval<Entry &>().*Priority)>;
  using queue_type = priority_bucket_runqueue<Entry, Hook, Priority, NumPriorities>;

  // Real 4BSD halves `estcpu` roughly once per second of wall-clock
  // time (its `schedcpu()` sweep); this header reproduces that same
  // real-time period, but triggers the (possibly multi-step) decay
  // lazily -- computed from `now - state.last_decay` -- instead of
  // counting a periodic tick.
  static constexpr std::uint64_t decay_half_life_millis = 1'000;
  static constexpr std::uint64_t max_half_lives_per_step = 32; // caps a very long sleep's decay loop
  // `estcpu` accumulates in microseconds of runtime; this is the
  // (tunable) "microseconds of runtime per priority level" scale factor
  // standing in for 4BSD's `estcpu >> 2` shift (which assumes a
  // different, ticks-based `estcpu` unit).
  static constexpr std::uint64_t micros_per_priority_level = 50'000;

public:
  using state_type = sched_4bsd_state<Entry, Hook, Priority, NumPriorities>;

  /** @brief Makes a (possibly never-before-seen) @p entry runnable. O(1). Also releases a stale `force_next`
   * pin on @p entry, if any (see `remove`'s doc for why). */
  static void enqueue(Entry &entry, instant now) noexcept {
    auto &st = entry.*State;
    if (st.last_decay == instant{})
      st.last_decay = now; // first use: nothing to decay yet
    decay(entry, now);
    recompute_priority(entry);
    auto *cpu = PerCpu::get();
    if (cpu->pinned == &entry)
      cpu->pinned = nullptr;
    cpu->queue.enqueue(entry);
  }

  /** @brief Selects and removes the highest-priority runnable task, or `nullptr` -- a pending `force_next` pin
   * (if any) always wins over the queue's own priority order. O(1). */
  static Entry *pick_next(instant now) noexcept {
    auto *st = PerCpu::get();
    if (Entry *pinned = st->pinned) {
      st->pinned = nullptr;
      (pinned->*State).dispatched_at = now;
      return pinned;
    }
    Entry *entry = st->queue.dequeue();
    if (entry != nullptr)
      (entry->*State).dispatched_at = now;
    return entry;
  }

  /** @brief A dispatched @p entry voluntarily gives up the CPU but stays runnable; re-scores and requeues it.
   * Also releases a stale `force_next` pin on @p entry, if any -- see `enqueue`'s doc. O(1). */
  static void on_yield(Entry &entry, instant now) noexcept {
    accumulate_runtime(entry, now);
    decay(entry, now);
    recompute_priority(entry);
    auto *cpu = PerCpu::get();
    if (cpu->pinned == &entry)
      cpu->pinned = nullptr;
    cpu->queue.enqueue(entry);
  }

  /** @brief Re-scores and requeues a just-dispatched @p entry that's still runnable -- call this (instead of
   * leaving it dispatched) when a caller-armed, tickless round-robin quantum timer fires. Same operation as
   * `on_yield`: this policy always re-scores on every return-to-runnable transition, voluntary or not. O(1). */
  static void requeue(Entry &entry, instant now) noexcept { on_yield(entry, now); }

  /** @brief A dispatched @p entry blocks (becomes non-runnable); not requeued until `on_wake`. Also clears
   * @p entry's `force_next` pin, if it happened to still be the pinned entry (blocking before ever being
   * dispatched/consumed by `pick_next`) -- so `pick_next` doesn't later hand out an entry the caller now
   * considers blocked. O(1). */
  static void on_block(Entry &entry, instant now) noexcept {
    accumulate_runtime(entry, now);
    auto *st = PerCpu::get();
    if (st->pinned == &entry)
      st->pinned = nullptr;
  }

  /** @brief A previously-`on_block`ed @p entry becomes runnable again; catches up decay, re-scores, and requeues it.
   * Also releases a stale `force_next` pin on @p entry, if any -- see `enqueue`'s doc. O(1). */
  static void on_wake(Entry &entry, instant now) noexcept {
    decay(entry, now);
    recompute_priority(entry);
    auto *cpu = PerCpu::get();
    if (cpu->pinned == &entry)
      cpu->pinned = nullptr;
    cpu->queue.enqueue(entry);
  }

  /** @brief Removes @p entry. O(1). Precondition: `is_linked(entry)` -- unless @p entry is the current
   * `force_next` pin, which is not linked by construction; in that case the pin is simply released instead of
   * touching the queue. */
  static void remove(Entry &entry) noexcept {
    auto *st = PerCpu::get();
    if (st->pinned == &entry) {
      st->pinned = nullptr;
      return;
    }
    st->queue.remove(entry);
  }

  /**
   * @brief Unlinks @p entry from wherever it currently sits (if linked; an already-blocked, unlinked @p entry
   * is also accepted -- see `sched_4bsd_state`'s doc note on when that's safe) and pins it so the very next
   * `pick_next` call returns it unconditionally, bypassing `estcpu`/`nice` priority entirely. O(1). No `now`
   * parameter: like `remove`, this is a purely structural operation -- it deliberately does not run decay or
   * recompute `Entry.*Priority`. At most one entry can be pinned at a time: calling `force_next` again before
   * the previous pin is consumed re-enqueues the previous one normally first, so it is never silently lost.
   */
  static void force_next(Entry &entry) noexcept {
    auto *st = PerCpu::get();
    if (queue_type::is_linked(entry))
      st->queue.remove(entry);
    if (st->pinned != nullptr)
      st->queue.enqueue(*st->pinned);
    st->pinned = &entry;
  }

  /** @brief Sets @p entry's `nice` value (like `setpriority(2)`); takes effect at the next recompute. */
  static void set_nice(Entry &entry, int nice) noexcept { (entry.*State).nice = nice; }

  /** @brief Whether @p entry is currently enqueued (on any CPU). O(1). */
  [[nodiscard]] static bool is_linked(const Entry &entry) noexcept { return queue_type::is_linked(entry); }

  [[nodiscard]] static bool empty() noexcept {
    auto *st = PerCpu::get();
    return st->pinned == nullptr && st->queue.empty();
  }
  [[nodiscard]] static std::size_t size() noexcept {
    auto *st = PerCpu::get();
    return st->queue.size() + (st->pinned != nullptr ? 1 : 0);
  }

private:
  static void accumulate_runtime(Entry &entry, instant now) noexcept {
    auto &st = entry.*State;
    st.estcpu += (now - st.dispatched_at).as_micros();
  }

  static void decay(Entry &entry, instant now) noexcept {
    auto &st = entry.*State;
    const std::uint64_t elapsed_ms = (now - st.last_decay).as_millis();
    std::uint64_t half_lives = elapsed_ms / decay_half_life_millis;
    if (half_lives > 0) {
      if (half_lives > max_half_lives_per_step)
        half_lives = max_half_lives_per_step;
      st.estcpu >>= half_lives;
      st.last_decay = now;
    }
  }

  static void recompute_priority(Entry &entry) noexcept {
    auto &st = entry.*State;
    constexpr auto base = static_cast<std::int64_t>(NumPriorities / 2); // PUSER-like baseline
    std::int64_t p = base + static_cast<std::int64_t>(st.estcpu / micros_per_priority_level) + 2 * st.nice;
    if (p < 0)
      p = 0;
    if (p >= static_cast<std::int64_t>(NumPriorities))
      p = static_cast<std::int64_t>(NumPriorities) - 1;
    entry.*Priority = static_cast<priority_type>(p);
  }
};

} // namespace structo
