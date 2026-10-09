// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file split_ring.hpp
 * @brief `split_virtq_driver` / `split_virtq_device`: the two ends of a
 * VIRTIO 1.x split virtqueue (§2.7), allocation-free and fallible.
 *
 * Template parameters (all tags, see `virtq_layout.hpp` / `virtq_memory.hpp`):
 *
 * - `RingSpace` -- address space through which *this side* reaches the ring areas;
 * - `BufSpace`  -- address space of descriptor `addr` fields (what the peer sees);
 * - `Mem`       -- ring-memory backend, `virtq_memory_traits<Mem, RingSpace>`;
 * - `Barriers`  -- ordering policy (`smp_virtq_barriers` by default).
 *
 * ## Optional features (negotiated by the transport; pass what was negotiated)
 *
 * - `VIRTIO_F_EVENT_IDX`: pass `event_idx = true` to both `try_create`s (the
 *   ring areas must be sized with `try_split_layout(n, true)`). Notification
 *   suppression then uses `used_event`/`avail_event` and `need_event()`
 *   instead of the `NO_INTERRUPT`/`NO_NOTIFY` flags; the bool toggles keep
 *   their meaning but write the event index.
 * - `VIRTIO_F_INDIRECT_DESC`: the driver uses `try_add_indirect` (the caller
 *   supplies the table memory); the device uses the `try_pop(storage, buf_mem)`
 *   overload, which reads indirect tables from buffer memory. The plain
 *   `try_pop(storage)` treats an `INDIRECT` flag as a protocol violation.
 *
 * ## Example
 *
 * @code
 * struct ring_space {};
 * using mem_t = structo::virtio::direct_virtq_memory<ring_space>;
 *
 * // Driver: add one request (a 16-byte header the device reads, a 512-byte
 * // buffer it writes), kick, later reap the completion.
 * reloco::array<split_driver_slot, 8> slots;
 * auto drv = split_virtq_driver<ring_space, ring_space, mem_t>::try_create(mem, addrs, 8, slots);
 * reloco::array<structo::sg_entry<ring_space>, 1> out{{{hdr_addr, 16}}}; // device-readable
 * reloco::array<structo::sg_entry<ring_space>, 1> in{{{buf_addr, 512}}}; // device-writable
 * (void)drv->try_add(out, in, token);
 * (void)drv->try_publish();
 * if (auto n = drv->needs_notify(); n && *n) ring_doorbell();
 *
 * // Device: pop, process, complete.
 * reloco::array<chain_segment<ring_space>, 8> segs;
 * auto dev = split_virtq_device<ring_space, ring_space, mem_t>::try_create(mem, addrs, 8);
 * auto chain = dev->try_pop(segs);
 * if (chain && chain->has_value()) {
 *   // try_read_chain(buf_mem, (*chain)->readable, 0, dst) ...
 *   (void)dev->try_push_used(**chain, written);
 * }
 * @endcode
 *
 * ## Hostile peer
 *
 * Each side treats everything it reads from the ring as untrusted: indices and
 * flags are fetched once into locals, descriptors are copied out before use,
 * and every peer-controlled index is masked with `nospec::sanitize` before it
 * is checked (`try_checked_index`). A violation latches the queue into a
 * *broken* state (`is_broken()`): every later call fails with
 * `error::invalid_state`, and the transport should signal `DEVICE_NEEDS_RESET`
 * (device side) or reset the device (driver side).
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
#include <type_traits>

