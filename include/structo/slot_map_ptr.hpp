// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file slot_map_ptr.hpp
 * @brief `structo::slot_map_ptr<T, Mapper, PhysInt>` plus two ready-made
 * `Mapper` policies, `structo::slot_map_mapper<...>` and
 * `structo::shared_slot_map_mapper<...>`: the dynamically-remapped
 * counterpart to `dmap_ptr`/`dmap_mapper` (`phys_addr.hpp`), for targets
 * where a permanent, whole-range direct map is not available or not
 * desirable -- e.g. a 32-bit system with far more physical memory than
 * spare virtual address space, or a TEE/secure-world image that must only
 * ever expose a small, on-demand window into Non-secure (REE) RAM rather
 * than mapping the entire Normal World address space into Secure World
 * page tables.
 *
 * Where `dmap_ptr::try_get()` returns a raw `T*` that stays valid for as
 * long as the (permanent) direct map itself is valid, `slot_map_ptr` has no
 * persistent virtual address at all: `try_map()` borrows one of a small,
 * fixed pool of virtual "slots", programs it (via `Mapper`) to point at
 * this pointer's physical address, and returns an RAII `slot_map_ptr::guard`
 * -- the only way to obtain a `T*`. The slot is released (unmapped,
 * TLB-invalidated) automatically when the guard is destroyed, so the
 * mapping's lifetime can never outlive the scope that requested it --
 * mirroring Linux's `kmap_atomic()`/`kunmap_atomic()` or FreeBSD's
 * `pmap_quick_enter_page()`/`pmap_quick_remove_page()`, expressed as a
 * move-only C++ RAII handle instead of a pair of free functions the caller
 * must remember to pair up correctly.
 *
 * ## `Mapper` contract
 *
 * `slot_map_ptr<T, Mapper, PhysInt>` requires `Mapper` to provide:
 * - `using space_tag = /``*`` a `phys_addr` SpaceTag ``*``/;`
 * - `struct mapped_slot { std::size_t index; void *vaddr; };`
 * - `template <typename T> static bool validate_phys(phys_addr<T, space_tag, PhysInt> p) noexcept;`
 * - `static result<mapped_slot> acquire(phys_addr<void, space_tag, PhysInt> phys, std::size_t size) noexcept;`
 * - `static void release(std::size_t slot) noexcept;`
 *
 * `structo::slot_map_mapper<SlotCount, ArchHooks, ExpectedSpace, PhysInt>` is
 * a ready-made `Mapper`: it implements the slot bookkeeping (plain,
 * non-atomic acquire/release over a fixed-size pool of `SlotCount` slots --
 * sound only for strictly per-CPU use with IRQs disabled, see its own docs)
 * on top of an architecture-supplied `ArchHooks` policy, which must provide:
 * - `static constexpr std::size_t slot_size;` (bytes per slot)
 * - `static void *slot_base(std::size_t slot) noexcept;` (fixed VA of a slot)
 * - `static result<void> program(std::size_t slot, PhysInt phys_aligned) noexcept;`
 *   (points `slot`'s fixed VA window at the `slot_size`-aligned physical
 *   page `phys_aligned`, performing whatever page-table write and TLB
 *   shootdown the architecture requires)
 * - `static void unprogram(std::size_t slot) noexcept;` (tears the mapping
 *   back down; must also be safe to call on an already-torn-down slot)
 *
 * Each slot maps exactly **one** `slot_size`-aligned physical page at a
 * time: `acquire()` rejects any `[phys, phys + size)` request that would
 * cross a `slot_size` boundary (`error::out_of_range`) rather than ever
 * spanning a request across two slots. So when pairing `slot_map_mapper`
 * with `compat_sg.hpp`'s codecs, `ArchHooks::slot_size` must match the
 * same `PageTraits::page_size` the codec is instantiated with (e.g.
 * `page_4k`) -- the caller picks both, and they must agree -- not some
 * larger, multi-page window.
 *
 * @code
 * // Secure-world example: a handful of fixed VA windows used to peek into
 * // Non-secure (REE) RAM one page at a time, without ever mapping the
 * // entire NS address space into the Secure page tables.
 * struct ns_peek_hooks {
 *   static constexpr std::size_t slot_size = 4096;
 *
 *   static void *slot_base(std::size_t slot) noexcept {
 *     return reinterpret_cast<void *>(NS_PEEK_WINDOW_BASE + slot * slot_size);
 *   }
 *   static reloco::result<void> program(std::size_t slot, std::uint64_t phys_aligned) noexcept {
 *     return arch_map_ns_page(slot_base(slot), phys_aligned); // arch-specific PTE write + TLBI
 *   }
 *   static void unprogram(std::size_t slot) noexcept { arch_unmap_page(slot_base(slot)); }
 * };
 *
 * using ns_peek_mapper = structo::slot_map_mapper<4, ns_peek_hooks, structo::nonsecure_phys_space>;
 *
 * auto ptr = structo::slot_map_ptr<uint32_t, ns_peek_mapper>::from_paddr(ns_phys);
 * if (ptr) {
 *   auto guard = (*ptr).try_map(); // maps one of the 4 slots for this scope only
 *   if (guard) {
 *     uint32_t value = **guard;
 *   } // slot is unmapped here, even on early return/exception
 * }
 * @endcode
 */

