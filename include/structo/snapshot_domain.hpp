// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file snapshot_domain.hpp
 * @brief `structo::snapshot_domain<T, Traits>`: lock-free reads of the current value of `T`, with updates that
 * return only when no reader can still observe the previous value (a grace period).
 *
 * ## Model
 *
 * The domain keeps two slots holding a `T` each; one is *current*. A reader pins the current slot for the
 * lifetime of a `read_guard` (an increment of a per-shard counter - no lock, no allocation; the guard may be
 * held across preemption or sleep) and gets a `const T &`. Anything reachable from that reference (for
 * example page descriptors found through a `memory_segment_map`) stays valid until the guard is destroyed.
 *
 * An update, serialized with other updates by `Traits::mutex_type`, builds the next value into the *other*
 * slot, publishes it with one atomic store, and then blocks until every reader that pinned the old slot has
 * left. When `update()` returns, the old value is unreachable by any thread and whatever it referred to can be
 * released. `update_async()` skips the wait; call `synchronize()` (or let the next update do it) before
 * releasing anything the old value pointed to.
 *
 * Readers must not call `update()`/`synchronize()` while holding a guard (the writer would wait for itself).
 *
 * ## Traits
 *
 * @code
 * struct my_traits {
 *   // Reader counter shards (power of two); spreads cache-line contention, any value works.
 *   static constexpr std::size_t shards = 8;
 *   // Which shard the calling thread uses, typically the current CPU id.
 *   static std::size_t current_shard() noexcept { return this_cpu_id(); }
 *   // Serializes writers; may sleep.
 *   using mutex_type = my_sleep_mutex; // lock() / unlock()
 *   // Futex-style blocking used only by the writer waiting for readers to drain: block while word == seen;
 *   // returning spuriously is fine because the domain re-checks.
 *   static void wait(std::atomic<std::uint32_t> &word, std::uint32_t seen) noexcept { park_on(word, seen); }
 *   static void wake_all(std::atomic<std::uint32_t> &word) noexcept { unpark_all(word); }
 * };
 *
 * structo::snapshot_domain<config, my_traits> cfg;     // T must be default constructible and movable
 * { auto g = cfg.read(); use(g->value); }              // reader: valid while 'g' lives
 * (void)cfg.update([](const config &cur) -> reloco::result<config> { config n = cur; n.value++; return n; });
 * @endcode
 */

#include <reloco/array.hpp>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace structo {

