<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::ram_disk` / `structo::hw::read_only_ram_disk`

`include/structo/hw/ram_disk.hpp`

Minimal `block_device_traits` backends over an in-memory buffer, bindable
through [`block_device_ref`](block_device_ref.md): a bootloader's loaded
initrd, an embedded filesystem blob, or a unit test's fake disk.

Neither class owns memory; both are `reloco::span` views, so the buffer must
outlive the disk and every `block_device_ref` bound to it.

- `ram_disk` wraps a mutable `span<std::byte>`: reads and writes.
- `read_only_ram_disk` wraps a `span<const std::byte>` and has no
  `try_write_blocks` at all, so a bound `block_device_ref` reports
  `is_read_only() == true` and writes fail with `unsupported_operation`.

## Usage

```cpp
alignas(8) std::array<std::byte, 64 * 1024> image{};

// Mutable disk. First argument: backing storage (must outlive the disk).
// Second: block size in bytes, default 512, must be non-zero (asserted).
// It is a caller's choice -- there is no controller imposing it. Use 1 for
// a plain byte-addressable view. A trailing partial block is dropped
// (block_count() is floor-rounded), not rejected.
structo::hw::ram_disk disk{image, 512};
structo::hw::block_device_ref dev{disk};      // 128 blocks

std::array<std::byte, 512> blk{};
dev.try_write_blocks(/*lba=*/3, blk);         // copy into image[3*512 ...]
dev.try_read_blocks(3, blk);

// Immutable disk over e.g. an initrd: the span is const.
structo::hw::read_only_ram_disk initrd{reloco::span<const std::byte>(image), 4096};
structo::hw::block_device_ref ro{initrd};
bool r = ro.is_read_only();                   // true
```

## API

Both classes have the same accessors (`ram_disk` yields `span<std::byte>`,
`read_only_ram_disk` yields `span<const std::byte>`):

| Member | Description |
| --- | --- |
| default constructor | Empty disk (`block_size()` and `block_count()` are `0`). |
| `explicit (storage, block_size = 512)` | Wraps `storage`; asserts `block_size != 0`. |
| `block_size()` | The configured block size. |
| `block_count()` | `storage().size() / block_size()`, floor-rounded. |
| `storage()` | The wrapped span. |

`block_device_traits<ram_disk>` implements `block_size`, `block_count`,
`try_read_blocks` and `try_write_blocks`; `block_device_traits<read_only_ram_disk>`
omits writing. Neither provides `try_flush`, `is_read_only` or
`is_available`, so flush is a no-op and the device is always available.
Bounds are validated by `block_device_ref`, not by the backend.
