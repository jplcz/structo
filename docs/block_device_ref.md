<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::block_device_ref`

`include/structo/hw/block_device_ref.hpp`

A type-erased, non-owning handle over a fixed-block-size, random-access
storage device (SD/eMMC/NVMe/virtio-blk, a RAM disk, or a test fake), plus the
`block_device_traits<Backend>` customization point a backend specializes to
be bindable through it. It is abstraction-only: no register poking, and no
notion of files or filesystems. The intended caller is a bootloader or other
allocation-free early code that already knows which blocks it needs.

Ready-made in-memory backends are in [`ram_disk`](ram_disk.md). The same
single-`vtable` `*_ref` shape is used by [`uart_ref`](uart_ref.md) and
[`otp_storage`](otp_storage.md).

Unbound refs (default-constructed) never trap: every fallible call fails
with `error::unsupported_operation`.

## Usage

```cpp
// Customization point: left undefined until you specialize it.
template <> struct structo::hw::block_device_traits<my_sd_card> {
  // Required. Fixed sector size in bytes (512, 4096, ...).
  static std::size_t block_size(my_sd_card &) noexcept { return 512; }
  // Required. Total number of addressable blocks.
  static std::uint64_t block_count(my_sd_card &c) noexcept { return c.sectors; }
  // Required. Read dst.size()/block_size() contiguous blocks starting at
  // `lba` into `dst`. block_device_ref has already checked that dst.size()
  // is a block multiple and that the range lies inside [0, block_count()).
  static reloco::result<void> try_read_blocks(my_sd_card &c, std::uint64_t lba,
                                              reloco::span<std::byte> dst) noexcept {
    return c.read(lba, dst);
  }

  // Optional (detected by SFINAE). Without try_write_blocks the device is
  // permanently read-only: is_read_only() is true, writes fail with
  // unsupported_operation.
  static reloco::result<void> try_write_blocks(my_sd_card &c, std::uint64_t lba,
                                               reloco::span<const std::byte> src) noexcept {
    return c.write(lba, src);
  }
  // Optional. Absent => try_flush() succeeds trivially (no write-back cache).
  static reloco::result<void> try_flush(my_sd_card &c) noexcept { return c.sync(); }
  // Optional runtime write-protect (e.g. the SD lock tab). Absent but
  // try_write_blocks present => assumed writable. When it returns true,
  // writes fail with error::permission_denied.
  static bool is_read_only(my_sd_card &c) noexcept { return c.write_protected; }
  // Optional. Is the media usable right now (card inserted, link trained)?
  // Absent => assumed true whenever bound.
  static bool is_available(my_sd_card &c) noexcept { return c.present; }
};

my_sd_card card;
structo::hw::block_device_ref dev{card};      // explicit, non-owning: `card` must outlive
                                              // dev and its copies; rvalues are rejected

if (!dev.is_available()) { /* no media */ }

std::array<std::byte, 512> sector;
// Read one block. lba = logical block address (block index, not bytes);
// dst size must be a multiple of dev.block_size() (else invalid_argument);
// a range past block_count() fails with out_of_range.
auto r = dev.try_read_blocks(/*lba=*/2048, sector);

// Byte-granular read: offset and length need not be block aligned.
// `scratch` (>= block_size() bytes, else invalid_argument) is used for a
// partial leading/trailing block; whole blocks are read straight into dst.
std::array<std::byte, 100> head;
std::array<std::byte, 512> scratch;
auto b = dev.try_read_bytes(/*byte_offset=*/1000, head, scratch);

if (!dev.is_read_only()) {
  dev.try_write_blocks(2048, sector);         // same alignment/range checks; permission_denied if read-only
  dev.try_flush();                            // push pending writes to the media
}
```

## API

| Member | Description |
| --- | --- |
| `block_device_ref()` | Unbound ref. |
| `explicit block_device_ref(Backend &)` | Binds a backend that has `block_device_traits` (needs `block_size`, `block_count`, `try_read_blocks`). Rvalue bindings are deleted. |
| `explicit operator bool()` | Whether bound. |
| `block_size()` | Block size in bytes; `0` if unbound. |
| `block_count()` | Total blocks; `0` if unbound. |
| `size_bytes()` | `block_count() * block_size()` (may overflow on absurdly large devices). |
| `is_available()` | Backend's `is_available`; `false` if unbound, `true` if the probe is absent. |
| `is_read_only()` | `true` if unbound or no write support; else the `is_read_only` probe, defaulting to `false`. |

## Operations

| Method | Errors added by the ref (before the backend runs) |
| --- | --- |
| `try_read_blocks(lba, span<byte> dst)` | `unsupported_operation` (unbound), `invalid_argument` (misaligned size), `out_of_range` (range outside the device) |
| `try_write_blocks(lba, span<const byte> src)` | same as above, plus `unsupported_operation` (no write support) and `permission_denied` (`is_read_only` probe true) |
| `try_flush()` | `unsupported_operation` (unbound); succeeds trivially without a `try_flush` backend |
| `try_read_bytes(byte_offset, dst, scratch)` | `invalid_argument` if `scratch` is smaller than a block; otherwise whatever `try_read_blocks` reports (`out_of_range` past the end) |

All return `reloco::result<void>` and are `[[nodiscard]]`. Backend errors
are passed through unchanged.
