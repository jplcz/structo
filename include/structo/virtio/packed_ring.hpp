// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file packed_ring.hpp
 * @brief `packed_virtq_driver` / `packed_virtq_device`: the two ends of a
 * VIRTIO 1.1 packed virtqueue (§2.8), allocation-free and fallible.
 *
 * Template parameters are the same tags as in `split_ring.hpp`:
 * `RingSpace` (where the ring areas live for this side), `BufSpace` (what
 * descriptor `addr` fields mean), `Mem` (`virtq_memory_traits<Mem, RingSpace>`)
 * and `Barriers`.
 *
 * ## Differences from the split ring API
 *
 * - There is no avail index to publish: a request becomes visible to the
 *   device the moment its head descriptor's flags are written, which
 *   `try_add` does last (after a write barrier). There is no `try_publish`.
 * - Descriptors are owned alternately by driver and device, distinguished by
 *   the `AVAIL`/`USED` bits against a wrap counter that each side tracks
 *   privately.
 * - The device completes a request by overwriting the descriptor at *its* used
 *   position with `{id, len}` and the used flags; both sides advance by the
 *   number of descriptors in the chain.
 * - Notification suppression uses the two event-suppression areas
 *   (`enable` / `disable` / `desc`); `desc` needs `VIRTIO_F_EVENT_IDX`
 *   (`event_idx = true`), otherwise it is treated as `enable`.
 * - Indirect descriptors are not supported: an `INDIRECT` flag from the peer
 *   is a protocol violation.
 *
 * ## Example
 *
 * @code
 * auto layout = structo::virtio::try_packed_layout(8);
 * auto addrs = packed_ring_addrs<ring_space>::try_from_contiguous(base, *layout);
 * reloco::array<packed_driver_slot, 8> slots;
 * auto drv = packed_virtq_driver<ring_space, ring_space, mem_t>::try_create(mem, *addrs, 8, slots);
 * (void)drv->try_add(out, in, token);            // visible to the device now
 * if (auto n = drv->needs_notify(); n && *n) ring_doorbell();
 *
 * auto dev = packed_virtq_device<ring_space, ring_space, mem_t>::try_create(mem, *addrs, 8);
 * reloco::array<chain_segment<ring_space>, 8> segs;
 * auto chain = dev->try_pop(segs);
 * if (chain && chain->has_value())
 *   (void)dev->try_push_used(**chain, written);
 * @endcode
 *
 * ## Hostile peer
 *
 * Same rules as the split ring: flags are loaded once, descriptors are copied
 * out before use, ids echoed by the device are range- and in-flight-checked
 * (`try_checked_index`), and a violation latches `is_broken()`.
 */

#include "virtq_barrier.hpp"
#include "virtq_chain.hpp"
#include "virtq_layout.hpp"
#include "virtq_memory.hpp"
#include "virtq_types.hpp"

#include <cstddef>
#include <cstdint>
#include <reloco/error.hpp>
#include <reloco/int_ops.hpp>
#include <reloco/optional.hpp>
#include <reloco/span.hpp>
#include <structo/sg_list.hpp>

