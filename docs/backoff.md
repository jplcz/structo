<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::sync::backoff`

`include/structo/sync/backoff.hpp`

A tiny, stack-only exponential-backoff state machine for spin-wait loops
polling externally-owned state (an atomic flag, a generation counter, a
ring slot, ...). It extracts the "capped, doubling burst of
`reloco::hint::spin_loop()` between polls" pattern that
`ipi_message::wait()` in `arch/ipi_dispatcher.hpp` uses.

A tight loop re-polling one atomic every iteration keeps issuing loads
against the cache line another core needs exclusively in order to write
its update, slowing the very write being awaited. Backing off cuts that
read traffic once a wait proves not to resolve instantly, while still
reacting immediately (a single spin hint) when it does.

Defaults: the burst starts at 1 and doubles up to 1024 spin hints per
`spin()`. Change the bounds only when a call site has measured a reason.

It is non-atomic and not shareable: one instance per spinning
thread/core. Contrast with the lock-level spin waits in
[`ticket_spin_lock`](ticket_spin_lock.md) and
[`queue_spin_lock`](queue_spin_lock.md).

## Usage

```cpp
#include <structo/sync/backoff.hpp>

void wait_for_ready(const std::atomic<bool> &ready) {
  // Defaults: first spin() issues 1 spin_loop() hint, then 2, 4, ... up to
  // 1024 per call. One instance per waiting thread; never share it.
  structo::sync::backoff bo;
  while (!ready.load(std::memory_order_acquire)) {
    bo.spin(); // one backoff burst, then the next burst doubles (capped)
  }
}

void wait_custom(const std::atomic<int> &gen, int seen) {
  // Custom bounds: first argument is initial_spins (hints in the first
  // spin()), second is max_spins (cap the burst never grows past).
  structo::sync::backoff bo(4, 256);
  while (gen.load(std::memory_order_acquire) == seen) {
    bo.spin();
  }
  // Reuse the same instance for a separate, later wait: restores the
  // burst to initial_spins (4 here).
  bo.reset();
}

void wait_oneliner(const std::atomic<bool> &ready) {
  // Convenience form: builds a default backoff internally and spins
  // until the predicate returns true. Pred must be invocable as bool().
  structo::sync::backoff::spin_until([&] { return ready.load(std::memory_order_acquire); });
}
```

## API

| Member | Description |
|---|---|
| `constexpr backoff(uint32_t initial_spins = 1, uint32_t max_spins = 1024) noexcept` | `explicit`. Sets the first burst and the cap. |
| `void spin() noexcept` | Issues the current burst of `reloco::hint::spin_loop()`, then doubles it (capped at `max_spins`). |
| `void reset() noexcept` | Returns the burst to `initial_spins`. |
| `constexpr uint32_t pending_spins() const noexcept` | Burst the next `spin()` will perform (`[[nodiscard]]`). |
| `template <typename Pred> static void spin_until(Pred &&pred)` | Spins with a default `backoff` until `pred()` is true; `noexcept` iff `Pred` is nothrow-invocable. |
