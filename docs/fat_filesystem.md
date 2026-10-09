# fat_filesystem

`include/structo/fs/fat.hpp` — read-only FAT12/FAT16/FAT32 reader with long file names, exposed through [`filesystem_ref`](filesystem_ref.md). Allocation-free; reads through a `block_device_ref` (e.g. a partition from [`partition_table`](partition_table.md), optionally behind a [`block_cache_ref`](block_cache_ref.md)).

```cpp
// A FAT partition as a block device (see partition_table.md). `part` must stay alive and in place.
auto part = table->try_open_partition(*esp);
auto dev = part->as_block_device();

// Caller storage, at least one device block (<= 4096 B). The filesystem uses it for unaligned reads,
// so it must outlive the mount and must not be used by anyone else meanwhile.
reloco::array<std::byte, 4096> scratch;

// Validates the boot sector / BPB and derives FAT12/16/32 from the cluster count.
// invalid_argument: not a FAT volume, or the BPB describes more than the device holds.
auto fat = structo::fs::fat_filesystem::try_mount(dev, scratch);

// Bind the type-erased accessor. `fat` must not move or die while `fs`, or anything opened from it, is in use.
structo::fs::filesystem_ref fs(*fat);

reloco::array<std::byte, 4096> cfg;
auto n = fs.try_read_file("/EFI/BOOT/boot.cfg", cfg);   // path matched case-insensitively; n = bytes read
```

Notes:

- Read-only: there is no `try_write`, so `fs.is_read_only()` is true and writes return `permission_denied`. `try_statfs` is not provided (`unsupported_operation`).
- Names: the long name (UTF-16 → UTF-8) is used when its checksum matches the 8.3 entry, otherwise the 8.3 name (honouring the NT lower-case flags). Comparison folds ASCII case only. Names longer than 255 UTF-8 bytes are truncated. "." and ".." are real entries in subdirectories and are skipped by directory iteration, but can be used in paths.
- At most `max_open_objects` (8) files and directories can be open at once; further opens return `busy`.
- Chains are followed with bounds checking; a free, bad or out-of-range link, or a chain shorter than the file size, gives `io_error`. Sequential reads cache the chain position, so they do not re-walk the chain.
- 512–4096-byte sectors, 1–128 sectors per cluster, one or two FATs. exFAT is not supported.
