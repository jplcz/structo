// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file rng_combinator.hpp
 * @brief `structo::hw::hw_rng_combinator`: combines several
 * `hw_rng_ref` sources (e.g. a real hardware RNG plus one or more weak
 * fallback jitter sources) into a single draw.
 *
 * This is the piece `arch/arm/hw_rng.hpp` and `arch/arm64/hw_rng.hpp`'s
 * `cntpct_rng`/`cntvct_rng` fallback backends exist to feed:
 *
 * @code
 * using namespace structo::arch::arm64;
 * using namespace structo::hw;
 *
 * rndr_rng rndr{};
 * cntpct_rng ct{};
 * cntvct_rng cv{};
 *
 * reloco::array<hw_rng_ref, 3> sources{hw_rng_ref(rndr), hw_rng_ref(ct), hw_rng_ref(cv)};
 * hw_rng_combinator combo(sources);
 *
 * auto word = combo.try_generate64(); // mixes whichever sources succeed
 *
 * // A combinator can itself be bound through another hw_rng_ref (e.g.
 * // to hand it to structo::prng's from_hw_rng factories):
 * hw_rng_ref ref(combo);
 * @endcode
 *
 * ## Combining strategy
 *
 * Every bound source is drawn from once per `try_generate64()` call
 * (with its own small per-source retry budget); every *successful*
 * draw is `XOR`-folded into an accumulator, which is then run through
 * `hw::detail::avalanche_mix64` to spread whatever entropy any one
 * source contributed across every output bit and remove any linear
 * structure a weak source (e.g. a raw timer reading) might otherwise
 * leave behind. A source that fails this round (including a weak
 * fallback source that is simply absent on this core) is skipped
 * entirely rather than aborting the whole draw -- the combined draw
 * only fails if *every* source failed, in which case it reports
 * whichever source's error was observed last.
 *
 * This is a simple, practical mixer (XOR-fold plus an avalanche
 * finalizer), not a formally-proven randomness extractor -- it cannot
 * manufacture entropy that wasn't present in at least one successful
 * source draw, it only spreads and decorrelates whatever was
 * contributed. Prefer feeding it at least one genuine hardware entropy
 * source (`rdrand_rng`, `rndr_rng`, the RISC-V `seed_rng`, ...);
 * weak-only combinations (e.g. just `cntpct_rng` + `cntvct_rng`) should
 * be a last resort for cores with no real hardware entropy source at
 * all, not a routine choice.
 *
 * `hw_rng_combinator` does not own its sources or their backing storage
 * -- the `span<const hw_rng_ref>` passed to its constructor, and every
 * backend any of those refs is bound to, must outlive it.
 */

#include <structo/hw/rng.hpp>

#include <cstdint>

namespace structo::hw {

/**
 * @brief Combines several `hw_rng_ref` sources into one draw by
 * `XOR`-folding every source that succeeds and avalanche-mixing the
 * result. See the @file-level docs for the full combining strategy and
 * rationale.
 */
class hw_rng_combinator {
public:
  /**
   * @brief Binds a (non-owning) list of sources to combine.
   * @param sources Sources to draw from and combine, in order. Must
   * outlive this combinator, along with every backend any of them is
   * bound to. May be empty (every draw then fails with
   * `error::unsupported_operation`).
   */
  constexpr explicit hw_rng_combinator(span<const hw_rng_ref> sources RELOCO_LIFETIMEBOUND) noexcept
      : sources_(sources) {}

  /**
   * @brief Whether at least one bound source reports itself available.
   * `false` if there are no sources at all.
   */
  [[nodiscard]] bool is_available() const noexcept {
    for (auto const &src : sources_) {
      if (src.is_available())
        return true;
    }
    return false;
  }

  /**
   * @brief Draws from every bound source (each retried up to
   * @p max_retries_per_source times), `XOR`-folds every successful
   * draw, and avalanche-mixes the result.
   * Fails with `error::unsupported_operation` if there are no sources,
   * or with whichever source's error was observed last if every source
   * failed this round.
   */
  [[nodiscard]] result<std::uint64_t>
  try_generate64(std::uint32_t max_retries_per_source = hw_rng_ref::default_max_retries) const noexcept {
    std::uint64_t acc = 0;
    bool any_ok = false;
    error last_error = error::unsupported_operation;

    for (auto const &src : sources_) {
      auto word = src.try_generate64(max_retries_per_source);
      if (word) {
        acc ^= word.value();
        any_ok = true;
      } else {
        last_error = word.error();
      }
    }

    if (!any_ok)
      return unexpected(last_error);
    return detail::avalanche_mix64(acc);
  }

private:
  span<const hw_rng_ref> sources_;
};

template <> struct hw_rng_traits<hw_rng_combinator> {
  static result<std::uint64_t> try_generate64(hw_rng_combinator &b) noexcept { return b.try_generate64(); }
  static bool is_available(hw_rng_combinator &b) noexcept { return b.is_available(); }
};

} // namespace structo::hw
