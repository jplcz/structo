// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file partition_table.hpp
 * @brief MBR and GPT decoders on top of `block_device_ref`. Partitions are read on demand (nothing is
 * cached or stored per partition), can be enumerated or looked up, and decode to `partition_device`, which
 * binds to a `block_device_ref` of its own.
 *
 * Supported: the classic MBR (4 primary slots plus the EBR chain of an extended partition, as logical
 * partitions 5, 6, ...), and GPT (CRC-checked header and entry array, backup header at the last LBA used if the
 * primary is damaged, protective-MBR detection). LBAs are in device blocks. Device block sizes up to 4096 are
 * supported.
 *
 * @code
 * // Why: find the EFI System Partition (or the root filesystem) on a disk without caring whether it is MBR
 * // or GPT, then hand it to a filesystem reader as an ordinary block device.
 *
 * structo::hw::block_device_ref disk(sd_card);                 // the whole disk (must outlive everything below)
 *
 * // Detects the scheme: a protective MBR means GPT, a valid MBR means MBR. Fails with error::not_found if the
 * // disk has no partition table, error::invalid_argument if a table is present but corrupt.
 * auto table = structo::hw::partition_table::try_open(disk);
 * if (!table)
 *   return;
 *
 * // Enumerate. Each item is a result<partition_info> because entries are read from the device as you go;
 * // a read error or malformed entry yields one error item and ends the iteration.
 * auto it = table->entries();
 * for (auto p : it) {
 *   if (!p)
 *     break;
 *   log(p->number, p->first_lba, p->block_count, p->name_view());   // number is 1-based, like Linux sdX1
 * }
 *
 * // Look up by GPT type GUID, then turn it into a block device.
 * auto esp = table->find_by_type_guid(structo::hw::gpt_types::efi_system);
 * if (!esp)
 *   return;
 * auto part = table->try_open_partition(*esp);                 // a partition_device: a movable value
 * if (!part)
 *   return;
 * structo::hw::block_device_ref esp_dev = part->as_block_device();   // LBA 0 of esp_dev is the partition start;
 *                                                                    // `part` must outlive esp_dev
 * @endcode
 */

#include <reloco/array.hpp>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/iterator.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/optional.hpp>
#include <reloco/span.hpp>
#include <reloco/string_view.hpp>
#include <structo/boot/uefi.hpp>
#include <structo/detail/boot_bytes.hpp>
#include <structo/hw/block_device_ref.hpp>

#include <cstddef>
#include <cstdint>

namespace structo::hw {

using namespace reloco;
namespace boot_bytes = structo::boot::detail;

using guid = structo::boot::uefi::guid;

enum class partition_scheme : std::uint8_t { none, mbr, gpt };

/** @brief Largest device block size the decoders support (they use stack buffers of this size). */
inline constexpr std::size_t partition_max_block_size = 4096;

namespace detail {

/** @brief Parses "01234567-89AB-CDEF-0123-456789ABCDEF" into a `guid`; a malformed string yields a null guid. */
[[nodiscard]] constexpr guid parse_guid(string_view s) noexcept {
  if (s.size() != 36)
    return {};
  array<std::uint8_t, 16> raw{};
  std::size_t n = 0;
  for (std::size_t i = 0; i < 36;) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (s[i] != '-')
        return {};
      ++i;
      continue;
    }
    array<std::uint8_t, 2> v{};
    for (std::size_t k = 0; k < 2; ++k) {
      const char c = s[i + k];
      if (c >= '0' && c <= '9')
        v[k] = static_cast<std::uint8_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        v[k] = static_cast<std::uint8_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        v[k] = static_cast<std::uint8_t>(c - 'A' + 10);
      else
        return {};
    }
    raw[n++] = static_cast<std::uint8_t>(v[0] * 16 + v[1]);
    i += 2;
  }
  guid g;
  g.data1 = (static_cast<std::uint32_t>(raw[0]) << 24) | (static_cast<std::uint32_t>(raw[1]) << 16) |
            (static_cast<std::uint32_t>(raw[2]) << 8) | raw[3];
  g.data2 = static_cast<std::uint16_t>((raw[4] << 8) | raw[5]);
  g.data3 = static_cast<std::uint16_t>((raw[6] << 8) | raw[7]);
  for (std::size_t i = 0; i < 8; ++i)
    g.data4[i] = raw[8 + i];
  return g;
}

} // namespace detail