#include "fixed_bitmap.hpp"
#include "phys_addr.hpp"

#include <cstddef>
#include <cstdint>
#include <reloco/detail/assert.hpp>
#include <reloco/error.hpp>
#include <reloco/flat_hash_map.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/spin_lock.hpp>
#include <type_traits>
#include <utility>

namespace structo {

using namespace reloco;

// ============================================================================
// slot_map_mapper: architecture-agnostic slot-pool bookkeeping over ArchHooks
// ============================================================================

/**
 * @brief Ready-made `Mapper` for `slot_map_ptr`: a fixed-size pool of
 * `SlotCount` virtual-address slots, acquired/released by this class, with
 * the actual page-table programming delegated to `ArchHooks`. See the
 * @file-level docs for `ArchHooks`'s contract.
 *
 * @note Not thread-safe, and deliberately so: the pool's `busy` tracking
 * is a plain (non-atomic) `fixed_bitmap<SlotCount>`. The only sound way to
 * use this `Mapper` is per-CPU, with IRQs disabled around `acquire()`/
 * `release()` -- mirroring Linux's `kmap_atomic()` (strictly CPU-local,
 * preempt/IRQ-off) rather than a globally shared pool. If a slot pool
 * needs to be shared across cores/contexts, use `shared_slot_map_mapper`
 * instead, which is explicitly `Lock`-protected.
 */
template <std::size_t SlotCount, typename ArchHooks, typename ExpectedSpace = default_phys_space,
          typename PhysInt = std::uint64_t>
struct slot_map_mapper {
  static_assert(SlotCount > 0, "slot_map_mapper requires at least one slot");

  using space_tag = ExpectedSpace;

  /** @brief A slot borrowed from the pool, mapped to the requested physical range. */
  struct mapped_slot {
    std::size_t index;
    void *vaddr;
  };

  /** @brief No fixed window to bound-check against; `ArchHooks::program()` is authoritative. */
  template <typename T>
  [[nodiscard]] static bool validate_phys(phys_addr<T, ExpectedSpace, PhysInt> /*p*/) noexcept {
    return true;
  }

  /**
   * @brief Claims a free slot and programs it to cover `[phys, phys + size)`.
   * @return The claimed slot plus the mapped virtual pointer for `phys`
   * (i.e. already adjusted for `phys`'s offset within its aligned slot
   * page), or `error::out_of_range` if `size` cannot fit within one slot,
   * `error::busy` if every slot is currently in use, or `ArchHooks::program`'s
   * own error.
   */
  [[nodiscard]] static result<mapped_slot> acquire(phys_addr<void, ExpectedSpace, PhysInt> phys,
                                                    std::size_t size) noexcept {
    if (size == 0 || size > ArchHooks::slot_size)
      return unexpected(error::out_of_range);

    const PhysInt slot_mask = static_cast<PhysInt>(ArchHooks::slot_size) - 1;
    const PhysInt phys_aligned = static_cast<PhysInt>(phys.value & ~slot_mask);
    const std::size_t page_offset = static_cast<std::size_t>(phys.value & slot_mask);
    if (page_offset + size > ArchHooks::slot_size)
      return unexpected(error::out_of_range); // Crosses a slot-sized boundary.

    auto free_slot = busy_table().lowest_clear();
    if (!free_slot.has_value())
      return unexpected(error::busy);
    const std::size_t i = *free_slot;
    auto res = ArchHooks::program(i, phys_aligned);
    if (!res)
      return unexpected(res.error());
    busy_table().set(i);
    RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
    void *vaddr = static_cast<std::byte *>(ArchHooks::slot_base(i)) + page_offset;
    RELOCO_END_UNSAFE_BUFFER_USAGE
    return mapped_slot{i, vaddr};
  }

