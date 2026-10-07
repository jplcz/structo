// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file virtio_mmio.hpp
 * @brief Device-side VIRTIO-over-MMIO (v2, VIRTIO 1.x, little-endian only)
 * transport that plugs into `structo::hypervisor::mmio_device_ref`.
 *
 * The transport owns the register file, feature negotiation, the status
 * state machine and one split-ring queue per queue index. It knows nothing
 * about the device type: a `Function` supplies the device ID, features,
 * config space and request processing (see `virtio_blk.hpp`).
 *
 * Ring and buffer memory live in one guest address space `GuestSpace`, accessed
 * through `Mem` (a `virtq_memory_traits<Mem, GuestSpace>` backend, e.g.
 * `virtq_memory_ref<GuestSpace>`). Packed rings are not wired up yet.
 *
 * Interrupts: the transport is poll-based. After each MMIO access (or
 * `try_kick`) call `irq_asserted()` and drive the interrupt line accordingly;
 * the guest clears causes through `InterruptACK`.
 *
 * ### Function requirements
 * @code
 * struct my_function {
 *   // Virtio device type (e.g. 2 = block).
 *   static constexpr std::uint32_t device_id = 2;
 *   // Number of virtqueues and the largest ring size offered for each.
 *   static constexpr std::uint32_t queue_count = 1;
 *   static constexpr std::uint32_t queue_max_size = 128;
 *   // Device-specific feature bits (VERSION_1 is added by the transport).
 *   std::uint64_t device_features() const noexcept;
 *   // Size of the device config space exposed at offset 0x100.
 *   std::size_t config_size() const noexcept;
 *   reloco::result<void> try_read_config(std::uint64_t offset, reloco::span<std::byte> dst) noexcept;
 *   // Drain queue @p qidx after a guest kick. An error makes the transport
 *   // raise DEVICE_NEEDS_RESET.
 *   template <typename Queue, typename Mem>
 *   reloco::result<void> process(Mem &mem, std::uint32_t qidx, Queue &q) noexcept;
 * };
 * @endcode
 */

#include "le_bytes.hpp"
#include "split_ring.hpp"
#include "virtq_memory.hpp"
#include "virtq_types.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <reloco/array.hpp>
#include <reloco/error.hpp>
#include <reloco/optional.hpp>
#include <reloco/span.hpp>
#include <structo/hypervisor/mmio_device_ref.hpp>

namespace structo::virtio {

/** @brief virtio-mmio register offsets and bit definitions. */
namespace mmio_reg {
inline constexpr std::uint64_t magic = 0x000;
inline constexpr std::uint64_t version = 0x004;
inline constexpr std::uint64_t device_id = 0x008;
inline constexpr std::uint64_t vendor_id = 0x00c;
inline constexpr std::uint64_t device_features = 0x010;
inline constexpr std::uint64_t device_features_sel = 0x014;
inline constexpr std::uint64_t driver_features = 0x020;
inline constexpr std::uint64_t driver_features_sel = 0x024;
inline constexpr std::uint64_t queue_sel = 0x030;
inline constexpr std::uint64_t queue_num_max = 0x034;
inline constexpr std::uint64_t queue_num = 0x038;
inline constexpr std::uint64_t queue_ready = 0x044;
inline constexpr std::uint64_t queue_notify = 0x050;
inline constexpr std::uint64_t interrupt_status = 0x060;
inline constexpr std::uint64_t interrupt_ack = 0x064;
inline constexpr std::uint64_t status = 0x070;
inline constexpr std::uint64_t queue_desc_low = 0x080;
inline constexpr std::uint64_t queue_desc_high = 0x084;
inline constexpr std::uint64_t queue_driver_low = 0x090;
inline constexpr std::uint64_t queue_driver_high = 0x094;
inline constexpr std::uint64_t queue_device_low = 0x0a0;
inline constexpr std::uint64_t queue_device_high = 0x0a4;
inline constexpr std::uint64_t config_generation = 0x0fc;
inline constexpr std::uint64_t config = 0x100;

inline constexpr std::uint32_t magic_value = 0x74726976; // "virt"
inline constexpr std::uint32_t version_value = 2;
inline constexpr std::uint32_t vendor_value = 0x554d4551; // arbitrary, "QEMU"-style vendor tag

inline constexpr std::uint32_t status_acknowledge = 1;
inline constexpr std::uint32_t status_driver = 2;
inline constexpr std::uint32_t status_driver_ok = 4;
inline constexpr std::uint32_t status_features_ok = 8;
inline constexpr std::uint32_t status_needs_reset = 0x40;
inline constexpr std::uint32_t status_failed = 0x80;

inline constexpr std::uint32_t irq_used_buffer = 1;
inline constexpr std::uint32_t irq_config_change = 2;
} // namespace mmio_reg

/**
 * @brief virtio-mmio v2 device transport.
 * @tparam GuestSpace Address space of guest ring and buffer addresses.
 * @tparam Mem Memory backend for `GuestSpace` (`virtq_memory_traits<Mem, GuestSpace>`).
 * @tparam Function Device function (see the file comment for requirements).
 * @tparam MaxQueues Capacity of the queue table; `Function::queue_count` must not exceed it.
 */
template <typename GuestSpace, typename Mem, typename Function, std::size_t MaxQueues = 4>
class virtio_mmio_device {
public:
  using queue = split_virtq_device<GuestSpace, GuestSpace, Mem>;
  using guest_addr = phys_addr<void, GuestSpace>;

