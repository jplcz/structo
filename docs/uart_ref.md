<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::uart_ref`

`include/structo/hw/uart_ref.hpp`

A type-erased, non-owning handle over the typical operations and settings
of a *basic* (interrupt-free, polled) UART -- configuring baud rate/word
format, checking TX/RX readiness, and sending/receiving bytes -- plus the
`uart_traits<Backend>` customization point a concrete backend specializes
to be bindable through it.

This header is deliberately abstraction-only, the UART counterpart of
[`io_space_ref.hpp`](io_space_ref.md): there is no 16550/PL011/
virtio-console register poking here, no clock-to-divisor math, nothing
chip-specific. A concrete backend (a real 16550 driven over
`io_space_ref<port_io_space>`, a PL011 driven over
`io_space_ref<device_io_space>`, a hypervisor para-virtual console, or a
unit test's in-memory fake) implements `uart_traits<Backend>` however it
needs to -- commonly by internally using an `io_space_ref`/`io_address`
pair, but that is an implementation detail this header neither requires
nor depends on.

Only polled operation is modeled, matching a basic debug-console UART
with interrupts left disabled: readiness is always checked explicitly
(`tx_ready`/`rx_ready`) rather than awaited via a completion callback, and
every blocking convenience built on top (`put_byte`/`get_byte`/`write`/
`read_available`) is a bounded spin loop over that polling, never a true
interrupt-driven wait.

## Why `structo::hw`

`uart_ref` lives in `structo::hw`, distinct from the address-tagging/
access-erasure layer (`io_address`/`io_space_ref`, which remain directly
in `structo::`). `structo::hw` is for device-*level* behavioral
interfaces -- "this is a UART, here's what a UART does" -- as opposed to
the lower-level "here's how to read/write a register" layer a `hw`
backend is commonly, but not necessarily, built on top of. Future
device-model abstractions (e.g. a timer, an interrupt controller) belong
in `structo::hw` alongside `uart_ref`, physically under
`include/structo/hw/`.

## Why type-erased, like `io_space_ref`

Exactly as `io_space_ref` erases the concrete I/O-access backend behind a
small, fixed vtable (a two-word handle: an untyped context pointer plus a
`const vtable *`, no virtual base class, no RTTI, no allocation of its
own), `uart_ref` erases the concrete UART backend the same way. This lets
a single debug-console/log-sink routine be written once against
`uart_ref` and handed whatever concrete UART a given boot stage/platform/
test actually has, decided at runtime.

## `uart_config`

```cpp
enum class uart_data_bits : std::uint8_t { five = 5, six = 6, seven = 7, eight = 8 };
enum class uart_parity : std::uint8_t { none, odd, even, mark, space };
enum class uart_stop_bits : std::uint8_t { one, one_point_five, two };
enum class uart_flow_control : std::uint8_t { none, rts_cts, xon_xoff };

struct uart_config {
  std::uint32_t baud_rate = 115200;
  uart_data_bits data_bits = uart_data_bits::eight;
  uart_parity parity = uart_parity::none;
  uart_stop_bits stop_bits = uart_stop_bits::one;
  uart_flow_control flow_control = uart_flow_control::none;
};

