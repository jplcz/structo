// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file virtio_console.hpp
 * @brief VIRTIO console device function (device ID 3), single port, for `virtio_mmio_device`.
 *
 * A byte stream in each direction, no multiport. Two queues:
 *  - queue 0 (receiveq): the guest posts empty buffers; the device fills them with
 *    input staged by `try_input`;
 *  - queue 1 (transmitq): the guest posts filled buffers; the device hands the bytes
 *    to the `Sink` (guest output).
 *
 * Input never touches guest memory when staged: bytes go into a bounded FIFO and are
 * delivered when queue 0 is serviced (`try_kick(0)`, or the guest posting buffers).
 * `try_input` takes what fits and reports how many bytes it accepted; the caller keeps
 * the rest (natural backpressure for a serial line or a host terminal).
 *
 * Optional feature `SIZE`: when constructed with non-zero columns and rows the device
 * offers `VIRTIO_CONSOLE_F_SIZE` and exposes them in config space; `set_size` followed
 * by `virtio_mmio_device::notify_config_changed()` informs a running guest. The
 * emergency-write feature is not offered (the transport's config space is read-only).
 *
 * ### Sink requirements
 * @code
 * struct my_sink {
 *   // Consume all of @p bytes (guest output). On failure the chunk is dropped and counted.
 *   reloco::result<void> try_write(reloco::span<const std::byte> bytes) noexcept;
 * };
 * @endcode
 */

#include "le_bytes.hpp"
#include "virtq_chain.hpp"
#include "virtq_types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <reloco/array.hpp>
#include <reloco/error.hpp>
#include <reloco/span.hpp>

namespace structo::virtio {

namespace console {
inline constexpr std::uint32_t device_id = 3;
inline constexpr unsigned feature_size = 0;
inline constexpr std::size_t config_size = 12; ///< cols(2) rows(2) max_nr_ports(4) emerg_wr(4)

/** @brief Diagnostic counters. */
struct stats {
  std::uint64_t output_bytes = 0;   ///< Bytes the guest wrote that the sink consumed.
  std::uint64_t output_dropped = 0; ///< Bytes the sink refused.
  std::uint64_t input_bytes = 0;    ///< Bytes delivered into guest buffers.
};
} // namespace console

/**
 * @brief Single-port console function over a `Sink`.
 * @tparam GuestSpace Address space of the guest's ring/buffer addresses (must match the transport's).
 * @tparam Sink Guest-output consumer (see the file comment).
 * @tparam InputCapacity Size in bytes of the staged-input FIFO.
 */
template <typename GuestSpace, typename Sink, std::size_t InputCapacity = 4096> class virtio_console_function {
public:
  static constexpr std::uint32_t device_id = console::device_id;
  static constexpr std::uint32_t queue_count = 2;
  static constexpr std::uint32_t queue_max_size = 128;

  static_assert(InputCapacity >= 1, "need at least one byte of input staging");

  /** @param columns,rows Terminal size to advertise; both non-zero to offer `SIZE`. */
  explicit virtio_console_function(Sink &sink, std::uint16_t columns = 0, std::uint16_t rows = 0) noexcept
      : sink_(&sink), cols_(columns), rows_(rows) {}

  [[nodiscard]] std::uint64_t device_features() const noexcept {
    return has_size() ? std::uint64_t{1} << console::feature_size : 0;
  }

  [[nodiscard]] std::size_t config_size() const noexcept { return console::config_size; }

  /** @brief Config space: cols, rows (zero without `SIZE`), max_nr_ports = 1, emerg_wr = 0. */
  [[nodiscard]] reloco::result<void> try_read_config(std::uint64_t offset, reloco::span<std::byte> dst) noexcept {
    if (offset > console::config_size || dst.size() > console::config_size - offset)
      return reloco::unexpected(reloco::error::out_of_range);
    reloco::array<std::byte, console::config_size> raw{};
    auto rs = raw.as_span();
    store_le<std::uint16_t>(rs, cols_);
    store_le<std::uint16_t>(rs.subspan(2), rows_);
    store_le<std::uint32_t>(rs.subspan(4), 1);
    for (std::size_t i = 0; i < dst.size(); ++i)
      dst[i] = raw[static_cast<std::size_t>(offset) + i];
    return {};
  }

  /** @brief Changes the advertised size (take effect for the guest after `notify_config_changed()`). */
  void set_size(std::uint16_t columns, std::uint16_t rows) noexcept {
    cols_ = columns;
    rows_ = rows;
  }

  /**
   * @brief Stages input for the guest.
   * @return How many leading bytes of @p bytes were accepted (the FIFO may be full); the caller
   * retries the rest later.
   */
  [[nodiscard]] std::size_t try_input(reloco::span<const std::byte> bytes) noexcept {
    const std::size_t n = std::min(bytes.size(), InputCapacity - count_);
    for (std::size_t i = 0; i < n; ++i)
      fifo_[(head_ + count_ + i) % InputCapacity] = bytes[i];
    count_ += n;
    return n;
  }

  /** @brief Staged input bytes not yet delivered; non-zero after `try_input` means `try_kick(0)` has work. */
  [[nodiscard]] std::size_t pending_input() const noexcept { return count_; }

  [[nodiscard]] const console::stats &stats() const noexcept { return stats_; }

  /** @brief Services queue @p qidx (0 = deliver staged input, 1 = consume guest output). */
  template <typename QueueView, typename Mem>
  [[nodiscard]] reloco::result<void> process(Mem &mem, std::uint32_t qidx, QueueView &q) noexcept {
    return qidx == 0 ? deliver_input(mem, q) : consume_output(mem, q);
  }

private:
  [[nodiscard]] bool has_size() const noexcept { return cols_ != 0 && rows_ != 0; }

  template <typename Mem, typename QueueView> reloco::result<void> deliver_input(Mem &mem, QueueView &q) noexcept {
    while (count_ != 0) {
      auto popped = q.try_pop(reloco::span<typename QueueView::segment>(segs_.data(), segs_.size()));
      if (!popped)
        return reloco::unexpected(popped.error());
      if (!popped->has_value())
        return {}; // no buffers posted: keep the input staged
      const auto &c = **popped;
      const std::uint64_t want = std::min<std::uint64_t>(c.writable_bytes, count_);
      std::uint64_t done = 0;
      while (done < want) {
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(want - done, buf_.size()));
        for (std::size_t i = 0; i < n; ++i)
          buf_[i] = fifo_[(head_ + i) % InputCapacity];
        if (!try_write_chain(mem, c.writable, done, reloco::span<const std::byte>(buf_.data(), n)))
          break; // unwritable guest buffer: keep what was not delivered
        head_ = (head_ + n) % InputCapacity;
        count_ -= n;
        done += n;
      }
      stats_.input_bytes += done;
      if (auto r = q.try_push_used(c, static_cast<std::uint32_t>(done)); !r)
        return r;
    }
    return {};
  }

  template <typename Mem, typename QueueView> reloco::result<void> consume_output(Mem &mem, QueueView &q) noexcept {
    for (;;) {
      auto popped = q.try_pop(reloco::span<typename QueueView::segment>(segs_.data(), segs_.size()));
      if (!popped)
        return reloco::unexpected(popped.error());
      if (!popped->has_value())
        return {};
      const auto &c = **popped;
      std::uint64_t done = 0;
      while (done < c.readable_bytes) {
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(c.readable_bytes - done, buf_.size()));
        if (!try_read_chain(mem, c.readable, done, reloco::span<std::byte>(buf_.data(), n)))
          break;
        if (sink_->try_write(reloco::span<const std::byte>(buf_.data(), n)))
          stats_.output_bytes += n;
        else
          stats_.output_dropped += n;
        done += n;
      }
      if (auto r = q.try_push_used(c, 0); !r)
        return r;
    }
  }

  Sink *sink_;
  std::uint16_t cols_;
  std::uint16_t rows_;
  reloco::array<std::byte, InputCapacity> fifo_{};
  std::size_t head_ = 0;
  std::size_t count_ = 0;
  console::stats stats_{};
  reloco::array<chain_segment<GuestSpace>, queue_max_size> segs_{};
  reloco::array<std::byte, 256> buf_{};
};

} // namespace structo::virtio
