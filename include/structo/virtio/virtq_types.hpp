// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file virtq_types.hpp
 * @brief VIRTIO 1.x virtqueue wire structures, flag/feature constants, and
 * the pure-arithmetic notification-suppression helper (`need_event`).
 *
 * Everything here is plain fixed-width integers laid out exactly as the
 * VIRTIO 1.x specification (split virtqueue §2.7, packed virtqueue §2.8)
 * requires. This header performs no memory access of its own; see
 * `virtq_memory.hpp` for how ring memory is reached and `virtq_layout.hpp`
 * for ring sizing.
 *
 * ## Little-endian hosts only
 *
 * VIRTIO 1.x rings are little-endian on the wire. Big-endian hosts (and
 * legacy, native-endian pre-1.0 devices) are deliberately unsupported: the
 * structures below are used as-is with no byte swapping, and the build
 * fails on a big-endian target.
 */

#include <cstddef>
#include <cstdint>
#include <type_traits>

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "structo::virtio supports little-endian hosts only (VIRTIO 1.x wire format)");
#endif

namespace structo::virtio {

// ============================================================================
// Descriptor / ring flags
// ============================================================================

/** @brief `virtq_desc::flags` / `virtq_packed_desc::flags` bits common to both ring layouts. */
inline constexpr std::uint16_t desc_f_next = 1u << 0;     ///< Buffer continues via `next` (split) / next slot (packed).
inline constexpr std::uint16_t desc_f_write = 1u << 1;    ///< Buffer is device-writable (otherwise device-readable).
inline constexpr std::uint16_t desc_f_indirect = 1u << 2; ///< Buffer holds a table of indirect descriptors.

/** @brief Packed-ring-only descriptor flag bits (§2.8.1). */
inline constexpr std::uint16_t desc_f_avail = 1u << 7;
inline constexpr std::uint16_t desc_f_used = 1u << 15;

/** @brief Split ring: `virtq_avail::flags` -- driver asks the device not to interrupt. */
inline constexpr std::uint16_t avail_f_no_interrupt = 1u << 0;
/** @brief Split ring: `virtq_used::flags` -- device asks the driver not to notify. */
inline constexpr std::uint16_t used_f_no_notify = 1u << 0;

/** @brief Packed ring: event-suppression `flags` field values (§2.8.10). */
inline constexpr std::uint16_t event_flags_enable = 0x0;  ///< Always send notifications.
inline constexpr std::uint16_t event_flags_disable = 0x1; ///< Never send notifications.
inline constexpr std::uint16_t event_flags_desc = 0x2;    ///< Notify only at the specified descriptor (needs EVENT_IDX).

// ============================================================================
// Feature bits relevant to the ring itself
// ============================================================================

/** @brief Ring-level feature bit numbers (the bit *position*, not a mask). */
inline constexpr unsigned feature_ring_indirect_desc = 28;
inline constexpr unsigned feature_ring_event_idx = 29;
inline constexpr unsigned feature_version_1 = 32;
inline constexpr unsigned feature_access_platform = 33;
inline constexpr unsigned feature_ring_packed = 34;
inline constexpr unsigned feature_in_order = 35;
inline constexpr unsigned feature_order_platform = 36;
inline constexpr unsigned feature_sr_iov = 37;
inline constexpr unsigned feature_notification_data = 38;

/** @brief Tests @p bit (a `feature_*` position) in a 64-bit negotiated feature set. */
[[nodiscard]] constexpr bool has_feature(std::uint64_t features, unsigned bit) noexcept {
  return bit < 64 && ((features >> bit) & 1u) != 0;
}

// ============================================================================
// Limits
// ============================================================================

/** @brief Largest queue size either ring layout allows. */
inline constexpr std::uint32_t max_queue_size = 32768;

/** @brief Size in bytes of one descriptor (split, packed, or indirect-table entry). */
inline constexpr std::size_t desc_size = 16;

// ============================================================================
// Split virtqueue wire structures (§2.7)
// ============================================================================

/** @brief One split-ring descriptor-table entry (also the indirect-table entry format). */
struct virtq_desc {
  std::uint64_t addr;  ///< Buffer address in the *buffer* address space (the queue's `BufSpace`).
  std::uint32_t len;   ///< Buffer length in bytes.
  std::uint16_t flags; ///< `desc_f_*`.
  std::uint16_t next;  ///< Next descriptor index when `desc_f_next` is set.
};

/** @brief Split-ring available-ring header; `ring[queue_size]` of `std::uint16_t` follows (then `used_event` if EVENT_IDX). */
struct virtq_avail_header {
  std::uint16_t flags;
  std::uint16_t idx;
};

/** @brief One used-ring element. */
struct virtq_used_elem {
  std::uint32_t id;  ///< Head descriptor index of the completed chain (widened to 32 bits).
  std::uint32_t len; ///< Total bytes the device wrote into the chain's writable buffers.
};

/** @brief Split-ring used-ring header; `ring[queue_size]` of `virtq_used_elem` follows (then `avail_event` if EVENT_IDX). */
struct virtq_used_header {
  std::uint16_t flags;
  std::uint16_t idx;
};

// ============================================================================
// Packed virtqueue wire structures (§2.8)
// ============================================================================

/** @brief One packed-ring descriptor. */
struct virtq_packed_desc {
  std::uint64_t addr;
  std::uint32_t len;
  std::uint16_t id;    ///< Buffer ID; echoed back by the device in the used descriptor.
  std::uint16_t flags; ///< `desc_f_*`, including `desc_f_avail`/`desc_f_used` wrap-counter bits.
};

/** @brief Packed-ring event-suppression structure (driver area and device area share this shape). */
struct virtq_packed_event {
  std::uint16_t off_wrap; ///< Bits 0..14: descriptor event offset; bit 15: wrap counter.
  std::uint16_t flags;    ///< `event_flags_*`.
};

static_assert(sizeof(virtq_desc) == 16 && alignof(virtq_desc) == 8, "virtq_desc layout");
static_assert(offsetof(virtq_desc, addr) == 0 && offsetof(virtq_desc, len) == 8 && offsetof(virtq_desc, flags) == 12 &&
                  offsetof(virtq_desc, next) == 14,
              "virtq_desc layout");
static_assert(sizeof(virtq_avail_header) == 4 && alignof(virtq_avail_header) == 2, "virtq_avail_header layout");
static_assert(offsetof(virtq_avail_header, idx) == 2, "virtq_avail_header layout");
static_assert(sizeof(virtq_used_elem) == 8 && alignof(virtq_used_elem) == 4, "virtq_used_elem layout");
static_assert(sizeof(virtq_used_header) == 4 && alignof(virtq_used_header) == 2, "virtq_used_header layout");
static_assert(sizeof(virtq_packed_desc) == 16 && alignof(virtq_packed_desc) == 8, "virtq_packed_desc layout");
static_assert(offsetof(virtq_packed_desc, len) == 8 && offsetof(virtq_packed_desc, id) == 12 &&
                  offsetof(virtq_packed_desc, flags) == 14,
              "virtq_packed_desc layout");
static_assert(sizeof(virtq_packed_event) == 4 && alignof(virtq_packed_event) == 2, "virtq_packed_event layout");

static_assert(std::is_trivially_copyable_v<virtq_desc> && std::is_standard_layout_v<virtq_desc>);
static_assert(std::is_trivially_copyable_v<virtq_used_elem> && std::is_standard_layout_v<virtq_used_elem>);
static_assert(std::is_trivially_copyable_v<virtq_packed_desc> && std::is_standard_layout_v<virtq_packed_desc>);
static_assert(std::is_trivially_copyable_v<virtq_packed_event> && std::is_standard_layout_v<virtq_packed_event>);

// ============================================================================
// Notification suppression arithmetic
// ============================================================================

/**
 * @brief EVENT_IDX test (`vring_need_event()` in the spec/Linux): whether
 * moving an index from @p old_idx to @p new_idx crossed @p event_idx, i.e.
 * whether the peer asked to be notified for this update. All arithmetic is
 * modulo 2^16, so index wrap-around is handled.
 */
[[nodiscard]] constexpr bool need_event(std::uint16_t event_idx, std::uint16_t new_idx, std::uint16_t old_idx) noexcept {
  return static_cast<std::uint16_t>(new_idx - event_idx - 1u) < static_cast<std::uint16_t>(new_idx - old_idx);
}

} // namespace structo::virtio
