// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file polled_net_device.hpp
 * @brief `structo::hw::polled_net_device<Backend>`: adapts a *polled*,
 * non-blocking frame device (SLIP/PPP/Ethernet registers, no interrupts) to
 * the coroutine-based `net_device_ref` interface. C++20 only.
 *
 * `Backend` supplies non-blocking primitives as members; "no progress yet"
 * is `error::try_again`:
 *
 * @code
 * struct my_polled_nic {
 *   std::size_t mtu() const noexcept;
 *   reloco::result<bool> link_up() noexcept;
 *   // Queue one whole frame, or error::try_again if there is no room now.
 *   reloco::result<void> try_send(reloco::span<const std::uint8_t> frame) noexcept;
 *   // Copy one whole frame into dst and return its length, error::try_again
 *   // if none is pending, error::out_of_range if dst is too small.
 *   reloco::result<std::size_t> try_receive(reloco::span<std::uint8_t> dst) noexcept;
 *   // Optional: reloco::result<structo::hw::net_mac_address> mac_address() noexcept;
 * };
 * @endcode
 *
 * A coroutine awaiting `send()`/`receive()` suspends only if the backend
 * reported `try_again`; it is then parked in the adapter until the owner's
 * loop (main loop, timer tick ...) calls `poll()`, which retries and resumes
 * whichever operations can now complete. An interrupt-driven backend would
 * instead implement `net_device_traits` directly and resume its parked
 * coroutines from its IRQ handler.
 *
 * At most one receiver and one sender may be parked at once; a second
 * concurrent waiter of the same kind fails with `error::busy`. Dropping a
 * parked task unparks it. The adapter must outlive coroutines awaiting it.
 *
 * @code
 * my_polled_nic hw;
 * structo::hw::polled_net_device<my_polled_nic> nic{hw}; // owns/wraps the backend
 * structo::hw::net_device_ref dev{nic};                  // the coroutine interface
 *
 * reloco::task<void> echo(structo::hw::net_device_ref dev) {
 *   std::array<std::uint8_t, 1500> buf; // must outlive each co_await
 *   for (;;) {
 *     // The inner co_await suspends until a frame arrives, yielding a
 *     // result<size_t>; the outer one unwraps it or ends the task with the error.
 *     std::size_t n = co_await co_await dev.receive(buf);
 *     co_await co_await dev.send({buf.data(), n});
 *   }
 * }
 *
 * auto t = echo(dev); // lazy
 * t.resume();
 * for (;;)
 *   nic.poll();       // drives all parked coroutines forward
 * @endcode
 */

#include "net_device_ref.hpp"

#include <reloco/coroutine.hpp>