namespace structo::virtio {

/** @brief Driver-private per-descriptor bookkeeping (never visible to the device). */
struct split_driver_slot {
  std::uintptr_t token = 0;   ///< Caller cookie of the chain headed here.
  std::uint64_t in_bytes = 0; ///< Total device-writable bytes of the chain headed here.
  std::uint16_t next = 0;     ///< Free-list link (free slot) or chain link (in-flight slot).
  std::uint16_t chain_len = 0;
  bool in_flight = false;
};

using split_completion = virtq_completion;

// ============================================================================
// Driver
// ============================================================================

template <typename RingSpace, typename BufSpace, typename Mem, typename Barriers = smp_virtq_barriers>
class split_virtq_driver {
public:
  using ring_addrs = split_ring_addrs<RingSpace>;
  using sg_type = sg_entry<BufSpace, std::uint64_t>;
  using completion = split_completion;

  /**
   * @brief Binds a driver to a (zeroed) ring and initialises the avail ring.
   * @param slots Caller-owned bookkeeping, at least @p queue_size entries.
   * `error::invalid_argument` on a bad queue size/addresses/slot count.
   */
  [[nodiscard]] static reloco::result<split_virtq_driver> try_create(Mem &mem, const ring_addrs &addrs,
                                                                     std::uint32_t queue_size,
                                                                     reloco::span<split_driver_slot> slots,
                                                                     bool event_idx = false) noexcept {
    auto layout = try_split_layout(queue_size, event_idx);
    if (!layout)
      return reloco::unexpected(layout.error());
    if (auto v = addrs.try_validate(*layout); !v)
      return reloco::unexpected(v.error());
    if (slots.size() < queue_size)
      return reloco::unexpected(reloco::error::invalid_argument);

    for (std::uint32_t i = 0; i < queue_size; ++i) {
      slots[i] = split_driver_slot{};
      slots[i].next = static_cast<std::uint16_t>(i + 1u);
    }

    auto idx_addr = addrs.avail_idx_addr();
    if (!idx_addr)
      return reloco::unexpected(idx_addr.error());
    if (auto r = traits::try_store16(mem, addrs.avail_flags_addr(), 0); !r)
      return reloco::unexpected(r.error());
    if (auto r = traits::try_store16(mem, *idx_addr, 0); !r)
      return reloco::unexpected(r.error());
    if (event_idx) {
      auto ev = addrs.used_event_addr(queue_size);
      if (!ev)
        return reloco::unexpected(ev.error());
      if (auto r = traits::try_store16(mem, *ev, 0); !r)
        return reloco::unexpected(r.error());
    }

    return split_virtq_driver(mem, addrs, queue_size, slots, event_idx);
  }

  [[nodiscard]] std::uint32_t queue_size() const noexcept { return queue_size_; }
  [[nodiscard]] std::uint32_t free_descriptors() const noexcept { return num_free_; }
  [[nodiscard]] bool is_broken() const noexcept { return broken_; }

