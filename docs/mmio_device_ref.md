<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hypervisor::mmio_device_ref`

`include/structo/hypervisor/mmio_device_ref.hpp`

A type-erased, non-owning handle over one *emulated* MMIO device -- the
hypervisor side of a guest MMIO access, as opposed to
[`io_space_ref`](io_space_ref.md)/`mmio_space_backend`
(`mmio_space.hpp`), which are for code that itself *is* a guest/kernel
touching a real (or passed-through) hardware register window.

## The expected caller: a VM-exit MMIO handler

Once the architecture's exit qualification has been decoded into a
faulting guest-physical address and access width, and some
address-space map (out of this header's scope -- a sorted range table,
an interval tree, ...) has resolved that address down to "device D,
offset O within D's own window", the handler calls `try_read`/
`try_write` on the `mmio_device_ref` for device D with that offset --
exactly the last-mile role
[`hypervisor/vm_exit_dispatcher.hpp`](vm_exit_dispatcher.md) plays for
the exit-reason side of the same trap.

```cpp
struct doorbell_device {
  std::uint32_t rings = 0;
};

template <> struct structo::hypervisor::mmio_device_traits<doorbell_device> {
  static std::size_t size(doorbell_device &) noexcept { return 8; }

  static reloco::result<void> try_read(doorbell_device &d, std::uint64_t offset,
                                       reloco::span<std::byte> dst) noexcept {
    if (offset != 4 || dst.size() != 4)
      return reloco::unexpected(reloco::error::invalid_argument);
    std::memcpy(dst.data(), &d.rings, 4);
    return {};
  }

  static reloco::result<void> try_write(doorbell_device &d, std::uint64_t offset,
                                        reloco::span<const std::byte> src) noexcept {
    if (offset != 0 || src.size() != 4)
      return reloco::unexpected(reloco::error::invalid_argument);
    ++d.rings; // any write is a doorbell ring; the written bits are ignored
    return {};
  }
};

doorbell_device dev;
structo::hypervisor::mmio_device_ref ref(dev);
```

## Customization point: `mmio_device_traits<Backend>`

Mandatory:

```cpp
static std::size_t size(Backend &) noexcept;
static reloco::result<void> try_read(Backend &, std::uint64_t offset, reloco::span<std::byte> dst) noexcept;
```

`try_read` fills `dst` with `dst.size()` raw bytes starting at byte
offset `offset`, in whatever order the backend's own register layout
naturally produces them -- no endianness conversion happens in this
header, exactly like `mmio_space_backend`'s `volatile` dereferences do
none of their own. `mmio_device_ref` checks `offset`/`dst.size()`
against `size()` itself (`error::out_of_range`); no particular access
width is required -- real accesses are overwhelmingly 1/2/4/8 bytes, but
a backend free to reject unsupported widths itself with
`error::invalid_argument`.

Optional (detected via SFINAE, same idiom as `block_device_traits`):

```cpp
static reloco::result<void> try_write(Backend &, std::uint64_t offset, reloco::span<const std::byte> src) noexcept;
static bool is_read_only(Backend &) noexcept;
static bool is_available(Backend &) noexcept;
static reloco::result<void> try_reset(Backend &) noexcept;
```

- No `try_write` at all -> the device is hard-wired read-only:
  `is_read_only()` always reports `true`, `try_write()` always fails
  with `error::unsupported_operation`.
- `try_write` present, `is_read_only` absent -> writable whenever bound.
- `try_write` present, `is_read_only` present -> `try_write()` fails
  with `error::permission_denied` whenever the probe reports `true`
  (e.g. a guest-visible "locked" flash region) -- this check happens
  *inside* the dispatch entry, after the "does this backend support
  writing at all" check, so a hard-wired-read-only backend never
  accidentally reports `permission_denied` instead of
  `unsupported_operation`.
- `is_available` absent -> `is_available()` reports `true` whenever
  bound (for devices that can be transiently unavailable, e.g. a
  hot-unplugged virtio-mmio device whose address range is still mapped
  but no longer backed).
- `try_reset` absent -> `try_reset()` succeeds trivially (no internal
  state a VM reset needs to clear).

## API

- `std::size_t size() const noexcept` -- `0` if unbound.
- `bool is_available() const noexcept` -- `false` if unbound.
- `bool is_read_only() const noexcept` -- `true` if unbound or the
  backend has no write support at all.
- `result<void> try_read(std::uint64_t offset, span<std::byte> dst) const noexcept`
- `result<void> try_write(std::uint64_t offset, span<const std::byte> src) const noexcept`
- `result<void> try_reset() const noexcept` -- e.g. for a whole-VM/guest
  reset.

`try_read`/`try_write` both bounds-check `[offset, offset + size)`
against `[0, size())` before ever reaching the backend
(`error::out_of_range`), and reject an empty `dst`/`src`
(`error::invalid_argument`) -- the same boundary-checking-lives-in-the-
ref-not-every-backend convention `block_device_ref::try_read_blocks`
uses for `lba`/block-count checks.

## Testing

`tests/test_mmio_device_ref.cpp` exercises: unbound-ref safety (every
method fails with `error::unsupported_operation` without crashing);
write-then-read round trips; both read and write past the device's
window failing with `error::out_of_range`; empty accesses failing with
`error::invalid_argument`; the runtime `is_read_only` probe rejecting
writes with `error::permission_denied`; `is_available()` reflecting the
backend; a backend's `try_read` failure propagating through unchanged;
`try_reset()` clearing backend state and counting invocations; and a
second, write-trait-less backend confirming the "no write support at
all" path reports `error::unsupported_operation` (not
`error::permission_denied`) and a missing `try_reset` succeeds
trivially.

## See also

- [`io_space_ref.md`](io_space_ref.md) -- the opposite direction: a
  guest/kernel accessing a real (or passed-through) hardware register
  window.
- [`vm_exit_dispatcher.md`](vm_exit_dispatcher.md) -- the exit-reason
  side of the same VM-exit trap this header's `offset`/width already
  assumes has been decoded.
- [`mmio_print_device.md`](mmio_print_device.md) -- a complete, ready-to-use
  `mmio_device_traits` backend built on this header.
- [`mmio_text_console.md`](mmio_text_console.md) -- a more elaborate
  `mmio_device_traits` backend built on this header, pairing the
  trapped control window with a directly-mapped, allocator-owned
  framebuffer.
- [`mmio_framebuffer_device.md`](mmio_framebuffer_device.md) -- the
  same split applied to raw pixels instead of text cells.
- [`mmio_gpu_command_buffer_device.md`](mmio_gpu_command_buffer_device.md)
  -- a pseudo hardware-accelerated 2D drawing device built the same
  way: a directly-mapped command buffer plus a trapped "execute"
  doorbell.
- [`reference.md`](reference.md) -- the full per-header API map.
