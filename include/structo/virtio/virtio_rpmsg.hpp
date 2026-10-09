// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file virtio_rpmsg.hpp
 * @brief VIRTIO rpmsg device function (device ID 7), hypervisor/device side, for
 * `virtio_mmio_device` (or any transport with the same `Function` contract).
 *
 * The guest runs Linux's `virtio_rpmsg_bus` (or an OpenAMP/FreeBSD equivalent) as
 * the *driver*; this class is the *remote* processor. Two queues:
 *  - queue 0 (guest rx): the guest posts empty buffers, this device fills them
 *    with hypervisor-to-guest messages;
 *  - queue 1 (guest tx): the guest posts filled buffers, this device dispatches
 *    them to bound endpoints.
 *
 * Every buffer holds `{le32 src, le32 dst, le32 reserved, le16 len, le16 flags}`
 * followed by `len` payload bytes. With the NS feature, endpoint 53 carries
 * name-service messages `{char name[32], le32 addr, le32 flags}`.
 *
 * Safety: the header is copied out once and `len` is validated against both the
 * buffer and `MaxPayload`; the payload is copied into a device-owned buffer
 * before a handler sees it (no TOCTOU); malformed or unroutable messages are
 * counted and dropped, never trusted. Sending never touches guest memory: a
 * message is staged in a bounded queue and delivered when queue 0 is serviced.
 *
 * ### Using it
 * @code
 * using rpmsg_t = virtio_rpmsg_function<guest_space>;
 * rpmsg_t rpmsg;
 * virtio_mmio_device<guest_space, mem_t, rpmsg_t> dev(mem, rpmsg);
 *
 * // Receive: handlers run from dev.try_kick(1) (the guest's tx queue).
 * (void)rpmsg.try_bind(0x400, [](void *ctx, std::uint32_t src, std::uint32_t dst,
 *                               reloco::span<const std::byte> payload) noexcept { ... }, &my_state);
 *
 * // Send: stage, then drain queue 0 (and raise the guest interrupt, see below).
 * (void)rpmsg.try_announce("my-service", 0x400);        // name-service "create" for the guest
 * (void)rpmsg.try_send(0x400, 0x401, payload);
 * (void)dev.try_kick(0);                                // delivers staged messages into guest rx buffers
 * @endcode
 * Replies staged by a handler (during `try_kick(1)`) go out on the next
 * `try_kick(0)`; check `pending()` after `try_kick(1)`. `try_kick(0)` also runs
 * when the guest posts new rx buffers, so a message that found none is delivered
 * as soon as buffers arrive.
 */

#include "le_bytes.hpp"
#include "virtq_chain.hpp"
#include "virtq_types.hpp"

#include <cstddef>
#include <cstdint>
#include <reloco/array.hpp>
#include <reloco/error.hpp>
#include <reloco/span.hpp>
#include <reloco/string_view.hpp>

