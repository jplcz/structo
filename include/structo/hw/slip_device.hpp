// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file slip_device.hpp
 * @brief SLIP (RFC 1055) framing over a `uart_ref`: an allocation-free,
 * sans-IO codec (`slip_encode`, `slip_decoder`) and `slip_device<Mtu>`, a
 * *polled* network backend usable through `polled_net_device` /
 * `net_device_ref`. Part of the C++20-only network stack (empty otherwise;
 * `RELOCO_HAS_COROUTINES` is 0).
 *
 * SLIP has no link-layer address (so no `mac_address`) and no link
 * negotiation (`link_up()` is always true).
 *
 * @code
 * structo::hw::uart_ref uart{my_uart};              // the serial line
 * structo::hw::slip_device<1006> slip{uart};        // 1006 = RFC 1055 max frame size
 * structo::hw::polled_net_device<decltype(slip)> pnd{slip}; // adds coroutine send/receive
 * structo::hw::net_device_ref nic{pnd};             // what the IP stack consumes
 *
 * // Main loop: pnd.poll() retries parked coroutines; slip.service() keeps the
 * // UART TX FIFO fed even when no coroutine is currently waiting.
 * for (;;) {
 *   slip.service();
 *   pnd.poll();
 * }
 * @endcode
 *
 * ## Wire format
 *
 * Frames end with `END` (0xC0); `END`/`ESC` inside data are sent as
 * `ESC ESC_END` / `ESC ESC_ESC`. A frame is also *started* with `END` so line
 * noise preceding it is flushed as a (dropped) garbage frame. The decoder
 * ignores empty frames (back-to-back `END`s).
 */

#include <reloco/coroutine.hpp>

#if RELOCO_HAS_COROUTINES

#include "uart_ref.hpp"

#include <reloco/error.hpp>
#include <reloco/span.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace structo::hw {

inline constexpr std::uint8_t slip_end = 0xC0;
inline constexpr std::uint8_t slip_esc = 0xDB;
inline constexpr std::uint8_t slip_esc_end = 0xDC;
inline constexpr std::uint8_t slip_esc_esc = 0xDD;

/** @brief Size of the SLIP encoding of `data` (leading + trailing END included). */
[[nodiscard]] constexpr std::size_t slip_encoded_size(span<const std::uint8_t> data) noexcept {
  std::size_t n = 2;
  for (std::uint8_t b : data)
    n += (b == slip_end || b == slip_esc) ? 2 : 1;
  return n;
}

/** @brief Worst-case encoded size of a `payload`-byte frame. */
[[nodiscard]] constexpr std::size_t slip_max_encoded_size(std::size_t payload) noexcept { return payload * 2 + 2; }

/**
 * @brief Encodes `data` as one SLIP frame into `out`.
 * @return Bytes written, or `error::out_of_range` if `out` is too small.
 */
[[nodiscard]] inline result<std::size_t> slip_encode(span<const std::uint8_t> data, span<std::uint8_t> out) noexcept {
  if (out.size() < slip_encoded_size(data))
    return unexpected(error::out_of_range);
  std::size_t n = 0;
  out[n++] = slip_end;
  for (std::uint8_t b : data) {
    if (b == slip_end) {
      out[n++] = slip_esc;
      out[n++] = slip_esc_end;
    } else if (b == slip_esc) {
      out[n++] = slip_esc;
      out[n++] = slip_esc_esc;
    } else {
      out[n++] = b;
    }
  }
  out[n++] = slip_end;
  return n;
}

/**
 * @brief Incremental SLIP decoder writing into a caller-supplied buffer.
 *
 * Feed bytes with `push()`. It returns `true` when a complete frame is
 * available via `frame()` (valid until the next `push()`). A malformed frame
 * (bad escape sequence, or longer than the buffer) is discarded at its
 * terminating END and counted in `dropped()`; decoding then resumes.
 */
class slip_decoder {
public:
  explicit slip_decoder(span<std::uint8_t> buf) noexcept : buf_(buf) {}

  /** @brief Consumes one byte; true if it completed a frame. */
  [[nodiscard]] bool push(std::uint8_t b) noexcept {
    if (done_) {
      len_ = 0;
      done_ = false;
    }
    if (b == slip_end) {
      const bool ok = !bad_ && !esc_ && len_ > 0;
      if (bad_ || esc_)
        ++dropped_;
      bad_ = false;
      esc_ = false;
      if (ok) {
        done_ = true;
        return true;
      }
      len_ = 0;
      return false;
    }
    if (bad_)
      return false;
    if (esc_) {
      esc_ = false;
      if (b == slip_esc_end)
        b = slip_end;
      else if (b == slip_esc_esc)
        b = slip_esc;
      else {
        bad_ = true; // protocol violation: drop the whole frame
        return false;
      }
    } else if (b == slip_esc) {
      esc_ = true;
      return false;
    }
    if (len_ == buf_.size()) {
      bad_ = true;
      return false;
    }
    buf_[len_++] = b;
    return false;
  }

