<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::hw_rng_ref` (`hw/rng.hpp`)

`include/structo/hw/rng.hpp`

The abstraction-only header of the hardware-RNG layer: a type-erased,
non-owning handle `hw_rng_ref` over a hardware entropy source
(`RDRAND`/`RDSEED`, `RNDR`/`RNDRRS`, the RISC-V Zkr `seed` CSR, ...) and
the `hw_rng_traits<Backend>` customization point a backend specializes.
It contains no instruction encodings; concrete backends live under
`arch/{x86,arm64,riscv}/hw_rng.hpp` or are test fakes. For the full
overview, why it is separate from [`prng`](prng.md), and the backends,
see [`hw_rng`](hw_rng.md).

## Usage

```cpp
#include <structo/hw/rng.hpp>

// A deterministic fake backend, e.g. for tests.
struct fake_rng { std::uint64_t next = 1; };

// Opt in: specialize the (otherwise undefined) traits for the backend.
template <> struct structo::hw::hw_rng_traits<fake_rng> {
  // Mandatory. Returns one 64-bit word, or error::try_again for a
  // transient "not ready yet" (hw_rng_ref retries on exactly that
  // error), or another error for a non-transient failure.
  static reloco::result<std::uint64_t> try_generate64(fake_rng &b) noexcept { return b.next++; }

  // Optional (SFINAE-detected). Reports whether the instruction/CSR is
  // really present on this core. If omitted, a bound ref reports true.
  static bool is_available(fake_rng &) noexcept { return true; }
};

void seed_from_hw() {
  fake_rng backend;

  // Bind by lvalue reference; `backend` must outlive the ref and all its
  // copies. Rvalue bindings are deleted. A default-constructed ref is
  // unbound and every operation on it fails with
  // error::unsupported_operation (or is_available() returns false).
  structo::hw::hw_rng_ref rng(backend);

  if (!rng.is_available())
    return;

  // Argument: max_retries -- how many times to re-draw while the backend
  // returns error::try_again (default hw_rng_ref::default_max_retries
  // == 16). Fails with try_again once exhausted.
  auto w = rng.try_generate64(32);

  // Low 32 bits of one try_generate64 draw; same max_retries meaning.
  auto h = rng.try_generate32();

  // Fills `buf` with one 64-bit draw per 8 bytes (or part). The argument
  // after the span is max_retries_per_word. On failure, stops and
  // returns the error, leaving buf partly filled.
  std::byte buf[24];
  auto r = rng.try_fill(buf, 8);
  (void)w; (void)h; (void)r;
}
```

## API

| Member | Description |
|---|---|
| `hw_rng_traits<Backend>` | Customization point; `try_generate64(Backend&)` required, `is_available(Backend&)` optional. |
| `hw_rng_ref::default_max_retries` | `16`. |
| `hw_rng_ref::vtable` | `{ generate64, is_available }` function pointers, one table per bound `Backend`. |
| `explicit hw_rng_ref(Backend &)` | Binds a backend that has a `hw_rng_traits` specialization. |
| `explicit operator bool()` | Whether bound. |
| `bool is_available() const` | `false` if unbound; backend's probe, or `true` if it has none. |
| `result<uint64_t> try_generate64(max_retries = 16) const` | Draws a word, retrying on `error::try_again`; `unsupported_operation` if unbound. |
| `result<uint32_t> try_generate32(max_retries = 16) const` | Low half of one 64-bit draw. |
| `result<void> try_fill(span<std::byte>, max_retries_per_word = 16) const` | Fills a buffer; partial on failure. |
| `detail::avalanche_mix64(uint64_t)` | `constexpr` splitmix64 finalizer for scrambling weak 64-bit values; mixes only, adds no entropy. |

See also: [`hw_rng`](hw_rng.md), [`prng`](prng.md), [`uart_ref`](uart_ref.md).
