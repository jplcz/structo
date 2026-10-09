// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file virtq_layout.hpp
 * @brief Ring sizing/alignment arithmetic for split and packed virtqueues
 * and the address-space-tagged triple of ring-area base addresses
 * (`split_ring_addrs<RingSpace>` / `packed_ring_addrs<RingSpace>`).
 *
 * Sizes follow VIRTIO 1.x §2.7 (split) and §2.8 (packed):
 *
 * | Area | Size (bytes) | Alignment |
 * |---|---|---|
 * | split descriptor table | `16 * N` | 16 |
 * | split available ring | `4 + 2N` (+2 `used_event` with EVENT_IDX) | 2 |
 * | split used ring | `4 + 8N` (+2 `avail_event` with EVENT_IDX) | 4 |
 * | packed descriptor ring | `16 * N` | 16 |
 * | packed driver / device event suppression | 4 each | 4 |
 *
 * `N` (the queue size) must be a power of two in `[1, 32768]` for split
 * rings and any value in `[1, 32768]` for packed rings.
 *
 * The three areas of a ring are *independent* base addresses -- a driver
 * hands each to the device separately (`QueueDescLow/High`, ...) -- so
 * `*_ring_addrs` carries three `phys_addr<void, RingSpace>` rather than
 * one base plus offsets. `try_from_contiguous` produces the conventional
 * single-allocation placement when the caller wants one.
 *
 * All addresses are `phys_addr<void, RingSpace>`: the tag is the address
 * space the *accessing side* uses to reach the ring (a driver's CPU view,
 * a VMM's guest-physical view, ...), so a bus address cannot be passed
 * where a CPU-physical one is expected.
 */

#include "virtq_types.hpp"

#include <cstddef>
#include <cstdint>
#include <reloco/detail/compat.hpp>
#include <reloco/error.hpp>
#include <reloco/speculation_defense.hpp>
#include <structo/phys_addr.hpp>

namespace structo::virtio {

inline constexpr std::size_t split_desc_align = 16;
inline constexpr std::size_t split_avail_align = 2;
inline constexpr std::size_t split_used_align = 4;
inline constexpr std::size_t packed_desc_align = 16;
inline constexpr std::size_t packed_event_align = 4;
inline constexpr std::size_t packed_event_size = sizeof(virtq_packed_event);

/** @brief Whether @p queue_size is valid for a split ring (power of two in `[1, 32768]`). */
[[nodiscard]] constexpr bool is_valid_split_queue_size(std::uint32_t queue_size) noexcept {
  return queue_size >= 1 && queue_size <= max_queue_size && (queue_size & (queue_size - 1u)) == 0;
}

/** @brief Whether @p queue_size is valid for a packed ring (any value in `[1, 32768]`). */
[[nodiscard]] constexpr bool is_valid_packed_queue_size(std::uint32_t queue_size) noexcept {
  return queue_size >= 1 && queue_size <= max_queue_size;
}

/** @brief Size of a split ring's descriptor table. @pre valid queue size. */
[[nodiscard]] constexpr std::size_t split_desc_bytes(std::uint32_t queue_size) noexcept {
  return desc_size * queue_size;
}

/** @brief Size of a split ring's available ring. @pre valid queue size. */
[[nodiscard]] constexpr std::size_t split_avail_bytes(std::uint32_t queue_size, bool event_idx) noexcept {
  return sizeof(virtq_avail_header) + sizeof(std::uint16_t) * queue_size + (event_idx ? sizeof(std::uint16_t) : 0);
}

/** @brief Size of a split ring's used ring. @pre valid queue size. */
[[nodiscard]] constexpr std::size_t split_used_bytes(std::uint32_t queue_size, bool event_idx) noexcept {
  return sizeof(virtq_used_header) + sizeof(virtq_used_elem) * queue_size + (event_idx ? sizeof(std::uint16_t) : 0);
}

/** @brief Size of a packed ring's descriptor ring. @pre valid queue size. */
[[nodiscard]] constexpr std::size_t packed_desc_bytes(std::uint32_t queue_size) noexcept {
  return desc_size * queue_size;
}

/** @brief Offsets/sizes of the three areas of one split ring. */
struct split_layout {
  std::uint32_t queue_size = 0;
  bool event_idx = false;

  std::size_t desc_size = 0;
  std::size_t avail_size = 0;
  std::size_t used_size = 0;

  /** Conventional single-allocation placement: desc, then avail, then used (each aligned). */
  std::size_t desc_offset = 0;
  std::size_t avail_offset = 0;
  std::size_t used_offset = 0;
  std::size_t total_size = 0;
};

/** @brief Offsets/sizes of the three areas of one packed ring. */
struct packed_layout {
  std::uint32_t queue_size = 0;

  std::size_t desc_size = 0;
  std::size_t event_size = 0; ///< Size of each of the two event-suppression areas.

  std::size_t desc_offset = 0;
  std::size_t driver_event_offset = 0;
  std::size_t device_event_offset = 0;
  std::size_t total_size = 0;
};

/**
 * @brief Validates a peer-controlled descriptor/ring index against @p limit.
 *
 * The index is masked with `nospec::sanitize` before the failing branch so a
 * mispredicted bounds check cannot be used to speculatively index past the
 * ring. @p index must be a private copy (already read once from peer memory).
 * @return `error::security_violation` if `index >= limit`.
 */
[[nodiscard]] inline RELOCO_CONSTEXPR20 reloco::result<std::uint32_t> try_checked_index(std::uint32_t index,
                                                                                        std::uint32_t limit) noexcept {
  const bool ok = index < limit;
  const std::uint32_t safe = reloco::nospec::sanitize(index, ok, std::uint32_t{0});
  if (!ok)
    return reloco::unexpected(reloco::error::security_violation);
  return safe;
}

namespace detail {
[[nodiscard]] constexpr std::size_t align_up(std::size_t v, std::size_t a) noexcept { return (v + a - 1) & ~(a - 1); }
} // namespace detail

/**
 * @brief Computes the split-ring layout for @p queue_size.
 * @return `error::invalid_argument` if @p queue_size is not a power of two in `[1, 32768]`.
 */
[[nodiscard]] inline RELOCO_CONSTEXPR20 reloco::result<split_layout> try_split_layout(std::uint32_t queue_size,
                                                                                      bool event_idx) noexcept {
  if (!is_valid_split_queue_size(queue_size))
    return reloco::unexpected(reloco::error::invalid_argument);

  split_layout l;
  l.queue_size = queue_size;
  l.event_idx = event_idx;
  l.desc_size = split_desc_bytes(queue_size);
  l.avail_size = split_avail_bytes(queue_size, event_idx);
  l.used_size = split_used_bytes(queue_size, event_idx);
  l.desc_offset = 0;
  l.avail_offset = detail::align_up(l.desc_offset + l.desc_size, split_avail_align);
  l.used_offset = detail::align_up(l.avail_offset + l.avail_size, split_used_align);
  l.total_size = l.used_offset + l.used_size;
  return l;
}

/**
 * @brief Computes the packed-ring layout for @p queue_size.
 * @return `error::invalid_argument` if @p queue_size is not in `[1, 32768]`.
 */
[[nodiscard]] inline RELOCO_CONSTEXPR20 reloco::result<packed_layout>
try_packed_layout(std::uint32_t queue_size) noexcept {
  if (!is_valid_packed_queue_size(queue_size))
    return reloco::unexpected(reloco::error::invalid_argument);

  packed_layout l;
  l.queue_size = queue_size;
  l.desc_size = packed_desc_bytes(queue_size);
  l.event_size = packed_event_size;
  l.desc_offset = 0;
  l.driver_event_offset = detail::align_up(l.desc_offset + l.desc_size, packed_event_align);
  l.device_event_offset = detail::align_up(l.driver_event_offset + l.event_size, packed_event_align);
  l.total_size = l.device_event_offset + l.event_size;
  return l;
}

namespace detail {

/** @brief Validates one ring area: non-null, aligned, and `[addr, addr + size)` does not wrap. */
template <typename RingSpace>
[[nodiscard]] RELOCO_CONSTEXPR20 reloco::result<void> check_area(phys_addr<void, RingSpace> addr, std::size_t size,
                                                                 std::size_t align) noexcept {
  if (addr.is_null())
    return reloco::unexpected(reloco::error::invalid_argument);
  if (addr.value % align != 0)
    return reloco::unexpected(reloco::error::invalid_argument);
  auto end = addr.try_add(static_cast<std::uint64_t>(size));
  if (!end)
    return reloco::unexpected(end.error());
  return {};
}

} // namespace detail

/**
 * @brief Base addresses of a split ring's three areas, as seen by the
 * accessing side through address space @p RingSpace.
 */
template <typename RingSpace> struct split_ring_addrs {
  using space_tag = RingSpace;
  using addr_type = phys_addr<void, RingSpace>;

  addr_type desc;
  addr_type avail;
  addr_type used;

  /**
   * @brief Places all three areas contiguously from @p base following @p layout.
   * @return `error::integer_overflow` if the ring would run past the end of the address space.
   */
  [[nodiscard]] static RELOCO_CONSTEXPR20 reloco::result<split_ring_addrs>
  try_from_contiguous(addr_type base, const split_layout &layout) noexcept {
    auto d = base.try_add(layout.desc_offset);
    auto a = base.try_add(layout.avail_offset);
    auto u = base.try_add(layout.used_offset);
    if (!d || !a || !u)
      return reloco::unexpected(reloco::error::integer_overflow);
    return split_ring_addrs{*d, *a, *u};
  }

  /** @brief Checks null/alignment/wrap of each area against @p layout. */
  [[nodiscard]] RELOCO_CONSTEXPR20 reloco::result<void> try_validate(const split_layout &layout) const noexcept {
    if (auto r = detail::check_area(desc, layout.desc_size, split_desc_align); !r)
      return r;
    if (auto r = detail::check_area(avail, layout.avail_size, split_avail_align); !r)
      return r;
    return detail::check_area(used, layout.used_size, split_used_align);
  }

  /** @brief Address of descriptor-table entry @p index (`error::security_violation` if `index >= queue_size`). */
  [[nodiscard]] RELOCO_CONSTEXPR20 reloco::result<addr_type> desc_at(std::uint32_t index,
                                                                     std::uint32_t queue_size) const noexcept {
    return element_at(desc, index, queue_size, desc_size_v);
  }

  /** @brief Address of `avail.ring[index]` (past the 4-byte header). */
  [[nodiscard]] RELOCO_CONSTEXPR20 reloco::result<addr_type> avail_ring_at(std::uint32_t index,
                                                                           std::uint32_t queue_size) const noexcept {
    auto base = avail.try_add(sizeof(virtq_avail_header));
    if (!base)
      return reloco::unexpected(base.error());
    return element_at(*base, index, queue_size, sizeof(std::uint16_t));
  }

  /** @brief Address of `used.ring[index]` (past the 4-byte header). */
  [[nodiscard]] RELOCO_CONSTEXPR20 reloco::result<addr_type> used_elem_at(std::uint32_t index,
                                                                          std::uint32_t queue_size) const noexcept {
    auto base = used.try_add(sizeof(virtq_used_header));
    if (!base)
      return reloco::unexpected(base.error());
    return element_at(*base, index, queue_size, sizeof(virtq_used_elem));
  }

  /** @brief Address of `avail.idx`. */
  [[nodiscard]] RELOCO_CONSTEXPR20 reloco::result<addr_type> avail_idx_addr() const noexcept {
    return avail.try_add(offsetof(virtq_avail_header, idx));
  }
  /** @brief Address of `avail.flags`. */
  [[nodiscard]] constexpr addr_type avail_flags_addr() const noexcept { return avail; }
  /** @brief Address of `used.idx`. */
  [[nodiscard]] RELOCO_CONSTEXPR20 reloco::result<addr_type> used_idx_addr() const noexcept {
    return used.try_add(offsetof(virtq_used_header, idx));
  }
  /** @brief Address of `used.flags`. */
  [[nodiscard]] constexpr addr_type used_flags_addr() const noexcept { return used; }

  /** @brief Address of `used_event` (trailing field of the avail ring; EVENT_IDX only). */
  [[nodiscard]] RELOCO_CONSTEXPR20 reloco::result<addr_type> used_event_addr(std::uint32_t queue_size) const noexcept {
    return avail.try_add(sizeof(virtq_avail_header) + sizeof(std::uint16_t) * std::uint64_t{queue_size});
  }
  /** @brief Address of `avail_event` (trailing field of the used ring; EVENT_IDX only). */
  [[nodiscard]] RELOCO_CONSTEXPR20 reloco::result<addr_type> avail_event_addr(std::uint32_t queue_size) const noexcept {
    return used.try_add(sizeof(virtq_used_header) + sizeof(virtq_used_elem) * std::uint64_t{queue_size});
  }

private:
  static constexpr std::uint64_t desc_size_v = sizeof(virtq_desc);

  [[nodiscard]] static RELOCO_CONSTEXPR20 reloco::result<addr_type>
  element_at(addr_type base, std::uint32_t index, std::uint32_t queue_size, std::uint64_t stride) noexcept {
    auto safe = try_checked_index(index, queue_size);
    if (!safe)
      return reloco::unexpected(safe.error());
    return base.try_add(std::uint64_t{*safe} * stride);
  }
};

/** @brief Base addresses of a packed ring's three areas, as seen through address space @p RingSpace. */
template <typename RingSpace> struct packed_ring_addrs {
  using space_tag = RingSpace;
  using addr_type = phys_addr<void, RingSpace>;

  addr_type desc;
  addr_type driver_event;
  addr_type device_event;

  [[nodiscard]] static RELOCO_CONSTEXPR20 reloco::result<packed_ring_addrs>
  try_from_contiguous(addr_type base, const packed_layout &layout) noexcept {
    auto d = base.try_add(layout.desc_offset);
    auto de = base.try_add(layout.driver_event_offset);
    auto ve = base.try_add(layout.device_event_offset);
    if (!d || !de || !ve)
      return reloco::unexpected(reloco::error::integer_overflow);
    return packed_ring_addrs{*d, *de, *ve};
  }

  /** @brief Address of packed descriptor @p index (`error::security_violation` if `index >= queue_size`). */
  [[nodiscard]] RELOCO_CONSTEXPR20 reloco::result<addr_type> desc_at(std::uint32_t index,
                                                                     std::uint32_t queue_size) const noexcept {
    auto safe = try_checked_index(index, queue_size);
    if (!safe)
      return reloco::unexpected(safe.error());
    return desc.try_add(std::uint64_t{*safe} * sizeof(virtq_packed_desc));
  }

  [[nodiscard]] RELOCO_CONSTEXPR20 reloco::result<void> try_validate(const packed_layout &layout) const noexcept {
    if (auto r = detail::check_area(desc, layout.desc_size, packed_desc_align); !r)
      return r;
    if (auto r = detail::check_area(driver_event, layout.event_size, packed_event_align); !r)
      return r;
    return detail::check_area(device_event, layout.event_size, packed_event_align);
  }
};

} // namespace structo::virtio
