# ext4_filesystem

`include/structo/fs/ext4.hpp` — read-only ext2/ext3/ext4 reader exposed through [`filesystem_ref`](filesystem_ref.md). Allocation-free; reads through a `block_device_ref` (e.g. a partition from [`partition_table`](partition_table.md), optionally behind a [`block_cache_ref`](block_cache_ref.md)).

```cpp
// A Linux root partition as a block device (see partition_table.md). `part` must stay alive and in place.
auto part = table->try_open_partition(*root);
auto dev = part->as_block_device();

// Caller storage, at least one device block (<= 4096 B). Used for unaligned and metadata reads,
// so it must outlive the mount and must not be used by anyone else meanwhile.
reloco::array<std::byte, 4096> scratch;

// Validates the superblock and geometry.
// invalid_argument: bad magic / geometry larger than the device.
// unsupported_operation: incompatible features (meta_bg, inline_data, encryption, ...).
// invalid_state: journal needs recovery (volume was not cleanly unmounted).
auto ext = structo::fs::ext4_filesystem::try_mount(dev, scratch);

// Bind the type-erased accessor. `ext` must not move or die while `fs`, or anything opened from it, is in use.
structo::fs::filesystem_ref fs(*ext);

reloco::array<std::byte, 4096> cfg;
auto n = fs.try_read_file("/boot/grub/grub.cfg", cfg);   // symlinks are followed; n = bytes read
```

Notes:

- Read-only: no `try_write`, so `fs.is_read_only()` is true and writes return `permission_denied`. `try_statfs` is not provided.
- Block sizes 1/2/4 KiB; 32- and 64-bit group descriptors; extent trees and classic block maps (direct, indirect, double, triple). Holes and uninitialized extents read as zeros.
- Directories are scanned linearly; htree index nodes are skipped. Names are case-sensitive; "." and ".." are skipped in listings but usable in paths.
- Symlinks (fast and slow) are followed in every path component up to 8 levels; more (or a loop) gives `invalid_argument`.
- Checksums and extended attributes are not verified or read. Corrupt extents/pointers give `io_error`.
- At most `max_open_objects` (8) files and directories can be open at once; further opens return `busy`.