namespace structo::virtio {

namespace detail {

/** @brief Wrap-counter flag bits of an *available* descriptor for wrap counter @p w. */
[[nodiscard]] constexpr std::uint16_t packed_avail_flags(bool w) noexcept { return w ? desc_f_avail : desc_f_used; }
/** @brief Wrap-counter flag bits of a *used* descriptor for wrap counter @p w. */
[[nodiscard]] constexpr std::uint16_t packed_used_flags(bool w) noexcept {
  return w ? static_cast<std::uint16_t>(desc_f_avail | desc_f_used) : std::uint16_t{0};
}
[[nodiscard]] constexpr bool packed_is_avail(std::uint16_t flags, bool w) noexcept {
  return ((flags & desc_f_avail) != 0) == w && ((flags & desc_f_used) != 0) != w;
}
[[nodiscard]] constexpr bool packed_is_used(std::uint16_t flags, bool w) noexcept {
  return ((flags & desc_f_avail) != 0) == w && ((flags & desc_f_used) != 0) == w;
}

/** @brief Advances ring position/wrap counter by @p n (`n <= queue_size`). */
constexpr void packed_advance(std::uint32_t &pos, bool &wrap, std::uint32_t n, std::uint32_t queue_size) noexcept {
  pos += n;
  if (pos >= queue_size) {
    pos -= queue_size;
    wrap = !wrap;
  }
}

/** @brief `off_wrap` value for the ring position @p pos + @p after (clamped below the ring size). */
[[nodiscard]] constexpr std::uint16_t packed_event_after(std::uint32_t pos, bool wrap, std::uint32_t after,
                                                         std::uint32_t queue_size) noexcept {
  if (after >= queue_size)
    after = queue_size - 1;
  packed_advance(pos, wrap, after, queue_size);
  return static_cast<std::uint16_t>(pos | (wrap ? 0x8000u : 0u));
}

/**
 * @brief EVENT_IDX decision for a packed ring (shared by both sides).
 * @param off_wrap  Peer-supplied event position + wrap counter (arithmetic only).
 * @param pos,wrap  This side's current position and wrap counter.
 * @param added     Descriptors consumed/produced since the previous decision.
 */
[[nodiscard]] constexpr bool packed_need_event(std::uint16_t off_wrap, std::uint32_t pos, bool wrap,
                                               std::uint16_t added, std::uint32_t queue_size) noexcept {
  const bool ev_wrap = (off_wrap & 0x8000u) != 0;
  std::uint16_t ev = static_cast<std::uint16_t>(off_wrap & 0x7FFFu);
  if (ev_wrap != wrap)
    ev = static_cast<std::uint16_t>(ev - queue_size);
  const std::uint16_t new_idx = static_cast<std::uint16_t>(pos);
  const std::uint16_t old_idx = static_cast<std::uint16_t>(new_idx - added);
  return need_event(ev, new_idx, old_idx);
}

} // namespace detail

/** @brief Driver-private per-buffer-id bookkeeping (never visible to the device). */
struct packed_driver_slot {
  std::uintptr_t token = 0;
  std::uint64_t in_bytes = 0; ///< Total device-writable bytes of the request.
  std::uint16_t next = 0;     ///< Free-id list link.
  std::uint16_t num = 0;      ///< Ring descriptors the request occupies.
  bool in_flight = false;
};

// ============================================================================
// Driver
// ============================================================================

template <typename RingSpace, typename BufSpace, typename Mem, typename Barriers = smp_virtq_barriers>
class packed_virtq_driver {
public:
  using ring_addrs = packed_ring_addrs<RingSpace>;
  using sg_type = sg_entry<BufSpace, std::uint64_t>;
  using completion = virtq_completion;

  /**
   * @brief Binds a driver to a (zeroed) packed ring.
   * @param slots Caller-owned bookkeeping, at least @p queue_size entries.
   * @param event_idx `VIRTIO_F_EVENT_IDX` was negotiated.
   */
  [[nodiscard]] static reloco::result<packed_virtq_driver> try_create(Mem &mem, const ring_addrs &addrs,
                                                                      std::uint32_t queue_size,
                                                                      reloco::span<packed_driver_slot> slots,
                                                                      bool event_idx = false) noexcept {
    auto layout = try_packed_layout(queue_size);
    if (!layout)
      return reloco::unexpected(layout.error());
    if (auto v = addrs.try_validate(*layout); !v)
      return reloco::unexpected(v.error());
    if (slots.size() < queue_size)
      return reloco::unexpected(reloco::error::invalid_argument);

    for (std::uint32_t i = 0; i < queue_size; ++i) {
      slots[i] = packed_driver_slot{};
      slots[i].next = static_cast<std::uint16_t>(i + 1u);
    }
    if (auto r = store_event(mem, addrs.driver_event, 0, event_flags_enable); !r)
      return reloco::unexpected(r.error());
    return packed_virtq_driver(mem, addrs, queue_size, slots, event_idx);
  }

