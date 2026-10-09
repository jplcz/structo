// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file fat.hpp
 * @brief `structo::fs::fat_filesystem`: a read-only FAT12/FAT16/FAT32 reader (with long file names) that plugs
 * into `filesystem_ref` through `filesystem_traits<fat_filesystem>`.
 *
 * It reads through a `block_device_ref` (typically a partition from `partition_table`, optionally behind a
 * `block_cache_ref`), allocates nothing, and keeps a small fixed table of open files/directories
 * (`max_open_objects`). The FAT variant is derived from the cluster count, as the specification requires.
 *
 * Paths use `/` as separator, are matched case-insensitively (ASCII folding) against the long name, and fall back
 * to the 8.3 name when no valid long name is present. Leading, trailing and repeated slashes are ignored.
 *
 * @code
 * // Why: a bootloader reads its config/kernel from the EFI System Partition (FAT) it was started from.
 * auto table = structo::hw::partition_table::try_open(disk, scratch);   // disk: block_device_ref, scratch: >= 1 block
 * auto esp = table->find_by_type_guid(structo::hw::gpt_types::efi_system);
 * auto part = table->try_open_partition(*esp);                          // partition_device (keep alive and in place)
 * auto part_dev = part->as_block_device();                              // block_device_ref over the partition
 *
 * // `mount_scratch` is caller storage of at least one device block (<= 4096 bytes); the filesystem uses it for
 * // unaligned reads and must not be shared with other users while mounted.
 * reloco::array<std::byte, 4096> mount_scratch;
 * auto fat = structo::fs::fat_filesystem::try_mount(part_dev, mount_scratch);   // validates the boot sector/BPB
 *
 * // `fat` must stay alive and in place while `fs` (or any file opened through it) is used.
 * structo::fs::filesystem_ref fs(*fat);
 * reloco::array<std::byte, 4096> cfg;
 * auto n = fs.try_read_file("/EFI/BOOT/boot.cfg", cfg);                 // bytes read
 * @endcode
 */

#include <structo/detail/boot_bytes.hpp>
#include <structo/fs/filesystem_ref.hpp>
#include <structo/hw/block_device_ref.hpp>

#include <reloco/array.hpp>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/span.hpp>
#include <reloco/string_view.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace structo::fs {

using namespace reloco;
namespace boot_bytes = structo::boot::detail;

enum class fat_type : std::uint8_t { fat12, fat16, fat32 };

class fat_filesystem {
public:
  /** @brief Size of the open-object table shared by files and directories. */
  static constexpr std::size_t max_open_objects = 8;

  /** @brief Validates the BPB on @p dev and returns a mounted, read-only filesystem. @p scratch must hold at least
   * one device block and stay valid for the lifetime of the object.
   * Fails with `error::invalid_argument` if the volume is not a valid FAT volume. */
  [[nodiscard]] static result<fat_filesystem> try_mount(hw::block_device_ref dev,
                                                        span<std::byte> scratch) noexcept {
    fat_filesystem fs(dev, scratch);
    if (auto r = fs.parse_boot_sector(); !r)
      return unexpected(r.error());
    return fs;
  }

  [[nodiscard]] fat_type type() const noexcept { return type_; }
  [[nodiscard]] std::uint32_t cluster_size() const noexcept { return cluster_bytes_; }
  [[nodiscard]] std::uint32_t cluster_count() const noexcept { return cluster_count_; }
  /** @brief Number of currently open files and directories. */
  [[nodiscard]] std::size_t open_count() const noexcept {
    std::size_t n = 0;
    for (const auto &s : slots_)
      n += s.used ? 1 : 0;
    return n;
  }

private:
  friend struct filesystem_traits<fat_filesystem>;

  static constexpr std::uint32_t dirent_size = 32;
  static constexpr std::uint8_t attr_volume_id = 0x08;
  static constexpr std::uint8_t attr_directory = 0x10;
  static constexpr std::uint8_t attr_lfn = 0x0F;

  // Position inside a directory. `cluster` holds the cluster containing entry `index` (0 for the fixed FAT12/16 root).
  struct cursor {
    std::uint32_t start = 0;
    std::uint32_t cluster = 0;
    std::uint32_t index = 0;
    bool end = false;
  };

