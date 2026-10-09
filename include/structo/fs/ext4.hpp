// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file ext4.hpp
 * @brief `structo::fs::ext4_filesystem`: a read-only ext2/ext3/ext4 reader that plugs into `filesystem_ref`
 * through `filesystem_traits<ext4_filesystem>`.
 *
 * Supported: 1/2/4 KiB blocks, extent trees and classic (direct/indirect) block maps, 32- and 64-bit group
 * descriptors, flex_bg, sparse files (holes read as zeros; uninitialized extents too), linear directory scan (so
 * htree-indexed directories work), fast and slow symlinks (followed in paths, up to `max_symlink_depth` levels).
 * Checksums are not verified and extended attributes are ignored.
 *
 * Not supported (mount fails with `unsupported_operation`): meta_bg, inline_data, encryption, casefold,
 * compression, journal devices, blocks larger than 4 KiB. A volume whose journal needs recovery (unclean
 * shutdown) is refused with `invalid_state` because on-disk metadata may be stale.
 *
 * Allocation-free; keeps a fixed table of `max_open_objects` open files/directories. Paths use `/`, are matched
 * case-sensitively, and ".", ".." and symlinks may be used in them.
 *
 * @code
 * // Why: a bootloader reads /boot/vmlinuz, the initrd and a config from a Linux root/boot partition.
 * auto part = table->try_open_partition(*linux_root);    // partition_device; keep alive and in place
 * auto dev = part->as_block_device();                    // block_device_ref over the partition
 *
 * // Caller storage of at least one device block (<= 4096 B), used for unaligned reads; it must outlive the mount
 * // and not be shared meanwhile.
 * reloco::array<std::byte, 4096> scratch;
 *
 * // Validates the superblock and group layout. invalid_argument: not ext2/3/4; unsupported_operation: a feature
 * // listed above; invalid_state: journal needs recovery.
 * auto ext = structo::fs::ext4_filesystem::try_mount(dev, scratch);
 *
 * // `ext` must stay alive and in place while `fs`, or anything opened through it, is used.
 * structo::fs::filesystem_ref fs(*ext);
 * auto f = fs.try_open("/boot/vmlinuz");                 // owning file; path, followed symlinks included
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

class ext4_filesystem {
public:
  /** @brief Size of the open-object table shared by files and directories. */
  static constexpr std::size_t max_open_objects = 8;
  /** @brief Maximum number of nested symlink resolutions in one path lookup. */
  static constexpr unsigned max_symlink_depth = 8;

  [[nodiscard]] static result<ext4_filesystem> try_mount(hw::block_device_ref dev,
                                                         span<std::byte> scratch) noexcept {
    ext4_filesystem fs(dev, scratch);
    if (auto r = fs.parse_superblock(); !r)
      return unexpected(r.error());
    return fs;
  }

  [[nodiscard]] std::uint32_t block_size() const noexcept { return bs_; }
  [[nodiscard]] std::uint32_t group_count() const noexcept { return group_count_; }
  [[nodiscard]] bool uses_extents() const noexcept { return (incompat_ & incompat_extents) != 0; }
  /** @brief Number of currently open files and directories. */
  [[nodiscard]] std::size_t open_count() const noexcept {
    std::size_t n = 0;
    for (const auto &s : slots_)
      n += s.used ? 1 : 0;
    return n;
  }

private:
  friend struct filesystem_traits<ext4_filesystem>;

  static constexpr std::uint32_t root_ino = 2;
  static constexpr std::uint32_t incompat_filetype = 0x2;
  static constexpr std::uint32_t incompat_recover = 0x4;
  static constexpr std::uint32_t incompat_extents = 0x40;
  static constexpr std::uint32_t incompat_64bit = 0x80;
  static constexpr std::uint32_t incompat_largedir = 0x4000;
  // Everything we can read: filetype, extents, 64bit, mmp, flex_bg, ea_inode, csum_seed, largedir.
  static constexpr std::uint32_t incompat_supported = 0x2 | 0x40 | 0x80 | 0x100 | 0x200 | 0x400 | 0x2000 | 0x4000;
  static constexpr std::uint32_t inode_flag_extents = 0x80000;
  static constexpr std::uint32_t inode_flag_inline = 0x10000000;
  static constexpr std::uint16_t mode_mask = 0xF000;
  static constexpr std::uint16_t mode_reg = 0x8000;
  static constexpr std::uint16_t mode_dir = 0x4000;
  static constexpr std::uint16_t mode_lnk = 0xA000;
  static constexpr std::size_t symlink_buffer = 512;
  // Larger than any file; used as the length of a hole that extends to the end of the file.
  static constexpr std::uint64_t unbounded_run = 1ull << 32;