  [[nodiscard]] std::uint32_t queue_size() const noexcept { return queue_size_; }
  [[nodiscard]] std::uint32_t free_descriptors() const noexcept { return num_free_; }
  [[nodiscard]] bool is_broken() const noexcept { return broken_; }

  /**
   * @brief Adds a request (@p out segments the device reads, then @p in
   * segments it writes) and makes it visible to the device.
   *
   * `error::capacity_exceeded` if too few descriptors are free;
   * `error::invalid_argument` for an empty request or a segment over 4 GiB.
   * A memory error while writing the descriptors latches the queue broken.
   */
  [[nodiscard]] reloco::result<void> try_add(reloco::span<const sg_type> out, reloco::span<const sg_type> in,
                                             std::uintptr_t token) noexcept {
    if (broken_)
      return reloco::unexpected(reloco::error::invalid_state);
    const std::size_t total = out.size() + in.size();
    if (total == 0)
      return reloco::unexpected(reloco::error::invalid_argument);
    if (total > num_free_)
      return reloco::unexpected(reloco::error::capacity_exceeded);

    std::uint64_t in_bytes = 0;
    for (const sg_type &e : out)
      if (e.length > 0xFFFFFFFFull)
        return reloco::unexpected(reloco::error::invalid_argument);
    for (const sg_type &e : in) {
      if (e.length > 0xFFFFFFFFull)
        return reloco::unexpected(reloco::error::invalid_argument);
      auto sum = reloco::checked_add(in_bytes, e.length);
      if (!sum)
        return reloco::unexpected(sum.error());
      in_bytes = *sum;
    }

    const std::uint16_t id = free_head_;
    std::uint16_t head_flags = 0;
    for (std::size_t k = 0; k < total; ++k) {
      const bool is_in = k >= out.size();
      const sg_type &e = is_in ? in[k - out.size()] : out[k];
      const bool last = k + 1 == total;

      std::uint32_t pos = avail_pos_;
      bool wrap = avail_wrap_;
      detail::packed_advance(pos, wrap, static_cast<std::uint32_t>(k), queue_size_);

      const std::uint16_t flags = static_cast<std::uint16_t>(
          detail::packed_avail_flags(wrap) | (is_in ? desc_f_write : 0u) | (last ? 0u : desc_f_next));
      if (auto w = write_body(pos, e.addr.value, static_cast<std::uint32_t>(e.length), id); !w)
        return fail(w.error());
      if (k == 0)
        head_flags = flags;
      else if (auto w = store_flags(pos, flags); !w)
        return fail(w.error());
    }

    // Publishing: every descriptor is fully written before the head becomes available.
    Barriers::wmb();
    if (auto w = store_flags(avail_pos_, head_flags); !w)
      return fail(w.error());

    free_head_ = slots_[id].next;
    slots_[id].token = token;
    slots_[id].in_bytes = in_bytes;
    slots_[id].num = static_cast<std::uint16_t>(total);
    slots_[id].in_flight = true;
    num_free_ -= static_cast<std::uint32_t>(total);
    num_added_ = static_cast<std::uint16_t>(num_added_ + total);
    detail::packed_advance(avail_pos_, avail_wrap_, static_cast<std::uint32_t>(total), queue_size_);
    return {};
  }

  /**
   * @brief Whether the device wants a kick for what was added since the last
   * call (call once per batch of `try_add`s).
   */
  [[nodiscard]] reloco::result<bool> needs_notify() noexcept {
    if (broken_)
      return reloco::unexpected(reloco::error::invalid_state);
    Barriers::mb();
    auto flags = traits::try_load16(*mem_, flags_addr_of(addrs_.device_event));
    if (!flags)
      return reloco::unexpected(flags.error());
    const std::uint16_t added = num_added_;
    num_added_ = 0;
    if (*flags == event_flags_disable)
      return false;
    if (*flags == event_flags_desc && event_idx_) {
      auto off_wrap = traits::try_load16(*mem_, addrs_.device_event);
      if (!off_wrap)
        return reloco::unexpected(off_wrap.error());
      return detail::packed_need_event(*off_wrap, avail_pos_, avail_wrap_, added, queue_size_);
    }
    return true;
  }