  struct found {
    array<char, max_name_length> name{};
    std::uint8_t name_length = 0;
    std::uint8_t attr = 0;
    std::uint32_t cluster = 0;
    std::uint32_t size = 0;

    [[nodiscard]] string_view name_view() const noexcept { return string_view(name.data(), name_length); }
    [[nodiscard]] bool is_dir() const noexcept { return (attr & attr_directory) != 0; }
  };

  struct slot {
    bool used = false;
    bool dir = false;
    std::uint32_t first_cluster = 0;
    std::uint32_t size = 0;
    cursor cur;
    // Cached position in the cluster chain so sequential reads do not rewalk it.
    std::uint32_t hint_index = 0;
    std::uint32_t hint_cluster = 0;
  };

  fat_filesystem(hw::block_device_ref dev, span<std::byte> scratch) noexcept : dev_(dev), scratch_(scratch) {}

  template <std::size_t N> [[nodiscard]] result<array<std::byte, N>> read_at(std::uint64_t offset) const noexcept {
    array<std::byte, N> buf{};
    auto r = dev_.try_read_bytes(offset, span<std::byte>(buf), scratch_);
    if (!r)
      return unexpected(r.error());
    return buf;
  }

  template <typename T> [[nodiscard]] static T le(span<const std::byte> b, std::size_t off) noexcept {
    auto v = boot_bytes::read_le_at<T>(b, off);
    return v ? *v : T{};
  }

  result<void> parse_boot_sector() noexcept {
    auto sec = read_at<512>(0);
    if (!sec)
      return unexpected(sec.error());
    const span<const std::byte> b(*sec);
    if (le<std::uint16_t>(b, 510) != 0xAA55)
      return unexpected(error::invalid_argument);

    const std::uint32_t bps = le<std::uint16_t>(b, 11);
    const std::uint32_t spc = static_cast<std::uint8_t>((*sec)[13]);
    const std::uint32_t reserved = le<std::uint16_t>(b, 14);
    const std::uint32_t nfats = static_cast<std::uint8_t>((*sec)[16]);
    const std::uint32_t root_entries = le<std::uint16_t>(b, 17);
    std::uint32_t total = le<std::uint16_t>(b, 19);
    if (total == 0)
      total = le<std::uint32_t>(b, 32);
    std::uint32_t fat_sectors = le<std::uint16_t>(b, 22);
    if (fat_sectors == 0)
      fat_sectors = le<std::uint32_t>(b, 36);

    const bool bps_ok = bps >= 512 && bps <= 4096 && (bps & (bps - 1)) == 0;
    const bool spc_ok = spc >= 1 && spc <= 128 && (spc & (spc - 1)) == 0;
    if (!bps_ok || !spc_ok || reserved == 0 || nfats == 0 || nfats > 2 || fat_sectors == 0 || total == 0)
      return unexpected(error::invalid_argument);

    const std::uint32_t root_sectors = (root_entries * dirent_size + bps - 1) / bps;
    const std::uint64_t meta = static_cast<std::uint64_t>(reserved) + static_cast<std::uint64_t>(nfats) * fat_sectors + root_sectors;
    if (meta >= total)
      return unexpected(error::invalid_argument);

    const std::uint32_t data_sectors = total - static_cast<std::uint32_t>(meta);
    cluster_count_ = data_sectors / spc;
    if (cluster_count_ == 0)
      return unexpected(error::invalid_argument);
    type_ = cluster_count_ < 4085 ? fat_type::fat12 : cluster_count_ < 65525 ? fat_type::fat16 : fat_type::fat32;

    if (type_ == fat_type::fat32) {
      root_cluster_ = le<std::uint32_t>(b, 44);
      if (root_entries != 0 || root_cluster_ < 2 || root_cluster_ >= cluster_count_ + 2)
        return unexpected(error::invalid_argument);
    } else if (root_entries == 0) {
      return unexpected(error::invalid_argument);
    }

    cluster_bytes_ = bps * spc;
    fat_offset_ = static_cast<std::uint64_t>(reserved) * bps;
    root_offset_ = fat_offset_ + static_cast<std::uint64_t>(nfats) * fat_sectors * bps;
    root_entry_count_ = root_entries;
    data_offset_ = root_offset_ + static_cast<std::uint64_t>(root_sectors) * bps;
    if (data_offset_ + static_cast<std::uint64_t>(cluster_count_) * cluster_bytes_ > dev_.size_bytes())
      return unexpected(error::invalid_argument);
    return {};
  }