  struct inode_info {
    std::uint16_t mode = 0;
    std::uint32_t flags = 0;
    std::uint32_t blocks_512 = 0;
    std::uint32_t file_acl = 0;
    std::uint64_t size = 0;
    array<std::byte, 60> block{}; // i_block: extent root or block map

    [[nodiscard]] bool is_dir() const noexcept { return (mode & mode_mask) == mode_dir; }
    [[nodiscard]] bool is_reg() const noexcept { return (mode & mode_mask) == mode_reg; }
    [[nodiscard]] bool is_symlink() const noexcept { return (mode & mode_mask) == mode_lnk; }
    [[nodiscard]] file_type type() const noexcept {
      return is_reg() ? file_type::regular : is_dir() ? file_type::directory : is_symlink() ? file_type::symlink
                                                                                           : file_type::other;
    }
  };

  struct mapping {
    std::uint64_t phys = 0; // 0 = hole
    std::uint64_t run = 1;  // contiguous blocks from the requested one, starting at phys
  };

  struct found {
    std::uint32_t ino = 0;
    array<char, max_name_length> name{};
    std::uint8_t name_length = 0;
    [[nodiscard]] string_view name_view() const noexcept { return string_view(name.data(), name_length); }
  };

  struct slot {
    bool used = false;
    bool dir = false;
    inode_info inode;
    std::uint64_t dir_pos = 0;
  };

  ext4_filesystem(hw::block_device_ref dev, span<std::byte> scratch) noexcept : dev_(dev), scratch_(scratch) {}

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

  result<void> parse_superblock() noexcept {
    auto raw = read_at<512>(1024);
    if (!raw)
      return unexpected(raw.error());
    const span<const std::byte> b(*raw);
    if (le<std::uint16_t>(b, 56) != 0xEF53)
      return unexpected(error::invalid_argument);

    const std::uint32_t log_bs = le<std::uint32_t>(b, 24);
    if (log_bs > 2)
      return unexpected(error::unsupported_operation);
    bs_ = 1024u << log_bs;
    const std::uint32_t rev = le<std::uint32_t>(b, 76);
    inode_size_ = rev == 0 ? 128u : le<std::uint16_t>(b, 88);
    incompat_ = le<std::uint32_t>(b, 96);
    if (incompat_ & incompat_recover)
      return unexpected(error::invalid_state);
    if (incompat_ & ~incompat_supported & ~incompat_recover)
      return unexpected(error::unsupported_operation);

    inodes_count_ = le<std::uint32_t>(b, 0);
    blocks_count_ = le<std::uint32_t>(b, 4);
    if (incompat_ & incompat_64bit)
      blocks_count_ |= static_cast<std::uint64_t>(le<std::uint32_t>(b, 336)) << 32;
    first_data_block_ = le<std::uint32_t>(b, 20);
    blocks_per_group_ = le<std::uint32_t>(b, 32);
    inodes_per_group_ = le<std::uint32_t>(b, 40);
    desc_size_ = (incompat_ & incompat_64bit) ? le<std::uint16_t>(b, 254) : 32u;

    const bool inode_ok = inode_size_ >= 128 && inode_size_ <= bs_ && (inode_size_ & (inode_size_ - 1)) == 0;
    const bool desc_ok = desc_size_ >= 32 && desc_size_ <= bs_ && (desc_size_ & (desc_size_ - 1)) == 0;
    if (!inode_ok || !desc_ok || blocks_per_group_ == 0 || inodes_per_group_ == 0 || inodes_count_ == 0 ||
        blocks_count_ <= first_data_block_)
      return unexpected(error::invalid_argument);
    if (blocks_count_ > dev_.size_bytes() / bs_)
      return unexpected(error::invalid_argument);
    group_count_ = static_cast<std::uint32_t>((blocks_count_ - first_data_block_ + blocks_per_group_ - 1) / blocks_per_group_);
    if (static_cast<std::uint64_t>(group_count_) * inodes_per_group_ < inodes_count_)
      return unexpected(error::invalid_argument);
    // The root inode must be a directory.
    auto root = load_inode(root_ino);
    if (!root)
      return unexpected(error::invalid_argument);
    if (!root->is_dir())
      return unexpected(error::invalid_argument);
    return {};
  }