  /** @brief Tears down and returns slot `slot` to the free pool. */
  static void release(std::size_t slot) noexcept {
    ArchHooks::unprogram(slot);
    busy_table().clear(slot);
  }

private:
  // Function-local static: avoids a separate out-of-line definition for the
  // pool's storage while still giving every instantiation of this template
  // its own pool (one per distinct <SlotCount, ArchHooks, ExpectedSpace, PhysInt>).
  // Plain (non-atomic) tracking: see the @class-level note above -- this
  // `Mapper` is only sound for per-CPU use with IRQs disabled.
  [[nodiscard]] static fixed_bitmap<SlotCount> &busy_table() noexcept {
    static fixed_bitmap<SlotCount> table{};
    return table;
  }
};

// ============================================================================
// shared_slot_map_mapper: lock-protected, refcounted sharing over ArchHooks
// ============================================================================

/**
 * @brief Default, non-blocking `WaitPolicy` for `shared_slot_map_mapper`:
 * `acquire()` fails immediately with `error::busy` once every row is
 * occupied by a distinct physical page, exactly matching
 * `slot_map_mapper`'s own behavior. See `shared_slot_map_mapper`'s docs
 * for the full `WaitPolicy` contract.
 */
struct slot_map_no_wait_policy {
  template <typename LockT> [[nodiscard]] static result<void> wait(LockT & /*lock*/) noexcept {
    return unexpected(error::busy);
  }
  static void notify_all() noexcept {}
};

/**
 * @brief Ready-made `Mapper` for `slot_map_ptr`, like `slot_map_mapper`, but
 * letting concurrent `acquire()` calls for the *same* physical page share
 * one already-live slot instead of each claiming (or blocking on) a
 * separate one.
 *
 * `slot_map_mapper` treats every `acquire()` as independent: N concurrent
 * callers mapping the exact same hot physical page still consume N
 * distinct slots (and fail with `error::busy` once the pool is exhausted),
 * even though they could all safely read through one shared virtual
 * mapping. `shared_slot_map_mapper<EntryCount, ArchHooks, Lock, WaitPolicy,
 * ...>` instead keeps a `Lock`-protected `reloco::flat_hash_map<PhysInt,
 * entry>` (see `flat_hash_map.hpp`), keyed by the slot-aligned physical
 * address, with a reference count per occupied row: a request for a
 * physical page already resident just bumps that row's refcount and
 * reuses its existing `ArchHooks::slot_base()` pointer, and only claims a
 * fresh `ArchHooks` slot (via `program()`) when the page is not yet mapped
 * by anyone. The slot is only actually torn down (`unprogram()`) once the
 * last sharer releases it.
 *
 * `EntryCount` is supplied by the caller -- distinct from, but no larger
 * than, the number of hardware slots `ArchHooks` itself provides -- since
 * `ArchHooks` here is expected to back a single hardware table spanning
 * an entire fixed VA range (`EntryCount` contiguous `ArchHooks::slot_size`
 * windows), rather than a handful of scattered single-page windows: the
 * hash map's own backing storage is reserved up front, in `try_init()`,
 * to exactly this many rows, and `acquire()` never grows it further.
 *
 * `release()` needs the reverse mapping (which physical page a given
 * slot currently backs) to find its row in `by_phys` again. Rather than
 * shadowing that in a second, separately-maintained array -- which the
 * page table itself already records -- `ArchHooks` must additionally
 * provide:
 * - `static PhysInt phys_of(std::size_t slot) noexcept;` (the
 *   `slot_size`-aligned physical address last passed to `program()` for
 *   `slot`; undefined for a slot that is not currently programmed)
 *
 * This is on top of the `ArchHooks` contract documented at the top of
 * this file for `slot_map_mapper` (`slot_size`, `slot_base()`,
 * `program()`, `unprogram()`); `slot_map_mapper` itself has no need for
 * `phys_of()` and does not call it.
 *
 * `Lock` must provide `lock()`/`unlock()`; it defaults to
 * `reloco::spin_lock`, but any type with that minimal surface -- a
 * kernel's own native spinlock/mutex type included -- works equally well.
 *
 * ## `WaitPolicy`: waiting for a slot, FreeBSD `lock_object`-style
 *
 * Neither this type nor `WaitPolicy` is coupled to any one lock or
 * condition-variable implementation. This mirrors FreeBSD's own
 * `lock_object`/`lock_class` abstraction (`sys/lock.h`): `msleep(9)`/
 * `cv_wait(9)` can block on *any* lock class (`mtx(9)`, `sx(9)`,
 * `rw(9)`, `lockmgr(9)`) because each one's `lock_class` supplies the
 * same small set of lock/unlock operations, not because those primitives
 * share a common base type. `WaitPolicy` plays exactly that role here:
 * it is handed the same, already-locked `Lock &` guarding this mapper's
 * table, and must know how to unlock/relock *that specific type*
 * appropriately -- `shared_slot_map_mapper` itself never needs to.
 *
 * `WaitPolicy` must provide:
 * - `template <typename LockT> static result<void> wait(LockT &lock) noexcept;`
 *   Called by `acquire()`, with `lock` already held, exactly when no free
 *   row was found. Two valid shapes:
 *   1. Return an error (e.g. `error::busy`) without touching `lock` at
 *      all -- `acquire()` propagates it immediately, unlocking `lock`
 *      itself. This is what the default `slot_map_no_wait_policy` does.
 *   2. Atomically release `lock`, block until some `release()` elsewhere
 *      calls `notify_all()` (or a spurious wake occurs), then reacquire
 *      `lock` before returning -- and, having done so, *always* return
 *      success (`{}`): `acquire()` simply re-scans for a free row
 *      afterward regardless of why this particular wake happened, like
 *      any `while (!pred()) cv_wait(...)`-style loop already must
 *      tolerate.
 * - `static void notify_all() noexcept;` Called by `release()`, while
 *   still holding `lock`, immediately after freeing a row -- wakes any
 *   context currently parked in `wait()`.
 *
 * Satisfying this contract correctly (choosing the right wait channel,
 * avoiding missed wakeups, unlocking/relocking `Lock` soundly) is
 * entirely the caller-supplied `WaitPolicy`'s responsibility, exactly as
 * it is the caller's responsibility to pick a `Lock` appropriate to the
 * context `acquire()`/`release()` run in (e.g. never a blocking policy
 * from an interrupt handler) -- `shared_slot_map_mapper` only ever calls
 * `wait()`/`notify_all()` at the two well-defined points documented
 * above and otherwise stays out of the way.
 *
 * ## Initialization
 *
 * Unlike `slot_map_mapper` (whose pool is a plain, always-ready
 * `std::atomic<bool>` array), `shared_slot_map_mapper` owns a
 * heap-allocated hash table and therefore cannot finish constructing
 * itself as a side effect of first use -- the caller is responsible for
 * calling `try_init()` exactly once (e.g. at boot, before any other core
 * can reach `acquire()`) before this `Mapper` is used; `acquire()`/
 * `release()` assume this has already happened and do not perform any
 * lazy/implicit allocation themselves.
 *
 * @code
 * using ns_peek_mapper = structo::shared_slot_map_mapper<64, ns_peek_hooks, structo::nonsecure_phys_space>;
 * if (!ns_peek_mapper::try_init()) { / *``handle allocation failure``* / }
 *
 * auto ptr = structo::slot_map_ptr<uint32_t, ns_peek_mapper>::from_paddr(ns_phys);
 * if (ptr) {
 *   auto guard_a = (*ptr).try_map(); // claims (or shares) a row for ns_phys's page
 *   auto guard_b = (*ptr).try_map(); // same page: shares guard_a's row/slot, refcount == 2
 * } // both guards release; the slot is torn down only once the second one does
 * @endcode
 */
template <std::size_t EntryCount, typename ArchHooks, typename Lock = reloco::spin_lock,
          typename WaitPolicy = slot_map_no_wait_policy, typename ExpectedSpace = default_phys_space,
          typename PhysInt = std::uint64_t>
struct shared_slot_map_mapper {
  static_assert(EntryCount > 0, "shared_slot_map_mapper requires at least one entry");