inline constexpr uart_config uart_config_115200_8n1{};
```

`uart_config` is an aggregate with `==`/`!=`. `uart_config_115200_8n1` is
the conventional debug-console default (115200 baud, 8 data bits, no
parity, 1 stop bit, no flow control).

## Customizing: `uart_traits<Backend>`

Left undefined for any `Backend` that hasn't opted in (mirroring
`io_space_traits`/`container_ref_traits`). A specialization must supply
exactly five mandatory functions -- the minimal "abstract operations and
settings" contract; everything else is synthesized generically on top of
these by `uart_ref` itself:

```cpp
template <> struct structo::hw::uart_traits<my_backend> {
  static reloco::result<void> configure(my_backend &, const structo::hw::uart_config &) noexcept;
  static reloco::result<bool> tx_ready(my_backend &) noexcept;
  static reloco::result<bool> rx_ready(my_backend &) noexcept;
  static reloco::result<void> try_put_byte(my_backend &, std::uint8_t) noexcept;
  static reloco::result<std::uint8_t> try_get_byte(my_backend &) noexcept;
};
```

- `configure` applies a new `uart_config` (baud rate, word format, flow
  control) to the backend.
- `tx_ready` / `rx_ready` report, without blocking, whether the next
  `try_put_byte`/`try_get_byte` is expected to succeed right now.
- `try_put_byte` / `try_get_byte` attempt exactly one non-blocking byte
  transfer, reporting `error::try_again` (or any other backend-specific
  error) if the device isn't currently ready.

Optionally, a backend may also supply a readback of its current
configuration:

```cpp
static reloco::result<structo::hw::uart_config> current_config(my_backend &) noexcept;
```

Detected via SFINAE (the same optional-member idiom
`target_ptr_space_traits::min_value`/`max_value` and
`io_space_traits::read_repN`/`write_repN` use). If absent,
`uart_ref::current_config()` returns
`unexpected(error::unsupported_operation)` -- there is no sane generic
way to synthesize a hardware-config readback from the other four
mandatory operations, unlike `io_space_ref`'s rep-loop fallback.

## Operations

Forwarded directly to the bound backend's trait functions:

- `configure(const uart_config &)`
- `current_config()` (optional; see above)
- `tx_ready()` / `rx_ready()`
- `try_put_byte(std::uint8_t)` / `try_get_byte()`

Synthesized generically on top of the five mandatory operations, needing
no further backend support:

- `put_byte(std::uint8_t b, std::uint32_t max_spins = default_max_spins)`
  -- spins on `tx_ready()` until ready or `max_spins` is exhausted (then
  returns `error::timed_out`), then calls `try_put_byte(b)`.
- `get_byte(std::uint32_t max_spins = default_max_spins)` -- the
  receive-side mirror of `put_byte`, spinning on `rx_ready()`.
- `write(span<const std::uint8_t> data, std::uint32_t max_spins = default_max_spins)`
  -- calls `put_byte` once per byte, in order, stopping at (and
  propagating) the first failure.
- `write_string(std::string_view s, std::uint32_t max_spins = default_max_spins)`
  -- a debug-console convenience: behaves like `write`, except every
  `'\n'` is sent as `"\r\n"`.
- `read_available(span<std::uint8_t> dst)` -- a non-blocking drain:
  repeatedly calls `try_get_byte()` while `rx_ready()` holds and `dst`
  still has room, returning the number of bytes actually copied into
  `dst` (`0` if nothing was ready yet; stops early, without error, once
  `dst` fills up, leaving any further already-ready bytes buffered in the
  backend for the next call).

`default_max_spins` is `1'000'000`, applied as the default bound for
every spin-looping convenience above; pass an explicit smaller/larger
value to tune the trade-off between unresponsive devices blocking for
too long and transient readiness stalls (e.g. under contention/
virtualization) being mistaken for a hung device.

An unbound (default-constructed) `uart_ref` fails every operation with
`error::unsupported_operation`, mirroring `io_space_ref`'s and
`mutable_container_ref`'s null-safety convention.

## Example

```cpp
struct my_16550 { /* ... built internally on io_space_ref<port_io_space> ... */ };

template <> struct structo::hw::uart_traits<my_16550> {
  // ... five mandatory functions, plus optionally current_config ...
};

void print_boot_banner(structo::hw::uart_ref console) {
  console.configure(structo::hw::uart_config_115200_8n1);
  console.write_string("booting...\n");
}

my_16550 com1;
print_boot_banner(structo::hw::uart_ref(com1)); // same routine, any backend
```

See also: [`io_space_ref.md`](io_space_ref.md) (the lower-level,
address-tagged access layer a concrete UART backend is commonly built
on), [`io_address.md`](io_address.md).