/** @brief Well-known GPT partition type GUIDs. */
namespace gpt_types {
inline constexpr guid efi_system = detail::parse_guid("C12A7328-F81F-11D2-BA4B-00A0C93EC93B");
inline constexpr guid bios_boot = detail::parse_guid("21686148-6449-6E6F-744E-656564454649");
inline constexpr guid linux_filesystem = detail::parse_guid("0FC63DAF-8483-4772-8E79-3D69D8477DE4");
inline constexpr guid linux_swap = detail::parse_guid("0657FD6D-A4AB-43C4-84E5-0933C84B4F4F");
/** @brief The `XBOOTLDR` partition of the Boot Loader Specification. */
inline constexpr guid linux_xbootldr = detail::parse_guid("BC13C2FF-59E6-4262-A352-B275FD6F7172");
inline constexpr guid linux_root_x86_64 = detail::parse_guid("4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709");
inline constexpr guid linux_root_arm64 = detail::parse_guid("B921B045-1DF0-41C3-AF44-4C6F280D3FAE");
inline constexpr guid linux_root_riscv64 = detail::parse_guid("72EC70A6-CF74-40E6-BD49-4BDA08E8F224");
inline constexpr guid microsoft_basic_data = detail::parse_guid("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7");
} // namespace gpt_types

/** @brief Common MBR partition type bytes. */
namespace mbr_types {
inline constexpr std::uint8_t empty = 0x00;
inline constexpr std::uint8_t fat12 = 0x01;
inline constexpr std::uint8_t fat16_small = 0x04;
inline constexpr std::uint8_t extended_chs = 0x05;
inline constexpr std::uint8_t fat16 = 0x06;
inline constexpr std::uint8_t ntfs_exfat = 0x07;
inline constexpr std::uint8_t fat32_chs = 0x0B;
inline constexpr std::uint8_t fat32_lba = 0x0C;
inline constexpr std::uint8_t extended_lba = 0x0F;
inline constexpr std::uint8_t linux_swap = 0x82;
inline constexpr std::uint8_t linux_native = 0x83;
inline constexpr std::uint8_t extended_linux = 0x85;
inline constexpr std::uint8_t gpt_protective = 0xEE;
inline constexpr std::uint8_t efi_system = 0xEF;

[[nodiscard]] constexpr bool is_extended(std::uint8_t t) noexcept {
  return t == extended_chs || t == extended_lba || t == extended_linux;
}
} // namespace mbr_types

/** @brief GPT partition attribute bits. */
namespace gpt_attributes {
inline constexpr std::uint64_t required_to_function = 1ull << 0;
inline constexpr std::uint64_t no_block_io_protocol = 1ull << 1;
/** @brief "Legacy BIOS bootable"; reported as `partition_info::bootable`. */
inline constexpr std::uint64_t legacy_bios_bootable = 1ull << 2;
} // namespace gpt_attributes

/** @brief One decoded partition. Fields that do not apply to the scheme are zero. */
struct partition_info {
  partition_scheme scheme = partition_scheme::none;
  /** @brief 1-based: MBR slots 1-4, logical partitions 5+; GPT entry index + 1. */
  std::uint32_t number = 0;
  std::uint64_t first_lba = 0;
  /** @brief Size in device blocks. */
  std::uint64_t block_count = 0;
  /** @brief MBR active flag, or GPT "legacy BIOS bootable" attribute. */
  bool bootable = false;
  /** @brief MBR only: this slot is an extended-partition container (its logical partitions follow as 5+). */
  bool is_extended = false;
  /** @brief MBR only. */
  std::uint8_t mbr_type = 0;
  /** @brief GPT only. */
  guid type_guid{};
  /** @brief GPT only. */
  guid unique_guid{};
  /** @brief GPT only. */
  std::uint64_t attributes = 0;
  /** @brief GPT only: the partition name, UTF-16 folded to ASCII (non-ASCII becomes '?'). */
  array<char, 36> name{};
  std::uint8_t name_length = 0;

