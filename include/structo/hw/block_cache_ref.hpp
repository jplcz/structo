// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file block_cache_ref.hpp
 * @brief `structo::hw::block_cache_ref`: a small, allocation-free, write-back LRU
 * block cache layered on a `block_device_ref`, using storage supplied by the caller.
 *
 * The caller hands over one byte span; the cache carves a 16-byte header, a
 * 24-byte descriptor per slot and `block_size()` bytes of data per slot out of it
 * (as many whole slots as fit). Nothing is allocated, and the cache never owns the
 * storage: like `block_device_ref` it is a cheap, copyable, non-owning handle, and
 * all copies share the same cached state.
 *
 * Policy: write-back, least-recently-used eviction. Reads and writes are always in
 * whole blocks; a write never reads the old contents first. Requests larger than
 * the cache bypass it (after writing back and dropping any overlapping cached
 * blocks), so a big sequential read does not wipe the working set.
 *
 * The cache has no destructor-time flush: **call `try_flush()` before dropping the
 * storage or the device**, or dirty blocks are lost. A single-threaded design:
 * callers sharing a cache across cores must serialise access.
 *
 * `block_cache_ref` itself satisfies `block_device_traits`, so it can be wrapped in
 * a `block_device_ref` and handed to anything that reads blocks (partition views,
 * filesystem readers).
 *
 * @code
 * // Why: a FAT/ext4 reader re-reads the same FAT sectors and inode-table blocks
 * // constantly; caching them avoids hitting slow SD/eMMC every time.
 *
 * structo::hw::block_device_ref disk(sd_card);                 // the real device (must outlive the cache)
 *
 * // Storage the cache lives in. Sized as needed: with 512-byte blocks this holds
 * // (16384 - 16) / (24 + 512) = 30 cached blocks. Must outlive the cache.
 * reloco::array<std::byte, 16384> storage;
 *
 * auto cache = structo::hw::block_cache_ref::try_create(disk, storage);   // fails if storage fits < 1 slot
 * if (!cache)
 *   return;
 *
 * reloco::array<std::byte, 1024> two_blocks;
 * (void)cache->try_read_blocks(2048, two_blocks);              // lba, destination (multiple of block size)
 * (void)cache->try_write_blocks(2048, two_blocks);             // lba, source; stays dirty in the cache
 * (void)cache->try_flush();                                    // write back every dirty block, then flush the device
 *
 * structo::hw::block_device_ref cached_disk(*cache);           // hand the cache to a filesystem reader
 * @endcode
 */

#include <reloco/detail/compat.hpp>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/span.hpp>
#include <structo/detail/boot_bytes.hpp>
#include <structo/hw/block_device_ref.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace structo::hw {

class RELOCO_POINTER block_cache_ref {
public:
  constexpr block_cache_ref() noexcept = default;

