// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file page_queue_scan.hpp
 * @brief Page-daemon scan over an intrusive page queue whose lock is dropped while each page is processed.
 *
 * A daemon that ages or reclaims pages cannot hold the queue lock for a whole pass: the body needs other locks
 * (cache/object lock), may sleep for I/O, and other CPUs keep adding, removing and re-queueing pages meanwhile.
 * A plain list iterator would then point at a page that has been freed or moved. `page_queue_scan` keeps its
 * position in a **marker**: a dummy descriptor that sits *in the queue* right after the page last returned.
 * Because the marker (not a page) is the cursor, any page can leave the queue while the lock is dropped and
 * the scan still knows where to continue.
 *
 * Each step (`next()`, or the loop head of a range-for) takes the queue lock itself, moves the marker past the
 * next real page, and releases the lock before returning that page's handle. The loop body therefore runs
 * *without* the queue lock, and the page it was given is only a hint: **take the lock again (`locked()`) and
 * check that the page is still on this queue before acting on it.** Destroying the scan (including `break`,
 * `return` or an exception out of the loop) removes the marker.
 *
 * The queue is accessed through `Ops`, which you implement over your own intrusive queue:
 *
 * @code
 * struct active_queue_ops {
 *   using handle_type = page_handle;                       // a pointer-like/compressed page handle (copyable)
 *   // First page of the queue, skipping markers (any scan's marker); nullopt when empty. Lock is held.
 *   reloco::optional<handle_type> first() noexcept;
 *   // Page after 'pos' in the queue, skipping markers; nullopt at the tail. 'pos' may be our marker. Lock held.
 *   reloco::optional<handle_type> next_after(handle_type pos) noexcept;
 *   // Link 'marker' right after 'pos' / unlink it. Lock held. The marker is a descriptor with queue links
 *   // that your own queue code recognises as "not a real page" (so other scans and reclaim skip it).
 *   void insert_after(handle_type pos, handle_type marker) noexcept;
 *   void remove(handle_type marker) noexcept;
 * };
 *
 * // Typical daemon pass: 'marker' is a per-daemon (or per-queue-and-daemon) descriptor allocated once.
 * structo::page_queue_scan scan(ops, queue_lock, marker, decay_scan_count(...));   // 4th arg: max_visits
 * for (auto page : scan) {                                  // queue lock is NOT held in the body
 *   scan.locked([&] {                                       // re-take it to validate and to move the page
 *     if (!still_on_active_queue(page)) return;             // freed / moved / re-queued meanwhile: skip
 *     age_or_demote(page);                                  // e.g. page_decay<>::step + queue_move
 *   });
 * }
 * @endcode
 *
 * Integration:
 * - **Page decay:** pass `decay_scan_count()` as `max_visits` and run `page_decay::step()` in `locked()` (or
 *   under the cache lock first, then `locked()` for the queue move; keep the documented lock order).
 * - **Hotplug/offline:** pages being isolated are removed from the queue under the queue lock, which the scan
 *   tolerates by design; the marker descriptor must itself never live in an offlining segment (allocate it
 *   statically).
 * - **Several daemons / queues:** give each scan its own marker. `Ops::first/next_after` must skip every
 *   marker so scans never return each other's markers as pages.
 * - A page re-queued at the tail while being scanned can be returned again; use `max_visits` to bound a pass.
 */

#include <reloco/iterator.hpp>
#include <reloco/optional.hpp>

#include <cstddef>

namespace structo {

template <typename Ops, typename Lockable>
class page_queue_scan : public reloco::iterator_adaptor<page_queue_scan<Ops, Lockable>, typename Ops::handle_type> {
public:
  using handle_type = typename Ops::handle_type;
  using item_type = handle_type;

  /**
   * @param ops        Queue accessors (see file comment); held by reference.
   * @param lock       BasicLockable guarding the queue (`lock()` / `unlock()`); held by reference.
   * @param marker     Descriptor used as the in-queue cursor; must not be in the queue at construction.
   * @param max_visits Stop after this many pages (0 = until the tail).
   */
  page_queue_scan(Ops &ops, Lockable &lock, handle_type marker, std::size_t max_visits = 0) noexcept
      : ops_(ops), lock_(lock), marker_(marker), max_visits_(max_visits) {}

  page_queue_scan(const page_queue_scan &) = delete;
  page_queue_scan &operator=(const page_queue_scan &) = delete;

  ~page_queue_scan() { finish(); }

  /** Next page after the marker, or nullopt at the tail / after `max_visits` pages. Takes the lock itself. */
  [[nodiscard]] reloco::optional<handle_type> next_impl() noexcept {
    if (done_) {
      return reloco::nullopt;
    }
    if (max_visits_ != 0 && visited_ >= max_visits_) {
      finish();
      return reloco::nullopt;
    }
    lock_.lock();
    reloco::optional<handle_type> found = linked_ ? ops_.next_after(marker_) : ops_.first();
    if (linked_) {
      ops_.remove(marker_);
      linked_ = false;
    }
    if (found) {
      ops_.insert_after(*found, marker_);
      linked_ = true;
    }
    lock_.unlock();
    if (!found) {
      done_ = true;
      return reloco::nullopt;
    }
    ++visited_;
    return found;
  }

  /** Runs `fn` with the queue lock held (the lock is not held in the loop body itself). */
  template <typename Fn> void locked(Fn &&fn) {
    lock_.lock();
    fn();
    lock_.unlock();
  }

  /** Ends the scan early and unlinks the marker (also done by the destructor). */
  void finish() noexcept {
    if (linked_) {
      lock_.lock();
      ops_.remove(marker_);
      lock_.unlock();
      linked_ = false;
    }
    done_ = true;
  }

  [[nodiscard]] std::size_t visited() const noexcept { return visited_; }

private:
  Ops &ops_;
  Lockable &lock_;
  handle_type marker_;
  std::size_t max_visits_;
  std::size_t visited_{0};
  bool linked_{false};
  bool done_{false};
};

} // namespace structo