  static_assert(Function::queue_count <= MaxQueues, "MaxQueues too small for Function::queue_count");
  static_assert(Function::queue_max_size >= 1, "queue_max_size must be non-zero");

  /** @brief Binds the transport. @p mem and @p fn must outlive it. */
  virtio_mmio_device(Mem &mem, Function &fn) noexcept : mem_(&mem), fn_(&fn) {}

  /** @brief Whether the interrupt line should be asserted. */
  [[nodiscard]] bool irq_asserted() const noexcept { return irq_status_ != 0; }

  /** @brief Window size: register file plus config space. */
  [[nodiscard]] std::size_t size() const noexcept { return mmio_reg::config + fn_->config_size(); }

  /** @brief Current status register (for tests/diagnostics). */
  [[nodiscard]] std::uint32_t status() const noexcept { return status_; }

  /** @brief Negotiated driver feature bits. */
  [[nodiscard]] std::uint64_t driver_features() const noexcept { return driver_features_; }

  /** @brief Signals a device config change to the guest (bumps ConfigGeneration, raises the config interrupt). */
  void notify_config_changed() noexcept {
    ++config_gen_;
    irq_status_ |= mmio_reg::irq_config_change;
  }

  /** @brief Drains queue @p qidx as if the guest had written QueueNotify. */
  [[nodiscard]] reloco::result<void> try_kick(std::uint32_t qidx) noexcept {
    if (qidx >= Function::queue_count || !queues_[qidx].q.has_value() || !(status_ & mmio_reg::status_driver_ok))
      return reloco::unexpected(reloco::error::invalid_state);
    auto &q = *queues_[qidx].q;
    auto r = fn_->process(*mem_, qidx, q);
    if (!r) {
      enter_needs_reset();
      return r;
    }
    auto irq = q.should_interrupt();
    if (!irq) {
      enter_needs_reset();
      return reloco::unexpected(irq.error());
    }
    if (*irq)
      irq_status_ |= mmio_reg::irq_used_buffer;
    return {};
  }

  /** @brief Resets the device to its power-on state (Status = 0). */
  void reset() noexcept {
    status_ = 0;
    driver_features_ = 0;
    device_sel_ = 0;
    driver_sel_ = 0;
    queue_sel_ = 0;
    irq_status_ = 0;
    for (auto &s : queues_)
      s = queue_state{};
  }