  [[nodiscard]] result<inode_info> load_inode(std::uint32_t ino) const noexcept {
    if (ino == 0 || ino > inodes_count_)
      return unexpected(error::invalid_argument);
    const std::uint32_t group = (ino - 1) / inodes_per_group_;
    const std::uint32_t index = (ino - 1) % inodes_per_group_;
    if (group >= group_count_)
      return unexpected(error::io_error);
    // Group descriptors follow the superblock block (no meta_bg support).
    const std::uint64_t desc_off = static_cast<std::uint64_t>(first_data_block_ + 1) * bs_ +
                                   static_cast<std::uint64_t>(group) * desc_size_;
    auto gd = read_at<12>(desc_off);
    if (!gd)
      return unexpected(gd.error());
    std::uint64_t table = le<std::uint32_t>(span<const std::byte>(*gd), 8);
    if (desc_size_ >= 64) {
      auto hi = read_at<4>(desc_off + 0x28);
      if (!hi)
        return unexpected(hi.error());
      table |= static_cast<std::uint64_t>(le<std::uint32_t>(span<const std::byte>(*hi), 0)) << 32;
    }
    if (table == 0 || table >= blocks_count_)
      return unexpected(error::io_error);
    auto raw = read_at<128>(table * bs_ + static_cast<std::uint64_t>(index) * inode_size_);
    if (!raw)
      return unexpected(raw.error());
    const span<const std::byte> b(*raw);
    inode_info in;
    in.mode = le<std::uint16_t>(b, 0);
    in.size = le<std::uint32_t>(b, 4);
    in.blocks_512 = le<std::uint32_t>(b, 28);
    in.flags = le<std::uint32_t>(b, 32);
    in.file_acl = le<std::uint32_t>(b, 104);
    if (in.is_reg() || (in.is_dir() && (incompat_ & incompat_largedir)))
      in.size |= static_cast<std::uint64_t>(le<std::uint32_t>(b, 108)) << 32;
    if (in.flags & inode_flag_inline)
      return unexpected(error::unsupported_operation);
    for (std::size_t i = 0; i < in.block.size(); ++i)
      in.block[i] = (*raw)[40 + i];
    return in;
  }

  // One 12-byte extent header/entry/index at `off` within a node (the inode's i_block, or a block on disk).
  [[nodiscard]] result<array<std::byte, 12>> node_get(const inode_info &in, bool root, std::uint64_t node_off,
                                                      std::size_t off) const noexcept {
    if (root) {
      if (off + 12 > in.block.size())
        return unexpected(error::io_error);
      array<std::byte, 12> out{};
      for (std::size_t i = 0; i < 12; ++i)
        out[i] = in.block[off + i];
      return out;
    }
    return read_at<12>(node_off + off);
  }

  [[nodiscard]] result<mapping> checked(std::uint64_t phys, std::uint64_t run) const noexcept {
    if (phys >= blocks_count_ || run > blocks_count_ - phys)
      return unexpected(error::io_error);
    return mapping{phys, run};
  }

  [[nodiscard]] result<mapping> map_extent(const inode_info &in, std::uint64_t lb) const noexcept {
    bool root = true;
    std::uint64_t node_off = 0;
    for (unsigned level = 0; level < 6; ++level) {
      auto hdr = node_get(in, root, node_off, 0);
      if (!hdr)
        return unexpected(hdr.error());
      const span<const std::byte> h(*hdr);
      if (le<std::uint16_t>(h, 0) != 0xF30A)
        return unexpected(error::io_error);
      const std::size_t entries = le<std::uint16_t>(h, 2);
      const std::size_t depth = le<std::uint16_t>(h, 6);
      const std::size_t capacity = root ? in.block.size() : bs_;
      if (depth > 5 || 12 + entries * 12 > capacity)
        return unexpected(error::io_error);

      if (depth == 0) {
        for (std::size_t i = 0; i < entries; ++i) {
          auto e = node_get(in, root, node_off, 12 + i * 12);
          if (!e)
            return unexpected(e.error());
          const span<const std::byte> x(*e);
          const std::uint64_t first = le<std::uint32_t>(x, 0);
          std::uint64_t len = le<std::uint16_t>(x, 4);
          const bool uninit = len > 32768;
          if (uninit)
            len -= 32768;
          if (lb < first)
            return mapping{0, first - lb}; // hole before this extent
          if (lb < first + len) {
            const std::uint64_t run = first + len - lb;
            if (uninit)
              return mapping{0, run}; // preallocated, reads as zeros
            const std::uint64_t start = (static_cast<std::uint64_t>(le<std::uint16_t>(x, 6)) << 32) | le<std::uint32_t>(x, 8);
            return checked(start + (lb - first), run);
          }
        }
        return mapping{0, unbounded_run};
      }

      // Index node: descend into the last child whose first block is <= lb.
      bool found_child = false;
      std::uint64_t child = 0;
      for (std::size_t i = 0; i < entries; ++i) {
        auto e = node_get(in, root, node_off, 12 + i * 12);
        if (!e)
          return unexpected(e.error());
        const span<const std::byte> x(*e);
        if (le<std::uint32_t>(x, 0) > lb) {
          if (i == 0)
            return mapping{0, le<std::uint32_t>(x, 0) - lb};
          break;
        }
        child = (static_cast<std::uint64_t>(le<std::uint16_t>(x, 8)) << 32) | le<std::uint32_t>(x, 4);
        found_child = true;
      }
      if (!found_child || child == 0 || child >= blocks_count_)
        return unexpected(error::io_error);
      root = false;
      node_off = child * bs_;
    }
    return unexpected(error::io_error);
  }