  using space_tag = ExpectedSpace;

  /** @brief A slot borrowed from the pool, mapped to the requested physical range. */
  struct mapped_slot {
    std::size_t index;
    void *vaddr;
  };

  /** @brief No fixed window to bound-check against; `ArchHooks::program()` is authoritative. */
  template <typename T>
  [[nodiscard]] static bool validate_phys(phys_addr<T, ExpectedSpace, PhysInt> /*p*/) noexcept {
    return true;
  }

  /**
   * @brief Allocates and reserves the backing hash table's `EntryCount`
   * rows. Must be called exactly once before any `acquire()`/`release()`
   * call; idempotent (a second call is a no-op returning success) so long
   * as the first call succeeded.
   */
  [[nodiscard]] static result<void> try_init(allocator_ref alloc = default_allocator()) noexcept {
    locked_state &ls = shared();
    ls.lock.lock();
    if (ls.data.by_phys.capacity() > 0) {
      ls.lock.unlock();
      return {};
    }
    auto created = flat_hash_map<PhysInt, entry>::try_allocate(alloc);
    if (!created) {
      ls.lock.unlock();
      return unexpected(created.error());
    }
    ls.data.by_phys = std::move(*created);
    auto reserve_res = ls.data.by_phys.try_reserve(EntryCount);
    ls.lock.unlock();
    return reserve_res;
  }

