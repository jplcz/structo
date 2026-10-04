// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file backref_ptr.hpp
 * @brief `structo::backref_ptr<T, Cell>`: a non-owning, safely-invalidated
 * backpointer -- the "child points back at its current owner" field, e.g.
 * FreeBSD's `vm_page->object` (the page's current `vm_object`, cleared
 * under lock when the page is freed/reassigned) or Linux's
 * `struct page::mapping`.
 *
 * Unlike `dmap_ptr`/`slot_map_ptr` (which remap *physical memory*),
 * `backref_ptr` remaps nothing -- it safely guards a plain `T*` field so
 * the holder (e.g. a `struct page`) can never observe it mid-update nor
 * race the owner (e.g. its `vm_object`) clearing/reassigning it out from
 * under a reader. There is exactly one way to read or write the pointer:
 * through a `guard` returned by `lock()`/`try_lock()` -- the `guard`
 * itself is the *proof* that whatever synchronization `Cell` requires is
 * actually held, since it is only ever constructible by a successful
 * `lock()`/`try_lock()` call; there is no API to poke the pointer without
 * one.
 *
 * ## Choosing a `Cell`
 *
 * `backref_ptr<T, Cell>` never implements its own locking protocol:
 * `Cell` composes one of `reloco`'s existing Rust-`std::sync`-equivalent
 * owned-lock types, so the same `lock()`/`try_lock()`/`guard` shape this
 * header exposes inherits those types' own (already reviewed, already
 * tested) safety properties verbatim.
 *
 * | `Cell` | Backs onto | Model |
 * |---|---|---|
 * | `embedded_mutex_cell<T, MutexT = reloco::mutex>` (default) | `reloco::guarded_mutex<T *, MutexT>` | Exclusive-only:
 * `Mutex<Option<*mut T>>`. One lock per `backref_ptr` instance. | | `embedded_rw_cell<T, SharedMutexT =
 * reloco::shared_mutex>` | `reloco::rw_lock<T *, SharedMutexT>` | `RwLock<Option<*mut T>>`: any number of concurrent
 * `shared_lock()` readers, or one exclusive `lock()` writer to reassign/detach. One lock per instance. | |
 * `embedded_seqlock_cell<T>` | `reloco::guarded_seqlock<T *>` | Lock-free `read_unlocked()` (optimistic, retried
 * internally); `lock()`/`try_lock()` still serialize writers. **Only validates the pointer field itself** -- does not
 * pin `*T`'s lifetime, so only safe to dereference the returned `T*` if paired with an external reclamation scheme
 * (RCU/epoch/hazard pointers) that guarantees `T` outlives the read. | | `striped_mutex_cell<T, N, LockT =
 * reloco::spin_lock, Tag = T>` | `structo::sync::lock_striping<N, LockT>` | Exclusive-only, like `embedded_mutex_cell`,
 * but the lock itself lives in a shared static table hashed by this `backref_ptr`'s own address instead of inside the
 * instance -- zero lock bytes per instance, for dense arrays (e.g. a `struct page[]`). | | `striped_rw_cell<T, N,
 * SharedLockT = reloco::shared_mutex, Tag = T>` | `structo::sync::lock_striping<N, SharedLockT>` | `embedded_rw_cell`'s
 * shared-readers/exclusive-writer model, but struck from the same shared static table as `striped_mutex_cell` instead
 * of an embedded lock. | | `striped_seqlock_cell<T, N, LockT = reloco::spin_lock, Tag = T>` |
 * `structo::sync::lock_striping<N, LockT>` | `embedded_seqlock_cell`'s lock-free `read_unlocked()`, but validated
 * against a shared static table's per-stripe sequence counter instead of an embedded one -- shares both
 * `embedded_seqlock_cell`'s lifetime caveat and `striped_mutex_cell`'s cross-key collision trade-off. |
 *
 * `embedded_mutex_cell`/`striped_mutex_cell` are the right default for the
 * common case (one owner at a time, readers and writers equally rare);
 * reach for `embedded_rw_cell` only if concurrent *readers* are actually
 * expected to be common relative to reassignment/detach; reach for
 * `embedded_seqlock_cell` only once a reclamation scheme is already in
 * place to make its lifetime caveat sound.
 *
 * @code
 * struct vm_object; // the owner
 *
 * struct vm_page { // the holder: a back-reference to its current owner
 *   structo::backref_ptr<vm_object> owner; // embedded_mutex_cell<vm_object> by default
 * };
 *
 * void vm_object_remove_page(vm_object *obj, vm_page *page) {
 *   auto g = page->owner.lock();
 *   if (g.get() == obj)
 *     g.reset(nullptr); // detach -- safe even if a concurrent reader is mid-try_lock()
 * }
 *
 * reloco::result<void> peek_owner_name(vm_page *page) {
 *   auto g = page->owner.try_lock();
 *   if (!g)
 *     return reloco::unexpected(g.error()); // e.g. error::busy
 *   if (g->get() == nullptr)
 *     return reloco::unexpected(reloco::error::empty_pointer);
 *   log_name((*g)->name()); // operator-> / operator* reach through to T directly
 *   return {};
 * }
 * @endcode
 *
 * ## `Cell` contract
 *
 * A custom `Cell` (e.g. one backed by a kernel's own native lock type, or
 * an RCU-protected pointer) must provide:
 * - `using state = /``*`` default-constructible, holds whatever this `Cell`
 *   needs (an embedded lock, or nothing at all for an externally-looked-up
 *   one) ``*``/;`
 * - `class guard { ... };` -- default-inconstructible from outside `Cell`,
 *   exposing at least `T *get() const noexcept;` and
 *   `void reset(T *target) noexcept;`.
 * - `static guard lock(state &s, const void *self) noexcept;` -- `self`
 *   is the owning `backref_ptr`'s own address, for `Cell`s (like
 *   `striped_cell`) that look their lock up externally by hashing it;
 *   ignored by `Cell`s with their own embedded lock.
 * - `static reloco::result<guard> try_lock(state &s, const void *self) noexcept;`
 *
 * A minimal example `Cell` delegating to a caller-supplied
 * `reloco::spin_lock` embedded directly in `state` (i.e. reimplementing
 * roughly what `embedded_mutex_cell` already provides, for illustration):
 * @code
 * template <typename T> struct spin_lock_cell {
 *   struct state {
 *     reloco::spin_lock lock;
 *     T *ptr = nullptr;
 *   };
 *
 *   class [[nodiscard]] guard {
 *   public:
 *     [[nodiscard]] T *get() const noexcept { return s_->ptr; }
 *     void reset(T *target) noexcept { s_->ptr = target; }
 *     ~guard() noexcept { s_->lock.unlock(); }
 *   private:
 *     friend struct spin_lock_cell;
 *     explicit guard(state *s) noexcept : s_(s) {}
 *     state *s_;
 *   };
 *
 *   static guard lock(state &s, const void *) noexcept {
 *     s.lock.lock();
 *     return guard(&s);
 *   }
 *   static reloco::result<guard> try_lock(state &s, const void *) noexcept {
 *     if (!s.lock.try_lock())
 *       return reloco::unexpected(reloco::error::busy);
 *     return guard(&s);
 *   }
 * };
 * @endcode
 */

