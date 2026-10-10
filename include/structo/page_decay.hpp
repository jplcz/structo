// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file page_decay.hpp
 * @brief Pure policy helpers for a page daemon that demotes pages ACTIVE -> INACTIVE from their access flag.
 *
 * Aging (NFU / "shift-right history"): every page carries a small history word of `Bits` bits. On each daemon
 * scan the history shifts right and the newest bit becomes "was the page accessed since the last scan". A page
 * whose history reaches 0 was not touched during the last `Bits` scans and is demoted. Frequently used pages keep
 * high bits set and stay ACTIVE; the history length sets how long a page survives without access.
 *
 * Everything here is `constexpr` integer math. The caller owns the ACTIVE/INACTIVE lists, harvests and clears the
 * hardware access bit (page-table walk / rmap) and stores the history word in its page descriptor
 * (`page_age_field` packs it into a flags word).
 *
 * @code
 * using decay = structo::page_decay<4>;        // 4 bits: demote after 4 consecutive idle scans
 *
 * // Page daemon, per scan of one ACTIVE page. 'accessed' = access bit harvested from every mapping of the page
 * // (and cleared), or the software "referenced" flag for unmapped page-cache pages.
 * auto r = decay::step(page.age, accessed);
 * page.age = r.age;
 * if (r.action == structo::decay_action::demote) move_to_inactive(page);
 *
 * // A page entering ACTIVE (promoted from INACTIVE after an access) starts with a full history.
 * page.age = decay::fresh();
 *
 * // How much to scan this round, from memory pressure (free pages vs. watermarks) and list balance.
 * structo::decay_pressure_config cfg{};         // defaults: keep inactive >= 50 % of active+inactive
 * std::uint64_t n = structo::decay_scan_count(cfg, active_pages, inactive_pages, free_pages, wm_low, wm_high);
 * @endcode
 */

#include <structo/memory_pressure.hpp>

#include <cstdint>
#include <type_traits>

namespace structo {

/** Outcome of one aging step. */
enum class decay_action : std::uint8_t {
  keep_active, //!< Accessed recently enough: stays on the ACTIVE list.
  demote,      //!< History exhausted: move to the INACTIVE list.
};

/** @tparam Bits History length in bits, 1..16 (idle scans a fresh page survives). */
template <unsigned Bits = 4> struct page_decay {
  static_assert(Bits >= 1 && Bits <= 16, "history must be 1..16 bits");
  using age_type = std::conditional_t<(Bits <= 8), std::uint8_t, std::uint16_t>;

  static constexpr age_type top_bit = static_cast<age_type>(1u << (Bits - 1));
  static constexpr age_type max_age = static_cast<age_type>((1u << Bits) - 1u);

  struct outcome {
    age_type age;
    decay_action action;
  };

  /** History of a page that just became ACTIVE: survives `Bits` idle scans. */
  [[nodiscard]] static constexpr age_type fresh() noexcept { return max_age; }

  /** One daemon scan: shift the history and record whether the page was accessed since the previous scan. */
  [[nodiscard]] static constexpr outcome step(age_type age, bool accessed) noexcept {
    const auto next = static_cast<age_type>((age >> 1) | (accessed ? top_bit : 0));
    return {next, next == 0 ? decay_action::demote : decay_action::keep_active};
  }

  /** Records an access seen outside a scan (e.g. `mark_page_accessed()` on a read) without shifting. */
  [[nodiscard]] static constexpr age_type touch(age_type age) noexcept { return static_cast<age_type>(age | top_bit); }

  /** Idle scans left before the page is demoted (0 = already demotable). */
  [[nodiscard]] static constexpr unsigned idle_scans_left(age_type age) noexcept {
    unsigned n = 0;
    while (age != 0) {
      ++n;
      age = static_cast<age_type>(age >> 1);
    }
    return n;
  }
};

/** Packs a `Bits`-wide history into bits `[Shift, Shift + Bits)` of an unsigned flags word. */
template <unsigned Shift, unsigned Bits = 4, typename Word = std::uint32_t> struct page_age_field {
  static_assert(std::is_unsigned_v<Word>);
  static_assert(Shift + Bits <= sizeof(Word) * 8, "field does not fit in the flags word");
  static constexpr Word mask = static_cast<Word>(((Word{1} << Bits) - 1u) << Shift);

  [[nodiscard]] static constexpr typename page_decay<Bits>::age_type get(Word flags) noexcept {
    return static_cast<typename page_decay<Bits>::age_type>((flags & mask) >> Shift);
  }
  [[nodiscard]] static constexpr Word set(Word flags, typename page_decay<Bits>::age_type age) noexcept {
    return static_cast<Word>((flags & static_cast<Word>(~mask)) | ((static_cast<Word>(age) << Shift) & mask));
  }
};

/** Tuning for `decay_scan_count`. */
struct decay_pressure_config {
  std::uint32_t inactive_target_percent{50}; //!< Desired INACTIVE share of ACTIVE+INACTIVE.
  std::uint32_t balance_urgency{16};         //!< Urgency (0..256) used when only the lists are unbalanced.
  std::uint64_t min_batch{32};               //!< Smallest non-empty scan when any scanning is wanted.
};

/** True when the INACTIVE list is smaller than the target share, so ACTIVE pages should be aged. */
[[nodiscard]] constexpr bool decay_inactive_is_low(const decay_pressure_config &cfg, std::uint64_t active,
                                                   std::uint64_t inactive) noexcept {
  const std::uint64_t total = active + inactive;
  return inactive < total / 100 * cfg.inactive_target_percent + (total % 100) * cfg.inactive_target_percent / 100;
}

/** Urgency 0..256 (memory_pressure.hpp): 0 when free >= `wm_high`, 256 when free <= `wm_low`, linear between. */
[[nodiscard]] constexpr std::uint32_t decay_urgency(std::uint64_t free_pages, std::uint64_t wm_low,
                                                    std::uint64_t wm_high) noexcept {
  return pressure_urgency(free_pages, memory_watermarks{wm_low, wm_low, wm_high});
}

/**
 * How many ACTIVE pages the daemon should age this round. Scales with memory pressure; with plenty of free
 * memory it still scans a little (`balance_urgency`) while the INACTIVE list is too small, and not at all when
 * both are fine. Never exceeds `active`.
 */
[[nodiscard]] constexpr std::uint64_t decay_scan_count(const decay_pressure_config &cfg, std::uint64_t active,
                                                       std::uint64_t inactive, std::uint64_t free_pages,
                                                       std::uint64_t wm_low, std::uint64_t wm_high) noexcept {
  std::uint32_t urgency = decay_urgency(free_pages, wm_low, wm_high);
  if (urgency == 0 && decay_inactive_is_low(cfg, active, inactive)) {
    urgency = cfg.balance_urgency;
  }
  if (urgency == 0 || active == 0) {
    return 0;
  }
  // active * urgency / 256 without overflowing for very large page counts.
  std::uint64_t n = (active >> 8) * urgency + (((active & 0xFF) * urgency) >> 8);
  if (n < cfg.min_batch) {
    n = cfg.min_batch;
  }
  return n < active ? n : active;
}

} // namespace structo
