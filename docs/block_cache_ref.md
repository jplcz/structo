# `hw/block_cache_ref.hpp`

A small write-back LRU cache over a `block_device_ref`. The caller supplies all storage; nothing is
allocated. It is a cheap, copyable, non-owning handle (copies share the same cached state).

```cpp
// Why: filesystem readers re-read the same FAT sectors / inode blocks constantly; caching them
// avoids repeated trips to a slow SD/eMMC device.

structo::hw::block_device_ref disk(sd_card);          // the real device; must outlive the cache

// Backing storage, owned by the caller and must outlive the cache. The cache carves a 16-byte
// header, 24 bytes of bookkeeping per slot and block_size() bytes of data per slot out of it:
// with 512-byte blocks this gives (16384 - 16) / (24 + 512) = 30 cached blocks.
reloco::array<std::byte, 16384> storage;

// Fails with unsupported_operation (unbound device / zero block size) or invalid_argument
// (storage too small for even one slot).
auto cache = structo::hw::block_cache_ref::try_create(disk, storage);

reloco::array<std::byte, 1024> two_blocks;
(void)cache->try_read_blocks(2048, two_blocks);       // lba, destination (multiple of block size)
(void)cache->try_write_blocks(2048, two_blocks);      // lba, source; stays dirty in the cache
(void)cache->try_read_bytes(1000, small_buf);         // byte offset, destination; no scratch needed
(void)cache->try_flush();                             // write back all dirty blocks, then flush the device

// Layer a filesystem reader on top: the cache is itself a block device.
structo::hw::block_device_ref cached_disk(*cache);
```

Behaviour:
- **Write-back, LRU.** A write never reads the old block; a dirty victim is written back before reuse
  (if that fails the request fails and the cache is unchanged).
- **Bypass.** A request spanning more blocks than the cache has slots goes straight to the device, after
  writing back (reads) or dropping (writes) overlapping cached blocks, so it stays coherent and does not
  evict the working set.
- **No flush on destruction.** Call `try_flush()` before dropping the storage or device.
- `try_invalidate()` drops all cached blocks and fails with `error::busy` while any are dirty.
- Writes to a read-only device fail with `error::permission_denied`.
- Single-threaded: serialise access if shared between cores.