  /** @brief Enables/disables device interrupts (driver event-suppression flags). */
  [[nodiscard]] reloco::result<void> try_set_interrupts_enabled(bool enabled) noexcept {
    if (broken_)
      return reloco::unexpected(reloco::error::invalid_state);
    return traits::try_store16(*mem_, flags_addr_of(addrs_.driver_event),
                               enabled ? event_flags_enable : event_flags_disable);
  }

  /**
   * @brief EVENT_IDX only: asks for an interrupt once @p after more
   * descriptors have been completed (e.g. to coalesce completions).
   * `error::invalid_state` without EVENT_IDX.
   */
  [[nodiscard]] reloco::result<void> try_set_interrupt_after(std::uint32_t after) noexcept {
    if (broken_ || !event_idx_)
      return reloco::unexpected(reloco::error::invalid_state);
    return store_event(*mem_, addrs_.driver_event,
                       detail::packed_event_after(last_used_pos_, used_wrap_, after, queue_size_), event_flags_desc);
  }

  /**
   * @brief Reaps one completion, or an empty optional if the device has none.
   *
   * The id and length come from the device: an id that is out of range or not
   * in flight, or a length beyond the request's writable capacity, latches the
   * queue broken and reports `error::security_violation`.
   */
  [[nodiscard]] reloco::result<reloco::optional<completion>> try_get_used() noexcept {
    if (broken_)
      return reloco::unexpected(reloco::error::invalid_state);

    auto head_flags_at = flags_at(last_used_pos_);
    if (!head_flags_at)
      return reloco::unexpected(head_flags_at.error());
    auto flags = traits::try_load16(*mem_, *head_flags_at); // single fetch
    if (!flags)
      return reloco::unexpected(flags.error());
    if (!detail::packed_is_used(*flags, used_wrap_))
      return reloco::optional<completion>{};
    Barriers::rmb();

    auto at = addrs_.desc_at(last_used_pos_, queue_size_);
    if (!at)
      return reloco::unexpected(at.error());
    auto len_at = at->try_add(offsetof(virtq_packed_desc, len));
    auto id_at = at->try_add(offsetof(virtq_packed_desc, id));
    if (!len_at || !id_at)
      return reloco::unexpected(reloco::error::integer_overflow);
    auto len = try_read_object<std::uint32_t>(*mem_, *len_at);
    if (!len)
      return reloco::unexpected(len.error());
    auto raw_id = traits::try_load16(*mem_, *id_at);
    if (!raw_id)
      return reloco::unexpected(raw_id.error());

    auto id = try_checked_index(*raw_id, queue_size_);
    if (!id)
      return fail(reloco::error::security_violation);
    packed_driver_slot &slot = slots_[*id];
    if (!slot.in_flight || *len > slot.in_bytes)
      return fail(reloco::error::security_violation);

    completion c{slot.token, *len};
    slot.in_flight = false;
    slot.next = free_head_;
    free_head_ = static_cast<std::uint16_t>(*id);
    num_free_ += slot.num;
    detail::packed_advance(last_used_pos_, used_wrap_, slot.num, queue_size_);
    return reloco::optional<completion>(c);
  }

private:
  using traits = virtq_memory_traits<Mem, RingSpace>;
  using addr_type = phys_addr<void, RingSpace>;

  packed_virtq_driver(Mem &mem, const ring_addrs &addrs, std::uint32_t queue_size,
                      reloco::span<packed_driver_slot> slots, bool event_idx) noexcept
      : mem_(&mem), addrs_(addrs), slots_(slots), queue_size_(queue_size), num_free_(queue_size),
        event_idx_(event_idx) {}

  [[nodiscard]] static addr_type flags_addr_of(addr_type event) noexcept {
    auto a = event.try_add(offsetof(virtq_packed_event, flags));
    return a ? *a : addr_type{};
  }

