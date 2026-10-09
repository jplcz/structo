// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file usb_hcd_env.hpp
 * @brief Platform layer shared by the generic USB host controller drivers (`ohci_hcd`, `ehci_hcd`,
 * `xhci_hcd`): the `Env` contract a board provides (MMIO registers, coherent DMA memory, a millisecond
 * delay), a RAII DMA block, and a fixed-size table that maps in-flight `usb_transfer`s to driver state.
 * C++20 only (empty otherwise).
 *
 * The controller drivers know *nothing* about the SoC: no addresses, no cache maintenance, no clocks and
 * no interrupt controller. Everything board specific goes through one `Env` class:
 *
 * @code
 * struct my_usb_env {
 *   // Reads/writes a 32-bit controller register. `offset` is a byte offset from the start of the
 *   // controller's MMIO window (for xHCI/EHCI: from the first capability register). The implementation
 *   // does the volatile access and any bus/endianness fix-up the SoC needs.
 *   std::uint32_t read32(std::size_t offset) noexcept;
 *   void write32(std::size_t offset, std::uint32_t value) noexcept;
 *
 *   // Allocates `size` bytes of ZEROED memory that the controller can DMA to and from, aligned to `align`
 *   // (a power of two, up to 4096) and physically contiguous. The memory must be coherent (uncached, or
 *   // the platform keeps it coherent): the drivers do not do cache maintenance. For OHCI/EHCI without
 *   // 64-bit support the memory must lie below 4 GiB. `phys` is the address the *controller* uses (the
 *   // bus address, after any IOMMU/offset). On failure return an empty buffer (virt == nullptr).
 *   structo::hw::usb_dma_buffer dma_alloc(std::size_t size, std::size_t align) noexcept;
 *   void dma_free(const structo::hw::usb_dma_buffer &buf) noexcept;
 *
 *   // Full memory barrier: called after descriptors are written and before the controller is told to
 *   // look at them (ring a doorbell, set a list pointer, ...), and after reading a completion flag
 *   // before reading the descriptor it covers.
 *   void barrier() noexcept;
 *
 *   // Waits `ms` milliseconds without blocking the CPU (used for the >= 50 ms port reset). Typically
 *   // `return sched.sleep_for(ms)` wrapped in a task, or a timer-interrupt driven awaitable.
 *   reloco::task<void> delay_ms(unsigned ms) noexcept;
 * };
 * @endcode
 *
 * Driver conventions (all three controllers):
 * - `usb_host_traits<Driver>` is specialized, so `usb_host_controller_ref{driver}` works.
 * - `void irq() noexcept` (alias `poll()`): call it from the controller's interrupt handler *or*
 *   periodically from a polled loop (e.g. the scheduler's idle hook). It acknowledges the controller,
 *   completes finished transfers (`usb_transfer::complete`, which may resume a coroutine inline and may
 *   re-enter `submit()`/`cancel()`: update all driver state *before* completing) and reports
 *   port changes through `usb_port_status::changed`.
 * - `reloco::result<void> start() noexcept`: resets and starts the controller; call once before use.
 * - Register waits are bounded spin loops, never unbounded.
 * - The driver allocates its schedule memory with `dma_alloc` once in `start()` and each transfer's bounce
 *   buffer at `submit()`; the caller's `usb_transfer::data` need not be DMA-capable.
 */

#include <reloco/coroutine.hpp>
#include <reloco/detail/compat.hpp>

#if RELOCO_HAS_COROUTINES

#include <reloco/array.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/span.hpp>

#include "usb_host_controller_ref.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

namespace structo::hw {

/** @brief A block of DMA-able memory: CPU pointer + the address the controller uses. */
struct usb_dma_buffer {
  void *virt = nullptr;
  std::uint64_t phys = 0;
  std::size_t size = 0;

  [[nodiscard]] explicit operator bool() const noexcept { return virt != nullptr; }
};

namespace detail {

template <typename Env, typename = void> struct is_usb_hcd_env : std::false_type {};
template <typename Env>
struct is_usb_hcd_env<
    Env, std::void_t<decltype(std::declval<Env &>().read32(std::size_t{})),
                     decltype(std::declval<Env &>().write32(std::size_t{}, std::uint32_t{})),
                     decltype(std::declval<Env &>().dma_alloc(std::size_t{}, std::size_t{})),
                     decltype(std::declval<Env &>().dma_free(std::declval<const usb_dma_buffer &>())),
                     decltype(std::declval<Env &>().barrier()), decltype(std::declval<Env &>().delay_ms(0u))>>
    : std::true_type {};

} // namespace detail

/** @brief True if `Env` provides everything the generic USB host controller drivers need. */
template <typename Env> inline constexpr bool is_usb_hcd_env_v = detail::is_usb_hcd_env<Env>::value;

/** @brief Owning handle to zeroed DMA memory obtained from an `Env`; freed on destruction. */
template <typename Env> class usb_dma_block {
public:
  usb_dma_block() noexcept = default;
  usb_dma_block(const usb_dma_block &) = delete;
  usb_dma_block &operator=(const usb_dma_block &) = delete;
  usb_dma_block(usb_dma_block &&o) noexcept : env_(o.env_), buf_(o.buf_) { o.buf_ = {}; }
  usb_dma_block &operator=(usb_dma_block &&o) noexcept {
    if (this != &o) {
      reset();
      env_ = o.env_;
      buf_ = o.buf_;
      o.buf_ = {};
    }
    return *this;
  }
  ~usb_dma_block() { reset(); }

  /** @brief Allocates `size` zeroed bytes aligned to `align`; false on failure (the block stays empty). */
  [[nodiscard]] bool allocate(Env &env, std::size_t size, std::size_t align) noexcept {
    reset();
    usb_dma_buffer b = env.dma_alloc(size, align);
    if (!b.virt)
      return false;
    b.size = size;
    const reloco::span<std::uint8_t> all(static_cast<std::uint8_t *>(b.virt), size);
    for (std::size_t i = 0; i < size; ++i)
      all[i] = 0;
    env_ = &env;
    buf_ = b;
    return true;
  }

  void reset() noexcept {
    if (buf_.virt && env_)
      env_->dma_free(buf_);
    buf_ = {};
  }

  [[nodiscard]] explicit operator bool() const noexcept { return buf_.virt != nullptr; }
  [[nodiscard]] void *virt() const noexcept { return buf_.virt; }
  /** @brief The whole block as a byte span (empty if unallocated). */
  [[nodiscard]] reloco::span<std::uint8_t> as_span() const noexcept {
    return reloco::span<std::uint8_t>(static_cast<std::uint8_t *>(buf_.virt), buf_.size);
  }
  /** @brief Copies `src` into the block at `offset` (bounds-checked). */
  void copy_in(std::size_t offset, reloco::span<const std::uint8_t> src) const noexcept {
    if (src.empty())
      return;
    const reloco::span<std::uint8_t> whole = as_span();
    const reloco::span<std::uint8_t> dst = whole.subspan(offset, src.size());
    for (std::size_t i = 0; i < src.size(); ++i)
      dst[i] = src[i];
  }
  /** @brief Copies `dst.size()` bytes starting at `offset` out of the block (bounds-checked). */
  void copy_out(std::size_t offset, reloco::span<std::uint8_t> dst) const noexcept {
    if (dst.empty())
      return;
    const reloco::span<std::uint8_t> whole = as_span();
    const reloco::span<std::uint8_t> src = whole.subspan(offset, dst.size());
    for (std::size_t i = 0; i < dst.size(); ++i)
      dst[i] = src[i];
  }
  [[nodiscard]] std::uint64_t phys() const noexcept { return buf_.phys; }
  [[nodiscard]] std::size_t size() const noexcept { return buf_.size; }
  /** @brief Bus address of byte `offset` into the block. */
  [[nodiscard]] std::uint64_t phys_at(std::size_t offset) const noexcept { return buf_.phys + offset; }
  /** @brief Typed view of the hardware structure at byte `offset` (bounds-checked; `offset` must be aligned for T). */
  template <typename T> [[nodiscard]] T *at(std::size_t offset = 0) const noexcept {
    const reloco::span<std::uint8_t> whole = as_span();
    (void)whole.subspan(offset, sizeof(T)); // the bounds check; the address is formed as an integer
    return reinterpret_cast<T *>(reinterpret_cast<std::uintptr_t>(buf_.virt) + offset);
  }

private:
  Env *env_ = nullptr;
  usb_dma_buffer buf_{};
};

/**
 * @brief Fixed-capacity table associating each in-flight `usb_transfer` with driver state `State`
 * (default-constructible). `acquire` fails (returns nullptr) when all `N` slots are busy; the driver then
 * returns `error::try_again`-like failure from `submit()`.
 */
template <typename State, std::size_t N> class usb_transfer_table {
public:
  struct slot {
    usb_transfer *xfer = nullptr; ///< nullptr = free.
    State st{};
  };

  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return N; }

  /** @brief Claims a free slot for `t` with default-initialized state; nullptr if the table is full. */
  [[nodiscard]] slot *acquire(usb_transfer &t) noexcept {
    for (auto &s : slots_) {
      if (!s.xfer) {
        s.xfer = &t;
        s.st = State{};
        return &s;
      }
    }
    return nullptr;
  }

  [[nodiscard]] slot *find(const usb_transfer &t) noexcept {
    for (auto &s : slots_)
      if (s.xfer == &t)
        return &s;
    return nullptr;
  }

  /** @brief Frees the slot (the state is reset). */
  void release(slot &s) noexcept {
    s.xfer = nullptr;
    s.st = State{};
  }

  [[nodiscard]] slot &operator[](std::size_t i) noexcept { return slots_[i]; }
  [[nodiscard]] std::size_t in_use() const noexcept {
    std::size_t n = 0;
    for (const auto &s : slots_)
      n += s.xfer != nullptr;
    return n;
  }

private:
  reloco::array<slot, N> slots_{};
};

} // namespace structo::hw

#endif // RELOCO_HAS_COROUTINES
