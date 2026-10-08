// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file scheduler.hpp
 * @brief `structo::bootldr::scheduler`: a small cooperative, single-threaded
 * coroutine scheduler for boot loaders. C++20 only (empty otherwise).
 *
 * It runs `reloco::task<void>` coroutines round-robin and gives them three
 * ways to wait: `yield()` (let the others run), `sleep_for()/sleep_until()`
 * (against a caller-supplied millisecond clock) and `event` (a manual-reset
 * flag many tasks can wait on), plus `join()` for a spawned task. Hardware
 * that has to be polled (the network stack's `polled_net_device::poll()`,
 * a UART, a timer ...) is registered as a *poller* and called once per round;
 * a coroutine parked on that hardware is resumed from inside the poller, so
 * everything composes with the rest of the stack unchanged.
 *
 * ## Memory
 *
 * The scheduler takes a `reloco::allocator_ref` (default: the process-wide
 * default allocator). It is used for the task table; waiting itself never
 * allocates (wait entries are intrusive and live in the suspended coroutine
 * frames). Coroutine *frames* are allocated by `reloco::task` from whatever
 * allocator you pass to the coroutine, so use `sched.allocator()` to keep
 * everything in one pool. Allocation failure is reported as
 * `error::allocation_failed` (from `spawn`) or, for a frame that could not
 * be allocated, as that task's failure; nothing throws or traps.
 *
 * ## Errors
 *
 * A task that ends with an error is reported through the optional fault
 * handler (detached tasks) or `join()` (joinable tasks).
 *
 * @code
 * std::uint64_t now_ms(void *) noexcept;            // your monotonic millisecond clock
 * void poll_net(void *pnd) noexcept {               // one polling step of some device
 *   static_cast<structo::hw::polled_net_device<my_backend> *>(pnd)->poll();
 * }
 *
 * structo::bootldr::scheduler sched{reloco::default_allocator()}; // task table comes from this allocator
 * sched.set_clock(now_ms, nullptr);                  // required for sleep_for()/sleep_until()
 * sched.add_poller(poll_net, &pnd);                  // called once per round, before tasks run
 *
 * // Coroutine frames are allocated from the scheduler's allocator too.
 * reloco::task<void> blink(reloco::allocator_arg_t, reloco::allocator_ref, structo::bootldr::scheduler &s) {
 *   for (;;) {
 *     toggle_led();
 *     // Inner co_await sleeps; the outer one unwraps the result<void> (error if no clock is set).
 *     co_await co_await s.sleep_for(500);
 *   }
 * }
 *
 * reloco::task<void> load_image(reloco::allocator_arg_t, reloco::allocator_ref, structo::bootldr::scheduler &s) {
 *   co_await download_over_tftp();                 // any coroutine work; others run while it waits
 *   co_await s.yield();                            // let the other tasks run now
 * }
 *
 * auto id = sched.spawn(blink(reloco::allocator_arg, sched.allocator(), sched)); // detached: fire and forget
 * auto main_job = sched.spawn(load_image(reloco::allocator_arg, sched.allocator(), sched),
 *                             structo::bootldr::spawn_mode::joinable); // keep the result for join()
 * sched.run();                                      // returns when every task has finished
 * @endcode
 *
 * Only call scheduler members from tasks it runs or from the thread that
 * calls `run()`/`run_once()`; there is no locking.
 */

#include <reloco/intrusive_c_tailq.hpp>
#include <reloco/coroutine.hpp>

#if RELOCO_HAS_COROUTINES

