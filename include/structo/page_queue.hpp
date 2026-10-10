// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file page_queue.hpp
 * @brief Counted intrusive page queue (active / inactive / free / private offline queues) over a pluggable list.
 *
 * `page_queue<List, PageView>` is the container a VM keeps its page queues in. It adds what every such queue
 * needs on top of a bare intrusive list: a page count, order-preserving splicing between queues, LRU style
 * requeueing, iteration, and *marker* support for lock-dropping scans (`page_queue_scan`). Pages are handles
 * (`PageView::os_page_type`: pointers, compressed handles or indices), never raw addresses.
 *
 * **Markers.** A marker is a dummy descriptor linked into the queue as a scan cursor (see page_queue_scan.hpp
 * and `os_traits::is_marker`). The queue skips them everywhere (iteration, `front`, `next`, `pop_front`) and does
 * not count them. They are linked and unlinked only through `scan_ops()`; every page operation here asserts
 * that its argument is *not* a marker.
 *
 * **Locking.** The queue has no lock of its own; the caller holds the queue lock around every call (the
 * scan helper does that for you). Do not splice a queue that has an active scan: the scan's marker would move
 * with the pages (the element-wise path asserts on that; native `splice_*` cannot see it).
 *
 * The underlying list (`List`) only has to provide the following. All functions take/return handles:
 * @code
 * struct my_list {
 *   void clear() noexcept;
 *   reloco::optional<handle> front() const noexcept;                  // first node, any kind
 *   reloco::optional<handle> next(handle) const noexcept;             // node after, nullopt at the tail
 *   void push_front(handle) noexcept;
 *   void push_back(handle) noexcept;
 *   void insert_after(handle pos, handle node) noexcept;
 *   void insert_before(handle pos, handle node) noexcept;
 *   void remove(handle) noexcept;
 *   reloco::optional<handle> pop_front() noexcept;
 *   // Optional O(1) fast paths; without them splicing moves node by node (O(n)):
 *   void splice_back(my_list &other) noexcept;                        // append all of other, keep order
 *   void splice_front(my_list &other) noexcept;                       // prepend all of other, keep order
 * };
 * @endcode
 * `tailq_page_list<T, Hook>` below adapts `reloco::c_tailq` for pointer-handle descriptors.
 *
 * @code
 * struct page { page_link link; std::uint32_t flags; };                // descriptor with an embedded tailq hook
 * using list_t  = structo::tailq_page_list<page, &page::link>;
 * using queue_t = structo::page_queue<list_t, page_view_t>;           // page_view_t: your page_view over page *
 *
 * queue_t active, inactive;          // two queues, protected by the caller's queue lock
 * active.push_back(p);                // newest at the tail; size() is now 1
 * active.move_to_back(p);             // LRU "touch": requeue without changing size()
 * inactive.splice_back(active);       // move every page of 'active' to the tail of 'inactive', keeping order
 * for (auto h : inactive) { ... }     // iterate real pages (markers skipped); lock must stay held
 * auto ops = inactive.scan_ops();     // hand to page_queue_scan for lock-dropping scans
 * @endcode
 *
 * Integration:
 * - **Page daemon / decay:** use `scan_ops()` with `page_queue_scan` ([page_queue_scan.md](page_queue_scan.md)); on
 *   demotion `inactive.push_back(p)` after `active.remove(p)` (or `splice_*` for whole batches).
 * - **Hotplug / offline:** the disconnector's private queue is a `page_queue` too; `splice_back` the isolated
 *   free lists into it and `size()` is the number of pages already won. Marker descriptors live outside any
 *   segment that can be offlined.
 * - **Buddy / free queues:** `buddy_allocator`'s `FreeList` is a different (order-indexed) container; use
 *   `page_queue` for queue-based free/clear/laundry lists.
 */

#include <structo/phys_page.hpp>

#include <reloco/detail/assert.hpp>
#include <reloco/intrusive_c_tailq.hpp>
#include <reloco/iterator.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/optional.hpp>

#include <cstddef>
#include <type_traits>
#include <utility>