  /**
   * @brief Stages one request: @p out segments the device reads, then @p in
   * segments it writes. Not visible to the device until `try_publish()`.
   *
   * `error::capacity_exceeded` if there are too few free descriptors (retry
   * after reaping completions); `error::invalid_argument` for an empty request
   * or a segment longer than 4 GiB.
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

    auto in_bytes_r = validate_segments(out, in);
    if (!in_bytes_r)
      return reloco::unexpected(in_bytes_r.error());
    const std::uint64_t in_bytes = *in_bytes_r;

    // Consecutive free-list entries become the chain, so slot links already
    // describe it; nothing driver-side is modified until every write succeeded.
    const std::uint16_t head = free_head_;
    std::uint16_t cur = head;
    for (std::size_t i = 0; i < total; ++i) {
      const bool is_in = i >= out.size();
      const sg_type &e = is_in ? in[i - out.size()] : out[i];
      const bool last = i + 1 == total;

      virtq_desc d{};
      d.addr = e.addr.value;
      d.len = static_cast<std::uint32_t>(e.length);
      d.flags = static_cast<std::uint16_t>((is_in ? desc_f_write : 0u) | (last ? 0u : desc_f_next));
      d.next = last ? std::uint16_t{0} : slots_[cur].next;

      auto at = addrs_.desc_at(cur, queue_size_);
      if (!at)
        return reloco::unexpected(at.error());
      if (auto w = try_write_object(*mem_, *at, d); !w)
        return w;
      if (!last)
        cur = slots_[cur].next;
    }

    auto ring_at = addrs_.avail_ring_at(staged_idx_ % queue_size_, queue_size_);
    if (!ring_at)
      return reloco::unexpected(ring_at.error());
    if (auto s = traits::try_store16(*mem_, *ring_at, head); !s)
      return s;

    const std::uint16_t new_free_head = slots_[cur].next;
    slots_[head].token = token;
    slots_[head].in_bytes = in_bytes;
    slots_[head].chain_len = static_cast<std::uint16_t>(total);
    slots_[head].in_flight = true;
    free_head_ = new_free_head;
    num_free_ -= static_cast<std::uint32_t>(total);
    ++in_flight_chains_;
    ++staged_idx_;
    return {};
  }

  /**
   * @brief Like `try_add`, but the whole request occupies a single ring
   * descriptor pointing at an indirect table (`VIRTIO_F_INDIRECT_DESC`).
   *
   * @param table_mem   Memory backend through which the driver writes the table.
   * @param table_write Table location as seen by the driver (`TableSpace`).
   * @param table_dev   The same bytes as the device addresses them (`BufSpace`).
   * @param table_bytes Table capacity; at least 16 bytes per segment.
   *
   * The table must stay untouched until the request completes. Errors as for
   * `try_add`; `error::invalid_argument` if @p table_bytes is too small.
   */
  template <typename TableMem, typename TableSpace>
  [[nodiscard]] reloco::result<void>
  try_add_indirect(reloco::span<const sg_type> out, reloco::span<const sg_type> in, std::uintptr_t token,
                   TableMem &table_mem, phys_addr<void, TableSpace> table_write, phys_addr<void, BufSpace> table_dev,
                   std::size_t table_bytes) noexcept {
    if (broken_)
      return reloco::unexpected(reloco::error::invalid_state);
    const std::size_t total = out.size() + in.size();
    if (total == 0 || total > 0xFFFFFFFFu / sizeof(virtq_desc))
      return reloco::unexpected(reloco::error::invalid_argument);
    const std::size_t need = total * sizeof(virtq_desc);
    if (table_bytes < need)
      return reloco::unexpected(reloco::error::invalid_argument);
    if (num_free_ == 0)
      return reloco::unexpected(reloco::error::capacity_exceeded);
    auto in_bytes_r = validate_segments(out, in);
    if (!in_bytes_r)
      return reloco::unexpected(in_bytes_r.error());

    for (std::size_t i = 0; i < total; ++i) {
      const bool is_in = i >= out.size();
      const sg_type &e = is_in ? in[i - out.size()] : out[i];
      const bool last = i + 1 == total;
      virtq_desc d{};
      d.addr = e.addr.value;
      d.len = static_cast<std::uint32_t>(e.length);
      d.flags = static_cast<std::uint16_t>((is_in ? desc_f_write : 0u) | (last ? 0u : desc_f_next));
      d.next = last ? std::uint16_t{0} : static_cast<std::uint16_t>(i + 1);
      auto at = table_write.try_add(i * sizeof(virtq_desc));
      if (!at)
        return reloco::unexpected(at.error());
      if (auto w = try_write_object(table_mem, *at, d); !w)
        return w;
    }

    const std::uint16_t head = free_head_;
    virtq_desc main{};
    main.addr = table_dev.value;
    main.len = static_cast<std::uint32_t>(need);
    main.flags = desc_f_indirect;
    auto at = addrs_.desc_at(head, queue_size_);
    if (!at)
      return reloco::unexpected(at.error());
    if (auto w = try_write_object(*mem_, *at, main); !w)
      return w;
    auto ring_at = addrs_.avail_ring_at(staged_idx_ % queue_size_, queue_size_);
    if (!ring_at)
      return reloco::unexpected(ring_at.error());
    if (auto s = traits::try_store16(*mem_, *ring_at, head); !s)
      return s;

    const std::uint16_t new_free_head = slots_[head].next;
    slots_[head].token = token;
    slots_[head].in_bytes = *in_bytes_r;
    slots_[head].chain_len = 1;
    slots_[head].in_flight = true;
    free_head_ = new_free_head;
    --num_free_;
    ++in_flight_chains_;
    ++staged_idx_;
    return {};
  }

