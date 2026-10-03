// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file cpu_online_dispatcher.hpp
 * @brief `structo::arch::cpu_online_dispatcher<Tag, MaxCpus>`: a
 * lock-free-for-readers, seqlock-guarded tracker of "which CPUs are
 * currently online", built directly on `reloco::guarded_seqlock<cpu_mask<
 * Tag, MaxCpus>, reloco::spin_lock>`.
 *
 * Any code that targets a *subset* of CPUs computed from "whoever is
 * online right now" (the prototypical example being an IPI dispatcher
 * deciding who to send a broadcast/shootdown to) has a hotplug race to
 * solve: a core can go offline in between the moment the target mask is
 * computed and the moment it's actually used, and a plain, torn read of
 * a multi-word `cpu_mask` racing a concurrent hotplug update could
 * observe a mix of before- and after-update state. This header does not
 * reinvent that synchronization primitive: `reloco::guarded_seqlock<T,
 * MutexT>` (see `guarded_seqlock.hpp`) already *is* exactly Rust's would-
 * be `SeqLock<T>` -- a `reloco::guarded_mutex`-style RAII `write_guard`
 * for mutually-exclusive writers plus a lock-free, typestate-checked
 * `read_tx`/`read()` for readers -- so `cpu_online_dispatcher` is a thin,
 * domain-flavored facade over `guarded_seqlock<cpu_mask<Tag, MaxCpus>,
 * reloco::spin_lock>`, renaming its generic `write_lock()`/`read()` to
 * the CPU-hotplug vocabulary (`mark_online`/`mark_offline`/
 * `with_online_mask`) instead of re-deriving the seqlock protocol itself.
 *
 * `reloco::spin_lock` (not `reloco::mutex`) is `guarded_seqlock`'s
 * `MutexT` here because hotplug's writer side -- a CPU announcing it just
 * came online, or an offlining core's teardown path -- is exactly the
 * kind of "no OS thread to park, possibly before a scheduler even
 * exists" context `spin_lock.hpp`'s own docs describe.
 *
 * ## Reader protocol: lock-free, retries internally
 *
 * `with_online_mask()` hands a guaranteed-consistent `const cpu_mask<Tag,
 * MaxCpus> &` snapshot to a caller-supplied callback, spinning
 * internally (via `guarded_seqlock::read()`) for as long as a concurrent
 * writer keeps racing the read -- a reader never blocks *on* the
 * `spin_lock` itself, only ever retries its own lock-free read:
 *
 * @code
 * structo::arch::cpu_online_dispatcher<my_cpu_tag, 128> online;
 *
 * online.with_online_mask([](const auto &mask) {
 *   send_ipis_to(mask); // guaranteed internally consistent
 * });
 * @endcode
 *
 * For callers that need the lower-level, manual acquire/verify/extract
 * steps directly (e.g. to avoid a redundant copy, or to interleave other
 * work between them), `begin_read()` hands out `guarded_seqlock`'s own
 * `read_tx` typestate object unchanged -- see `seqlock.hpp` for its full
 * `verify()`/`extract()` contract.
 *
 * ## Writer protocol: hotplug events are mutually exclusive, not lock-free
 *
 * `mark_online`/`mark_offline` each take a `write_guard` (via
 * `guarded_seqlock::write_lock()`), mutate the one bit for the affected
 * CPU through it, and let the guard's destructor commit the sequence
 * bump and release the `spin_lock` -- the Rust-`MutexGuard`-flavored RAII
 * `guarded_mutex.hpp`/`seqlock.hpp` already establish elsewhere in this
 * ecosystem, rather than a hand-rolled `lock()`/`unlock()` pair a caller
 * could forget to balance.
 */

#include "cpu_mask.hpp"

#include <reloco/hint.hpp>
#include <reloco/seqlock.hpp>
#include <reloco/spin_lock.hpp>

#include <cstddef>
#include <type_traits>
#include <utility>

namespace structo::arch {

/**
 * @brief Seqlock-guarded "which CPUs are online" tracker over
 * `cpu_mask<Tag, MaxCpus>`; see the @file-level docs above for the full
 * reader/writer protocol.
 * @tparam Tag Phantom `cpu_mask` tag, see `cpu_mask.hpp`.
 * @tparam MaxCpus Number of CPU indices this tracker can represent.
 */
template <typename Tag, std::size_t MaxCpus> class cpu_online_dispatcher {
public:
  using mask_type = cpu_mask<Tag, MaxCpus>;

private:
  using seqlock_type = reloco::guarded_seqlock<mask_type, reloco::spin_lock>;

public:
  /** @brief `reloco::guarded_seqlock`'s own lock-free reader typestate
   * object, for callers that want manual control over the acquire/
   * verify/extract steps instead of `with_online_mask()`'s all-in-one
   * retry loop. See `seqlock.hpp` for its `verify()`/`extract()` contract. */
  using read_tx = typename seqlock_type::read_tx;

  constexpr cpu_online_dispatcher() noexcept = default;

  cpu_online_dispatcher(const cpu_online_dispatcher &) = delete;
  cpu_online_dispatcher &operator=(const cpu_online_dispatcher &) = delete;

  // -------------------------------------------------------------------------
  // Reader side (lock-free; retries internally against a racing writer)
  // -------------------------------------------------------------------------

  /** @brief A guaranteed-consistent snapshot of the online mask,
   * retrying internally for as long as a writer keeps racing the read. */
  [[nodiscard]] mask_type snapshot() const noexcept { return lock_.read(); }

  /** @brief Begins a manual, low-level reader transaction -- see
   * `read_tx`'s own docs (`seqlock.hpp`) for the `verify()`/`extract()`
   * contract this hands out unchanged. */
  [[nodiscard]] read_tx begin_read() const noexcept { return read_tx(lock_); }

  /**
   * @brief Convenience wrapper: hands a guaranteed-consistent `const
   * mask_type &` snapshot to @p compute, retrying the whole read for as
   * long as a concurrent hotplug event keeps racing it.
   * @tparam Compute Invocable as `Compute(const mask_type &)`.
   */
  template <typename Compute>
  [[nodiscard]] auto with_online_mask(Compute &&compute) const
      noexcept(std::is_nothrow_invocable_v<Compute, const mask_type &>)
          -> std::invoke_result_t<Compute, const mask_type &> {
    const mask_type mask = snapshot();
    return compute(static_cast<const mask_type &>(mask));
  }

  /**
   * @brief Two-callback form, following `read_tx`'s own optimistic
   * pattern directly: speculatively extracts a (possibly torn/stale)
   * mask, runs @p preprocess against *that* speculative mask to produce
   * some `U`, and only *then* validates the sequence -- retrying the
   * whole thing (new speculative mask, re-run @p preprocess) if a writer
   * raced it. @p compute only ever runs once, against a mask/`U` pair
   * that's been confirmed consistent.
   *
   * This exists for @p preprocess bodies that need to inspect the mask
   * themselves (e.g. picking targets out of it) and would otherwise
   * have to be redone from scratch after the fact if `with_online_mask`
   * instead re-validated before calling @p preprocess.
   * @tparam Preprocess Invocable as `Preprocess(const mask_type &)`, returning some `U`.
   * @tparam Compute Invocable as `Compute(const mask_type &, U)`.
   */
  template <typename Preprocess, typename Compute>
  [[nodiscard]] auto with_online_mask(Preprocess &&preprocess, Compute &&compute) const
      noexcept(std::is_nothrow_invocable_v<Preprocess, const mask_type &> &&
               std::is_nothrow_invocable_v<
                   Compute, const mask_type &,
                   std::invoke_result_t<Preprocess, const mask_type &>>)
          -> std::invoke_result_t<Compute, const mask_type &,
                                   std::invoke_result_t<Preprocess, const mask_type &>> {
    for (;;) {
      read_tx tx(lock_);
      const mask_type mask = tx.extract(); // try get mask (speculative, may be stale/torn)
      auto preprocessed = preprocess(static_cast<const mask_type &>(mask));
      if (tx.verify()) {
        return compute(static_cast<const mask_type &>(mask), std::move(preprocessed));
      }
      reloco::hint::spin_loop();
    }
  }

  // -------------------------------------------------------------------------
  // Writer side (CPU hotplug: mutually exclusive, not lock-free)
  // -------------------------------------------------------------------------

  /**
   * @brief Marks `cpu` online, committing a strong synchronization point
   * every reader validates against once the returned write guard is
   * destroyed. @p on_online runs *before* the bit is set, still under
   * the exclusive writer lock (serialized against every other
   * concurrent hotplug event) -- e.g. to (re-)initialize `cpu`'s
   * per-CPU IPI state before it becomes a valid send target.
   * @tparam OnOnline Invocable as `OnOnline(std::size_t cpu, const mask_type &mask)`.
   */
  template <typename OnOnline>
  void mark_online(std::size_t cpu, OnOnline &&on_online) noexcept(
      std::is_nothrow_invocable_v<OnOnline, std::size_t, const mask_type &>) {
    auto guard = lock_.write_lock();
    on_online(cpu, static_cast<const mask_type &>(*guard));
    guard->set(cpu);
  }

  /**
   * @brief Marks `cpu` offline, committing a strong synchronization
   * point every reader validates against once the returned write guard
   * is destroyed. The bit is cleared *before* @p on_offline runs, so no
   * reader can observe `cpu` as a valid send target once @p on_offline
   * starts -- still under the exclusive writer lock (serialized against
   * every other concurrent hotplug event) -- e.g. to abort or complete
   * IPIs left stranded in `cpu`'s per-CPU queue.
   * @tparam OnOffline Invocable as `OnOffline(std::size_t cpu, const mask_type &mask)`.
   */
  template <typename OnOffline>
  void mark_offline(std::size_t cpu, OnOffline &&on_offline) noexcept(
      std::is_nothrow_invocable_v<OnOffline, std::size_t, const mask_type &>) {
    auto guard = lock_.write_lock();
    guard->clear(cpu);
    on_offline(cpu, static_cast<const mask_type &>(*guard));
  }

private:
  seqlock_type lock_{};
};

} // namespace structo::arch