  /**
   * @brief Shares an already-live slot mapping `phys`'s slot-aligned page
   * if one exists (bumping its refcount), otherwise claims a free slot
   * and programs it -- blocking via `WaitPolicy::wait()` if none is free
   * and `WaitPolicy` says to.
   * @return The (possibly shared) slot plus the mapped virtual pointer for
   * `phys`, or `error::out_of_range` if `size` cannot fit within one slot,
   * whatever `WaitPolicy::wait()` fails with once every row is occupied
   * by a distinct physical page, or `ArchHooks::program`'s own error.
   */
  [[nodiscard]] static result<mapped_slot> acquire(phys_addr<void, ExpectedSpace, PhysInt> phys,
                                                    std::size_t size) noexcept {
    if (size == 0 || size > ArchHooks::slot_size)
      return unexpected(error::out_of_range);

    const PhysInt slot_mask = static_cast<PhysInt>(ArchHooks::slot_size) - 1;
    const PhysInt phys_aligned = static_cast<PhysInt>(phys.value & ~slot_mask);
    const std::size_t page_offset = static_cast<std::size_t>(phys.value & slot_mask);
    if (page_offset + size > ArchHooks::slot_size)
      return unexpected(error::out_of_range); // Crosses a slot-sized boundary.

    locked_state &ls = shared();
    ls.lock.lock();
    RELOCO_ASSERT(ls.data.by_phys.capacity() > 0,
                  "shared_slot_map_mapper::acquire() called before try_init() succeeded");

    for (;;) {
      auto found = ls.data.by_phys.try_at(phys_aligned);
      if (found) {
        entry &e = found->get();
        ++e.refcount;
        RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
        void *vaddr = static_cast<std::byte *>(ArchHooks::slot_base(e.slot)) + page_offset;
        RELOCO_END_UNSAFE_BUFFER_USAGE
        ls.lock.unlock();
        return mapped_slot{e.slot, vaddr};
      }

      auto free_slot_opt = ls.data.busy.lowest_clear_from(ls.data.free_hint);
      if (!free_slot_opt.has_value())
        free_slot_opt = ls.data.busy.lowest_clear(); // hint overshot the end; wrap around and rescan from 0.

      if (!free_slot_opt.has_value()) {
        // WaitPolicy::wait() either fails without touching ls.lock (the
        // default policy) -- in which case it is still held here and must
        // be unlocked before returning -- or unlocks/blocks/relocks it
        // around a successful wait, leaving it held either way.
        auto wait_res = WaitPolicy::wait(ls.lock);
        if (!wait_res) {
          ls.lock.unlock();
          return unexpected(wait_res.error());
        }
        continue; // Re-scan: the set of free rows may have changed.
      }
      const std::size_t free_slot = *free_slot_opt;

      auto prog_res = ArchHooks::program(free_slot, phys_aligned);
      if (!prog_res) {
        ls.lock.unlock();
        return unexpected(prog_res.error());
      }

      auto ins_res = ls.data.by_phys.try_insert(phys_aligned, entry{free_slot, 1});
      if (!ins_res) {
        ArchHooks::unprogram(free_slot);
        ls.lock.unlock();
        return unexpected(ins_res.error());
      }
      ls.data.busy.set(free_slot);
      ls.data.free_hint = free_slot + 1; // Next acquire() resumes just past this one.

      RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
      void *vaddr = static_cast<std::byte *>(ArchHooks::slot_base(free_slot)) + page_offset;
      RELOCO_END_UNSAFE_BUFFER_USAGE
      ls.lock.unlock();
      return mapped_slot{free_slot, vaddr};
    }
  }