  [[nodiscard]] result<std::uint32_t> block_ptr(std::uint32_t table_block, std::uint64_t index) const noexcept {
    if (table_block == 0)
      return std::uint32_t{0};
    if (table_block >= blocks_count_)
      return unexpected(error::io_error);
    auto raw = read_at<4>(static_cast<std::uint64_t>(table_block) * bs_ + index * 4);
    if (!raw)
      return unexpected(raw.error());
    return le<std::uint32_t>(span<const std::byte>(*raw), 0);
  }

  [[nodiscard]] result<mapping> map_indirect(const inode_info &in, std::uint64_t lb) const noexcept {
    const std::uint64_t ppb = bs_ / 4;
    const span<const std::byte> blk(in.block);
    std::uint32_t p = 0;
    if (lb < 12) {
      p = le<std::uint32_t>(blk, static_cast<std::size_t>(lb) * 4);
    } else if (lb - 12 < ppb) {
      auto r = block_ptr(le<std::uint32_t>(blk, 48), lb - 12);
      if (!r)
        return unexpected(r.error());
      p = *r;
    } else if (lb - 12 - ppb < ppb * ppb) {
      const std::uint64_t n = lb - 12 - ppb;
      auto l1 = block_ptr(le<std::uint32_t>(blk, 52), n / ppb);
      if (!l1)
        return unexpected(l1.error());
      auto r = block_ptr(*l1, n % ppb);
      if (!r)
        return unexpected(r.error());
      p = *r;
    } else if (lb - 12 - ppb - ppb * ppb < ppb * ppb * ppb) {
      const std::uint64_t n = lb - 12 - ppb - ppb * ppb;
      auto l1 = block_ptr(le<std::uint32_t>(blk, 56), n / (ppb * ppb));
      if (!l1)
        return unexpected(l1.error());
      auto l2 = block_ptr(*l1, (n / ppb) % ppb);
      if (!l2)
        return unexpected(l2.error());
      auto r = block_ptr(*l2, n % ppb);
      if (!r)
        return unexpected(r.error());
      p = *r;
    } else {
      return unexpected(error::io_error);
    }
    if (p == 0)
      return mapping{0, 1};
    return checked(p, 1);
  }

  [[nodiscard]] result<mapping> map_block(const inode_info &in, std::uint64_t lb) const noexcept {
    return (in.flags & inode_flag_extents) ? map_extent(in, lb) : map_indirect(in, lb);
  }

