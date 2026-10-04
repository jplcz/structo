// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file lock_striping.hpp
 * @brief `structo::sync::lock_striping<N, LockT>`: a shared, static-sized
 * array of `N` locks indexed by hashing an address, so a large number of
 * protected objects can share a small, fixed pool of locks instead of each
 * embedding its own -- mirroring FreeBSD's `vm_page_lockptr()`/`pa_lock[]`
 * hashed page-lock array (every `struct vm_page` in the system shares one
 * of a handful of global mutexes, chosen by hashing the page's own
 * address, rather than each page carrying a full lock of its own).
 *
 * This is purely a memory-density trade-off versus an embedded lock per
 * instance (`reloco::guarded_mutex<T>`/`reloco::rw_lock<T>`/
 * `reloco::guarded_seqlock<T>`, see `backref_ptr.hpp`'s
 * `embedded_mutex_cell`/`embedded_rw_cell`/`embedded_seqlock_cell`): two
 * unrelated objects that happen to hash to the same stripe will
 * needlessly contend (or, for the seqlock-style reader below, needlessly
 * retry) against each other, so `N` should be picked comfortably larger
 * than the expected number of *concurrently contended* objects (not the
 * total object count) -- exactly the same trade-off FreeBSD's own
 * `PA_LOCK_COUNT` makes.
 *
 * Every stripe carries both a `LockT` (for `lock_for()`/`shared_lock_for()`)
 * *and* a sequence counter bumped around every exclusive critical section
 * -- the same counter `reloco::guarded_seqlock` embeds per protected value,
 * just here shared across whichever objects hash to that stripe. This
 * lets `backref_ptr.hpp`'s `striped_seqlock_cell` build a lock-free
 * optimistic reader (`sequence_for()`/`validate_for()`) out of the exact
 * same table `striped_mutex_cell`/`striped_rw_cell` use for their
 * exclusive/shared access -- callers that never use the seqlock-style
 * reader pay only one `std::atomic<std::uint32_t>` of unused space per
 * stripe (table-wide, not per protected object) and two extra atomic RMW
 * ops per exclusive critical section.
 *
 * `LockT` must provide `lock()`/`unlock()`/`try_lock()` with the same
 * signatures as `reloco::spin_lock` (the default, used by
 * `lock_for()`/`try_lock_for()`); `shared_lock_for()`/`try_shared_lock_for()`
 * additionally require `lock_shared()`/`unlock_shared()`/`try_lock_shared()`
 * (e.g. `reloco::shared_mutex`) -- calling them with a `LockT` that lacks
 * those (e.g. the default `reloco::spin_lock`) simply fails to compile at
 * the call site, exactly as if the methods did not exist.
 *
 * @code
 * // One shared table of 64 spinlocks, chosen by hashing each `vm_page`'s
 * // own address -- no lock lives inside `vm_page` itself.
 * structo::sync::lock_striping<64> page_locks;
 *
 * void detach_page_owner(vm_page *page) {
 *   auto guard = page_locks.lock_for(page); // picks (and locks) one of the 64 stripes
 *   page->owner = nullptr;
 * } // stripe unlocked here
 * @endcode
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <reloco/array.hpp>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/hint.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/spin_lock.hpp>
#include <utility>

