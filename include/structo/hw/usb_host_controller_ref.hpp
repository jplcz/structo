// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file usb_host_controller_ref.hpp
 * @brief `structo::hw::usb_host_controller_ref`: a type-erased, non-owning handle over a USB *host*
 * controller (xHCI/EHCI/OHCI, DWC2/DWC3, MUSB ... in host mode), with an interrupt-driven coroutine
 * transfer API. C++20 only (empty otherwise).
 *
 * The controller driver only moves transfers: it accepts a `usb_transfer` (pipe, setup packet,
 * buffer), runs it on the wire, and calls `transfer.complete(status, actual_length)` from its
 * interrupt handler. A coroutine awaiting the transfer is resumed from that call. Everything else
 * (enumeration, descriptors, class drivers) lives above, in `structo/usb/`.
 *
 * Driver contract, via `usb_host_traits<Backend>` (all `noexcept`):
 *
 * @code
 * template <> struct structo::hw::usb_host_traits<my_hcd> {
 *   // Number of root hub ports.
 *   static unsigned port_count(my_hcd &) noexcept;
 *   // Connection/enable/speed of a root port (read the port status register).
 *   static reloco::result<structo::hw::usb_port_status> port_status(my_hcd &, unsigned port) noexcept;
 *   // Resets the port (>= 50 ms, then the 10 ms recovery time) and completes with the port enabled
 *   // and its speed known. A task because the reset takes time: use your timer interrupt to resume it.
 *   static reloco::task<void> reset_port(my_hcd &, unsigned port) noexcept;
 *   // Queues `t` on the schedule. Return an error (e.g. error::not_found for a gone device) if it
 *   // cannot be queued. Later, from the IRQ handler (or inline for an immediate completion), call
 *   // t.complete(status, actual). `t` stays valid until complete() or cancel().
 *   static reloco::result<void> submit(my_hcd &, structo::hw::usb_transfer &t) noexcept;
 *   // Withdraws `t` (its awaiting coroutine was destroyed). After this returns the driver must not
 *   // touch `t` or call t.complete() again.
 *   static void cancel(my_hcd &, structo::hw::usb_transfer &t) noexcept;
 *   // Optional: reset the data toggle of an endpoint (after CLEAR_FEATURE(ENDPOINT_HALT) or SET_INTERFACE).
 *   static void reset_data_toggle(my_hcd &, const structo::hw::usb_pipe &) noexcept;
 * };
 * @endcode
 *
 * The controller keeps data toggles, splits transfers into packets and honours the 2 ms SET_ADDRESS
 * recovery interval itself (or the stack is given a delay callback, see `usb::usb_host`). A transfer
 * ends on a short packet; `actual` is the number of bytes moved.
 */

#include <reloco/coroutine.hpp>
#include <reloco/detail/compat.hpp>

#if RELOCO_HAS_COROUTINES

