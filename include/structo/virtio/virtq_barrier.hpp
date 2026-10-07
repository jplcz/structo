// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file virtq_barrier.hpp
 * @brief The memory-barrier policy virtqueue code is parameterized on, and
 * `smp_virtq_barriers`, the default for rings shared between CPUs (a
 * software device, a VMM emulating a device, or a test).
 *
 * Every ordering requirement of the split/packed ring protocol funnels through
 * three operations, so a driver for *real* hardware (which needs device-ordering
 * barriers -- `VIRTIO_F_ORDER_PLATFORM`, `dma_wmb()`-style) supplies its own
 * policy instead of touching the ring code:
 *
 * @code
 * struct my_io_barriers {
 *   static void wmb() noexcept; // prior stores visible before any later store
 *   static void rmb() noexcept; // prior loads complete before any later load/store
 *   static void mb() noexcept;  // full barrier: stores visible before subsequent loads
 * };
 * @endcode
 *
 * | Side | Where it is used | Operation |
 * |---|---|---|
 * | driver | descriptors + avail ring entry written -> publish `avail.idx` | `wmb` |
 * | driver | publish avail -> read `used.flags` (notify decision) | `mb` |
 * | driver | read `used.idx` -> read used elements | `rmb` |
 * | device | read `avail.idx` -> read avail entry/descriptors | `rmb` |
 * | device | write used element -> publish `used.idx` | `wmb` |
 * | device | publish used -> read `avail.flags` (interrupt decision) | `mb` |
 */

#include <atomic>

namespace structo::virtio {

/** @brief Barriers for rings shared between CPUs (SMP ordering, not device-MMIO ordering). */
struct smp_virtq_barriers {
  static void wmb() noexcept { std::atomic_thread_fence(std::memory_order_release); }
  static void rmb() noexcept { std::atomic_thread_fence(std::memory_order_acquire); }
  static void mb() noexcept { std::atomic_thread_fence(std::memory_order_seq_cst); }
};

} // namespace structo::virtio