namespace structo::virtio {

namespace rpmsg {
inline constexpr std::uint32_t device_id = 7;
inline constexpr unsigned feature_ns = 0;              ///< The device announces/receives services via endpoint 53.
inline constexpr std::uint32_t ns_addr = 53;           ///< Well-known name-service endpoint.
inline constexpr std::uint32_t addr_any = 0xFFFFFFFFu; ///< "No endpoint"; never bindable.
inline constexpr std::size_t header_size = 16;
inline constexpr std::size_t name_size = 32;
inline constexpr std::size_t ns_msg_size = 40;
inline constexpr std::uint32_t ns_create = 0;
inline constexpr std::uint32_t ns_destroy = 1;
inline constexpr std::size_t default_buffer_size = 512; ///< Linux's rpmsg buffer size (header + payload).

/** @brief Decoded rpmsg message header. */
struct header {
  std::uint32_t src = 0;
  std::uint32_t dst = 0;
  std::uint16_t len = 0;
  std::uint16_t flags = 0;
};

/** @brief Encodes @p h into the first `header_size` bytes of @p dst (must be at least that long). */
inline void encode_header(reloco::span<std::byte> dst, const header &h) noexcept {
  store_le<std::uint32_t>(dst, h.src);
  store_le<std::uint32_t>(dst.subspan(4), h.dst);
  store_le<std::uint32_t>(dst.subspan(8), 0);
  store_le<std::uint16_t>(dst.subspan(12), h.len);
  store_le<std::uint16_t>(dst.subspan(14), h.flags);
}

/** @brief Decodes a header from at least `header_size` bytes. */
[[nodiscard]] inline header decode_header(reloco::span<const std::byte> src) noexcept {
  header h;
  h.src = load_le<std::uint32_t>(src);
  h.dst = load_le<std::uint32_t>(src.subspan(4));
  h.len = load_le<std::uint16_t>(src.subspan(12));
  h.flags = load_le<std::uint16_t>(src.subspan(14));
  return h;
}

/** @brief Decoded name-service message payload. */
struct ns_msg {
  reloco::array<char, name_size> name{}; ///< NUL-terminated (the last byte is forced to NUL on decode).
  std::uint32_t addr = 0;
  std::uint32_t flags = 0; ///< `ns_create` or `ns_destroy`.
};

/** @brief Encodes @p m into `ns_msg_size` bytes of @p dst. */
inline void encode_ns(reloco::span<std::byte> dst, const ns_msg &m) noexcept {
  for (std::size_t i = 0; i < name_size; ++i)
    dst[i] = static_cast<std::byte>(m.name[i]);
  store_le<std::uint32_t>(dst.subspan(32), m.addr);
  store_le<std::uint32_t>(dst.subspan(36), m.flags);
}

/** @brief Decodes a name-service payload; `error::out_of_range` if @p src is shorter than `ns_msg_size`. */
[[nodiscard]] inline reloco::result<ns_msg> try_decode_ns(reloco::span<const std::byte> src) noexcept {
  if (src.size() < ns_msg_size)
    return reloco::unexpected(reloco::error::out_of_range);
  ns_msg m;
  for (std::size_t i = 0; i < name_size; ++i)
    m.name[i] = static_cast<char>(src[i]);
  m.name[name_size - 1] = '\0';
  m.addr = load_le<std::uint32_t>(src.subspan(32));
  m.flags = load_le<std::uint32_t>(src.subspan(36));
  return m;
}

/** @brief Endpoint receive callback (see `virtio_rpmsg_function::try_bind`). @p payload is valid only during the call.
 */
using rx_fn = void (*)(void *ctx, std::uint32_t src, std::uint32_t dst, reloco::span<const std::byte> payload) noexcept;

/** @brief Diagnostic counters. */
struct stats {
  std::uint64_t rx_delivered = 0; ///< Guest messages dispatched to a bound endpoint.
  std::uint64_t rx_unrouted = 0;  ///< Guest messages for an address with no endpoint.
  std::uint64_t rx_malformed = 0; ///< Short buffer, `len` beyond the buffer, or unreadable guest memory.
  std::uint64_t rx_oversize = 0;  ///< Payload larger than `MaxPayload`.
  std::uint64_t tx_delivered = 0; ///< Messages written into a guest rx buffer.
  std::uint64_t tx_lost = 0;      ///< Staged messages dropped (guest buffer too small or unwritable).
  std::uint64_t tx_full = 0;      ///< `try_send` refused because the staging queue was full.
};
} // namespace rpmsg

/**
 * @brief rpmsg device function.
 * @tparam GuestSpace Address space of the guest's ring/buffer addresses (must match the transport's).
 * @tparam MaxEndpoints Capacity of the endpoint table.
 * @tparam MaxPayload Largest payload accepted or sent (default: Linux's 512-byte buffer minus the header).
 * @tparam PendingDepth Capacity of the staged hypervisor-to-guest queue.
 */
template <typename GuestSpace, std::size_t MaxEndpoints = 8,
          std::size_t MaxPayload = rpmsg::default_buffer_size - rpmsg::header_size, std::size_t PendingDepth = 8>
class virtio_rpmsg_function {
public:
  static constexpr std::uint32_t device_id = rpmsg::device_id;
  static constexpr std::uint32_t queue_count = 2;
  static constexpr std::uint32_t queue_max_size = 512;

  static_assert(MaxEndpoints >= 1 && PendingDepth >= 1, "need at least one endpoint and one staging slot");
  static_assert(MaxPayload >= rpmsg::ns_msg_size && MaxPayload <= 0xFFFFu,
                "payload must fit the name-service message and le16");

  /** @param offer_ns Offer `VIRTIO_RPMSG_F_NS`: the guest then sends and expects name-service messages. */
  explicit virtio_rpmsg_function(bool offer_ns = true) noexcept : offer_ns_(offer_ns) {}

