// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

// A toy "callout subsystem" demonstrating `structo::callout` end-to-end:
// a single-core, deadline-ordered `reloco::c_tailq` of intrusive
// `hook_type` links -- not a real timer wheel, not thread/interrupt-safe,
// and not part of the public `structo` API -- that implements the two
// mandatory `callout` subsystem operations (`submit`/`cancel`) over a
// simulated software clock driven entirely by `advance_to()`, standing
// in for whatever would otherwise drive it on real hardware (a periodic
// timer interrupt, a hardware timer wheel's own expiry IRQ, ...).
//
// This is exactly what `callout.hpp`'s optional `Subsystem::hook_type`
// customization point exists for: `hook_type` is embedded directly
// inside every `async_kernel_object<toy_callout_scheduler, N>` (at a
// fixed, `N`-independent sub-object address), so a *single*
// `reloco::c_tailq<hook_type, &hook_type::link>` can intrusively link
// together every live `callout<toy_callout_scheduler, N>` -- for any
// mix of `N` -- with zero allocation and no fixed slot-count limit,
// `TAILQ_REMOVE`-style O(1) `cancel`. A real subsystem would replace the
// single sorted queue below with actual wheel buckets; the intrusive-
// hook technique is the same either way.

#include <structo/callout.hpp>

#include <reloco/intrusive_c_tailq.hpp>

#include <cstdint>
#include <cstdio>

namespace {

using reloco::duration;
using reloco::result;

// A single-core, intrusively-linked, deadline-ordered queue standing in
// for a real timer wheel. "Single-core" here means exactly one logical
// scheduler for the whole program (a `static` singleton), matching a toy
// single-core kernel's single timer-interrupt context -- not safe to
// `submit`/`cancel` concurrently with `advance_to()` from another core
// without external locking, exactly like `async_kernel_object`/`callout`
// themselves document.
class toy_callout_scheduler {
public:
  // `Subsystem::hook_type`: the intrusive link `callout.hpp` embeds
  // inside every `async_kernel_object<toy_callout_scheduler, N>`, laid
  // out exactly like a `TAILQ_ENTRY(hook_type)` (see
  // `intrusive_c_tailq.hpp`), plus the type-erased "who to fire, and
  // how" this scheduler needs per entry.
  struct hook_type {
    struct {
      hook_type *next = nullptr;
      hook_type **prev = nullptr;
    } link;
    void *obj = nullptr;
    void (*fire)(void *) noexcept = nullptr;
    duration deadline{};
  };

  // The two mandatory `callout` subsystem operations -- see
  // `callout.hpp`'s file-level docs for the exact contract. `Obj` is
  // whatever `async_kernel_object<toy_callout_scheduler, N>` the owning
  // `callout<toy_callout_scheduler, N>` wraps; `self.hook()` is reachable
  // only from here (`friend Traits` in `async_kernel_object.hpp`).
  template <typename Obj> static result<void> submit(Obj &self, duration period) noexcept {
    hook_type &h = self.hook();
    h.obj = &self;
    h.fire = [](void *ctx) noexcept { static_cast<Obj *>(ctx)->fire(); };
    h.deadline = now_ + period;

    // Keep the queue sorted ascending by deadline (an O(n) insertion
    // scan -- a real timer wheel would bucket by deadline instead of
    // sorting a flat list): splice `h` in right before the first entry
    // due no earlier than it.
    for (auto &existing : pending_) {
      if (h.deadline < existing.deadline) {
        pending_.insert_before(existing, h);
        return {};
      }
    }
    pending_.push_back(h);
    return {};
  }

  template <typename Obj> static result<void> cancel(Obj &self) noexcept {
    // O(1): `TAILQ_REMOVE`-equivalent, no scan needed -- the whole point
    // of an intrusive link over the flat-array/pointer-scan alternative.
    pending_.remove(self.hook());
    return {};
  }

  // Advances the simulated clock to @p now and fires every entry whose
  // deadline has elapsed, in deadline order. Each entry is popped off
  // the front -- unlinking it -- *before* firing (mirroring
  // `async_kernel_object::fire` itself already having atomically claimed
  // `pending`), so a callback re-arming itself (e.g. via
  // `callout::reset_periodic`) safely re-inserts the same, already-
  // unlinked `hook_type` rather than racing this loop.
  static void advance_to(duration now) noexcept {
    now_ = now;
    while (!pending_.empty() && pending_.front()->deadline <= now_) {
      hook_type *h = pending_.pop_front();
      h->fire(h->obj);
    }
  }

private:
  static inline reloco::c_tailq<hook_type, &hook_type::link> pending_{};
  static inline duration now_{};
};

using toy_callout = structo::callout<toy_callout_scheduler>;

} // namespace

int main() {
  toy_callout heartbeat;
  toy_callout one_shot;
  int heartbeat_count = 0;

  // Periodic: re-arms itself every 10ms until `stop()`/`deactivate()`.
  (void)heartbeat.reset_periodic(duration::from_millis(10), [&heartbeat_count](toy_callout &) noexcept {
    ++heartbeat_count;
    std::printf("heartbeat #%d\n", heartbeat_count);
  });

  // One-shot: fires exactly once, then stays idle (not re-armed).
  (void)one_shot.reset(duration::from_millis(25), [](toy_callout &) noexcept { std::printf("one-shot fired\n"); });

  // Drive the simulated clock forward in 5ms steps, standing in for
  // whatever real timer interrupt would call `advance_to()` on actual
  // hardware.
  for (std::uint64_t ms = 5; ms <= 40; ms += 5) {
    std::printf("-- t=%llums --\n", static_cast<unsigned long long>(ms));
    toy_callout_scheduler::advance_to(duration::from_millis(ms));
  }

  (void)heartbeat.drain(); // safe to let `heartbeat`/`one_shot` go out of scope afterwards
  (void)one_shot.drain();

  std::printf("total heartbeats: %d\n", heartbeat_count);
  return 0;
}
