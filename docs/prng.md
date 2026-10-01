<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::prng`

`include/structo/prng.hpp`

Small, fast, fully deterministic pseudo-random generators --
`splitmix64`, `xoshiro256ss` (xoshiro256**), and `pcg32` -- each directly
seedable and each with a `from_hw_rng` factory that draws its initial
state from a [`structo::hw::hw_rng_ref`](hw_rng.md).

None of these are cryptographically secure -- do not use them to
generate keys, nonces, or anything else a security boundary depends on.
They are meant for everything else a kernel/hypervisor needs randomness
for: scheduling jitter, ASLR-style slot selection, hash table seeding,
retry backoff, and test-data generation.

## Why these three

- **`splitmix64`** -- a single-64-bit-state generator with excellent
  avalanche behavior, used by every other generator in this file to
  *expand* a single 64-bit seed into its own (wider) internal state --
  the standard technique `xoshiro256ss`'s own reference implementation
  recommends. Also usable standalone when 64 bits of state is enough.
- **`xoshiro256ss`** -- 256 bits of state, `2^256 - 1` period, passes
  every standard empirical randomness test suite (BigCrush, PractRand);
  the recommended general-purpose default in this file.
- **`pcg32`** (PCG-XSH-RR 64/32) -- 64 bits of state plus a 64-bit
  stream selector, 32-bit output; smaller and faster to seed than
  `xoshiro256ss`, and its stream parameter gives cheaply-constructed,
  statistically-independent *parallel* streams from unrelated seeds
  (e.g. one stream per CPU) without needing extra entropy per stream.

## Seeding

Every generator supports two equally-direct ways to seed it:

- A fixed-seed constructor, entirely deterministic and reproducible
  (useful for tests and any scenario that explicitly wants repeatable
  output).
- `from_hw_rng(structo::hw::hw_rng_ref, ...)`, which draws the needed
  number of 64-bit words from a bound hardware RNG and returns
  `reloco::result<Self>`, propagating the first draw failure.

```cpp
#include <structo/arch/x86/hw_rng.hpp>
#include <structo/prng.hpp>

structo::arch::x86::rdrand_rng rd{};
structo::hw::hw_rng_ref hw(rd);

auto rng = structo::prng::xoshiro256ss::from_hw_rng(hw);
if (rng) {
  std::uint64_t word = rng->next();
  // ...
}
```

## API

```cpp
class splitmix64 {
public:
  constexpr explicit splitmix64(std::uint64_t seed) noexcept;
  static result<splitmix64> from_hw_rng(const hw::hw_rng_ref &, std::uint32_t max_retries = ...) noexcept;
  constexpr std::uint64_t next() noexcept;
};

class xoshiro256ss {
public:
  constexpr explicit xoshiro256ss(std::uint64_t seed) noexcept; // expands via splitmix64
  static result<xoshiro256ss> from_hw_rng(const hw::hw_rng_ref &, std::uint32_t max_retries = ...) noexcept; // 4 direct draws
  constexpr std::uint64_t next() noexcept;
};

class pcg32 {
public:
  constexpr explicit pcg32(std::uint64_t seed, std::uint64_t sequence = 1) noexcept;
  static result<pcg32> from_hw_rng(const hw::hw_rng_ref &, std::uint32_t max_retries = ...) noexcept; // seed + sequence draws
  constexpr std::uint32_t next() noexcept;
};
```

Note the one deliberate asymmetry: `xoshiro256ss::xoshiro256ss(seed)`
*expands* a single 64-bit seed into all 4 state words via `splitmix64`
(the standard, well-tested technique for this generator specifically),
while `xoshiro256ss::from_hw_rng` draws 4 words *directly* from the
hardware RNG instead -- every word is independently drawn hardware
entropy, no expansion needed since real entropy is already available.

## Testing

Every generator is unit-tested (`tests/test_prng.cpp`) for seed
reproducibility (same seed -> same sequence), seed/sequence sensitivity
(different seeds/streams -> different output), short-run distinctness,
and `from_hw_rng`'s success and error-propagation paths against a
deterministic fake `hw_rng_traits` backend. `splitmix64` is additionally
checked against its well-known public-domain reference implementation's
first few outputs for seed `0`.

## See also

- [`hw_rng.md`](hw_rng.md) -- the hardware entropy source these generators seed from