  [[nodiscard]] std::uint64_t device_features() const noexcept {
    return offer_ns_ ? std::uint64_t{1} << rpmsg::feature_ns : 0;
  }

  [[nodiscard]] std::size_t config_size() const noexcept { return 0; }

  [[nodiscard]] reloco::result<void> try_read_config(std::uint64_t offset, reloco::span<std::byte> dst) noexcept {
    if (offset != 0 || !dst.empty())
      return reloco::unexpected(reloco::error::out_of_range);
    return {};
  }

  /**
   * @brief Routes guest messages sent to @p addr to @p fn.
   * Endpoint 53 may be bound to receive the guest's name-service messages (`rpmsg::try_decode_ns`).
   * `already_exists` if bound, `capacity_exceeded` if the table is full, `invalid_argument` for `addr_any`/null @p fn.
   */
  [[nodiscard]] reloco::result<void> try_bind(std::uint32_t addr, rpmsg::rx_fn fn, void *ctx = nullptr) noexcept {
    if (addr == rpmsg::addr_any || fn == nullptr)
      return reloco::unexpected(reloco::error::invalid_argument);
    for (const auto &e : eps_)
      if (e.fn != nullptr && e.addr == addr)
        return reloco::unexpected(reloco::error::already_exists);
    for (auto &e : eps_) {
      if (e.fn == nullptr) {
        e = endpoint{addr, fn, ctx};
        return {};
      }
    }
    return reloco::unexpected(reloco::error::capacity_exceeded);
  }

  /** @brief Removes the endpoint at @p addr; `not_found` if none. */
  [[nodiscard]] reloco::result<void> try_unbind(std::uint32_t addr) noexcept {
    for (auto &e : eps_) {
      if (e.fn != nullptr && e.addr == addr) {
        e = endpoint{};
        return {};
      }
    }
    return reloco::unexpected(reloco::error::not_found);
  }

  /**
   * @brief Stages a hypervisor-to-guest message; delivered by the next queue-0 service.
   * `invalid_argument` if @p payload exceeds `MaxPayload`; `try_again` if the staging queue is full.
   */
  [[nodiscard]] reloco::result<void> try_send(std::uint32_t src, std::uint32_t dst,
                                              reloco::span<const std::byte> payload) noexcept {
    if (payload.size() > MaxPayload)
      return reloco::unexpected(reloco::error::invalid_argument);
    if (count_ == PendingDepth) {
      ++stats_.tx_full;
      return reloco::unexpected(reloco::error::try_again);
    }
    auto &slot = pending_[(head_ + count_) % PendingDepth];
    slot.src = src;
    slot.dst = dst;
    slot.len = payload.size();
    for (std::size_t i = 0; i < payload.size(); ++i)
      slot.data[i] = payload[i];
    ++count_;
    return {};
  }

  /**
   * @brief Stages a name-service message telling the guest that service @p name is available (or gone) at @p addr.
   * The guest creates (or removes) a channel named @p name bound to @p addr. Requires the NS feature to have been
   * negotiated for the guest to act on it; the device does not check. `invalid_argument` if @p name does not fit
   * in `name_size - 1` bytes.
   */
  [[nodiscard]] reloco::result<void> try_announce(reloco::string_view name, std::uint32_t addr,
                                                  bool create = true) noexcept {
    if (name.size() >= rpmsg::name_size)
      return reloco::unexpected(reloco::error::invalid_argument);
    rpmsg::ns_msg m;
    for (std::size_t i = 0; i < name.size(); ++i)
      m.name[i] = name[i];
    m.addr = addr;
    m.flags = create ? rpmsg::ns_create : rpmsg::ns_destroy;
    reloco::array<std::byte, rpmsg::ns_msg_size> raw{};
    rpmsg::encode_ns(raw.as_span(), m);
    return try_send(addr, rpmsg::ns_addr, reloco::span<const std::byte>(raw.data(), raw.size()));
  }

  /** @brief Staged messages not yet delivered; non-zero after `try_kick(1)` means `try_kick(0)` has work. */
  [[nodiscard]] std::size_t pending() const noexcept { return count_; }

  [[nodiscard]] const rpmsg::stats &stats() const noexcept { return stats_; }

