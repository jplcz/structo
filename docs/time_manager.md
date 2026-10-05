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

## Integrating with `reloco::instant`: `instant_clock_traits.hpp`

`include/structo/hw/instant_clock_traits.hpp` bridges a kernel's `time_manager` to
[reloco](https://github.com/jplcz/reloco)'s `reloco::instant_clock_traits<Tag>` customization point, so kernel
code can call `reloco::instant_clock_traits<Tag>::now()` directly (not `reloco::instant::now()` --
see below) and get a `reloco::duration` backed by `time_manager::try_monotonic_now()`/`try_realtime_now()`.

### `kernel_time_manager()`: a kernel-supplied getter, not an internal global

`instant_clock_traits<Tag>::now()` is a stateless, zero-argument, `noexcept` static function with no way to
receive a reference to *which* `time_manager` to read from. Rather than own a hidden global/singleton, the
header declares `structo::hw::kernel_time_manager()` as a customization point with **no definition** --
exactly one definition, returning a reference to whatever `time_manager` the embedding kernel already
maintains (already `start()`-ed before any use), must be linked into the final binary:

```cpp
structo::hw::time_manager &structo::hw::kernel_time_manager() noexcept {
  static structo::hw::time_manager instance = /* ... */;
  return instance;
}
```

Two tags select which `time_manager` accessor is read: `kernel_monotonic_clock_tag`
(`try_monotonic_now()`) and `kernel_realtime_clock_tag` (`try_realtime_now()`).

### Failure handling: `time_manager_clock_failure_policy<Tag>`, another opt-in customization point

`try_monotonic_now()`/`try_realtime_now()` are fallible, but `instant_clock_traits<Tag>::now()` must return a
bare `reloco::duration` unconditionally, and typical callers never expect (or check for) a failure at all.
There is no single right answer -- it depends on the architecture's own counter guarantees -- so, mirroring
every other `*_traits<Tag>`-style hook in `structo` (`time_source_traits`, `hw_rng_traits`), this is itself
**left undefined** until the embedding kernel specializes it for whichever tag it uses:

```cpp
template <> struct structo::hw::time_manager_clock_failure_policy<structo::hw::kernel_monotonic_clock_tag> {
  // Called only once try_monotonic_now()/try_realtime_now() has itself already failed; must still return
  // some duration unconditionally (never throws/aborts/returns a result<>).
  static reloco::duration recover(structo::hw::time_manager &mgr, reloco::error err) noexcept;
  // Called on every *successful* read instead, so a policy wanting a fallback cache can keep one warm; a
  // policy that never needs one can leave this a no-op.
  static void observe(reloco::duration value) noexcept;
};
```

A kernel that uses either tag without specializing this gets an "incomplete type" compile error at the call
site, not a silently-chosen default -- and, since `instant_clock_traits<Tag>::now()`'s body is a (dummy-)
template rather than an ordinary member function, that completeness check is deferred to each call site
rather than forced as soon as the header is merely included. Both `recover`/`observe` are called on whichever
`time_manager_clock_failure_policy<Tag>` is actually active for `Tag` -- never unconditionally on some other,
unrelated template -- so a `Tag` that doesn't want a fallback cache pays no cost for one.

Two ready-made policies implementing both are provided for convenience:

- `trap_time_failure_policy<Tag>` -- its `recover` traps unconditionally via `RELOCO_ASSERT`, since a
  fabricated fallback duration (zero, stale/cached, or otherwise) can silently violate monotonicity/
  invariants elsewhere just as badly as a crash; `Tag`s with no real recovery path are treated as a
  genuine, unrecoverable condition rather than something to paper over.
- `retry_spin_timer_policy<Tag>` -- its `recover` retries `try_monotonic_now()`/`try_realtime_now()` in a
  tight, unbounded loop until it succeeds, never fabricating or caching a substitute. Appropriate only when
  the underlying failure really is expected to be transient (e.g. a losing seqlock race) and there is no
  sibling clock source to fail over to in the first place -- which, in practice, covers most ARM and RISC-V
  targets: both architectures specify essentially one architectural counter (ARM's generic timer; RISC-V's
  `time` CSR/`mtime`), so a broken/absent counter is normally handled one layer up (the whole counter is
  declared unusable system-wide, falling back to a slower tick-based source) rather than by switching to a
  sibling source from inside `recover()`.

Opt in by having your own `time_manager_clock_failure_policy<Tag>` specialization inherit from whichever
fits, rather than reimplementing the same trap/retry loop by hand -- a custom policy (e.g. an x86 one-shot
`switch_source()` retry) can also fall back to `trap_time_failure_policy` as a last resort once it has
genuinely exhausted its own recovery options. Because this inheritance is explicit, both policies are only
ever instantiated for a `Tag` that actually asks for one -- never implicitly for every `Tag` in existence.
See the header's own file-level docs for worked ARM/x86 examples.

### Why not `RELOCO_INSTANT_CLOCK_TAG`/`reloco::instant::now()`?

This header does not set `RELOCO_INSTANT_CLOCK_TAG` or otherwise wire itself into `reloco::instant::now()`.
Kernel code is expected to call `reloco::instant_clock_traits<kernel_monotonic_clock_tag>::now()` (or the
realtime tag) directly instead. `RELOCO_INSTANT_CLOCK_TAG` only matters to generic code that reads a POSIX-style
instant through `reloco::instant` itself; redefining it differently across translation units of the same
binary would also risk an ODR violation on `reloco::instant`'s own inline method bodies.

### Testing

`tests/test_instant_clock_traits.cpp` supplies the test binary's one definition of `kernel_time_manager()`
(via a test-settable static slot) and exercises: `trap_time_failure_policy` trapping (`EXPECT_DEATH`) both
directly and through the real tags' `instant_clock_traits` specializations on failure; both real tags reading
through a started `time_manager` on success; and `retry_spin_timer_policy`'s `recover()` against an
already-working `time_manager`.

## See also

- [`timer_ref.md`](timer_ref.md) -- the complementary countdown/interval-timer handle, and the `*_ref`
  type-erasure pattern `time_source_ref` follows
- `docs/coding-guide.md`'s "Type-erase a `*_ref` handle's backend behind one `vtable`" section