#include <reloco/array.hpp>
#include <reloco/error.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/span.hpp>

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace structo::hw {

enum class usb_speed : std::uint8_t { low, full, high, super, super_plus };
enum class usb_direction : std::uint8_t { out = 0, in = 1 };
enum class usb_transfer_type : std::uint8_t { control = 0, isochronous = 1, bulk = 2, interrupt = 3 };

/** @brief The 8-byte SETUP stage of a control transfer. */
struct usb_setup_packet {
  std::uint8_t request_type = 0;
  std::uint8_t request = 0;
  std::uint16_t value = 0;
  std::uint16_t index = 0;
  std::uint16_t length = 0;

  [[nodiscard]] constexpr bool device_to_host() const noexcept { return (request_type & 0x80) != 0; }

  /** @brief Little-endian wire format. */
  [[nodiscard]] reloco::array<std::uint8_t, 8> to_bytes() const noexcept {
    return {request_type,
            request,
            static_cast<std::uint8_t>(value),
            static_cast<std::uint8_t>(value >> 8),
            static_cast<std::uint8_t>(index),
            static_cast<std::uint8_t>(index >> 8),
            static_cast<std::uint8_t>(length),
            static_cast<std::uint8_t>(length >> 8)};
  }
};

/** @brief Where a transfer goes: device address + endpoint + how it behaves. */
struct usb_pipe {
  std::uint8_t address = 0;                     ///< 0 = not yet addressed (default control pipe during enumeration).
  std::uint8_t endpoint = 0;                    ///< Endpoint number 0..15, without the direction bit.
  usb_direction direction = usb_direction::out; ///< Ignored for control pipes (taken from the setup packet).
  usb_transfer_type type = usb_transfer_type::control;
  std::uint16_t max_packet = 8;
  usb_speed speed = usb_speed::full;
  std::uint8_t interval = 0; ///< Interrupt/isochronous polling interval as in the endpoint descriptor.
};

struct usb_port_status {
  bool connected = false;
  bool enabled = false;
  bool changed = false; ///< Connect/disconnect happened since the last read (cleared by the read).
  usb_speed speed = usb_speed::full;
};

enum class usb_status : std::uint8_t {
  ok,
  stall,     ///< Endpoint STALLed (protocol error: control request unsupported, MSC command failed ...).
  timeout,   ///< No response / NAK timeout.
  babble,    ///< Device sent more data than the buffer.
  bus_error, ///< CRC, bit stuffing, PID, DMA ... errors.
  cancelled,
  disconnected, ///< The device went away.
};

/** @brief Outcome of one transfer. */
struct usb_completion {
  usb_status status = usb_status::ok;
  std::size_t actual = 0;

  [[nodiscard]] constexpr bool ok() const noexcept { return status == usb_status::ok; }

  /** @brief `actual` on success, else an `error` (stall/bus errors -> `io_error`, timeout -> `timed_out`, gone ->
   * `operation_canceled`). */
  [[nodiscard]] reloco::result<std::size_t> to_result() const noexcept {
    switch (status) {
    case usb_status::ok:
      return actual;
    case usb_status::timeout:
      return reloco::unexpected(reloco::error::timed_out);
    case usb_status::cancelled:
    case usb_status::disconnected:
      return reloco::unexpected(reloco::error::operation_canceled);
    default:
      return reloco::unexpected(reloco::error::io_error);
    }
  }
};

/**
 * @brief One transfer in flight. Lives in the awaiting coroutine's frame; the controller driver reads the
 * request fields and calls `complete()` exactly once.
 */
class usb_transfer {
public:
  usb_pipe pipe;
  usb_setup_packet setup; ///< Control transfers only.
  void *data = nullptr;   ///< Buffer: source for OUT, destination for IN (or the data stage of a control transfer).
  std::size_t length = 0; ///< Buffer size / bytes to send. May be 0.

  usb_transfer() noexcept = default;
  usb_transfer(const usb_transfer &) = delete;
  usb_transfer &operator=(const usb_transfer &) = delete;

  /** @brief True for IN transfers, or control transfers whose setup packet is device-to-host. */
  [[nodiscard]] bool is_in() const noexcept {
    return pipe.type == usb_transfer_type::control ? setup.device_to_host() : pipe.direction == usb_direction::in;
  }

  /** @brief Optional observer, called from `complete()` before the waiter resumes (used by `bootldr::usb_stack`). */
  void (*done_hook)(void *ctx, usb_transfer &t) noexcept = nullptr;
  void *done_ctx = nullptr;

  /** @brief Called by the controller driver (IRQ or inline) to finish the transfer; resumes the waiter. */
  void complete(usb_status status, std::size_t actual) noexcept {
    result_ = usb_completion{status, actual};
    if (done_hook)
      done_hook(done_ctx, *this);
    if (state_.exchange(done, std::memory_order_acq_rel) == waiting)
      waiter_.resume();
  }

protected:
  static constexpr std::uint8_t idle = 0;
  static constexpr std::uint8_t waiting = 1;
  static constexpr std::uint8_t done = 2;

  std::atomic<std::uint8_t> state_{idle};
  std::coroutine_handle<> waiter_{};
  usb_completion result_{};
};

/** @brief Opt-in customization point for a USB host controller; see the file-level docs. */
template <typename Backend> struct usb_host_traits;

namespace detail {

template <typename Backend, typename = void> struct has_usb_host_traits : std::false_type {};
template <typename Backend>
struct has_usb_host_traits<
    Backend,
    std::void_t<decltype(usb_host_traits<Backend>::port_count), decltype(usb_host_traits<Backend>::port_status),
                decltype(usb_host_traits<Backend>::reset_port), decltype(usb_host_traits<Backend>::submit),
                decltype(usb_host_traits<Backend>::cancel)>> : std::true_type {};

template <typename Traits, typename Backend, typename = void> struct usb_has_reset_toggle : std::false_type {};
template <typename Traits, typename Backend>
struct usb_has_reset_toggle<
    Traits, Backend,
    std::void_t<decltype(Traits::reset_data_toggle(std::declval<Backend &>(), std::declval<const usb_pipe &>()))>>
    : std::true_type {};

} // namespace detail

/**
 * @brief Type-erased, non-owning handle over a USB host controller. Unbound refs fail with
 * `error::unsupported_operation`.
 */
class RELOCO_POINTER usb_host_controller_ref {
public:
  struct vtable {
    unsigned (*port_count)(void *ctx) noexcept;
    reloco::result<usb_port_status> (*port_status)(void *ctx, unsigned port) noexcept;
    reloco::task<void> (*reset_port)(void *ctx, unsigned port) noexcept;
    reloco::result<void> (*submit)(void *ctx, usb_transfer &t) noexcept;
    void (*cancel)(void *ctx, usb_transfer &t) noexcept;
    void (*reset_data_toggle)(void *ctx, const usb_pipe &p) noexcept;
  };

  /** @brief Awaitable transfer: `usb_completion c = co_await ref.in(pipe, buf);`. */
  class [[nodiscard]] transfer_awaiter : public usb_transfer {
  public:
    transfer_awaiter(const usb_host_controller_ref &ref, const usb_pipe &p, const usb_setup_packet &s, void *d,
                     std::size_t n) noexcept
        : ref_(&ref) {
      pipe = p;
      setup = s;
      data = d;
      length = n;
    }
    transfer_awaiter(const transfer_awaiter &) = delete;
    ~transfer_awaiter() {
      if (submitted_ && state_.load(std::memory_order_acquire) != done)
        ref_->cancel(*this);
    }

    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> h) noexcept {
      waiter_ = h;
      auto r = ref_->submit(*this);
      if (!r) {
        result_ =
            usb_completion{r.error() == reloco::error::not_found ? usb_status::disconnected : usb_status::bus_error, 0};
        return false;
      }
      submitted_ = true;
      // Completion may already have happened (inline completion, or an IRQ on another CPU).
      return state_.exchange(waiting, std::memory_order_acq_rel) != done;
    }
    usb_completion await_resume() noexcept {
      submitted_ = false;
      return result_;
    }

  private:
    const usb_host_controller_ref *ref_;
    bool submitted_ = false;
  };

  constexpr usb_host_controller_ref() noexcept = default;

  template <typename Backend, std::enable_if_t<detail::has_usb_host_traits<Backend>::value, int> = 0>
  constexpr explicit usb_host_controller_ref(Backend &b RELOCO_LIFETIMEBOUND RELOCO_LIFETIME_CAPTURE_BY_THIS) noexcept
      : ctx_(std::addressof(b)), vtbl_(&s_vtbl<Backend>) {}

  template <typename Backend, std::enable_if_t<!std::is_lvalue_reference_v<Backend>, int> = 0>
  usb_host_controller_ref(Backend &&) = delete;

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return vtbl_ != nullptr; }

  [[nodiscard]] unsigned port_count() const noexcept { return vtbl_ ? vtbl_->port_count(ctx_) : 0; }

  [[nodiscard]] reloco::result<usb_port_status> port_status(unsigned port) const noexcept {
    if (!vtbl_)
      return reloco::unexpected(reloco::error::unsupported_operation);
    return vtbl_->port_status(ctx_, port);
  }

  /** @brief Resets a root port; completes once it is enabled and `port_status()` reports its speed. */
  [[nodiscard]] reloco::task<void> reset_port(unsigned port) const noexcept {
    if (!vtbl_)
      return fail_void();
    return vtbl_->reset_port(ctx_, port);
  }

  /** @brief Control transfer; the data stage direction comes from `setup`, `data` is its buffer (may be empty). */
  [[nodiscard]] transfer_awaiter control(const usb_pipe &pipe, const usb_setup_packet &setup,
                                         reloco::span<std::uint8_t> data = {}) const noexcept {
    usb_pipe p = pipe;
    p.type = usb_transfer_type::control;
    p.endpoint = 0;
    return transfer_awaiter{*this, p, setup, data.data(), data.size()};
  }

  /** @brief Bulk/interrupt IN transfer into `buf`; ends on a short packet or when `buf` is full. */
  [[nodiscard]] transfer_awaiter in(const usb_pipe &pipe, reloco::span<std::uint8_t> buf) const noexcept {
    usb_pipe p = pipe;
    p.direction = usb_direction::in;
    return transfer_awaiter{*this, p, usb_setup_packet{}, buf.data(), buf.size()};
  }

  /** @brief Bulk/interrupt OUT transfer of `buf` (may be empty: a zero-length packet). */
  [[nodiscard]] transfer_awaiter out(const usb_pipe &pipe, reloco::span<const std::uint8_t> buf) const noexcept {
    usb_pipe p = pipe;
    p.direction = usb_direction::out;
    return transfer_awaiter{*this, p, usb_setup_packet{}, const_cast<std::uint8_t *>(buf.data()), buf.size()};
  }

  /** @brief Resets an endpoint's data toggle (no-op if the controller does not need it). */
  void reset_data_toggle(const usb_pipe &pipe) const noexcept {
    if (vtbl_)
      vtbl_->reset_data_toggle(ctx_, pipe);
  }

  /** @brief Raw submit/cancel for code that wraps a controller (e.g. `bootldr::usb_stack`); awaiters use these too. */
  reloco::result<void> submit(usb_transfer &t) const noexcept {
    if (!vtbl_)
      return reloco::unexpected(reloco::error::unsupported_operation);
    return vtbl_->submit(ctx_, t);
  }
  void cancel(usb_transfer &t) const noexcept {
    if (vtbl_)
      vtbl_->cancel(ctx_, t);
  }

