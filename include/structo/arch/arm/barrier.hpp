// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file barrier.hpp
 * @brief `structo::arch::arm::barrier_traits`: full/read/write hardware
 * memory-barrier primitives for 32-bit ARM (ARMv7-A and earlier), via
 * `DMB`.
 *
 * See `structo::arch::x86::barrier_traits` (`arch/x86/barrier.hpp`) for
 * the full rationale behind this being a plain, directly-callable,
 * zero-sized static-method struct rather than a type-erased
 * customization point:
 *
 * @code
 * using namespace structo::arch::arm;
 * // ... populate a shared ring buffer ...
 * barrier_traits::hw_write_barrier(); // publish the writes
 * flag.store(1, std::memory_order_relaxed);
 * @endcode
 *
 * ## Semantics
 *
 * - `hw_full_barrier()` -- orders all prior loads/stores before all
 *   subsequent loads/stores (`DMB SY`).
 * - `hw_read_barrier()` -- orders prior loads before subsequent loads.
 * - `hw_write_barrier()` -- orders prior stores before subsequent stores
 *   (`DMB ST`).
 *
 * ## No load-only `DMB` option pre-ARMv8
 *
 * `DMB`'s `LD` (load-only) option was only added in ARMv8 (see
 * `structo::arch::arm64::barrier_traits`); ARMv7-A's `DMB` only offers
 * `SY`/`ST`(/inner-shareable-domain variants), with no way to order
 * loads-before-loads alone. `hw_read_barrier()` therefore conservatively
 * falls back to the full `DMB SY` barrier here -- strictly stronger
 * than required, but correct.
 *
 * These are *hardware* completion-ordering barriers, distinct from a
 * compiler/language-level ordering constraint -- pair with
 * `std::atomic`/`std::atomic_thread_fence` (or a plain
 * `asm volatile("" ::: "memory")` compiler barrier) for code that also
 * needs to stop the compiler itself from reordering the surrounding
 * non-atomic accesses.
 *
 * Only compiled on a real 32-bit ARM target (`__arm__`, excluding
 * `__aarch64__`); on every other host this header is an intentional
 * no-op so it stays header-check-clean cross-compiled from any machine.
 */

#if defined(__arm__) && !defined(__aarch64__)

namespace structo::arch::arm {

/**
 * @brief Full/read/write hardware memory-barrier primitives for 32-bit
 * ARM. See the @file-level docs for exact semantics and the pre-ARMv8
 * `DMB LD` caveat.
 */
struct barrier_traits {
  /** @brief Orders all prior loads/stores before all subsequent loads/stores (`DMB SY`). */
  static void hw_full_barrier() noexcept { asm volatile("dmb sy" ::: "memory"); }

  /**
   * @brief Orders prior loads before subsequent loads. Conservatively a
   * full `DMB SY` -- ARMv7-A's `DMB` has no load-only option (see the
   * @file-level docs).
   */
  static void hw_read_barrier() noexcept { asm volatile("dmb sy" ::: "memory"); }

  /** @brief Orders prior stores before subsequent stores (`DMB ST`). */
  static void hw_write_barrier() noexcept { asm volatile("dmb st" ::: "memory"); }
};

} // namespace structo::arch::arm

#endif // defined(__arm__) && !defined(__aarch64__)