  /** @brief Builds a cache over @p dev using @p storage. Fails with `error::unsupported_operation` if @p dev is
   * unbound or reports a zero block size, and `error::invalid_argument` if @p storage cannot hold one slot.
   * Any previous contents of @p storage are discarded. */
  [[nodiscard]] static result<block_cache_ref> try_create(block_device_ref dev,
                                                          span<std::byte> storage) noexcept {
    const std::size_t bs = dev.block_size();
    if (!dev || bs == 0)
      return unexpected(error::unsupported_operation);
    if (storage.size() < header_size + slot_meta_size + bs)
      return unexpected(error::invalid_argument);
    block_cache_ref c;
    c.dev_ = dev;
    c.storage_ = storage;
    c.block_size_ = bs;
    c.slots_ = (storage.size() - header_size) / (slot_meta_size + bs);
    c.reset_metadata();
    return c;
  }

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return slots_ != 0; }
  [[nodiscard]] std::size_t block_size() const noexcept { return block_size_; }
  [[nodiscard]] std::uint64_t block_count() const noexcept { return slots_ ? dev_.block_count() : 0; }
  /** @brief Number of blocks the cache can hold. */
  [[nodiscard]] std::size_t slot_count() const noexcept { return slots_; }
  [[nodiscard]] bool is_read_only() const noexcept { return slots_ == 0 || dev_.is_read_only(); }

  /** @brief Number of cached blocks not yet written back. */
  [[nodiscard]] std::size_t dirty_count() const noexcept {
    std::size_t n = 0;
    for (std::size_t i = 0; i < slots_; ++i)
      if (slot_flags(i) & flag_dirty)
        ++n;
    return n;
  }

  /** @brief Reads `dst.size() / block_size()` blocks starting at @p lba. */
  [[nodiscard]] result<void> try_read_blocks(std::uint64_t lba, span<std::byte> dst) const noexcept {
    auto n = check_range(lba, dst.size());
    if (!n)
      return unexpected(n.error());
    if (*n > slots_)
      return read_bypass(lba, *n, dst);
    for (std::size_t i = 0; i < *n; ++i) {
      auto slot = acquire(lba + i, true);
      if (!slot)
        return unexpected(slot.error());
      copy(dst.subspan(i * block_size_, block_size_), slot_data(*slot));
    }
    return {};
  }

  /** @brief Writes `src.size() / block_size()` blocks starting at @p lba into the cache (write-back). Fails with
   * `error::permission_denied` if the device is read-only. */
  [[nodiscard]] result<void> try_write_blocks(std::uint64_t lba, span<const std::byte> src) const noexcept {
    auto n = check_range(lba, src.size());
    if (!n)
      return unexpected(n.error());
    if (dev_.is_read_only())
      return unexpected(error::permission_denied);
    if (*n > slots_)
      return write_bypass(lba, *n, src);
    for (std::size_t i = 0; i < *n; ++i) {
      auto slot = acquire(lba + i, false);
      if (!slot)
        return unexpected(slot.error());
      copy(slot_data(*slot), src.subspan(i * block_size_, block_size_));
      set_slot_flags(*slot, flag_valid | flag_dirty);
    }
    return {};
  }

  /** @brief Reads an arbitrary byte range through the cache; no scratch buffer is needed. */
  [[nodiscard]] result<void> try_read_bytes(std::uint64_t byte_offset, span<std::byte> dst) const noexcept {
    if (!slots_)
      return unexpected(error::unsupported_operation);
    std::size_t done = 0;
    while (done < dst.size()) {
      const std::uint64_t off = byte_offset + done;
      const std::size_t in_block = static_cast<std::size_t>(off % block_size_);
      const std::size_t take = std::min(block_size_ - in_block, dst.size() - done);
      if (off / block_size_ >= dev_.block_count())
        return unexpected(error::out_of_range);
      auto slot = acquire(off / block_size_, true);
      if (!slot)
        return unexpected(slot.error());
      copy(dst.subspan(done, take), slot_data(*slot).subspan(in_block, take));
      done += take;
    }
    return {};
  }

  /** @brief Writes back every dirty block, then flushes the device. On failure the blocks not yet written stay
   * dirty. */
  [[nodiscard]] result<void> try_flush() const noexcept {
    if (!slots_)
      return unexpected(error::unsupported_operation);
    for (std::size_t i = 0; i < slots_; ++i) {
      if (auto r = write_back(i); !r)
        return r;
    }
    return dev_.try_flush();
  }

  /** @brief Drops every cached block. Fails with `error::busy` (dropping nothing) if any block is dirty; flush first. */
  [[nodiscard]] result<void> try_invalidate() const noexcept {
    if (!slots_)
      return unexpected(error::unsupported_operation);
    if (dirty_count() != 0)
      return unexpected(error::busy);
    reset_metadata();
    return {};
  }