private:
  static reloco::task<void> fail_void() noexcept { co_await reloco::unexpected(reloco::error::unsupported_operation); }

  template <typename Backend> static unsigned port_count_entry(void *c) noexcept {
    return usb_host_traits<Backend>::port_count(*static_cast<Backend *>(c));
  }
  template <typename Backend> static reloco::result<usb_port_status> port_status_entry(void *c, unsigned p) noexcept {
    return usb_host_traits<Backend>::port_status(*static_cast<Backend *>(c), p);
  }
  template <typename Backend> static reloco::task<void> reset_port_entry(void *c, unsigned p) noexcept {
    return usb_host_traits<Backend>::reset_port(*static_cast<Backend *>(c), p);
  }
  template <typename Backend> static reloco::result<void> submit_entry(void *c, usb_transfer &t) noexcept {
    return usb_host_traits<Backend>::submit(*static_cast<Backend *>(c), t);
  }
  template <typename Backend> static void cancel_entry(void *c, usb_transfer &t) noexcept {
    usb_host_traits<Backend>::cancel(*static_cast<Backend *>(c), t);
  }
  template <typename Backend> static void reset_toggle_entry(void *c, const usb_pipe &p) noexcept {
    using traits = usb_host_traits<Backend>;
    if constexpr (detail::usb_has_reset_toggle<traits, Backend>::value)
      traits::reset_data_toggle(*static_cast<Backend *>(c), p);
    else {
      (void)c;
      (void)p;
    }
  }

  template <typename Backend>
  static constexpr vtable s_vtbl{&port_count_entry<Backend>, &port_status_entry<Backend>, &reset_port_entry<Backend>,
                                 &submit_entry<Backend>,     &cancel_entry<Backend>,      &reset_toggle_entry<Backend>};

  void *ctx_ = nullptr;
  const vtable *vtbl_ = nullptr;
};

} // namespace structo::hw

#endif // RELOCO_HAS_COROUTINES
