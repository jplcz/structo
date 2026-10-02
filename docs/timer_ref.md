<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::timer_ref`

`include/structo/hw/timer_ref.hpp`

A type-erased, non-owning handle over the typical operations of a
hardware countdown/interval timer -- arming a one-shot or auto-reload
expiry, canceling it, checking whether it is currently armed, polling for
expiry, and querying the backend's capabilities -- plus the
`timer_traits<Backend>` customization point a concrete backend
specializes to be bindable through it.

This header is deliberately abstraction-only, the timer counterpart of
[`uart_ref.hpp`](uart_ref.md): there is no architectural-timer/PIT/HPET
register poking here, no clock-to-divisor math, nothing chip-specific. A
concrete backend (ARM generic timer `CNTP_*_EL0`, x86 LAPIC timer/HPET/
TSC-deadline, RISC-V `mtimecmp`, or a unit test's software fake)
implements `timer_traits<Backend>` however it needs to.

Only polled completion is modeled, matching `uart_ref`'s `tx_ready`/
`rx_ready`: whether a timer has expired is always checked explicitly
(`try_wait`), never awaited via a completion callback -- wiring an actual
interrupt to call back into software (if the backend supports one at
all) is left entirely to the backend/platform, outside this header's
scope.

## Why `structo::hw`

Like `uart_ref`/`hw_rng_ref`, `timer_ref` lives in `structo::hw`: device-
*level* behavioral interfaces -- "this is a timer, here's what a timer
does" -- as opposed to the lower-level register-access layer a concrete
backend is commonly, but not necessarily, built on top of.

## Why type-erased, like `uart_ref`/`hw_rng_ref`

Exactly as `uart_ref`/`hw_rng_ref` erase their concrete backend behind a
small, fixed vtable (a two-word handle: an untyped context pointer plus a
`const vtable *`, no virtual base class, no RTTI, no allocation of its
own), `timer_ref` erases the concrete timer backend the same way. This
lets a single scheduler tick/watchdog-kick/timeout routine be written
once against `timer_ref` and handed whatever concrete timer a given boot
stage/platform/test actually has, decided at runtime.

## Rust-inspired API shape

Two pieces of this header deliberately mirror well-known Rust API shapes
rather than inventing a new one:

- **Periods and readback are `reloco::duration`**, matching Rust's
  `std::time::Duration` -- integer-only arithmetic, no `<chrono>`
  dependency on freestanding targets (see `duration.hpp`'s own
  file-level docs).
- **`try_wait()`/`wait()` follow the `embedded-hal`/`nb` crate's
  `nb::Result<T, E>` "would block" shape** (also the shape Rust's
  `Future::poll` uses, modulo explicit polling vs. being driven by a
  waker): a non-blocking poll that returns `error::try_again` -- reused
  here rather than inventing a dedicated `error::would_block`, since it
  is exactly what `error::try_again` already documents -- when the timer
  has not expired yet, and `wait()` is the blocking spin-loop built on
  top, mirroring `nb`'s own `nb::block!` macro and `uart_ref::put_byte`'s
  spin loop over `try_put_byte`.

## `timer_mode` / `timer_capabilities`

```cpp
enum class timer_mode : std::uint8_t { one_shot, periodic };

struct timer_capabilities {
  bool supports_one_shot = true;
  bool supports_periodic = true;
  duration min_period{};
  duration max_period{}; // zero conventionally means "no known upper bound"
  duration resolution{}; // zero conventionally means "unknown"
};
```

`timer_capabilities` is an aggregate with `==`/`!=`.

## Customizing: `timer_traits<Backend>`

Left undefined for any `Backend` that hasn't opted in (mirroring
`uart_traits`/`hw_rng_traits`). A specialization must supply exactly four
mandatory functions -- everything else (`start`/`start_periodic`/`wait`/
`restart`) is synthesized generically on top of these by `timer_ref`
itself:

```cpp
template <> struct structo::hw::timer_traits<my_backend> {
  static reloco::result<void> try_start(my_backend &, structo::hw::timer_mode, reloco::duration) noexcept;
  static reloco::result<void> cancel(my_backend &) noexcept;
  static reloco::result<bool> is_active(my_backend &) noexcept;
  static reloco::result<void> try_wait(my_backend &) noexcept;
};
```

- `try_start` arms the timer to expire `period` from now, in the given
  mode, replacing any prior arming.
- `cancel` disarms the timer; idempotent.
- `is_active` reports whether the timer is currently armed and counting.
- `try_wait` is a non-blocking poll for expiry (see "Rust-inspired API
  shape" above): `error::try_again` if not yet expired, otherwise success
  (clearing the pending-expiry latch).

Optionally, a backend may also supply:

```cpp
static reloco::result<reloco::duration> remaining(my_backend &) noexcept;
static reloco::result<structo::hw::timer_capabilities> capabilities(my_backend &) noexcept;
```

- `remaining` reports the time left until the next expiry, for hardware
  that can read its own down-counter back.
- `capabilities` reports the backend's supported modes/period range/
  resolution.

Detected via SFINAE (the same optional-member idiom
`uart_traits::current_config`/`hw_rng_traits::is_available` use). If
absent, `timer_ref::remaining()`/`timer_ref::capabilities()` return
`unexpected(error::unsupported_operation)`.

## Operations

Forwarded directly to the bound backend's trait functions:

- `try_start(timer_mode, duration)`
- `cancel()`
- `is_active()`
- `try_wait()`
- `remaining()` (optional; see above)
- `capabilities()` (optional; see above)

Synthesized generically on top of the four mandatory operations, needing
no further backend support:

- `start(duration period)` -- `try_start(timer_mode::one_shot, period)`.
- `start_periodic(duration period)` -- `try_start(timer_mode::periodic, period)`.
- `restart(timer_mode mode, duration period)` -- `cancel()` followed by
  `try_start(mode, period)`, discarding any partially-elapsed previous
  countdown.
- `wait(std::uint32_t max_spins = default_max_spins)` -- spins on
  `try_wait()` until it succeeds or `max_spins` is exhausted (then
  returns `error::timed_out`), the `nb::block!`-equivalent blocking
  wrapper.

`default_max_spins` is `1'000'000`, matching `uart_ref`'s own default.

An unbound (default-constructed) `timer_ref` fails every operation with
`error::unsupported_operation`, mirroring `uart_ref`/`hw_rng_ref`'s
null-safety convention.

## Example

```cpp
struct my_arch_timer { /* ... CNTP_TVAL_EL0/CNTP_CTL_EL0, ... */ };

template <> struct structo::hw::timer_traits<my_arch_timer> {
  // ... four mandatory functions, plus optionally remaining/capabilities ...
};

void sleep_ms(structo::hw::timer_ref tmr, std::uint32_t ms) {
  tmr.start(reloco::duration::from_millis(ms));
  tmr.wait();
}

my_arch_timer generic_timer;
sleep_ms(structo::hw::timer_ref(generic_timer), 10); // same routine, any backend
```

See also: [`uart_ref.md`](uart_ref.md), [`hw_rng.md`](hw_rng.md) (the
other `structo::hw` type-erased device-level handles).
