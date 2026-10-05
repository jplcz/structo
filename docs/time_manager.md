<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::time_manager`

`include/structo/hw/time_manager.hpp`, `include/structo/hw/time_source_ref.hpp`,
`include/structo/hw/vdso_clock_page.hpp`, `include/structo/hw/vdso_clock_writer.hpp`,
`include/structo/hw/vdso_clock_reader.hpp`

A tickless kernel-level timekeeping facility ("timehands"/timecounter
manager, in FreeBSD/Linux terminology) built from four cooperating
pieces:

- **`time_source_ref`** -- a type-erased, non-owning handle over a
  free-running hardware counter (TSC/`CNTVCT_EL0`/`CNTPCT_EL0`/RISC-V
  `mtime`), the opposite complement of `timer_ref`'s countdown/interval
  model: it only ever answers "what is your raw count right now".
- **`vdso_clock_page`** -- the bitness-agnostic, shared-memory structure
  a kernel publishes a counter-plus-reference-instant snapshot through,
  so user space can compute "what time is it" without a syscall --
  Linux's `vvar`/vDSO page, FreeBSD's `vdso_timehands`, and Windows'
  `KUSER_SHARED_DATA` all play this same role.
- **`vdso_clock_writer`** -- the kernel-side seqlock writer for one
  `vdso_clock_page` slot.
- **`vdso_clock_reader<Traits>`** -- the user-space-side seqlock reader,
  parameterized on a `Traits::read_counter` hook for "how do I execute
  `RDTSC`/read `CNTVCT_EL0`/read `mtime` from user space".
- **`time_manager`** -- the actual tickless manager tying all of the
  above together: samples a `time_source_ref` to maintain monotonic and
  realtime "now", and optionally republishes both into a
  `vdso_clock_page`.

## Why `time_source_ref` is separate from `timer_ref`

`timer_ref` (`timer_ref.md`) models a countdown/interval timer: arm it
for a period, get notified when it elapses. `time_source_ref` models the
complementary primitive a tickless timekeeping facility actually needs:
a free-running counter with no notion of "armed", only "what is your
raw count right now" (`try_now`) plus static `capabilities()` (`clock_hz`,
wraparound bound `max_value`, and the `is_monotonic`/`is_per_cpu` flags
below).

### Customization point: `time_source_traits<Backend>`

Left undefined for any `Backend` that hasn't opted in, mirroring
`uart_traits`/`hw_rng_traits`:

```cpp
template <> struct structo::hw::time_source_traits<my_backend> {
  static reloco::result<structo::hw::cycles> try_now(my_backend &) noexcept;
  static structo::hw::time_source_capabilities capabilities(my_backend &) noexcept;
};
```

## Tickless: no periodic internal work is required to keep time

Unlike a classic `jiffies`-style kernel that must increment a counter on
every hardclock tick, `time_manager` never needs to run on a fixed
schedule to stay correct *between* calls to `resync()` --
`try_monotonic_now`/`try_realtime_now` always compute the live answer as
"the last published reference instant, plus however far the hardware
counter has advanced since", exactly the computation `vdso_clock_reader`
performs in user space. The one periodic obligation is `resync()`
itself, needed to keep the published reference recent enough for the
next wraparound-detection window to stay valid -- see
`max_resync_interval()` for the recommended cadence. `resync()` takes no
arguments and needs no scheduling policy of its own; an external event
timer/callout calls it on whatever cadence `max_resync_interval()`
recommends.

## Mutual exclusion: writers are caller-serialized, reads stay lockless

`start`/`set_realtime`/`resync`/`switch_source` mutate shared state and
are **not** internally synchronized against concurrent calls to each
other -- exactly like `vdso_clock_writer`, whose precondition `time_manager`
inherits: the caller must already hold whatever lock serializes
concurrent writers (e.g. a per-instance kernel spin lock) for the entire
extent of any one of these calls. `try_monotonic_now`/`try_realtime_now`,
by contrast, are genuinely lock-free and safe to call from any context
(including concurrently with an in-progress `resync()`), via the same
seqlock-retry protocol `vdso_clock_reader` uses.

## Handling an imperfect hardware counter: `is_monotonic`/`is_per_cpu`

`time_source_capabilities` describes two properties that materially
change how `time_manager` behaves, not just advisory metadata:

- **`is_monotonic == false`** (the counter can genuinely stutter/step
  backward, not just wrap at its natural width): `resync()` tracks a
  high-water mark instead of blindly trusting "the new sample is
  smaller, so it must have wrapped". A sample at or below the
  high-water mark contributes zero elapsed time (clamped, counted in
  `backstep_count()`) and does **not** move the mark backward -- the
  published monotonic clock itself never regresses, and automatically
  recovers full accounting once the counter climbs back above the mark,
  with no special-cased recovery step. An unstable counter can never
  feed a shared `vdso_clock_page` (user space has no way to apply this
  same high-water-mark recovery against a live read that dips below the
  last published reference), so `try_create`/`switch_source` refuse to
  bind a `vdso_clock_page` together with an `is_monotonic == false`
  source.
- **`is_per_cpu == true`** (the raw counter is not a single,
  globally-consistent count, e.g. a non-invariant TSC): a snapshot
  published by one CPU is meaningless correlated against a counter read
  on a *different* CPU, exactly what a shared `vdso_clock_page` requires
  every reader to do safely. `try_create`/`switch_source` therefore also
  refuse to bind a `vdso_clock_page` together with an `is_per_cpu ==
  true` source; such a source may still back a purely this-CPU-local,
  non-VDSO `time_manager` (one instance per core).

## Reset-vs-wraparound: a hardware counter reset must never look like a huge forward jump

A genuine wraparound (`raw_now < last` because the counter rolled over
its natural width) and a hardware reset-to-zero (e.g. a core
power-off/power-on cycle resetting an unsynchronized TSC) are
indistinguishable by *sign* alone, but very distinguishable by
*magnitude* on a near-full-range counter: a real wrap observed at or
below `max_resync_interval()`'s recommended cadence can imply at most
half the counter's full period, while a reset-to-zero on a counter that
had climbed close to its maximum would otherwise be misread as an
enormous, bogus forward time jump (far worse than simply freezing).
`resync()`/`switch_source()`/the lockless read path all bound the
accepted wrap delta to half the counter's period (`max_value`); anything
larger is treated as an untrusted backward step -- the same
clamp-and-flag path used for an `is_monotonic == false` source. This
converges the two failure modes (`is_monotonic == false`, and a
plausibility-bounded wraparound check for `is_monotonic == true`) onto
one clamp-and-self-heal recovery story.

## Observability: two independent, non-blocking backstep counters

Clamping a backward step must stay silent and lock-free from any
caller's point of view (including a context that can't take the writer
lock, or `vdso_clock_reader`'s equivalent lockless user-space read,
which has no lock to take at all). Instead of surfacing an error a
caller would have to handle on every read, `time_manager` exposes two
independent, purely diagnostic atomic counters a separate monitoring
task can poll (as *deltas*, with hysteresis, not absolute totals) to
decide policy:

- **`backstep_count()`** -- how many caller-locked writer calls
  (`resync()`/`switch_source()`) observed the counter at or below the
  high-water mark.
- **`live_backstep_count()`** -- how many lock-free
  `try_monotonic_now`/`try_realtime_now` reads observed the counter at
  or below the currently-published reference.

Both are always `0` for an `is_monotonic == true` source whose
wraparounds all stay within the plausibility bound. A transient
(single-shot) backstep needs no caller action at all -- self-healing
already handles it; only a *persistent/frequent* pattern (rising deltas
across multiple polling windows) is actionable, typically by calling
`switch_source()` to fail over to a better-behaved counter.

## Switching the bound counter: a single-slot republish, not a timehands ring

`switch_source()` rebinds the hardware counter in place (e.g. an
unstable TSC falling back to a platform timer, or picking up a
newly-calibrated clock source) without losing monotonic continuity: it
first finalizes elapsed time against the *outgoing* counter exactly like
`resync()` would, then samples the *incoming* counter once and
republishes both clocks correlated against that same finalized
monotonic instant -- the switch behaves as one atomic combination of
"resync, then rebind". Because every field of a slot (including
`source`/`counter_hz`) is republished together under one seqlock
generation bump, a concurrent lockless reader can never observe a torn
mix of the old and new source's fields.

A multi-generation "timehands ring" (FreeBSD's `windup`-style design,
keeping several past snapshots alive so a slow reader is never forced to
retry against an in-progress writer) is deliberately not used:
writers here are already fully caller-serialized and a slot's publish is
a handful of plain stores, so the existing bounded seqlock retry
(`max_read_attempts`) has no realistic chance of being exhausted by a
source switch; a ring would only earn its complexity under very
different constraints (lock-free concurrent writers, or smoothing a
live frequency recalibration instead of a discrete switch). On any
validation/read failure, `switch_source()` leaves the manager unmodified,
still bound to the outgoing counter.

## `time_manager`'s API

```cpp
class time_manager {
public:
  static constexpr std::uint32_t max_read_attempts = 100;

