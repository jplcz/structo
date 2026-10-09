// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file clock_ref.hpp
 * @brief `structo::hw::clock_ref`: a type-erased, non-owning handle over
 * a hardware free-running counter that is only ever *read* -- the clock
 * counterpart of `timer_ref.hpp`'s countdown/interval timer -- plus the
 * `clock_traits<Backend>` customization point and `clock_reader`, a
 * small feed-forward engine that turns raw (narrow, wrapping) counter
 * samples into monotonic forward steps.
 *
 * ## What the hardware has to provide
 *
 * Only two things: a way to read the raw counter and its frequency.
 * Optionally, the counter width in bits (default 64):
 *
 * @code
 * template <> struct structo::hw::clock_traits<my_backend> {
 *   // Raw counter value, right now. Only the low `counter_bits` bits are significant.
 *   static reloco::result<std::uint64_t> read_counter(my_backend &) noexcept;
 *   // Counting frequency in Hz. Must be non-zero for duration conversion.
 *   static std::uint64_t frequency_hz(my_backend &) noexcept;
 *   // Optional: counter width, 1..64. Absent means a full 64-bit counter.
 *   static unsigned counter_bits(my_backend &) noexcept;
 * };
 * @endcode
 *
 * ## Overflow/underflow handling
 *
 * A counter narrower than 64 bits wraps. `clock_ref::delta(prev, now)`
 * computes the forward distance modulo `2^counter_bits`, so a single
 * wrap between two samples is transparently handled. The same modular
 * arithmetic means a *backward* step shows up as a huge forward step
 * (close to a full counter range), which is how it is detected below.
 *
 * ## Feed-forward reading: `clock_reader`
 *
 * `clock_reader` remembers the last raw sample. Each `step()` reads the
 * counter and returns the forward distance since the previous sample.
 * Any step that is "impossible" for a healthy clock -- a backward step
 * (seen as a near-full-range forward step) or a forward step larger than
 * `max_step` (default: half the counter range) -- is reported as a
 * *glitch*: the reader discards the bogus delta, rebases onto the newly
 * read value, and the next `step()` measures relative to that new value.
 * The caller never sees a negative or absurd interval; it just learns a
 * glitch happened and can resynchronize its own state.
 *
 * ## Integration with `reloco::duration`/`reloco::instant`
 *
 * Steps and limits are expressible as `reloco::duration`
 * (`step_duration()`, the `duration` constructor overload for
 * `max_step`). The reader also accumulates every accepted step (glitches
 * excluded) into a 64-bit cycle total since `reset()`, exposed as
 * `now()` -- a `reloco::instant` whose epoch is the reset point -- and
 * `since_reset()`. The total is converted to time in one division, so no
 * per-step rounding error accumulates. Because glitched intervals are
 * dropped, the instant never runs backward and never jumps forward.
 *
 * ## Sharing one clock between CPUs: `atomic_clock_reader`
 *
 * `clock_reader` is single-owner. `atomic_clock_reader` is the same
 * feed-forward engine with its state in atomics, so any number of CPUs
 * may call `step()`/`now()` on one shared instance without a lock. The
 * last sample is advanced with `reloco::atomic::fetch_update` (a CAS
 * loop that re-reads the counter on every retry, so a loser never acts on
 * a stale sample); the CPU whose CAS wins *owns* the interval it
 * consumed and adds it to a shared atomic total, so every cycle is
 * counted exactly once and the shared `instant` is monotonic. The
 * underlying counter must be coherent across the CPUs using it
 * (`is_per_cpu == false`); a per-CPU counter with skew would appear as
 * glitches.
 *
 * A stored sample of `0` or `UINT64_MAX` ("infinite") means "not
 * primed": the sample is kept as `raw + 1` (modulo 2^64), so every real
 * counter value, including a raw `0`, is representable. Only the two
 * raw values of a full-width counter that would alias a sentinel
 * (`UINT64_MAX - 1` and `UINT64_MAX`) are nudged by one/two cycles, at a
 * point a 64-bit counter never reaches. `invalidate()` stores the
 * "infinite" sentinel to deliberately drop the reference sample.
 *
 * `max_step` is the longest gap you can legitimately go without polling;
 * half the counter range (the default and the maximum useful value) is
 * the largest that still distinguishes a backstep from a forward step.
 */

