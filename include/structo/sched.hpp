// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file sched.hpp
 * @brief Four stateless, per-CPU-trait-driven scheduling policies built
 * on `runqueue.hpp`: `structo::noop_sched`, `structo::fixed_priority_sched`,
 * `structo::sched_ule` (FreeBSD `SCHED_ULE`-flavored), and
 * `structo::sched_4bsd` (FreeBSD `SCHED_4BSD`-flavored).
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
 * ## The four policies
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
 * not verbatim ports of, their FreeBSD counterparts.
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

/** @brief `noop_sched<Entry, Hook, PerCpu>`'s per-CPU state-blob type. */
template <typename Entry, auto Hook> using noop_sched_state = fifo_runqueue<Entry, Hook>;

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
public:
  using state_type = noop_sched_state<Entry, Hook>;

  /** @brief Makes @p entry runnable, at the tail of the queue. O(1). */
  static void enqueue(Entry &entry) noexcept { PerCpu::get()->enqueue(entry); }

  /** @brief Selects and removes the next task to run, or `nullptr` if none is runnable. O(1). */
  static Entry *pick_next() noexcept { return PerCpu::get()->dequeue(); }

  /** @brief Removes @p entry. O(1). Precondition: `is_linked(entry)`. */
  static void remove(Entry &entry) noexcept { PerCpu::get()->remove(entry); }

  /** @brief Whether @p entry is currently enqueued (on any CPU). O(1). */
  [[nodiscard]] static bool is_linked(const Entry &entry) noexcept { return state_type::is_linked(entry); }

  [[nodiscard]] static bool empty() noexcept { return PerCpu::get()->empty(); }
  [[nodiscard]] static std::size_t size() noexcept { return PerCpu::get()->size(); }
};

// --------------------------------------------------------------------
// fixed_priority_sched
// --------------------------------------------------------------------

/** @brief `fixed_priority_sched<...>`'s per-CPU state-blob type. */
template <typename Entry, auto Hook, auto Priority, std::size_t NumPriorities>
using fixed_priority_sched_state = priority_bucket_runqueue<Entry, Hook, Priority, NumPriorities>;

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
public:
  using state_type = fixed_priority_sched_state<Entry, Hook, Priority, NumPriorities>;

  /** @brief Makes @p entry runnable, at the tail of its `Entry.*Priority` bucket. O(1). */
  static void enqueue(Entry &entry) noexcept { PerCpu::get()->enqueue(entry); }

  /** @brief Selects and removes the highest-priority runnable task, or `nullptr`. O(1). */
  static Entry *pick_next() noexcept { return PerCpu::get()->dequeue(); }

  /**
   * @brief Re-enqueues a just-dispatched @p entry at the tail of its own
   * bucket -- call this (instead of leaving it dispatched) when a
   * caller-armed, tickless round-robin quantum timer fires while
   * @p entry is still runnable, for `SCHED_RR`-like behavior. O(1).
   */
  static void requeue(Entry &entry) noexcept { PerCpu::get()->enqueue(entry); }

  /** @brief Removes @p entry. O(1). Precondition: `is_linked(entry)`. */
  static void remove(Entry &entry) noexcept { PerCpu::get()->remove(entry); }

  /** @brief Whether @p entry is currently enqueued (on any CPU). O(1). */
  [[nodiscard]] static bool is_linked(const Entry &entry) noexcept { return state_type::is_linked(entry); }

  [[nodiscard]] static bool empty() noexcept { return PerCpu::get()->empty(); }
  [[nodiscard]] static std::size_t size() noexcept { return PerCpu::get()->size(); }
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

