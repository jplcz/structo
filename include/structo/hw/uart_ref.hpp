// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file uart_ref.hpp
 * @brief `structo::uart_ref`: a type-erased, non-owning handle over the
 * typical operations and settings of a *basic* (interrupt-free, polled)
 * UART -- configuring baud rate/word format, checking TX/RX readiness,
 * and sending/receiving bytes -- plus the `uart_traits<Backend>`
 * customization point a concrete backend specializes to be bindable
 * through it.
 *
 * This header is deliberately abstraction-only, the UART counterpart of
 * `io_space_ref.hpp`: there is no 16550/PL011/virtio-console register
 * poking here, no clock-to-divisor math, nothing chip-specific. A
 * concrete backend (a real 16550 driven over `io_space_ref<port_io_space>`,
 * a PL011 driven over `io_space_ref<device_io_space>`, a hypervisor
 * para-virtual console, or a unit test's in-memory fake) implements
 * `uart_traits<Backend>` however it needs to -- commonly by internally
 * using an `io_space_ref`/`io_address` pair from `io_space_ref.hpp`, but
 * that is an implementation detail this header neither requires nor
 * depends on.
 *
 * Only polled operation is modeled, matching a basic debug-console UART
 * with interrupts left disabled: readiness is always checked explicitly
 * (`tx_ready`/`rx_ready`) rather than awaited via a completion callback,
 * and every blocking convenience built on top (`put_byte`/`get_byte`/
 * `write`/`read_available`) is a bounded spin loop over that polling,
 * never a true interrupt-driven wait.
 *
 * ## Why type-erased, like `io_space_ref`
 *
 * Exactly as `io_space_ref` erases the concrete I/O-access backend
 * behind a small, fixed vtable (a two-word handle: an untyped context
 * pointer plus a `const vtable *`, no virtual base class, no RTTI, no
 * allocation of its own -- the same shape `container_ref.hpp`/
 * `function_ref.hpp` use), `uart_ref` erases the concrete UART backend
 * the same way. This lets a single debug-console/log-sink routine be
 * written once against `uart_ref` and handed whatever concrete UART a
 * given boot stage/platform/test actually has, decided at runtime.
 *
 * ## Customizing: `uart_traits<Backend>`
 *
 * `uart_traits<Backend>` is left undefined for any `Backend` that hasn't
 * opted in (mirroring `io_space_traits`/`container_ref_traits`). A
 * specialization must supply exactly five functions -- the minimal
 * "abstract operations and settings" contract, everything else
 * (`put_byte`/`get_byte`/`write`/`read_available`/`write_string`) is
 * synthesized generically on top of these by `uart_ref` itself:
 *
 * @code
 * template <> struct structo::hw::uart_traits<my_backend> {
 *   static reloco::result<void> configure(my_backend &, const structo::hw::uart_config &) noexcept;
 *   static reloco::result<bool> tx_ready(my_backend &) noexcept;
 *   static reloco::result<bool> rx_ready(my_backend &) noexcept;
 *   static reloco::result<void> try_put_byte(my_backend &, std::uint8_t) noexcept;
 *   static reloco::result<std::uint8_t> try_get_byte(my_backend &) noexcept;
 * };
 * @endcode
 *
 * Optionally, a backend may also supply `current_config`, reporting its
 * actually-applied settings back (useful when `configure`'s request
 * can't be matched exactly, e.g. an unsupported baud rate was rounded to
 * the nearest one the divisor can represent):
 *
 * @code
 * static reloco::result<structo::hw::uart_config> current_config(my_backend &) noexcept;
 * @endcode
 *
 * Detected via SFINAE (the same optional-member idiom
 * `target_ptr_space_traits::min_value`/`max_value` and
 * `io_space_traits::read_repN`/`write_repN` use); if absent,
 * `uart_ref::current_config()` fails with `error::unsupported_operation`.
 */

#include <reloco/detail/assert.hpp>
#include <reloco/detail/compat.hpp>
#include <reloco/error.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/span.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>

namespace structo {

using namespace reloco;

/** @brief Hardware-model abstractions (currently: @ref uart_ref and its
 * supporting types) -- device-level behavioral interfaces, as opposed to
 * the address-tagging/access-erasure layer (`io_address`/`io_space_ref`)
 * they are commonly, but not necessarily, built on top of. */
namespace hw {

// ============================================================================
// UART Settings
// ============================================================================

/** @brief Number of data bits per frame. */
enum class uart_data_bits : std::uint8_t { five = 5, six = 6, seven = 7, eight = 8 };

/** @brief Parity bit mode. */
enum class uart_parity : std::uint8_t { none, odd, even, mark, space };

/** @brief Number of stop bits per frame. */
enum class uart_stop_bits : std::uint8_t { one, one_point_five, two };

/** @brief Hardware/software flow control mode. */
enum class uart_flow_control : std::uint8_t { none, rts_cts, xon_xoff };

/**
 * @brief The typical settings of a basic, polled UART: baud rate plus
 * frame format and flow control.
 *
 * Aggregate, value-type configuration -- deliberately chip-agnostic: it
 * says nothing about clock dividers, FIFO depths, or register layout,
 * only the settings every UART user cares about regardless of the
 * concrete hardware underneath.
 */
struct uart_config {
  std::uint32_t baud_rate = 115200;
  uart_data_bits data_bits = uart_data_bits::eight;
  uart_parity parity = uart_parity::none;
  uart_stop_bits stop_bits = uart_stop_bits::one;
  uart_flow_control flow_control = uart_flow_control::none;

  [[nodiscard]] friend constexpr bool operator==(const uart_config &a, const uart_config &b) noexcept {
    return a.baud_rate == b.baud_rate && a.data_bits == b.data_bits && a.parity == b.parity &&
           a.stop_bits == b.stop_bits && a.flow_control == b.flow_control;
  }
  [[nodiscard]] friend constexpr bool operator!=(const uart_config &a, const uart_config &b) noexcept {
    return !(a == b);
  }
};

/** @brief A common, widely-supported default: 115200 8N1, no flow control. */
inline constexpr uart_config uart_config_115200_8n1{};

// ============================================================================
// Customization Point
// ============================================================================

/**
 * @brief Opt-in customization point describing how to configure and
 * poll/byte-transfer a concrete UART backend, through @ref uart_ref.
 *
 * Intentionally left undefined for any `Backend` that hasn't been
 * adapted, mirroring `io_space_traits`/`container_ref_traits`. See the
 * @file-level docs above for the complete required/optional member list.
 */
template <typename Backend> struct uart_traits;

namespace detail {

template <typename Backend, typename = void> struct has_uart_traits : std::false_type {};

template <typename Backend>
struct has_uart_traits<Backend, std::void_t<decltype(uart_traits<Backend>::configure),
                                            decltype(uart_traits<Backend>::tx_ready),
                                            decltype(uart_traits<Backend>::rx_ready),
                                            decltype(uart_traits<Backend>::try_put_byte),
                                            decltype(uart_traits<Backend>::try_get_byte)>> : std::true_type {};

// Detects the optional Traits::current_config readback.
template <typename Traits, typename = void> struct uart_has_current_config : std::false_type {};
template <typename Traits>
struct uart_has_current_config<Traits, std::void_t<decltype(Traits::current_config)>> : std::true_type {};

} // namespace detail

// ============================================================================
// Type-Erased UART Handle
// ============================================================================

/**
 * @brief Type-erased, non-owning handle over the typical operations and
 * settings of a basic, interrupt-free UART, for whatever concrete
 * backend it is bound to.
 *
 * Default-constructed (or copied from a default-constructed) refs are
 * *unbound*: every operation on one fails with
 * `error::unsupported_operation` rather than trapping, mirroring
 * `io_space_ref`/`mutable_container_ref`'s null-safety convention.
 */
class RELOCO_POINTER uart_ref {
public:
  /** @brief Spin-loop iteration bound used by the default-argument overloads of
   * `put_byte`/`get_byte`/`write`/`read_available`. */
  static constexpr std::uint32_t default_max_spins = 1'000'000;

  /** @brief Fixed, per-bound-backend-type dispatch table. */
  struct vtable {
    result<void> (*configure)(void *ctx, const uart_config &cfg) noexcept;
    result<uart_config> (*current_config)(void *ctx) noexcept;
    result<bool> (*tx_ready)(void *ctx) noexcept;
    result<bool> (*rx_ready)(void *ctx) noexcept;
    result<void> (*try_put_byte)(void *ctx, std::uint8_t b) noexcept;
    result<std::uint8_t> (*try_get_byte)(void *ctx) noexcept;
  };

  /** @brief Constructs an unbound ref. */
  constexpr uart_ref() noexcept = default;

  /**
   * @brief Binds this ref to an existing, adapted backend.
   * @tparam Backend Concrete backend type, deduced. Must have a
   * @ref uart_traits specialization.
   * @param b Backend to bind. Must outlive this handle and every copy of
   * it. Marked `explicit`: binding a backend is always a deliberate step,
   * never an implicit conversion.
   */
  template <typename Backend, std::enable_if_t<detail::has_uart_traits<Backend>::value, int> = 0>
  constexpr explicit uart_ref(Backend &b RELOCO_LIFETIMEBOUND RELOCO_LIFETIME_CAPTURE_BY_THIS) noexcept
      : ctx_(std::addressof(b)), vtbl_(&s_vtbl<Backend>) {}

  /** @brief Rejects rvalue/temporary backend bindings. */
  template <typename Backend, std::enable_if_t<!std::is_lvalue_reference_v<Backend>, int> = 0>
  uart_ref(Backend &&) = delete;

  /** @brief Whether this ref is bound to a backend. */
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return vtbl_ != nullptr; }

  // --------------------------------------------------------------------
  // Mandatory backend operations (directly forwarded).
  // --------------------------------------------------------------------

  /** @brief Applies `cfg` (baud rate, frame format, flow control) to the bound backend. */
  [[nodiscard]] result<void> configure(const uart_config &cfg) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->configure(ctx_, cfg);
  }

  /**
   * @brief Reads back the backend's actually-applied settings.
   * Fails with `error::unsupported_operation` if this ref is unbound, or
   * if the bound backend does not implement the optional
   * `uart_traits::current_config`.
   */
  [[nodiscard]] result<uart_config> current_config() const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->current_config(ctx_);
  }

  /** @brief Whether the backend can accept another byte without blocking. */
  [[nodiscard]] result<bool> tx_ready() const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->tx_ready(ctx_);
  }

  /** @brief Whether the backend has a received byte available to read. */
  [[nodiscard]] result<bool> rx_ready() const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->rx_ready(ctx_);
  }

  /**
   * @brief Attempts to send one byte without blocking.
   * Fails with `error::try_again` (propagated from the backend) if the
   * backend is not currently `tx_ready`.
   */
  [[nodiscard]] result<void> try_put_byte(std::uint8_t b) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->try_put_byte(ctx_, b);
  }

  /**
   * @brief Attempts to receive one byte without blocking.
   * Fails with `error::try_again` (propagated from the backend) if the
   * backend is not currently `rx_ready`.
   */
  [[nodiscard]] result<std::uint8_t> try_get_byte() const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->try_get_byte(ctx_);
  }

  // --------------------------------------------------------------------
  // Generic conveniences, synthesized purely from the five mandatory
  // operations above -- no further backend support is required for any
  // of these.
  // --------------------------------------------------------------------

  /**
   * @brief Sends one byte, spinning on `tx_ready`/`try_put_byte` for up
   * to `max_spins` iterations.
   * Fails with `error::timed_out` if `max_spins` is exhausted, or
   * whatever the backend itself reports.
   */
  [[nodiscard]] result<void> put_byte(std::uint8_t b, std::uint32_t max_spins = default_max_spins) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    for (std::uint32_t i = 0; i < max_spins; ++i) {
      auto ready = vtbl_->tx_ready(ctx_);
      if (!ready)
        return unexpected(ready.error());
      if (ready.value())
        return vtbl_->try_put_byte(ctx_, b);
    }
    return unexpected(error::timed_out);
  }

  /**
   * @brief Receives one byte, spinning on `rx_ready`/`try_get_byte` for
   * up to `max_spins` iterations.
   * Fails with `error::timed_out` if `max_spins` is exhausted, or
   * whatever the backend itself reports.
   */
  [[nodiscard]] result<std::uint8_t> get_byte(std::uint32_t max_spins = default_max_spins) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    for (std::uint32_t i = 0; i < max_spins; ++i) {
      auto ready = vtbl_->rx_ready(ctx_);
      if (!ready)
        return unexpected(ready.error());
      if (ready.value())
        return vtbl_->try_get_byte(ctx_);
    }
    return unexpected(error::timed_out);
  }

  /**
   * @brief Sends every byte of `data`, in order, via `put_byte`.
   * `max_spins` bounds each individual byte's wait, not the whole call.
   * Stops and fails (propagating `put_byte`'s error) at the first byte
   * that cannot be sent.
   */
  [[nodiscard]] result<void> write(span<const std::uint8_t> data,
                                   std::uint32_t max_spins = default_max_spins) const noexcept {
    for (std::size_t i = 0; i < data.size(); ++i) {
      auto res = put_byte(data[i], max_spins);
      if (!res)
        return res;
    }
    return {};
  }

  /**
   * @brief Debug-console convenience: sends every byte of `text`, like
   * `write`, additionally translating each `'\n'` into `"\r\n"` (the
   * usual serial-terminal line-ending expectation).
   */
  [[nodiscard]] result<void> write_string(std::string_view text,
                                          std::uint32_t max_spins = default_max_spins) const noexcept {
    for (char c : text) {
      if (c == '\n') {
        auto cr = put_byte(static_cast<std::uint8_t>('\r'), max_spins);
        if (!cr)
          return cr;
      }
      auto res = put_byte(static_cast<std::uint8_t>(c), max_spins);
      if (!res)
        return res;
    }
    return {};
  }

  /**
   * @brief Drains whatever is *currently* available (never blocks
   * waiting for more): reads bytes into `dst` while `rx_ready` keeps
   * reporting `true`, stopping early if `dst` fills up first.
   * @return The number of bytes actually read (may be `0`).
   */
  [[nodiscard]] result<std::size_t> read_available(span<std::uint8_t> dst) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    std::size_t n = 0;
    while (n < dst.size()) {
      auto ready = vtbl_->rx_ready(ctx_);
      if (!ready)
        return unexpected(ready.error());
      if (!ready.value())
        break;
      auto b = vtbl_->try_get_byte(ctx_);
      if (!b)
        return unexpected(b.error());
      dst[n] = b.value();
      ++n;
    }
    return n;
  }

