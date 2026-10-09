// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file event.hpp
 * @brief `structo::usb::event` (single-waiter awaitable wake-up flag) and `byte_ring` (lock-free SPSC byte
 * ring) used by the USB class drivers to connect interrupt-context/user-context producers with their
 * long-running transfer coroutines. C++20 only.
 */

#include <reloco/coroutine.hpp>
#include <reloco/detail/compat.hpp>
#include <reloco/span.hpp>

#if RELOCO_HAS_COROUTINES

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>

namespace structo::usb {

/**
 * @brief One coroutine waits, anyone notifies. A notification with no waiter is remembered, so the next
 * `wait()` returns immediately. Wake-ups may be spurious: always re-check your condition in a loop.
 */
class event {
public:
  class [[nodiscard]] awaiter {
  public:
    explicit awaiter(event &e) noexcept : e_(&e) {}
    bool await_ready() noexcept { return e_->signaled_.exchange(false, std::memory_order_acq_rel); }
    bool await_suspend(std::coroutine_handle<> h) noexcept {
      e_->waiter_.store(h.address(), std::memory_order_release);
      if (e_->signaled_.exchange(false, std::memory_order_acq_rel)) {
        // Signalled while parking: continue unless notify() already took the waiter and resumed us.
        return e_->waiter_.exchange(nullptr, std::memory_order_acq_rel) == nullptr;
      }
      return true;
    }
    void await_resume() noexcept {}

  private:
    event *e_;
  };

  [[nodiscard]] awaiter wait() noexcept { return awaiter{*this}; }

  /** @brief Wakes the waiter (resuming it inline) or arms the next `wait()`. */
  void notify() noexcept {
    signaled_.store(true, std::memory_order_release);
    if (void *a = waiter_.exchange(nullptr, std::memory_order_acq_rel))
      std::coroutine_handle<>::from_address(a).resume();
  }

private:
  std::atomic<bool> signaled_{false};
  std::atomic<void *> waiter_{nullptr};
};

/** @brief Lock-free single-producer/single-consumer byte ring of capacity `N` bytes (N a power of two). */
template <std::size_t N> class byte_ring {
  static_assert(N > 0 && (N & (N - 1)) == 0, "capacity must be a power of two");

public:
  [[nodiscard]] std::size_t size() const noexcept {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
  }
  [[nodiscard]] std::size_t free() const noexcept { return N - size(); }
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }
  [[nodiscard]] static constexpr std::size_t capacity() noexcept { return N; }

  bool push(std::uint8_t b) noexcept {
    const std::size_t h = head_.load(std::memory_order_relaxed);
    if (h - tail_.load(std::memory_order_acquire) == N)
      return false;
    buf_[h & (N - 1)] = b;
    head_.store(h + 1, std::memory_order_release);
    return true;
  }

  bool pop(std::uint8_t &b) noexcept {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    if (head_.load(std::memory_order_acquire) == t)
      return false;
    b = buf_[t & (N - 1)];
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

  /** @brief Pushes up to `src.size()` bytes; returns how many fit. */
  std::size_t push(reloco::span<const std::uint8_t> src) noexcept {
    std::size_t i = 0;
    while (i < src.size() && push(src[i]))
      ++i;
    return i;
  }

  /** @brief Pops up to `dst.size()` bytes; returns how many were available. */
  std::size_t pop(reloco::span<std::uint8_t> dst) noexcept {
    std::size_t i = 0;
    while (i < dst.size() && pop(dst[i]))
      ++i;
    return i;
  }

private:
  std::atomic<std::size_t> head_{0};
  std::atomic<std::size_t> tail_{0};
  reloco::array<std::uint8_t, N> buf_{};
};

} // namespace structo::usb

#endif // RELOCO_HAS_COROUTINES
