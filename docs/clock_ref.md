<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::clock_ref`

`include/structo/hw/clock_ref.hpp`

The clock counterpart of [`timer_ref`](timer_ref.md): a type-erased,
non-owning handle over a hardware free-running counter that is only ever
*read*, plus two feed-forward readers that turn raw, possibly narrow and
wrapping samples into monotonic time (`clock_reader`, single owner, and
`atomic_clock_reader`, one lock-free source shared by many CPUs).

`timer_ref` arms and waits; `clock_ref` only answers "what is the counter
now". It is the *reading* primitive for code that just needs elapsed time
(bootloader scheduler, boot prompt, PPP timers). It is simpler than
[`time_source_ref`](time_manager.md) + `time_manager`, which exist to
publish wall-clock/monotonic time to a vDSO page; use `clock_ref` when you
only need a monotonic "now" and a counter width.

> **C++ standard:** `clock_ref.hpp` itself (`clock_ref`, `clock_reader`,
> `atomic_clock_reader`, `reader_now_ms`) builds as C++17. The consumers in
> [Plugging into the bootloader and network code](#plugging-into-the-bootloader-and-network-code)
> that are coroutine-based -- `bootldr::scheduler` and `hw::ppp_device` --
> require **C++20**. `bootldr::boot_prompt` is plain synchronous code and
> works in C++17.

## What the hardware provides

Only a counter read and a frequency; the counter width is optional
(default 64 bits).

```cpp
// A 32-bit, 1 MHz free-running timer register (e.g. a SoC's TIM2 in
// up-counting mode). `my_timer` is whatever type owns the register access.
template <> struct structo::hw::clock_traits<my_timer> {
  // Raw counter value right now. Only the low `counter_bits` bits matter;
  // clock_ref masks the rest. May fail (e.g. clock gated off).
  static reloco::result<std::uint64_t> read_counter(my_timer &t) noexcept { return t.regs->cnt; }

  // Counting frequency in Hz. Must be non-zero; 0 makes duration
  // conversion fail with error::invalid_argument.
  static std::uint64_t frequency_hz(my_timer &) noexcept { return 1'000'000; }

  // Optional: width of the counter in bits (1..64). Without it the
  // counter is treated as a full 64-bit one that never wraps in practice.
  static unsigned counter_bits(my_timer &) noexcept { return 32; }
};
```

## `clock_ref`

```cpp
my_timer timer;
structo::hw::clock_ref clk{timer};            // non-owning; `timer` must outlive it

auto raw   = clk.read();                      // result<cycles>, masked to counter_bits
auto hz    = clk.frequency_hz();              // result<uint64_t>
auto span  = clk.delta(prev, now);            // forward distance modulo the counter range,
                                              // so one wrap between samples is handled
auto dur   = clk.to_duration(span.value());   // cycles -> reloco::duration at hz
```

Unbound refs fail every call with `error::unsupported_operation`.

## `clock_reader`: feed-forward, single owner

The reader remembers the last raw sample. Each `step()` returns the
forward distance since the previous sample as a `clock_step`:

- `elapsed` -- cycles since the last sample (zero on a glitch).
- `glitch` -- set when the sample was a **backstep** (which modular
  arithmetic shows as a near-full-range forward step) or a forward step
  larger than `max_step`. The bogus delta is discarded and the reader
  **rebases onto the newly read value**, so the next `step()` is
  relative to it. The caller never sees a negative or absurd interval.

```cpp
structo::hw::clock_reader reader{clk};        // max_step defaults to half the counter range
// Or give the longest legitimate gap between polls as a duration:
// structo::hw::clock_reader reader{clk, reloco::duration::from_secs(2)};

if (!reader.reset()) { /* counter could not be read */ }   // sets the reference sample, zeroes the total

auto s = reader.step();                       // result<clock_step>
if (s && s->glitch) {
  // the clock jumped; resynchronize anything that depended on continuity
}

auto t = reader.now();                        // result<reloco::instant>, epoch = reset()
auto d = reader.step_duration();              // this step as a reloco::duration
```

Accepted steps accumulate (saturating, 64-bit) into a total since
`reset()`, exposed as `total()`, `since_reset()` (a `duration`) and
`last_instant()`/`now()` (a `reloco::instant`). The total is converted to
time in one division, so no per-step rounding error builds up, and glitched
intervals are dropped, so the instant never runs backward or jumps ahead.
An unprimed reader primes itself on its first `step()`.

## `atomic_clock_reader`: one source, many CPUs

Same semantics, with state in atomics so any number of CPUs may call
`step()`/`now()` on one shared instance without a lock.

```cpp
structo::hw::atomic_clock_reader shared{clk, reloco::duration::from_secs(2)};
shared.reset();                               // once, before sharing; must not race with step()

// On any CPU, concurrently:
auto t = shared.now();                        // shared monotonic instant
```

- The last sample is advanced with `reloco::atomic::fetch_update`; the
  counter is re-read on every CAS retry, so a loser never acts on a stale
  sample.
- The CPU whose CAS wins *owns* the interval it consumed and adds it to the
  shared total, so every cycle is counted exactly once.
- The counter must be coherent across CPUs (`is_per_cpu == false` in
  `time_source_capabilities` terms); per-CPU counters with skew would show up
  as glitches. `glitch_count()` reports them.
- **Not primed** is encoded in the stored sample: `0` and `UINT64_MAX` mean
  "no reference". Samples are stored as `raw + 1`, so a real raw `0` works;
  only raw `UINT64_MAX - 1`/`UINT64_MAX` of a full-width counter are nudged by
  one or two cycles. `step()` fails with `error::not_initialized` until
  `reset()`; `invalidate()` drops the reference on purpose.

## Plugging into the bootloader and network code

(`scheduler` and `ppp_device` need C++20; `boot_prompt` and the readers do not.)

The bootloader and network code take plain `uint64_t (*)(void *)`
millisecond clocks. `reader_now_ms<Reader>` adapts a reader to that shape
(each call steps the reader; glitches leave time unchanged; a failed read
returns the last known time), and these APIs accept a reader directly:

```cpp
structo::hw::atomic_clock_reader clock{clk};   // or clock_reader on a single CPU
clock.reset();

structo::bootldr::scheduler sched;
sched.set_clock(clock);                         // drives now_ms(), sleeps, DHCP/TFTP/ARP timers

auto r = structo::bootldr::boot_prompt(uart, clock);    // boot-prompt timeout

structo::hw::ppp_device<1500> ppp{uart, clock};         // PPP retransmit timers
```

Only genuine clock reads use a reader. Sleeping and retry deadlines still
work through the scheduler's `now_ms()`; the network code itself only takes a
`now_ms` argument and needs no change.
