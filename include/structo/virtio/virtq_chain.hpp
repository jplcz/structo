// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file virtq_chain.hpp
 * @brief A validated, private snapshot of one descriptor chain popped by a
 * device (`avail_chain<BufSpace>`) and helpers to move bytes through its
 * scatter-gather segments.
 *
 * Segments are plain `(address, length)` pairs tagged with the *buffer*
 * address space; no pointer into peer memory is ever formed. Bytes move only
 * through `virtq_memory_traits<BufMem, BufSpace>`, one fallible copy at a time.
 */

#include "virtq_memory.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <reloco/error.hpp>
#include <reloco/span.hpp>
#include <structo/phys_addr.hpp>

namespace structo::virtio {

/** @brief One buffer segment of a descriptor chain. */
template <typename BufSpace> struct chain_segment {
  phys_addr<void, BufSpace> addr;
  std::uint32_t len = 0;
};

/**
 * @brief A device-side view of one available chain.
 *
 * `readable` (device-readable, "out") segments always precede `writable`
 * ("in") ones; both view the caller-provided storage passed to `try_pop` and
 * are valid only while it is.
 */
template <typename BufSpace> struct avail_chain {
  std::uint16_t head = 0;       ///< Head descriptor id (split) / buffer id (packed), echoed back in the used ring.
  std::uint16_t desc_count = 0; ///< Ring descriptors the chain occupies (packed rings advance by this on completion).
  reloco::span<const chain_segment<BufSpace>> readable;
  reloco::span<const chain_segment<BufSpace>> writable;
  std::uint64_t readable_bytes = 0;
  std::uint64_t writable_bytes = 0;
};

/** @brief One reaped completion. */
struct virtq_completion {
  std::uintptr_t token = 0;
  std::uint32_t len = 0; ///< Bytes the device reports having written.
};

/**
 * @brief Copies `dst.size()` bytes out of @p segments starting at byte @p offset
 * into @p dst. `error::out_of_range` if the segments are too short.
 */
template <typename BufMem, typename BufSpace>
[[nodiscard]] reloco::result<void> try_read_chain(BufMem &mem, reloco::span<const chain_segment<BufSpace>> segments,
                                                  std::uint64_t offset, reloco::span<std::byte> dst) noexcept {
  std::size_t done = 0;
  for (const auto &seg : segments) {
    if (done == dst.size())
      break;
    if (offset >= seg.len) {
      offset -= seg.len;
      continue;
    }
    const std::size_t n = std::min<std::size_t>(seg.len - static_cast<std::size_t>(offset), dst.size() - done);
    auto at = seg.addr.try_add(offset);
    if (!at)
      return reloco::unexpected(at.error());
    auto r = virtq_memory_traits<BufMem, BufSpace>::try_read(mem, *at, dst.subspan(done, n));
    if (!r)
      return r;
    done += n;
    offset = 0;
  }
  if (done != dst.size())
    return reloco::unexpected(reloco::error::out_of_range);
  return {};
}

/**
 * @brief Copies @p src into @p segments starting at byte @p offset.
 * `error::out_of_range` if the segments are too short.
 */
template <typename BufMem, typename BufSpace>
[[nodiscard]] reloco::result<void> try_write_chain(BufMem &mem, reloco::span<const chain_segment<BufSpace>> segments,
                                                   std::uint64_t offset, reloco::span<const std::byte> src) noexcept {
  std::size_t done = 0;
  for (const auto &seg : segments) {
    if (done == src.size())
      break;
    if (offset >= seg.len) {
      offset -= seg.len;
      continue;
    }
    const std::size_t n = std::min<std::size_t>(seg.len - static_cast<std::size_t>(offset), src.size() - done);
    auto at = seg.addr.try_add(offset);
    if (!at)
      return reloco::unexpected(at.error());
    auto r = virtq_memory_traits<BufMem, BufSpace>::try_write(mem, *at, src.subspan(done, n));
    if (!r)
      return r;
    done += n;
    offset = 0;
  }
  if (done != src.size())
    return reloco::unexpected(reloco::error::out_of_range);
  return {};
}

} // namespace structo::virtio