  [[nodiscard]] reloco::result<void> try_read(std::uint64_t offset, reloco::span<std::byte> dst) noexcept {
    if (offset >= mmio_reg::config)
      return fn_->try_read_config(offset - mmio_reg::config, dst);
    if (dst.size() != 4 || (offset & 3u) != 0)
      return reloco::unexpected(reloco::error::invalid_argument);
    store_le<std::uint32_t>(dst, read_reg(offset));
    return {};
  }

  [[nodiscard]] reloco::result<void> try_write(std::uint64_t offset, reloco::span<const std::byte> src) noexcept {
    if (offset >= mmio_reg::config)
      return reloco::unexpected(reloco::error::permission_denied); // config space is read-only here
    if (src.size() != 4 || (offset & 3u) != 0)
      return reloco::unexpected(reloco::error::invalid_argument);
    return write_reg(offset, load_le<std::uint32_t>(src));
  }

private:
  struct queue_state {
    std::uint32_t num = 0;
    bool ready = false;
    std::uint64_t desc = 0;
    std::uint64_t avail = 0;
    std::uint64_t used = 0;
    reloco::optional<queue> q;
  };

  [[nodiscard]] std::uint64_t offered() const noexcept {
    return fn_->device_features() | (std::uint64_t{1} << feature_version_1);
  }

  [[nodiscard]] queue_state *selected() noexcept {
    return queue_sel_ < Function::queue_count ? &queues_[queue_sel_] : nullptr;
  }

  [[nodiscard]] std::uint32_t read_reg(std::uint64_t offset) noexcept {
    switch (offset) {
    case mmio_reg::magic:
      return mmio_reg::magic_value;
    case mmio_reg::version:
      return mmio_reg::version_value;
    case mmio_reg::device_id:
      return Function::device_id;
    case mmio_reg::vendor_id:
      return mmio_reg::vendor_value;
    case mmio_reg::device_features:
      return device_sel_ == 0   ? static_cast<std::uint32_t>(offered())
             : device_sel_ == 1 ? static_cast<std::uint32_t>(offered() >> 32)
                                : 0u;
    case mmio_reg::queue_num_max:
      return selected() ? Function::queue_max_size : 0u;
    case mmio_reg::queue_ready: {
      auto *s = selected();
      return s && s->ready ? 1u : 0u;
    }
    case mmio_reg::interrupt_status:
      return irq_status_;
    case mmio_reg::status:
      return status_;
    case mmio_reg::config_generation:
      return config_gen_;
    default:
      return 0; // write-only or reserved registers read as zero
    }
  }