/** @brief `sched_ule<...>`'s per-CPU state-blob type: the `curr`/`next` queue pair. */
template <typename Entry, auto Hook, auto Priority, std::size_t NumPriorities> struct sched_ule_state {
  priority_bucket_runqueue<Entry, Hook, Priority, NumPriorities> curr{};
  priority_bucket_runqueue<Entry, Hook, Priority, NumPriorities> next{};
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

  /** @brief Makes a never-before-seen @p entry runnable, as fully interactive. O(1). */
  static void enqueue(Entry &entry, instant now) noexcept {
    auto &st = entry.*State;
    st.sleep_started = now;
    st.run_micros = 0;
    st.sleep_micros = 0;
    st.in_next_queue = true;
    assign_priority(entry, interact_max);
    PerCpu::get()->next.enqueue(entry);
  }

  /**
   * @brief Selects and removes the next task to run from `curr`
   * (swapping `curr`/`next` first if `curr` is empty), or `nullptr` if
   * both are empty. O(1).
   */
  static Entry *pick_next(instant now) noexcept {
    auto *blob = PerCpu::get();
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

  /** @brief A dispatched @p entry blocks (becomes non-runnable); not requeued until `on_wake`. O(1). */
  static void on_block(Entry &entry, instant now) noexcept {
    accumulate_run(entry, now);
    (entry.*State).sleep_started = now;
  }

  /** @brief A previously-`on_block`ed @p entry becomes runnable again; re-scores and requeues it. O(1). */
  static void on_wake(Entry &entry, instant now) noexcept {
    accumulate_sleep(entry, now);
    requeue_after_run(entry);
  }

  /** @brief Removes @p entry from whichever of `curr`/`next` it currently sits in. O(1). Precondition:
   * `is_linked(entry)`. */
  static void remove(Entry &entry) noexcept {
    auto *blob = PerCpu::get();
    if ((entry.*State).in_next_queue)
      blob->next.remove(entry);
    else
      blob->curr.remove(entry);
  }

  /** @brief Whether @p entry is currently enqueued in `curr` or `next` (on any CPU). O(1). */
  [[nodiscard]] static bool is_linked(const Entry &entry) noexcept { return bucket_type::is_linked(entry); }

  [[nodiscard]] static bool empty() noexcept {
    auto *blob = PerCpu::get();
    return blob->curr.empty() && blob->next.empty();
  }

  [[nodiscard]] static std::size_t size() noexcept {
    auto *blob = PerCpu::get();
    return blob->curr.size() + blob->next.size();
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

  static void requeue_after_run(Entry &entry) noexcept {
    auto &st = entry.*State;
    const std::uint64_t total = st.run_micros + st.sleep_micros;
    const std::uint64_t score = total == 0 ? interact_max : (st.sleep_micros * interact_max) / total;
    assign_priority(entry, score);
    st.in_next_queue = true;
    PerCpu::get()->next.enqueue(entry);
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

/** @brief `sched_4bsd<...>`'s per-CPU state-blob type. */
template <typename Entry, auto Hook, auto Priority, std::size_t NumPriorities>
using sched_4bsd_state = priority_bucket_runqueue<Entry, Hook, Priority, NumPriorities>;

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

  /** @brief Makes a (possibly never-before-seen) @p entry runnable. O(1). */
  static void enqueue(Entry &entry, instant now) noexcept {
    auto &st = entry.*State;
    if (st.last_decay == instant{})
      st.last_decay = now; // first use: nothing to decay yet
    decay(entry, now);
    recompute_priority(entry);
    PerCpu::get()->enqueue(entry);
  }

  /** @brief Selects and removes the highest-priority runnable task, or `nullptr`. O(1). */
  static Entry *pick_next(instant now) noexcept {
    Entry *entry = PerCpu::get()->dequeue();
    if (entry != nullptr)
      (entry->*State).dispatched_at = now;
    return entry;
  }

  /** @brief A dispatched @p entry voluntarily gives up the CPU but stays runnable; re-scores and requeues it. O(1). */
  static void on_yield(Entry &entry, instant now) noexcept {
    accumulate_runtime(entry, now);
    decay(entry, now);
    recompute_priority(entry);
    PerCpu::get()->enqueue(entry);
  }

  /** @brief A dispatched @p entry blocks (becomes non-runnable); not requeued until `on_wake`. O(1). */
  static void on_block(Entry &entry, instant now) noexcept { accumulate_runtime(entry, now); }

  /** @brief A previously-`on_block`ed @p entry becomes runnable again; catches up decay, re-scores, and requeues it.
   * O(1). */
  static void on_wake(Entry &entry, instant now) noexcept {
    decay(entry, now);
    recompute_priority(entry);
    PerCpu::get()->enqueue(entry);
  }

  /** @brief Removes @p entry. O(1). Precondition: `is_linked(entry)`. */
  static void remove(Entry &entry) noexcept { PerCpu::get()->remove(entry); }

  /** @brief Sets @p entry's `nice` value (like `setpriority(2)`); takes effect at the next recompute. */
  static void set_nice(Entry &entry, int nice) noexcept { (entry.*State).nice = nice; }

  /** @brief Whether @p entry is currently enqueued (on any CPU). O(1). */
  [[nodiscard]] static bool is_linked(const Entry &entry) noexcept { return state_type::is_linked(entry); }

  [[nodiscard]] static bool empty() noexcept { return PerCpu::get()->empty(); }
  [[nodiscard]] static std::size_t size() noexcept { return PerCpu::get()->size(); }

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
