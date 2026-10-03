<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::otp_storage_ref`

`include/structo/hw/otp_storage.hpp`

A type-erased, non-owning handle over hardware one-time-programmable
(OTP) persistent storage -- an eFuse/antifuse array (e.g.
Allwinner/Rockchip SoC eFuse controllers, NXP OCOTP, a RISC-V OpenTitan
OTP controller) or a flash-sector-backed OTP partition (e.g. STM32's
OTP area, physically regular flash but logically presented as
write-once via a dedicated lock mechanism) -- plus the
`otp_storage_traits<Backend>` customization point a concrete backend
specializes to be bindable through it. Follows the same single-`vtable`,
resolved-once-per-`Backend` shape as [`hw_rng_ref`](hw_rng.md) -- see
`docs/coding-guide.md`'s "Type-erase a `*_ref` handle's backend behind
one `vtable`" section.

## The one universal invariant: programming is OR-only and irreversible

Every OTP/eFuse technology -- electrical fuses, antifuses, or a
flash-based OTP partition -- shares one physical contract regardless of
vendor: a "blank"/erased bit can be *programmed* ("burned"/"blown")
from its blank state to its programmed state exactly once, and never
back. There is no erase operation. Programming a byte therefore never
*overwrites* it -- it bitwise-**ORs** the requested bits into whatever
is already there (conventionally blank = `0`, programmed = `1`; a few
vendors invert this polarity, which a backend's own documentation
covers, not this header). Calling `try_program` a second time with
overlapping bits is not an error -- any bit already `1` simply stays `1`
-- but there is no way to ask for a bit to go back to `0`.

This is also why, unlike [`hw_rng_ref::try_generate64`](hw_rng.md),
neither `try_read` nor `try_program` here retries automatically on a
transient error: a spurious retry of an idempotent entropy draw is
harmless, but silently retrying a program pulse on storage that can
never be un-burned is not something this header will ever decide on a
caller's behalf. A caller that wants a retry loop (e.g. around a
`error::busy` from a programming-voltage pump still charging) is
expected to write it explicitly.

## Customization point: `otp_storage_traits<Backend>`

Left undefined for any `Backend` that hasn't opted in. A specialization
must supply:

```cpp
template <> struct structo::hw::otp_storage_traits<my_backend> {
  static std::size_t size_bytes(my_backend &) noexcept;
  static reloco::result<void> try_read(my_backend &, std::size_t offset,
                                       reloco::span<std::byte> dst) noexcept;
  static reloco::result<void> try_program(my_backend &, std::size_t offset,
                                          reloco::span<const std::byte> bits) noexcept;
};
```

`size_bytes` reports the storage's total addressable size; any range
beyond it is rejected by `otp_storage_ref` itself with
`error::out_of_range` before the backend is ever called. `try_program`
fails with `error::permission_denied` if any byte in range falls in a
previously-locked region, or whatever other error the backend's
hardware itself reports (e.g. `error::busy` while a
programming-voltage pump is still charging).

Optionally, a backend may also supply region locking, for hardware
that supports permanently write-protecting part of its OTP storage
independently of whether every bit in it has been programmed yet:

```cpp
static bool is_locked(my_backend &, std::size_t offset, std::size_t len) noexcept;
static reloco::result<void> try_lock(my_backend &, std::size_t offset, std::size_t len) noexcept;
```

and whether the storage is actually accessible right now:

```cpp
static bool is_available(my_backend &) noexcept;
```

All three are detected via SFINAE. If `is_locked`/`try_lock` are
absent, `otp_storage_ref::is_locked()` always reports `false` and
`try_lock()` always fails with `error::unsupported_operation` -- the
backend has no concept of sub-region locking, not that everything is
always unlocked forever. If `is_available` is absent,
`otp_storage_ref::is_available()` reports `true` whenever bound.

## `otp_storage_ref`'s API

```cpp
class otp_storage_ref {
public:
  constexpr otp_storage_ref() noexcept;                       // unbound
  template <typename Backend> explicit otp_storage_ref(Backend &) noexcept;

  constexpr explicit operator bool() const noexcept;
  std::size_t size_bytes() const noexcept;
  bool is_available() const noexcept;
  bool is_locked(std::size_t offset, std::size_t len) const noexcept;

  result<void> try_read(std::size_t offset, span<std::byte> dst) const noexcept;
  result<void> try_program(std::size_t offset, span<const std::byte> bits) const noexcept;
  result<void> try_lock(std::size_t offset, std::size_t len) const noexcept;

  result<void> try_program_verify(std::size_t offset, span<const std::byte> bits,
                                  span<std::byte> scratch) const noexcept;
};
```

A default-constructed (or default-constructed-copied) `otp_storage_ref`
is *unbound*: every operation fails with `error::unsupported_operation`
rather than trapping, mirroring `hw_rng_ref`/`io_space_ref`/`uart_ref`'s
null-safety convention.

`try_program_verify` is a generic convenience synthesized purely from
`try_program`/`try_read` -- no further backend support is required. It
programs `bits` at `offset`, then reads the same range back and
confirms every requested `1` bit actually reads back as `1`: the
read-back-and-verify step most real eFuse/OTP programming procedures
mandate, since a programming pulse can fail to fully burn a bit without
the controller itself reporting an error. The verify compares
`(readback[i] & bits[i]) == bits[i]` per byte, *not*
`readback[i] == bits[i]`: other bits already programmed by an earlier,
unrelated `try_program` call in the same byte are expected and not a
verification failure, only a requested bit that failed to take is.

## Example

```cpp
using namespace structo::hw;

my_efuse_controller ctrl;
otp_storage_ref otp(ctrl);

std::array<std::byte, 4> serial_bits{/* ... */};
std::array<std::byte, 4> scratch{};
auto burned = otp.try_program_verify(/*offset=*/0, serial_bits, scratch);
if (!burned) {
  // handle error::permission_denied (locked), error::busy (pump still
  // charging), error::io_error (a bit failed to take), ...
}

// Permanently write-protect the region once fully provisioned.
auto locked = otp.try_lock(/*offset=*/0, /*len=*/4);
```

See also: [`hw_rng.md`](hw_rng.md), [`uart_ref.md`](uart_ref.md).
