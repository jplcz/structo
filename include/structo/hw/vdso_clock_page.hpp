// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file vdso_clock_page.hpp
 * @brief `structo::hw::vdso_clock_page`: the bitness-agnostic, shared-
 * memory structure a kernel publishes a free-running hardware counter
 * through (a TSC/`CNTVCT_EL0`/`CNTPCT_EL0`/RISC-V `mtime` sample
 * correlated with a wall-clock or monotonic reference instant) so user
 * space can compute "what time is it now" without a syscall -- the same
 * role Linux's `vvar`/vDSO data page, FreeBSD's `vdso_timehands`, and
 * Windows' `KUSER_SHARED_DATA` time fields all play for their own
 * platforms.
 *
 * ## Shape: one `vdso_clock_slot` per clock, each its own seqlock
 *
 * `vdso_clock_page` holds one independent @ref vdso_clock_slot per
 * `vdso_clock_id` (`realtime`, `monotonic`) rather than one shared
 * generation counter for both: the two clocks are updated on their own
 * schedules by the kernel (a monotonic clock never needs the leap-
 * second/`settimeofday`-style step a realtime clock does), and sharing
 * one seqlock would force a reader of one clock to retry because of
 * concurrent churn on the *other*. Each slot is `alignas(64)` (a
 * typical cache-line size) so the two slots' independent writer
 * traffic cannot false-share a cache line.
 *
 * ## The seqlock: `generation`, even == stable, odd == update in progress
 *
 * Each slot carries its own `std::atomic<std::uint32_t> generation`,
 * following the same even/odd seqlock convention FreeBSD's `timehands`
 * and Linux's vDSO use: a value is stable (safe to read) exactly when
 * `generation` is even; the writer increments it to an odd value before
 * touching any other field, writes the new snapshot, then increments it
 * again (back to even) once done. A reader loops: read `generation`,
 * read every other field, read `generation` again, and only accepts the
 * read if both reads agree *and* are even -- see `vdso_clock_reader.hpp`
 * for that loop and `vdso_clock_writer.hpp` for the writer side.
 *
 * `generation`/`reference_*` are updated here purely via the seqlock
 * protocol (lock-free from the *reader's* perspective); the *writer*
 * side's own mutual exclusion -- so two concurrent kernel updaters never
 * interleave their odd/even transitions -- is the caller's own
 * responsibility (e.g. a per-slot kernel mutex held around every
 * `vdso_clock_writer`/`vdso_clock_update_guard` call), not something
 * this header or `vdso_clock_writer.hpp` provides; see that header's
 * own docs.
 *
 * ## Why this must stay bitness-agnostic
 *
 * This structure is designed to be mapped at the same physical address
 * into both the kernel's own address space and (read-only -- see below)
 * a 32-bit *or* 64-bit user-space process's address space, so its
 * layout must not depend on the reader/writer's pointer width, `long`
 * width, or enum underlying-type defaults:
 * - every field is a fixed-width integer (`std::uint32_t`/
 *   `std::uint64_t`) or an enum explicitly based on one -- never
 *   `std::size_t`, `long`, a bare `enum class` with an implementation-
 *   defined underlying type, or a pointer;
 * - `std::atomic<std::uint32_t>` is required to be lock-free
 *   (`static_assert`ed below) so the generation counter is a plain
 *   4-byte load/store on every supported target, not a hidden fallback
 *   lock table keyed by address (which could never be shared
 *   consistently across two different address spaces anyway);
 * - every slot's size/alignment is pinned with a `static_assert`, so a
 *   future field addition that would silently change the ABI between a
 *   32-bit and 64-bit build is caught at compile time instead of
 *   corrupting cross-bitness sharing at runtime.
 *
 * ## Read-only from user space
 *
 * The page backing `vdso_clock_page` is intended to be mapped **read-
 * only** into user space (e.g. `PROT_READ` only, never `PROT_WRITE`) --
 * user space only ever *reads* a published snapshot and computes
 * elapsed time against its own counter read (see
 * `vdso_clock_reader.hpp`); it never writes any field, including
 * `generation` itself. This is safe with a plain, lock-free
 * `std::atomic<std::uint32_t>::load`: every mainstream target lowers it
 * to an ordinary load instruction, which works unmodified against a
 * read-only mapping. Only the kernel's own writable mapping of the same
 * physical page (see `vdso_clock_writer.hpp`) ever stores into a slot.
 */

#include <atomic>
#include <cstdint>

#include <reloco/array.hpp>

namespace structo {

namespace hw {

/**
 * @brief Which hardware free-running counter a @ref vdso_clock_slot's
 * `reference_counter` sample was taken from. Purely informational for
 * the writer; the reader's own `Traits::read_counter(source)` hook
 * (see `vdso_clock_reader.hpp`) is what actually knows how to read each
 * one.
 */
enum class vdso_clock_source : std::uint32_t {
  /** @brief No sample has been published yet (the slot's initial state). */
  none = 0,
  /** @brief x86/x86-64 `RDTSC`/`RDTSCP`. */
  x86_tsc,
  /** @brief ARMv8-A/ARMv9-A virtual counter, `CNTVCT_EL0`. */
  arm_cntvct_el0,
  /** @brief ARMv8-A/ARMv9-A physical counter, `CNTPCT_EL0`. */
  arm_cntpct_el0,
  /** @brief RISC-V `rdtime`/`time` CSR. */
  riscv_time,
};

/** @brief Which clock a @ref vdso_clock_slot represents; indexes
 * `vdso_clock_page::slots`. */
enum class vdso_clock_id : std::uint32_t {
  /** @brief Wall-clock time -- `reference_secs`/`reference_subsec_nanos` are a duration since the Unix epoch. */
  realtime = 0,
  /** @brief Monotonic, never-stepped time -- `reference_secs`/`reference_subsec_nanos` are a duration since an
   * arbitrary, kernel-chosen epoch (e.g. boot), meaningful only as a difference between two readings. */
  monotonic = 1,
};

/** @brief Number of clocks `vdso_clock_page` carries; see @ref vdso_clock_id. */
inline constexpr std::uint32_t vdso_clock_id_count = 2;

/**
 * @brief One seqlock-protected clock snapshot: a free-running counter
 * reading (`reference_counter`, at frequency `counter_hz`) correlated
 * with a wall-clock/monotonic instant (`reference_secs`/
 * `reference_subsec_nanos`). See the @file-level docs for the seqlock
 * protocol and bitness-agnostic layout rules.
 */
struct alignas(64) vdso_clock_slot {
  /** @brief Seqlock generation: even == stable (safe to read), odd == a writer is mid-update. Monotonically
   * increasing; never decreases or wraps in practice (would require ~2^31 updates). */
  std::atomic<std::uint32_t> generation{0};
  /** @brief Which hardware counter `reference_counter` was sampled from; `none` until the first publish. */
  vdso_clock_source source{vdso_clock_source::none};
  /** @brief Reserved for future use; always `0` today. Keeps the struct's size a round number and leaves room to
   * grow without an ABI-breaking relayout. */
  std::uint32_t reserved0{0};
  /** @brief The counter's frequency, in Hz, as of this snapshot (a calibrated/measured rate can change slightly
   * between publishes on some hardware, e.g. a re-calibrated TSC). */
  std::uint64_t counter_hz{0};
  /** @brief The raw counter value sampled at the same instant as `reference_secs`/`reference_subsec_nanos`. */
  std::uint64_t reference_counter{0};
  /** @brief Whole seconds of the reference instant; see @ref vdso_clock_id for what epoch this is measured
   * against. */
  std::uint64_t reference_secs{0};
  /** @brief Sub-second nanoseconds of the reference instant, in `[0, 1'000'000'000)`. */
  std::uint32_t reference_subsec_nanos{0};
  /** @brief Reserved for future use; always `0` today. */
  std::uint32_t reserved1{0};
};

static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "vdso_clock_slot::generation must be lock-free to be safely shared across a read-only user-space "
              "mapping -- a fallback lock table keyed by address could never be consistent across two different "
              "address spaces");
static_assert(sizeof(vdso_clock_slot) == 64, "vdso_clock_slot's layout must stay pinned across bitness/ABI");
static_assert(alignof(vdso_clock_slot) == 64, "vdso_clock_slot's layout must stay pinned across bitness/ABI");

/**
 * @brief The full shared-memory page layout: one @ref vdso_clock_slot
 * per @ref vdso_clock_id. See the @file-level docs for the intended
 * kernel-writable/user-read-only dual mapping.
 */
struct vdso_clock_page {
  /** @brief Indexed by `static_cast<std::uint32_t>(vdso_clock_id)`; prefer @ref slot over indexing this
   * directly. */
  reloco::array<vdso_clock_slot, vdso_clock_id_count> slots{};

  /** @brief The slot for @p id. */
  [[nodiscard]] vdso_clock_slot &slot(vdso_clock_id id) noexcept {
    return slots[static_cast<std::uint32_t>(id)];
  }

  /** @copydoc slot */
  [[nodiscard]] const vdso_clock_slot &slot(vdso_clock_id id) const noexcept {
    return slots[static_cast<std::uint32_t>(id)];
  }
};

static_assert(sizeof(vdso_clock_page) == vdso_clock_id_count * sizeof(vdso_clock_slot),
              "vdso_clock_page's layout must stay pinned across bitness/ABI");

} // namespace hw
} // namespace structo