namespace structo::sync {

/**
 * @brief Shared, fixed-size array of `N` locks selected by hashing an
 * address. See the @file-level docs for the rationale and `LockT`
 * contract.
 *
 * @tparam N Number of stripes (locks) in the table. Must be positive.
 * @tparam LockT Lock type backing each stripe; defaults to
 * `reloco::spin_lock`.
 */
template <std::size_t N, typename LockT = reloco::spin_lock> class lock_striping {
  static_assert(N > 0, "lock_striping requires at least one stripe");

  struct stripe {
    LockT lock{};
    // Bumped around every exclusive critical section (odd while held,
    // even while free), exactly like `reloco::guarded_seqlock`'s own
    // per-value counter -- shared here across every key hashing to this
    // stripe. Only consulted by `sequence_for()`/`validate_for()`;
    // `lock_for()`/`shared_lock_for()` alone never need it read back.
    std::atomic<std::uint32_t> seq{0};
  };

public:
  constexpr lock_striping() noexcept = default;

  lock_striping(const lock_striping &) = delete;
  lock_striping &operator=(const lock_striping &) = delete;

  /**
   * @brief RAII handle on one exclusively-locked stripe. Move-only;
   * releases the stripe's `LockT` (and bumps its sequence counter back to
   * even) on destruction.
   *
   * @note Not annotated with Clang's Thread Safety Analysis capability
   * macros (unlike `reloco::guarded_mutex`/`reloco::rw_lock`): which
   * stripe this guard holds is a runtime value (the hash of a runtime
   * address), so there is no single, statically-nameable capability for
   * the analysis to track -- exactly the same reason
   * `reloco::guarded_seqlock`'s lock-free reader path opts out of it.
   */
  class [[nodiscard]] guard {
  public:
    constexpr guard() noexcept = default;
    ~guard() noexcept { reset(); }

    guard(const guard &) = delete;
    guard &operator=(const guard &) = delete;

    guard(guard &&other) noexcept : stripe_(std::exchange(other.stripe_, nullptr)) {}

    guard &operator=(guard &&other) noexcept {
      if (this != &other) {
        reset();
        stripe_ = std::exchange(other.stripe_, nullptr);
      }
      return *this;
    }

    /** @brief Early explicit unlock before scope exit. Idempotent. */
    void reset() noexcept {
      if (stripe_ != nullptr) {
        stripe_->seq.fetch_add(1, std::memory_order_release); // Signal readers: back to EVEN.
        stripe_->lock.unlock();
        stripe_ = nullptr;
      }
    }

  private:
    friend class lock_striping;
    explicit guard(stripe *s) noexcept : stripe_(s) {}
    stripe *stripe_{nullptr};
  };

  /**
   * @brief RAII handle on one shared (read)-locked stripe. Move-only;
   * releases the stripe's `LockT` shared half on destruction. Does not
   * touch the sequence counter (shared access never races the
   * seqlock-style reader's own validation, since a `lock_for()` writer
   * cannot be concurrently active while any `shared_guard` is alive).
   */
  class [[nodiscard]] shared_guard {
  public:
    constexpr shared_guard() noexcept = default;
    ~shared_guard() noexcept { reset(); }

    shared_guard(const shared_guard &) = delete;
    shared_guard &operator=(const shared_guard &) = delete;

    shared_guard(shared_guard &&other) noexcept : stripe_(std::exchange(other.stripe_, nullptr)) {}

    shared_guard &operator=(shared_guard &&other) noexcept {
      if (this != &other) {
        reset();
        stripe_ = std::exchange(other.stripe_, nullptr);
      }
      return *this;
    }

    /** @brief Early explicit unlock before scope exit. Idempotent. */
    void reset() noexcept {
      if (stripe_ != nullptr) {
        stripe_->lock.unlock_shared();
        stripe_ = nullptr;
      }
    }

  private:
    friend class lock_striping;
    explicit shared_guard(stripe *s) noexcept : stripe_(s) {}
    stripe *stripe_{nullptr};
  };

  /**
   * @brief Blocks until the stripe `key` hashes to is exclusively
   * acquired.
   * @param key Any address used purely as a hash input -- it is never
   * dereferenced; typically the protected object's own address.
   */
  [[nodiscard]] guard lock_for(const void *key) & noexcept {
    stripe &s = stripe_for(key);
    s.lock.lock();
    s.seq.fetch_add(1, std::memory_order_acquire); // Signal readers: now ODD.
    return guard(&s);
  }

  /** @brief Non-blocking counterpart to `lock_for`; fails with
   * `error::busy` if the stripe is currently held (shared or exclusive)
   * elsewhere. */
  [[nodiscard]] reloco::result<guard> try_lock_for(const void *key) & noexcept {
    stripe &s = stripe_for(key);
    if (!s.lock.try_lock())
      return reloco::unexpected(reloco::error::busy);
    s.seq.fetch_add(1, std::memory_order_acquire);
    return guard(&s);
  }

  /**
   * @brief Blocks until the stripe `key` hashes to is acquired for
   * shared (read) access. Requires `LockT::lock_shared()`.
   */
  [[nodiscard]] shared_guard shared_lock_for(const void *key) & noexcept {
    stripe &s = stripe_for(key);
    s.lock.lock_shared();
    return shared_guard(&s);
  }

  /** @brief Non-blocking counterpart to `shared_lock_for`; fails with
   * `error::busy` if an exclusive lock is currently held elsewhere.
   * Requires `LockT::try_lock_shared()`. */
  [[nodiscard]] reloco::result<shared_guard> try_shared_lock_for(const void *key) & noexcept {
    stripe &s = stripe_for(key);
    if (!s.lock.try_lock_shared())
      return reloco::unexpected(reloco::error::busy);
    return shared_guard(&s);
  }

  /**
   * @brief Snapshots `key`'s stripe sequence counter for a lock-free,
   * seqlock-style optimistic read: odd means a `lock_for()`/
   * `try_lock_for()` writer is currently active on this stripe (for
   * *any* key hashing to it, not necessarily `key` itself).
   */
  [[nodiscard]] std::uint32_t sequence_for(const void *key) const noexcept {
    return stripe_for(key).seq.load(std::memory_order_acquire);
  }

  /**
   * @brief Re-validates a read that started at `start_seq` (as returned
   * by `sequence_for()`): must be called *after* the caller has already
   * copied out whatever data it is optimistically reading. Returns
   * `false` if a writer was active when the read began, or became active
   * before this call -- either way, the caller must discard its snapshot
   * and retry.
   */
  [[nodiscard]] bool validate_for(const void *key, std::uint32_t start_seq) const noexcept {
    if (start_seq % 2 != 0)
      return false; // A writer was already active when the read began.
    std::atomic_thread_fence(std::memory_order_acquire);
    return start_seq == stripe_for(key).seq.load(std::memory_order_relaxed);
  }

private:
  /**
   * @brief Avalanches a pointer's bits before reducing mod `N`.
   *
   * `std::hash<const void*>` is the identity function on both libstdc++
   * and libc++ -- it performs no bit-mixing at all, just a
   * `reinterpret_cast<size_t>`. Heap allocations are always aligned
   * (typically to 16 bytes, sometimes more), so their low, always-zero
   * bits would otherwise dominate `% N` whenever `N` is a power of two
   * (a very likely choice): e.g. with `N <= 16`, *every* 16-byte-aligned
   * pointer would hash to stripe 0, collapsing the whole table down to
   * one lock.
   *
   * The key is first shifted right by 6 bits -- dividing out a typical
   * 64-byte cache-line size -- since those low bits carry no useful
   * entropy for picking a stripe (they are either always zero from
   * alignment, or merely distinguish *offsets within the same cache
   * line*, which should not scatter across stripes anyway). The
   * remaining bits are then avalanched with Murmur3's `fmix32` finalizer
   * rather than a 64-bit mixer such as splitmix64: it needs only 32-bit
   * multiplies, which are a single `MUL` instruction on both AArch32 and
   * AArch64, whereas a 64-bit multiply costs a `UMULL`/`UMULH` pair on
   * AArch32 (and is still pricier than a 32-bit `MUL` on AArch64). The
   * truncation to 32 bits discards only high address bits, which in
   * practice vary far less often (between unrelated mappings) than the
   * mid-range bits retained here.
   */
  [[nodiscard]] static std::size_t mix(const void *key) noexcept {
    auto x = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(key) >> 6);
    x ^= x >> 16;
    x *= 0x85ebca6bU;
    x ^= x >> 13;
    x *= 0xc2b2ae35U;
    x ^= x >> 16;
    return static_cast<std::size_t>(x);
  }

  [[nodiscard]] stripe &stripe_for(const void *key) noexcept { return stripes_[mix(key) % N]; }
  [[nodiscard]] const stripe &stripe_for(const void *key) const noexcept { return stripes_[mix(key) % N]; }

  reloco::array<stripe, N> stripes_{};
};

} // namespace structo::sync