  /** @brief Makes every staged request visible to the device (write barrier, then `avail.idx`). */
  [[nodiscard]] reloco::result<void> try_publish() noexcept {
    if (broken_)
      return reloco::unexpected(reloco::error::invalid_state);
    auto idx_addr = addrs_.avail_idx_addr();
    if (!idx_addr)
      return reloco::unexpected(idx_addr.error());
    Barriers::wmb();
    return traits::try_store16(*mem_, *idx_addr, staged_idx_);
  }

  /**
   * @brief Whether the device wants a notification (kick) for what was just
   * published. With EVENT_IDX, call once per `try_publish()`: it compares the
   * device's `avail_event` against the indices published since the last call.
   */
  [[nodiscard]] reloco::result<bool> needs_notify() noexcept {
    if (broken_)
      return reloco::unexpected(reloco::error::invalid_state);
    Barriers::mb();
    if (event_idx_) {
      auto ev_addr = addrs_.avail_event_addr(queue_size_);
      if (!ev_addr)
        return reloco::unexpected(ev_addr.error());
      auto ev = traits::try_load16(*mem_, *ev_addr); // device-controlled; arithmetic only
      if (!ev)
        return reloco::unexpected(ev.error());
      const std::uint16_t old = notified_idx_;
      notified_idx_ = staged_idx_;
      return need_event(*ev, staged_idx_, old);
    }
    auto flags = traits::try_load16(*mem_, addrs_.used_flags_addr());
    if (!flags)
      return reloco::unexpected(flags.error());
    return (*flags & used_f_no_notify) == 0;
  }

  /**
   * @brief Enables/disables device interrupts: the `NO_INTERRUPT` flag, or with
   * EVENT_IDX `used_event` (enabled: interrupt on the next completion;
   * disabled: none until the index wraps around).
   */
  [[nodiscard]] reloco::result<void> try_set_interrupts_enabled(bool enabled) noexcept {
    if (broken_)
      return reloco::unexpected(reloco::error::invalid_state);
    if (event_idx_)
      return try_set_used_event(enabled ? last_used_ : static_cast<std::uint16_t>(last_used_ - 1u));
    return traits::try_store16(*mem_, addrs_.avail_flags_addr(), enabled ? 0 : avail_f_no_interrupt);
  }

  /**
   * @brief EVENT_IDX only: asks for an interrupt once the device's used index
   * passes @p used_event (e.g. `last_used + n` to coalesce @p n completions).
   * `error::invalid_state` without EVENT_IDX.
   */
  [[nodiscard]] reloco::result<void> try_set_used_event(std::uint16_t used_event) noexcept {
    if (broken_ || !event_idx_)
      return reloco::unexpected(reloco::error::invalid_state);
    auto ev = addrs_.used_event_addr(queue_size_);
    if (!ev)
      return reloco::unexpected(ev.error());
    return traits::try_store16(*mem_, *ev, used_event);
  }