#include <structo/hw/clock_cycles.hpp>

#include <reloco/atomic_ops.hpp>
#include <reloco/detail/compat.hpp>
#include <reloco/duration.hpp>
#include <reloco/error.hpp>
#include <reloco/instant.hpp>
#include <reloco/lifetime.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace structo {

using namespace reloco;

namespace hw {

/** @brief Opt-in customization point for a readable hardware counter; see the file-level docs. */
template <typename Backend> struct clock_traits;

namespace detail {

template <typename Backend, typename = void> struct has_clock_traits : std::false_type {};
template <typename Backend>
struct has_clock_traits<
    Backend, std::void_t<decltype(clock_traits<Backend>::read_counter), decltype(clock_traits<Backend>::frequency_hz)>>
    : std::true_type {};

// Detects the optional Traits::counter_bits.
template <typename Traits, typename = void> struct clock_has_counter_bits : std::false_type {};
template <typename Traits>
struct clock_has_counter_bits<Traits, std::void_t<decltype(Traits::counter_bits)>> : std::true_type {};

} // namespace detail

/**
 * @brief Type-erased, non-owning handle over a readable hardware counter.
 *
 * Unbound refs fail every operation with `error::unsupported_operation`,
 * like `timer_ref`/`time_source_ref`.
 */
class RELOCO_POINTER clock_ref {
public:
  /** @brief Fixed, per-bound-backend-type dispatch table. */
  struct vtable {
    result<std::uint64_t> (*read_counter)(void *ctx) noexcept;
    std::uint64_t (*frequency_hz)(void *ctx) noexcept;
    unsigned (*counter_bits)(void *ctx) noexcept;
  };

  /** @brief Constructs an unbound ref. */
  constexpr clock_ref() noexcept = default;

  /** @brief Binds to an existing, adapted backend, which must outlive this handle and its copies. */
  template <typename Backend, std::enable_if_t<detail::has_clock_traits<Backend>::value, int> = 0>
  constexpr explicit clock_ref(Backend &b RELOCO_LIFETIMEBOUND RELOCO_LIFETIME_CAPTURE_BY_THIS) noexcept
      : ctx_(std::addressof(b)), vtbl_(&s_vtbl<Backend>) {}

  /** @brief Rejects rvalue/temporary backend bindings. */
  template <typename Backend, std::enable_if_t<!std::is_lvalue_reference_v<Backend>, int> = 0>
  clock_ref(Backend &&) = delete;

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return vtbl_ != nullptr; }

  /** @brief Reads the raw counter, masked to `counter_bits()` significant bits. */
  [[nodiscard]] result<cycles> read() const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    auto raw = vtbl_->read_counter(ctx_);
    if (!raw)
      return unexpected(raw.error());
    return cycles{raw.value() & mask_of(clamp_bits(vtbl_->counter_bits(ctx_)))};
  }

  /** @brief Counting frequency in Hz; `error::invalid_argument` if the backend reports 0. */
  [[nodiscard]] result<std::uint64_t> frequency_hz() const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    std::uint64_t hz = vtbl_->frequency_hz(ctx_);
    if (hz == 0)
      return unexpected(error::invalid_argument);
    return hz;
  }

  /** @brief Counter width in bits, 1..64. */
  [[nodiscard]] result<unsigned> counter_bits() const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return clamp_bits(vtbl_->counter_bits(ctx_));
  }

  /** @brief Largest raw value the counter reaches before wrapping to 0. */
  [[nodiscard]] result<cycles> max_value() const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return cycles{mask_of(clamp_bits(vtbl_->counter_bits(ctx_)))};
  }

  /**
   * @brief Forward distance from @p prev to @p now, modulo the counter
   * range, so one wrap between the samples is handled transparently.
   */
  [[nodiscard]] result<cycles> delta(cycles prev, cycles now) const noexcept {
    auto max = max_value();
    if (!max)
      return unexpected(max.error());
    return cycles{(now.raw() - prev.raw()) & max.value().raw()};
  }

  /** @brief Converts a cycle count of this clock to a `duration`. */
  [[nodiscard]] result<duration> to_duration(cycles c) const noexcept {
    auto hz = frequency_hz();
    if (!hz)
      return unexpected(hz.error());
    return checked_cycles_to_duration(c, hz.value());
  }