  [[nodiscard]] static result<time_manager> try_create(time_source_ref counter, vdso_clock_source source,
                                                        vdso_clock_page *vdso_page = nullptr) noexcept;

  [[nodiscard]] result<void> start(duration initial_monotonic = {}) noexcept;
  [[nodiscard]] result<void> set_realtime(duration wall_clock_now) noexcept;
  [[nodiscard]] result<void> resync() noexcept;
  [[nodiscard]] result<void> switch_source(time_source_ref new_counter, vdso_clock_source new_source) noexcept;

  [[nodiscard]] result<duration> try_monotonic_now() const noexcept;
  [[nodiscard]] result<duration> try_realtime_now() const noexcept;

  [[nodiscard]] result<duration> max_resync_interval() const noexcept;
  [[nodiscard]] const time_source_capabilities &capabilities() const noexcept;
  [[nodiscard]] std::uint64_t backstep_count() const noexcept;
  [[nodiscard]] std::uint64_t live_backstep_count() const noexcept;
};
```

`time_manager` is movable (needed so `try_create` can return
`result<time_manager>` by value) but not copyable -- a `vdso_clock_slot`
embeds a `std::atomic`, and copying would silently produce two
independent instances racing to publish into the same `vdso_clock_page`.

```cpp
#include <structo/hw/time_manager.hpp>