  /**
   * @brief Drops one reference to `slot`'s current mapping; tears it down
   * (`ArchHooks::unprogram()`) and frees the row only once the last
   * sharer has released it, then wakes any `WaitPolicy::wait()` waiter.
   */
  static void release(std::size_t slot) noexcept {
    locked_state &ls = shared();
    ls.lock.lock();
    // Must be read before unprogram() below, which may tear down whatever
    // page-table state ArchHooks::phys_of() relies on.
    const PhysInt phys_aligned = ArchHooks::phys_of(slot);
    auto found = ls.data.by_phys.try_at(phys_aligned);
    RELOCO_ASSERT(found.has_value(), "shared_slot_map_mapper::release: slot not currently mapped");
    entry &e = found->get();
    RELOCO_ASSERT(e.refcount > 0, "shared_slot_map_mapper::release: refcount underflow");
    bool freed = false;
    if (--e.refcount == 0) {
      static_cast<void>(ls.data.by_phys.try_remove(phys_aligned));
      ArchHooks::unprogram(slot);
      ls.data.busy.clear(slot);
      if (slot < ls.data.free_hint)
        ls.data.free_hint = slot; // Prefer reusing the lowest now-free slot next.
      freed = true;
    }
    if (freed)
      WaitPolicy::notify_all();
    ls.lock.unlock();
  }

private:
  /** @brief A row: which hardware slot backs this physical page, and how many live guards share it. */
  struct entry {
    std::size_t slot;
    std::size_t refcount;
  };

  struct state {
    flat_hash_map<PhysInt, entry> by_phys{};
    fixed_bitmap<EntryCount> busy{};
    // Lowest slot index not yet known to be busy as of the last scan --
    // `acquire()` resumes its search here instead of always rescanning
    // from 0, and `release()` pulls it back down whenever it frees a
    // lower-indexed slot, so the next acquire() reuses that slot right
    // away instead of continuing to skip over it.
    std::size_t free_hint = 0;
  };

  /** @brief `Lock` paired directly with the data it guards, so `WaitPolicy::wait()` can unlock/relock it by reference. */
  struct locked_state {
    Lock lock;
    state data;
  };