  /**
   * @brief Reaps one completion, or an empty optional if the device has none.
   *
   * The used index, element id, and length are device-controlled: a completion
   * for an unknown/idle id, a length beyond the chain's writable capacity, or
   * more completions than outstanding requests latches the queue broken and
   * reports `error::security_violation`.
   */
  [[nodiscard]] reloco::result<reloco::optional<completion>> try_get_used() noexcept {
    if (broken_)
      return reloco::unexpected(reloco::error::invalid_state);

    auto used_idx_addr = addrs_.used_idx_addr();
    if (!used_idx_addr)
      return reloco::unexpected(used_idx_addr.error());
    auto idx = traits::try_load16(*mem_, *used_idx_addr); // single fetch
    if (!idx)
      return reloco::unexpected(idx.error());
    if (*idx == last_used_)
      return reloco::optional<completion>{};

    const std::uint16_t pending = static_cast<std::uint16_t>(*idx - last_used_);
    if (pending > in_flight_chains_)
      return fail(reloco::error::security_violation);
    Barriers::rmb();

    auto elem_at = addrs_.used_elem_at(last_used_ % queue_size_, queue_size_);
    if (!elem_at)
      return reloco::unexpected(elem_at.error());
    auto elem = try_read_object<virtq_used_elem>(*mem_, *elem_at); // snapshot
    if (!elem)
      return reloco::unexpected(elem.error());

    auto id = try_checked_index(elem->id, queue_size_);
    if (!id)
      return fail(reloco::error::security_violation);
    split_driver_slot &head = slots_[*id];
    if (!head.in_flight || elem->len > head.in_bytes)
      return fail(reloco::error::security_violation);

    completion c{head.token, elem->len};

    // Return the chain to the free list.
    std::uint16_t last_id = static_cast<std::uint16_t>(*id);
    for (std::uint16_t n = 1; n < head.chain_len; ++n)
      last_id = slots_[last_id].next;
    slots_[last_id].next = free_head_;
    free_head_ = static_cast<std::uint16_t>(*id);
    num_free_ += head.chain_len;
    head.in_flight = false;
    --in_flight_chains_;
    ++last_used_;
    return reloco::optional<completion>(c);
  }

private:
  using traits = virtq_memory_traits<Mem, RingSpace>;

  split_virtq_driver(Mem &mem, const ring_addrs &addrs, std::uint32_t queue_size, reloco::span<split_driver_slot> slots,
                     bool event_idx) noexcept
      : mem_(&mem), addrs_(addrs), slots_(slots), queue_size_(queue_size), num_free_(queue_size),
        event_idx_(event_idx) {}

  /** @brief Rejects oversized segments; returns the total device-writable byte count. */
  [[nodiscard]] static reloco::result<std::uint64_t> validate_segments(reloco::span<const sg_type> out,
                                                                       reloco::span<const sg_type> in) noexcept {
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
    return in_bytes;
  }

  [[nodiscard]] reloco::unexpected<reloco::error> fail(reloco::error e) noexcept {
    broken_ = true;
    return reloco::unexpected(e);
  }

  Mem *mem_;
  ring_addrs addrs_;
  reloco::span<split_driver_slot> slots_;
  std::uint32_t queue_size_;
  std::uint32_t num_free_;
  std::uint32_t in_flight_chains_ = 0;
  std::uint16_t free_head_ = 0;
  std::uint16_t staged_idx_ = 0;
  std::uint16_t last_used_ = 0;
  std::uint16_t notified_idx_ = 0;
  bool event_idx_;
  bool broken_ = false;
};

// ============================================================================
// Device
// ============================================================================

template <typename RingSpace, typename BufSpace, typename Mem, typename Barriers = smp_virtq_barriers>
class split_virtq_device {
public:
  using ring_addrs = split_ring_addrs<RingSpace>;
  using segment = chain_segment<BufSpace>;
  using chain = avail_chain<BufSpace>;