  [[nodiscard]] reloco::result<void> write_reg(std::uint64_t offset, std::uint32_t v) noexcept {
    auto *s = selected();
    switch (offset) {
    case mmio_reg::device_features_sel:
      device_sel_ = v;
      break;
    case mmio_reg::driver_features_sel:
      driver_sel_ = v;
      break;
    case mmio_reg::driver_features:
      // Features are frozen once the driver has asked for FEATURES_OK.
      if (status_ & mmio_reg::status_features_ok)
        break;
      if (driver_sel_ == 0)
        driver_features_ = (driver_features_ & ~std::uint64_t{0xffff'ffffu}) | v;
      else if (driver_sel_ == 1)
        driver_features_ = (driver_features_ & 0xffff'ffffu) | (std::uint64_t{v} << 32);
      break;
    case mmio_reg::queue_sel:
      queue_sel_ = v;
      break;
    case mmio_reg::queue_num:
      if (s && !s->ready)
        s->num = v;
      break;
    case mmio_reg::queue_desc_low:
      set_addr(s, &queue_state::desc, v, false);
      break;
    case mmio_reg::queue_desc_high:
      set_addr(s, &queue_state::desc, v, true);
      break;
    case mmio_reg::queue_driver_low:
      set_addr(s, &queue_state::avail, v, false);
      break;
    case mmio_reg::queue_driver_high:
      set_addr(s, &queue_state::avail, v, true);
      break;
    case mmio_reg::queue_device_low:
      set_addr(s, &queue_state::used, v, false);
      break;
    case mmio_reg::queue_device_high:
      set_addr(s, &queue_state::used, v, true);
      break;
    case mmio_reg::queue_ready:
      if (s)
        set_queue_ready(*s, v & 1u);
      break;
    case mmio_reg::queue_notify:
      // Notifications for unknown/inactive queues are ignored (guests may kick spuriously).
      if (v < Function::queue_count && queues_[v].q.has_value() && (status_ & mmio_reg::status_driver_ok))
        (void)try_kick(v);
      break;
    case mmio_reg::interrupt_ack:
      irq_status_ &= ~v;
      break;
    case mmio_reg::status:
      write_status(v);
      break;
    default:
      break; // read-only or reserved
    }
    return {};
  }

  static void set_addr(queue_state *s, std::uint64_t queue_state::*field, std::uint32_t v, bool high) noexcept {
    if (!s || s->ready)
      return;
    std::uint64_t &f = s->*field;
    f = high ? (f & 0xffff'ffffu) | (std::uint64_t{v} << 32) : (f & ~std::uint64_t{0xffff'ffffu}) | v;
  }

  void set_queue_ready(queue_state &s, std::uint32_t ready) noexcept {
    if (!ready) {
      s.ready = false;
      s.q.reset();
      return;
    }
    // Queues may only be brought up after FEATURES_OK and before DRIVER_OK.
    if (s.ready || !(status_ & mmio_reg::status_features_ok) || (status_ & mmio_reg::status_driver_ok))
      return;
    if (s.num == 0 || s.num > Function::queue_max_size)
      return;
    split_ring_addrs<GuestSpace> addrs{guest_addr{s.desc}, guest_addr{s.avail}, guest_addr{s.used}};
    const bool event_idx = has_feature(driver_features_, feature_ring_event_idx);
    auto q = queue::try_create(*mem_, addrs, s.num, event_idx);
    if (!q)
      return; // stays not-ready; the guest observes QueueReady == 0
    s.q = *q;
    s.ready = true;
  }

  void write_status(std::uint32_t v) noexcept {
    if (v == 0) {
      reset();
      return;
    }
    if ((v & mmio_reg::status_features_ok) && !(status_ & mmio_reg::status_features_ok)) {
      const bool ok = (driver_features_ & ~offered()) == 0 && has_feature(driver_features_, feature_version_1);
      if (!ok)
        v &= ~mmio_reg::status_features_ok; // the guest reads it back and sees the refusal
    }
    status_ = v | (status_ & mmio_reg::status_needs_reset);
  }

  void enter_needs_reset() noexcept {
    status_ |= mmio_reg::status_needs_reset;
    irq_status_ |= mmio_reg::irq_config_change;
  }

  Mem *mem_;
  Function *fn_;
  std::uint32_t status_ = 0;
  std::uint64_t driver_features_ = 0;
  std::uint32_t device_sel_ = 0;
  std::uint32_t driver_sel_ = 0;
  std::uint32_t queue_sel_ = 0;
  std::uint32_t irq_status_ = 0;
  std::uint32_t config_gen_ = 0;
  reloco::array<queue_state, MaxQueues> queues_{};
};

} // namespace structo::virtio

namespace structo::hypervisor {

/** @brief Plugs `virtio_mmio_device` into the generic emulated-MMIO handle. */
template <typename GuestSpace, typename Mem, typename Function, std::size_t MaxQueues>
struct mmio_device_traits<virtio::virtio_mmio_device<GuestSpace, Mem, Function, MaxQueues>> {
  using device = virtio::virtio_mmio_device<GuestSpace, Mem, Function, MaxQueues>;

  static std::size_t size(device &d) noexcept { return d.size(); }
  static reloco::result<void> try_read(device &d, std::uint64_t off, reloco::span<std::byte> dst) noexcept {
    return d.try_read(off, dst);
  }
  static reloco::result<void> try_write(device &d, std::uint64_t off, reloco::span<const std::byte> src) noexcept {
    return d.try_write(off, src);
  }
  static reloco::result<void> try_reset(device &d) noexcept {
    d.reset();
    return {};
  }
};

} // namespace structo::hypervisor