  [[nodiscard]] string_view name_view() const noexcept { return string_view(name.data(), name_length); }
};

class partition_table;

/** @brief A partition as a block device: LBA 0 is the partition's first block. Bind it with `as_block_device()`.
 *
 * A plain copyable value; a `block_device_ref` obtained from it points at this object, so it must stay alive and
 * must not be moved while that ref is in use. Reads, writes and flushes are forwarded to the parent disk with the
 * LBA shifted, so layering a `block_cache_ref` on the whole disk first gives every partition a shared cache. */
class partition_device {
public:
  constexpr partition_device() noexcept = default;

  /** @brief Maps blocks [first_lba, first_lba + block_count) of @p parent. Fails with `error::out_of_range` if that
   * does not fit the parent, `error::unsupported_operation` if @p parent is unbound. */
  [[nodiscard]] static result<partition_device> try_create(block_device_ref parent, std::uint64_t first_lba,
                                                           std::uint64_t block_count,
                                                           bool read_only = false) noexcept {
    if (!parent)
      return unexpected(error::unsupported_operation);
    const std::uint64_t total = parent.block_count();
    if (first_lba > total || block_count > total - first_lba)
      return unexpected(error::out_of_range);
    partition_device d;
    d.parent_ = parent;
    d.first_lba_ = first_lba;
    d.block_count_ = block_count;
    d.read_only_ = read_only;
    return d;
  }

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return static_cast<bool>(parent_); }
  [[nodiscard]] std::uint64_t first_lba() const noexcept { return first_lba_; }
  [[nodiscard]] std::uint64_t block_count() const noexcept { return block_count_; }
  [[nodiscard]] std::size_t block_size() const noexcept { return parent_.block_size(); }

  /** @brief A `block_device_ref` over this partition; valid while this object lives and stays in place. */
  [[nodiscard]] inline block_device_ref as_block_device() noexcept RELOCO_LIFETIMEBOUND;

private:
  friend struct block_device_traits<partition_device>;

  block_device_ref parent_;
  std::uint64_t first_lba_ = 0;
  std::uint64_t block_count_ = 0;
  bool read_only_ = false;
};

template <> struct block_device_traits<partition_device> {
  static std::size_t block_size(partition_device &d) noexcept { return d.parent_.block_size(); }
  static std::uint64_t block_count(partition_device &d) noexcept { return d.block_count_; }
  static result<void> try_read_blocks(partition_device &d, std::uint64_t lba, span<std::byte> dst) noexcept {
    return d.parent_.try_read_blocks(d.first_lba_ + lba, dst);
  }
  static result<void> try_write_blocks(partition_device &d, std::uint64_t lba, span<const std::byte> src) noexcept {
    return d.parent_.try_write_blocks(d.first_lba_ + lba, src);
  }
  static result<void> try_flush(partition_device &d) noexcept { return d.parent_.try_flush(); }
  static bool is_read_only(partition_device &d) noexcept { return d.read_only_ || d.parent_.is_read_only(); }
  static bool is_available(partition_device &d) noexcept { return d.parent_.is_available(); }
};

// Defined after the traits specialization so that block_device_ref can see it.
inline block_device_ref partition_device::as_block_device() noexcept { return block_device_ref(*this); }

