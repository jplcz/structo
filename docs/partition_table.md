# `hw/partition_table.hpp`

MBR and GPT decoders over a `block_device_ref`. Partitions are read from the device on demand (nothing is
stored per partition), can be enumerated or looked up, and decode to a `partition_device` that binds as a
`block_device_ref`.

```cpp
// Why: find the EFI System Partition (or root filesystem) without caring whether the disk is MBR or GPT,
// then hand it to a filesystem reader as an ordinary block device.

structo::hw::block_device_ref disk(sd_card);   // the whole disk; must outlive everything below

// Detects the scheme: protective MBR -> GPT, valid MBR -> MBR. error::not_found = no table;
// error::invalid_argument = a GPT is present but both headers/entry arrays are corrupt.
// try_open_mbr() / try_open_gpt() force a scheme.
auto table = structo::hw::partition_table::try_open(disk);
if (!table)
  return;

// Enumerate: each item is a result<partition_info> because entries are read as you go. A device error or
// malformed entry yields one error item and ends the iteration.
auto it = table->entries();
for (auto p : it) {
  if (!p)
    break;
  // number: 1-based (MBR slots 1-4, logical 5+; GPT entry index + 1). first_lba / block_count are in
  // device blocks. name_view(): GPT name folded to ASCII.
  log(p->number, p->first_lba, p->block_count, p->name_view());
}

// Lookups: by GPT type GUID, unique GUID or name; by MBR type byte; or by 1-based number.
auto esp = table->find_by_type_guid(structo::hw::gpt_types::efi_system);
auto root = table->find_by_name("root");
auto swap = table->find_by_mbr_type(structo::hw::mbr_types::linux_swap);

// Decode to a block device. `part` is a movable value; the block_device_ref points at it, so keep it alive
// and in place while the ref is used. LBA 0 of `esp_dev` is the first block of the partition.
auto part = table->try_open_partition(*esp);
structo::hw::block_device_ref esp_dev = part->as_block_device();
```

Notes:
- **MBR:** 4 primary slots plus the EBR chain of an extended partition (reported as logical partitions
  5, 6, ...; the container itself has `is_extended` set and is skipped by `find_by_mbr_type`). A sector
  whose boot flags are not 0x00/0x80 (e.g. a FAT boot sector) is not treated as an MBR. A broken or
  looping EBR chain yields an error item (`error::invalid_argument`).
- **GPT:** header and entry-array CRCs are checked once at open; the backup header at the last LBA is
  used if the primary is damaged (`used_backup_header()`). Unused entries are skipped.
- Device block sizes 512..4096 only (`error::unsupported_operation` otherwise).
- Every enumeration step reads from the device. Put a `block_cache_ref` under the table to avoid repeats.
- `partition_device` forwards reads/writes/flush to the parent with the LBA shifted, so one cache on the
  whole disk is shared by all partitions.
