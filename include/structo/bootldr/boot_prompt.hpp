// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file boot_prompt.hpp
 * @brief Synchronous "press a key to boot" prompt over `hw::uart_ref`.
 *
 * `bootldr::boot_prompt` prints a message (optionally with a seconds
 * countdown), then busy-polls the UART until the wanted key arrives or the
 * time runs out. Any other key is ignored. It blocks the caller (no
 * scheduler or coroutines needed, so it works in C++17 and before the
 * scheduler exists) and measures time with a caller-supplied millisecond
 * clock.
 *
 * ```cpp
 * std::uint64_t now_ms(void *) noexcept;   // your monotonic millisecond clock
 *
 * structo::bootldr::boot_prompt_options opt;
 * opt.key = ' ';                           // key that approves
 * opt.timeout_ms = 5000;                   // how long to wait for it
 * opt.message = "Press SPACE to boot";     // shown before the countdown
 * auto r = structo::bootldr::boot_prompt(uart, now_ms, nullptr, opt);
 * if (r && *r == structo::bootldr::prompt_result::key_pressed) {
 *   // approved by the user
 * } else {
 *   // timed out (or the UART failed: r.error())
 * }
 * ```
 */

#include <reloco/error.hpp>
#include <reloco/string_view.hpp>
#include <structo/hw/uart_ref.hpp>

#include <microfmt/microfmt.hpp>

#include <cstdint>

namespace structo::bootldr {

/** @brief Outcome of a completed prompt. */
enum class prompt_result : std::uint8_t {
  key_pressed, ///< the wanted key arrived within the timeout
  timed_out    ///< nobody pressed it in time
};

/** @brief Monotonic millisecond clock: `fn(ctx)` returns the current time in ms. */
using prompt_clock_fn = std::uint64_t (*)(void *ctx) noexcept;

/** @brief What to ask for and how long to wait. */
struct boot_prompt_options {
  std::uint8_t key = ' ';                         ///< the approving key
  std::uint32_t timeout_ms = 3000;                ///< how long to wait for it
  reloco::string_view message = "Press SPACE to boot"; ///< prompt text (no trailing newline)
  bool ignore_case = true;                        ///< 'b' also matches 'B'
  bool flush_input = true;                        ///< drop bytes already received before the prompt
  bool countdown = true;                          ///< redraw "(N)" once per second
};

namespace detail {

inline std::uint8_t prompt_fold(std::uint8_t c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<std::uint8_t>(c - 'A' + 'a') : c;
}

// Redraws the countdown line "\r<message> (N) ".
inline reloco::result<void> prompt_draw(const hw::uart_ref &uart, reloco::string_view msg, std::uint32_t secs) noexcept {
  const auto line = microfmt::format<128>("\r{} ({}) ", msg, secs);
  return uart.write_string(line.view());
}

} // namespace detail

/**
 * @brief Shows the prompt and waits up to `opt.timeout_ms` for `opt.key`.
 *
 * @param uart  console UART (printed to and read from).
 * @param clock millisecond clock; must not be null.
 * @param ctx   passed to `clock`.
 * @param opt   key, timeout and text.
 * @return `key_pressed` or `timed_out`; a UART error (other than "no byte yet") is returned as is,
 *         `error::invalid_argument` if `clock` is null.
 */
[[nodiscard]] inline reloco::result<prompt_result> boot_prompt(const hw::uart_ref &uart, prompt_clock_fn clock,
                                                               void *ctx, const boot_prompt_options &opt = {}) noexcept {
  if (!clock)
    return reloco::unexpected(reloco::error::invalid_argument);

  if (opt.flush_input) {
    for (;;) {
      auto ready = uart.rx_ready();
      if (!ready)
        return reloco::unexpected(ready.error());
      if (!*ready)
        break;
      auto b = uart.try_get_byte();
      if (!b && b.error() != reloco::error::try_again)
        return reloco::unexpected(b.error());
    }
  }

  const std::uint64_t start = clock(ctx);
  const std::uint8_t wanted = opt.ignore_case ? detail::prompt_fold(opt.key) : opt.key;
  std::uint32_t shown = 0; // last countdown value drawn; 0 = nothing yet

  if (!opt.countdown) {
    if (auto r = uart.write_string(opt.message); !r)
      return reloco::unexpected(r.error());
  }

  prompt_result outcome = prompt_result::timed_out;
  for (;;) {
    const std::uint64_t elapsed = clock(ctx) - start;

    if (opt.countdown) {
      const std::uint64_t left = elapsed >= opt.timeout_ms ? 0 : opt.timeout_ms - elapsed;
      const auto secs = static_cast<std::uint32_t>((left + 999) / 1000);
      if (secs != shown && secs != 0) {
        shown = secs;
        if (auto r = detail::prompt_draw(uart, opt.message, secs); !r)
          return reloco::unexpected(r.error());
      }
    }

    auto ready = uart.rx_ready();
    if (!ready)
      return reloco::unexpected(ready.error());
    if (*ready) {
      auto b = uart.try_get_byte();
      if (b) {
        const std::uint8_t c = opt.ignore_case ? detail::prompt_fold(*b) : *b;
        if (c == wanted) {
          outcome = prompt_result::key_pressed;
          break;
        }
      } else if (b.error() != reloco::error::try_again) {
        return reloco::unexpected(b.error());
      }
      continue; // drain every pending byte before testing the deadline
    }

    if (elapsed >= opt.timeout_ms)
      break;
  }

  if (auto r = uart.write_string("\n"); !r)
    return reloco::unexpected(r.error());
  return outcome;
}

} // namespace structo::bootldr