namespace detail {

inline constexpr std::size_t mbr_entries_offset = 446;
inline constexpr std::size_t mbr_entry_size = 16;
inline constexpr std::size_t mbr_signature_offset = 510;
inline constexpr std::size_t gpt_min_entry_size = 128;
inline constexpr std::uint32_t gpt_max_entries = 4096;
inline constexpr std::uint32_t mbr_max_logical = 128;

// Reads @p dst.size() bytes at byte offset @p offset, using a stack scratch block.
[[nodiscard]] inline result<void> read_bytes(const block_device_ref &dev, std::uint64_t offset,
                                             span<std::byte> dst) noexcept {
  array<std::byte, partition_max_block_size> scratch;
  return dev.try_read_bytes(offset, dst, span<std::byte>(scratch));
}

struct mbr_entry {
  bool bootable = false;
  std::uint8_t type = 0;
  std::uint32_t start = 0;
  std::uint32_t count = 0;
  [[nodiscard]] bool used() const noexcept { return type != 0 && count != 0; }
};

// The boot flag must be 0x00 or 0x80; anything else means this sector is not an MBR (e.g. a FAT boot sector).
[[nodiscard]] inline result<mbr_entry> decode_mbr_entry(span<const std::byte> sector, std::size_t off) noexcept {
  auto flag = boot_bytes::read_le_at<std::uint8_t>(sector, off);
  auto type = boot_bytes::read_le_at<std::uint8_t>(sector, off + 4);
  auto start = boot_bytes::read_le_at<std::uint32_t>(sector, off + 8);
  auto count = boot_bytes::read_le_at<std::uint32_t>(sector, off + 12);
  if (!flag || !type || !start || !count)
    return unexpected(error::out_of_bounds);
  if (*flag != 0x00 && *flag != 0x80)
    return unexpected(error::invalid_argument);
  return mbr_entry{*flag == 0x80, *type, *start, *count};
}

[[nodiscard]] inline bool fits(const block_device_ref &dev, std::uint64_t first, std::uint64_t count) noexcept {
  const std::uint64_t total = dev.block_count();
  return first <= total && count <= total - first;
}

} // namespace detail

/** @brief Iterates the used partitions of a table, in table order. Entries are read from the device on demand.
 * A device error or malformed entry yields one error item, then iteration ends. */
class RELOCO_POINTER partition_iterator;

/** @brief A decoded partition table. Holds only the parsed header; partitions are read from the device when
 * enumerated or looked up, so it is cheap to keep and copy. The device must outlive it. */
class partition_table {
public:
  /** @brief Detects the scheme and validates the table. Protective MBR -> GPT; valid MBR -> MBR; otherwise a GPT
   * without a protective MBR is tried. `error::not_found` if there is no table, `error::invalid_argument` if a
   * GPT is present but corrupt (both headers), `error::unsupported_operation` for an unbound device or a block
   * size outside [512, 4096]. */
  [[nodiscard]] static result<partition_table> try_open(block_device_ref dev) noexcept {
    auto mbr = try_open_mbr(dev);
    if (mbr) {
      if (!mbr->has_protective_entry())
        return mbr;
      return try_open_gpt(dev);
    }
    if (mbr.error() != error::not_found && mbr.error() != error::invalid_argument)
      return mbr;
    auto gpt = try_open_gpt(dev);
    if (gpt)
      return gpt;
    return unexpected(gpt.error() == error::invalid_argument ? error::not_found : gpt.error());
  }

  /** @brief Opens an MBR (a protective-MBR disk opens as MBR, with its single 0xEE slot). */
  [[nodiscard]] static result<partition_table> try_open_mbr(block_device_ref dev) noexcept {
    if (auto r = check_device(dev); !r)
      return unexpected(r.error());
    array<std::byte, 512> sector;
    if (auto r = detail::read_bytes(dev, 0, span<std::byte>(sector)); !r)
      return unexpected(r.error());
    auto sig = boot_bytes::read_le_at<std::uint16_t>(span<const std::byte>(sector), detail::mbr_signature_offset);
    if (!sig || *sig != 0xAA55)
      return unexpected(error::not_found);
    partition_table t;
    t.dev_ = dev;
    t.scheme_ = partition_scheme::mbr;
    for (std::size_t i = 0; i < 4; ++i) {
      auto e = detail::decode_mbr_entry(span<const std::byte>(sector), detail::mbr_entries_offset + i * detail::mbr_entry_size);
      if (!e)
        return unexpected(error::not_found);
    }
    for (std::size_t i = 0; i < 64; ++i)
      t.mbr_entries_[i] = sector[detail::mbr_entries_offset + i];
    return t;
  }

