// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file ipi_dispatcher.hpp
 * @brief `structo::arch::ipi_dispatcher<PerCpu, Handlers, Sender, MaskT,
 * MaxReasons>`: a local orchestrator for inter-processor interrupts,
 * compatible with `hw::irqc_ref::send_ipi` but deliberately *not* bound
 * to any IRQ chip or security domain itself -- it only ever decides
 * *who* to notify and *how* the notification is represented in memory;
 * routing that decision to real hardware (choosing the IRQ chip, the
 * `irq_security_domain`, and binding `ipi_id` to an actual vector) is
 * entirely the caller's job, via @p Sender.
 *
 * ## IPIs are boot-time-fixed, never configured dynamically
 *
 * Every reason an IPI can carry is a flat, compile-time-sized index
 * `[0, MaxReasons)`, and the code that runs for a given reason is
 * resolved through @p Handlers -- a trait the kernel wires up once,
 * during early boot, with whatever fixed dispatch table it wants
 * (a `switch`, a `constexpr` array of function pointers, ...). There is
 * deliberately no `register_handler()`/`unregister_handler()` runtime
 * API anywhere in this header: that would require synchronizing
 * concurrent dispatch against a handler table that can change out from
 * under it, which is exactly the spinlock-shaped problem this
 * dispatcher is designed to avoid. If a kernel port needs to vary *what*
 * runs for a reason, it does so by swapping `Handlers` at compile time
 * (or branching inside `Handlers::invoke` on boot-time-fixed state),
 * never at IPI-dispatch time.
 *
 * ## Single shared IPI vector, two delivery mechanisms
 *
 * This dispatcher assumes exactly one hardware IPI vector carries
 * everything it sends -- @p Sender's `send_ipi(target_mask, ipi_id)` is
 * free to pick whatever `ipi_id` it likes (or ignore the parameter
 * entirely on hardware with only one), but the receiving side always
 * discovers *what* happened by polling (`poll()`), never by a
 * dedicated-vector-per-reason dispatch. Two delivery mechanisms share
 * that one vector, chosen per call:
 *
 * - **Generation counters** (`notify()`/`poll_reasons()`): for the
 *   common "1 IPI reason = no payload" case. `notify()` bumps a
 *   per-reason, per-target `std::atomic<std::uint64_t>` generation
 *   counter (remote-writable, lock-free) before sending the hardware
 *   IPI; the target later calls `poll_reasons()` to scan every reason's
 *   counter for a change since it last looked, coalescing any number of
 *   `notify()` calls that landed between two polls into a single
 *   `Handlers::invoke()`.
 * - **Message queue** (`push_message()`/`enqueue()`/`poll_messages()`/
 *   `call_sync()`): for IPIs that need to carry an argument or
 *   synchronously run arbitrary code on one or more targets. Each
 *   `ipi_message` is a caller-owned (typically stack-allocated) node
 *   carrying a `reloco::function_ref` plus an atomic completion
 *   counter, so exactly **one** message object is ever needed per call,
 *   regardless of how many CPUs it targets -- there is no "one message
 *   per target" requirement, which would not scale to a broadcast on a
 *   large CPU count. Two different queues carry a pushed message,
 *   chosen automatically by target count:
 *   - **Exactly one remote target**: pushed onto that CPU's own
 *     lock-free, per-CPU Treiber stack (a single CAS loop, no shared
 *     state, no spinlock touched at all).
 *   - **Two or more remote targets**: pushed onto a single queue shared
 *     by every CPU (`push_front` under a short-held `reloco::spin_lock`
 *     guarding a `reloco::c_list`), tagged with a `mask_type` of which
 *     targets still haven't claimed it. Every target CPU cheaply
 *     bails out of `poll_messages()` without ever touching the shared
 *     lock by comparing a monotonic generation counter (bumped once
 *     per multicast push) against the value it last observed; only when
 *     that differs does it take the lock to scan the shared list. A
 *     claimed node is unlinked (O(1), via `reloco::c_list::remove`) the
 *     moment the last pending target claims it. Crucially, the lock is
 *     only ever held for this cheap claim-and-maybe-unlink bookkeeping
 *     -- every claimed callback runs, and every completion-counter
 *     decrement happens, strictly *after* the lock has been released,
 *     so an arbitrarily slow callback on one CPU can never stall any
 *     other CPU's claim scan.
 *
 * `poll()` runs both mechanisms in one call -- the single entry point a
 * target's one IPI vector's handler calls.
 *
 * ## Per-CPU state is kernel-owned; the shared multicast queue is not
 *
 * Every *per-CPU* field `ipi_dispatcher` operates on lives in kernel-
 * owned per-CPU storage -- see `arch/per_cpu_ptr.hpp`'s storage-free
 * design, which @p PerCpu mirrors exactly (`PerCpu::get(cpu)` resolving
 * a `*ipi_percpu_state<mask_type, MaxReasons>` the kernel placed in its
 * own per-CPU memory). The one exception is the shared multicast queue
 * itself (its spinlock, its `c_list`, and its generation counter): since
 * it is inherently a single resource shared by every CPU targetable by
 * one `ipi_dispatcher` instantiation -- not a per-CPU one -- it is held
 * as `static` state directly on the dispatcher class template, keyed
 * purely by the `<PerCpu, Handlers, Sender, MaskT, MaxReasons>`
 * instantiation itself. This is the sole piece of state
 * `ipi_dispatcher` owns; every other method operates purely through
 * `PerCpu::get(cpu)` and `Sender::send_ipi(...)`.
 *
 * ## Single security domain
 *
 * One `ipi_dispatcher` instantiation is scoped to exactly one
 * `hw::irq_security_domain` -- @p Sender's `send_ipi(target_mask,
 * ipi_id)` contract deliberately drops the domain parameter
 * `hw::irqc_ref::send_ipi` itself takes, so binding a domain is the
 * caller's job (a thin adapter closing over a fixed
 * `irq_security_domain` when wrapping `irqc_ref`). Cross-domain IPI use
 * requires a second, separate `ipi_dispatcher` instantiation (and
 * adapter) scoped to the other domain, never one dispatcher juggling
 * both.
 *
 * @code
 * // Sender adapter: binds hw::irqc_ref::send_ipi to a fixed domain.
 * struct my_normal_world_sender {
 *   static reloco::result<void> send_ipi(reloco::span<const std::uint64_t> target_mask,
 *                                         std::uint32_t ipi_id) noexcept {
 *     return g_irqc.send_ipi(target_mask, normal_world_domain, ipi_id);
 *   }
 * };
 *
 * enum my_ipi_reason : std::uint32_t { reschedule = 0, tlb_shootdown = 1, call_function = 2 };
 *
 * struct my_handlers {
 *   static void invoke(std::uint32_t reason, std::size_t executing_cpu) noexcept {
 *     switch (reason) {
 *     case reschedule: handle_reschedule(executing_cpu); break;
 *     case tlb_shootdown: handle_tlb_shootdown(executing_cpu); break;
 *     default: break;
 *     }
 *   }
 * };
 *
 * using core_mask = structo::arch::cpu_mask<structo::arch::physical_cpu_tag, 128>;
 * using my_ipis = structo::arch::ipi_dispatcher<my_ipi_percpu_tag_resolver, my_handlers,
 *                                               my_normal_world_sender, core_mask, 3>;
 *
 * // Sender side, e.g. the scheduler waking a remote CPU:
 * my_ipis::notify(core_mask::single(target_cpu), reschedule);
 *
 * // Synchronous call on a handful of CPUs, e.g. a TLB shootdown -- a
 * // single stack-allocated message, however many CPUs are targeted:
 * my_ipis::message_type msg;
 * my_ipis::call_sync(this_cpu(), targets, msg, tlb_shootdown_ipi_id,
 *                     [](std::size_t cpu) noexcept { flush_tlb_range(cpu, addr, len); });
 *
 * // Receiver side, inside the one shared IPI vector's handler:
 * my_ipis::poll(this_cpu());
 * @endcode
 *
 * ## CPU hotplug
 *
 * `on_cpu_online`/`on_cpu_offline` are exposed, not driven internally --
 * this header never touches `cpu_online_dispatcher.hpp` itself. The
 * kernel wires them directly as the hotplug callbacks it already passes
 * to `cpu_online_dispatcher::mark_online`/`mark_offline` (their
 * `(std::size_t cpu, const mask_type &mask) noexcept` signature matches
 * exactly, so they can be passed as-is, with no adapter lambda needed):
 *
 * @code
 * structo::arch::cpu_online_dispatcher<structo::arch::physical_cpu_tag, 128> online;
 *
 * // During this CPU's own bring-up path, before it is advertised as a
 * // valid IPI target:
 * online.mark_online(this_cpu(), my_ipis::on_cpu_online);
 *
 * // During this CPU's teardown path, once it has stopped being a valid
 * // IPI target:
 * online.mark_offline(this_cpu(), my_ipis::on_cpu_offline);
 * @endcode
 */

#include <reloco/detail/assert.hpp>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/function_ref.hpp>
#include <reloco/intrusive_c_list.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/optional.hpp>
#include <reloco/span.hpp>
#include <reloco/spin_lock.hpp>
#include <reloco/unique_lock.hpp>

#include <structo/sync/backoff.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace structo::arch {

template <typename PerCpu, typename Handlers, typename Sender, typename MaskT, std::size_t MaxReasons>
class ipi_dispatcher;

/**
 * @brief A caller-owned (typically stack-allocated) node binding a
 * single argument-carrying or synchronous-call IPI to one or more
 * target CPUs; see the @file-level docs above. Exactly one `ipi_message`
 * is needed per call, however many CPUs it targets.
 *
 * Non-copyable, non-movable: a pushed `ipi_message` is linked by
 * address into either a target CPU's intrusive Treiber stack or the
 * shared multicast list, so its storage must outlive every use
 * (`wait()`/`is_done()`) a sender makes of it, and every claim a target
 * CPU makes of it.
 * @tparam MaskT A `cpu_mask<Tag, N>` instantiation (see
 * `arch/cpu_mask.hpp`), matching the `ipi_dispatcher` this message is
 * used with.
 */
template <typename MaskT> class ipi_message {
public:
  using mask_type = MaskT;

  constexpr ipi_message() noexcept = default;

  ipi_message(const ipi_message &) = delete;
  ipi_message &operator=(const ipi_message &) = delete;
  ipi_message(ipi_message &&) = delete;
  ipi_message &operator=(ipi_message &&) = delete;

  /** @brief Whether every target has claimed (or discarded, e.g.
   * because it went offline mid-flight) this message. */
  [[nodiscard]] bool is_done() const noexcept { return ref_count_.load(std::memory_order_acquire) == 0; }

  /** @brief Whether every target has claimed (or discarded, e.g.
   * because it went offline mid-flight) this message. */
  [[nodiscard]] bool is_done_relaxed() const noexcept { return ref_count_.load(std::memory_order_relaxed) == 0; }

  /** @brief Spins until `is_done()`, backing off between checks (via
   * `structo::sync::backoff`) so a long wait doesn't keep re-issuing
   * loads against the same cache line on every single iteration -- that
   * line is also what every target CPU's completing
   * `ref_count_.fetch_sub()` needs exclusive ownership of to make
   * progress. */
  void wait() const noexcept {
    while (!is_done()) {
      // Perform relaxed load not to kill cache with atomic loads
      structo::sync::backoff bo;
      while (!is_done_relaxed()) {
        bo.spin();
      }
    }
  }

  /** @brief Rebinds this (already-idle, i.e. `is_done()`) message to run
   * @p fn the next time each of its targets claims it. `call_sync()`
   * calls this internally; callers driving `push_message()`/`enqueue()`
   * directly (fire-and-forget, no blocking `wait()`) must call this
   * themselves first. */
  void bind(reloco::function_ref<void(std::size_t)> fn) noexcept { fn_ = fn; }

private:
  template <typename, typename, typename, typename, std::size_t> friend class ipi_dispatcher;

  // Layout required by `reloco::c_list<ipi_message, &ipi_message::link_>`
  // (`detail::c_list_hook_layout<ipi_message>`): `next`/`prev` are only
  // ever touched while holding the shared multicast lock. In unicast
  // mode, the very same `next` field is recycled as the lock-free
  // Treiber-stack link instead (mutually exclusive uses in time: a
  // message is either pushed to exactly one CPU's unicast stack, or to
  // the shared multicast list, never both at once) -- `prev` is left
  // untouched in unicast mode.
  struct link_type {
    ipi_message *next = nullptr;
    ipi_message **prev = nullptr;
  };

  link_type link_{};

  friend struct reloco::detail::c_list_hook_access<ipi_message, &ipi_message::link_>;

  reloco::optional<reloco::function_ref<void(std::size_t)>> fn_{};

  // Targets that have not yet claimed this message; meaningful only in
  // multicast mode, where it is read/written exclusively while holding
  // the shared multicast lock (so needs no atomics of its own either).
  mask_type pending_{};

  // Number of targets (remote and/or local) still to run `fn_`;
  // decremented, lock-free, by each target strictly after running
  // `fn_`, which is what `wait()`/`is_done()` poll.
  std::atomic<std::uint32_t> ref_count_{0};
};

/**
 * @brief The per-CPU data model `ipi_dispatcher` operates on -- owned
 * and placed into per-CPU storage by the kernel itself (see
 * `arch/per_cpu_ptr.hpp`), never by the dispatcher.
 * @tparam MaskT Must match the `ipi_dispatcher` this state is used with.
 * @tparam MaxReasons Number of generation-counter-tracked IPI reasons,
 * `[0, MaxReasons)`.
 */
template <typename MaskT, std::size_t MaxReasons> struct ipi_percpu_state {
  static_assert(MaxReasons >= 1, "ipi_percpu_state requires at least one reason");

  using message_type = ipi_message<MaskT>;

  /** @brief One reason's remote-writable generation counter alongside
   * this CPU's own, purely-local last-observed value. */
  struct reason_slot {
    std::atomic<std::uint64_t> generation{0};
    std::uint64_t last_seen{0};
  };

  reason_slot reasons[MaxReasons]{};

  /** @brief Head of this CPU's own lock-free unicast Treiber stack. */
  std::atomic<message_type *> unicast_head{nullptr};

  /** @brief Last-observed value of the shared multicast queue's
   * generation counter; purely local, read-modify-write only by this
   * CPU, used to skip the shared lock entirely when nothing changed. */
  std::uint64_t last_seen_multicast_generation{0};
};

// Reason indices are bounds-checked against MaxReasons (assert/early-return) before indexing the per-CPU slot array.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

/**
 * @brief Local IPI orchestrator; see the @file-level docs above for the
 * full design and usage.
 * @tparam PerCpu Resolves `ipi_percpu_state<MaskT, MaxReasons> *` for a
 * given CPU, mirroring `per_cpu_ptr<Tag, T>::get(cpu)`'s exact
 * signature: `static ipi_percpu_state<MaskT, MaxReasons> *get(std::size_t
 * cpu) noexcept;`.
 * @tparam Handlers Resolves the fixed, boot-time-configured handler for
 * a reason: `static void invoke(std::uint32_t reason, std::size_t
 * executing_cpu) noexcept;`.
 * @tparam Sender Issues the actual hardware IPI, pre-bound to exactly
 * one `hw::irq_security_domain`: `static reloco::result<void>
 * send_ipi(reloco::span<const std::uint64_t> target_mask, std::uint32_t
 * ipi_id) noexcept;` -- see `hw::irqc_ref::send_ipi` for the richer,
 * domain-taking signature a thin adapter should wrap.
 * @tparam MaskT A `cpu_mask<Tag, N>` instantiation (see
 * `arch/cpu_mask.hpp`); should match whatever `cpu_online_dispatcher`
 * tracks the same CPU space, so `on_cpu_online`/`on_cpu_offline` can be
 * wired in directly as its hotplug callbacks.
 * @tparam MaxReasons Number of generation-counter-tracked IPI reasons.
 */
template <typename PerCpu, typename Handlers, typename Sender, typename MaskT, std::size_t MaxReasons>
class ipi_dispatcher {
public:
  using mask_type = MaskT;
  using message_type = ipi_message<mask_type>;
  using state_type = ipi_percpu_state<mask_type, MaxReasons>;
  using reason_type = std::uint32_t;

  static_assert(MaxReasons >= 1, "ipi_dispatcher requires at least one reason");

  ipi_dispatcher() = delete;

  // -------------------------------------------------------------------------
  // CPU hotplug: exposed hooks, driven by kernel code (see @file docs)
  // -------------------------------------------------------------------------

  /**
   * @brief Resets `cpu`'s per-CPU IPI state to a clean baseline (every
   * generation counter and its locally-observed value reset to zero,
   * unicast queue emptied, multicast generation caught up to the
   * current value so the first poll after coming online doesn't
   * uselessly re-scan every message that predates it) before it becomes
   * a valid IPI target again. Matches `cpu_online_dispatcher<...>::
   * mark_online`'s callback signature exactly, so it can be wired in
   * directly, with no adapter lambda.
   */
  static void on_cpu_online(std::size_t cpu, const mask_type &mask) noexcept {
    (void)mask;
    state_type *state = PerCpu::get(cpu);
    for (std::size_t reason = 0; reason < MaxReasons; ++reason) {
      state->reasons[reason].generation.store(0, std::memory_order_relaxed);
      state->reasons[reason].last_seen = 0;
    }
    state->unicast_head.store(nullptr, std::memory_order_relaxed);
    state->last_seen_multicast_generation = multicast_generation_.load(std::memory_order_acquire);
  }

  /**
   * @brief Aborts every `ipi_message` still pending for `cpu` -- both
   * its own unicast queue (drained wholesale) and, for any still-queued
   * multicast message, just this CPU's claim on it (never touching
   * other targets' claims) -- without ever running their callback, so
   * no `call_sync()` sender keeps spinning on `wait()` forever for a CPU
   * that just went away. Matches `cpu_online_dispatcher<...>::
   * mark_offline`'s callback signature exactly, so it can be wired in
   * directly, with no adapter lambda.
   */
  static void on_cpu_offline(std::size_t cpu, const mask_type &mask) noexcept {
    (void)mask;
    state_type *state = PerCpu::get(cpu);

    message_type *node = state->unicast_head.exchange(nullptr, std::memory_order_acq_rel);
    while (node != nullptr) {
      message_type *next = node->link_.next;
      abort_claim(*node); // never runs fn_
      node = next;
    }

    reloco::unique_lock<reloco::spin_lock> guard(multicast_lock_);
    for (auto it = multicast_queue_.begin(); it != multicast_queue_.end();) {
      message_type &msg = *it;
      ++it; // advance before a possible removal below
      if (msg.pending_.test(cpu)) {
        msg.pending_.clear(cpu);
        if (msg.pending_.none()) {
          multicast_queue_.remove(msg);
        }
        abort_claim(msg); // never runs fn_
      }
    }
  }

  // -------------------------------------------------------------------------
  // Receiver side
  // -------------------------------------------------------------------------

  /** @brief Scans every reason's generation counter for a change since
   * `executing_cpu` last observed it, invoking `Handlers::invoke` once
   * per reason that advanced -- any number of `notify()` calls between
   * two polls coalesce into one invocation. There is deliberately no
   * dedicated-vector/`dispatch(reason, cpu)` alternative: every IPI this
   * dispatcher sends shares the one hardware vector @p Sender is bound
   * to, so `poll()`/`poll_reasons()` is always how a target discovers
   * *which* reason(s) fired. */
  static void poll_reasons(std::size_t executing_cpu) noexcept {
    state_type *state = PerCpu::get(executing_cpu);
    for (std::size_t reason = 0; reason < MaxReasons; ++reason) {
      auto &slot = state->reasons[reason];
      const std::uint64_t current = slot.generation.load(std::memory_order_acquire);
      if (current != slot.last_seen) {
        slot.last_seen = current;
        Handlers::invoke(static_cast<reason_type>(reason), executing_cpu);
      }
    }
  }

  /** @brief Drains and runs every `ipi_message` queued for
   * `executing_cpu` on its own unicast stack, then claims and runs every
   * still-pending multicast message that targets it (first bailing out,
   * lock-free, if the shared multicast generation hasn't changed since
   * last observed). */
  static void poll_messages(std::size_t executing_cpu) noexcept {
    poll_unicast(executing_cpu);
    poll_multicast(executing_cpu);
  }

  /** @brief Convenience: `poll_reasons()` followed by `poll_messages()`. */
  static void poll(std::size_t executing_cpu) noexcept {
    poll_reasons(executing_cpu);
    poll_messages(executing_cpu);
  }

  // -------------------------------------------------------------------------
  // Sender side
  // -------------------------------------------------------------------------

  /** @brief Bumps `reason`'s generation counter on every CPU in
   * `targets`, then issues one batched hardware IPI covering all of them. */
  static reloco::result<void> notify(const mask_type &targets, reason_type reason) noexcept {
    RELOCO_ASSERT(reason < MaxReasons, "ipi_dispatcher: reason out of range");
    for (std::size_t cpu : targets) {
      PerCpu::get(cpu)->reasons[reason].generation.fetch_add(1, std::memory_order_release);
    }
    return Sender::send_ipi(targets.words(), static_cast<std::uint32_t>(reason));
  }

  /**
   * @brief Software-only push of @p msg to every CPU in @p targets
   * (exactly one `ipi_message`, regardless of how many targets); does
   * **not** itself send a hardware IPI -- use `enqueue()` for
   * push-then-send, or batch several pushes before one shared `send()`.
   * Chooses the lock-free per-CPU unicast stack automatically when
   * @p targets has exactly one member, the shared multicast queue
   * otherwise. @p targets must not be empty.
   */
  static void push_message(const mask_type &targets, message_type &msg) noexcept {
    const std::size_t count = targets.count();
    RELOCO_ASSERT(count >= 1, "ipi_dispatcher: push_message() requires at least one target");
    msg.ref_count_.store(static_cast<std::uint32_t>(count), std::memory_order_relaxed);

    if (count == 1) {
      push_unicast(*targets.begin(), msg);
      return;
    }

    msg.pending_ = targets;
    {
      reloco::unique_lock<reloco::spin_lock> guard(multicast_lock_);
      multicast_queue_.push_front(msg);
    }
    multicast_generation_.fetch_add(1, std::memory_order_release);
  }

  /** @brief `push_message()` to every CPU in @p targets, followed by a
   * single batched hardware IPI covering all of them. */
  static reloco::result<void> enqueue(const mask_type &targets, message_type &msg, std::uint32_t ipi_id) noexcept {
    push_message(targets, msg);
    return send(targets, ipi_id);
  }

  /** @brief Raw batched hardware send, bypassing both generation
   * counters and the message queues -- for callers that already pushed
   * messages/bumped generations themselves and just need the final,
   * single wire send covering every affected target. */
  static reloco::result<void> send(const mask_type &targets, std::uint32_t ipi_id) noexcept {
    return Sender::send_ipi(targets.words(), ipi_id);
  }

  /**
   * @brief Synchronously runs @p fn on every CPU in @p targets, blocking
   * until all of them have run it, using exactly one `ipi_message` (@p
   * msg) regardless of how many CPUs are targeted (if `self_cpu` is
   * itself in @p targets, it runs @p fn directly, inline, without going
   * through any queue at all).
   * @tparam Fn Invocable as `Fn(std::size_t executing_cpu) noexcept`.
   */
  template <typename Fn>
  static reloco::result<void> call_sync(std::size_t self_cpu, const mask_type &targets, message_type &msg,
                                        std::uint32_t ipi_id, Fn &&fn) noexcept {
    static_assert(std::is_nothrow_invocable_v<Fn, std::size_t>, "call_sync's Fn must be noexcept");

    mask_type remote_targets = targets;
    const bool run_locally = targets.test(self_cpu);
    if (run_locally) {
      remote_targets.clear(self_cpu);
    }

    reloco::result<void> sent{};
    if (!remote_targets.none()) {
      msg.bind(fn);
      push_message(remote_targets, msg);
      sent = send(remote_targets, ipi_id);
    }

    if (run_locally) {
      fn(self_cpu);
    }

    if (!sent.has_value()) {
      return sent;
    }
    if (!remote_targets.none()) {
      msg.wait();
    }
    return {};
  }

private:
  /** @brief Lock-free push of @p msg onto `target_cpu`'s own unicast
   * Treiber stack. */
  static void push_unicast(std::size_t target_cpu, message_type &msg) noexcept {
    state_type *state = PerCpu::get(target_cpu);
    message_type *head = state->unicast_head.load(std::memory_order_relaxed);
    do {
      msg.link_.next = head;
    } while (
        !state->unicast_head.compare_exchange_weak(head, &msg, std::memory_order_release, std::memory_order_relaxed));
  }

  /** @brief Drains `executing_cpu`'s own unicast stack (lock-free),
   * running each message's callback then decrementing its completion
   * counter. */
  static void poll_unicast(std::size_t executing_cpu) noexcept {
    state_type *state = PerCpu::get(executing_cpu);
    message_type *node = state->unicast_head.exchange(nullptr, std::memory_order_acquire);
    while (node != nullptr) {
      message_type *next = node->link_.next;
      run_claim(*node, executing_cpu);
      node = next;
    }
  }

  /** @brief Claims and runs every still-pending multicast message that
   * targets `executing_cpu`, bailing out before ever touching the
   * shared lock if the multicast generation counter hasn't moved since
   * last observed. */
  static void poll_multicast(std::size_t executing_cpu) noexcept {
    state_type *state = PerCpu::get(executing_cpu);
    if (multicast_generation_.load(std::memory_order_acquire) == state->last_seen_multicast_generation) {
      return;
    }

    // Claims (and, if we were the last pending target, the matching
    // O(1) unlink) happen one message at a time, each under its own
    // short-held lock acquisition; each claimed message's callback then
    // runs, and its completion counter is decremented, strictly after
    // that lock has been released, so no other CPU's claim scan is ever
    // blocked behind an arbitrarily slow callback. Re-acquiring the
    // lock per claim (rather than batching every claim made in one scan
    // before running any of them) is what lets a single message safely
    // stay a single shared field: once this loop reads `claimed` back
    // out from under the lock, no other CPU can touch *that* message
    // again on this CPU's behalf, since this CPU's own pending bit was
    // already cleared -- nothing else needs the message to itself
    // remain untouched while its callback runs.
    for (;;) {
      message_type *claimed = nullptr;
      {
        reloco::unique_lock<reloco::spin_lock> guard(multicast_lock_);
        state->last_seen_multicast_generation = multicast_generation_.load(std::memory_order_relaxed);
        for (auto &msg : multicast_queue_) {
          if (msg.pending_.test(executing_cpu)) {
            msg.pending_.clear(executing_cpu);
            if (msg.pending_.none()) {
              multicast_queue_.remove(msg);
            }
            claimed = &msg;
            break;
          }
        }
      }
      if (claimed == nullptr) {
        break;
      }
      run_claim(*claimed, executing_cpu);
    }
  }

  /** @brief Runs a claimed message's callback (if any) then signals
   * completion of this CPU's claim. */
  static void run_claim(message_type &msg, std::size_t executing_cpu) noexcept {
    if (msg.fn_.has_value()) {
      msg.fn_.value()(executing_cpu);
    }
    msg.ref_count_.fetch_sub(1, std::memory_order_release);
  }

  /** @brief Signals completion of this CPU's claim without running the
   * callback -- used only to abort stray messages on hotplug teardown. */
  static void abort_claim(message_type &msg) noexcept { msg.ref_count_.fetch_sub(1, std::memory_order_release); }

  static inline reloco::spin_lock multicast_lock_{};
  static inline reloco::c_list<message_type, &message_type::link_> multicast_queue_{};
  static inline std::atomic<std::uint64_t> multicast_generation_{0};
};

RELOCO_END_UNSAFE_BUFFER_USAGE

} // namespace structo::arch