private:
  static constexpr unsigned clamp_bits(unsigned bits) noexcept { return bits == 0 || bits > 64 ? 64 : bits; }
  static constexpr std::uint64_t mask_of(unsigned bits) noexcept {
    return bits >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << bits) - 1);
  }

  template <typename Backend> static result<std::uint64_t> read_entry(void *ctx) noexcept {
    return clock_traits<Backend>::read_counter(*static_cast<Backend *>(ctx));
  }
  template <typename Backend> static std::uint64_t frequency_entry(void *ctx) noexcept {
    return clock_traits<Backend>::frequency_hz(*static_cast<Backend *>(ctx));
  }
  template <typename Backend> static unsigned bits_entry(void *ctx) noexcept {
    if constexpr (detail::clock_has_counter_bits<clock_traits<Backend>>::value)
      return clock_traits<Backend>::counter_bits(*static_cast<Backend *>(ctx));
    else
      return 64;
  }

  template <typename Backend>
  static constexpr vtable s_vtbl{&read_entry<Backend>, &frequency_entry<Backend>, &bits_entry<Backend>};

  void *ctx_ = nullptr;
  const vtable *vtbl_ = nullptr;
};

/** @brief Result of one `clock_reader::step()`. */
struct clock_step {
  /** @brief Forward cycles since the previous sample; 0 when `glitch` is set. */
  cycles elapsed{};
  /** @brief Whether the sample was a backstep or an oversized forward step; the reader rebased onto it. */
  bool glitch = false;
};

/**
 * @brief Feed-forward reader over a `clock_ref`; see the file-level docs.
 *
 * Holds only the last raw sample and the step limit, so it is cheap and
 * can be created per consumer.
 */
class clock_reader {
public:
  constexpr clock_reader() noexcept = default;

  /**
   * @param clk Clock to read; must outlive the reader.
   * @param max_step Largest forward step considered legitimate. `cycles{0}`
   * (default) selects half the counter range; larger values are clamped
   * to that.
   */
  explicit clock_reader(clock_ref clk, cycles max_step = cycles{0}) noexcept : clk_(clk), max_step_(max_step) {}

  /** @brief As above, with the step limit given as a `duration` (converted at the clock's frequency;
   * a limit that does not convert falls back to the default half-range limit). */
  clock_reader(clock_ref clk, duration max_step) noexcept : clk_(clk) {
    auto hz = clk.frequency_hz();
    if (hz) {
      auto c = checked_duration_to_cycles(max_step, hz.value());
      if (c)
        max_step_ = c.value();
    }
  }

  /** @brief Samples the counter as the new reference point and zeroes the accumulated total. */
  [[nodiscard]] result<void> reset() noexcept {
    auto now = clk_.read();
    if (!now)
      return unexpected(now.error());
    last_ = now.value();
    total_ = cycles{0};
    primed_ = true;
    return {};
  }

  /**
   * @brief Reads the counter and returns the step since the last sample.
   *
   * An unprimed reader primes itself on the first call and reports a
   * zero, non-glitch step. A backstep or step above the limit yields
   * `glitch = true` with zero `elapsed`; the reader is rebased onto the
   * new value either way, so the next call measures from there.
   */
  [[nodiscard]] result<clock_step> step() noexcept {
    auto now = clk_.read();
    if (!now)
      return unexpected(now.error());
    if (!primed_) {
      last_ = now.value();
      primed_ = true;
      return clock_step{};
    }
    auto d = clk_.delta(last_, now.value());
    auto max = clk_.max_value();
    if (!d || !max)
      return unexpected(!d ? d.error() : max.error());

    last_ = now.value();

    std::uint64_t limit = max.value().raw() / 2;
    if (max_step_.raw() != 0 && max_step_.raw() < limit)
      limit = max_step_.raw();
    if (d.value().raw() > limit)
      return clock_step{cycles{0}, true};
    total_ = total_.saturating_add(d.value());
    return clock_step{d.value(), false};
  }

  /** @brief Like `step()`, but converts a non-glitch step's `elapsed` to a duration (zero on glitch). */
  [[nodiscard]] result<duration> step_duration(bool *glitch = nullptr) noexcept {
    auto s = step();
    if (!s)
      return unexpected(s.error());
    if (glitch)
      *glitch = s.value().glitch;
    return clk_.to_duration(s.value().elapsed);
  }