  /** @brief Opens a GPT: the primary header at LBA 1, else the backup at the last LBA. Both headers and the
   * entry array are CRC-checked. */
  [[nodiscard]] static result<partition_table> try_open_gpt(block_device_ref dev) noexcept {
    if (auto r = check_device(dev); !r)
      return unexpected(r.error());
    const std::uint64_t total = dev.block_count();
    if (total < 3)
      return unexpected(error::not_found);
    partition_table t;
    t.dev_ = dev;
    t.scheme_ = partition_scheme::gpt;
    auto primary = t.read_gpt_header(1);
    if (primary)
      return t;
    auto backup = t.read_gpt_header(total - 1);
    if (backup) {
      t.used_backup_header_ = true;
      return t;
    }
    return unexpected(primary.error());
  }

  [[nodiscard]] partition_scheme scheme() const noexcept { return scheme_; }
  [[nodiscard]] block_device_ref device() const noexcept { return dev_; }
  /** @brief GPT only: the disk GUID. */
  [[nodiscard]] const guid &disk_guid() const noexcept { return disk_guid_; }
  /** @brief GPT only: the primary header was damaged and the backup was used. */
  [[nodiscard]] bool used_backup_header() const noexcept { return used_backup_header_; }
  /** @brief GPT only: capacity of the entry array (used or not). */
  [[nodiscard]] std::uint32_t gpt_entry_capacity() const noexcept { return gpt_num_entries_; }
  /** @brief MBR only: a 0xEE entry is present, i.e. the real table is a GPT. */
  [[nodiscard]] bool has_protective_entry() const noexcept {
    for (std::size_t i = 0; i < 4; ++i) {
      auto e = detail::decode_mbr_entry(span<const std::byte>(mbr_entries_), i * detail::mbr_entry_size);
      if (e && e->type == mbr_types::gpt_protective)
        return true;
    }
    return false;
  }

  /** @brief A fresh single-pass iterator over the used partitions. */
  [[nodiscard]] partition_iterator entries() const noexcept;

  /** @brief The partition with 1-based @p number; `error::not_found` if the slot is unused. */
  [[nodiscard]] result<partition_info> try_get(std::uint32_t number) const noexcept;

  /** @brief First partition for which @p pred returns true; `error::not_found` if none, or the error of a malformed
   * entry met first. */
  template <typename Pred> [[nodiscard]] result<partition_info> find_if(Pred pred) const noexcept;

  /** @brief GPT: first partition whose type GUID is @p type. */
  [[nodiscard]] result<partition_info> find_by_type_guid(const guid &type) const noexcept {
    return find_if([&](const partition_info &p) { return p.scheme == partition_scheme::gpt && p.type_guid == type; });
  }
  /** @brief GPT: the partition with unique GUID @p id. */
  [[nodiscard]] result<partition_info> find_by_unique_guid(const guid &id) const noexcept {
    return find_if([&](const partition_info &p) { return p.scheme == partition_scheme::gpt && p.unique_guid == id; });
  }
  /** @brief GPT: first partition named @p name (exact, ASCII-folded). */
  [[nodiscard]] result<partition_info> find_by_name(string_view name) const noexcept {
    return find_if([&](const partition_info &p) { return p.scheme == partition_scheme::gpt && p.name_view() == name; });
  }
  /** @brief MBR: first non-extended partition of type byte @p type. */
  [[nodiscard]] result<partition_info> find_by_mbr_type(std::uint8_t type) const noexcept {
    return find_if([&](const partition_info &p) {
      return p.scheme == partition_scheme::mbr && !p.is_extended && p.mbr_type == type;
    });
  }

