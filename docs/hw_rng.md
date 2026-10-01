<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::hw_rng_ref`

`include/structo/hw/rng.hpp`, `include/structo/hw/rng_combinator.hpp`,
`include/structo/arch/{x86,arm64,arm,riscv}/hw_rng.hpp`

A type-erased, non-owning handle over a hardware random/entropy source
(`RDRAND`/`RDSEED`, `RNDR`/`RNDRRS`, the RISC-V Zkr `seed` CSR, ...), plus
the `hw_rng_traits<Backend>` customization point a concrete backend
specializes to be bindable through it. Follows the same single-`vtable`,
resolved-once-per-`Backend` shape as [`uart_ref`](uart_ref.md) -- see
`docs/coding-guide.md`'s "Type-erase a `*_ref` handle's backend behind
one `vtable`" section.

## Why this exists separately from `structo::prng`

A real hardware RNG instruction only ever answers "here are a few raw
entropy bits" -- small, slow by CPU standards, and sometimes transiently
unavailable. It is not meant to be drawn from in a tight loop for every
random byte a program needs. [`structo::prng`](prng.md) instead provides
fast, portable pseudo-random generators, *seeded once* from a
`hw_rng_ref` at startup -- the general off-the-shelf
CSPRNG/PRNG pattern, kept here as two separate, independently testable
layers.

## Customization point: `hw_rng_traits<Backend>`

Left undefined for any `Backend` that hasn't opted in. A specialization
must supply exactly one function:

```cpp
template <> struct structo::hw::hw_rng_traits<my_backend> {
  static reloco::result<std::uint64_t> try_generate64(my_backend &) noexcept;
};
```

`try_generate64` draws one 64-bit word of hardware randomness. It fails
with `error::try_again` if the hardware reports a transient "not ready
yet" condition (e.g. `RDRAND`/`RDSEED`'s carry flag clear, `RNDR`'s
`PSTATE.Z` set, the Zkr `seed` CSR's `OPST` reporting `WAIT`/`BIST`) --
`hw_rng_ref` retries on exactly this error, up to a caller-chosen bound,
never on any other error (assumed non-transient).

Optionally, a backend may also supply `is_available`:

```cpp
static bool is_available(my_backend &) noexcept;
```

Detected via SFINAE; if absent, `hw_rng_ref::is_available()` reports
`true` whenever bound.

## `hw_rng_ref`'s API

```cpp
class hw_rng_ref {
public:
  static constexpr std::uint32_t default_max_retries = 16;

  constexpr hw_rng_ref() noexcept;                       // unbound
  template <typename Backend> explicit hw_rng_ref(Backend &) noexcept;

  constexpr explicit operator bool() const noexcept;
  bool is_available() const noexcept;

  result<std::uint64_t> try_generate64(std::uint32_t max_retries = default_max_retries) const noexcept;
  result<std::uint32_t> try_generate32(std::uint32_t max_retries = default_max_retries) const noexcept;
  result<void> try_fill(span<std::byte> dst, std::uint32_t max_retries_per_word = default_max_retries) const noexcept;
};
```

A default-constructed (or default-constructed-copied) `hw_rng_ref` is
*unbound*: every operation fails with `error::unsupported_operation`
rather than trapping, mirroring `io_space_ref`/`uart_ref`'s null-safety
convention.

```cpp
#include <structo/arch/x86/hw_rng.hpp>
#include <structo/hw/rng.hpp>

structo::arch::x86::rdrand_rng rd{};
structo::hw::hw_rng_ref rng(rd);

