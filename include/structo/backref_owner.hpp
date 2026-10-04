// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file backref_owner.hpp
 * @brief `structo::backref_owner<BackrefField, Container, LockT>`: the
 * *owner*-side counterpart to [`backref_ptr`](backref_ptr.hpp) -- tracks
 * every holder currently attached to one owner (e.g. a `vm_object`'s
 * resident pages) and keeps each holder's `backref_ptr` in lock-step with
 * that tracking, including clearing every attached holder's pointer in
 * one call when the owner itself is going away (e.g. FreeBSD's
 * `vm_object_terminate()` walking `memq` and calling `vm_page_remove()`
 * on everything still resident).
 *
 * `backref_ptr<T, Cell>` alone only ever answers "what does *this one*
 * holder currently point at, and can I safely read/change it" -- it has
 * no notion of *which* holders a given owner currently has, so nothing
 * can walk "all of my holders" to clear them when the owner is destroyed.
 * `backref_owner` adds exactly that, and nothing else: it does not
 * replace `backref_ptr`'s own locking (reading/writing one holder's
 * pointer is still always done through its `Cell`), it only sequences
 * *that* with inserting/removing the holder from whatever membership
 * structure the owner actually keeps its holders in.
 *
 * ## Container-agnostic by design
 *
 * Real owners keep their holders in very different structures depending
 * on what they need to do with them afterwards -- a FreeBSD `vm_object`
 * keeps `memq` as a plain `TAILQ` for "walk everything", but production
 * object/page-cache designs at least as often key pages by an offset in
 * a tree (an `intrusive_splay_tree`/`intrusive_rbtree` for `try_find`,
 * not just `TAILQ`'s linear walk) -- and some may want neither (an
 * array, a `flat_set`, ...). `backref_owner` therefore does not embed
 * any specific container: @p Container is a caller-supplied adapter
 * satisfying the small `Container` contract below, so the exact same
 * `backref_owner` works unchanged whichever membership structure an
 * owner actually needs for its own reasons.
 *
 * ## `Container` contract
 *
 * A `Container` must be default-constructible and provide:
 * - `reloco::result<void> insert(Holder &holder) & noexcept;` -- links
 *   `holder` in. May fail (e.g. `error::already_exists` for a
 *   keyed/sorted container) without having linked it; `backref_owner`
 *   leaves the holder's `backref_ptr` untouched in that case.
 * - `void remove(Holder &holder) & noexcept;` -- unlinks `holder`,
 *   already known (by the caller) to be currently linked.
 * - `template <typename F> void clear_and_dispose(F &&fn) & noexcept;`
 *   -- calls `fn(holder)` once for every currently-linked holder (in
 *   unspecified order), leaving the container empty once it returns.
 * - `template <typename F> void for_each(F &&fn) & noexcept;` -- calls
 *   `fn(holder)` once for every currently-linked holder (in unspecified
 *   order) without unlinking anything.
 * - `template <typename Pred, typename Disposer> void remove_if(Pred
 *   &&pred, Disposer &&disposer) & noexcept;` -- for every
 *   currently-linked holder for which `pred(holder)` returns `true`,
 *   unlinks it and calls `disposer(holder)`; holders for which `pred`
 *   returns `false` stay linked and untouched.
 * - `[[nodiscard]] bool empty() const & noexcept;`
 *
 * A `Container` may optionally also provide `template <typename K>
 * reloco::result<std::reference_wrapper<Holder>> try_find(const K &key)
 * & noexcept;` for keyed containers (e.g. the splay-tree adapter below)
 * -- `backref_owner::try_find()` simply forwards to it and is only ever
 * instantiated (so only ever required to exist) if a caller actually
 * calls it, same as any other member function template; a
 * walk-everything container with no notion of a key (e.g. the tailq
 * adapter below) need not provide it at all.
 *
 * Two example adapters, for the two containers mentioned above (both
 * just forward to an already-reviewed `reloco` intrusive container --
 * `backref_owner` itself never needs to know this):
 *
 * @code
 * // FreeBSD vm_object::memq-style: a plain walk-everything tail queue.
 * template <typename Holder, auto Hook> struct tailq_container {
 *   reloco::c_tailq<Holder, Hook> list;
 *
 *   reloco::result<void> insert(Holder &holder) & noexcept {
 *     list.push_back(holder);
 *     return {};
 *   }
 *   void remove(Holder &holder) & noexcept { list.remove(holder); }
 *   template <typename F> void clear_and_dispose(F &&fn) & noexcept {
 *     while (Holder *h = list.pop_front())
 *       fn(*h);
 *   }
 *   template <typename F> void for_each(F &&fn) & noexcept {
 *     for (Holder &holder : list)
 *       fn(holder);
 *   }
 *   template <typename Pred, typename Disposer> void remove_if(Pred &&pred, Disposer &&disposer) & noexcept {
 *     for (auto it = list.begin(); it != list.end();) {
 *       Holder &holder = *it;
 *       ++it; // advance past `holder` before possibly unlinking it
 *       if (pred(holder)) {
 *         list.remove(holder);
 *         disposer(holder);
 *       }
 *     }
 *   }
 *   [[nodiscard]] bool empty() const & noexcept { return list.empty(); }
 * };
 *
 * // Keyed-by-offset, e.g. a page cache looking pages up by file offset.
 * template <typename Holder, intrusive_splay_tree_hook<Holder> Holder::*Hook, typename KeyOf>
 * struct splay_container {
 *   reloco::intrusive_splay_tree<Holder, Hook, KeyOf> tree;
 *
 *   reloco::result<void> insert(Holder &holder) & noexcept { return tree.try_insert(holder); }
 *   void remove(Holder &holder) & noexcept { tree.remove(holder); }
 *   template <typename F> void clear_and_dispose(F &&fn) & noexcept {
 *     tree.clear_and_dispose(std::forward<F>(fn));
 *   }
 *   template <typename F> void for_each(F &&fn) & noexcept {
 *     for (Holder &holder : tree)
 *       fn(holder);
 *   }
 *   template <typename Pred, typename Disposer> void remove_if(Pred &&pred, Disposer &&disposer) & noexcept {
 *     for (auto it = tree.begin(); it != tree.end();) {
 *       if (pred(*it)) {
 *         Holder &holder = *it;
 *         it = tree.erase(it);
 *         disposer(holder);
 *       } else {
 *         ++it;
 *       }
 *     }
 *   }
 *   [[nodiscard]] bool empty() const & noexcept { return tree.empty(); }
 *
 *   // Optional: only keyed containers need offer this.
 *   template <typename K> reloco::result<std::reference_wrapper<Holder>> try_find(const K &key) & noexcept {
 *     return tree.try_find(key);
 *   }
 * };
 * @endcode
 *
 * ## Usage
 *
 * @code
 * struct vm_object;
 *
 * struct vm_page {
 *   structo::backref_ptr<vm_object> owner;
 *   reloco::c_tailq<vm_page, &vm_page::memq_link>::hook_type memq_link;
 * };
 *
 * struct vm_object {
 *   structo::backref_owner<&vm_page::owner, tailq_container<vm_page, &vm_page::memq_link>> pages;
 * };
 *
 * void vm_object_insert_page(vm_object &obj, vm_page &page) {
 *   auto r = obj.pages.attach(obj, page); // links `page` into `obj.pages` *and* sets `page.owner`
 *   RELOCO_ASSERT(r.has_value(), "page already resident somewhere");
 * }
 *
 * void vm_object_remove_page(vm_object &obj, vm_page &page) {
 *   obj.pages.detach(page); // unlinks `page` *and* clears `page.owner` back to nullptr
 * }
 *
 * void vm_object_terminate(vm_object &obj) {
 *   obj.pages.detach_all(); // clears every still-resident page's `owner` back to nullptr
 * }
 *
 * // Reparenting one still-resident page from a shadow object onto the
 * // object it shadows (e.g. during `vm_object_collapse`) -- walking
 * // every resident page this way is exactly how FreeBSD's own
 * // collapse/backing-scan logic reparents a whole object's worth:
 * void vm_page_reparent(vm_object &shadow, vm_object &backing, vm_page &page) {
 *   auto r = shadow.pages.migrate_to(backing.pages, backing, page);
 *   RELOCO_ASSERT(r.has_value(), "backing object already has a page at this key");
 * }
 *
 * // Looking an already-resident page up by key, e.g. before deciding
 * // whether a fault needs to allocate a new page at all (only works
 * // because `tailq_container` above was swapped for a keyed
 * // `splay_container<vm_page, &vm_page::offset_link, vm_page_key_of>`
 * // -- see the `Container` contract above for why `try_find` is
 * // optional and container-dependent):
 * vm_page *vm_page_lookup(vm_object &obj, std::uint64_t offset) {
 *   auto r = obj.pages.try_find(offset);
 *   return r.has_value() ? &r.value().get() : nullptr;
 * }
 *
 * // FreeBSD's vm_object_page_remove(object, start, end): evict just the
 * // pages whose offset falls in [start, end), leaving the rest resident.
 * void vm_object_page_remove(vm_object &obj, std::uint64_t start, std::uint64_t end) {
 *   obj.pages.detach_if([&](vm_page &page) noexcept { return page.offset >= start && page.offset < end; });
 * }
 *
 * // A read-only walk that never touches membership or the backref at
 * // all, e.g. tallying how many resident pages are currently dirty:
 * std::size_t vm_object_count_dirty(vm_object &obj) {
 *   std::size_t dirty = 0;
 *   obj.pages.for_each([&](vm_page &page) noexcept { dirty += page.dirty ? 1 : 0; });
 *   return dirty;
 * }
 * @endcode
 *
 * ## Locking
 *
 * `backref_owner` holds exactly one lock of its own (@p LockT, default
 * `reloco::spin_lock`), guarding `Container` membership only -- it is
 * taken and released around `insert`/`remove`/`clear_and_dispose` calls.
 * In `attach()`/`detach()` (and their `try_` counterparts) it is never
 * held nested with a holder's own `Cell` lock (that lock is always
 * acquired strictly after this one has already been released). There is
 * therefore no lock-ordering hazard to reason about between the two for
 * those calls: a concurrent `backref_ptr::lock()`/`try_lock()`/
 * `read_unlocked()` reader of one specific holder never has to wait on
 * (or participate in any ordering with) `backref_owner`'s own lock at
 * all.
 *
 * `migrate_to()`/`try_migrate_to()` are the one deliberate exception:
 * they acquire the holder's `Cell` lock *first* and hold it across both
 * registries' critical sections, releasing it only once the backref has
 * been reassigned (see their own doc comments for why -- in short, to
 * avoid a reader observing the holder already unlinked from/relinked
 * into a `Container` while the backref still names the old owner).
 * This cannot introduce a deadlock against `attach()`/`detach()`: those
 * never hold a registry lock while waiting on a `Cell` lock (they always
 * release the registry lock first), so the reversed nesting order
 * `migrate_to()` uses never has to wait on a thread stuck in the other
 * order.
 *
 * This does mean `attach()`/`detach()` are each individually atomic
 * (membership and the holder's pointer are never observed inconsistent
 * from *outside* either critical section), but the two critical
 * sections making up one call are not atomic *with each other*: a
 * reader could in principle observe a holder briefly linked into the
 * container with its `backref_ptr` not yet reset to the new owner
 * (during `attach()`), or already cleared to `nullptr` despite the
 * holder still being linked (`detach()`, until it finishes). This is the
 * same trade-off `backref_ptr` itself already documents for its own
 * `Cell`s (never *incorrect*, only a momentary staleness window), and
 * matches the FreeBSD/Linux precedent this header is modeled on: both
 * require the holder to already be otherwise quiesced (e.g. "busied")
 * by the caller before `attach`/`detach`/reassignment, which is exactly
 * why `detach_all()` requires no concurrent `attach()`/`detach()`/
 * `try_attach()`/`try_detach()` call on the same `backref_owner` (see
 * its own doc comment) -- it is only ever meant for owner teardown, once
 * nothing else can reach the owner to attach a new holder anymore.
 */