  /** @brief Turns @p info into a block device over this table's disk. Fails with `error::out_of_range` if the
   * partition does not fit the disk. */
  [[nodiscard]] result<partition_device> try_open_partition(const partition_info &info) const noexcept {
    return partition_device::try_create(dev_, info.first_lba, info.block_count);
  }
  [[nodiscard]] result<partition_device> try_open_partition(std::uint32_t number) const noexcept {
    auto info = try_get(number);
    if (!info)
      return unexpected(info.error());
    return try_open_partition(*info);
  }

private:
  friend class partition_iterator;

  [[nodiscard]] static result<void> check_device(const block_device_ref &dev) noexcept {
    const std::size_t bs = dev.block_size();
    if (!dev || bs < 512 || bs > partition_max_block_size)
      return unexpected(error::unsupported_operation);
    return {};
  }

  // Reads and validates the GPT header at @p lba and the entry array it describes; fills the gpt_* members.
  [[nodiscard]] result<void> read_gpt_header(std::uint64_t lba) noexcept {
    const std::size_t bs = dev_.block_size();
    array<std::byte, partition_max_block_size> block;
    const span<std::byte> whole(block);
    auto hdr = whole.first(bs);
    if (auto r = dev_.try_read_blocks(lba, hdr); !r)
      return r;
    const auto h = span<const std::byte>(hdr);

    static constexpr array<std::byte, 8> signature = {std::byte{'E'}, std::byte{'F'}, std::byte{'I'}, std::byte{' '},
                                                      std::byte{'P'}, std::byte{'A'}, std::byte{'R'}, std::byte{'T'}};
    for (std::size_t i = 0; i < 8; ++i)
      if (h[i] != signature[i])
        return unexpected(error::invalid_argument);

    auto header_size = boot_bytes::read_le_at<std::uint32_t>(h, 12);
    auto header_crc = boot_bytes::read_le_at<std::uint32_t>(h, 16);
    auto my_lba = boot_bytes::read_le_at<std::uint64_t>(h, 24);
    auto first_usable = boot_bytes::read_le_at<std::uint64_t>(h, 40);
    auto last_usable = boot_bytes::read_le_at<std::uint64_t>(h, 48);
    auto disk = structo::boot::uefi::read_guid(h, 56);
    auto entries_lba = boot_bytes::read_le_at<std::uint64_t>(h, 72);
    auto num = boot_bytes::read_le_at<std::uint32_t>(h, 80);
    auto esize = boot_bytes::read_le_at<std::uint32_t>(h, 84);
    auto entries_crc = boot_bytes::read_le_at<std::uint32_t>(h, 88);
    if (!header_size || !header_crc || !my_lba || !first_usable || !last_usable || !disk || !entries_lba || !num ||
        !esize || !entries_crc)
      return unexpected(error::invalid_argument);

    if (*header_size < 92 || *header_size > bs)
      return unexpected(error::invalid_argument);
    static constexpr array<std::byte, 4> zero_crc = {};
    std::uint32_t crc = boot_bytes::crc32_update(0, h.first(16));
    crc = boot_bytes::crc32_update(crc, span<const std::byte>(zero_crc));
    crc = boot_bytes::crc32_update(crc, h.subspan(20, *header_size - 20));
    if (crc != *header_crc || *my_lba != lba)
      return unexpected(error::invalid_argument);

    if (*esize < detail::gpt_min_entry_size || *esize % 8 != 0 || *esize > partition_max_block_size ||
        *num > detail::gpt_max_entries || *first_usable > *last_usable || *last_usable >= dev_.block_count())
      return unexpected(error::invalid_argument);

    // Validate the entry array in whole blocks (the array starts block-aligned).
    const std::uint64_t total_bytes = static_cast<std::uint64_t>(*num) * *esize;
    const std::uint64_t array_blocks = (total_bytes + bs - 1) / bs;
    if (!detail::fits(dev_, *entries_lba, array_blocks))
      return unexpected(error::invalid_argument);
    std::uint32_t acrc = 0;
    std::uint64_t remaining = total_bytes;
    for (std::uint64_t b = 0; b < array_blocks; ++b) {
      auto one = whole.first(bs);
      if (auto r = dev_.try_read_blocks(*entries_lba + b, one); !r)
        return r;
      const std::size_t take = remaining < bs ? static_cast<std::size_t>(remaining) : bs;
      acrc = boot_bytes::crc32_update(acrc, span<const std::byte>(one).first(take));
      remaining -= take;
    }
    if (acrc != *entries_crc)
      return unexpected(error::invalid_argument);

    disk_guid_ = *disk;
    gpt_entries_lba_ = *entries_lba;
    gpt_num_entries_ = *num;
    gpt_entry_size_ = *esize;
    return {};
  }