  [[nodiscard]] static reloco::result<void> store_event(Mem &mem, addr_type event, std::uint16_t off_wrap,
                                                        std::uint16_t flags) noexcept {
    if (auto r = traits::try_store16(mem, event, off_wrap); !r)
      return r;
    Barriers::wmb();
    return traits::try_store16(mem, flags_addr_of(event), flags);
  }

  [[nodiscard]] reloco::result<addr_type> flags_at(std::uint32_t pos) const noexcept {
    auto at = addrs_.desc_at(pos, queue_size_);
    if (!at)
      return reloco::unexpected(at.error());
    return at->try_add(offsetof(virtq_packed_desc, flags));
  }

  [[nodiscard]] reloco::result<void> store_flags(std::uint32_t pos, std::uint16_t flags) noexcept {
    auto at = flags_at(pos);
    if (!at)
      return reloco::unexpected(at.error());
    return traits::try_store16(*mem_, *at, flags);
  }

  [[nodiscard]] reloco::result<void> write_body(std::uint32_t pos, std::uint64_t addr, std::uint32_t len,
                                                std::uint16_t id) noexcept {
    auto at = addrs_.desc_at(pos, queue_size_);
    if (!at)
      return reloco::unexpected(at.error());
    auto len_at = at->try_add(offsetof(virtq_packed_desc, len));
    auto id_at = at->try_add(offsetof(virtq_packed_desc, id));
    if (!len_at || !id_at)
      return reloco::unexpected(reloco::error::integer_overflow);
    if (auto w = try_write_object(*mem_, *at, addr); !w)
      return w;
    if (auto w = try_write_object(*mem_, *len_at, len); !w)
      return w;
    return traits::try_store16(*mem_, *id_at, id);
  }

  [[nodiscard]] reloco::unexpected<reloco::error> fail(reloco::error e) noexcept {
    broken_ = true;
    return reloco::unexpected(e);
  }

  Mem *mem_;
  ring_addrs addrs_;
  reloco::span<packed_driver_slot> slots_;
  std::uint32_t queue_size_;
  std::uint32_t num_free_;
  std::uint32_t avail_pos_ = 0;
  std::uint32_t last_used_pos_ = 0;
  std::uint16_t free_head_ = 0;
  std::uint16_t num_added_ = 0;
  bool avail_wrap_ = true;
  bool used_wrap_ = true;
  bool event_idx_;
  bool broken_ = false;
};

// ============================================================================
// Device
// ============================================================================

template <typename RingSpace, typename BufSpace, typename Mem, typename Barriers = smp_virtq_barriers>
class packed_virtq_device {
public:
  using ring_addrs = packed_ring_addrs<RingSpace>;
  using segment = chain_segment<BufSpace>;
  using chain = avail_chain<BufSpace>;

  /** @brief Binds a device to a packed ring; @p event_idx: `VIRTIO_F_EVENT_IDX` was negotiated. */
  [[nodiscard]] static reloco::result<packed_virtq_device> try_create(Mem &mem, const ring_addrs &addrs,
                                                                      std::uint32_t queue_size,
                                                                      bool event_idx = false) noexcept {
    auto layout = try_packed_layout(queue_size);
    if (!layout)
      return reloco::unexpected(layout.error());
    if (auto v = addrs.try_validate(*layout); !v)
      return reloco::unexpected(v.error());
    if (auto r = traits::try_store16(mem, addrs.device_event, 0); !r)
      return reloco::unexpected(r.error());
    auto flags = flags_addr_of(addrs.device_event);
    if (auto r = traits::try_store16(mem, flags, event_flags_enable); !r)
      return reloco::unexpected(r.error());
    return packed_virtq_device(mem, addrs, queue_size, event_idx);
  }

  [[nodiscard]] std::uint32_t queue_size() const noexcept { return queue_size_; }
  [[nodiscard]] bool is_broken() const noexcept { return broken_; }