  [[nodiscard]] std::uint64_t cluster_offset(std::uint32_t cluster) const noexcept {
    return data_offset_ + static_cast<std::uint64_t>(cluster - 2) * cluster_bytes_;
  }
  [[nodiscard]] bool valid_cluster(std::uint32_t c) const noexcept { return c >= 2 && c < cluster_count_ + 2; }

  // Returns the next cluster in the chain, 0 at the end of the chain. A free, bad or out-of-range link is `io_error`.
  [[nodiscard]] result<std::uint32_t> next_cluster(std::uint32_t c) const noexcept {
    if (!valid_cluster(c))
      return unexpected(error::io_error);
    std::uint32_t v = 0;
    std::uint32_t eoc = 0;
    if (type_ == fat_type::fat12) {
      auto raw = read_at<2>(fat_offset_ + c + c / 2);
      if (!raw)
        return unexpected(raw.error());
      v = le<std::uint16_t>(span<const std::byte>(*raw), 0);
      v = (c & 1) ? (v >> 4) : (v & 0x0FFF);
      eoc = 0xFF8;
    } else if (type_ == fat_type::fat16) {
      auto raw = read_at<2>(fat_offset_ + static_cast<std::uint64_t>(c) * 2);
      if (!raw)
        return unexpected(raw.error());
      v = le<std::uint16_t>(span<const std::byte>(*raw), 0);
      eoc = 0xFFF8;
    } else {
      auto raw = read_at<4>(fat_offset_ + static_cast<std::uint64_t>(c) * 4);
      if (!raw)
        return unexpected(raw.error());
      v = le<std::uint32_t>(span<const std::byte>(*raw), 0) & 0x0FFFFFFF;
      eoc = 0x0FFFFFF8;
    }
    if (v >= eoc)
      return std::uint32_t{0};
    if (!valid_cluster(v))
      return unexpected(error::io_error);
    return v;
  }

  [[nodiscard]] std::uint32_t dir_start(std::uint32_t cluster) const noexcept {
    return (cluster == 0 && type_ == fat_type::fat32) ? root_cluster_ : cluster;
  }

  [[nodiscard]] cursor begin_dir(std::uint32_t first_cluster) const noexcept {
    const std::uint32_t s = dir_start(first_cluster);
    return cursor{s, s, 0, false};
  }

  // Reads the raw 32-byte entry at the cursor and advances. Returns false at the end of the directory.
  [[nodiscard]] result<bool> read_raw(cursor &c, array<std::byte, dirent_size> &out) const noexcept {
    if (c.end)
      return false;
    std::uint64_t off = 0;
    if (c.start == 0) {
      if (c.index >= root_entry_count_) {
        c.end = true;
        return false;
      }
      off = root_offset_ + static_cast<std::uint64_t>(c.index) * dirent_size;
    } else {
      const std::uint32_t per_cluster = cluster_bytes_ / dirent_size;
      off = cluster_offset(c.cluster) + static_cast<std::uint64_t>(c.index % per_cluster) * dirent_size;
    }
    auto raw = read_at<dirent_size>(off);
    if (!raw)
      return unexpected(raw.error());
    out = *raw;
    ++c.index;
    if (c.start != 0 && c.index % (cluster_bytes_ / dirent_size) == 0) {
      auto next = next_cluster(c.cluster);
      if (!next)
        return unexpected(next.error());
      if (*next == 0)
        c.end = true;
      else
        c.cluster = *next;
    }
    return true;
  }

  [[nodiscard]] static std::uint8_t short_name_checksum(span<const std::byte> name11) noexcept {
    std::uint8_t sum = 0;
    for (std::size_t i = 0; i < 11; ++i)
      sum = static_cast<std::uint8_t>(((sum & 1) << 7) + (sum >> 1) + static_cast<std::uint8_t>(name11[i]));
    return sum;
  }