  block_device_ref dev_;
  partition_scheme scheme_ = partition_scheme::none;
  array<std::byte, 64> mbr_entries_{};
  guid disk_guid_{};
  std::uint64_t gpt_entries_lba_ = 0;
  std::uint32_t gpt_num_entries_ = 0;
  std::uint32_t gpt_entry_size_ = 0;
  bool used_backup_header_ = false;
};

class RELOCO_POINTER partition_iterator : public iterator_adaptor<partition_iterator, result<partition_info>> {
public:
  using item_type = result<partition_info>;

  explicit partition_iterator(const partition_table &t) noexcept : table_(t) {}

  [[nodiscard]] optional<item_type> next_impl() noexcept {
    if (done_)
      return nullopt;
    switch (table_.scheme_) {
    case partition_scheme::gpt:
      return next_gpt();
    case partition_scheme::mbr:
      return next_mbr();
    default:
      done_ = true;
      return nullopt;
    }
  }

private:
  [[nodiscard]] optional<item_type> fail(error e) noexcept {
    done_ = true;
    return optional<item_type>(item_type(unexpected(e)));
  }

  [[nodiscard]] optional<item_type> next_gpt() noexcept {
    const block_device_ref dev = table_.dev_;
    while (index_ < table_.gpt_num_entries_) {
      const std::uint32_t idx = index_++;
      array<std::byte, detail::gpt_min_entry_size> raw;
      const std::uint64_t off = table_.gpt_entries_lba_ * dev.block_size() +
                                static_cast<std::uint64_t>(idx) * table_.gpt_entry_size_;
      if (auto r = detail::read_bytes(dev, off, span<std::byte>(raw)); !r)
        return fail(r.error());
      const auto e = span<const std::byte>(raw);
      auto type = structo::boot::uefi::read_guid(e, 0);
      auto unique = structo::boot::uefi::read_guid(e, 16);
      auto first = boot_bytes::read_le_at<std::uint64_t>(e, 32);
      auto last = boot_bytes::read_le_at<std::uint64_t>(e, 40);
      auto attrs = boot_bytes::read_le_at<std::uint64_t>(e, 48);
      if (!type || !unique || !first || !last || !attrs)
        return fail(error::invalid_argument);
      if (*type == guid{})
        continue;
      if (*first > *last || *last >= dev.block_count())
        return fail(error::invalid_argument);

      partition_info p;
      p.scheme = partition_scheme::gpt;
      p.number = idx + 1;
      p.first_lba = *first;
      p.block_count = *last - *first + 1;
      p.type_guid = *type;
      p.unique_guid = *unique;
      p.attributes = *attrs;
      p.bootable = (*attrs & gpt_attributes::legacy_bios_bootable) != 0;
      // 36 UTF-16LE code units; stop at the first NUL, fold to ASCII.
      for (std::size_t i = 0; i < 36; ++i) {
        const auto unit = boot_bytes::read_le_at<std::uint16_t>(e, 56 + i * 2).value_or(0);
        if (unit == 0)
          break;
        p.name[i] = unit < 0x80 ? static_cast<char>(unit) : '?';
        p.name_length = static_cast<std::uint8_t>(i + 1);
      }
      return optional<item_type>(item_type(p));
    }
    done_ = true;
    return nullopt;
  }

