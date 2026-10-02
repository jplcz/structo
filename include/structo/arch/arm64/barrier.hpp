// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file barrier.hpp
 * @brief `structo::arch::arm64::barrier_traits`: full/read/write
 * hardware memory-barrier primitives for ARM64 (AArch64), via `DMB`.
 *
 * See `structo::arch::x86::barrier_traits` (`arch/x86/barrier.hpp`) for
 * the full rationale behind this being a plain, directly-callable,
 * zero-sized static-method struct rather than a type-erased
 * customization point:
 *
 * @code
 * using namespace structo::arch::arm64;
 * // ... populate a shared ring buffer ...
 * barrier_traits::hw_write_barrier(); // publish the writes
 * flag.store(1, std::memory_order_relaxed);
 * @endcode
 *
 * ## Semantics
 *
 * - `hw_full_barrier()` -- orders all prior loads/stores before all
 *   subsequent loads/stores (`DMB SY`).
 * - `hw_read_barrier()` -- orders prior loads before subsequent loads
 *   (`DMB LD`); does not order stores. Unlike ARMv7-A (see
 *   `structo::arch::arm::barrier_traits`), ARMv8's `DMB` has a genuine
 *   load-only `LD` option.
 * - `hw_write_barrier()` -- orders prior stores before subsequent stores
 *   (`DMB ST`); does not order loads.
 *
 * These are *hardware* completion-ordering barriers, distinct from a
 * compiler/language-level ordering constraint -- pair with
 * `std::atomic`/`std::atomic_thread_fence` (or a plain
 * `asm volatile("" ::: "memory")` compiler barrier) for code that also
 * needs to stop the compiler itself from reordering the surrounding
 * non-atomic accesses.
 *
 * Only compiled on a real ARM64 target (`__aarch64__`); on every other
 * host this header is an intentional no-op so it stays
 * header-check-clean cross-compiled from any machine.
 */

#if defined(__aarch64__)

namespace structo::arch::arm64 {

/**
 * @brief Full/read/write hardware memory-barrier primitives for ARM64
 * (AArch64). See the @file-level docs for exact semantics.
 */
struct barrier_traits {
  /** @brief Orders all prior loads/stores before all subsequent loads/stores (`DMB SY`). */
  static void hw_full_barrier() noexcept { asm volatile("dmb sy" ::: "memory"); }

  /** @brief Orders prior loads before subsequent loads (`DMB LD`); does not order stores. */
  static void hw_read_barrier() noexcept { asm volatile("dmb ld" ::: "memory"); }

  /** @brief Orders prior stores before subsequent stores (`DMB ST`); does not order loads. */
  static void hw_write_barrier() noexcept { asm volatile("dmb st" ::: "memory"); }
};

} // namespace structo::arch::arm64

#endif // defined(__aarch64__)