private:
  template <typename Backend> static result<void> configure_entry(void *ctx, const uart_config &cfg) noexcept {
    return uart_traits<Backend>::configure(*static_cast<Backend *>(ctx), cfg);
  }

  template <typename Backend> static result<uart_config> current_config_entry(void *ctx) noexcept {
    using traits = uart_traits<Backend>;
    if constexpr (detail::uart_has_current_config<traits>::value) {
      return traits::current_config(*static_cast<Backend *>(ctx));
    } else {
      (void)ctx;
      return unexpected(error::unsupported_operation);
    }
  }

  template <typename Backend> static result<bool> tx_ready_entry(void *ctx) noexcept {
    return uart_traits<Backend>::tx_ready(*static_cast<Backend *>(ctx));
  }

  template <typename Backend> static result<bool> rx_ready_entry(void *ctx) noexcept {
    return uart_traits<Backend>::rx_ready(*static_cast<Backend *>(ctx));
  }

  template <typename Backend> static result<void> try_put_byte_entry(void *ctx, std::uint8_t b) noexcept {
    return uart_traits<Backend>::try_put_byte(*static_cast<Backend *>(ctx), b);
  }

  template <typename Backend> static result<std::uint8_t> try_get_byte_entry(void *ctx) noexcept {
    return uart_traits<Backend>::try_get_byte(*static_cast<Backend *>(ctx));
  }

  template <typename Backend>
  static constexpr vtable s_vtbl{&configure_entry<Backend>, &current_config_entry<Backend>, &tx_ready_entry<Backend>,
                                 &rx_ready_entry<Backend>,  &try_put_byte_entry<Backend>,    &try_get_byte_entry<Backend>};

  void *ctx_ = nullptr;
  const vtable *vtbl_ = nullptr;
};

} // namespace hw
} // namespace structo