// my_tsc_backend is illustrative; adapt your own counter via a `time_source_traits<Backend>`
// specialization, following time_source_ref.hpp's `arm_cntvct_backend` example.
my_tsc_backend tsc{};
structo::hw::time_source_ref ref(tsc);

structo::hw::vdso_clock_page page;
auto mgr = structo::hw::time_manager::try_create(ref, structo::hw::vdso_clock_source::x86_tsc, &page);
if (!mgr)
  return; // unbound counter, zero clock_hz, or vdso-incompatible capabilities

// caller holds whatever lock serializes writers for the extent of start()/resync()/set_realtime()/switch_source()
auto started = mgr->start();

// on whatever cadence mgr->max_resync_interval() recommends:
auto resynced = mgr->resync();

// lock-free, safe from any context (e.g. a scheduler tick, an IRQ handler):
auto now = mgr->try_monotonic_now();
```

## Testing

Each piece is unit-tested independently with `TEST_F`-based GoogleTest
fixtures and deterministic scripted fake backends (no real hardware
counter access):

- `tests/test_time_source_ref.cpp` -- `time_source_capabilities`
  equality, unbound-ref failure paths, bound-ref `try_now`/`capabilities`
  round-trips and error forwarding.
- `tests/test_vdso_clock_page.cpp` -- default-constructed slot state,
  per-clock storage independence, and the page/slot layout
  `static_assert`s double-checked at runtime.
- `tests/test_vdso_clock_writer.cpp` -- `vdso_clock_update_guard`'s
  odd-during-update/even-after generation protocol, field mutation, and
  move semantics (no double generation bump).
- `tests/test_vdso_clock_reader.cpp` -- round-trip reads against a
  writer-published slot, `error::not_initialized` for an unpublished
  slot, and `error::try_again` when the seqlock retry bound is exhausted
  against a writer that never completes.
- `tests/test_time_manager.cpp` -- `try_create`'s validation matrix
  (unbound counter, zero `clock_hz`, `vdso_page` incompatible with
  `is_monotonic`/`is_per_cpu`), `start`/`resync`/`set_realtime` basic
  flows, `vdso_clock_page` publish-then-external-reader round-trips,
  legitimate narrow-counter wraparound, a full-width reset-to-zero
  correctly clamped instead of producing a bogus forward jump (both via
  `resync()` and the lockless read path), `is_monotonic == false`
  backstep clamp-and-recovery, `backstep_count()`/`live_backstep_count()`
  independence, `max_resync_interval()`, `switch_source()` (basic
  frequency-changing switch, unbound/vdso-incompatible/not-yet-started
  rejection), and move construction/assignment.

## See also

- [`timer_ref.md`](timer_ref.md) -- the complementary countdown/interval-timer handle, and the `*_ref`
  type-erasure pattern `time_source_ref` follows
- `docs/coding-guide.md`'s "Type-erase a `*_ref` handle's backend behind one `vtable`" section
