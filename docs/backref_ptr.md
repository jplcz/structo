<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::backref_ptr`

`include/structo/backref_ptr.hpp`

A non-owning, safely-invalidated backpointer -- the "child points back
at its current owner" field, e.g. FreeBSD's `vm_page->object` (the
page's current `vm_object`, cleared under lock when the page is
freed/reassigned) or Linux's `struct page::mapping`.

Unlike [`dmap_ptr`/`slot_map_ptr`](reference.md) (which remap *physical
memory*), `backref_ptr` remaps nothing -- it safely guards a plain `T*`
field so the holder (e.g. a `struct page`) can never observe it
mid-update nor race the owner (e.g. its `vm_object`) clearing/
reassigning it out from under a reader. There is exactly one way to
read or write the pointer: through a `guard` returned by
`lock()`/`try_lock()` -- the `guard` itself is the *proof* that
whatever synchronization `Cell` requires is actually held, since it is
only ever constructible by a successful `lock()`/`try_lock()` call;
there is no API to poke the pointer without one.

## Usage

```cpp
struct vm_object; // the owner

struct vm_page { // the holder: a back-reference to its current owner
  structo::backref_ptr<vm_object> owner; // embedded_mutex_cell<vm_object> by default
};

void vm_object_remove_page(vm_object *obj, vm_page *page) {
  auto g = page->owner.lock();
  if (g.get() == obj)
    g.reset(nullptr); // detach -- safe even if a concurrent reader is mid-try_lock()
}

