// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file virtio_blk.hpp
 * @brief VIRTIO block device function (device ID 2) for `virtio_mmio_device`.
 *
 * Each request is a descriptor chain: a 16-byte readable header
 * `{le32 type, le32 reserved, le64 sector}`, data buffers (writable for
 * reads, readable for writes), and a final writable status byte. The header is
 * copied out once (no TOCTOU), the sector range is validated against the
 * capacity with overflow-safe arithmetic, and a malformed chain is completed
 * with `IOERR`/`UNSUPP` rather than trusted.
 *
 * Supported: `IN`, `OUT`, `FLUSH`, `GET_ID`; features `FLUSH`, `SEG_MAX`,
 * `BLK_SIZE` and (read-only stores) `RO`. Config space: capacity, size_max,
 * seg_max, geometry (zero), blk_size.
 *
 * ### Store requirements
 * @code
 * struct ram_store {
 *   // Capacity in 512-byte sectors.
 *   std::uint64_t capacity_sectors() const noexcept;
 *   // True if writes must be refused (offers VIRTIO_BLK_F_RO).
 *   bool read_only() const noexcept;
 *   // Read/write @c buf.size() bytes (a non-zero multiple of 512) at @p sector.
 *   // The range is already validated against capacity_sectors().
 *   reloco::result<void> try_read(std::uint64_t sector, reloco::span<std::byte> buf) noexcept;
 *   reloco::result<void> try_write(std::uint64_t sector, reloco::span<const std::byte> buf) noexcept;
 *   reloco::result<void> try_flush() noexcept;
 * };
 * @endcode
 */

#include "le_bytes.hpp"
#include "virtq_chain.hpp"
#include "virtq_types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <reloco/array.hpp>
#include <reloco/error.hpp>
#include <reloco/span.hpp>

