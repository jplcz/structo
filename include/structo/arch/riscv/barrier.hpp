// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file barrier.hpp
 * @brief `structo::arch::riscv::barrier_traits`: full/read/write
 * hardware memory-barrier primitives for RISC-V, via `FENCE`.
 *
 * See `structo::arch::x86::barrier_traits` (`arch/x86/barrier.hpp`) for
 * the full rationale behind this being a plain, directly-callable,
 * zero-sized static-method struct rather than a type-erased
 * customization point:
 *
 * @code
 * using namespace structo::arch::riscv;
 * // ... populate a shared ring buffer ...
 * barrier_traits::hw_write_barrier(); // publish the writes
 * flag.store(1, std::memory_order_relaxed);
 * @endcode
 *
 * ## Semantics
 *
 * RISC-V's `FENCE pred, succ` instruction takes explicit predecessor/
 * successor operand sets (any combination of `i`/`o`/`r`/`w`); this
 * header uses the device/memory `r`/`w` bits only, mirroring Linux's
 * `RISCV_FENCE(p, s)` macro convention:
 *
 * - `hw_full_barrier()` -- orders all prior loads/stores before all
 *   subsequent loads/stores (`fence rw, rw`).
 * - `hw_read_barrier()` -- orders prior loads before subsequent loads
 *   (`fence r, r`); does not order stores.
 * - `hw_write_barrier()` -- orders prior stores before subsequent stores
 *   (`fence w, w`); does not order loads.
 *
 * These are *hardware* completion-ordering barriers, distinct from a
 * compiler/language-level ordering constraint -- pair with
 * `std::atomic`/`std::atomic_thread_fence` (or a plain
 * `asm volatile("" ::: "memory")` compiler barrier) for code that also
 * needs to stop the compiler itself from reordering the surrounding
 * non-atomic accesses.
 *
 * Only compiled on a real RISC-V target (`__riscv`); on every other host
 * this header is an intentional no-op so it stays header-check-clean
 * cross-compiled from any machine.
 */

#if defined(__riscv)

namespace structo::arch::riscv {

/**
 * @brief Full/read/write hardware memory-barrier primitives for
 * RISC-V. See the @file-level docs for exact semantics.
 */
struct barrier_traits {
  /** @brief Orders all prior loads/stores before all subsequent loads/stores (`fence rw, rw`). */
  static void hw_full_barrier() noexcept { asm volatile("fence rw, rw" ::: "memory"); }

  /** @brief Orders prior loads before subsequent loads (`fence r, r`); does not order stores. */
  static void hw_read_barrier() noexcept { asm volatile("fence r, r" ::: "memory"); }

  /** @brief Orders prior stores before subsequent stores (`fence w, w`); does not order loads. */
  static void hw_write_barrier() noexcept { asm volatile("fence w, w" ::: "memory"); }
};

} // namespace structo::arch::riscv

#endif // defined(__riscv)
