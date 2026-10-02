// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file barrier.hpp
 * @brief `structo::arch::x86::barrier_traits`: full/read/write hardware
 * memory-barrier primitives for x86/x86-64, via `MFENCE`/`LFENCE`/
 * `SFENCE`.
 *
 * Unlike `io_space_traits`/`hw_rng_traits`, which are customization
 * points consumed through a type-erased `*_ref` handle (the concrete
 * backend genuinely varies at runtime), the backend for a memory
 * barrier is always known at compile time -- there is never a reason to
 * choose one dynamically. `barrier_traits` is therefore a plain,
 * directly-callable, zero-sized static-method struct, used straight
 * from calling code without any erasure layer:
 *
 * @code
 * using namespace structo::arch::x86;
 * // ... populate a shared ring buffer ...
 * barrier_traits::hw_write_barrier(); // publish the writes
 * flag.store(1, std::memory_order_relaxed);
 * @endcode
 *
 * ## Semantics
 *
 * - `hw_full_barrier()` -- orders all prior loads/stores before all
 *   subsequent loads/stores (`MFENCE`).
 * - `hw_read_barrier()` -- orders prior loads before subsequent loads
 *   (`LFENCE`); does not order stores.
 * - `hw_write_barrier()` -- orders prior stores before subsequent stores
 *   (`SFENCE`); does not order loads.
 *
 * These are *hardware* completion-ordering barriers (what a single core
 * observes of its own loads/stores becoming globally visible, and what
 * it is guaranteed to observe of another core's), distinct from a
 * compiler/language-level ordering constraint -- pair with
 * `std::atomic`/`std::atomic_thread_fence` (or a plain `asm volatile("" ::: "memory")`
 * compiler barrier) for code that also needs to stop the compiler itself
 * from reordering the surrounding non-atomic accesses.
 *
 * x86/x86-64's own memory model (TSO, modulo store-buffer forwarding)
 * already orders most same-core load/store pairs without any of these
 * -- they matter chiefly for: ordering accesses around a genuinely
 * non-temporal store (`MOVNT*`), around MMIO where the bus/device
 * ordering is not implied by TSO, or portable code shared with
 * architectures that need an explicit barrier far more often (ARM,
 * RISC-V).
 *
 * Only compiled on a real x86/x86-64 target (`__i386__`/`__x86_64__`);
 * on every other host this header is an intentional no-op so it stays
 * header-check-clean cross-compiled from any machine.
 */

#if defined(__i386__) || defined(__x86_64__)

namespace structo::arch::x86 {

/**
 * @brief Full/read/write hardware memory-barrier primitives for
 * x86/x86-64. See the @file-level docs for exact semantics and the
 * compiler-barrier caveat.
 */
struct barrier_traits {
  /** @brief Orders all prior loads/stores before all subsequent loads/stores (`MFENCE`). */
  static void hw_full_barrier() noexcept { asm volatile("mfence" ::: "memory"); }

  /** @brief Orders prior loads before subsequent loads (`LFENCE`); does not order stores. */
  static void hw_read_barrier() noexcept { asm volatile("lfence" ::: "memory"); }

  /** @brief Orders prior stores before subsequent stores (`SFENCE`); does not order loads. */
  static void hw_write_barrier() noexcept { asm volatile("sfence" ::: "memory"); }
};

} // namespace structo::arch::x86

#endif // defined(__i386__) || defined(__x86_64__)