  /** @brief Cycles accumulated by accepted steps since `reset()`/priming. Does not sample the counter. */
  [[nodiscard]] constexpr cycles total() const noexcept { return total_; }

  /** @brief Accumulated time since `reset()`/priming, as of the last `step()`. Does not sample the counter. */
  [[nodiscard]] result<duration> since_reset() const noexcept { return clk_.to_duration(total_); }

  /** @brief Accumulated time as a `reloco::instant` (epoch = `reset()`), as of the last `step()`. */
  [[nodiscard]] result<instant> last_instant() const noexcept {
    auto d = since_reset();
    if (!d)
      return unexpected(d.error());
    return instant{} + d.value();
  }

  /** @brief Samples the counter (one `step()`) and returns the resulting `instant`; glitches leave it unchanged. */
  [[nodiscard]] result<instant> now() noexcept {
    auto s = step();
    if (!s)
      return unexpected(s.error());
    return last_instant();
  }

  /** @brief Last raw sample taken. */
  [[nodiscard]] constexpr cycles last() const noexcept { return last_; }
  [[nodiscard]] constexpr bool primed() const noexcept { return primed_; }
  [[nodiscard]] constexpr const clock_ref &clock() const noexcept { return clk_; }

private:
  clock_ref clk_{};
  cycles max_step_{};
  cycles last_{};
  cycles total_{};
  bool primed_ = false;
};

/**
 * @brief Lock-free, multi-CPU variant of `clock_reader`; see the
 * file-level docs. Non-copyable and non-movable (it owns atomics).
 */
class atomic_clock_reader {
public:
  /** @param clk Clock to read; must outlive the reader.
   * @param max_step Largest legitimate forward step; `cycles{0}` selects half the counter range. */
  explicit atomic_clock_reader(clock_ref clk, cycles max_step = cycles{0}) noexcept
      : clk_(clk), max_step_(max_step.raw()) {}

  /** @brief As above, with the limit given as a `duration` (falls back to the default if it can't convert). */
  atomic_clock_reader(clock_ref clk, duration max_step) noexcept : clk_(clk) {
    auto hz = clk.frequency_hz();
    if (hz) {
      auto c = checked_duration_to_cycles(max_step, hz.value());
      if (c)
        max_step_ = c.value().raw();
    }
  }

  atomic_clock_reader(const atomic_clock_reader &) = delete;
  atomic_clock_reader &operator=(const atomic_clock_reader &) = delete;

  /**
   * @brief Samples the counter as the reference point and zeroes the total.
   * Must not race with `step()`/`now()`; call it once before sharing the reader.
   */
  [[nodiscard]] result<void> reset() noexcept {
    auto now = clk_.read();
    if (!now)
      return unexpected(now.error());
    total_.store(0, std::memory_order_relaxed);
    glitches_.store(0, std::memory_order_relaxed);
    last_.store(encode(now.value().raw()), std::memory_order_release);
    return {};
  }

  /** @brief Drops the reference sample (stores the "infinite" sentinel); `step()` fails with
   * `error::not_initialized` until the next `reset()`. */
  void invalidate() noexcept { last_.store(unprimed_inf, std::memory_order_release); }

  /** @brief Whether `reset()` has primed the reader (stored sample is neither `0` nor `UINT64_MAX`). */
  [[nodiscard]] bool primed() const noexcept { return is_primed(last_.load(std::memory_order_acquire)); }

