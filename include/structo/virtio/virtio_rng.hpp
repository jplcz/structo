// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file virtio_rng.hpp
 * @brief VIRTIO entropy device function (device ID 4) for `virtio_mmio_device`.
 *
 * One queue. The guest posts device-writable buffers and the device fills each
 * with random bytes from a `Source`. No features and no config space.
 *
 * A request is served with *at most* `max_request_bytes` bytes: the specification
 * lets the device return fewer bytes than the buffer holds, and the guest asks
 * again. This bounds the time spent per request, so a guest posting a huge buffer
 * cannot monopolise the hypervisor. If the source fails part-way, the bytes
 * produced so far are returned (possibly none).
 *
 * ### Source requirements
 * @code
 * struct my_source {
 *   // Fill @p dst entirely with random bytes, or fail.
 *   reloco::result<void> try_fill(reloco::span<std::byte> dst) noexcept;
 * };
 * @endcode
 * `structo::hw::hw_rng_ref` satisfies this as is (its retry-bound argument defaults).
 */

#include "virtq_chain.hpp"
#include "virtq_types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <reloco/array.hpp>
#include <reloco/error.hpp>
#include <reloco/span.hpp>

namespace structo::virtio {

namespace rng {
inline constexpr std::uint32_t device_id = 4;
inline constexpr std::uint32_t max_request_bytes = 4096; ///< Upper bound served per buffer.
} // namespace rng

/**
 * @brief Entropy device function over a `Source`.
 * @tparam GuestSpace Address space of the guest's ring/buffer addresses (must match the transport's).
 * @tparam Source Random byte source (see the file comment).
 */
template <typename GuestSpace, typename Source> class virtio_rng_function {
public:
  static constexpr std::uint32_t device_id = rng::device_id;
  static constexpr std::uint32_t queue_count = 1;
  static constexpr std::uint32_t queue_max_size = 128;

  explicit virtio_rng_function(Source &source) noexcept : source_(&source) {}

  [[nodiscard]] std::uint64_t device_features() const noexcept { return 0; }
  [[nodiscard]] std::size_t config_size() const noexcept { return 0; }

  [[nodiscard]] reloco::result<void> try_read_config(std::uint64_t offset, reloco::span<std::byte> dst) noexcept {
    if (offset != 0 || !dst.empty())
      return reloco::unexpected(reloco::error::out_of_range);
    return {};
  }

  /** @brief Bytes of entropy handed to guests so far. */
  [[nodiscard]] std::uint64_t bytes_served() const noexcept { return served_; }

  /** @brief Drains the queue, filling every posted buffer. */
  template <typename QueueView, typename Mem>
  [[nodiscard]] reloco::result<void> process(Mem &mem, std::uint32_t, QueueView &q) noexcept {
    for (;;) {
      auto popped = q.try_pop(reloco::span<typename QueueView::segment>(segs_.data(), segs_.size()));
      if (!popped)
        return reloco::unexpected(popped.error());
      if (!popped->has_value())
        return {};
      const auto &c = **popped;
      const std::uint32_t written = fill(mem, c);
      served_ += written;
      if (auto r = q.try_push_used(c, written); !r)
        return r;
    }
  }

private:
  template <typename Mem, typename Chain> std::uint32_t fill(Mem &mem, const Chain &c) noexcept {
    const std::uint64_t want = std::min<std::uint64_t>(c.writable_bytes, rng::max_request_bytes);
    std::uint64_t done = 0;
    while (done < want) {
      const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(want - done, buf_.size()));
      const reloco::span<std::byte> chunk(buf_.data(), n);
      if (!source_->try_fill(chunk))
        break;
      if (!try_write_chain(mem, c.writable, done, reloco::span<const std::byte>(buf_.data(), n)))
        break;
      done += n;
    }
    return static_cast<std::uint32_t>(done);
  }

  Source *source_;
  std::uint64_t served_ = 0;
  reloco::array<chain_segment<GuestSpace>, queue_max_size> segs_{};
  reloco::array<std::byte, 256> buf_{};
};

} // namespace structo::virtio