  /** @brief Binds a device to a ring and initialises the used ring header. */
  [[nodiscard]] static reloco::result<split_virtq_device>
  try_create(Mem &mem, const ring_addrs &addrs, std::uint32_t queue_size, bool event_idx = false) noexcept {
    auto layout = try_split_layout(queue_size, event_idx);
    if (!layout)
      return reloco::unexpected(layout.error());
    if (auto v = addrs.try_validate(*layout); !v)
      return reloco::unexpected(v.error());

    auto idx_addr = addrs.used_idx_addr();
    if (!idx_addr)
      return reloco::unexpected(idx_addr.error());
    if (auto r = traits::try_store16(mem, addrs.used_flags_addr(), 0); !r)
      return reloco::unexpected(r.error());
    if (auto r = traits::try_store16(mem, *idx_addr, 0); !r)
      return reloco::unexpected(r.error());
    if (event_idx) {
      auto ev = addrs.avail_event_addr(queue_size);
      if (!ev)
        return reloco::unexpected(ev.error());
      if (auto r = traits::try_store16(mem, *ev, 0); !r)
        return reloco::unexpected(r.error());
    }
    return split_virtq_device(mem, addrs, queue_size, event_idx);
  }

  [[nodiscard]] std::uint32_t queue_size() const noexcept { return queue_size_; }
  [[nodiscard]] bool is_broken() const noexcept { return broken_; }

  /**
   * @brief Pops the next available chain, or an empty optional if none.
   *
   * @param storage Caller-owned segment storage the returned chain views.
   * Size it to at least `queue_size()`; a longer chain than fits latches the
   * queue broken (`error::capacity_exceeded`).
   *
   * Every descriptor is copied out and validated: head/`next` in range, no
   * loops (a chain visits at most `queue_size` descriptors), no `INDIRECT`
   * (not negotiated in this header), device-readable segments before
   * device-writable ones. Violations latch broken and report
   * `error::security_violation`.
   */
  [[nodiscard]] reloco::result<reloco::optional<chain>> try_pop(reloco::span<segment> storage) noexcept {
    return pop_impl<no_buf_mem>(storage, nullptr);
  }