#if RELOCO_HAS_COROUTINES

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace structo::hw {

template <typename Backend> class polled_net_device {
  // State shared by the two awaiter kinds. Lives in the awaiting coroutine's frame.
  struct op_base {
    polled_net_device *dev = nullptr;
    std::coroutine_handle<> waiter{};
    bool parked = false;
  };

public:
  /** @brief Awaitable returned by `receive()`; resumes with `result<size_t>` (frame length). */
  class [[nodiscard]] receive_awaiter : op_base {
  public:
    receive_awaiter(polled_net_device &d, span<std::uint8_t> buf) noexcept : buf_(buf), res_(unexpected(error::try_again)) {
      this->dev = &d;
    }
    receive_awaiter(const receive_awaiter &) = delete;
    receive_awaiter &operator=(const receive_awaiter &) = delete;
    // Move is only valid before the awaiter is awaited (it is moved into the frame by co_await).
    receive_awaiter(receive_awaiter &&o) noexcept : op_base(o), buf_(o.buf_), res_(std::move(o.res_)) {
      o.dev = nullptr;
    }
    ~receive_awaiter() {
      if (this->parked && this->dev)
        this->dev->rx_ = nullptr;
    }

    bool await_ready() noexcept {
      res_ = this->dev->be_->try_receive(buf_);
      return !(!res_ && res_.error() == error::try_again);
    }
    bool await_suspend(std::coroutine_handle<> h) noexcept {
      if (this->dev->rx_) {
        res_ = unexpected(error::busy);
        return false;
      }
      this->waiter = h;
      this->parked = true;
      this->dev->rx_ = this;
      return true;
    }
    result<std::size_t> await_resume() noexcept { return std::move(res_); }

  private:
    friend class polled_net_device;
    span<std::uint8_t> buf_;
    result<std::size_t> res_;
  };

  /** @brief Awaitable returned by `send()`; resumes with `result<void>`. */
  class [[nodiscard]] send_awaiter : op_base {
  public:
    send_awaiter(polled_net_device &d, span<const std::uint8_t> frame) noexcept : frame_(frame), res_(unexpected(error::try_again)) {
      this->dev = &d;
    }
    send_awaiter(const send_awaiter &) = delete;
    send_awaiter &operator=(const send_awaiter &) = delete;
    send_awaiter(send_awaiter &&o) noexcept : op_base(o), frame_(o.frame_), res_(std::move(o.res_)) { o.dev = nullptr; }
    ~send_awaiter() {
      if (this->parked && this->dev)
        this->dev->tx_ = nullptr;
    }

    bool await_ready() noexcept {
      res_ = this->dev->be_->try_send(frame_);
      return !(!res_ && res_.error() == error::try_again);
    }
    bool await_suspend(std::coroutine_handle<> h) noexcept {
      if (this->dev->tx_) {
        res_ = unexpected(error::busy);
        return false;
      }
      this->waiter = h;
      this->parked = true;
      this->dev->tx_ = this;
      return true;
    }
    result<void> await_resume() noexcept { return std::move(res_); }

  private:
    friend class polled_net_device;
    span<const std::uint8_t> frame_;
    result<void> res_;
  };

  explicit polled_net_device(Backend &be) noexcept : be_(&be) {}
  polled_net_device(const polled_net_device &) = delete;
  polled_net_device &operator=(const polled_net_device &) = delete;

  [[nodiscard]] Backend &backend() const noexcept { return *be_; }

  /** @brief Awaits one received frame copied into `buf` (must stay valid until resumed). */
  [[nodiscard]] receive_awaiter receive(span<std::uint8_t> buf) noexcept { return receive_awaiter(*this, buf); }

  /** @brief Awaits queueing of one frame (`frame` must stay valid until resumed). */
  [[nodiscard]] send_awaiter send(span<const std::uint8_t> frame) noexcept { return send_awaiter(*this, frame); }

  /** @brief True if a coroutine is parked waiting for a frame. */
  [[nodiscard]] bool receive_pending() const noexcept { return rx_ != nullptr; }
  /** @brief True if a coroutine is parked waiting for TX room. */
  [[nodiscard]] bool send_pending() const noexcept { return tx_ != nullptr; }

  /**
   * @brief Retries parked operations and resumes every coroutine that can now
   * complete (successfully or with a hard error).
   * @return Number of coroutines resumed.
   */
  std::size_t poll() noexcept {
    std::size_t resumed = 0;
    if (auto *op = rx_) {
      op->res_ = be_->try_receive(op->buf_);
      if (!(!op->res_ && op->res_.error() == error::try_again)) {
        rx_ = nullptr;
        op->parked = false;
        ++resumed;
        op->waiter.resume(); // may park new operations
      }
    }
    if (auto *op = tx_) {
      op->res_ = be_->try_send(op->frame_);
      if (!(!op->res_ && op->res_.error() == error::try_again)) {
        tx_ = nullptr;
        op->parked = false;
        ++resumed;
        op->waiter.resume();
      }
    }
    return resumed;
  }

private:
  Backend *be_;
  receive_awaiter *rx_ = nullptr;
  send_awaiter *tx_ = nullptr;
};

template <typename Backend> struct net_device_traits<polled_net_device<Backend>> {
  using device = polled_net_device<Backend>;

  static std::size_t mtu(const device &d) noexcept { return d.backend().mtu(); }
  static result<bool> link_up(device &d) noexcept { return d.backend().link_up(); }
  template <typename B = Backend>
  static auto mac_address(device &d) noexcept -> decltype(std::declval<B &>().mac_address()) {
    return d.backend().mac_address();
  }

  static task<std::size_t> receive(device &d, span<std::uint8_t> dst) noexcept {
    co_return co_await co_await d.receive(dst);
  }
  static task<void> send(device &d, span<const std::uint8_t> frame) noexcept {
    co_await co_await d.send(frame);
    co_return;
  }
};

} // namespace structo::hw

#endif // RELOCO_HAS_COROUTINES