  [[nodiscard]] optional<item_type> next_mbr() noexcept {
    const block_device_ref dev = table_.dev_;
    const auto entries = span<const std::byte>(table_.mbr_entries_);

    while (phase_ == 0) {
      if (slot_ == 4) {
        if (ext_count_ == 0) {
          done_ = true;
          return nullopt;
        }
        phase_ = 1;
        ebr_lba_ = ext_first_;
        number_ = 5;
        break;
      }
      const std::uint32_t slot = slot_++;
      auto e = detail::decode_mbr_entry(entries, slot * detail::mbr_entry_size);
      if (!e)
        return fail(e.error());
      if (!e->used())
        continue;
      if (!detail::fits(dev, e->start, e->count))
        return fail(error::out_of_range);
      partition_info p;
      p.scheme = partition_scheme::mbr;
      p.number = slot + 1;
      p.first_lba = e->start;
      p.block_count = e->count;
      p.mbr_type = e->type;
      p.bootable = e->bootable;
      p.is_extended = mbr_types::is_extended(e->type);
      if (p.is_extended && ext_count_ == 0) {
        ext_first_ = e->start;
        ext_count_ = e->count;
      }
      return optional<item_type>(item_type(p));
    }

    // Logical partitions: each EBR holds the logical partition (entry 0, relative to the EBR) and a link to the
    // next EBR (entry 1, relative to the start of the extended partition).
    while (logical_seen_ < detail::mbr_max_logical) {
      array<std::byte, 512> sector;
      if (auto r = detail::read_bytes(dev, ebr_lba_ * dev.block_size(), span<std::byte>(sector)); !r)
        return fail(r.error());
      const auto s = span<const std::byte>(sector);
      auto sig = boot_bytes::read_le_at<std::uint16_t>(s, detail::mbr_signature_offset);
      if (!sig || *sig != 0xAA55)
        return fail(error::invalid_argument);
      auto logical = detail::decode_mbr_entry(s, detail::mbr_entries_offset);
      auto link = detail::decode_mbr_entry(s, detail::mbr_entries_offset + detail::mbr_entry_size);
      if (!logical || !link)
        return fail(error::invalid_argument);
      ++logical_seen_;

      optional<item_type> result_item;
      if (logical->used()) {
        const std::uint64_t first = ebr_lba_ + logical->start;
        if (!detail::fits(dev, first, logical->count))
          return fail(error::out_of_range);
        partition_info p;
        p.scheme = partition_scheme::mbr;
        p.number = number_++;
        p.first_lba = first;
        p.block_count = logical->count;
        p.mbr_type = logical->type;
        p.bootable = logical->bootable;
        result_item = optional<item_type>(item_type(p));
      }

      if (link->used()) {
        const std::uint64_t next = ext_first_ + link->start;
        // The chain must move forward and stay on the disk, which also rules out loops.
        if (next <= ebr_lba_ || next >= dev.block_count())
          return fail(error::invalid_argument);
        ebr_lba_ = next;
      } else {
        logical_seen_ = detail::mbr_max_logical; // End of chain; stop after reporting this one.
      }
      if (result_item)
        return result_item;
    }
    done_ = true;
    return nullopt;
  }

  partition_table table_;
  bool done_ = false;
  std::uint32_t index_ = 0;        // GPT: next entry index
  std::uint32_t phase_ = 0;        // MBR: 0 = primary slots, 1 = EBR chain
  std::uint32_t slot_ = 0;
  std::uint64_t ext_first_ = 0;
  std::uint64_t ext_count_ = 0;
  std::uint64_t ebr_lba_ = 0;
  std::uint32_t number_ = 5;
  std::uint32_t logical_seen_ = 0;
};

inline partition_iterator partition_table::entries() const noexcept { return partition_iterator(*this); }

template <typename Pred> inline result<partition_info> partition_table::find_if(Pred pred) const noexcept {
  auto it = entries();
  for (auto p : it) {
    if (!p)
      return p;
    if (pred(*p))
      return p;
  }
  return unexpected(error::not_found);
}

inline result<partition_info> partition_table::try_get(std::uint32_t number) const noexcept {
  return find_if([&](const partition_info &p) { return p.number == number; });
}

} // namespace structo::hw
