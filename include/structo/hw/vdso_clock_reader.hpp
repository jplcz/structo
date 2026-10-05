// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file vdso_clock_reader.hpp
 * @brief `structo::hw::vdso_clock_reader<Traits>`: the user-space half
 * of the VDSO clock page (`vdso_clock_page.hpp`) -- retries the seqlock
 * until it observes a consistent `vdso_clock_slot` snapshot, reads the
 * hardware counter itself, and computes the resulting instant (a
 * duration since the clock's own epoch -- the Unix epoch for
 * `vdso_clock_id::realtime`, an arbitrary kernel-chosen epoch for
 * `vdso_clock_id::monotonic`).
 *
 * `vdso_clock_reader` only ever takes a `const vdso_clock_slot &`: user
 * space's mapping of the clock page is read-only (see
 * `vdso_clock_page.hpp`'s own file-level docs), and nothing here ever
 * needs (or is able) to write through it.
 *
 * ## `Traits`
 *
 * The only hardware-specific hook a caller supplies is reading the raw
 * free-running counter identified by a @ref vdso_clock_source -- "how
 * do I execute `RDTSC`/read `CNTVCT_EL0`/read `mtime` from user space",
 * which is both architecture-specific and, on some of them, requires
 * selecting *which* instruction at runtime (e.g. a `source` published
 * by the kernel as `arm_cntvct_el0` vs. `arm_cntpct_el0`):
 *
 * @code
 * struct my_vdso_reader_traits {
 *   static std::uint64_t read_counter(structo::hw::vdso_clock_source source) noexcept {
 *     switch (source) {
 *     case structo::hw::vdso_clock_source::x86_tsc: {
 *       std::uint32_t lo, hi;
 *       asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
 *       return (static_cast<std::uint64_t>(hi) << 32) | lo;
 *     }
 *     case structo::hw::vdso_clock_source::arm_cntvct_el0: {
 *       std::uint64_t val;
 *       asm volatile("mrs %0, cntvct_el0" : "=r"(val));
 *       return val;
 *     }
 *     default:
 *       return 0; // unsupported source; try_now() will report a conversion/overflow error downstream
 *     }
 *   }
 * };
 * @endcode
 */

#include <structo/hw/clock_cycles.hpp>
#include <structo/hw/vdso_clock_page.hpp>

#include <reloco/duration.hpp>
#include <reloco/error.hpp>

#include <atomic>
#include <cstdint>

namespace structo {

using namespace reloco;

namespace hw {

/**
 * @brief Retries a @ref vdso_clock_slot's seqlock to obtain a
 * consistent snapshot, then computes the resulting instant using
 * @p Traits to read the live hardware counter. See the @file-level
 * docs' `Traits` example.
 * @tparam Traits Supplies `static std::uint64_t read_counter(vdso_clock_source source) noexcept`.
 */
template <typename Traits> class vdso_clock_reader {
public:
  /** @brief Upper bound on seqlock retry attempts before giving up with `error::try_again` -- guards against a
   * pathologically unlucky (or buggy, perpetually-mid-update) writer starving the reader forever. A real writer
   * update is a handful of plain stores, so this bound is never reached in practice. */
  static constexpr std::uint32_t max_attempts = 100;

  /**
   * @brief Reads @p slot and returns the instant it represents as of right now: the published reference instant
   * plus however much the live counter has advanced since the reference sample was taken.
   * @param slot The clock slot to read (`vdso_clock_page::slot`). Read-only.
   * @return A `duration` since the clock's own epoch (see @ref vdso_clock_id), or:
   * - `error::not_initialized` if the kernel has not published a first snapshot yet (`source ==
   *   vdso_clock_source::none`);
   * - `error::try_again` if `max_attempts` consecutive seqlock retries were all torn/mid-update;
   * - `error::integer_overflow`/`error::invalid_argument` propagated from the counter-to-duration conversion
   *   (`clock_cycles.hpp`), e.g. a zero `counter_hz` or a live counter reading that is behind the published
   *   reference sample (a non-monotonic/misbehaving counter).
   */
  [[nodiscard]] static result<duration> try_now(const vdso_clock_slot &slot) noexcept {
    vdso_clock_source source{};
    std::uint64_t counter_hz = 0;
    std::uint64_t reference_counter = 0;
    std::uint64_t reference_secs = 0;
    std::uint32_t reference_subsec_nanos = 0;

    bool consistent = false;
    for (std::uint32_t attempt = 0; attempt < max_attempts; ++attempt) {
      const std::uint32_t gen_before = slot.generation.load(std::memory_order_seq_cst);
      if ((gen_before & 1u) != 0u)
        continue; // writer mid-update; retry without even reading the other fields.

      source = slot.source;
      counter_hz = slot.counter_hz;
      reference_counter = slot.reference_counter;
      reference_secs = slot.reference_secs;
      reference_subsec_nanos = slot.reference_subsec_nanos;

      const std::uint32_t gen_after = slot.generation.load(std::memory_order_seq_cst);
      if (gen_after == gen_before) {
        consistent = true;
        break;
      }
    }
    if (!consistent)
      return unexpected(error::try_again);

    if (source == vdso_clock_source::none)
      return unexpected(error::not_initialized);

    const std::uint64_t raw_now = Traits::read_counter(source);
    auto elapsed_cycles = cycles{raw_now}.checked_sub(cycles{reference_counter});
    if (!elapsed_cycles)
      return unexpected(elapsed_cycles.error());

    auto elapsed = checked_cycles_to_duration(elapsed_cycles.value(), counter_hz);
    if (!elapsed)
      return unexpected(elapsed.error());

    return duration::from_secs(reference_secs) + duration::from_nanos(reference_subsec_nanos) + elapsed.value();
  }
};

} // namespace hw
} // namespace structo