#include "sync/lock_striping.hpp"

#include <cstddef>
#include <cstring>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/guarded_mutex.hpp>
#include <reloco/hint.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/mutex.hpp>
#include <reloco/rw_lock.hpp>
#include <reloco/seqlock.hpp>
#include <reloco/spin_lock.hpp>
#include <type_traits>
#include <utility>

namespace structo {

// ============================================================================
// embedded_mutex_cell: exclusive-only, one reloco::guarded_mutex per instance
// ============================================================================

/** @brief Default `Cell` for `backref_ptr`: an embedded
 * `reloco::guarded_mutex<T *, MutexT>` -- exclusive-only access, matching
 * Rust's `Mutex<Option<*mut T>>`. See the @file-level docs for the full
 * `Cell` comparison table. */
template <typename T, typename MutexT = reloco::mutex> struct embedded_mutex_cell {
  using state = reloco::guarded_mutex<T *, MutexT>;

  /** @brief Proof that `state`'s `MutexT` is held; the only way to read
   * or reassign the guarded pointer. */
  class [[nodiscard]] guard {
  public:
    guard(guard &&) noexcept = default;
    guard &operator=(guard &&) noexcept = default;

    [[nodiscard]] T *get() const noexcept { return *inner_; }
    void reset(T *target) noexcept { *inner_ = target; }
    [[nodiscard]] T *operator->() const noexcept RELOCO_LIFETIMEBOUND { return *inner_; }
    [[nodiscard]] std::add_lvalue_reference_t<T> operator*() const noexcept RELOCO_LIFETIMEBOUND { return **inner_; }

  private:
    friend struct embedded_mutex_cell;
    explicit guard(typename state::guard inner) noexcept : inner_(std::move(inner)) {}
    typename state::guard inner_;
  };

  [[nodiscard]] static guard lock(state &s, const void * /*self*/) noexcept { return guard(s.lock()); }

  [[nodiscard]] static reloco::result<guard> try_lock(state &s, const void * /*self*/) noexcept {
    auto r = s.try_lock();
    if (!r.has_value())
      return reloco::unexpected(r.error());
    return guard(std::move(r.value()));
  }
};

// ============================================================================
// embedded_rw_cell: shared readers + exclusive writer, one reloco::rw_lock
// per instance
// ============================================================================

/** @brief `Cell` for `backref_ptr` built on an embedded
 * `reloco::rw_lock<T *, SharedMutexT>` -- any number of concurrent
 * `shared_lock()` readers, or one exclusive `lock()`/`try_lock()` writer
 * to reassign/detach. See the @file-level docs for when this is (and is
 * not) worth the extra `SharedMutexT` over `embedded_mutex_cell`. */
template <typename T, typename SharedMutexT = reloco::shared_mutex> struct embedded_rw_cell {
  using state = reloco::rw_lock<T *, SharedMutexT>;

  /** @brief Exclusive (write) proof: the only `guard` kind that can
   * `reset()`. */
  class [[nodiscard]] guard {
  public:
    guard(guard &&) noexcept = default;
    guard &operator=(guard &&) noexcept = default;

    [[nodiscard]] T *get() const noexcept { return *inner_; }
    void reset(T *target) noexcept { *inner_ = target; }
    [[nodiscard]] T *operator->() const noexcept RELOCO_LIFETIMEBOUND { return *inner_; }
    [[nodiscard]] std::add_lvalue_reference_t<T> operator*() const noexcept RELOCO_LIFETIMEBOUND { return **inner_; }

  private:
    friend struct embedded_rw_cell;
    explicit guard(typename state::write_guard inner) noexcept : inner_(std::move(inner)) {}
    typename state::write_guard inner_;
  };

  /** @brief Shared (read) proof: `get()` only -- no `reset()`, since any
   * number of these may be concurrently alive. */
  class [[nodiscard]] read_guard {
  public:
    read_guard(read_guard &&) noexcept = default;
    read_guard &operator=(read_guard &&) noexcept = default;

    [[nodiscard]] T *get() const noexcept { return *inner_; }
    [[nodiscard]] T *operator->() const noexcept RELOCO_LIFETIMEBOUND { return *inner_; }
    [[nodiscard]] std::add_lvalue_reference_t<T> operator*() const noexcept RELOCO_LIFETIMEBOUND { return **inner_; }

  private:
    friend struct embedded_rw_cell;
    explicit read_guard(typename state::read_guard inner) noexcept : inner_(std::move(inner)) {}
    typename state::read_guard inner_;
  };

  [[nodiscard]] static guard lock(state &s, const void * /*self*/) noexcept { return guard(s.write()); }

  [[nodiscard]] static reloco::result<guard> try_lock(state &s, const void * /*self*/) noexcept {
    auto r = s.try_write();
    if (!r.has_value())
      return reloco::unexpected(r.error());
    return guard(std::move(r.value()));
  }

  [[nodiscard]] static read_guard shared_lock(state &s, const void * /*self*/) noexcept { return read_guard(s.read()); }

  [[nodiscard]] static reloco::result<read_guard> try_shared_lock(state &s, const void * /*self*/) noexcept {
    auto r = s.try_read();
    if (!r.has_value())
      return reloco::unexpected(r.error());
    return read_guard(std::move(r.value()));
  }
};

// ============================================================================
// embedded_seqlock_cell: lock-free optimistic reads, one reloco::guarded_seqlock
// per instance
// ============================================================================

/** @brief `Cell` for `backref_ptr` built on an embedded
 * `reloco::guarded_seqlock<T *>` -- `read_unlocked()` is lock-free
 * (spins internally on an optimistic retry, never blocks a writer);
 * `lock()`/`try_lock()` still serialize writers against each other. See
 * the @file-level docs' table for this `Cell`'s lifetime caveat --
 * `read_unlocked()` only validates the pointer field itself, never pins
 * `*T`. */
template <typename T> struct embedded_seqlock_cell {
  using state = reloco::guarded_seqlock<T *>;

  /** @brief Exclusive (write) proof. */
  class [[nodiscard]] guard {
  public:
    guard(guard &&) noexcept = default;
    guard &operator=(guard &&) noexcept = default;

    [[nodiscard]] T *get() const noexcept { return *inner_; }
    void reset(T *target) noexcept { *inner_ = target; }
    [[nodiscard]] T *operator->() const noexcept RELOCO_LIFETIMEBOUND { return *inner_; }
    [[nodiscard]] std::add_lvalue_reference_t<T> operator*() const noexcept RELOCO_LIFETIMEBOUND { return **inner_; }

  private:
    friend struct embedded_seqlock_cell;
    explicit guard(typename state::write_guard inner) noexcept : inner_(std::move(inner)) {}
    typename state::write_guard inner_;
  };

  [[nodiscard]] static guard lock(state &s, const void * /*self*/) noexcept { return guard(s.write_lock()); }

  [[nodiscard]] static reloco::result<guard> try_lock(state &s, const void * /*self*/) noexcept {
    auto r = s.try_write_lock();
    if (!r.has_value())
      return reloco::unexpected(r.error());
    return guard(std::move(r.value()));
  }

  /** @brief Lock-free optimistic read: spins internally until an
   * un-torn snapshot of the pointer field is observed, then returns it
   * by value. See the @file-level docs' lifetime caveat before
   * dereferencing the result. */
  [[nodiscard]] static T *read_unlocked(const state &s, const void * /*self*/) noexcept { return s.read(); }
};

// ============================================================================
// striped_cell: exclusive-only, lock looked up in a shared static table
// ============================================================================

/** @brief `Cell` for `backref_ptr` with **zero embedded lock bytes**: the
 * lock is looked up, by hashing this `backref_ptr`'s own address, in a
 * shared `structo::sync::lock_striping<N, LockT>` table private to this
 * `<T, N, LockT, Tag>` instantiation. For dense arrays of backrefs (e.g.
 * one per `struct page`) where embedding a full lock in every instance
 * is not affordable. See `lock_striping.hpp`'s docs for the stripe-count
 * trade-off.
 *
 * `Tag` defaults to `T`, giving every distinct `T` using this `Cell` its
 * own private table; pass the same explicit `Tag` across several
 * `striped_mutex_cell<T1, N, LockT, SharedTag>`/`striped_mutex_cell<T2,
 * N, LockT, SharedTag>` instantiations to have them share one table
 * instead (mirroring `structo::arch::per_cpu_ptr<Tag, T>`'s own `Tag`
 * role). The same `Tag` convention is shared by `striped_rw_cell` and
 * `striped_seqlock_cell` below. */
template <typename T, std::size_t N, typename LockT = reloco::spin_lock, typename Tag = T> struct striped_mutex_cell {
  using state = T *; // Plain pointer: every access is already serialized by
                     // whichever stripe `table()` hands out below.

  /** @brief Proof that this instance's stripe is held. */
  class [[nodiscard]] guard {
  public:
    guard(guard &&) noexcept = default;
    guard &operator=(guard &&) noexcept = default;

    [[nodiscard]] T *get() const noexcept { return *slot_; }
    void reset(T *target) noexcept { *slot_ = target; }
    [[nodiscard]] T *operator->() const noexcept RELOCO_LIFETIMEBOUND { return *slot_; }
    [[nodiscard]] std::add_lvalue_reference_t<T> operator*() const noexcept RELOCO_LIFETIMEBOUND { return **slot_; }

  private:
    friend struct striped_mutex_cell;
    guard(state *slot, typename sync::lock_striping<N, LockT>::guard inner) noexcept
        : slot_(slot), inner_(std::move(inner)) {}
    state *slot_;
    typename sync::lock_striping<N, LockT>::guard inner_;
  };

  [[nodiscard]] static guard lock(state &s, const void *self) noexcept { return guard(&s, table().lock_for(self)); }

  [[nodiscard]] static reloco::result<guard> try_lock(state &s, const void *self) noexcept {
    auto r = table().try_lock_for(self);
    if (!r.has_value())
      return reloco::unexpected(r.error());
    return guard(&s, std::move(r.value()));
  }

private:
  // Function-local static: one shared table per <T, N, LockT, Tag>
  // instantiation, mirroring slot_map_mapper's own busy_table() pattern.
  [[nodiscard]] static sync::lock_striping<N, LockT> &table() noexcept {
    static sync::lock_striping<N, LockT> instance;
    return instance;
  }
};

// ============================================================================
// striped_rw_cell: shared readers + exclusive writer, lock looked up in a
// shared static table
// ============================================================================

/** @brief `Cell` for `backref_ptr` combining `embedded_rw_cell`'s shared-
 * readers/exclusive-writer model with `striped_mutex_cell`'s zero-
 * embedded-lock storage: the `SharedLockT` is looked up, by hashing this
 * `backref_ptr`'s own address, in a shared
 * `structo::sync::lock_striping<N, SharedLockT>` table. `SharedLockT`
 * must additionally support `lock_shared()`/`unlock_shared()`/
 * `try_lock_shared()` (e.g. `reloco::shared_mutex`, the default) -- see
 * `lock_striping.hpp`'s own `LockT` contract. */
template <typename T, std::size_t N, typename SharedLockT = reloco::shared_mutex, typename Tag = T>
struct striped_rw_cell {
  using state = T *;

