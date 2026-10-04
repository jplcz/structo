<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::backref_owner`

`include/structo/backref_owner.hpp`

The *owner*-side counterpart to [`backref_ptr`](backref_ptr.md): tracks
every holder currently attached to one owner (e.g. a `vm_object`'s
resident pages) and keeps each holder's `backref_ptr` in lock-step with
that tracking, including clearing every attached holder's pointer in
one call when the owner itself is going away (e.g. FreeBSD's
`vm_object_terminate()` walking `memq` and calling `vm_page_remove()`
on everything still resident).

`backref_ptr<T, Cell>` alone only ever answers "what does *this one*
holder currently point at, and can I safely read/change it" -- it has
no notion of *which* holders a given owner currently has, so nothing
can walk "all of my holders" to clear them when the owner is destroyed.
`backref_owner` adds exactly that, and nothing else: it does not
replace `backref_ptr`'s own locking (reading/writing one holder's
pointer is still always done through its `Cell`), it only sequences
*that* with inserting/removing the holder from whatever membership
structure the owner actually keeps its holders in.

## Container-agnostic by design

Real owners keep their holders in very different structures depending
on what they need to do with them afterwards -- a FreeBSD `vm_object`
keeps `memq` as a plain `TAILQ` for "walk everything", but production
object/page-cache designs at least as often key pages by an offset in
a tree (an `intrusive_splay_tree`/`intrusive_rbtree` for `try_find`,
not just `TAILQ`'s linear walk) -- and some may want neither. Rather
than picking one, `backref_owner` is generic infrastructure: it is the
caller's responsibility to provide a small `Container` adapter
proxying whichever structure they actually need, so the exact same
`backref_owner` works unchanged either way.

## Usage

```cpp
struct vm_object;

struct vm_page {
  structo::backref_ptr<vm_object> owner;
  reloco::c_tailq<vm_page, &vm_page::memq_link>::hook_type memq_link;
};

struct vm_object {
  structo::backref_owner<&vm_page::owner, tailq_container<vm_page, &vm_page::memq_link>> pages;
};

void vm_object_insert_page(vm_object &obj, vm_page &page) {
  auto r = obj.pages.attach(obj, page); // links `page` into `obj.pages` *and* sets `page.owner`
  RELOCO_ASSERT(r.has_value(), "page already resident somewhere");
}

void vm_object_remove_page(vm_object &obj, vm_page &page) {
  obj.pages.detach(page); // unlinks `page` *and* clears `page.owner` back to nullptr
}

void vm_object_terminate(vm_object &obj) {
  obj.pages.detach_all(); // clears every still-resident page's `owner` back to nullptr
}

// Reparenting one still-resident page from a shadow object onto the
// object it shadows (e.g. during `vm_object_collapse`) -- walking
// every resident page this way is exactly how FreeBSD's own
// collapse/backing-scan logic reparents a whole object's worth:
void vm_page_reparent(vm_object &shadow, vm_object &backing, vm_page &page) {
  auto r = shadow.pages.migrate_to(backing.pages, backing, page);
  RELOCO_ASSERT(r.has_value(), "backing object already has a page at this key");
}

// Looking an already-resident page up by key, e.g. before deciding
// whether a fault needs to allocate a new page at all (only works
// because `tailq_container` above was swapped for a keyed
// `splay_container<vm_page, &vm_page::offset_link, vm_page_key_of>`
// -- see the `Container` contract below for why `try_find` is
// optional and container-dependent):
vm_page *vm_page_lookup(vm_object &obj, std::uint64_t offset) {
  auto r = obj.pages.try_find(offset);
  return r.has_value() ? &r.value().get() : nullptr;
}

// FreeBSD's vm_object_page_remove(object, start, end): evict just the
// pages whose offset falls in [start, end), leaving the rest resident.
void vm_object_page_remove(vm_object &obj, std::uint64_t start, std::uint64_t end) {
  obj.pages.detach_if([&](vm_page &page) noexcept { return page.offset >= start && page.offset < end; });
}

