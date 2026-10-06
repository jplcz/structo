<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hypervisor::mmio_print_device`

`include/structo/hypervisor/mmio_print_device.hpp`

A complete, ready-to-use [`mmio_device_ref`](mmio_device_ref.md) backend:
a one-byte, write-only "print port" MMIO device that forwards every byte
the guest writes to it straight to a bound `structo::hw::console_ref`
sink, one `put()` call per write -- the long-standing Bochs/QEMU `0xE9`
debug-port hack, just MMIO instead of port I/O.

```cpp
structo::hw::vga_text_console screen; // or any other console_traits-adapted backend
structo::hw::console_ref console(screen);
structo::hypervisor::mmio_print_device print_port(console);
structo::hypervisor::mmio_device_ref dev(print_port);

// In the VM-exit MMIO handler, once (device, relative offset) has been resolved to print_port's single
// byte at offset 0:
const std::byte guest_byte = std::byte{'H'};
(void)dev.try_write(0, reloco::span<const std::byte>(&guest_byte, 1)); // prints 'H' to `screen`
```

## Why `console_ref`, not `uart_ref`

`console_ref::put(char)` already performs the "friendly text stream"
translation a printed byte stream needs (`'\n'` -> carriage-return +
line-feed, `'\t'` -> next tab stop, ...) and is `noexcept`/never-fails
(a `put` on an unbound `console_ref` silently does nothing) -- so
`try_write` needs no error handling at all. `hw::uart_ref` would work
too (most real debug ports *are* a UART), but would require spinning on
`tx_ready`/handling `error::timed_out` for every single guest byte.

## Read side: a self-identification probe

Reading this device's one byte always returns `probe_magic` (`std::byte{0xE9}`,
matching Bochs's own debug port) regardless of what was last written --
there is no real register state, only a fixed value so guest code can
distinguish "this address is mapped and really is a print port" from
"nothing is mapped here" before blindly writing to it.

## API

- `static inline constexpr std::byte probe_magic{0xE9};`
- `constexpr explicit mmio_print_device(hw::console_ref sink) noexcept`
- `constexpr hw::console_ref sink() const noexcept`
- `mmio_device_traits<mmio_print_device>`: `size()` always `1`;
  `try_read` always succeeds with `probe_magic`; `try_write` forwards
  the single byte to `sink().put()` and always succeeds; `is_available`
  reflects whether the bound `sink()` is itself bound.

No write-only/read-only distinction beyond the above: the device always
accepts writes (there is nothing to protect), and `mmio_device_ref`'s
own bounds check already guarantees exactly one byte at offset `0` ever
reaches `try_read`/`try_write`.

## Testing

`tests/test_mmio_print_device.cpp` exercises: `size() == 1`; writing
each byte of a string prints the expected characters to a recording
`console_traits` fake (with `'\n'` correctly *not* appearing as a
literal glyph, since `console_ref::put()` treats it as a cursor
movement); reading always returning `probe_magic`, including after a
write; an out-of-range (wider-than-one-byte) access being rejected by
`mmio_device_ref` itself before ever reaching the device; and
`is_available()` reflecting whether the bound sink is itself bound,
including that writing through an unbound sink remains safe (it just
prints nowhere).

## See also

- [`mmio_device_ref.md`](mmio_device_ref.md) -- the type-erased handle
  this device is bound through.
- [`console_ref.md`](console_ref.md) -- the sink this device forwards
  every write to.
- [`reference.md`](reference.md) -- the full per-header API map.