  // Reads file contents; returns bytes read (0 at or past the end of the file).
  [[nodiscard]] result<std::size_t> inode_read(const inode_info &in, std::uint64_t offset,
                                               span<std::byte> dst) const noexcept {
    if (offset >= in.size || dst.empty())
      return std::size_t{0};
    const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(dst.size(), in.size - offset));
    std::size_t done = 0;
    while (done < want) {
      const std::uint64_t pos = offset + done;
      const std::uint64_t lb = pos / bs_;
      const std::uint64_t in_block = pos % bs_;
      auto m = map_block(in, lb);
      if (!m)
        return unexpected(m.error());
      const auto avail = static_cast<std::size_t>(std::min<std::uint64_t>(m->run * bs_ - in_block, want - done));
      if (m->phys == 0) {
        for (std::size_t i = 0; i < avail; ++i)
          dst[done + i] = std::byte{0};
      } else {
        auto r = dev_.try_read_bytes(m->phys * bs_ + in_block, dst.subspan(done, avail), scratch_);
        if (!r)
          return unexpected(r.error());
      }
      done += avail;
    }
    return done;
  }

  // Next live directory entry at `pos` (a byte offset into the directory); returns false at the end.
  [[nodiscard]] result<bool> next_dirent(const inode_info &dir, std::uint64_t &pos, found &out) const noexcept {
    for (;;) {
      if (pos >= dir.size)
        return false;
      array<std::byte, 8> hdr{};
      auto n = inode_read(dir, pos, span<std::byte>(hdr));
      if (!n)
        return unexpected(n.error());
      if (*n != 8)
        return unexpected(error::io_error);
      const span<const std::byte> h(hdr);
      const std::uint32_t ino = le<std::uint32_t>(h, 0);
      const std::size_t rec_len = le<std::uint16_t>(h, 4);
      const std::size_t name_len = static_cast<std::uint8_t>(hdr[6]);
      const std::size_t in_block = static_cast<std::size_t>(pos % bs_);
      if (rec_len < 8 || rec_len % 4 != 0 || in_block + rec_len > bs_ || 8 + name_len > rec_len)
        return unexpected(error::io_error);
      const std::uint64_t entry_pos = pos;
      pos += rec_len;
      if (ino == 0)
        continue; // deleted entry, or an htree index node disguised as one
      out = found{};
      out.ino = ino;
      out.name_length = static_cast<std::uint8_t>(name_len);
      array<std::byte, max_name_length> raw{};
      const span<std::byte> raw_span(raw);
      auto nr = inode_read(dir, entry_pos + 8, raw_span.first(name_len));
      if (!nr)
        return unexpected(nr.error());
      if (*nr != name_len)
        return unexpected(error::io_error);
      for (std::size_t i = 0; i < name_len; ++i)
        out.name[i] = static_cast<char>(raw[i]);
      return true;
    }
  }

  [[nodiscard]] result<std::uint32_t> lookup(const inode_info &dir, string_view name) const noexcept {
    std::uint64_t pos = 0;
    found f;
    for (;;) {
      auto more = next_dirent(dir, pos, f);
      if (!more)
        return unexpected(more.error());
      if (!*more)
        return unexpected(error::not_found);
      if (f.name_view() == name)
        return f.ino;
    }
  }

  [[nodiscard]] result<std::size_t> read_symlink(const inode_info &in, array<char, symlink_buffer> &out) const noexcept {
    if (in.size == 0 || in.size >= symlink_buffer)
      return unexpected(error::invalid_argument);
    const auto n = static_cast<std::size_t>(in.size);
    // Fast symlink: the target lives in i_block and no data blocks are allocated.
    const std::uint32_t ea_sectors = in.file_acl != 0 ? bs_ / 512 : 0;
    if (in.blocks_512 == ea_sectors && n <= in.block.size()) {
      for (std::size_t i = 0; i < n; ++i)
        out[i] = static_cast<char>(in.block[i]);
      return n;
    }
    array<std::byte, symlink_buffer> raw{};
    const span<std::byte> raw_span(raw);
    auto r = inode_read(in, 0, raw_span.first(n));
    if (!r)
      return unexpected(r.error());
    if (*r != n)
      return unexpected(error::io_error);
    for (std::size_t i = 0; i < n; ++i)
      out[i] = static_cast<char>(raw[i]);
    return n;
  }

  // Resolves `path` to an inode number, following symlinks (also the last component). Absolute paths start at the
  // root, others at `start_dir`.
  [[nodiscard]] result<std::uint32_t> resolve(string_view path, std::uint32_t start_dir, unsigned depth) const noexcept {
    std::uint32_t cur = (!path.empty() && path[0] == '/') ? root_ino : start_dir;
    std::size_t pos = 0;
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
      if (comp == ".")
        continue;
      auto dir = load_inode(cur);
      if (!dir)
        return unexpected(dir.error());
      if (!dir->is_dir())
        return unexpected(error::invalid_argument);
      auto ino = lookup(*dir, comp);
      if (!ino)
        return unexpected(ino.error());
      auto child = load_inode(*ino);
      if (!child)
        return unexpected(child.error());
      if (child->is_symlink()) {
        if (depth >= max_symlink_depth)
          return unexpected(error::invalid_argument);
        array<char, symlink_buffer> target{};
        auto len = read_symlink(*child, target);
        if (!len)
          return unexpected(len.error());
        auto resolved = resolve(string_view(target.data(), *len), cur, depth + 1);
        if (!resolved)
          return unexpected(resolved.error());
        cur = *resolved;
      } else {
        cur = *ino;
      }
    }
    return cur;
  }

  [[nodiscard]] result<fs_handle> alloc_slot(const inode_info &in) noexcept {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      if (slots_[i].used)
        continue;
      slots_[i].used = true;
      slots_[i].dir = in.is_dir();
      slots_[i].inode = in;
      slots_[i].dir_pos = 0;
      return static_cast<fs_handle>(i + 1);
    }
    return unexpected(error::busy);
  }

  [[nodiscard]] result<std::size_t> index_of(fs_handle h, bool want_dir) const noexcept {
    if (h == 0 || h > slots_.size())
      return unexpected(error::invalid_argument);
    const auto i = static_cast<std::size_t>(h - 1);
    if (!slots_[i].used || slots_[i].dir != want_dir)
      return unexpected(error::invalid_argument);
    return i;
  }

  [[nodiscard]] result<inode_info> resolve_inode(string_view path) const noexcept {
    auto ino = resolve(path, root_ino, 0);
    if (!ino)
      return unexpected(ino.error());
    return load_inode(*ino);
  }

  hw::block_device_ref dev_;
  span<std::byte> scratch_;
  std::uint32_t bs_ = 0;
  std::uint32_t inode_size_ = 0;
  std::uint32_t incompat_ = 0;
  std::uint32_t inodes_count_ = 0;
  std::uint32_t first_data_block_ = 0;
  std::uint32_t blocks_per_group_ = 0;
  std::uint32_t inodes_per_group_ = 0;
  std::uint32_t desc_size_ = 0;
  std::uint32_t group_count_ = 0;
  std::uint64_t blocks_count_ = 0;
  array<slot, max_open_objects> slots_{};
};