#include <reloco/allocator.hpp>
#include <reloco/default_allocator.hpp>
#include <reloco/error.hpp>
#include <reloco/vector.hpp>

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace structo::bootldr {

/** @brief Identifies a spawned task; 0 is never a valid id. */
using task_id = std::uint32_t;

/** @brief What happens to a task's outcome when it finishes. */
enum class spawn_mode : std::uint8_t {
  detached, ///< fire and forget: the slot is freed; a failure goes to the fault handler
  joinable  ///< the outcome is kept until `join()` collects it
};

namespace detail {

class wait_list;

// Intrusive wait entry; it lives inside the awaiter, i.e. inside the suspended coroutine frame,
// so it unlinks itself when that frame is destroyed.
struct wait_node {
  // Same layout as FreeBSD TAILQ_ENTRY, as expected by reloco::c_tailq.
  struct {
    wait_node *next = nullptr;
    wait_node **prev = nullptr;
  } link;
  wait_list *owner = nullptr; // null <=> not linked
  std::coroutine_handle<> h;
  std::uint64_t key = 0; // deadline (timers) or task id (joiners)

  wait_node() noexcept = default;
  wait_node(const wait_node &) = delete;
  wait_node &operator=(const wait_node &) = delete;
  ~wait_node() { unlink(); }

  [[nodiscard]] bool linked() const noexcept { return owner != nullptr; }
  inline void unlink() noexcept;
};

class wait_list {
public:
  wait_list() noexcept = default;
  wait_list(const wait_list &) = delete;
  wait_list &operator=(const wait_list &) = delete;
  // Entries that outlive the list are detached rather than left dangling.
  ~wait_list() {
    while (wait_node *n = q_.pop_front())
      n->owner = nullptr;
  }

  [[nodiscard]] bool empty() const noexcept { return q_.empty(); }
  [[nodiscard]] wait_node *first() noexcept { return q_.front(); }
  [[nodiscard]] wait_node *after(wait_node *n) noexcept { return n->link.next; }
  void push_back(wait_node *n) noexcept {
    q_.push_back(*n);
    n->owner = this;
  }
  void remove(wait_node *n) noexcept {
    q_.remove(*n);
    n->owner = nullptr;
  }
  void splice_to(wait_list &dst) noexcept {
    while (wait_node *n = first()) {
      remove(n);
      dst.push_back(n);
    }
  }

private:
  reloco::c_tailq<wait_node, &wait_node::link> q_;
};

inline void wait_node::unlink() noexcept {
  if (owner)
    owner->remove(this);
}

} // namespace detail

class scheduler {
public:
  using clock_fn = std::uint64_t (*)(void *) noexcept;
  using hook_fn = void (*)(void *) noexcept;
  using fault_fn = void (*)(void *, task_id, reloco::error) noexcept;

  explicit scheduler(reloco::allocator_ref alloc = reloco::default_allocator()) noexcept
      : alloc_(alloc), pollers_(alloc), slots_(alloc) {}
  scheduler(const scheduler &) = delete;
  scheduler &operator=(const scheduler &) = delete;
  // Destroy the task frames first: their awaiters unlink from the lists below.
  ~scheduler() { slots_.clear(); }

  /** @brief The allocator to pass to coroutines (`allocator_arg, sched.allocator()`) so frames share the scheduler's pool. */
  [[nodiscard]] reloco::allocator_ref allocator() const noexcept { return alloc_; }

  /** @brief Monotonic millisecond clock; required for `sleep_for()`/`sleep_until()`. */
  void set_clock(clock_fn fn, void *ctx) noexcept {
    clock_ = fn;
    clock_ctx_ = ctx;
  }

  /** @brief Current time from the configured clock, or 0 if none is set. */
  [[nodiscard]] std::uint64_t now_ms() const noexcept { return clock_ ? clock_(clock_ctx_) : 0; }

  /** @brief Registers a function called once at the start of every round (poll a device, kick a watchdog ...). */
  [[nodiscard]] reloco::result<void> add_poller(hook_fn fn, void *ctx) noexcept {
    return pollers_.try_push_back(poller{fn, ctx});
  }

  /** @brief Called by `run()` when a round left nothing ready (e.g. `wfi`, a spin hint). */
  void set_idle(hook_fn fn, void *ctx) noexcept {
    idle_ = fn;
    idle_ctx_ = ctx;
  }

  /** @brief Called when a detached task finishes with an error. */
  void set_fault_handler(fault_fn fn, void *ctx) noexcept {
    fault_ = fn;
    fault_ctx_ = ctx;
  }

  /** @brief Number of tasks that have not finished yet. */
  [[nodiscard]] std::size_t live() const noexcept { return live_; }

  /**
   * @brief Takes ownership of `t` and runs it from the next round.
   * Returns its id, or `error::allocation_failed` if the task table could not grow
   * (`t` is then destroyed).
   */
  [[nodiscard]] reloco::result<task_id> spawn(reloco::task<void> t, spawn_mode mode = spawn_mode::detached) noexcept {
    std::size_t idx = slots_.size();
    for (std::size_t i = 0; i < slots_.size(); ++i)
      if (!slots_[i].used) {
        idx = i;
        break;
      }
    if (idx == slots_.size())
      if (auto r = slots_.try_push_back(slot{}); !r)
        return reloco::unexpected(r.error());
    slot &s = slots_[idx];
    s.t = std::move(t);
    s.id = next_id_++;
    if (next_id_ == 0)
      next_id_ = 1;
    s.used = true;
    s.started = false;
    s.finished = false;
    s.joinable = mode == spawn_mode::joinable;
    s.status = {};
    ++live_;
    return s.id;
  }

  /**
   * @brief Destroys an unfinished task (its locals are destroyed, it never resumes). A joinable task then
   * completes with `error::operation_canceled`; `error::invalid_argument` if `id` is unknown or already finished.
   */
  [[nodiscard]] reloco::result<void> cancel(task_id id) noexcept {
    const std::size_t i = find(id);
    if (i == npos || slots_[i].finished)
      return reloco::unexpected(reloco::error::invalid_argument);
    reloco::task<void> dead = std::move(slots_[i].t);
    dead = reloco::task<void>{}; // destroys the frame (and unlinks its wait entries)
    finish(i, reloco::unexpected(reloco::error::operation_canceled));
    return {};
  }

  /** @brief One scheduling round; returns how many tasks are still unfinished. */
  std::size_t run_once() noexcept {
    for (std::size_t i = 0; i < pollers_.size(); ++i) {
      const poller p = pollers_[i];
      p.fn(p.ctx);
    }
    wake_due_timers();
    for (std::size_t i = 0; i < slots_.size(); ++i)
      if (slots_[i].used && !slots_[i].started && !slots_[i].finished) {
        slots_[i].started = true;
        slots_[i].t.resume(); // runs to its first suspension point
      }
    detail::wait_list batch; // tasks that yield again land in ready_ and run next round
    ready_.splice_to(batch);
    while (detail::wait_node *n = batch.first()) {
      n->unlink();
      n->h.resume(); // `n` may be gone after this
    }
    reap();
    return live_;
  }

  /** @brief Runs rounds until every task has finished; calls the idle hook when nothing was ready. */
  void run() noexcept {
    while (run_once() != 0)
      if (ready_.empty() && idle_)
        idle_(idle_ctx_);
  }

  /** @brief Awaitable: lets every other ready task run once before this one continues. */
  [[nodiscard]] auto yield() noexcept { return yield_awaiter{*this}; }

  /** @brief Awaitable: continue after `ms` milliseconds; result is `error::invalid_state` without a clock. */
  [[nodiscard]] auto sleep_for(std::uint64_t ms) noexcept { return sleep_awaiter{*this, clock_ ? now() + ms : 0}; }

  /** @brief Awaitable: continue once the clock reaches `deadline_ms`. */
  [[nodiscard]] auto sleep_until(std::uint64_t deadline_ms) noexcept { return sleep_awaiter{*this, deadline_ms}; }

  /**
   * @brief Awaitable: waits for a joinable task and yields its outcome (then forgets it, so join each task once).
   * `error::invalid_argument` for an unknown, detached or already joined id.
   */
  [[nodiscard]] auto join(task_id id) noexcept { return join_awaiter{*this, id}; }

  /** @brief A manual-reset flag tasks can wait on. Must outlive its waiters. */
  class event {
  public:
    explicit event(scheduler &s) noexcept : s_(&s) {}
    event(const event &) = delete;
    event &operator=(const event &) = delete;

    [[nodiscard]] bool is_set() const noexcept { return set_; }
    /** @brief Wakes every waiter (they run next round) and makes later waits complete immediately. */
    void set() noexcept {
      set_ = true;
      waiters_.splice_to(s_->ready_);
    }
    void reset() noexcept { set_ = false; }
    /** @brief Awaitable: completes when the event is set (immediately if it already is). */
    [[nodiscard]] auto wait() noexcept { return wait_awaiter{*this}; }
    /**
     * @brief Awaitable: like `wait()` but gives up after `ms` milliseconds. Yields `result<void>`:
     * success if the event is set, `error::timed_out` otherwise, `error::invalid_state` without a clock
     * (unless the event is already set).
     */
    [[nodiscard]] auto wait_for(std::uint64_t ms) noexcept { return timed_wait_awaiter{*this, ms}; }

  private:
    struct timed_wait_awaiter {
      event &e;
      std::uint64_t ms;
      detail::wait_node wait_node_; // in the event's waiters
      detail::wait_node timer_node_; // in the scheduler's timers; whichever fires first resumes us
      timed_wait_awaiter(event &ev, std::uint64_t t) noexcept : e(ev), ms(t) {}
      [[nodiscard]] bool await_ready() const noexcept { return e.set_ || e.s_->clock_ == nullptr; }
      void await_suspend(std::coroutine_handle<> h) noexcept {
        const std::uint64_t deadline = e.s_->now() + ms;
        wait_node_.h = h;
        timer_node_.h = h;
        timer_node_.key = deadline;
        e.waiters_.push_back(&wait_node_);
        (ms == 0 ? e.s_->ready_ : e.s_->timers_).push_back(&timer_node_);
      }
      reloco::result<void> await_resume() noexcept {
        wait_node_.unlink(); // the one that did not fire must not resume us again
        timer_node_.unlink();
        if (e.set_)
          return {};
        if (!e.s_->clock_)
          return reloco::unexpected(reloco::error::invalid_state);
        return reloco::unexpected(reloco::error::timed_out);
      }
    };

    struct wait_awaiter {
      event &e;
      detail::wait_node node;
      explicit wait_awaiter(event &ev) noexcept : e(ev) {}
      [[nodiscard]] bool await_ready() const noexcept { return e.set_; }
      void await_suspend(std::coroutine_handle<> h) noexcept {
        node.h = h;
        e.waiters_.push_back(&node);
      }
      void await_resume() const noexcept {}
    };

    scheduler *s_;
    bool set_ = false;
    detail::wait_list waiters_;
  };

private:
  static constexpr std::size_t npos = static_cast<std::size_t>(-1);

  struct poller {
    hook_fn fn = nullptr;
    void *ctx = nullptr;
  };

  struct slot {
    reloco::task<void> t;
    task_id id = 0;
    bool used = false;
    bool started = false;
    bool finished = false;
    bool joinable = false;
    reloco::result<void> status;
  };

  struct yield_awaiter {
    scheduler &s;
    detail::wait_node node;
    explicit yield_awaiter(scheduler &sc) noexcept : s(sc) {}
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) noexcept {
      node.h = h;
      s.ready_.push_back(&node);
    }
    void await_resume() const noexcept {}
  };

  struct sleep_awaiter {
    scheduler &s;
    std::uint64_t deadline;
    detail::wait_node node;
    sleep_awaiter(scheduler &sc, std::uint64_t d) noexcept : s(sc), deadline(d) {}
    [[nodiscard]] bool await_ready() const noexcept { return s.clock_ == nullptr; }
    void await_suspend(std::coroutine_handle<> h) noexcept {
      node.h = h;
      node.key = deadline;
      (deadline <= s.now() ? s.ready_ : s.timers_).push_back(&node);
    }
    reloco::result<void> await_resume() const noexcept {
      if (!s.clock_)
        return reloco::unexpected(reloco::error::invalid_state);
      return {};
    }
  };

  struct join_awaiter {
    scheduler &s;
    task_id id;
    detail::wait_node node;
    join_awaiter(scheduler &sc, task_id i) noexcept : s(sc), id(i) {}
    [[nodiscard]] bool await_ready() const noexcept {
      const std::size_t i = s.find(id);
      return i == npos || !s.slots_[i].joinable || s.slots_[i].finished;
    }
    void await_suspend(std::coroutine_handle<> h) noexcept {
      node.h = h;
      node.key = id;
      s.joiners_.push_back(&node);
    }
    reloco::result<void> await_resume() noexcept {
      const std::size_t i = s.find(id);
      if (i == npos || !s.slots_[i].joinable || !s.slots_[i].finished)
        return reloco::unexpected(reloco::error::invalid_argument);
      reloco::result<void> st = std::move(s.slots_[i].status);
      s.slots_[i] = slot{};
      return st;
    }
  };

  [[nodiscard]] std::uint64_t now() const noexcept { return clock_(clock_ctx_); }

  [[nodiscard]] std::size_t find(task_id id) const noexcept {
    if (id == 0)
      return npos;
    for (std::size_t i = 0; i < slots_.size(); ++i)
      if (slots_[i].used && slots_[i].id == id)
        return i;
    return npos;
  }

  void wake_due_timers() noexcept {
    if (!clock_ || timers_.empty())
      return;
    const std::uint64_t t = now();
    for (detail::wait_node *n = timers_.first(); n;) {
      detail::wait_node *next = timers_.after(n);
      if (n->key <= t) {
        n->unlink();
        ready_.push_back(n);
      }
      n = next;
    }
  }

  void wake_joiners(task_id id) noexcept {
    for (detail::wait_node *n = joiners_.first(); n;) {
      detail::wait_node *next = joiners_.after(n);
      if (n->key == id) {
        n->unlink();
        ready_.push_back(n);
      }
      n = next;
    }
  }

  // Records the outcome of slot `i`: detached tasks are freed (failures reported), joinable ones wait for join().
  void finish(std::size_t i, reloco::result<void> st) noexcept {
    const task_id id = slots_[i].id;
    --live_;
    if (slots_[i].joinable) {
      slots_[i].finished = true;
      slots_[i].status = std::move(st);
      wake_joiners(id);
      return;
    }
    slots_[i] = slot{};
    if (!st && fault_)
      fault_(fault_ctx_, id, st.error());
  }

  void reap() noexcept {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      if (!slots_[i].used || !slots_[i].started || slots_[i].finished || !slots_[i].t.done())
        continue;
      reloco::result<void> st = slots_[i].t.take();
      reloco::task<void> dead = std::move(slots_[i].t); // frees the frame
      (void)dead;
      finish(i, std::move(st));
    }
  }

  reloco::allocator_ref alloc_;
  clock_fn clock_ = nullptr;
  void *clock_ctx_ = nullptr;
  hook_fn idle_ = nullptr;
  void *idle_ctx_ = nullptr;
  fault_fn fault_ = nullptr;
  void *fault_ctx_ = nullptr;
  task_id next_id_ = 1;
  std::size_t live_ = 0;
  detail::wait_list ready_;
  detail::wait_list timers_;
  detail::wait_list joiners_;
  reloco::vector<poller> pollers_;
  reloco::vector<slot> slots_; // destroyed first (see ~scheduler): its frames' wait entries unlink from the lists above
};

} // namespace structo::bootldr

#endif // RELOCO_HAS_COROUTINES