namespace structo {

namespace detail {
template <typename List, typename = void> struct has_native_splice : std::false_type {};
template <typename List>
struct has_native_splice<List, std::void_t<decltype(std::declval<List &>().splice_back(std::declval<List &>())),
                                            decltype(std::declval<List &>().splice_front(std::declval<List &>()))>>
    : std::true_type {};
} // namespace detail

template <typename List, typename PageView> class page_queue {
public:
  using list_type = List;
  using page_type = PageView;
  using handle_type = typename PageView::os_page_type;

  page_queue() noexcept = default;
  page_queue(const page_queue &) = delete;
  page_queue &operator=(const page_queue &) = delete;

  /** Number of real pages (markers are not counted). */
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

  /** First real page, or nullopt. */
  [[nodiscard]] reloco::optional<handle_type> front() const noexcept { return skip_markers(list_.front()); }
  /** Real page after `pos`, or nullopt at the tail. `pos` must be a queued real page. */
  [[nodiscard]] reloco::optional<handle_type> next(handle_type pos) const noexcept {
    assert_page(pos);
    return skip_markers(list_.next(pos));
  }

  void push_front(handle_type p) noexcept {
    assert_page(p);
    list_.push_front(p);
    ++size_;
  }
  void push_back(handle_type p) noexcept {
    assert_page(p);
    list_.push_back(p);
    ++size_;
  }
  void insert_after(handle_type pos, handle_type p) noexcept {
    assert_page(pos);
    assert_page(p);
    list_.insert_after(pos, p);
    ++size_;
  }
  void insert_before(handle_type pos, handle_type p) noexcept {
    assert_page(pos);
    assert_page(p);
    list_.insert_before(pos, p);
    ++size_;
  }

  /** Unlinks a page that is on this queue. */
  void remove(handle_type p) noexcept {
    assert_page(p);
    RELOCO_ASSERT(size_ > 0, "page_queue::remove on an empty queue");
    list_.remove(p);
    --size_;
  }

  /** Removes and returns the first real page. */
  [[nodiscard]] reloco::optional<handle_type> pop_front() noexcept {
    auto f = front();
    if (f) {
      list_.remove(*f);
      --size_;
    }
    return f;
  }

  /** LRU touch: moves a queued page to the tail (size unchanged). */
  void move_to_back(handle_type p) noexcept {
    assert_page(p);
    list_.remove(p);
    list_.push_back(p);
  }
  /** Moves a queued page to the head (size unchanged). */
  void move_to_front(handle_type p) noexcept {
    assert_page(p);
    list_.remove(p);
    list_.push_front(p);
  }

  /** Appends all pages of `other` (keeping their order); `other` becomes empty. */
  void splice_back(page_queue &other) noexcept {
    if (&other == this || other.size_ == 0) {
      return;
    }
    if constexpr (detail::has_native_splice<List>::value) {
      list_.splice_back(other.list_);
    } else {
      while (auto h = other.pop_node()) {
        list_.push_back(*h);
      }
    }
    size_ += other.size_;
    other.size_ = 0;
  }

  /** Prepends all pages of `other` (keeping their order); `other` becomes empty. */
  void splice_front(page_queue &other) noexcept {
    if (&other == this || other.size_ == 0) {
      return;
    }
    if constexpr (detail::has_native_splice<List>::value) {
      list_.splice_front(other.list_);
    } else {
      auto anchor = list_.front();
      while (auto h = other.pop_node()) {
        if (anchor) {
          list_.insert_before(*anchor, *h); // each page lands right before the old head: order preserved
        } else {
          list_.push_back(*h);
        }
      }
    }
    size_ += other.size_;
    other.size_ = 0;
  }

  /** Unlinks every page (and marker); the caller must have finished or reset all scans. */
  void clear() noexcept {
    list_.clear();
    size_ = 0;
  }

  /** Forward iterator over real pages; the queue lock must stay held for the whole loop. */
  class RELOCO_POINTER iterator {
  public:
    iterator() noexcept = default;
    iterator(const page_queue *q RELOCO_LIFETIMEBOUND, reloco::optional<handle_type> cur) noexcept : q_(q), cur_(cur) {}
    [[nodiscard]] handle_type operator*() const noexcept { return *cur_; }
    iterator &operator++() noexcept {
      cur_ = q_->next(*cur_);
      return *this;
    }
    [[nodiscard]] friend bool operator==(const iterator &a, const iterator &b) noexcept {
      if (a.cur_.has_value() != b.cur_.has_value()) {
        return false;
      }
      return !a.cur_.has_value() || *a.cur_ == *b.cur_;
    }
    [[nodiscard]] friend bool operator!=(const iterator &a, const iterator &b) noexcept { return !(a == b); }

  private:
    const page_queue *q_{nullptr};
    reloco::optional<handle_type> cur_{};
  };

  [[nodiscard]] iterator begin() const noexcept RELOCO_LIFETIMEBOUND { return {this, front()}; }
  [[nodiscard]] iterator end() const noexcept RELOCO_LIFETIMEBOUND { return {this, reloco::nullopt}; }

  /** `reloco::iterator_adaptor` over real pages (compose with `.take()`, `.map()`, ...); lock stays held. */
  class RELOCO_POINTER page_walk : public reloco::iterator_adaptor<page_walk, handle_type> {
  public:
    using item_type = handle_type;
    explicit page_walk(const page_queue &q RELOCO_LIFETIMEBOUND) noexcept : q_(&q), cur_(q.front()) {}
    [[nodiscard]] reloco::optional<handle_type> next_impl() noexcept {
      auto out = cur_;
      if (out) {
        cur_ = q_->next(*out);
      }
      return out;
    }

  private:
    const page_queue *q_;
    reloco::optional<handle_type> cur_;
  };
  [[nodiscard]] page_walk pages() const noexcept RELOCO_LIFETIMEBOUND { return page_walk(*this); }

  /** Accessors for `page_queue_scan`: link/unlink marker descriptors without counting them. */
  class RELOCO_POINTER scan_ops_type {
  public:
    using handle_type = typename page_queue::handle_type;
    explicit scan_ops_type(page_queue &q RELOCO_LIFETIMEBOUND) noexcept : q_(&q) {}
    [[nodiscard]] reloco::optional<handle_type> first() noexcept { return q_->front(); }
    [[nodiscard]] reloco::optional<handle_type> next_after(handle_type pos) noexcept {
      return q_->skip_markers(q_->list_.next(pos)); // 'pos' may be a marker, so no page assertion here
    }
    void insert_after(handle_type pos, handle_type marker) noexcept {
      RELOCO_ASSERT(PageView::os_traits_type::is_marker(marker), "scan marker must be a marker descriptor");
      q_->list_.insert_after(pos, marker);
    }
    void remove(handle_type marker) noexcept {
      RELOCO_ASSERT(PageView::os_traits_type::is_marker(marker), "scan marker must be a marker descriptor");
      q_->list_.remove(marker);
    }

  private:
    page_queue *q_;
  };
  [[nodiscard]] scan_ops_type scan_ops() noexcept RELOCO_LIFETIMEBOUND { return scan_ops_type(*this); }

private:
  static void assert_page([[maybe_unused]] handle_type p) noexcept {
    RELOCO_ASSERT(!PageView::os_traits_type::is_marker(p), "marker descriptor used as a page in page_queue");
  }

  [[nodiscard]] reloco::optional<handle_type> skip_markers(reloco::optional<handle_type> n) const noexcept {
    while (n && PageView::os_traits_type::is_marker(*n)) {
      n = list_.next(*n);
    }
    return n;
  }

  // Pops the next node of any kind for node-by-node splicing; a linked marker here is a caller bug.
  [[nodiscard]] reloco::optional<handle_type> pop_node() noexcept {
    auto h = list_.pop_front();
    RELOCO_ASSERT(!h || !PageView::os_traits_type::is_marker(*h), "splice of a queue with a linked scan marker");
    return h;
  }

  List list_{};
  std::size_t size_{0};
};

/**
 * @brief `reloco::c_tailq` adapter for pointer-handle descriptors (`T *`), satisfying the `page_queue` list contract.
 * @tparam T    Descriptor type embedding the hook.
 * @tparam Hook Pointer-to-member of the hook field, as for `reloco::c_tailq<T, Hook>`.
 */
template <typename T, auto Hook> class tailq_page_list {
  using tailq_type = reloco::c_tailq<T, Hook>;
  using access = reloco::detail::c_tailq_hook_access<T, Hook>;

public:
  void clear() noexcept { list_.clear(); }

  [[nodiscard]] reloco::optional<T *> front() const noexcept { return wrap(list_.front()); }
  [[nodiscard]] reloco::optional<T *> next(T *p) const noexcept { return wrap(access::next(static_cast<const T *>(p))); }

  void push_front(T *p) noexcept { list_.push_front(*p); }
  void push_back(T *p) noexcept { list_.push_back(*p); }
  void insert_after(T *pos, T *p) noexcept { list_.insert_after(*pos, *p); }
  void insert_before(T *pos, T *p) noexcept { list_.insert_before(*pos, *p); }
  void remove(T *p) noexcept { list_.remove(*p); }
  [[nodiscard]] reloco::optional<T *> pop_front() noexcept { return wrap(list_.pop_front()); }

private:
  static reloco::optional<T *> wrap(T *p) noexcept {
    if (p == nullptr) {
      return reloco::nullopt;
    }
    return p;
  }

  tailq_type list_;
};

} // namespace structo