  // Function-local static: avoids a separate out-of-line definition while
  // still giving every instantiation of this template its own state (one
  // per distinct <EntryCount, ArchHooks, Lock, WaitPolicy, ExpectedSpace, PhysInt>).
  [[nodiscard]] static locked_state &shared() noexcept {
    static locked_state instance;
    return instance;
  }
};

// ============================================================================
// Smart Pointer for Scoped Dynamically-Remapped Access
// ============================================================================

/**
 * @brief Pointer-like, dynamically-remapped view of a physical address.
 *
 * Unlike `dmap_ptr`, this type never exposes a `T*` with unbounded
 * lifetime: `try_map()` returns an RAII `guard` that owns the underlying
 * slot for its own lifetime only. See the @file-level docs for the
 * rationale and `Mapper` contract.
 *
 * @tparam T Pointed-to type.
 * @tparam Mapper Dynamic slot-mapping policy (see `slot_map_mapper`).
 * @tparam PhysInt Physical-address integer type.
 */
template <typename T, typename Mapper, typename PhysInt = std::uint64_t> class slot_map_ptr {
public:
  using space_tag = typename Mapper::space_tag;
  using phys_type = phys_addr<T, space_tag, PhysInt>;

  constexpr slot_map_ptr() noexcept = default;
  constexpr slot_map_ptr(std::nullptr_t) noexcept {}

  [[nodiscard]] constexpr phys_type phys() const noexcept { return paddr_; }
  [[nodiscard]] constexpr bool is_null() const noexcept { return paddr_.is_null(); }
  constexpr explicit operator bool() const noexcept { return !paddr_.is_null(); }

  [[nodiscard]] static result<slot_map_ptr> from_paddr(phys_type phys) noexcept {
    if (phys.is_null()) {
      return slot_map_ptr{};
    }
    if (!Mapper::validate_phys(phys))
      return unexpected(error::security_violation);
    return slot_map_ptr(phys);
  }

  /**
   * @brief RAII handle to a live, dynamically-remapped slot.
   *
   * Move-only: exactly one `guard` owns a given slot at a time. The slot
   * is unmapped on destruction, on an explicit `reset()`, or when
   * move-assigned/moved-from (the moved-from guard becomes empty and its
   * destructor becomes a no-op).
   */
  class [[nodiscard]] RELOCO_POINTER guard {
  public:
    constexpr guard() noexcept = default;
    ~guard() noexcept { reset(); }

    guard(const guard &) = delete;
    guard &operator=(const guard &) = delete;

    guard(guard &&other) noexcept
        : m_slot(std::exchange(other.m_slot, invalid_slot)), m_ptr(std::exchange(other.m_ptr, nullptr)) {}

    guard &operator=(guard &&other) noexcept {
      if (this != &other) {
        reset();
        m_slot = std::exchange(other.m_slot, invalid_slot);
        m_ptr = std::exchange(other.m_ptr, nullptr);
      }
      return *this;
    }

    [[nodiscard]] constexpr bool is_null() const noexcept { return m_slot == invalid_slot; }
    constexpr explicit operator bool() const noexcept { return m_slot != invalid_slot; }

    [[nodiscard]] T *get() const noexcept RELOCO_LIFETIMEBOUND { return m_ptr; }
    [[nodiscard]] T *operator->() const noexcept RELOCO_LIFETIMEBOUND { return m_ptr; }
    // `std::add_lvalue_reference_t<T>` (not plain `T&`) so this declaration stays
    // well-formed for `T = void` (yielding `void`); the body is only instantiated
    // if actually called, mirroring `std::shared_ptr<void>::operator*()`.
    [[nodiscard]] std::add_lvalue_reference_t<T> operator*() const noexcept RELOCO_LIFETIMEBOUND { return *m_ptr; }

    /** @brief Early explicit unmap before scope exit. Idempotent. */
    void reset() noexcept {
      if (m_slot != invalid_slot) {
        Mapper::release(m_slot);
        m_slot = invalid_slot;
        m_ptr = nullptr;
      }
    }

  private:
    friend class slot_map_ptr;
    guard(std::size_t slot, T *ptr) noexcept : m_slot(slot), m_ptr(ptr) {}

    static constexpr std::size_t invalid_slot = ~std::size_t(0);
    std::size_t m_slot{invalid_slot};
    T *m_ptr{nullptr};
  };

  /**
   * @brief Borrows a free slot and maps it to `[phys(), phys() + size)` for
   * the lifetime of the returned `guard`.
   * @param size Byte span to map, starting at `phys()`. Must fit within a
   * single `Mapper` slot.
   */
  [[nodiscard]] result<guard> try_map(std::size_t size) const noexcept {
    if (is_null())
      return unexpected(error::invalid_argument);
    auto res = Mapper::acquire(paddr_.template cast_type<void>(), size);
    if (!res.has_value())
      return unexpected(res.error());
    return guard(res.value().index, static_cast<T *>(res.value().vaddr));
  }

  /** @brief `try_map(std::size_t)` overload defaulting `size` to `sizeof(T)`. */
  template <typename U = T> [[nodiscard]] result<guard> try_map() const noexcept {
    static_assert(!std::is_void_v<U>, "slot_map_ptr<void, ...>::try_map() requires an explicit size argument");
    return try_map(sizeof(U));
  }

private:
  constexpr explicit slot_map_ptr(phys_type v) : paddr_(v) {}

  phys_type paddr_{nullptr}; // Automatically uses the ~0 initialization
};

namespace detail {
// Never instantiated beyond this type-only probe: slot_map_mapper's
// space_tag alias (the only thing the static_assert below needs) does not
// require ArchHooks to have any members, since none of its methods are
// odr-used here.
struct slot_map_zero_overhead_probe_hooks {};
} // namespace detail

static_assert(sizeof(slot_map_ptr<void, slot_map_mapper<1, detail::slot_map_zero_overhead_probe_hooks>>) ==
                  sizeof(std::uint64_t),
              "slot_map_ptr must have zero overhead");

} // namespace structo