// A read-only walk that never touches membership or the backref at
// all, e.g. tallying how many resident pages are currently dirty:
std::size_t vm_object_count_dirty(vm_object &obj) {
  std::size_t dirty = 0;
  obj.pages.for_each([&](vm_page &page) noexcept { dirty += page.dirty ? 1 : 0; });
  return dirty;
}
```

`BackrefField` is a pointer-to-member naming the holder's
`backref_ptr<Owner, Cell>` field (e.g. `&vm_page::owner`); `Owner`,
`Cell`, and `Holder` are all deduced from it, so `backref_owner` only
needs two template arguments spelled out: `BackrefField` and
`Container` (`LockT` defaults to `reloco::spin_lock`, matching
`structo::sync::lock_striping`'s own default).

## `Container` contract

A `Container` must be default-constructible and provide:

- `reloco::result<void> insert(Holder &holder) & noexcept;` -- links
  `holder` in. May fail (e.g. `error::already_exists` for a
  keyed/sorted container) without having linked it; `backref_owner`
  leaves the holder's `backref_ptr` untouched in that case.
- `void remove(Holder &holder) & noexcept;` -- unlinks `holder`,
  already known (by the caller) to be currently linked.
- `template <typename F> void clear_and_dispose(F &&fn) & noexcept;`
  -- calls `fn(holder)` once for every currently-linked holder (in
  unspecified order), leaving the container empty once it returns.
- `template <typename F> void for_each(F &&fn) & noexcept;` -- calls
  `fn(holder)` once for every currently-linked holder (in unspecified
  order) without unlinking anything.
- `template <typename Pred, typename Disposer> void remove_if(Pred
  &&pred, Disposer &&disposer) & noexcept;` -- for every
  currently-linked holder for which `pred(holder)` returns `true`,
  unlinks it and calls `disposer(holder)`; holders for which `pred`
  returns `false` stay linked and untouched.
- `[[nodiscard]] bool empty() const & noexcept;`

A `Container` may optionally also provide `template <typename K>
reloco::result<std::reference_wrapper<Holder>> try_find(const K &key)
& noexcept;` for keyed containers (e.g. the splay-tree adapter below)
-- `backref_owner::try_find()` simply forwards to it and its body is
only ever instantiated (so `Container::try_find` is only ever
required to exist) if a caller actually calls it, same as any other
member function template; a walk-everything container with no notion
of a key (e.g. the tailq adapter below) need not provide it at all.

Two example adapters, for the two containers mentioned above (both
just forward to an already-reviewed `reloco` intrusive container --
`backref_owner` itself never needs to know this):

```cpp
// FreeBSD vm_object::memq-style: a plain walk-everything tail queue.
template <typename Holder, auto Hook> struct tailq_container {
  reloco::c_tailq<Holder, Hook> list;

  reloco::result<void> insert(Holder &holder) & noexcept {
    list.push_back(holder);
    return {};
  }
  void remove(Holder &holder) & noexcept { list.remove(holder); }
  template <typename F> void clear_and_dispose(F &&fn) & noexcept {
    while (Holder *h = list.pop_front())
      fn(*h);
  }
  template <typename F> void for_each(F &&fn) & noexcept {
    for (Holder &holder : list)
      fn(holder);
  }
  template <typename Pred, typename Disposer> void remove_if(Pred &&pred, Disposer &&disposer) & noexcept {
    for (auto it = list.begin(); it != list.end();) {
      Holder &holder = *it;
      ++it; // advance past `holder` before possibly unlinking it
      if (pred(holder)) {
        list.remove(holder);
        disposer(holder);
      }
    }
  }
  [[nodiscard]] bool empty() const & noexcept { return list.empty(); }
};

// Keyed-by-offset, e.g. a page cache looking pages up by file offset.
template <typename Holder, reloco::intrusive_splay_tree_hook<Holder> Holder::*Hook, typename KeyOf>
struct splay_container {
  reloco::intrusive_splay_tree<Holder, Hook, KeyOf> tree;

  reloco::result<void> insert(Holder &holder) & noexcept { return tree.try_insert(holder); }
  void remove(Holder &holder) & noexcept { tree.remove(holder); }
  template <typename F> void clear_and_dispose(F &&fn) & noexcept {
    tree.clear_and_dispose(std::forward<F>(fn));
  }
  template <typename F> void for_each(F &&fn) & noexcept {
    for (Holder &holder : tree)
      fn(holder);
  }
  template <typename Pred, typename Disposer> void remove_if(Pred &&pred, Disposer &&disposer) & noexcept {
    for (auto it = tree.begin(); it != tree.end();) {
      if (pred(*it)) {
        Holder &holder = *it;
        it = tree.erase(it);
        disposer(holder);
      } else {
        ++it;
      }
    }
  }
  [[nodiscard]] bool empty() const & noexcept { return tree.empty(); }