  /** @brief Services queue @p qidx (0 = deliver staged messages, 1 = dispatch guest messages). */
  template <typename QueueView, typename Mem>
  [[nodiscard]] reloco::result<void> process(Mem &mem, std::uint32_t qidx, QueueView &q) noexcept {
    return qidx == 0 ? flush(mem, q) : receive(mem, q);
  }

private:
  struct endpoint {
    std::uint32_t addr = 0;
    rpmsg::rx_fn fn = nullptr;
    void *ctx = nullptr;
  };
  struct staged {
    std::uint32_t src = 0;
    std::uint32_t dst = 0;
    std::size_t len = 0;
    reloco::array<std::byte, MaxPayload> data{};
  };

  template <typename Mem, typename QueueView> reloco::result<void> flush(Mem &mem, QueueView &q) noexcept {
    while (count_ != 0) {
      auto popped = q.try_pop(reloco::span<typename QueueView::segment>(segs_.data(), segs_.size()));
      if (!popped)
        return reloco::unexpected(popped.error());
      if (!popped->has_value())
        return {}; // no rx buffers posted yet: keep the rest staged
      const auto &c = **popped;
      const auto &m = pending_[head_];

      std::uint32_t written = 0;
      if (c.writable_bytes >= rpmsg::header_size + m.len) {
        rpmsg::header h;
        h.src = m.src;
        h.dst = m.dst;
        h.len = static_cast<std::uint16_t>(m.len);
        rpmsg::encode_header(tx_buf_.as_span(), h);
        for (std::size_t i = 0; i < m.len; ++i)
          tx_buf_[rpmsg::header_size + i] = m.data[i];
        const std::size_t total = rpmsg::header_size + m.len;
        if (try_write_chain(mem, c.writable, 0, reloco::span<const std::byte>(tx_buf_.data(), total))) {
          written = static_cast<std::uint32_t>(total);
          ++stats_.tx_delivered;
        } else {
          ++stats_.tx_lost;
        }
      } else {
        ++stats_.tx_lost; // guest buffer too small for this message
      }
      head_ = (head_ + 1) % PendingDepth;
      --count_;
      if (auto r = q.try_push_used(c, written); !r)
        return r;
    }
    return {};
  }

  template <typename Mem, typename QueueView> reloco::result<void> receive(Mem &mem, QueueView &q) noexcept {
    for (;;) {
      auto popped = q.try_pop(reloco::span<typename QueueView::segment>(segs_.data(), segs_.size()));
      if (!popped)
        return reloco::unexpected(popped.error());
      if (!popped->has_value())
        return {};
      const auto &c = **popped;
      handle(mem, c);
      if (auto r = q.try_push_used(c, 0); !r)
        return r;
    }
  }

  template <typename Mem, typename Chain> void handle(Mem &mem, const Chain &c) noexcept {
    if (c.readable_bytes < rpmsg::header_size ||
        !try_read_chain(mem, c.readable, 0, reloco::span<std::byte>(rx_hdr_.data(), rx_hdr_.size()))) {
      ++stats_.rx_malformed;
      return;
    }
    const auto h = rpmsg::decode_header(reloco::span<const std::byte>(rx_hdr_.data(), rx_hdr_.size()));
    if (h.len > c.readable_bytes - rpmsg::header_size) {
      ++stats_.rx_malformed;
      return;
    }
    if (h.len > MaxPayload) {
      ++stats_.rx_oversize;
      return;
    }
    if (h.len != 0 && !try_read_chain(mem, c.readable, rpmsg::header_size,
                                      reloco::span<std::byte>(rx_buf_.data(), std::size_t{h.len}))) {
      ++stats_.rx_malformed;
      return;
    }
    for (const auto &e : eps_) {
      if (e.fn != nullptr && e.addr == h.dst) {
        ++stats_.rx_delivered;
        e.fn(e.ctx, h.src, h.dst, reloco::span<const std::byte>(rx_buf_.data(), std::size_t{h.len}));
        return;
      }
    }
    ++stats_.rx_unrouted;
  }

  bool offer_ns_;
  reloco::array<endpoint, MaxEndpoints> eps_{};
  reloco::array<staged, PendingDepth> pending_{};
  std::size_t head_ = 0;
  std::size_t count_ = 0;
  rpmsg::stats stats_{};
  reloco::array<chain_segment<GuestSpace>, 16> segs_{};
  reloco::array<std::byte, rpmsg::header_size> rx_hdr_{};
  reloco::array<std::byte, MaxPayload> rx_buf_{};
  reloco::array<std::byte, rpmsg::header_size + MaxPayload> tx_buf_{};
};

} // namespace structo::virtio