#include "backref_ptr.hpp"

#include <reloco/detail/assert.hpp>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/spin_lock.hpp>

#include <functional>
#include <utility>

namespace structo {

namespace detail {

/** @brief Pulls `Owner`/`Cell`/`Holder` out of a `backref_ptr<Owner,
 * Cell> Holder::*` pointer-to-member type -- the shape `backref_owner`'s
 * `BackrefField` non-type template parameter must have. */
template <typename M> struct backref_member_traits;

template <typename Owner, typename Cell, typename Holder> struct backref_member_traits<backref_ptr<Owner, Cell> Holder::*> {
  using owner_type = Owner;
  using cell_type = Cell;
  using holder_type = Holder;
};

} // namespace detail

/**
 * @brief Owner-side registry: tracks every holder attached to one owner
 * and keeps each holder's `backref_ptr<Owner, Cell>` (named by @p
 * BackrefField, e.g. `&vm_page::owner`) in lock-step with a
 * caller-supplied @p Container (see this file's doc comment for the
 * `Container` contract and two example adapters). @p LockT defaults to
 * `reloco::spin_lock`, matching `structo::sync::lock_striping`'s own
 * default.
 */
template <auto BackrefField, typename Container, typename LockT = reloco::spin_lock> class backref_owner {
  using traits = detail::backref_member_traits<decltype(BackrefField)>;

public:
  using owner_type = typename traits::owner_type;
  using cell_type = typename traits::cell_type;
  using holder_type = typename traits::holder_type;

  constexpr backref_owner() noexcept = default;

  backref_owner(const backref_owner &) = delete;
  backref_owner &operator=(const backref_owner &) = delete;

  /** @brief Traps (via `RELOCO_ASSERT`) if destroyed while any holder
   * is still attached -- call `detach_all()` first. */
  ~backref_owner() noexcept {
    RELOCO_ASSERT(container_.empty(), "structo::backref_owner: destroyed with holders still attached; call "
                                       "detach_all() first");
  }

  /**
   * @brief Links @p holder into `Container`, then sets its backref to
   * `&self`. Blocks until this registry's own lock is acquired. Fails
   * (without touching @p holder's backref) if `Container::insert` does;
   * `RELOCO_ASSERT`s @p holder was not already attached elsewhere.
   */
  reloco::result<void> attach(owner_type &self, holder_type &holder) & noexcept {
    lock_.lock();
    auto r = container_.insert(holder);
    lock_.unlock();
    if (!r.has_value())
      return r;
    auto g = (holder.*BackrefField).lock();
    RELOCO_ASSERT(g.get() == nullptr, "structo::backref_owner::attach: holder is already attached to an owner");
    g.reset(&self);
    return {};
  }

  /** @brief Non-blocking counterpart to `attach()`; fails with
   * `error::busy` if this registry's own lock is currently held
   * elsewhere (`Container::insert`'s own failure modes still apply
   * otherwise). */
  reloco::result<void> try_attach(owner_type &self, holder_type &holder) & noexcept {
    if (!lock_.try_lock())
      return reloco::unexpected(reloco::error::busy);
    auto r = container_.insert(holder);
    lock_.unlock();
    if (!r.has_value())
      return r;
    auto g = (holder.*BackrefField).lock();
    RELOCO_ASSERT(g.get() == nullptr, "structo::backref_owner::try_attach: holder is already attached to an owner");
    g.reset(&self);
    return {};
  }

  /**
   * @brief Unlinks @p holder from `Container`, then clears its backref
   * back to `nullptr`. Blocks until this registry's own lock is
   * acquired. @p holder must currently be linked (see `Container::remove`).
   */
  void detach(holder_type &holder) & noexcept {
    lock_.lock();
    container_.remove(holder);
    lock_.unlock();
    (holder.*BackrefField).lock().reset(nullptr);
  }

  /** @brief Non-blocking counterpart to `detach()`; fails with
   * `error::busy` if this registry's own lock is currently held
   * elsewhere. */
  reloco::result<void> try_detach(holder_type &holder) & noexcept {
    if (!lock_.try_lock())
      return reloco::unexpected(reloco::error::busy);
    container_.remove(holder);
    lock_.unlock();
    (holder.*BackrefField).lock().reset(nullptr);
    return {};
  }

  /**
   * @brief Owner teardown: clears every still-attached holder's backref
   * back to `nullptr` and empties `Container`. Must not run concurrently
   * with `attach()`/`try_attach()`/`detach()`/`try_detach()` on this same
   * registry -- see this file's "Locking" doc section.
   */
  void detach_all() & noexcept {
    lock_.lock();
    container_.clear_and_dispose(
        [](holder_type &holder) noexcept { (holder.*BackrefField).lock().reset(nullptr); });
    lock_.unlock();
  }

  /**
   * @brief Read-only walk of every currently-attached holder, in
   * `Container`'s own (unspecified) order (e.g. tallying how many
   * resident pages are dirty). Touches neither `Container` membership
   * nor any holder's backref. Blocks until this registry's own lock is
   * acquired, and holds it for the *entire* walk -- keep @p fn cheap,
   * and do not call back into this same registry's `attach`/`detach`/
   * `migrate_to`/`for_each`/`detach_if`/`try_find`/`empty`/`~backref_owner`
   * from within @p fn (`LockT` is not required to be reentrant).
   */
  template <typename F> void for_each(F &&fn) & noexcept {
    lock_.lock();
    container_.for_each(std::forward<F>(fn));
    lock_.unlock();
  }

  /**
   * @brief Conditionally detaches holders: for every currently-attached
   * holder for which `pred(holder)` returns `true`, unlinks it from
   * `Container` and clears its backref back to `nullptr`; holders for
   * which `pred` returns `false` stay attached and untouched (e.g.
   * FreeBSD's `vm_object_page_remove(object, start, end)` evicting only
   * the pages whose offset falls within a given range). Blocks until
   * this registry's own lock is acquired, and holds it for the *entire*
   * walk -- same reentrancy caveat as `for_each()` applies to @p pred.
   */
  template <typename Pred> void detach_if(Pred &&pred) & noexcept {
    lock_.lock();
    container_.remove_if(std::forward<Pred>(pred),
                          [](holder_type &holder) noexcept { (holder.*BackrefField).lock().reset(nullptr); });
    lock_.unlock();
  }

  /**
   * @brief Passthrough to `Container::try_find` (e.g. FreeBSD's
   * `vm_page_lookup()` finding an already-resident page by offset) --
   * only ever instantiated (so only ever required of `Container`) if a
   * caller actually calls it; a walk-everything `Container` with no
   * notion of a key need not provide `try_find` at all (see this file's
   * `Container` contract doc section). Blocks until this registry's own
   * lock is acquired, releasing it again before returning -- as with
   * every other `backref_owner` operation, the caller is responsible for
   * externally serializing this against a concurrent `attach`/`detach`/
   * `migrate_to` of the same holder racing the use of the returned
   * reference (see this file's "Locking" doc section).
   */
  template <typename K>
  [[nodiscard]] reloco::result<std::reference_wrapper<holder_type>> try_find(const K &key) & noexcept {
    lock_.lock();
    auto r = container_.try_find(key);
    lock_.unlock();
    return r;
  }

  /**
   * @brief Moves @p holder from this registry to @p target (e.g.
   * FreeBSD's `vm_object_collapse`/a fork reparenting a resident page
   * from a shadow object to the object it shadows, or a page moving to
   * the child on a copy-on-write fault): unlinks @p holder from this
   * `Container`, links it into @p target's, then reassigns its backref
   * to `&new_owner` -- all without ever requiring the backref to be
   * observed `nullptr` in between. Unlike `attach()`, this never asserts
   * the backref was previously empty, since migrating a holder that
   * already points at *this* registry's owner is exactly the point.
   *
   * @p target must be a different `backref_owner` instance (migrating a
   * holder to its own registry is a no-op `Container` would reject
   * anyway). On failure (@p target's own `Container::insert` rejects
   * @p holder, e.g. a keyed container already holding an equivalent
   * key), @p holder is reinserted back into this registry's `Container`
   * and the backref is left untouched, as if `migrate_to` had not been
   * called at all.
   *
   * Holds @p holder's own `Cell` lock across the *entire* migration
   * (acquired before either registry lock, released only once the
   * backref has been reassigned at the very end) -- this is the one
   * deliberate exception to this file's "Locking" section's "never held
   * nested" rule, made specifically so that no concurrent
   * `backref_ptr::lock()`/`try_lock()`/`read_unlocked()` reader can ever
   * observe @p holder already unlinked from (or relinked into) a
   * `Container` while its backref still names the *old* owner: readers
   * either block (or fail with `error::busy`) for the whole migration,
   * or see the fully-finished result, never anything in between. See
   * this file's "Locking" doc section for why nesting it this way
   * (rather than the registry-lock-then-Cell-lock order every other
   * method uses) cannot introduce a lock-ordering deadlock.
   *
   * Beyond that, blocks until each registry's own lock is acquired in
   * turn -- this registry's, then (separately) @p target's, then (on
   * failure only) this registry's again for the rollback. Must not run
   * concurrently with any other
   * `attach`/`try_attach`/`detach`/`try_detach`/`migrate_to`/
   * `try_migrate_to` call affecting the same @p holder, nor with
   * `detach_all()` on either registry.
   */
  reloco::result<void> migrate_to(backref_owner &target, owner_type &new_owner, holder_type &holder) & noexcept {
    auto guard = (holder.*BackrefField).lock();

    lock_.lock();
    container_.remove(holder);
    lock_.unlock();

    target.lock_.lock();
    auto r = target.container_.insert(holder);
    target.lock_.unlock();

    if (!r.has_value()) {
      rollback_insert(holder);
      return r;
    }

    guard.reset(&new_owner);
    return {};
  }

  /** @brief Non-blocking counterpart to `migrate_to()`; fails with
   * `error::busy` if @p holder's own `Cell` lock or either registry's
   * own lock is currently held elsewhere (rolling back as needed). */
  reloco::result<void> try_migrate_to(backref_owner &target, owner_type &new_owner, holder_type &holder) & noexcept {
    auto guard_result = (holder.*BackrefField).try_lock();
    if (!guard_result.has_value())
      return reloco::unexpected(guard_result.error());
    auto &guard = guard_result.value();

    if (!lock_.try_lock())
      return reloco::unexpected(reloco::error::busy);
    container_.remove(holder);
    lock_.unlock();

    if (!target.lock_.try_lock()) {
      rollback_insert(holder);
      return reloco::unexpected(reloco::error::busy);
    }
    auto r = target.container_.insert(holder);
    target.lock_.unlock();

    if (!r.has_value()) {
      rollback_insert(holder);
      return r;
    }

    guard.reset(&new_owner);
    return {};
  }

  /** @brief Whether any holder is currently attached. Blocks until this
   * registry's own lock is acquired. */
  [[nodiscard]] bool empty() & noexcept {
    lock_.lock();
    bool e = container_.empty();
    lock_.unlock();
    return e;
  }

private:
  // Restores `holder` into this registry's `Container` after a failed
  // `migrate_to`/`try_migrate_to` already removed it -- re-insertion
  // reusing the same key it was just removed under is expected to
  // always succeed; a `Container` for which that does not hold would
  // leave `holder` orphaned (removed from both registries, backref
  // untouched), which is why this traps rather than silently continuing.
  void rollback_insert(holder_type &holder) & noexcept {
    lock_.lock();
    auto rollback = container_.insert(holder);
    lock_.unlock();
    RELOCO_ASSERT(rollback.has_value(),
                  "structo::backref_owner::migrate_to: rollback insert into the source registry failed");
  }

  // Deliberately plain default-member-declarations, not `{}`
  // value-initializers: a `Container` (or `LockT`) whose default
  // constructor is implicitly defined on top of a *protected* base
  // constructor (e.g. `reloco::intrusive_splay_tree`, built on
  // `detail::intrusive_bst_base`'s protected default constructor) hits a
  // real GCC access-control bug under `-std=c++17` when value-initialized
  // via a brace NSDMI written outside that type's own definition --
  // `T member{};` fails to compile where `T member;` (default-
  // initialization, calling the exact same constructor) does not. Both
  // forms are otherwise equivalent for a type with a user-declared (even
  // if defaulted) default constructor, so this costs nothing.
  LockT lock_;
  Container container_;
};

} // namespace structo
