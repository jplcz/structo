<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `io_address<T, SpaceTag, IoInt>`

`include/structo/io_address.hpp`

A typed, tagged address into some device-register address space -- x86-
style port I/O, a memory-mapped register window, an ARM `Device-nGnRnE`
region, ... -- plus `reg_traits<Size>` and the `io_math` namespace for
register-size-aware offset arithmetic.

`io_address` is a sibling of [`phys_addr`](phys_addr.md), not a
specialization of it: `phys_addr` tags *where in physical memory* a byte
lives (host/guest/DMA/secure/...); `io_address` tags *which device
register space* an address lives in, a materially different axis -- two
`io_address`es can be numerically equal and still refer to completely
different hardware if their `SpaceTag`s differ.

`io_address` deliberately does *no* I/O itself -- no `inb`/`outb`, no
volatile MMIO load/store, nothing. It is pure address tagging and
compile-time-checked arithmetic, exactly like `phys_addr`. A later,
separate type (a type-erased `io_space_ref`, layered on top of this one)
is expected to own the actual access side: single/multi-size reads and
writes, `rep insb`/`rep outsb`-style string I/O, and whatever else a
concrete `SpaceTag`'s backing hardware needs.

## A universal framework, not an x86 header

Nothing here is specific to x86 port I/O or to ARM. `SpaceTag` is an
entirely open, caller-extensible set -- this header only ships a handful
of illustrative example tags:

- `default_io_space` -- a single, flat register space.
- `port_io_space` -- x86-style port-mapped I/O, accessed via `IN`/`OUT`.
- `device_io_space` -- a plain, domain-agnostic MMIO window.
- `secure_io_space` / `nonsecure_io_space` -- TrustZone-style secure/
  non-secure MMIO aliases.
- `realm_io_space` -- an ARM CCA Realm's MMIO alias.
- `hypervisor_io_space` -- MMIO owned/trapped-and-emulated by a
  hypervisor (EL2), not passed through to a guest.

Embedders targeting some other architecture's register space (RISC-V
PLIC/CLINT windows, a PCI config-space window, a software-defined
accelerator's doorbell space, ...) define their own empty tag struct and
get the exact same compile-time space separation for free.

## Why `cast_space()` exists here (unlike `target_ptr`)

[`target_ptr`](target_ptr.md) deliberately has no `cast_space()` at all,
because two *virtual* address spaces essentially never coincide
bit-for-bit. `io_address` follows `phys_addr`'s lead instead and keeps
the (unsafe, `RELOCO_UNSAFE_BUFFER_USAGE`-gated) escape hatch: unlike
virtual address spaces, device register spaces frequently *do* alias on
purpose -- a Secure and Non-secure MMIO window are often the exact same
physical decode at a fixed offset from each other, a hypervisor may
trap-and-emulate a guest-visible device at the identical address it
itself uses to program the real hardware, and a debug/back-door window
often mirrors a normal one verbatim. `cast_space()` lets an embedder
express "I know this specific platform aliases these two spaces"
explicitly, the same way `phys_addr::cast_space()` does for an
identity-mapped IOMMU.

## Comparisons

All six comparison operators (`==`, `!=`, `<`, `<=`, `>`, `>=`) are
provided (unlike `phys_addr`'s `==`/`!=` only), since ordering is
routinely meaningful here: checking an address falls within a BAR/window
range, sorting a device's registers, etc. They are only ever defined
between two `io_address`es instantiated with the exact same
`T`/`SpaceTag`/`IoInt`.

## Checked offset arithmetic

`io_address::checked_add(n)`/`checked_sub(n)` advance by `n` elements
(`n * sizeof(T)` bytes, `1` byte per step for `T = void`), failing with
`error::integer_overflow` instead of wrapping. `checked_offset_from`
computes the signed element-wise distance between two addresses, failing
with `error::invalid_argument` if the byte distance isn't an exact
multiple of `sizeof(T)`.

Unlike `target_ptr`, there is deliberately no `wrapping_add`/
`saturating_add` flavor: a device/port address silently wrapping around
is essentially always a bug -- there is no hardware notion of a
"saturated register" -- so the checked, fail-loudly variant is the only
offset primitive on the type itself.

## Register-size-aware offset math (`io_math`)

`reg_traits<Size>` (aliases: `reg8`, `reg16`, `reg32`, `reg64`) describes
a fixed register width, mirroring `phys_page.hpp`'s `page_traits`. The
`io_math` namespace (mirroring `pfn_translator.hpp`'s `page_math`) then
provides register-width-aware helpers over a raw, untyped `io_address`
(where `io_address::checked_add`'s `sizeof(T)` stride isn't what you
want -- e.g. a GICD/GICR-style distributor frame carved into uniform
4-byte slots):

- `io_math::offset<RegTraits>(addr)` -- the in-register byte offset.
- `io_math::is_aligned<RegTraits>(addr)` -- whether `addr` falls exactly
  on a register boundary.
- `io_math::align_down<RegTraits>(addr)` / `align_up<RegTraits>(addr)` --
  round to the nearest register boundary.
- `io_math::at_reg<RegTraits>(base, index)` -- the address of the
  `index`-th register past `base` (`index` may be negative), failing
  with `error::integer_overflow` on overflow.
- `io_math::reg_index<RegTraits>(addr, base)` -- the inverse of `at_reg`:
  the signed register-slot distance from `base` to `addr`, failing with
  `error::invalid_argument` if `addr` isn't exactly on a register
  boundary relative to `base`.

## Zero overhead

`io_address<T, SpaceTag, IoInt>` is exactly `sizeof(IoInt)` (default
`std::uint64_t`) with standard layout, regardless of `T` or `SpaceTag`.

See also: [`phys_addr.md`](phys_addr.md), [`target_ptr.md`](target_ptr.md).