  /** @brief Exclusive (write) proof: the only `guard` kind that can
   * `reset()`. */
  class [[nodiscard]] guard {
  public:
    guard(guard &&) noexcept = default;
    guard &operator=(guard &&) noexcept = default;

    [[nodiscard]] T *get() const noexcept { return *slot_; }
    void reset(T *target) noexcept { *slot_ = target; }
    [[nodiscard]] T *operator->() const noexcept RELOCO_LIFETIMEBOUND { return *slot_; }
    [[nodiscard]] std::add_lvalue_reference_t<T> operator*() const noexcept RELOCO_LIFETIMEBOUND { return **slot_; }

  private:
    friend struct striped_rw_cell;
    guard(state *slot, typename sync::lock_striping<N, SharedLockT>::guard inner) noexcept
        : slot_(slot), inner_(std::move(inner)) {}
    state *slot_;
    typename sync::lock_striping<N, SharedLockT>::guard inner_;
  };

  /** @brief Shared (read) proof: `get()` only -- no `reset()`, since any
   * number of these may be concurrently alive across every key hashing
   * to the same stripe. */
  class [[nodiscard]] read_guard {
  public:
    read_guard(read_guard &&) noexcept = default;
    read_guard &operator=(read_guard &&) noexcept = default;

    [[nodiscard]] T *get() const noexcept { return *slot_; }
    [[nodiscard]] T *operator->() const noexcept RELOCO_LIFETIMEBOUND { return *slot_; }
    [[nodiscard]] std::add_lvalue_reference_t<T> operator*() const noexcept RELOCO_LIFETIMEBOUND { return **slot_; }