  /** @brief The frame completed by the last `push()` that returned true. */
  [[nodiscard]] span<const std::uint8_t> frame() const noexcept { return {buf_.data(), done_ ? len_ : 0}; }

  /** @brief Number of malformed/oversized frames discarded so far. */
  [[nodiscard]] std::size_t dropped() const noexcept { return dropped_; }

  /** @brief Abandons any partial frame. */
  void reset() noexcept {
    len_ = 0;
    done_ = esc_ = bad_ = false;
  }

private:
  span<std::uint8_t> buf_;
  std::size_t len_ = 0;
  std::size_t dropped_ = 0;
  bool done_ = false;
  bool esc_ = false;
  bool bad_ = false;
};

/**
 * @brief Polled SLIP network backend over a `uart_ref`, for `polled_net_device`.
 * @tparam Mtu Largest frame payload carried (RFC 1055 suggests 1006).
 *
 * Owns an `Mtu`-byte RX frame buffer and a worst-case-sized TX buffer; no
 * allocation. `try_send` encodes into the TX buffer and returns as soon as
 * it is queued; the bytes drain to the UART as it becomes ready, driven by
 * `service()` (also invoked by `try_send`/`try_receive`). Only one frame is
 * queued at a time: while the previous one is still draining `try_send`
 * returns `error::try_again`.
 */
template <std::size_t Mtu = 1006> class slip_device {
public:
  explicit slip_device(uart_ref uart) noexcept : uart_(uart), dec_(span<std::uint8_t>(rx_)) {}
  slip_device(const slip_device &) = delete;
  slip_device &operator=(const slip_device &) = delete;

  [[nodiscard]] std::size_t mtu() const noexcept { return Mtu; }
  [[nodiscard]] result<bool> link_up() noexcept { return true; }

  /** @brief Moves queued TX bytes to the UART while it accepts them; never blocks. */
  result<void> service() noexcept {
    while (tx_pos_ < tx_len_) {
      auto ready = uart_.tx_ready();
      if (!ready)
        return unexpected(ready.error());
      if (!ready.value())
        return {};
      auto r = uart_.try_put_byte(tx_[tx_pos_]);
      if (!r)
        return r;
      ++tx_pos_;
    }
    return {};
  }

  /** @brief Queues one frame; `try_again` while the previous one is still draining. */
  [[nodiscard]] result<void> try_send(span<const std::uint8_t> frame) noexcept {
    if (frame.size() > Mtu)
      return unexpected(error::out_of_range);
    if (auto s = service(); !s)
      return s;
    if (tx_pos_ < tx_len_)
      return unexpected(error::try_again);
    auto n = slip_encode(frame, span<std::uint8_t>(tx_));
    if (!n)
      return unexpected(n.error());
    tx_len_ = n.value();
    tx_pos_ = 0;
    return service();
  }

  /**
   * @brief Drains the UART RX FIFO into the decoder and copies out one frame.
   * `try_again` if no complete frame has arrived; `out_of_range` (frame
   * dropped) if `dst` is smaller than the frame.
   */
  [[nodiscard]] result<std::size_t> try_receive(span<std::uint8_t> dst) noexcept {
    if (auto s = service(); !s)
      return unexpected(s.error());
    for (;;) {
      auto ready = uart_.rx_ready();
      if (!ready)
        return unexpected(ready.error());
      if (!ready.value())
        return unexpected(error::try_again);
      auto b = uart_.try_get_byte();
      if (!b)
        return unexpected(b.error());
      if (!dec_.push(b.value()))
        continue;
      auto f = dec_.frame();
      if (f.size() > dst.size())
        return unexpected(error::out_of_range);
      for (std::size_t i = 0; i < f.size(); ++i)
        dst[i] = f[i];
      return f.size();
    }
  }

  /** @brief Malformed/oversized received frames discarded so far. */
  [[nodiscard]] std::size_t rx_dropped() const noexcept { return dec_.dropped(); }

private:
  uart_ref uart_;
  std::array<std::uint8_t, Mtu> rx_{};
  std::array<std::uint8_t, slip_max_encoded_size(Mtu)> tx_{};
  slip_decoder dec_;
  std::size_t tx_len_ = 0;
  std::size_t tx_pos_ = 0;
};

} // namespace structo::hw

#endif // RELOCO_HAS_COROUTINES
