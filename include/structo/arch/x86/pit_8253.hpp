// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file pit_8253.hpp
 * @brief `structo::arch::x86::pit_8253`: a basic driver for the legacy
 * Intel 8253/8254 Programmable Interval Timer's channel 0 (the classic
 * PC/AT "system timer", wired to IRQ 0 on both the 8259 PIC and an
 * IOAPIC's GSI 2/legacy ISA override), exposed through
 * `structo::hw::timer_ref` by specializing
 * `structo::hw::timer_traits<pit_8253>`.
 *
 * The 8253/8254 counts down a 16-bit latch at a fixed, non-programmable
 * input clock of `input_clock_hz` (`1.193182 MHz`, one-third of the
 * original CGA dot clock -- channel 0's output pin is what IRQ 0 is
 * wired to, regardless of which of the chip's three channels a given
 * board actually uses it for). `try_start` picks one of the chip's own
 * two matching operating modes -- mode 0 ("interrupt on terminal
 * count") for `timer_mode::one_shot`, mode 2 ("rate generator") for
 * `timer_mode::periodic`, both of which pulse the channel's `OUT` pin
 * (and hence, wired through the PIC/IOAPIC, the IRQ line) the way this
 * driver's two `timer_mode` values need -- and programs the 16-bit
 * divisor computed from the requested period via `clock_cycles.hpp`.
 *
 * ## Interrupt-driven, not self-polling
 *
 * Unlike a down-counter register a caller can read back on a whim (the
 * ARM generic timer's `CNTP_TVAL_EL0`, say), the 8253/8254 has no
 * reliable software-visible "has this period elapsed" signal of its own
 * between interrupts -- the readback command (`0xC0`) can latch the
 * current count, but by design says nothing about whether `OUT` has
 * already pulsed since the last read, and in rate-generator mode (2)
 * that pulse is a single internal clock tick wide, not something a
 * polling loop could reliably catch anyway. So this driver, like real
 * PIT-based timer-tick code everywhere, is interrupt-driven: the
 * caller's real IRQ 0 handler (after dispatching through
 * `structo::hw::irqc_ref`, e.g. `pic_8259::post_filter`) must call
 * @ref pit_8253::notify_expired once per interrupt to tell this driver
 * "the armed period has elapsed" -- `try_wait`/`is_active`/the optional
 * callback are all software bookkeeping driven by that one call, not by
 * directly re-reading the chip.
 *
 * @code
 * structo::arch::x86::port_io_backend backend{};
 * structo::io_space_ref<structo::port_io_space> ports(backend);
 * structo::arch::x86::pit_8253 pit(ports);
 *
 * structo::hw::timer_ref timer(pit);
 * (void)timer.try_start(structo::hw::timer_mode::periodic, reloco::duration::from_millis(10));
 *
 * auto on_tick = []() { ++ticks; };
 * (void)timer.set_callback(on_tick);
 *
 * // From the real IRQ 0 trap entry, after dispatching through irqc_ref:
 * pit.notify_expired();
 * @endcode
 *
 * ## Validation
 *
 * Like `pic_8259.hpp`/`vga_crtc.hpp`, this driver issues real `OUT`
 * instructions (indirectly, through whatever `io_space_ref` it is bound
 * to) and so is validated only by compilation -- standalone
 * multi-standard compile plus the public-header-check build target --
 * not by a dedicated unit test against a fake backend, which would only
 * prove the fake behaves as scripted rather than that the real 8253/8254
 * command/divisor sequencing is correct.
 */

#include "../../hw/clock_cycles.hpp"
#include "../../hw/timer_ref.hpp"
#include "../../io_space_ref.hpp"

#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/function_ref.hpp>
#include <reloco/optional.hpp>

#include <cstddef>
#include <cstdint>

namespace structo::arch::x86 {

/**
 * @brief A basic driver for PIT channel 0, implementing
 * `structo::hw::timer_traits<pit_8253>`.
 *
 * Bound to a caller-supplied `io_space_ref<port_io_space>` rather than
 * hardcoding `port_io_backend` itself, matching `pic_8259`/`vga_crtc`'s
 * approach, so this driver stays usable against any backend emulating
 * the same two ports.
 */
class pit_8253 {
public:
  /** @brief Channel 0's data port -- the PC/AT system-timer channel this driver targets. */
  static constexpr std::uint16_t channel0_data_port = 0x40;
  /** @brief The shared mode/command register for all three channels. */
  static constexpr std::uint16_t command_port = 0x43;

  /** @brief The chip's fixed input clock: `1.193182 MHz` (one-third of the original CGA dot clock). */
  static constexpr std::uint64_t input_clock_hz = 1'193'182;

  /** @brief Binds this driver to @p ports and channel 0's data port. Does
   * not itself touch hardware until `try_start` first programs it. */
  explicit pit_8253(structo::io_space_ref<structo::port_io_space> ports) noexcept : ports_(ports) {}

  pit_8253(const pit_8253 &) = delete;
  pit_8253 &operator=(const pit_8253 &) = delete;
  pit_8253(pit_8253 &&) = delete;
  pit_8253 &operator=(pit_8253 &&) = delete;

  /**
   * @brief Called once per real IRQ 0 interrupt (after the caller's
   * `irqc_ref`-level dispatch, e.g. `pic_8259::post_filter`) to tell
   * this driver the armed period has elapsed: a one-shot timer becomes
   * inactive, the pending-expiry latch `try_wait` polls is set, and the
   * registered callback (if any) is invoked synchronously, in whatever
   * context this method itself is called from -- normally interrupt
   * context, so the callback must be interrupt-safe (short,
   * non-blocking, no allocation), exactly as `timer_ref.hpp`'s own
   * `set_callback` documents. A no-op if the timer is not currently
   * active (e.g. a spurious/late call after `cancel`).
   */
  void notify_expired() noexcept {
    if (!active_)
      return;
    if (mode_ == structo::hw::timer_mode::one_shot)
      active_ = false;
    pending_ = true;
    if (callback_)
      (*callback_)();
  }

private:
  friend struct structo::hw::timer_traits<pit_8253>;

  [[nodiscard]] reloco::result<void> out8(std::uint16_t port, std::uint8_t value) noexcept {
    return ports_.write(structo::io_address<std::uint8_t, structo::port_io_space>{port}, value);
  }

  [[nodiscard]] reloco::result<void> try_start(structo::hw::timer_mode mode, reloco::duration period) noexcept {
    auto requested = structo::hw::checked_duration_to_cycles(period, input_clock_hz);
    if (!requested)
      return reloco::unexpected(requested.error());
    std::uint64_t raw = requested.value().raw();
    // The 16-bit counter cannot represent 0 (it means "65536" in hardware) or
    // anything above that.
    if (raw == 0 || raw > 65536)
      return reloco::unexpected(reloco::error::invalid_argument);
    std::uint16_t divisor = (raw == 65536) ? std::uint16_t{0} : static_cast<std::uint16_t>(raw);

    // Command byte: channel 0 (bits 7-6 = 00), access lobyte/hibyte (bits
    // 5-4 = 11), operating mode (bits 3-1), binary (not BCD) counting (bit 0 = 0).
    std::uint8_t operating_mode = (mode == structo::hw::timer_mode::periodic) ? std::uint8_t{2} : std::uint8_t{0};
    std::uint8_t command = static_cast<std::uint8_t>(0x30 | (operating_mode << 1));
    if (auto r = out8(command_port, command); !r)
      return r;
    if (auto r = out8(channel0_data_port, static_cast<std::uint8_t>(divisor & 0xFF)); !r)
      return r;
    if (auto r = out8(channel0_data_port, static_cast<std::uint8_t>(divisor >> 8)); !r)
      return r;

    mode_ = mode;
    active_ = true;
    pending_ = false;
    return {};
  }

  [[nodiscard]] reloco::result<void> cancel() noexcept {
    // The 8253/8254 has no per-channel "stop" register -- channel 0 keeps
    // free-running once programmed. Disarming for real means masking IRQ 0
    // at the IRQ controller (`irqc_ref::disable_intr`), which is outside
    // this driver's own scope; here `cancel` only clears the software
    // latch so `is_active`/`try_wait`/`notify_expired` stop reporting/
    // acting on further ticks.
    active_ = false;
    pending_ = false;
    return {};
  }

  [[nodiscard]] reloco::result<bool> is_active() const noexcept { return active_; }

  [[nodiscard]] reloco::result<void> try_wait() noexcept {
    if (!pending_)
      return reloco::unexpected(reloco::error::try_again);
    pending_ = false;
    return {};
  }

  [[nodiscard]] reloco::result<structo::hw::timer_capabilities> capabilities() const noexcept {
    structo::hw::timer_capabilities caps;
    caps.supports_one_shot = true;
    caps.supports_periodic = true;
    caps.is_per_cpu = false; // a single PIT is shared across every core on the board.
    caps.min_period = structo::hw::checked_cycles_to_duration(structo::hw::cycles{1}, input_clock_hz).value();
    caps.max_period = structo::hw::checked_cycles_to_duration(structo::hw::cycles{65536}, input_clock_hz).value();
    caps.resolution = caps.min_period;
    caps.supports_callback = true;
    caps.clock_hz = input_clock_hz;
    return caps;
  }

  [[nodiscard]] reloco::result<void> set_callback(reloco::function_ref<void()> cb) noexcept {
    callback_ = cb;
    return {};
  }

  [[nodiscard]] reloco::result<void> clear_callback() noexcept {
    callback_ = {};
    return {};
  }

  structo::io_space_ref<structo::port_io_space> ports_;
  structo::hw::timer_mode mode_ = structo::hw::timer_mode::one_shot;
  bool active_ = false;
  bool pending_ = false;
  reloco::optional<reloco::function_ref<void()>> callback_{};
};

} // namespace structo::arch::x86

namespace structo::hw {

template <> struct timer_traits<structo::arch::x86::pit_8253> {
  using pit_8253 = structo::arch::x86::pit_8253;

  [[nodiscard]] static reloco::result<void> try_start(pit_8253 &pit, timer_mode mode, duration period) noexcept {
    return pit.try_start(mode, period);
  }
  [[nodiscard]] static reloco::result<void> cancel(pit_8253 &pit) noexcept { return pit.cancel(); }
  [[nodiscard]] static reloco::result<bool> is_active(pit_8253 &pit) noexcept { return pit.is_active(); }
  [[nodiscard]] static reloco::result<void> try_wait(pit_8253 &pit) noexcept { return pit.try_wait(); }
  [[nodiscard]] static reloco::result<timer_capabilities> capabilities(pit_8253 &pit) noexcept {
    return pit.capabilities();
  }
  [[nodiscard]] static reloco::result<void> set_callback(pit_8253 &pit, reloco::function_ref<void()> cb) noexcept {
    return pit.set_callback(cb);
  }
  [[nodiscard]] static reloco::result<void> clear_callback(pit_8253 &pit) noexcept { return pit.clear_callback(); }
};

} // namespace structo::hw