  /**
   * @brief `try_pop` with `VIRTIO_F_INDIRECT_DESC` support: an `INDIRECT`
   * descriptor's table is read through @p buf_mem (`virtq_memory_traits<BufMem, BufSpace>`).
   *
   * The table descriptor must be the end of its chain, have a non-zero
   * 16-byte-multiple length, and the table may not contain `INDIRECT`
   * descriptors; `next` links are bounds-checked against the table size and
   * loops are rejected. Segments of a direct prefix and the table are
   * flattened into one chain (readable before writable).
   */
  template <typename BufMem>
  [[nodiscard]] reloco::result<reloco::optional<chain>> try_pop(reloco::span<segment> storage,
                                                                BufMem &buf_mem) noexcept {
    return pop_impl<BufMem>(storage, &buf_mem);
  }

private:
  template <typename BufMem>
  [[nodiscard]] reloco::result<reloco::optional<chain>> pop_impl(reloco::span<segment> storage,
                                                                 BufMem *buf_mem) noexcept {
    if (broken_)
      return reloco::unexpected(reloco::error::invalid_state);

    auto idx_addr = addrs_.avail_idx_addr();
    if (!idx_addr)
      return fail(idx_addr.error());
    auto idx = traits::try_load16(*mem_, *idx_addr); // single fetch
    if (!idx)
      return fail(idx.error());
    if (*idx == last_avail_)
      return reloco::optional<chain>{};

    const std::uint16_t pending = static_cast<std::uint16_t>(*idx - last_avail_);
    if (pending > queue_size_)
      return fail(reloco::error::security_violation);
    Barriers::rmb();

    auto head_at = addrs_.avail_ring_at(last_avail_ % queue_size_, queue_size_);
    if (!head_at)
      return fail(head_at.error());
    auto head_raw = traits::try_load16(*mem_, *head_at);
    if (!head_raw)
      return fail(head_raw.error());
    auto head = try_checked_index(*head_raw, queue_size_);
    if (!head)
      return fail(head.error());

    std::size_t count = 0;
    std::size_t readable_count = 0;
    std::uint64_t readable_bytes = 0;
    std::uint64_t writable_bytes = 0;
    bool seen_writable = false;

    // Validates one copied descriptor and appends it as a segment.
    auto accept = [&](const virtq_desc &d) noexcept -> reloco::result<void> {
      const bool writable = (d.flags & desc_f_write) != 0;
      if (!writable && seen_writable)
        return reloco::unexpected(reloco::error::security_violation);
      seen_writable |= writable;
      if (count >= storage.size())
        return reloco::unexpected(reloco::error::capacity_exceeded);
      storage[count] = segment{phys_addr<void, BufSpace>{d.addr}, d.len};
      ++count;
      if (writable)
        writable_bytes += d.len;
      else {
        ++readable_count;
        readable_bytes += d.len;
      }
      return {};
    };

    bool complete = false;
    std::uint32_t cur = *head;
    for (std::uint32_t step = 0; step < queue_size_; ++step) {
      auto at = addrs_.desc_at(cur, queue_size_);
      if (!at)
        return fail(at.error());
      auto d = try_read_object<virtq_desc>(*mem_, *at); // snapshot; validated on the copy
      if (!d)
        return fail(d.error());

      if ((d->flags & desc_f_indirect) != 0) {
        if constexpr (std::is_same_v<BufMem, no_buf_mem>) {
          return fail(reloco::error::security_violation); // not negotiated
        } else {
          if (auto r = walk_indirect(*buf_mem, *d, accept); !r)
            return fail(r.error());
          complete = true;
          break;
        }
      }
      if (auto r = accept(*d); !r)
        return fail(r.error());

      if ((d->flags & desc_f_next) == 0) {
        complete = true;
        break;
      }
      auto next = try_checked_index(d->next, queue_size_);
      if (!next)
        return fail(next.error());
      cur = *next;
    }
    if (!complete)
      return fail(reloco::error::security_violation); // loop

    ++last_avail_;
    chain c;
    c.head = static_cast<std::uint16_t>(*head);
    c.desc_count = static_cast<std::uint16_t>(count);
    c.readable = storage.subspan(0, readable_count);
    c.writable = storage.subspan(readable_count, count - readable_count);
    c.readable_bytes = readable_bytes;
    c.writable_bytes = writable_bytes;
    return reloco::optional<chain>(c);
  }

  template <typename BufMem, typename Accept>
  [[nodiscard]] static reloco::result<void> walk_indirect(BufMem &buf_mem, const virtq_desc &ind,
                                                          Accept &accept) noexcept {
    if ((ind.flags & desc_f_next) != 0 || ind.len == 0 || ind.len % sizeof(virtq_desc) != 0)
      return reloco::unexpected(reloco::error::security_violation);
    const std::uint32_t n = ind.len / static_cast<std::uint32_t>(sizeof(virtq_desc));
    const phys_addr<void, BufSpace> base{ind.addr};

    std::uint32_t cur = 0;
    for (std::uint32_t step = 0; step < n; ++step) {
      auto at = base.try_add(std::uint64_t{cur} * sizeof(virtq_desc));
      if (!at)
        return reloco::unexpected(reloco::error::security_violation);
      auto d = try_read_object<virtq_desc>(buf_mem, *at); // snapshot
      if (!d)
        return reloco::unexpected(d.error());
      if ((d->flags & desc_f_indirect) != 0)
        return reloco::unexpected(reloco::error::security_violation);
      if (auto r = accept(*d); !r)
        return r;
      if ((d->flags & desc_f_next) == 0)
        return {};
      auto next = try_checked_index(d->next, n);
      if (!next)
        return reloco::unexpected(reloco::error::security_violation);
      cur = *next;
    }
    return reloco::unexpected(reloco::error::security_violation); // loop
  }

public:
  /**
   * @brief Completes @p c, reporting @p written bytes placed in its writable
   * segments, and publishes the used element (write barrier, then `used.idx`).
   * `error::invalid_argument` if @p written exceeds the chain's writable size
   * (a device bug; the queue is not latched).
   */
  [[nodiscard]] reloco::result<void> try_push_used(const chain &c, std::uint32_t written) noexcept {
    if (broken_)
      return reloco::unexpected(reloco::error::invalid_state);
    if (written > c.writable_bytes)
      return reloco::unexpected(reloco::error::invalid_argument);

    auto elem_at = addrs_.used_elem_at(used_idx_ % queue_size_, queue_size_);
    if (!elem_at)
      return reloco::unexpected(elem_at.error());
    auto idx_addr = addrs_.used_idx_addr();
    if (!idx_addr)
      return reloco::unexpected(idx_addr.error());

    if (auto w = try_write_object(*mem_, *elem_at, virtq_used_elem{c.head, written}); !w)
      return w;
    Barriers::wmb();
    const std::uint16_t next_idx = static_cast<std::uint16_t>(used_idx_ + 1u);
    if (auto s = traits::try_store16(*mem_, *idx_addr, next_idx); !s)
      return s;
    used_idx_ = next_idx;
    return {};
  }