  /**
   * @brief Reads the counter and returns the interval this call consumed.
   *
   * Safe from any CPU concurrently. Each interval is returned to exactly
   * one caller (the one whose CAS advanced the shared sample); a caller
   * that loses simply retries against a fresh read. Glitch semantics
   * match `clock_reader::step()`. Fails with `error::not_initialized`
   * before `reset()`.
   */
  [[nodiscard]] result<clock_step> step() noexcept {
    if (!primed())
      return unexpected(error::not_initialized);

    auto max = clk_.max_value();
    if (!max)
      return unexpected(max.error());
    std::uint64_t limit = max.value().raw() / 2;
    if (max_step_ != 0 && max_step_ < limit)
      limit = max_step_;

    // `f` may be re-run on CAS failure, so it only writes locals that the final run leaves correct.
    clock_step out{};
    error err{};
    bool failed = false;
    (void)reloco::atomic::fetch_update(last_, std::memory_order_acq_rel, std::memory_order_acquire,
                                       [&](std::uint64_t cur) noexcept -> optional<std::uint64_t> {
                                         if (!is_primed(cur)) {
                                           failed = true;
                                           err = error::not_initialized;
                                           return nullopt;
                                         }
                                         auto now = clk_.read();
                                         if (!now) {
                                           failed = true;
                                           err = now.error();
                                           return nullopt;
                                         }
                                         failed = false;
                                         std::uint64_t d = (now.value().raw() - decode(cur)) & max.value().raw();
                                         if (d == 0) {
                                           out = clock_step{};
                                           return nullopt;
                                         }
                                         out = d > limit ? clock_step{cycles{0}, true} : clock_step{cycles{d}, false};
                                         return encode(now.value().raw());
                                       });
    if (failed)
      return unexpected(err);

    if (out.glitch)
      glitches_.fetch_add(1, std::memory_order_relaxed);
    else if (out.elapsed.raw() != 0)
      total_.fetch_add(out.elapsed.raw(), std::memory_order_release);
    return out;
  }

  /** @brief Samples the counter (one `step()`) and returns the shared time as an `instant` (epoch = `reset()`). */
  [[nodiscard]] result<instant> now() noexcept {
    auto s = step();
    if (!s)
      return unexpected(s.error());
    return last_instant();
  }

  /** @brief Shared accumulated cycles since `reset()`. Does not sample the counter. */
  [[nodiscard]] cycles total() const noexcept { return cycles{total_.load(std::memory_order_acquire)}; }

  /** @brief Shared accumulated time since `reset()`. Does not sample the counter. */
  [[nodiscard]] result<duration> since_reset() const noexcept { return clk_.to_duration(total()); }

  /** @brief Shared accumulated time as an `instant`. Does not sample the counter. */
  [[nodiscard]] result<instant> last_instant() const noexcept {
    auto d = since_reset();
    if (!d)
      return unexpected(d.error());
    return instant{} + d.value();
  }

  /** @brief Number of glitches seen (by any CPU) since `reset()`. */
  [[nodiscard]] std::uint64_t glitch_count() const noexcept { return glitches_.load(std::memory_order_relaxed); }

  [[nodiscard]] const clock_ref &clock() const noexcept { return clk_; }

private:
  static constexpr std::uint64_t unprimed_inf = ~std::uint64_t{0};
  static constexpr bool is_primed(std::uint64_t stored) noexcept { return stored != 0 && stored != unprimed_inf; }

  // Stored as raw + 1 so that 0 stays reserved; the "infinite" sentinel is kept free the same way.
  static constexpr std::uint64_t encode(std::uint64_t raw) noexcept {
    std::uint64_t e = raw + 1;
    if (e == 0)
      return 1;
    return e == unprimed_inf ? unprimed_inf - 1 : e;
  }
  static constexpr std::uint64_t decode(std::uint64_t stored) noexcept { return stored - 1; }

  clock_ref clk_;
  std::uint64_t max_step_ = 0;
  std::atomic<std::uint64_t> last_{0};
  std::atomic<std::uint64_t> total_{0};
  std::atomic<std::uint64_t> glitches_{0};
};

/**
 * @brief Adapter to the `std::uint64_t (*)(void *ctx) noexcept` millisecond clock callbacks used by the
 * bootloader/network code (`scheduler::set_clock`, `boot_prompt`, `ppp_device`).
 *
 * Pass `&reader_now_ms<Reader>` with `ctx = &reader`, or use the reader-taking overloads of those APIs.
 * Each call advances the reader by one `step()` and returns the milliseconds since its `reset()`. A glitch
 * leaves the time unchanged; a failed counter read returns the last known time, so the clock never runs
 * backward or fails.
 */
template <typename Reader> [[nodiscard]] inline std::uint64_t reader_now_ms(void *ctx) noexcept {
  static_assert(std::is_same_v<Reader, clock_reader> || std::is_same_v<Reader, atomic_clock_reader>,
                "reader_now_ms needs a clock_reader or atomic_clock_reader");
  auto &r = *static_cast<Reader *>(ctx);
  auto t = r.now();
  if (!t)
    t = r.last_instant();
  return t ? (t.value() - instant{}).as_millis() : 0;
}

} // namespace hw
} // namespace structo