  static void append_utf8(found &f, std::uint32_t cp) noexcept {
    array<char, 4> tmp{};
    std::size_t n = 0;
    if (cp < 0x80) {
      tmp[0] = static_cast<char>(cp);
      n = 1;
    } else if (cp < 0x800) {
      tmp[0] = static_cast<char>(0xC0 | (cp >> 6));
      tmp[1] = static_cast<char>(0x80 | (cp & 0x3F));
      n = 2;
    } else if (cp < 0x10000) {
      tmp[0] = static_cast<char>(0xE0 | (cp >> 12));
      tmp[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      tmp[2] = static_cast<char>(0x80 | (cp & 0x3F));
      n = 3;
    } else {
      tmp[0] = static_cast<char>(0xF0 | (cp >> 18));
      tmp[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      tmp[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      tmp[3] = static_cast<char>(0x80 | (cp & 0x3F));
      n = 4;
    }
    if (f.name_length + n > max_name_length)
      return;
    for (std::size_t i = 0; i < n; ++i)
      f.name[f.name_length++] = tmp[i];
  }

  static constexpr std::size_t lfn_chars_per_entry = 13;
  static constexpr std::size_t lfn_max_entries = 20;

  // Next real directory entry (file or directory), with its long name when present. Returns false at the end.
  [[nodiscard]] result<bool> next_entry(cursor &c, found &out) const noexcept {
    array<std::uint16_t, lfn_chars_per_entry * lfn_max_entries> lfn{};
    std::size_t lfn_len = 0;
    std::uint8_t lfn_sum = 0;
    std::size_t lfn_expected = 0; // sequence number the next LFN entry must carry; 0 = no LFN in progress
    for (;;) {
      array<std::byte, dirent_size> e{};
      auto more = read_raw(c, e);
      if (!more)
        return unexpected(more.error());
      if (!*more)
        return false;
      const span<const std::byte> b(e);
      const auto first = static_cast<std::uint8_t>(e[0]);
      if (first == 0x00) {
        c.end = true;
        return false;
      }
      if (first == 0xE5) {
        lfn_expected = 0;
        continue;
      }
      const auto attr = static_cast<std::uint8_t>(e[11]);
      if (attr == attr_lfn) {
        const std::size_t seq = first & 0x1F;
        if (first & 0x40) {
          if (seq == 0 || seq > lfn_max_entries) {
            lfn_expected = 0;
            continue;
          }
          lfn_len = seq * lfn_chars_per_entry;
          lfn_sum = static_cast<std::uint8_t>(e[13]);
          lfn.fill(0);
          lfn_expected = seq;
        } else if (lfn_expected == 0 || seq != lfn_expected || static_cast<std::uint8_t>(e[13]) != lfn_sum) {
          lfn_expected = 0;
          continue;
        }
        // Each LFN entry holds 13 UTF-16 units at these byte offsets.
        constexpr array<std::size_t, lfn_chars_per_entry> offs{1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
        for (std::size_t i = 0; i < lfn_chars_per_entry; ++i)
          lfn[(seq - 1) * lfn_chars_per_entry + i] = le<std::uint16_t>(b, offs[i]);
        lfn_expected = seq - 1;
        if (lfn_expected == 0)
          lfn_expected = static_cast<std::size_t>(-1); // complete; validated by the short entry
        continue;
      }
      if (attr & attr_volume_id) {
        lfn_expected = 0;
        continue;
      }

      out = found{};
      out.attr = attr;
      out.cluster = static_cast<std::uint32_t>(le<std::uint16_t>(b, 26));
      if (type_ == fat_type::fat32)
        out.cluster |= static_cast<std::uint32_t>(le<std::uint16_t>(b, 20)) << 16;
      out.size = le<std::uint32_t>(b, 28);

      const bool have_lfn = lfn_expected == static_cast<std::size_t>(-1) && lfn_len != 0 &&
                            short_name_checksum(b.first(11)) == lfn_sum;
      if (have_lfn) {
        for (std::size_t i = 0; i < lfn_len && lfn[i] != 0 && lfn[i] != 0xFFFF; ++i) {
          std::uint32_t cp = lfn[i];
          if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < lfn_len && lfn[i + 1] >= 0xDC00 && lfn[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lfn[i + 1] - 0xDC00);
            ++i;
          }
          append_utf8(out, cp);
        }
      } else {
        // 8.3 name: base and extension, space padded; bits 3/4 of byte 12 request lower case (NT convention).
        const bool lower_base = (static_cast<std::uint8_t>(e[12]) & 0x08) != 0;
        const bool lower_ext = (static_cast<std::uint8_t>(e[12]) & 0x10) != 0;
        auto put = [&](std::size_t i, bool lower) {
          char ch = static_cast<char>(e[i]);
          if (lower && ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
          out.name[out.name_length++] = ch;
        };
        std::size_t base_len = 8;
        while (base_len > 0 && static_cast<char>(e[base_len - 1]) == ' ')
          --base_len;
        std::size_t ext_len = 3;
        while (ext_len > 0 && static_cast<char>(e[8 + ext_len - 1]) == ' ')
          --ext_len;
        // 0x05 stands in for a leading 0xE5 byte.
        for (std::size_t i = 0; i < base_len; ++i) {
          if (i == 0 && first == 0x05)
            out.name[out.name_length++] = static_cast<char>(0xE5);
          else
            put(i, lower_base);
        }
        if (ext_len != 0) {
          out.name[out.name_length++] = '.';
          for (std::size_t i = 0; i < ext_len; ++i)
            put(8 + i, lower_ext);
        }
      }
      return true;
    }
  }

  [[nodiscard]] static bool names_equal(string_view a, string_view b) noexcept {
    if (a.size() != b.size())
      return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
      char x = a[i];
      char y = b[i];
      if (x >= 'A' && x <= 'Z')
        x = static_cast<char>(x - 'A' + 'a');
      if (y >= 'A' && y <= 'Z')
        y = static_cast<char>(y - 'A' + 'a');
      if (x != y)
        return false;
    }
    return true;
  }

  struct resolved {
    bool is_dir = true;
    std::uint32_t cluster = 0; // 0 for the root directory
    std::uint32_t size = 0;
  };

  [[nodiscard]] result<resolved> resolve(string_view path) const noexcept {
    resolved cur;
    std::size_t pos = 0;
    bool at_root = true;
    while (pos < path.size()) {
      while (pos < path.size() && path[pos] == '/')
        ++pos;
      if (pos == path.size())
        break;
      std::size_t end = pos;
      while (end < path.size() && path[end] != '/')
        ++end;
      const string_view comp = path.substr(pos, end - pos);
      pos = end;
      if (!cur.is_dir)
        return unexpected(error::invalid_argument);
      if (comp == "." || (comp == ".." && at_root))
        continue;
      auto c = begin_dir(cur.cluster);
      found f;
      bool hit = false;
      for (;;) {
        auto more = next_entry(c, f);
        if (!more)
          return unexpected(more.error());
        if (!*more)
          break;
        if (names_equal(f.name_view(), comp)) {
          hit = true;
          break;
        }
      }
      if (!hit)
        return unexpected(error::not_found);
      cur = resolved{f.is_dir(), f.cluster, f.is_dir() ? 0u : f.size};
      at_root = f.is_dir() && f.cluster == 0;
    }
    return cur;
  }

  [[nodiscard]] result<fs_handle> alloc_slot(const resolved &r) noexcept {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      if (slots_[i].used)
        continue;
      slot s;
      s.used = true;
      s.dir = r.is_dir;
      s.first_cluster = r.cluster;
      s.size = r.size;
      s.cur = r.is_dir ? begin_dir(r.cluster) : cursor{};
      s.hint_index = 0;
      s.hint_cluster = r.cluster;
      slots_[i] = s;
      return static_cast<fs_handle>(i + 1);
    }
    return unexpected(error::busy);
  }

  // Validates a handle and returns its slot index.
  [[nodiscard]] result<std::size_t> index_of(fs_handle h, bool want_dir) const noexcept {
    if (h == 0 || h > slots_.size())
      return unexpected(error::invalid_argument);
    const auto i = static_cast<std::size_t>(h - 1);
    if (!slots_[i].used || slots_[i].dir != want_dir)
      return unexpected(error::invalid_argument);
    return i;
  }

  hw::block_device_ref dev_;
  span<std::byte> scratch_;
  fat_type type_ = fat_type::fat16;
  std::uint32_t cluster_bytes_ = 0;
  std::uint32_t cluster_count_ = 0;
  std::uint32_t root_cluster_ = 0;
  std::uint32_t root_entry_count_ = 0;
  std::uint64_t fat_offset_ = 0;
  std::uint64_t root_offset_ = 0;
  std::uint64_t data_offset_ = 0;
  array<slot, max_open_objects> slots_{};
};

template <> struct filesystem_traits<fat_filesystem> {
  static result<fs_handle> try_open(fat_filesystem &f, string_view path, open_flags flags) noexcept {
    if (flags != open_flags::read)
      return unexpected(error::permission_denied);
    auto r = f.resolve(path);
    if (!r)
      return unexpected(r.error());
    if (r->is_dir)
      return unexpected(error::invalid_argument);
    return f.alloc_slot(*r);
  }
  static result<fs_handle> try_open_dir(fat_filesystem &f, string_view path) noexcept {
    auto r = f.resolve(path);
    if (!r)
      return unexpected(r.error());
    if (!r->is_dir)
      return unexpected(error::invalid_argument);
    return f.alloc_slot(*r);
  }
  static result<void> try_close(fat_filesystem &f, fs_handle h) noexcept {
    if (h == 0 || h > f.slots_.size() || !f.slots_[static_cast<std::size_t>(h - 1)].used)
      return unexpected(error::invalid_argument);
    f.slots_[static_cast<std::size_t>(h - 1)].used = false;
    return {};
  }
  static result<std::size_t> try_read(fat_filesystem &f, fs_handle h, std::uint64_t offset,
                                      span<std::byte> dst) noexcept {
    auto si = f.index_of(h, false);
    if (!si)
      return unexpected(si.error());
    auto &s = f.slots_[*si];
    if (offset >= s.size || dst.empty())
      return std::size_t{0};
    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(dst.size(), s.size - offset));
    const std::uint32_t target = static_cast<std::uint32_t>(offset / f.cluster_bytes_);

    // Continue from the cached chain position when it is at or before the target, else restart at the head.
    std::uint32_t idx = s.hint_index;
    std::uint32_t cl = s.hint_cluster;
    if (idx > target) {
      idx = 0;
      cl = s.first_cluster;
    }
    while (idx < target) {
      auto next = f.next_cluster(cl);
      if (!next)
        return unexpected(next.error());
      if (*next == 0)
        return unexpected(error::io_error); // chain shorter than the recorded size
      cl = *next;
      ++idx;
    }

    std::size_t done = 0;
    while (done < want) {
      const std::uint64_t pos = offset + done;
      const auto in_cluster = static_cast<std::size_t>(pos % f.cluster_bytes_);
      const std::size_t n = std::min<std::size_t>(want - done, f.cluster_bytes_ - in_cluster);
      auto r = f.dev_.try_read_bytes(f.cluster_offset(cl) + in_cluster, dst.subspan(done, n), f.scratch_);
      if (!r)
        return unexpected(r.error());
      done += n;
      s.hint_index = idx;
      s.hint_cluster = cl;
      if (done < want) {
        auto next = f.next_cluster(cl);
        if (!next)
          return unexpected(next.error());
        if (*next == 0)
          return unexpected(error::io_error);
        cl = *next;
        ++idx;
      }
    }
    return done;
  }
  static result<bool> try_read_dir(fat_filesystem &f, fs_handle h, dir_entry &out) noexcept {
    auto si = f.index_of(h, true);
    if (!si)
      return unexpected(si.error());
    fat_filesystem::found e;
    auto more = f.next_entry(f.slots_[*si].cur, e);
    if (!more)
      return unexpected(more.error());
    if (!*more)
      return false;
    out = dir_entry{};
    out.set_name(e.name_view());
    out.type = e.is_dir() ? file_type::directory : file_type::regular;
    out.size = e.is_dir() ? 0 : e.size;
    return true;
  }
  static result<file_info> try_stat(fat_filesystem &f, string_view path) noexcept {
    auto r = f.resolve(path);
    if (!r)
      return unexpected(r.error());
    return file_info{r->is_dir ? file_type::directory : file_type::regular, r->size};
  }
  static result<file_info> try_fstat(fat_filesystem &f, fs_handle h) noexcept {
    if (h == 0 || h > f.slots_.size() || !f.slots_[static_cast<std::size_t>(h - 1)].used)
      return unexpected(error::invalid_argument);
    const auto &s = f.slots_[static_cast<std::size_t>(h - 1)];
    return file_info{s.dir ? file_type::directory : file_type::regular, s.size};
  }
};

} // namespace structo::fs