  /**
   * @brief Pops the next available chain, or an empty optional if none.
   *
   * @param storage Caller-owned segment storage the returned chain views; size
   * it to at least `queue_size()` (a longer chain latches `capacity_exceeded`).
   *
   * The head's flags are loaded once and checked against the device's wrap
   * counter before anything else is read; every following descriptor must also
   * be available, the chain may not exceed the ring, may not be `INDIRECT`, and
   * readable segments must precede writable ones. Violations latch broken and
   * report `error::security_violation`. `chain::head` is the buffer id taken
   * from the chain's last descriptor.
   */
  [[nodiscard]] reloco::result<reloco::optional<chain>> try_pop(reloco::span<segment> storage) noexcept {
    if (broken_)
      return reloco::unexpected(reloco::error::invalid_state);

    std::size_t count = 0;
    std::size_t readable_count = 0;
    std::uint64_t readable_bytes = 0;
    std::uint64_t writable_bytes = 0;
    bool seen_writable = false;
    bool complete = false;
    std::uint16_t id = 0;

    for (std::uint32_t k = 0; k < queue_size_; ++k) {
      std::uint32_t pos = avail_pos_;
      bool wrap = avail_wrap_;
      detail::packed_advance(pos, wrap, k, queue_size_);

      auto at = addrs_.desc_at(pos, queue_size_);
      if (!at)
        return fail(at.error());
      auto flags_at = at->try_add(offsetof(virtq_packed_desc, flags));
      if (!flags_at)
        return fail(flags_at.error());
      auto flags = traits::try_load16(*mem_, *flags_at); // single fetch of the ownership bits
      if (!flags)
        return fail(flags.error());
      if (!detail::packed_is_avail(*flags, wrap)) {
        if (k == 0)
          return reloco::optional<chain>{};
        return fail(reloco::error::security_violation); // chain not fully published
      }
      Barriers::rmb();
      auto d = try_read_object<virtq_packed_desc>(*mem_, *at); // snapshot; validated on the copy
      if (!d)
        return fail(d.error());

      if ((*flags & desc_f_indirect) != 0)
        return fail(reloco::error::security_violation);
      const bool writable = (*flags & desc_f_write) != 0;
      if (!writable && seen_writable)
        return fail(reloco::error::security_violation);
      seen_writable |= writable;

      if (count >= storage.size())
        return fail(reloco::error::capacity_exceeded);
      storage[count] = segment{phys_addr<void, BufSpace>{d->addr}, d->len};
      ++count;
      if (writable)
        writable_bytes += d->len;
      else {
        ++readable_count;
        readable_bytes += d->len;
      }

      if ((*flags & desc_f_next) == 0) {
        id = d->id;
        complete = true;
        break;
      }
    }
    if (!complete)
      return fail(reloco::error::security_violation); // chain longer than the ring

    detail::packed_advance(avail_pos_, avail_wrap_, static_cast<std::uint32_t>(count), queue_size_);
    chain c;
    c.head = id;
    c.desc_count = static_cast<std::uint16_t>(count);
    c.readable = storage.subspan(0, readable_count);
    c.writable = storage.subspan(readable_count, count - readable_count);
    c.readable_bytes = readable_bytes;
    c.writable_bytes = writable_bytes;
    return reloco::optional<chain>(c);
  }

  /**
   * @brief Completes @p c, reporting @p written bytes placed in its writable
   * segments: writes `{id, len}` at the used position, then the used flags
   * (after a write barrier), and advances by the chain's descriptor count.
   * `error::invalid_argument` if @p written exceeds the chain's writable size
   * or @p c is malformed (a device bug; the queue is not latched).
   */
  [[nodiscard]] reloco::result<void> try_push_used(const chain &c, std::uint32_t written) noexcept {
    if (broken_)
      return reloco::unexpected(reloco::error::invalid_state);
    if (written > c.writable_bytes || c.desc_count == 0 || c.desc_count > queue_size_)
      return reloco::unexpected(reloco::error::invalid_argument);

    auto at = addrs_.desc_at(used_pos_, queue_size_);
    if (!at)
      return reloco::unexpected(at.error());
    auto len_at = at->try_add(offsetof(virtq_packed_desc, len));
    auto id_at = at->try_add(offsetof(virtq_packed_desc, id));
    auto flags_at = at->try_add(offsetof(virtq_packed_desc, flags));
    if (!len_at || !id_at || !flags_at)
      return reloco::unexpected(reloco::error::integer_overflow);

    if (auto w = try_write_object(*mem_, *len_at, written); !w)
      return w;
    if (auto w = traits::try_store16(*mem_, *id_at, c.head); !w)
      return w;
    Barriers::wmb();
    const std::uint16_t flags = static_cast<std::uint16_t>(detail::packed_used_flags(used_wrap_) |
                                                           (written > 0 ? desc_f_write : 0u));
    if (auto w = traits::try_store16(*mem_, *flags_at, flags); !w)
      return w;

    detail::packed_advance(used_pos_, used_wrap_, c.desc_count, queue_size_);
    num_used_ = static_cast<std::uint16_t>(num_used_ + c.desc_count);
    return {};
  }