template <typename T, typename Traits> class snapshot_domain {
  static constexpr std::size_t shards = Traits::shards;
  static_assert(shards > 0 && (shards & (shards - 1)) == 0, "Traits::shards must be a power of two");

public:
  /** Pins the snapshot that was current at `read()`; releases it on destruction. */
  class [[nodiscard]] read_guard {
  public:
    read_guard(const read_guard &) = delete;
    read_guard &operator=(const read_guard &) = delete;
    read_guard &operator=(read_guard &&) = delete;
    read_guard(read_guard &&o) noexcept : dom_(o.dom_), slot_(o.slot_), shard_(o.shard_), live_(o.live_) {
      o.live_ = false;
    }
    ~read_guard() {
      if (live_) {
        dom_.leave(slot_, shard_);
      }
    }

    [[nodiscard]] const T &get() const noexcept { return dom_.slots_[slot_]; }
    [[nodiscard]] const T &operator*() const noexcept { return get(); }
    [[nodiscard]] const T *operator->() const noexcept { return &dom_.slots_[slot_]; }

  private:
    friend class snapshot_domain;
    read_guard(snapshot_domain &d, std::size_t slot, std::size_t shard) noexcept
        : dom_(d), slot_(slot), shard_(shard) {}
    snapshot_domain &dom_;
    std::size_t slot_;
    std::size_t shard_;
    bool live_{true};
  };

  snapshot_domain() = default;
  explicit snapshot_domain(T initial) noexcept(std::is_nothrow_move_assignable_v<T>) {
    slots_[0] = std::move(initial);
  }
  snapshot_domain(const snapshot_domain &) = delete;
  snapshot_domain &operator=(const snapshot_domain &) = delete;

  /** Pins and returns the current value. Wait-free apart from the (rare) retry racing a publish. */
  [[nodiscard]] read_guard read() noexcept {
    const std::size_t shard = Traits::current_shard() & (shards - 1);
    for (;;) {
      const std::size_t s = active_.load(std::memory_order_acquire);
      counters_[s][shard].count.fetch_add(1, std::memory_order_seq_cst);
      if (active_.load(std::memory_order_seq_cst) == s) {
        return read_guard(*this, s, shard);
      }
      leave(s, shard); // a publish raced us; the slot we pinned may be rewritten, pin the new one
    }
  }

  /**
   * Builds the next value with `build(const T &current) -> reloco::result<T>`, publishes it, and waits until
   * no reader can see the previous one. On a build error nothing changes.
   */
  template <typename F> [[nodiscard]] reloco::result<void> update(F &&build) {
    return do_update(std::forward<F>(build), true);
  }

  /**
   * Like `update`, but returns right after publishing. The previous value may still be read; call
   * `synchronize()` before releasing what it referenced. A following update waits first if needed.
   */
  template <typename F> [[nodiscard]] reloco::result<void> update_async(F &&build) {
    return do_update(std::forward<F>(build), false);
  }

  /** Blocks until no reader holds a value older than the current one. */
  void synchronize() {
    writer_lock lock(mutex_);
    synchronize_locked();
  }

  /** Writer-side view of the current value; only meaningful while updates are excluded by the caller. */
  [[nodiscard]] const T &current_for_writer() const noexcept {
    return slots_[active_.load(std::memory_order_acquire)];
  }

private:
  struct alignas(64) shard_counter {
    std::atomic<std::uint32_t> count{0};
  };

  struct writer_lock {
    explicit writer_lock(typename Traits::mutex_type &m) : m_(m) { m_.lock(); }
    ~writer_lock() { m_.unlock(); }
    writer_lock(const writer_lock &) = delete;
    writer_lock &operator=(const writer_lock &) = delete;
    typename Traits::mutex_type &m_;
  };

  void leave(std::size_t slot, std::size_t shard) noexcept {
    counters_[slot][shard].count.fetch_sub(1, std::memory_order_seq_cst);
    if (waiters_.load(std::memory_order_seq_cst) != 0) {
      drain_seq_.fetch_add(1, std::memory_order_seq_cst);
      Traits::wake_all(drain_seq_);
    }
  }

  [[nodiscard]] bool drained(std::size_t slot) const noexcept {
    for (std::size_t i = 0; i < shards; ++i) {
      if (counters_[slot][i].count.load(std::memory_order_seq_cst) != 0) {
        return false;
      }
    }
    return true;
  }

  template <typename F> reloco::result<void> do_update(F &&build, bool wait) {
    writer_lock lock(mutex_);
    synchronize_locked(); // the slot we are about to overwrite must have no readers
    const std::size_t cur = active_.load(std::memory_order_relaxed);
    const std::size_t next = cur ^ 1;
    auto built = build(static_cast<const T &>(slots_[cur]));
    if (!built) {
      return reloco::unexpected(built.error());
    }
    slots_[next] = std::move(*built);
    drain_slot_ = cur;
    drain_pending_ = true;
    active_.store(next, std::memory_order_seq_cst);
    if (wait) {
      synchronize_locked();
    }
    return {};
  }

  void synchronize_locked() {
    if (!drain_pending_) {
      return;
    }
    waiters_.fetch_add(1, std::memory_order_seq_cst);
    for (;;) {
      const std::uint32_t seen = drain_seq_.load(std::memory_order_seq_cst);
      if (drained(drain_slot_)) {
        break;
      }
      Traits::wait(drain_seq_, seen);
    }
    waiters_.fetch_sub(1, std::memory_order_seq_cst);
    drain_pending_ = false;
  }

  reloco::array<T, 2> slots_{};
  reloco::array<reloco::array<shard_counter, shards>, 2> counters_{};
  std::atomic<std::size_t> active_{0};
  std::atomic<std::uint32_t> waiters_{0};
  std::atomic<std::uint32_t> drain_seq_{0};
  typename Traits::mutex_type mutex_{};
  // Writer-side state, guarded by mutex_.
  std::size_t drain_slot_{0};
  bool drain_pending_{false};
};

} // namespace structo