reloco::result<void> peek_owner_name(vm_page *page) {
  auto g = page->owner.try_lock();
  if (!g)
    return reloco::unexpected(g.error()); // e.g. error::busy
  if (g->get() == nullptr)
    return reloco::unexpected(reloco::error::empty_pointer);
  log_name((*g)->name()); // operator-> / operator* reach through to T directly
  return {};
}
```

## Choosing a `Cell`

`backref_ptr<T, Cell>` never implements its own locking protocol:
`Cell` composes one of `reloco`'s existing Rust-`std::sync`-equivalent
owned-lock types (or, for the `striped_*` variants,
[`structo::sync::lock_striping`](lock_striping.md)), so the same
`lock()`/`try_lock()`/`guard` shape this header exposes inherits those
types' own (already reviewed, already tested) safety properties
verbatim.

| `Cell` | Backs onto | Model |
|---|---|---|
| `embedded_mutex_cell<T, MutexT = reloco::mutex>` (default) | `reloco::guarded_mutex<T *, MutexT>` | Exclusive-only: `Mutex<Option<*mut T>>`. One lock per `backref_ptr` instance. |
| `embedded_rw_cell<T, SharedMutexT = reloco::shared_mutex>` | `reloco::rw_lock<T *, SharedMutexT>` | `RwLock<Option<*mut T>>`: any number of concurrent `shared_lock()` readers, or one exclusive `lock()` writer to reassign/detach. One lock per instance. |
| `embedded_seqlock_cell<T>` | `reloco::guarded_seqlock<T *>` | Lock-free `read_unlocked()` (optimistic, retried internally); `lock()`/`try_lock()` still serialize writers. **Only validates the pointer field itself** -- does not pin `*T`'s lifetime, so only safe to dereference the returned `T*` if paired with an external reclamation scheme (RCU/epoch/hazard pointers) that guarantees `T` outlives the read. |
| `striped_mutex_cell<T, N, LockT = reloco::spin_lock, Tag = T>` | `structo::sync::lock_striping<N, LockT>` | Exclusive-only, like `embedded_mutex_cell`, but the lock itself lives in a shared static table hashed by this `backref_ptr`'s own address instead of inside the instance -- zero lock bytes per instance, for dense arrays (e.g. a `struct page[]`). |
| `striped_rw_cell<T, N, SharedLockT = reloco::shared_mutex, Tag = T>` | `structo::sync::lock_striping<N, SharedLockT>` | `embedded_rw_cell`'s shared-readers/exclusive-writer model, but struck from the same shared static table as `striped_mutex_cell` instead of an embedded lock. |
| `striped_seqlock_cell<T, N, LockT = reloco::spin_lock, Tag = T>` | `structo::sync::lock_striping<N, LockT>` | `embedded_seqlock_cell`'s lock-free `read_unlocked()`, but validated against a shared static table's per-stripe sequence counter instead of an embedded one -- shares both `embedded_seqlock_cell`'s lifetime caveat and `striped_mutex_cell`'s cross-key collision trade-off. |

`embedded_mutex_cell`/`striped_mutex_cell` are the right default for
the common case (one owner at a time, readers and writers equally
rare); reach for `embedded_rw_cell`/`striped_rw_cell` only if
concurrent *readers* are actually expected to be common relative to
reassignment/detach; reach for `embedded_seqlock_cell`/
`striped_seqlock_cell` only once a reclamation scheme is already in
place to make its lifetime caveat sound.

The `striped_*` cells' `Tag` parameter defaults to `T`, giving every
distinct `T` using one of them its own private
[`lock_striping`](lock_striping.md) table; pass the same explicit `Tag`
across several instantiations to have them share one table instead
(mirroring `structo::arch::per_cpu_ptr<Tag, T>`'s own `Tag` role).

## `Cell` contract

A custom `Cell` (e.g. one backed by a kernel's own native lock type, or
an RCU-protected pointer) must provide:

- `using state = /* default-constructible, holds whatever this Cell
  needs (an embedded lock, or nothing at all for an externally-looked-up
  one) */;`
- `class guard { ... };` -- default-inconstructible from outside `Cell`,
  exposing at least `T *get() const noexcept;` and
  `void reset(T *target) noexcept;`.
- `static guard lock(state &s, const void *self) noexcept;` -- `self`
  is the owning `backref_ptr`'s own address, for `Cell`s (like
  `striped_mutex_cell`) that look their lock up externally by hashing
  it; ignored by `Cell`s with their own embedded lock.
- `static reloco::result<guard> try_lock(state &s, const void *self) noexcept;`

A minimal example `Cell` delegating to a caller-supplied
`reloco::spin_lock` embedded directly in `state` (i.e. reimplementing
roughly what `embedded_mutex_cell` already provides, for illustration):

```cpp
template <typename T> struct spin_lock_cell {
  struct state {
    reloco::spin_lock lock;
    T *ptr = nullptr;
  };

  class [[nodiscard]] guard {
  public:
    [[nodiscard]] T *get() const noexcept { return s_->ptr; }
    void reset(T *target) noexcept { s_->ptr = target; }
    ~guard() noexcept { s_->lock.unlock(); }
  private:
    friend struct spin_lock_cell;
    explicit guard(state *s) noexcept : s_(s) {}
    state *s_;
  };

  static guard lock(state &s, const void *) noexcept {
    s.lock.lock();
    return guard(&s);
  }
  static reloco::result<guard> try_lock(state &s, const void *) noexcept {
    if (!s.lock.try_lock())
      return reloco::unexpected(reloco::error::busy);
    return guard(&s);
  }
};
```

A `Cell` that additionally wants to support `shared_lock()`/
`try_shared_lock()` also provides a `read_guard` type and
`static read_guard shared_lock(const state &s, const void *self) noexcept;`/
`static reloco::result<read_guard> try_shared_lock(const state &s, const void *self) noexcept;`
(see `embedded_rw_cell`/`striped_rw_cell`); one that wants
`read_unlocked()` provides
`static T *read_unlocked(const state &s, const void *self) noexcept;`
(see `embedded_seqlock_cell`/`striped_seqlock_cell`). Both are optional
-- `backref_ptr` exposes them as defaulted member function templates so
a `Cell` lacking either is otherwise entirely unaffected.

## See also

- [`structo::sync::lock_striping`](lock_striping.md) -- the shared
  table backing the `striped_*` cells.
- [`structo::backref_owner`](backref_owner.md) -- the owner-side
  registry that keeps a `backref_ptr` in lock-step with whatever
  membership structure (tail queue, splay tree, ...) the owner keeps
  its holders in, and clears every holder's pointer on owner teardown.
- `reloco::guarded_mutex`/`reloco::rw_lock`/`reloco::guarded_seqlock` --
  the embedded-lock types the other cells compose.