namespace structo::virtio {

namespace blk {
inline constexpr std::uint32_t device_id = 2;
inline constexpr unsigned feature_ro = 5;
inline constexpr unsigned feature_flush = 9;
inline constexpr unsigned feature_seg_max = 2;
inline constexpr unsigned feature_blk_size = 6;
inline constexpr std::uint32_t req_in = 0;
inline constexpr std::uint32_t req_out = 1;
inline constexpr std::uint32_t req_flush = 4;
inline constexpr std::uint32_t req_get_id = 8;
inline constexpr std::size_t id_size = 20;
inline constexpr std::size_t config_size = 24;
inline constexpr std::uint8_t status_ok = 0;
inline constexpr std::uint8_t status_ioerr = 1;
inline constexpr std::uint8_t status_unsupp = 2;
inline constexpr std::size_t header_size = 16;
inline constexpr std::uint32_t sector_size = 512;
} // namespace blk

/**
 * @brief Block device function over a `Store`.
 * @tparam GuestSpace Address space of the guest's ring/buffer addresses (must match the transport's).
 * @tparam Store Backing store (see the file comment).
 */
template <typename GuestSpace, typename Store> class virtio_blk_function {
public:
  static constexpr std::uint32_t device_id = blk::device_id;
  static constexpr std::uint32_t queue_count = 1;
  static constexpr std::uint32_t queue_max_size = 128;

  explicit virtio_blk_function(Store &store) noexcept : store_(&store) {}

  [[nodiscard]] std::uint64_t device_features() const noexcept {
    std::uint64_t f = (std::uint64_t{1} << blk::feature_flush) | (std::uint64_t{1} << blk::feature_seg_max) |
                      (std::uint64_t{1} << blk::feature_blk_size);
    if (store_->read_only())
      f |= std::uint64_t{1} << blk::feature_ro;
    return f;
  }

  [[nodiscard]] std::size_t config_size() const noexcept { return blk::config_size; }

  /** @brief Config space: capacity(8), size_max(4), seg_max(4), geometry(4, zero), blk_size(4). */
  [[nodiscard]] reloco::result<void> try_read_config(std::uint64_t offset, reloco::span<std::byte> dst) noexcept {
    if (offset > blk::config_size || dst.size() > blk::config_size - offset)
      return reloco::unexpected(reloco::error::out_of_range);
    reloco::array<std::byte, blk::config_size> raw{};
    auto rs = raw.as_span();
    store_le<std::uint64_t>(rs, store_->capacity_sectors());
    store_le<std::uint32_t>(rs.subspan(12), seg_max());
    store_le<std::uint32_t>(rs.subspan(20), blk::sector_size);
    for (std::size_t i = 0; i < dst.size(); ++i)
      dst[i] = raw[static_cast<std::size_t>(offset) + i];
    return {};
  }

  /** @brief Drains @p q, completing every available request. */
  template <typename QueueView, typename Mem>
  [[nodiscard]] reloco::result<void> process(Mem &mem, std::uint32_t, QueueView &q) noexcept {
    for (;;) {
      auto popped = q.try_pop(reloco::span<typename QueueView::segment>(segs_.data(), segs_.size()));
      if (!popped)
        return reloco::unexpected(popped.error());
      if (!popped->has_value())
        return {};
      const auto &c = **popped;
      const std::uint32_t written = handle(mem, c);
      if (auto r = q.try_push_used(c, written); !r)
        return r;
    }
  }

private:
  template <typename Mem, typename Chain> std::uint32_t handle(Mem &mem, const Chain &c) noexcept {
    if (c.writable_bytes == 0 || c.readable_bytes < blk::header_size)
      return 0; // no room for a status byte: nothing sensible to report

    reloco::array<std::byte, blk::header_size> hdr{};
    if (!try_read_chain(mem, c.readable, 0, reloco::span<std::byte>(hdr.data(), hdr.size())))
      return 0;
    const reloco::span<const std::byte> h(hdr.data(), hdr.size());
    auto type = load_le<std::uint32_t>(h);
    const auto h_sector = h.subspan(8);
    const auto sector = load_le<std::uint64_t>(h_sector);

    std::uint8_t status = blk::status_ok;
    std::uint32_t data_written = 0;
    if (c.readable.size() + c.writable.size() > std::size_t{seg_max()} + 2)
      type = ~std::uint32_t{0}; // more data segments than seg_max: refuse
    switch (type) {
    case blk::req_in:
      status = do_in(mem, c, sector, data_written);
      break;
    case blk::req_out:
      status = do_out(mem, c, sector);
      break;
    case blk::req_flush:
      status = c.readable_bytes == blk::header_size && c.writable_bytes == 1 && store_->try_flush()
                   ? blk::status_ok
                   : blk::status_ioerr;
      break;
    case blk::req_get_id:
      status = do_get_id(mem, c, data_written);
      break;
    default:
      status = blk::status_unsupp;
      break;
    }

    const std::byte sb{status};
    if (!try_write_chain(mem, c.writable, c.writable_bytes - 1, reloco::span<const std::byte>(&sb, 1)))
      return data_written;
    return data_written + 1;
  }

  static constexpr std::uint32_t seg_max() noexcept { return queue_max_size - 2; }

  template <typename Mem, typename Chain>
  std::uint8_t do_get_id(Mem &mem, const Chain &c, std::uint32_t &written) noexcept {
    if (c.readable_bytes != blk::header_size || c.writable_bytes != blk::id_size + 1)
      return blk::status_ioerr;
    reloco::array<std::byte, blk::id_size> id{}; // NUL-padded serial
    const reloco::span<const char> name("structo-virtio-blk", 18);
    for (std::size_t i = 0; i < name.size(); ++i)
      id[i] = static_cast<std::byte>(name[i]);
    if (!try_write_chain(mem, c.writable, 0, reloco::span<const std::byte>(id.data(), id.size())))
      return blk::status_ioerr;
    written = static_cast<std::uint32_t>(blk::id_size);
    return blk::status_ok;
  }

  [[nodiscard]] bool range_ok(std::uint64_t sector, std::uint64_t bytes) const noexcept {
    if (bytes == 0 || bytes % blk::sector_size != 0)
      return false;
    const std::uint64_t n = bytes / blk::sector_size;
    const std::uint64_t cap = store_->capacity_sectors();
    return sector <= cap && n <= cap - sector;
  }

  template <typename Mem, typename Chain>
  std::uint8_t do_in(Mem &mem, const Chain &c, std::uint64_t sector, std::uint32_t &written) noexcept {
    if (c.readable_bytes != blk::header_size)
      return blk::status_ioerr;
    const std::uint64_t bytes = c.writable_bytes - 1;
    if (!range_ok(sector, bytes))
      return blk::status_ioerr;
    std::uint64_t done = 0;
    while (done < bytes) {
      const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(bytes - done, buf_.size()));
      if (!store_->try_read(sector + done / blk::sector_size, reloco::span<std::byte>(buf_.data(), n)))
        return blk::status_ioerr;
      if (!try_write_chain(mem, c.writable, done, reloco::span<const std::byte>(buf_.data(), n)))
        return blk::status_ioerr;
      done += n;
      written = static_cast<std::uint32_t>(done);
    }
    return blk::status_ok;
  }

  template <typename Mem, typename Chain>
  std::uint8_t do_out(Mem &mem, const Chain &c, std::uint64_t sector) noexcept {
    if (store_->read_only() || c.writable_bytes != 1)
      return blk::status_ioerr;
    const std::uint64_t bytes = c.readable_bytes - blk::header_size;
    if (!range_ok(sector, bytes))
      return blk::status_ioerr;
    std::uint64_t done = 0;
    while (done < bytes) {
      const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(bytes - done, buf_.size()));
      if (!try_read_chain(mem, c.readable, blk::header_size + done, reloco::span<std::byte>(buf_.data(), n)))
        return blk::status_ioerr;
      if (!store_->try_write(sector + done / blk::sector_size, reloco::span<const std::byte>(buf_.data(), n)))
        return blk::status_ioerr;
      done += n;
    }
    return blk::status_ok;
  }

  Store *store_;
  reloco::array<chain_segment<GuestSpace>, queue_max_size> segs_{};
  reloco::array<std::byte, 4096> buf_{};
};

} // namespace structo::virtio
