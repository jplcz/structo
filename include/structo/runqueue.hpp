// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file runqueue.hpp
 * @brief Three pluggable, allocation-free intrusive runqueue policies
 * sharing one common `enqueue`/`dequeue`/`peek`/`remove` surface:
 * `structo::fifo_runqueue`, `structo::priority_list_runqueue`, and
 * `structo::priority_bucket_runqueue`.
 *
 * A scheduler's runqueue -- "which runnable thing should run next" -- is
 * one of the few data structures real kernels implement three or four
 * different ways depending on what they're optimizing for, and routinely
 * swap out as a scheduler matures (Linux's O(n) `O(1) scheduler`'s
 * per-priority array replaced an earlier simple sorted list, and was
 * itself later replaced by CFS's red-black tree). This header picks the
 * three classic, allocation-free building blocks every such design is
 * built from and gives them one identical API, so a caller (or a future
 * `callout_subsystem`-style scheduler) can swap the policy with a single
 * type alias change and no call-site changes:
 *
 * - **`fifo_runqueue<Entry, Hook>`**: plain FIFO order, ignores
 *   priority entirely -- `enqueue`/`dequeue`/`peek`/`remove` are all
 *   O(1). The simplest possible runqueue; a round-robin scheduler with a
 *   single priority class needs nothing more.
 * - **`priority_list_runqueue<Entry, Hook, Priority>`**: a single
 *   intrusive list kept sorted ascending by `Entry.*Priority` (lower
 *   value runs first, matching `nice(1)`/most kernel priority
 *   conventions) -- `dequeue`/`peek`/`remove` are O(1), but `enqueue` is
 *   O(n) (an insertion-sort scan to find where the new entry belongs).
 *   Good for a small number of simultaneously-runnable entries where an
 *   O(n) insert is cheaper than the bookkeeping `priority_bucket_runqueue`
 *   needs.
 * - **`priority_bucket_runqueue<Entry, Hook, Priority, NumPriorities>`**:
 *   Linux's old O(1) scheduler's design -- one intrusive list *per*
 *   priority level (`NumPriorities` of them) plus a fixed-size bitmap
 *   with one bit per level tracking which buckets are non-empty, so the
 *   highest-priority non-empty bucket is found with a single
 *   `__builtin_ctzll` (count-trailing-zeros) scan instead of walking a
 *   sorted list -- `enqueue`/`dequeue`/`peek`/`remove` are all O(1),
 *   independent of how many entries are queued, at the cost of a fixed
 *   `NumPriorities`-sized bucket array (and insertion order within a
 *   single priority level is FIFO, same as Linux's).
 *
 * ## Intrusive, not owning
 *
 * Exactly like `reloco::c_tailq` (which all three policies use
 * internally to link `Entry` nodes with zero allocation), none of these
 * types own the entries they queue -- `Entry` is caller-owned storage
 * (a stack variable, a field of a task-control-block, ...) that outlives
 * every runqueue operation referencing it. An `Entry` must embed its own
 * intrusive link field and declare it to the policy via the same
 * pointer-to-member `Hook` non-type template parameter `c_tailq` itself
 * uses:
 *
 * @code
 * struct my_task {
 *   struct {
 *     my_task *next = nullptr;
 *     my_task **prev = nullptr;
 *   } link;
 *   unsigned priority = 0; // lower runs first
 *   // ... whatever else a task needs ...
 * };
 *
 * using my_runqueue = structo::priority_bucket_runqueue<my_task, &my_task::link,
 *                                                        &my_task::priority, 32>;
 * @endcode
 *
 * `priority_list_runqueue`/`priority_bucket_runqueue`'s `Priority`
 * non-type template parameter is a pointer-to-member at an unsigned
 * integral field on `Entry` (read, never written, by either policy --
 * the caller is responsible for setting it before `enqueue`, and must
 * not change it while the entry is queued without `remove`ing it
 * first, same precondition `c_tailq::remove` itself already has for
 * any in-place mutation of linked state).
 *
 * ## Checking `remove`'s precondition: `is_linked`
 *
 * `remove(entry)`'s precondition -- @p entry is currently enqueued in
 * *this* runqueue -- is exactly the same one `reloco::c_tailq::remove`
 * itself has, and is just as easy to violate by accident (e.g. a
 * callback that both fires and is independently canceled racing each
 * other, or simply forgetting whether a given entry was ever
 * `enqueue`d in the first place). Every policy's `is_linked(entry)`
 * static method answers that precondition directly and in O(1),
 * without needing external bookkeeping: it reads @p entry's own hook
 * state (specifically, whether its `prev` link is non-null, the same
 * signal `c_tailq` itself relies on internally -- `prev` is always
 * non-null while linked, at any position, and is reset to `nullptr` by
 * `remove`/`pop_front` and starts `nullptr` on a freshly-constructed,
 * never-enqueued `Entry`). A guarded cancel path looks like:
 *
 * @code
 * if (my_runqueue::is_linked(entry))
 *   rq.remove(entry);
 * @endcode
 *
 * ## Why not a single `runqueue<Entry, Policy>` facade
 *
 * The three policies share one *method* surface, but deliberately stay
 * three distinct class templates rather than one `runqueue<Entry,
 * Policy>` facade dispatching through a `Policy` trait -- there is no
 * shared state representation to factor out (a plain `c_tailq`, a
 * sorted `c_tailq`, and an array of `c_tailq` plus a bitmap are simply
 * different objects), so a facade would only add a layer of indirection
 * with nothing left to share. Pick the policy type directly; swapping
 * it later is still a one-line `using` change since the method surface
 * is identical.
 */

#include <cstddef>
#include <cstdint>
#include <reloco/array.hpp>
#include <reloco/detail/assert.hpp>
#include <reloco/intrusive_c_tailq.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/optional.hpp>

#if !defined(__GNUC__) && !defined(__clang__)
#error "priority_bucket_runqueue's bitmap scan requires GCC or Clang (__builtin_ctzll)"
#endif

namespace structo {

using namespace reloco;

/**
 * @brief Plain FIFO runqueue: `enqueue`/`dequeue`/`peek`/`remove` are
 * all O(1); priority is not a concept this policy understands.
 * @tparam Entry Caller-owned node type; must embed an intrusive link
 * field matching `reloco::c_tailq`'s hook layout, named by @p Hook.
 * @tparam Hook Pointer-to-member of @p Entry's link field (e.g.
 * `&Entry::link`), exactly as `reloco::c_tailq<Entry, Hook>` expects.
 */
template <typename Entry, auto Hook> class fifo_runqueue {
public:
  constexpr fifo_runqueue() noexcept = default;

  fifo_runqueue(const fifo_runqueue &) = delete;
  fifo_runqueue &operator=(const fifo_runqueue &) = delete;
  fifo_runqueue(fifo_runqueue &&) noexcept = default;
  fifo_runqueue &operator=(fifo_runqueue &&) noexcept = default;

  /** @brief Enqueues @p entry at the tail. O(1). */
  void enqueue(Entry &entry) & noexcept {
    queue_.push_back(entry);
    ++size_;
  }

  /** @brief Removes and returns the head entry, or `nullptr` if empty. O(1). */
  Entry *dequeue() & noexcept {
    Entry *entry = queue_.pop_front();
    if (entry != nullptr)
      --size_;
    return entry;
  }

  /** @brief Returns the head entry without removing it, or `nullptr` if empty. O(1). */
  [[nodiscard]] Entry *peek() const & noexcept { return queue_.front(); }

  /**
   * @brief Removes @p entry from wherever it currently sits in the
   * queue. O(1). Precondition: @p entry is currently enqueued in *this*
   * runqueue (same precondition `reloco::c_tailq::remove` has; see
   * `is_linked` to check it first).
   */
  void remove(Entry &entry) & noexcept {
    queue_.remove(entry);
    --size_;
  }

  /**
   * @brief Returns whether @p entry is currently linked into *some*
   * `fifo_runqueue<Entry, Hook>` (not necessarily *this* instance) --
   * i.e. whether `remove(entry)` is currently safe to call. O(1).
   */
  [[nodiscard]] static bool is_linked(const Entry &entry) noexcept { return (entry.*Hook).prev != nullptr; }

  [[nodiscard]] bool empty() const & noexcept { return queue_.empty(); }
  [[nodiscard]] std::size_t size() const & noexcept { return size_; }

private:
  c_tailq<Entry, Hook> queue_{};
  std::size_t size_ = 0;
};

/**
 * @brief Priority-ordered runqueue backed by a single intrusive list
 * kept sorted ascending by `Entry.*Priority` (lower value runs first).
 * `dequeue`/`peek`/`remove` are O(1); `enqueue` is O(n) (insertion-sort
 * scan).
 * @tparam Entry Caller-owned node type; must embed an intrusive link
 * field matching `reloco::c_tailq`'s hook layout, named by @p Hook.
 * @tparam Hook Pointer-to-member of @p Entry's link field.
 * @tparam Priority Pointer-to-member of @p Entry's priority field (an
 * unsigned integral type); read-only to this policy.
 */
template <typename Entry, auto Hook, auto Priority> class priority_list_runqueue {
public:
  constexpr priority_list_runqueue() noexcept = default;

  priority_list_runqueue(const priority_list_runqueue &) = delete;
  priority_list_runqueue &operator=(const priority_list_runqueue &) = delete;
  priority_list_runqueue(priority_list_runqueue &&) noexcept = default;
  priority_list_runqueue &operator=(priority_list_runqueue &&) noexcept = default;

  /**
   * @brief Inserts @p entry in priority order (lower `Entry.*Priority`
   * runs first; ties resolve FIFO, matching insertion order). O(n).
   */
  void enqueue(Entry &entry) & noexcept {
    for (auto &existing : queue_) {
      if (entry.*Priority < existing.*Priority) {
        queue_.insert_before(existing, entry);
        ++size_;
        return;
      }
    }
    queue_.push_back(entry);
    ++size_;
  }

  /** @brief Removes and returns the highest-priority entry, or `nullptr` if empty. O(1). */
  Entry *dequeue() & noexcept {
    Entry *entry = queue_.pop_front();
    if (entry != nullptr)
      --size_;
    return entry;
  }

  /** @brief Returns the highest-priority entry without removing it, or `nullptr` if empty. O(1). */
  [[nodiscard]] Entry *peek() const & noexcept { return queue_.front(); }

  /**
   * @brief Removes @p entry from wherever it currently sits in the
   * queue. O(1). Precondition: @p entry is currently enqueued in *this*
   * runqueue (see `is_linked` to check it first).
   */
  void remove(Entry &entry) & noexcept {
    queue_.remove(entry);
    --size_;
  }

  /**
   * @brief Returns whether @p entry is currently linked into *some*
   * `priority_list_runqueue<Entry, Hook, Priority>` (not necessarily
   * *this* instance) -- i.e. whether `remove(entry)` is currently safe
   * to call. O(1).
   */
  [[nodiscard]] static bool is_linked(const Entry &entry) noexcept { return (entry.*Hook).prev != nullptr; }

  [[nodiscard]] bool empty() const & noexcept { return queue_.empty(); }
  [[nodiscard]] std::size_t size() const & noexcept { return size_; }

private:
  c_tailq<Entry, Hook> queue_{};
  std::size_t size_ = 0;
};

/**
 * @brief O(1) per-priority-bucket runqueue: `NumPriorities` intrusive
 * lists (one per priority level) plus a fixed-size bitmap tracking
 * which buckets are non-empty, so the highest-priority non-empty
 * bucket is found with a single count-trailing-zeros scan --
 * `enqueue`/`dequeue`/`peek`/`remove` are all O(1), matching the
 * classic Linux "O(1) scheduler" design. Insertion order within a
 * single priority level is FIFO.
 * @tparam Entry Caller-owned node type; must embed an intrusive link
 * field matching `reloco::c_tailq`'s hook layout, named by @p Hook.
 * @tparam Hook Pointer-to-member of @p Entry's link field.
 * @tparam Priority Pointer-to-member of @p Entry's priority field (an
 * unsigned integral type, value in `[0, NumPriorities)`; read-only to
 * this policy).
 * @tparam NumPriorities Number of distinct priority levels, `[0,
 * NumPriorities)`. A compile-time constant -- the bucket array and
 * bitmap are both fixed-size, no allocation.
 */
template <typename Entry, auto Hook, auto Priority, std::size_t NumPriorities> class priority_bucket_runqueue {
  static_assert(NumPriorities >= 1, "priority_bucket_runqueue requires at least one priority level");

  static constexpr std::size_t bits_per_word = sizeof(std::uint64_t) * 8;
  static constexpr std::size_t word_count = (NumPriorities + bits_per_word - 1) / bits_per_word;

public:
  constexpr priority_bucket_runqueue() noexcept = default;

  priority_bucket_runqueue(const priority_bucket_runqueue &) = delete;
  priority_bucket_runqueue &operator=(const priority_bucket_runqueue &) = delete;
  priority_bucket_runqueue(priority_bucket_runqueue &&) noexcept = default;
  priority_bucket_runqueue &operator=(priority_bucket_runqueue &&) noexcept = default;

  /**
   * @brief Pushes @p entry to the tail of its `Entry.*Priority` bucket.
   * O(1). Precondition: `entry.*Priority < NumPriorities`.
   */
  void enqueue(Entry &entry) & noexcept {
    const std::size_t prio = static_cast<std::size_t>(entry.*Priority);
    RELOCO_ASSERT(prio < NumPriorities, "priority_bucket_runqueue: priority out of range");
    buckets_[prio].push_back(entry);
    set_bit(prio);
    ++size_;
  }

  /**
   * @brief Removes and returns the head of the lowest-numbered
   * non-empty bucket (highest priority), or `nullptr` if every bucket
   * is empty. O(1).
   */
  Entry *dequeue() & noexcept {
    optional<std::size_t> prio = find_first_set();
    if (!prio.has_value())
      return nullptr;
    Entry *entry = buckets_[*prio].pop_front();
    if (buckets_[*prio].empty())
      clear_bit(*prio);
    --size_;
    return entry;
  }

  /**
   * @brief Returns the head of the lowest-numbered non-empty bucket
   * without removing it, or `nullptr` if every bucket is empty. O(1).
   */
  [[nodiscard]] Entry *peek() const & noexcept {
    optional<std::size_t> prio = find_first_set();
    if (!prio.has_value())
      return nullptr;
    return buckets_[*prio].front();
  }

  /**
   * @brief Removes @p entry from its `Entry.*Priority` bucket. O(1).
   * Precondition: @p entry is currently enqueued in *this* runqueue
   * (and its `Entry.*Priority` has not changed since `enqueue`; see
   * `is_linked` to check it first).
   */
  void remove(Entry &entry) & noexcept {
    const std::size_t prio = static_cast<std::size_t>(entry.*Priority);
    RELOCO_ASSERT(prio < NumPriorities, "priority_bucket_runqueue: priority out of range");
    buckets_[prio].remove(entry);
    if (buckets_[prio].empty())
      clear_bit(prio);
    --size_;
  }

  /**
   * @brief Returns whether @p entry is currently linked into *some*
   * `priority_bucket_runqueue<Entry, Hook, Priority, NumPriorities>`
   * bucket (not necessarily *this* instance) -- i.e. whether
   * `remove(entry)` is currently safe to call. O(1).
   */
  [[nodiscard]] static bool is_linked(const Entry &entry) noexcept { return (entry.*Hook).prev != nullptr; }

  [[nodiscard]] bool empty() const & noexcept { return size_ == 0; }
  [[nodiscard]] std::size_t size() const & noexcept { return size_; }

private:
  // prio < max_priorities is a class invariant, so bitmap_ indices are in range.
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

  void set_bit(std::size_t prio) & noexcept {
    bitmap_[prio / bits_per_word] |= (std::uint64_t{1} << (prio % bits_per_word));
  }

  void clear_bit(std::size_t prio) & noexcept {
    bitmap_[prio / bits_per_word] &= ~(std::uint64_t{1} << (prio % bits_per_word));
  }

  // Scans the bitmap word-by-word, using `__builtin_ctzll` (lowers to a
  // single `tzcnt`/`bsf`/`rbit`+`clz` hardware instruction on every
  // target this library supports) to locate the lowest set bit within
  // the first non-zero word, rather than a hand-rolled bit-by-bit loop.
  [[nodiscard]] optional<std::size_t> find_first_set() const noexcept {
    for (std::size_t i = 0; i < word_count; ++i) {
      const std::uint64_t word = bitmap_[i];
      if (word != 0)
        return i * bits_per_word + static_cast<std::size_t>(__builtin_ctzll(word));
    }
    return nullopt;
  }

  RELOCO_END_UNSAFE_BUFFER_USAGE

  reloco::array<c_tailq<Entry, Hook>, NumPriorities> buckets_{};
  std::uint64_t bitmap_[word_count] = {};
  std::size_t size_ = 0;
};

} // namespace structo