  // Optional: only keyed containers need offer this.
  template <typename K> reloco::result<std::reference_wrapper<Holder>> try_find(const K &key) & noexcept {
    return tree.try_find(key);
  }
};
```

## Locking

`backref_owner` holds exactly one lock of its own (`LockT`, default
`reloco::spin_lock`), guarding `Container` membership only -- it is
taken and released around `insert`/`remove`/`clear_and_dispose` calls.
In `attach()`/`detach()` (and their `try_` counterparts) it is never
held nested with a holder's own `Cell` lock (that lock is always
acquired strictly after this one has already been released). There is
therefore no lock-ordering hazard to reason about between the two for
those calls: a concurrent `backref_ptr::lock()`/`try_lock()`/
`read_unlocked()` reader of one specific holder never has to wait on
(or participate in any ordering with) `backref_owner`'s own lock at
all.

`migrate_to()`/`try_migrate_to()` are the one deliberate exception:
they acquire the holder's `Cell` lock *first* and hold it across both
registries' critical sections, releasing it only once the backref has
been reassigned. This closes a race the naive "remove, then insert,
then reset the backref" sequence would otherwise leave open: without
holding the `Cell` lock throughout, a concurrent reader could observe
the holder already unlinked from (or relinked into) a `Container`
while its backref still names the *old* owner, and act on that stale
pointer (e.g. try to detach the holder from the old owner a second
time). Holding the `Cell` lock across the whole call means such a
reader instead blocks (or fails with `error::busy`) for the entire
migration and only ever sees the fully-finished result. This reversed
nesting order cannot deadlock against `attach()`/`detach()`: those
never hold a registry lock while waiting on a `Cell` lock (they always
release the registry lock first), so neither order ever has to
wait on a thread stuck in the other.

This does mean `attach()`/`detach()` are each individually atomic
(membership and the holder's pointer are never observed inconsistent
from *outside* either critical section), but the two critical
sections making up one call are not atomic *with each other*: a
reader could in principle observe a holder briefly linked into the
container with its `backref_ptr` not yet reset to the new owner
(during `attach()`), or already cleared to `nullptr` despite the
holder still being linked (`detach()`, until it finishes). This is the
same trade-off `backref_ptr` itself already documents for its own
`Cell`s (never *incorrect*, only a momentary staleness window), and
matches the FreeBSD/Linux precedent this header is modeled on: both
require the holder to already be otherwise quiesced (e.g. "busied")
by the caller before `attach`/`detach`/reassignment, which is exactly
why `detach_all()` requires no concurrent `attach()`/`detach()`/
`try_attach()`/`try_detach()` call on the same `backref_owner` -- it is
only ever meant for owner teardown, once nothing else can reach the
owner to attach a new holder anymore.

`attach()`/`detach()` block until `backref_owner`'s own lock is
acquired; `try_attach()`/`try_detach()` fail with `error::busy` instead
of blocking. A `backref_owner` destroyed while any holder is still
attached traps via `RELOCO_ASSERT` -- call `detach_all()` first.

## Migrating between owners

`migrate_to(target, new_owner, holder)` moves a holder from this
registry to another `backref_owner` of the same type (e.g. FreeBSD's
`vm_object_collapse`/a fork reparenting a resident page from a shadow
object to the object it shadows, or a copy-on-write fault moving a page
to the child): it unlinks `holder` from this `Container`, links it into
`target`'s, then reassigns its backref to `&new_owner` -- all while
holding `holder`'s own `Cell` lock throughout, so the backref is never
observed `nullptr`, nor pointing at the old owner while already
unlinked from/relinked into a `Container` (see ["Locking"](#locking)
above). Unlike `attach()`, it never asserts the backref was previously
empty, since migrating a holder that already points at *this*
registry's owner is exactly the point.

If `target`'s own `Container::insert` rejects `holder` (e.g. a keyed
container already holding an equivalent key), `holder` is reinserted
back into this registry's `Container` and the backref is left
untouched, as if `migrate_to` had never been called -- the source
registry is restored to its exact prior state rather than left with an
orphaned holder. `try_migrate_to` is the non-blocking counterpart,
rolling back the same way if `holder`'s own `Cell` lock or either
registry's own lock turns out to be busy.

## Bulk operations: walking and selectively evicting holders

Three more operations round out the `vm_object`-style management
surface, all holding `backref_owner`'s own lock for the *entire* call
(keep callbacks cheap, and never call back into the same registry from
one -- `LockT` is not required to be reentrant):

- `for_each(fn)` -- a read-only walk calling `fn(holder)` for every
  currently-attached holder, in `Container`'s own unspecified order.
  Touches neither membership nor any holder's backref (e.g. tallying
  how many resident pages are dirty for `msync`).
- `detach_if(pred)` -- for every currently-attached holder for which
  `pred(holder)` returns `true`, unlinks it from `Container` *and*
  clears its backref back to `nullptr`, exactly like `detach()` would;
  holders for which `pred` returns `false` are left attached and
  untouched (e.g. FreeBSD's `vm_object_page_remove(object, start, end)`
  evicting only the pages whose offset falls within a given range,
  rather than `detach_all()`'s "evict everything").
- `try_find(key)` -- a passthrough to `Container::try_find` (e.g.
  FreeBSD's `vm_page_lookup()` finding an already-resident page by
  offset). Since this is a member function template, its body is only
  ever instantiated if a caller actually calls it, so `Container`
  types with no notion of a key (e.g. the tailq adapter above) need not
  provide `try_find` at all -- see the `Container` contract above.

## See also

- [`structo::backref_ptr`](backref_ptr.md) -- the holder-side pointer
  this registry keeps in lock-step with `Container` membership.
- `reloco::c_tailq`/`reloco::intrusive_splay_tree`/
  `reloco::intrusive_rbtree` -- example containers a `Container`
  adapter might forward to.
