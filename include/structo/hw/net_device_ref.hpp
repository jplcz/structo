// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file net_device_ref.hpp
 * @brief `structo::hw::net_device_ref`: a type-erased, non-owning,
 * *coroutine-based* handle over a frame-oriented network device (SLIP, PPP,
 * Ethernet ...), plus the `net_device_traits<Backend>` customization point.
 * C++20 only (empty otherwise; `RELOCO_HAS_COROUTINES` is 0).
 *
 * `send`/`receive` return `reloco::task`s. How a backend completes them is
 * its own business, which is what lets the same consumer code run over a
 * polled device (see `polled_net_device.hpp`) or, later, an interrupt-driven
 * one whose ISR/bottom half resumes the parked coroutine. Same shape as
 * `uart_ref`: a context pointer plus a `const vtable *`, no RTTI.
 *
 * ## Customizing: `net_device_traits<Backend>`
 *
 * @code
 * template <> struct structo::hw::net_device_traits<my_backend> {
 *   // Largest frame (as seen by the stack) the device can carry.
 *   static std::size_t mtu(const my_backend &) noexcept;
 *   // Whether the link is usable now (carrier / PPP LCP opened ...).
 *   static reloco::result<bool> link_up(my_backend &) noexcept;
 *   // Completes with one received frame copied into `dst` (its length);
 *   // `dst` stays valid until the task finishes.
 *   static reloco::task<std::size_t> receive(my_backend &, reloco::span<std::uint8_t> dst) noexcept;
 *   // Completes once the whole frame has been queued for transmission.
 *   static reloco::task<void> send(my_backend &, reloco::span<const std::uint8_t> frame) noexcept;
 * };
 * @endcode
 *
 * Optionally (SLIP/PPP have none) a link-layer address:
 *
 * @code
 * static reloco::result<structo::hw::net_mac_address> mac_address(my_backend &) noexcept;
 * @endcode
 *
 * If absent, `net_device_ref::mac_address()` fails with
 * `error::unsupported_operation`. Unbound refs fail the same way (their
 * tasks complete immediately with it).
 */

#include <reloco/coroutine.hpp>
#include <reloco/detail/compat.hpp>

#if RELOCO_HAS_COROUTINES

#include <reloco/error.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/span.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <reloco/array.hpp>
#include <type_traits>
#include <utility>

namespace structo {

using namespace reloco;

namespace hw {

/** @brief 48-bit link-layer (Ethernet) address. */
using net_mac_address = reloco::array<std::uint8_t, 6>;

/** @brief Customization point; left undefined for backends that haven't opted in. */
template <typename Backend> struct net_device_traits;

namespace detail {

template <typename Backend, typename = void> struct has_net_device_traits : std::false_type {};

template <typename Backend>
struct has_net_device_traits<
    Backend, std::void_t<decltype(net_device_traits<Backend>::mtu), decltype(net_device_traits<Backend>::link_up),
                         decltype(net_device_traits<Backend>::send), decltype(net_device_traits<Backend>::receive)>>
    : std::true_type {};

// Detects the optional Traits::mac_address.
template <typename Traits, typename Backend, typename = void> struct net_has_mac_address : std::false_type {};
template <typename Traits, typename Backend>
struct net_has_mac_address<Traits, Backend, std::void_t<decltype(Traits::mac_address(std::declval<Backend &>()))>>
    : std::true_type {};

} // namespace detail

/**
 * @brief Type-erased, non-owning handle over a polled network device.
 * Unbound refs fail every operation with `error::unsupported_operation`.
 */
class RELOCO_POINTER net_device_ref {
public:
  /** @brief Fixed, per-bound-backend-type dispatch table. */
  struct vtable {
    std::size_t (*mtu)(const void *ctx) noexcept;
    result<bool> (*link_up)(void *ctx) noexcept;
    result<net_mac_address> (*mac_address)(void *ctx) noexcept;
    task<void> (*send)(void *ctx, span<const std::uint8_t> frame) noexcept;
    task<std::size_t> (*receive)(void *ctx, span<std::uint8_t> dst) noexcept;
  };

  constexpr net_device_ref() noexcept = default;

  /** @brief Binds to `b`, which must outlive this handle and its copies. */
  template <typename Backend, std::enable_if_t<detail::has_net_device_traits<Backend>::value, int> = 0>
  constexpr explicit net_device_ref(Backend &b RELOCO_LIFETIMEBOUND RELOCO_LIFETIME_CAPTURE_BY_THIS) noexcept
      : ctx_(std::addressof(b)), vtbl_(&s_vtbl<Backend>) {}

  /** @brief Rejects rvalue/temporary backend bindings. */
  template <typename Backend, std::enable_if_t<!std::is_lvalue_reference_v<Backend>, int> = 0>
  net_device_ref(Backend &&) = delete;

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return vtbl_ != nullptr; }

  /** @brief Maximum frame size; `0` for an unbound ref. */
  [[nodiscard]] std::size_t mtu() const noexcept { return vtbl_ ? vtbl_->mtu(ctx_) : 0; }

  [[nodiscard]] result<bool> link_up() const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->link_up(ctx_);
  }

  [[nodiscard]] result<net_mac_address> mac_address() const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->mac_address(ctx_);
  }

  /** @brief Task completing once `frame` is queued (must stay valid until then). */
  [[nodiscard]] task<void> send(span<const std::uint8_t> frame) const noexcept {
    if (!vtbl_)
      return fail_void();
    return vtbl_->send(ctx_, frame);
  }

  /** @brief Task completing with the length of one frame received into `dst` (must stay valid until then). */
  [[nodiscard]] task<std::size_t> receive(span<std::uint8_t> dst) const noexcept {
    if (!vtbl_)
      return fail_size();
    return vtbl_->receive(ctx_, dst);
  }

private:
  template <typename Backend> static std::size_t mtu_entry(const void *ctx) noexcept {
    return net_device_traits<Backend>::mtu(*static_cast<const Backend *>(ctx));
  }
  template <typename Backend> static result<bool> link_up_entry(void *ctx) noexcept {
    return net_device_traits<Backend>::link_up(*static_cast<Backend *>(ctx));
  }
  template <typename Backend> static result<net_mac_address> mac_address_entry(void *ctx) noexcept {
    using traits = net_device_traits<Backend>;
    if constexpr (detail::net_has_mac_address<traits, Backend>::value) {
      return traits::mac_address(*static_cast<Backend *>(ctx));
    } else {
      (void)ctx;
      return unexpected(error::unsupported_operation);
    }
  }
  template <typename Backend> static task<void> send_entry(void *ctx, span<const std::uint8_t> frame) noexcept {
    return net_device_traits<Backend>::send(*static_cast<Backend *>(ctx), frame);
  }
  template <typename Backend> static task<std::size_t> receive_entry(void *ctx, span<std::uint8_t> dst) noexcept {
    return net_device_traits<Backend>::receive(*static_cast<Backend *>(ctx), dst);
  }

  static task<void> fail_void() noexcept {
    // task<void> cannot `co_return` an error; awaiting an unexpected ends it with one.
    co_await unexpected(error::unsupported_operation);
  }
  static task<std::size_t> fail_size() noexcept { co_return unexpected(error::unsupported_operation); }

  template <typename Backend>
  static constexpr vtable s_vtbl{&mtu_entry<Backend>, &link_up_entry<Backend>, &mac_address_entry<Backend>,
                                 &send_entry<Backend>, &receive_entry<Backend>};

  void *ctx_ = nullptr;
  const vtable *vtbl_ = nullptr;
};

} // namespace hw
} // namespace structo

#endif // RELOCO_HAS_COROUTINES
