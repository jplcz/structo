<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `early_rendezvous_wait<Traits>` / `structo_early_rendezvous_wait`

`include/structo/sync/early_rendezvous_barrier.h` (plain C, C99+)
`include/structo/sync/early_rendezvous_barrier.hpp` (C++ convenience wrapper)

The earliest-boot sibling of
[`core_rendezvous_barrier`](core_rendezvous_barrier.md): a trivial,
GCC/Clang-only (`__atomic_*` builtins, no `std::atomic`, no
`<stdatomic.h>`), purely-static-data SMP rendezvous barrier intended
for use *before* `.init_array`/global constructors have run, possibly
before any stack beyond a per-core startup stack exists, and certainly
before `new`/an allocator is available.

There is no instance, no constructor, and no class at all on the C
side -- just `struct structo_early_rendezvous_state` (two plain
`unsigned int`s) and one `static inline` function operating on a
pointer to it:

```c
#include <structo/sync/early_rendezvous_barrier.h>

static struct structo_early_rendezvous_state g_boot_barrier; /* zero-initialized by the language */

static void arch_spin_hint(void) { __asm__ __volatile__("yield" ::: "memory"); }

void secondary_core_entry(size_t num_cores) {
  const int is_leader = structo_early_rendezvous_wait(&g_boot_barrier, num_cores, arch_spin_hint, NULL, NULL);
  if (is_leader) {
    /* Exactly one arbitrarily-chosen core runs this once per wave. */
  }
}
```

A tentative definition of `structo_early_rendezvous_state` at
static/global storage duration (C or C++, `extern` or not) is
zero-initialized by the language long before any dynamic
initialization could run, so a secondary core that jumps directly into
this function with no guarantee any constructor anywhere has executed
yet can still call it safely. The caller owns the storage -- typically
a single global, optionally placed into a specific linker section via
`__attribute__((section(...)))` if it must live in identity-mapped or
uncached memory before the MMU/cache is configured identically on
every core -- this header never allocates or owns any state itself.

The C++ header adds no storage of its own; it is a thin wrapper
trading `structo_early_rendezvous_wait`'s raw `void (*)(void)` /
`void (*)(size_t, void *)` callback pair for an ordinary (optionally
capturing) callable:

```cpp
#include <structo/sync/early_rendezvous_barrier.hpp>

struct early_boot_rendezvous_traits {
  static void spin_wait() noexcept { asm volatile("yield" ::: "memory"); }
};

static structo_early_rendezvous_state g_boot_barrier{};

const bool is_leader = structo::sync::early_rendezvous_wait<early_boot_rendezvous_traits>(
    g_boot_barrier, num_cores, [](std::size_t arrived) {
      // Invoked once per spin iteration on every non-leader core.
      service_pending_ipis();
    });
```

`Traits` supplies exactly one hook, the same contract
`core_rendezvous_barrier::Traits` uses:

- `static void spin_wait() noexcept;` -- invoked once per spin
  iteration *after* the caller's own on-spin callback.

Like `core_rendezvous_barrier`, a generation counter makes the barrier
reusable across an unbounded number of waves, and
`structo_early_rendezvous_wait()`/`early_rendezvous_wait<Traits>()`
return nonzero/`true` for exactly one arbitrarily-chosen participant
per wave (the "leader" -- the one whose arrival completed it), `0`/
`false` for every other participant in the same wave, matching
`core_rendezvous_barrier::wait()`'s own leader-election convention.
`num_cores` is a plain runtime argument every call (not stored
anywhere -- there is no instance to store it in); every participant for
one wave must pass the same value. `0` is treated as `1`.

Because every participant explicitly supplies the state pointer/
reference, the same `Traits` spin policy can back any number of
independent barrier instances in the same binary -- unlike a
hidden-singleton design, there is no one-storage-per-`Traits`
limitation.

Assembly-callable `.macro`-based variants (ARM, AArch64, RISC-V,
x86/x86_64) for use from hand-written `.S` boot trampolines before any
C environment exists are deferred; this header currently ships C and
C++ variants only.

See also: [`core_rendezvous_barrier.md`](core_rendezvous_barrier.md), [`irq_guard.md`](irq_guard.md), [`core_pin_guard.md`](core_pin_guard.md), [`preemption_guard.md`](preemption_guard.md).