template <> struct filesystem_traits<ext4_filesystem> {
  static result<fs_handle> try_open(ext4_filesystem &f, string_view path, open_flags flags) noexcept {
    if (flags != open_flags::read)
      return unexpected(error::permission_denied);
    auto in = f.resolve_inode(path);
    if (!in)
      return unexpected(in.error());
    if (in->is_dir())
      return unexpected(error::invalid_argument);
    return f.alloc_slot(*in);
  }
  static result<fs_handle> try_open_dir(ext4_filesystem &f, string_view path) noexcept {
    auto in = f.resolve_inode(path);
    if (!in)
      return unexpected(in.error());
    if (!in->is_dir())
      return unexpected(error::invalid_argument);
    return f.alloc_slot(*in);
  }
  static result<void> try_close(ext4_filesystem &f, fs_handle h) noexcept {
    if (h == 0 || h > f.slots_.size() || !f.slots_[static_cast<std::size_t>(h - 1)].used)
      return unexpected(error::invalid_argument);
    f.slots_[static_cast<std::size_t>(h - 1)].used = false;
    return {};
  }
  static result<std::size_t> try_read(ext4_filesystem &f, fs_handle h, std::uint64_t offset,
                                      span<std::byte> dst) noexcept {
    auto si = f.index_of(h, false);
    if (!si)
      return unexpected(si.error());
    return f.inode_read(f.slots_[*si].inode, offset, dst);
  }
  static result<bool> try_read_dir(ext4_filesystem &f, fs_handle h, dir_entry &out) noexcept {
    auto si = f.index_of(h, true);
    if (!si)
      return unexpected(si.error());
    auto &s = f.slots_[*si];
    ext4_filesystem::found e;
    auto more = f.next_dirent(s.inode, s.dir_pos, e);
    if (!more)
      return unexpected(more.error());
    if (!*more)
      return false;
    auto child = f.load_inode(e.ino);
    if (!child)
      return unexpected(child.error());
    out = dir_entry{};
    out.set_name(e.name_view());
    out.type = child->type();
    out.size = child->is_reg() ? child->size : 0;
    return true;
  }
  static result<file_info> try_stat(ext4_filesystem &f, string_view path) noexcept {
    auto in = f.resolve_inode(path);
    if (!in)
      return unexpected(in.error());
    return file_info{in->type(), in->size};
  }
  static result<file_info> try_fstat(ext4_filesystem &f, fs_handle h) noexcept {
    if (h == 0 || h > f.slots_.size() || !f.slots_[static_cast<std::size_t>(h - 1)].used)
      return unexpected(error::invalid_argument);
    const auto &in = f.slots_[static_cast<std::size_t>(h - 1)].inode;
    return file_info{in.type(), in.size};
  }
};

} // namespace structo::fs