  /**
   * @brief Whether the driver wants an interrupt for what was completed since
   * the last call (call once per batch of `try_push_used`s).
   */
  [[nodiscard]] reloco::result<bool> should_interrupt() noexcept {
    if (broken_)
      return reloco::unexpected(reloco::error::invalid_state);
    Barriers::mb();
    auto flags = traits::try_load16(*mem_, flags_addr_of(addrs_.driver_event));
    if (!flags)
      return reloco::unexpected(flags.error());
    const std::uint16_t added = num_used_;
    num_used_ = 0;
    if (*flags == event_flags_disable)
      return false;
    if (*flags == event_flags_desc && event_idx_) {
      auto off_wrap = traits::try_load16(*mem_, addrs_.driver_event);
      if (!off_wrap)
        return reloco::unexpected(off_wrap.error());
      return detail::packed_need_event(*off_wrap, used_pos_, used_wrap_, added, queue_size_);
    }
    return true;
  }

  /** @brief Enables/disables driver notifications (device event-suppression flags). */
  [[nodiscard]] reloco::result<void> try_set_notify_enabled(bool enabled) noexcept {
    if (broken_)
      return reloco::unexpected(reloco::error::invalid_state);
    return traits::try_store16(*mem_, flags_addr_of(addrs_.device_event),
                               enabled ? event_flags_enable : event_flags_disable);
  }

  /**
   * @brief EVENT_IDX only: asks for a kick once @p after more descriptors have
   * been made available. `error::invalid_state` without EVENT_IDX.
   */
  [[nodiscard]] reloco::result<void> try_set_notify_after(std::uint32_t after) noexcept {
    if (broken_ || !event_idx_)
      return reloco::unexpected(reloco::error::invalid_state);
    if (auto r = traits::try_store16(*mem_, addrs_.device_event,
                                     detail::packed_event_after(avail_pos_, avail_wrap_, after, queue_size_));
        !r)
      return r;
    Barriers::wmb();
    return traits::try_store16(*mem_, flags_addr_of(addrs_.device_event), event_flags_desc);
  }

private:
  using traits = virtq_memory_traits<Mem, RingSpace>;
  using addr_type = phys_addr<void, RingSpace>;

  packed_virtq_device(Mem &mem, const ring_addrs &addrs, std::uint32_t queue_size, bool event_idx) noexcept
      : mem_(&mem), addrs_(addrs), queue_size_(queue_size), event_idx_(event_idx) {}

  [[nodiscard]] static addr_type flags_addr_of(addr_type event) noexcept {
    auto a = event.try_add(offsetof(virtq_packed_event, flags));
    return a ? *a : addr_type{};
  }

  [[nodiscard]] reloco::unexpected<reloco::error> fail(reloco::error e) noexcept {
    broken_ = true;
    return reloco::unexpected(e);
  }

  Mem *mem_;
  ring_addrs addrs_;
  std::uint32_t queue_size_;
  std::uint32_t avail_pos_ = 0;
  std::uint32_t used_pos_ = 0;
  std::uint16_t num_used_ = 0;
  bool avail_wrap_ = true;
  bool used_wrap_ = true;
  bool event_idx_;
  bool broken_ = false;
};

} // namespace structo::virtio