  private:
    friend struct striped_rw_cell;
    read_guard(const state *slot, typename sync::lock_striping<N, SharedLockT>::shared_guard inner) noexcept
        : slot_(slot), inner_(std::move(inner)) {}
    const state *slot_;
    typename sync::lock_striping<N, SharedLockT>::shared_guard inner_;
  };

  [[nodiscard]] static guard lock(state &s, const void *self) noexcept { return guard(&s, table().lock_for(self)); }

  [[nodiscard]] static reloco::result<guard> try_lock(state &s, const void *self) noexcept {
    auto r = table().try_lock_for(self);
    if (!r.has_value())
      return reloco::unexpected(r.error());
    return guard(&s, std::move(r.value()));
  }

  [[nodiscard]] static read_guard shared_lock(const state &s, const void *self) noexcept {
    return read_guard(&s, table().shared_lock_for(self));
  }

  [[nodiscard]] static reloco::result<read_guard> try_shared_lock(const state &s, const void *self) noexcept {
    auto r = table().try_shared_lock_for(self);
    if (!r.has_value())
      return reloco::unexpected(r.error());
    return read_guard(&s, std::move(r.value()));
  }

private:
  [[nodiscard]] static sync::lock_striping<N, SharedLockT> &table() noexcept {
    static sync::lock_striping<N, SharedLockT> instance;
    return instance;
  }
};

// ============================================================================
// striped_seqlock_cell: lock-free optimistic reads via a shared static
// table's per-stripe sequence counter
// ============================================================================

/** @brief `Cell` for `backref_ptr` combining `embedded_seqlock_cell`'s
 * lock-free `read_unlocked()` with `striped_mutex_cell`'s zero-embedded-
 * lock storage: the writer-side `LockT` *and* the sequence counter
 * `read_unlocked()` validates against both live in a shared
 * `structo::sync::lock_striping<N, LockT>` table, keyed by hashing this
 * `backref_ptr`'s own address, rather than embedded per instance.
 *
 * Shares `embedded_seqlock_cell`'s lifetime caveat (see the @file-level
 * table): `read_unlocked()` only validates the pointer field itself, not
 * `*T`'s lifetime. Additionally, because the sequence counter here is
 * shared by every key hashing to the same stripe, a write to a
 * *different*, colliding `backref_ptr` can force a spurious (but never
 * incorrect) retry here -- purely a throughput cost, not a correctness
 * one, exactly like this `Cell`'s lock contention under
 * `lock_for()`/`try_lock_for()`. */
template <typename T, std::size_t N, typename LockT = reloco::spin_lock, typename Tag = T> struct striped_seqlock_cell {
  using state = T *;

  /** @brief Exclusive (write) proof. */
  class [[nodiscard]] guard {
  public:
    guard(guard &&) noexcept = default;
    guard &operator=(guard &&) noexcept = default;

    [[nodiscard]] T *get() const noexcept { return *slot_; }
    void reset(T *target) noexcept { *slot_ = target; }
    [[nodiscard]] T *operator->() const noexcept RELOCO_LIFETIMEBOUND { return *slot_; }
    [[nodiscard]] std::add_lvalue_reference_t<T> operator*() const noexcept RELOCO_LIFETIMEBOUND { return **slot_; }

  private:
    friend struct striped_seqlock_cell;
    guard(state *slot, typename sync::lock_striping<N, LockT>::guard inner) noexcept
        : slot_(slot), inner_(std::move(inner)) {}
    state *slot_;
    typename sync::lock_striping<N, LockT>::guard inner_;
  };

  [[nodiscard]] static guard lock(state &s, const void *self) noexcept { return guard(&s, table().lock_for(self)); }

  [[nodiscard]] static reloco::result<guard> try_lock(state &s, const void *self) noexcept {
    auto r = table().try_lock_for(self);
    if (!r.has_value())
      return reloco::unexpected(r.error());
    return guard(&s, std::move(r.value()));
  }

  /** @brief Lock-free optimistic read: spins until an un-torn snapshot
   * of `s` is observed (validated against `self`'s stripe sequence
   * counter), then returns it. See this `Cell`'s and the @file-level
   * docs' lifetime caveats before dereferencing the result. */
  [[nodiscard]] static T *read_unlocked(const state &s, const void *self) noexcept {
    while (true) {
      const std::uint32_t start = table().sequence_for(self);
      if (start % 2 != 0) {
        reloco::hint::spin_loop(); // A writer is active on this stripe; retry.
        continue;
      }
      // `T *` is trivially copyable; snapshotting it while a writer may
      // concurrently be storing a different value is the same
      // deliberately-accepted, validated-after-the-fact race
      // `reloco::guarded_seqlock::read_tx` documents and `memcpy`s
      // around for the identical reason.
      RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
      T *snapshot;
      std::memcpy(&snapshot, &s, sizeof(T *));
      RELOCO_END_UNSAFE_BUFFER_USAGE
      if (table().validate_for(self, start))
        return snapshot;
      reloco::hint::spin_loop();
    }
  }

private:
  [[nodiscard]] static sync::lock_striping<N, LockT> &table() noexcept {
    static sync::lock_striping<N, LockT> instance;
    return instance;
  }
};

// ============================================================================
// backref_ptr: the pointer-like facade shared by every Cell above
// ============================================================================

/**
 * @brief Non-owning, safely-invalidated backpointer to `T`. See the
 * @file-level docs for the rationale and the full `Cell` comparison
 * table.
 *
 * @tparam T Pointed-to type.
 * @tparam Cell Synchronization strategy; defaults to
 * `embedded_mutex_cell<T>`.
 */
template <typename T, typename Cell = embedded_mutex_cell<T>> class backref_ptr {
public:
  using guard = typename Cell::guard;

  constexpr backref_ptr() noexcept = default;

  backref_ptr(const backref_ptr &) = delete;
  backref_ptr &operator=(const backref_ptr &) = delete;

  /**
   * @brief Blocks until exclusive access is acquired, then returns a
   * `guard` that can both `get()` and `reset()` the pointer.
   */
  [[nodiscard]] guard lock() & noexcept { return Cell::lock(state_, this); }

  /** @brief Non-blocking counterpart to `lock()`; fails with
   * `error::busy` if exclusive access is currently held elsewhere. */
  [[nodiscard]] reloco::result<guard> try_lock() & noexcept { return Cell::try_lock(state_, this); }

  /**
   * @brief Shared (read-only) access -- only available when `Cell`
   * provides it (currently `embedded_rw_cell`/`striped_rw_cell`; any
   * number of these may be held concurrently, alongside any number of
   * other `shared_lock()` holders, so long as no `lock()`/`try_lock()`
   * writer is held at the same time).
   *
   * @note A template defaulted to `Cell` (rather than a plain member)
   * purely so its declaration -- `typename C::read_guard` -- is only
   * ever checked against a `Cell` that actually defines `read_guard`/
   * `shared_lock` if this method is actually called; a `Cell` without
   * them (e.g. `embedded_mutex_cell`) is otherwise unaffected, exactly
   * as if this method did not exist for it.
   */
  template <typename C = Cell> [[nodiscard]] typename C::read_guard shared_lock() & noexcept {
    return C::shared_lock(state_, this);
  }

  /** @brief Non-blocking counterpart to `shared_lock()`. See its own
   * doc comment for why this is a defaulted template. */
  template <typename C = Cell> [[nodiscard]] reloco::result<typename C::read_guard> try_shared_lock() & noexcept {
    return C::try_shared_lock(state_, this);
  }

  /**
   * @brief Lock-free optimistic read -- only available when `Cell`
   * provides it (currently `embedded_seqlock_cell`/`striped_seqlock_cell`;
   * see that `Cell`'s docs and the @file-level table for its lifetime
   * caveat before dereferencing the result). See `shared_lock()`'s doc
   * comment for why this is a defaulted template.
   */
  template <typename C = Cell> [[nodiscard]] T *read_unlocked() const & noexcept {
    return C::read_unlocked(state_, this);
  }

private:
  typename Cell::state state_{};
};

} // namespace structo
