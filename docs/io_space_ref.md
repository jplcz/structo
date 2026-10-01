<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `io_space_ref<SpaceTag>`

`include/structo/io_space_ref.hpp`

A type-erased, non-owning handle that performs the actual fixed-width
loads/stores and `rep insb`/`outsb`-style string I/O over an
[`io_address<T, SpaceTag, IoInt>`](io_address.md), plus the
`io_space_traits<Backend>` customization point a concrete backend
specializes to be bindable through it.

`io_address.hpp` is deliberately pure address tagging and arithmetic --
no `inb`/`outb`, no volatile MMIO load/store, nothing. `io_space_ref` is
the layer promised there: it owns *access*, not addressing. An
`io_address` says *which* register a caller means; an `io_space_ref` says
*how* to actually read or write it, for one concrete backend (a port-I/O
driver issuing real `IN`/`OUT` instructions, a plain volatile-pointer MMIO
window, a hypervisor's trap-and-emulate stub, a unit test's in-memory fake
register file, ...).

## Why type-erased

Most of `structo` resolves its customization points at compile time
(`phys_translator<Policy>`, `target_ptr_space_traits<SpaceTag>`).
`io_space_ref` deliberately breaks that pattern and erases the backend
behind a small, fixed vtable -- the same two-word-handle shape
(an untyped context pointer plus a `const vtable *`, no virtual base
class, no RTTI, no allocation of its own) that `container_ref.hpp`'s
`mutable_container_ref` and `function_ref.hpp`'s `function_ref` already
use -- because the whole point of this type is to be passed across
boundaries where the concrete backend is *not* known at compile time: a
device driver written once against `io_space_ref<device_io_space>` and
handed a real MMIO backend in production but a fault-injecting fake in
tests, or a syscall/hypercall dispatcher choosing at runtime which
backend's `io_space_ref` to hand a device-emulation routine.

## Customizing: `io_space_traits<Backend>`

Left undefined for any `Backend` that hasn't opted in (mirroring
`container_ref_traits`/`allocator_traits`). A specialization must supply
8 mandatory, fixed-width, single-access functions over a raw
`std::uint64_t` "wire" address (regardless of the concrete `io_address`'s
own, possibly narrower, `IoInt`):

```cpp
template <> struct structo::io_space_traits<my_mmio_backend> {
  static reloco::result<std::uint8_t>  read8 (my_mmio_backend &, std::uint64_t addr) noexcept;
  static reloco::result<std::uint16_t> read16(my_mmio_backend &, std::uint64_t addr) noexcept;
  static reloco::result<std::uint32_t> read32(my_mmio_backend &, std::uint64_t addr) noexcept;
  static reloco::result<std::uint64_t> read64(my_mmio_backend &, std::uint64_t addr) noexcept;
  static reloco::result<void> write8 (my_mmio_backend &, std::uint64_t addr, std::uint8_t  val) noexcept;
  static reloco::result<void> write16(my_mmio_backend &, std::uint64_t addr, std::uint16_t val) noexcept;
  static reloco::result<void> write32(my_mmio_backend &, std::uint64_t addr, std::uint32_t val) noexcept;
  static reloco::result<void> write64(my_mmio_backend &, std::uint64_t addr, std::uint64_t val) noexcept;
};
```

A backend that genuinely cannot do some width (e.g. a port space with no
64-bit `IN`/`OUT`) must still provide that function, reporting its own
lack of support through `unexpected(error::unsupported_operation)`.

Optionally, a backend may also supply "rep" string-I/O fast paths for any
subset of widths:

```cpp
static reloco::result<void> read_rep8(my_backend &, std::uint64_t addr,
                                       std::uint8_t *dst, std::size_t count) noexcept;
static reloco::result<void> write_rep8(my_backend &, std::uint64_t addr,
                                        const std::uint8_t *src, std::size_t count) noexcept;
// ... and the 16/32/64-bit counterparts.
```

Real hardware with a true `rep insb`/`outsb` instruction (or an MMIO FIFO
backend that can batch a burst read under one lock) can service many
same-address accesses in a single trampoline call this way. These are
detected via SFINAE (the same optional-member idiom
`target_ptr_space_traits::min_value`/`max_value` uses); if a given
width's `read_repN`/`write_repN` is absent, `io_space_ref` synthesizes it
generically on top of that width's mandatory single-access `readN`/
`writeN`, so a minimal backend only ever needs to implement the 8
mandatory functions.

## Operations

- `read<T>(addr)` / `write<T>(addr, value)` -- one fixed-width access,
  dispatched to the matching vtable width by `sizeof(T)` (must be
  1/2/4/8 bytes, `T` trivially copyable). `T`'s raw bytes are `memcpy`'d
  to/from the wire-format unsigned integer, exactly like `target_ptr`'s
  materialize/store -- never `reinterpret_cast`, so no strict-aliasing UB
  and no extra alignment requirement on `T`.
- `read_rep<T>(addr, span<T> dst)` / `write_rep<T>(addr, span<const T> src)`
  -- `dst.size()`/`src.size()` consecutive accesses of the *same, fixed*
  address, stepping through a buffer: the classic x86 `rep insb`/`insw`/
  `insl` (and `rep outsb`/`outsw`/`outsl`) pattern used to drain/fill a
  hardware FIFO register. The address is **not** incremented between
  accesses -- for an incrementing-address bulk transfer, use `io_math`/
  `io_address::checked_add` plus repeated single-element `read`/`write`
  calls instead.

An unbound (default-constructed) ref fails every operation with
`error::unsupported_operation`, mirroring `mutable_container_ref`'s
null-safety convention.

## Runtime-selected address-space abstraction

`SpaceTag` is a compile-time axis, so if a driver only ever knows
*statically* which kind of space it's using (e.g. an always-port legacy
device), tags like `port_io_space`/`device_io_space` are enough on their
own. But plenty of real hardware (most PCI BARs) only reveals whether a
given resource is port- or memory-mapped at *runtime*, after probing. The
way to model that with `io_space_ref` is to give `SpaceTag` a *logical
resource* meaning (e.g. "this device's BAR"), not a literal "port vs
MMIO" meaning, and push the actual port/MMIO branch down into the bound
backend:

```cpp
struct pci_bar_space {}; // one tag, backend decides port vs MMIO

struct pci_bar_backend {
  enum class kind { port, mmio } kind_;
  union { std::uint16_t port_base; volatile std::byte *mmio_base; };
};

template <> struct structo::io_space_traits<pci_bar_backend> {
  static reloco::result<std::uint32_t> read32(pci_bar_backend &b, std::uint64_t addr) noexcept {
    return b.kind_ == pci_bar_backend::kind::port
             ? arch_inl(static_cast<std::uint16_t>(b.port_base + addr))
             : reloco::result<std::uint32_t>(load_volatile32(b.mmio_base + addr));
  }
  // ... same branch shape for read8/16/64, write8/16/32/64
};
```

Driver code then only ever sees `io_space_ref<pci_bar_space>` plus
`io_address<T, pci_bar_space>`, written once, working identically whether
the BAR turned out to be I/O- or memory-mapped -- the `kind_` branch is
the *only* place that knows the difference, resolved once at attach time
rather than scattered through the driver. This mirrors FreeBSD's
`bus_space_tag_t`/`bus_space_handle_t` KPI and Linux's `pci_iomap`, which
solve exactly this problem.

See also: [`io_address.md`](io_address.md), [`uart_ref.md`](uart_ref.md)
(a concrete higher-level device abstraction commonly, but not necessarily,
built on top of `io_space_ref`).
