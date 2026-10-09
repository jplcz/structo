<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::mmio_space_backend`

`include/structo/mmio_space.hpp`

A portable `io_space_traits` backend for a plain memory-mapped I/O
window, bindable through [`io_space_ref`](io_space_ref.md). Each access
is one `volatile` load or store of the requested width. There is no
inline assembly and no per-architecture header, so it behaves the same on
x86, x86-64, RISC-V, ARM and ARM64.

[`io_address`](io_address.md) tags addresses, and `io_space_ref` is the
type-erased access layer with the `io_space_traits<Backend>`
customization point. This header supplies the common memory-mapped
backend, the counterpart of the x86 port-I/O backend in
`arch/x86/port_io_space.hpp`.

## Usage

```cpp
// A register address: uint8_t is the access width, device_io_space is the
// address-space tag that ties it to the matching io_space_ref.
using lsr_addr = structo::io_address<std::uint8_t, structo::device_io_space>;

// `window` is a non-owning volatile* the caller has already mapped (e.g. a
// PCI BAR or a device-tree `reg` range). `window_size` is its size in
// bytes. If it is nonzero, every access is bounds-checked against
// [0, window_size). Omit it (0) to disable checks for an unbounded window.
structo::mmio_space_backend backend(window, window_size);

// Bind the backend to the type-erased access handle. `backend` must outlive
// `regs`.
structo::io_space_ref<structo::device_io_space> regs(backend);

// 0x04 is a byte OFFSET from `window`, not an absolute address. The result is
// a reloco::result<uint8_t>: an error is reported for an unbound backend or
// an out-of-range offset.
auto status = regs.read(lsr_addr{0x04});
if (status && (*status & 0x01)) {
  // Write a byte at offset 0x00. The value must match the width of the
  // address type.
  (void)regs.write(lsr_addr{0x00}, static_cast<std::uint8_t>(0xFF));
}
```

## API

| Member | Description |
|---|---|
| `mmio_space_backend()` | Constexpr default constructor; unbound (`is_bound()` is false). |
| `explicit mmio_space_backend(volatile void *base, std::size_t size = 0)` | Binds to the window of `size` bytes starting at `base`. `size == 0` disables bounds checking. |
| `is_bound()` | True if `base` was non-null. |
| `size()` | The size given at construction (`0` means unchecked). |
| `io_space_traits<mmio_space_backend>` | Specialization providing `read8/16/32/64` and `write8/16/32/64`. Each returns `reloco::result`. |

Errors reported by an access:

- `reloco::error::unsupported_operation`: the backend is unbound.
- `reloco::error::out_of_range`: `size != 0` and `addr + sizeof(access)`
  exceeds the window.

## Behaviour notes

- **Offsets, not pointers.** The `io_address` value is added to `base`
  on every access, like a FreeBSD `bus_space` handle plus offset. Pass
  the real base once at construction and use small device-relative
  offsets afterwards.
- **No implicit barriers.** An access is exactly one load or store. If
  the device needs completion ordering, add the appropriate
  `structo::arch::{x86,arm,arm64,riscv}::barrier_traits` call
  yourself, typically once per batch rather than once per register.
- **No alignment check.** A misaligned `base + addr` goes straight to a
  `volatile` dereference, with the target architecture's usual
  unaligned-access behaviour (possibly a fault).
- **No `read_rep*` / `write_rep*`.** `io_space_ref`'s generic loop issues
  one real `volatile` access per element at the same address. This is
  what read- or write-sensitive registers (FIFOs, clear-on-read status,
  doorbells) need, since no access is elided, merged or reordered.