  /**
   * @brief Whether the driver wants an interrupt for what was just published.
   * With EVENT_IDX, call once per batch of `try_push_used`: it compares the
   * driver's `used_event` against the indices published since the last call.
   */
  [[nodiscard]] reloco::result<bool> should_interrupt() noexcept {
    if (broken_)
      return reloco::unexpected(reloco::error::invalid_state);
    Barriers::mb();
    if (event_idx_) {
      auto ev_addr = addrs_.used_event_addr(queue_size_);
      if (!ev_addr)
        return reloco::unexpected(ev_addr.error());
      auto ev = traits::try_load16(*mem_, *ev_addr); // driver-controlled; arithmetic only
      if (!ev)
        return reloco::unexpected(ev.error());
      const std::uint16_t old = signaled_idx_;
      signaled_idx_ = used_idx_;
      return need_event(*ev, used_idx_, old);
    }
    auto flags = traits::try_load16(*mem_, addrs_.avail_flags_addr());
    if (!flags)
      return reloco::unexpected(flags.error());
    return (*flags & avail_f_no_interrupt) == 0;
  }

  /**
   * @brief Enables/disables driver notifications: the `NO_NOTIFY` flag, or with
   * EVENT_IDX `avail_event` (enabled: kick for the next request; disabled: none
   * until the index wraps around).
   */
  [[nodiscard]] reloco::result<void> try_set_notify_enabled(bool enabled) noexcept {
    if (broken_)
      return reloco::unexpected(reloco::error::invalid_state);
    if (event_idx_) {
      auto ev = addrs_.avail_event_addr(queue_size_);
      if (!ev)
        return reloco::unexpected(ev.error());
      return traits::try_store16(*mem_, *ev, enabled ? last_avail_ : static_cast<std::uint16_t>(last_avail_ - 1u));
    }
    return traits::try_store16(*mem_, addrs_.used_flags_addr(), enabled ? 0 : used_f_no_notify);
  }

private:
  using traits = virtq_memory_traits<Mem, RingSpace>;

  struct no_buf_mem {};

  split_virtq_device(Mem &mem, const ring_addrs &addrs, std::uint32_t queue_size, bool event_idx) noexcept
      : mem_(&mem), addrs_(addrs), queue_size_(queue_size), event_idx_(event_idx) {}

  [[nodiscard]] reloco::unexpected<reloco::error> fail(reloco::error e) noexcept {
    broken_ = true;
    return reloco::unexpected(e);
  }

  Mem *mem_;
  ring_addrs addrs_;
  std::uint32_t queue_size_;
  std::uint16_t last_avail_ = 0;
  std::uint16_t used_idx_ = 0;
  std::uint16_t signaled_idx_ = 0;
  bool event_idx_;
  bool broken_ = false;
};

} // namespace structo::virtio