private:
  static constexpr std::size_t header_size = 16;    // u64 LRU clock + padding
  static constexpr std::size_t slot_meta_size = 24; // u64 lba, u64 stamp, u64 flags
  static constexpr std::uint64_t flag_valid = 1;
  static constexpr std::uint64_t flag_dirty = 2;

  using bytes_t = span<std::byte>;

  [[nodiscard]] std::size_t meta_offset(std::size_t slot) const noexcept {
    return header_size + slot * slot_meta_size;
  }
  [[nodiscard]] bytes_t slot_data(std::size_t slot) const noexcept {
    return storage_.subspan(header_size + slots_ * slot_meta_size + slot * block_size_, block_size_);
  }
  [[nodiscard]] span<const std::byte> cstorage() const noexcept { return span<const std::byte>(storage_); }

  [[nodiscard]] std::uint64_t slot_lba(std::size_t s) const noexcept {
    return boot::detail::read_le_at<std::uint64_t>(cstorage(), meta_offset(s)).value_or(0);
  }
  [[nodiscard]] std::uint64_t slot_stamp(std::size_t s) const noexcept {
    return boot::detail::read_le_at<std::uint64_t>(cstorage(), meta_offset(s) + 8).value_or(0);
  }
  [[nodiscard]] std::uint64_t slot_flags(std::size_t s) const noexcept {
    return boot::detail::read_le_at<std::uint64_t>(cstorage(), meta_offset(s) + 16).value_or(0);
  }
  void set_slot_lba(std::size_t s, std::uint64_t v) const noexcept {
    (void)boot::detail::write_le_at<std::uint64_t>(storage_, meta_offset(s), v);
  }
  void set_slot_flags(std::size_t s, std::uint64_t v) const noexcept {
    (void)boot::detail::write_le_at<std::uint64_t>(storage_, meta_offset(s) + 16, v);
  }
  // Marks @p s most recently used.
  void touch(std::size_t s) const noexcept {
    const std::uint64_t clock = boot::detail::read_le_at<std::uint64_t>(cstorage(), 0).value_or(0) + 1;
    (void)boot::detail::write_le_at<std::uint64_t>(storage_, 0, clock);
    (void)boot::detail::write_le_at<std::uint64_t>(storage_, meta_offset(s) + 8, clock);
  }

  void reset_metadata() const noexcept {
    (void)boot::detail::write_le_at<std::uint64_t>(storage_, 0, 0);
    for (std::size_t i = 0; i < slots_; ++i) {
      set_slot_lba(i, 0);
      set_slot_flags(i, 0);
      (void)boot::detail::write_le_at<std::uint64_t>(storage_, meta_offset(i) + 8, 0);
    }
  }

  static void copy(bytes_t dst, span<const std::byte> src) noexcept {
    // Callers always pass equally sized, in-bounds spans (one block or one in-block range).
    RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
    std::memcpy(dst.data(), src.data(), dst.size());
    RELOCO_END_UNSAFE_BUFFER_USAGE
  }

  // Validates the request and returns its length in blocks.
  [[nodiscard]] result<std::uint64_t> check_range(std::uint64_t lba, std::size_t bytes) const noexcept {
    if (!slots_)
      return unexpected(error::unsupported_operation);
    if (bytes % block_size_ != 0)
      return unexpected(error::invalid_argument);
    const std::uint64_t n = bytes / block_size_;
    const std::uint64_t total = dev_.block_count();
    if (lba > total || n > total - lba)
      return unexpected(error::out_of_range);
    return n;
  }

  [[nodiscard]] result<void> write_back(std::size_t s) const noexcept {
    if (!(slot_flags(s) & flag_dirty))
      return {};
    if (auto r = dev_.try_write_blocks(slot_lba(s), span<const std::byte>(slot_data(s))); !r)
      return r;
    set_slot_flags(s, flag_valid);
    return {};
  }

  [[nodiscard]] bool find(std::uint64_t lba, std::size_t &out) const noexcept {
    for (std::size_t i = 0; i < slots_; ++i) {
      if ((slot_flags(i) & flag_valid) && slot_lba(i) == lba) {
        out = i;
        return true;
      }
    }
    return false;
  }

  // Returns the slot holding @p lba, loading it from the device when @p fill (otherwise the slot's contents are
  // undefined and the caller overwrites the whole block). A dirty victim is written back first; if that fails the
  // cache is left unchanged.
  [[nodiscard]] result<std::size_t> acquire(std::uint64_t lba, bool fill) const noexcept {
    std::size_t s = 0;
    if (find(lba, s)) {
      touch(s);
      return s;
    }
    bool have_free = false;
    for (std::size_t i = 0; i < slots_; ++i) {
      if (!(slot_flags(i) & flag_valid)) {
        s = i;
        have_free = true;
        break;
      }
      if (i == 0 || slot_stamp(i) < slot_stamp(s))
        s = i;
    }
    if (!have_free) {
      if (auto r = write_back(s); !r)
        return unexpected(r.error());
    }
    set_slot_flags(s, 0);
    if (fill) {
      if (auto r = dev_.try_read_blocks(lba, slot_data(s)); !r)
        return unexpected(r.error());
    }
    set_slot_lba(s, lba);
    set_slot_flags(s, flag_valid);
    touch(s);
    return s;
  }

  // Discards (writing dirty ones back first when @p write_dirty) cached blocks overlapping [lba, lba + n).
  [[nodiscard]] result<void> sync_range(std::uint64_t lba, std::uint64_t n, bool write_dirty) const noexcept {
    for (std::size_t i = 0; i < slots_; ++i) {
      if (!(slot_flags(i) & flag_valid))
        continue;
      const std::uint64_t l = slot_lba(i);
      if (l < lba || l - lba >= n)
        continue;
      if (write_dirty) {
        if (auto r = write_back(i); !r)
          return r;
      }
      set_slot_flags(i, 0);
    }
    return {};
  }

  [[nodiscard]] result<void> read_bypass(std::uint64_t lba, std::uint64_t n, bytes_t dst) const noexcept {
    if (auto r = sync_range(lba, n, true); !r)
      return r;
    return dev_.try_read_blocks(lba, dst);
  }

  [[nodiscard]] result<void> write_bypass(std::uint64_t lba, std::uint64_t n, span<const std::byte> src) const noexcept {
    // Cached copies are about to be overwritten on the device, so they are dropped without write-back.
    if (auto r = sync_range(lba, n, false); !r)
      return r;
    return dev_.try_write_blocks(lba, src);
  }

  block_device_ref dev_;
  bytes_t storage_;
  std::size_t block_size_ = 0;
  std::size_t slots_ = 0;
};

/** @brief Lets a `block_cache_ref` be bound to a `block_device_ref`, so filesystems can read through the cache. */
template <> struct block_device_traits<block_cache_ref> {
  static std::size_t block_size(block_cache_ref &c) noexcept { return c.block_size(); }
  static std::uint64_t block_count(block_cache_ref &c) noexcept { return c.block_count(); }
  static reloco::result<void> try_read_blocks(block_cache_ref &c, std::uint64_t lba, reloco::span<std::byte> dst) noexcept {
    return c.try_read_blocks(lba, dst);
  }
  static reloco::result<void> try_write_blocks(block_cache_ref &c, std::uint64_t lba,
                                               reloco::span<const std::byte> src) noexcept {
    return c.try_write_blocks(lba, src);
  }
  static reloco::result<void> try_flush(block_cache_ref &c) noexcept { return c.try_flush(); }
  static bool is_read_only(block_cache_ref &c) noexcept { return c.is_read_only(); }
};

} // namespace structo::hw
