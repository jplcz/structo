// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file virtio_balloon.hpp
 * @brief VIRTIO memory balloon device function (device ID 5) for `virtio_mmio_device`.
 *
 * The hypervisor asks the guest to give memory back by setting a *target* (`set_target`,
 * in 4 KiB pages) and raising the config-change interrupt. The guest *inflates* the
 * balloon by sending the page frame numbers it freed on queue 0; the device passes the
 * pages to a `Host` to reclaim (unmap, `madvise(DONTNEED)`, hand to another VM, ...).
 * The guest *deflates* by sending frame numbers on queue 1 when it wants pages back; the
 * device passes them to `Host::try_restore`.
 *
 * Two queues, no features. Every message is a readable array of little-endian 32-bit
 * page frame numbers; a frame number is always in 4 KiB units whatever the guest's own
 * page size is, so guest address = `pfn << 12`.
 *
 * Config space: `num_pages` (the target) and `actual`. The specification has the guest
 * write `actual`, but the transport's config space is read-only, so the device keeps its
 * own count (pages inflated minus pages deflated) and reports that.
 *
 * ### Safety
 * A frame number at or beyond `guest_pages` (the guest's RAM size in pages), or a message
 * that is not a whole number of 32-bit entries, is dropped and counted. Contiguous
 * frames are coalesced so the `Host` is called once per run. The device never
 * dereferences a ballooned page: reclaiming is the host's business, and a guest that
 * touches a ballooned page only hurts itself.
 *
 * ### Host requirements
 * @code
 * struct my_host {
 *   // Take @p count contiguous 4 KiB pages starting at @p first away from the guest.
 *   reloco::result<void> try_reclaim(structo::phys_addr<void, GuestSpace> first, std::uint32_t count) noexcept;
 *   // Give them back (the guest deflated).
 *   reloco::result<void> try_restore(structo::phys_addr<void, GuestSpace> first, std::uint32_t count) noexcept;
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
#include <structo/phys_addr.hpp>

namespace structo::virtio {

namespace balloon {
inline constexpr std::uint32_t device_id = 5;
inline constexpr std::size_t config_size = 8; ///< num_pages(4) actual(4)
inline constexpr std::uint32_t page_shift = 12;
inline constexpr std::size_t max_pfns_per_message = 1024; ///< Larger messages are dropped.

/** @brief Diagnostic counters. */
struct stats {
  std::uint64_t inflate_pages = 0; ///< Pages passed to `try_reclaim`.
  std::uint64_t deflate_pages = 0; ///< Pages passed to `try_restore`.
  std::uint64_t bad_messages = 0;  ///< Odd length, oversize, unreadable or out-of-range frames.
  std::uint64_t host_errors = 0;   ///< `Host` calls that failed.
};
} // namespace balloon

/**
 * @brief Balloon function over a `Host`.
 * @tparam GuestSpace Address space of the guest's ring/buffer addresses (must match the transport's).
 * @tparam Host Page reclaim/restore backend (see the file comment).
 */
template <typename GuestSpace, typename Host> class virtio_balloon_function {
public:
  static constexpr std::uint32_t device_id = balloon::device_id;
  static constexpr std::uint32_t queue_count = 2;
  static constexpr std::uint32_t queue_max_size = 128;

  /** @param guest_pages Guest RAM size in 4 KiB pages; frame numbers at or beyond it are refused. */
  virtio_balloon_function(Host &host, std::uint64_t guest_pages) noexcept : host_(&host), guest_pages_(guest_pages) {}

  [[nodiscard]] std::uint64_t device_features() const noexcept { return 0; }
  [[nodiscard]] std::size_t config_size() const noexcept { return balloon::config_size; }

  [[nodiscard]] reloco::result<void> try_read_config(std::uint64_t offset, reloco::span<std::byte> dst) noexcept {
    if (offset > balloon::config_size || dst.size() > balloon::config_size - offset)
      return reloco::unexpected(reloco::error::out_of_range);
    reloco::array<std::byte, balloon::config_size> raw{};
    auto rs = raw.as_span();
    store_le<std::uint32_t>(rs, target_);
    store_le<std::uint32_t>(rs.subspan(4), static_cast<std::uint32_t>(inflated_));
    for (std::size_t i = 0; i < dst.size(); ++i)
      dst[i] = raw[static_cast<std::size_t>(offset) + i];
    return {};
  }

  /** @brief Sets the number of pages the guest should hold in the balloon; follow with `notify_config_changed()`. */
  void set_target(std::uint32_t pages) noexcept { target_ = pages; }

  [[nodiscard]] std::uint32_t target() const noexcept { return target_; }

  /** @brief Pages currently in the balloon (inflated minus deflated). */
  [[nodiscard]] std::uint64_t inflated() const noexcept { return inflated_; }

  [[nodiscard]] const balloon::stats &stats() const noexcept { return stats_; }

  /** @brief Services queue @p qidx (0 = inflate, 1 = deflate). */
  template <typename QueueView, typename Mem>
  [[nodiscard]] reloco::result<void> process(Mem &mem, std::uint32_t qidx, QueueView &q) noexcept {
    const bool inflate = qidx == 0;
    for (;;) {
      auto popped = q.try_pop(reloco::span<typename QueueView::segment>(segs_.data(), segs_.size()));
      if (!popped)
        return reloco::unexpected(popped.error());
      if (!popped->has_value())
        return {};
      const auto &c = **popped;
      handle(mem, c, inflate);
      if (auto r = q.try_push_used(c, 0); !r)
        return r;
    }
  }

private:
  template <typename Mem, typename Chain> void handle(Mem &mem, const Chain &c, bool inflate) noexcept {
    if (c.readable_bytes % 4 != 0 || c.readable_bytes > balloon::max_pfns_per_message * 4) {
      ++stats_.bad_messages;
      return;
    }
    const std::size_t total = static_cast<std::size_t>(c.readable_bytes / 4);
    std::size_t run_start = 0; // first frame of the current contiguous run (valid while run_len != 0)
    std::uint32_t run_len = 0;
    std::uint64_t prev = 0;
    for (std::size_t i = 0; i < total; ++i) {
      reloco::array<std::byte, 4> raw{};
      if (!try_read_chain(mem, c.readable, i * 4, reloco::span<std::byte>(raw.data(), 4))) {
        ++stats_.bad_messages;
        break;
      }
      const std::uint64_t pfn = load_le<std::uint32_t>(reloco::span<const std::byte>(raw.data(), 4));
      if (pfn >= guest_pages_) {
        ++stats_.bad_messages;
        flush_run(run_start, run_len, inflate);
        run_len = 0;
        continue;
      }
      if (run_len != 0 && pfn == prev + 1) {
        ++run_len;
      } else {
        flush_run(run_start, run_len, inflate);
        run_start = static_cast<std::size_t>(pfn);
        run_len = 1;
      }
      prev = pfn;
    }
    flush_run(run_start, run_len, inflate);
  }

  void flush_run(std::size_t first_pfn, std::uint32_t count, bool inflate) noexcept {
    if (count == 0)
      return;
    const phys_addr<void, GuestSpace> at{static_cast<std::uint64_t>(first_pfn) << balloon::page_shift};
    if (inflate) {
      if (host_->try_reclaim(at, count)) {
        inflated_ += count;
        stats_.inflate_pages += count;
      } else {
        ++stats_.host_errors;
      }
    } else {
      const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::uint64_t>(count, inflated_));
      if (n != count)
        ++stats_.bad_messages; // deflating more than was inflated
      if (n != 0 && host_->try_restore(at, n)) {
        inflated_ -= n;
        stats_.deflate_pages += n;
      } else if (n != 0) {
        ++stats_.host_errors;
      }
    }
  }

  Host *host_;
  std::uint64_t guest_pages_;
  std::uint32_t target_ = 0;
  std::uint64_t inflated_ = 0;
  balloon::stats stats_{};
  reloco::array<chain_segment<GuestSpace>, queue_max_size> segs_{};
};

} // namespace structo::virtio