if (rng.is_available()) {
  auto word = rng.try_generate64();
  if (word) {
    // ...
  }
}
```

## Per-architecture backends

| Header | Backend(s) | Notes |
|---|---|---|
| `arch/x86/hw_rng.hpp` | `rdrand_rng`, `rdseed_rng` | `CPUID`-based `is_available()` (`ECX.RDRAND[30]` / `EBX.RDSEED[18]`); carry flag (`CF`) reports success |
| `arch/arm64/hw_rng.hpp` | `rndr_rng`, `rndrrs_rng` | `ID_AA64ISAR0_EL1.RNDR`-based `is_available()`; `PSTATE.Z == 0` reports success |
| `arch/arm64/hw_rng.hpp`, `arch/arm/hw_rng.hpp` | `cntpct_rng`, `cntvct_rng` | **Weak fallback only** -- see below |
| `arch/riscv/hw_rng.hpp` | `seed_rng` | Accumulates four 16-bit `ES16` samples per 64-bit draw per the Zkr `seed` CSR's `csrrw`-swap-with-zero protocol; `OPST == DEAD` reports `error::io_error` (unrecoverable, not retried) |

Every backend is a zero-sized tag type (no state of its own); each only
compiles its real asm for its own target architecture (`__i386__`/
`__x86_64__`, `__aarch64__`, `__arm__`, `__riscv`), so cross-compiled
header checks stay clean everywhere else.

ARMv7-A (`arm`) has no baseline architectural hardware-RNG instruction --
a genuine entropy source there is always a platform-specific TRNG
peripheral, out of scope for this library (wire it up with your own
`hw_rng_traits` specialization, following `arch/arm/hw_rng.hpp` as a
layout template).

### Fallback: `CNTPCT`/`CNTVCT` jitter combiners

`cntpct_rng`/`cntvct_rng` (both ARM64 and ARM32) are **not** hardware
entropy sources -- the Generic Timer's physical and virtual counters are
ordinary monotonic counters, fully predictable to any observer who
roughly knows the time. They exist purely as a *last-resort fallback*
for cores with no real hardware entropy source at all: each draw reads
the counter, spins briefly, reads it again, and mixes both readings plus
their delta through `hw::detail::avalanche_mix64`. The delta captures a
small amount of genuine execution-timing jitter (cache misses, bus
contention, interrupts) -- weak, but not zero; the raw counter values
contribute no real entropy on their own.

**Never use `cntpct_rng`/`cntvct_rng` alone.** Combine them with at least
one real entropy source (or several other independent weak sources) via
`hw_rng_combinator`, below. Nothing in this library reaches for them
automatically -- using them is always an explicit, opt-in choice.

## `hw_rng_combinator`: combining several sources

`include/structo/hw/rng_combinator.hpp`

```cpp
using namespace structo::arch::arm64;
using namespace structo::hw;

rndr_rng rndr{};
cntpct_rng ct{};
cntvct_rng cv{};

std::array<hw_rng_ref, 3> sources{hw_rng_ref(rndr), hw_rng_ref(ct), hw_rng_ref(cv)};
hw_rng_combinator combo(sources);

auto word = combo.try_generate64(); // mixes whichever sources succeed

// A combinator can itself be bound through another hw_rng_ref:
hw_rng_ref ref(combo);
```

Every bound source is drawn from once per `try_generate64()` call (its
own small per-source retry budget); every *successful* draw is
`XOR`-folded into an accumulator, then run through
`avalanche_mix64` to spread whatever entropy any one source contributed
across every output bit and remove linear structure a weak source might
otherwise leave behind. A source that fails this round (including a weak
fallback source simply absent on this core) is skipped rather than
aborting the whole draw -- the combined draw only fails if *every*
source failed, reporting whichever source's error was observed last.

This is a simple, practical mixer (XOR-fold plus an avalanche
finalizer), not a formally-proven randomness extractor: it cannot
manufacture entropy that wasn't present in at least one successful
source draw. Prefer feeding it at least one genuine hardware entropy
source; weak-only combinations should be a last resort, not a routine
choice. `hw_rng_combinator` does not own its sources -- the
`span<const hw_rng_ref>` passed to its constructor, and every backend
any of those refs is bound to, must outlive it.

## Testing

`hw_rng_ref`, per-backend `try_generate64`/`is_available` logic, and
`hw_rng_combinator` are unit-tested against deterministic fake backends
(`tests/test_hw_rng.cpp`, `tests/test_hw_rng_combinator.cpp`). The real
per-architecture instructions/registers/CSRs themselves are exercised
only by cross-compiling each backend header for its target architecture
(`-fsyntax-only`) plus a manual runtime check of the x86 backend (the
only one unprivileged and host-executable in this environment) --
matching the precedent set by `mmu_regs.hpp`'s `read()`/`write()`.

## See also

- [`uart_ref.md`](uart_ref.md) -- the `*_ref` type-erasure pattern this header follows
- [`prng.md`](prng.md) -- the fast pseudo-random generators meant to be seeded from this handle
- [`mmu_regs.md`](mmu_regs.md) -- another family of per-architecture, asm-gated headers with the same cross-compile-clean-elsewhere convention
