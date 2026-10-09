// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file cdc_acm.hpp
 * @brief USB CDC-ACM (virtual serial port) host driver exposed as a `hw::uart_ref` backend. C++20 only.
 *
 * `cdc_acm` buffers bytes in two lock-free rings, so `uart_ref`'s non-blocking `try_put_byte` /
 * `try_get_byte` work from any context, while two long-running coroutines (`run_rx`, `run_tx`) move
 * data over the bulk endpoints.
 *
 * @code
 * structo::usb::cdc_acm<256, 256> serial;       // 256-byte RX and TX rings (powers of two)
 * co_await serial.attach(dev);                  // dev: a configured usb_device exposing a CDC-ACM function
 * structo::hw::uart_ref uart{serial};           // use as any UART (console, logger sink ...)
 * auto rx = serial.run_rx();                    // pump tasks: start them on your scheduler / resume() them
 * auto tx = serial.run_tx();                    // from your executor; they finish after detach()
 * (void)uart.configure({.baud_rate = 9600});    // sent to the device as SET_LINE_CODING
 * uart.write_string("hello\r\n");               // queued, sent by run_tx
 * @endcode
 */

#include "../hw/uart_ref.hpp"
#include "event.hpp"
#include "usb_host.hpp"

#if RELOCO_HAS_COROUTINES

namespace structo::usb {

namespace cdc {
inline constexpr std::uint8_t subclass_acm = 0x02;
inline constexpr std::uint8_t subclass_ecm = 0x06;
inline constexpr std::uint8_t req_set_line_coding = 0x20;
inline constexpr std::uint8_t req_set_control_line_state = 0x22;
inline constexpr std::uint8_t req_set_ethernet_packet_filter = 0x43;
} // namespace cdc

template <std::size_t RxCap = 256, std::size_t TxCap = 256> class cdc_acm {
public:
  static constexpr std::size_t max_packet_limit = 512;

  /** @brief Finds the comm + data interfaces, asserts DTR/RTS and sends the current line coding. */
  [[nodiscard]] reloco::task<void> attach(usb_device &dev) noexcept {
    config_view cfg = dev.config();
    auto comm = cfg.find_interface(usb_class::cdc, cdc::subclass_acm);
    if (!comm)
      co_await reloco::unexpected(comm.error());
    auto data = cfg.find_interface(usb_class::cdc_data);
    if (!data)
      co_await reloco::unexpected(data.error());
    const endpoint_descriptor *in = data->find_endpoint(hw::usb_transfer_type::bulk, hw::usb_direction::in);
    const endpoint_descriptor *out = data->find_endpoint(hw::usb_transfer_type::bulk, hw::usb_direction::out);
    if (!in || !out)
      co_await reloco::unexpected(reloco::error::not_found);
    if (in->max_packet == 0 || in->max_packet > max_packet_limit || out->max_packet == 0 ||
        out->max_packet > max_packet_limit)
      co_await reloco::unexpected(reloco::error::invalid_argument);

    dev_ = &dev;
    in_ = dev.pipe_for(*in);
    out_ = dev.pipe_for(*out);

    hw::usb_setup_packet s{request_type::class_out_interface, cdc::req_set_control_line_state, 0x0003, comm->number, 0};
    auto r = co_await dev.control(s);
    if (!r)
      co_await reloco::unexpected(r.error());
    comm_if_ = comm->number;
    attached_.store(true, std::memory_order_release);
    line_dirty_.store(true, std::memory_order_release);
    tx_kick_.notify();
  }

  [[nodiscard]] bool attached() const noexcept { return attached_.load(std::memory_order_acquire); }

  /** @brief Marks the device gone and lets `run_rx`/`run_tx` finish. */
  void detach() noexcept {
    attached_.store(false, std::memory_order_release);
    tx_kick_.notify();
    rx_space_.notify();
  }

  /** @brief Receive pump: bulk IN -> RX ring. Reads only when the ring can take a whole packet. */
  [[nodiscard]] reloco::task<void> run_rx() noexcept {
    while (attached()) {
      if (rx_.free() < in_.max_packet) {
        co_await rx_space_.wait();
        continue;
      }
      hw::usb_completion c = co_await dev_->controller().in(in_, rx_buf_.as_span().subspan(0, in_.max_packet));
      if (c.ok()) {
        rx_.push(reloco::span<const std::uint8_t>(rx_buf_.as_span().subspan(0, c.actual)));
      } else if (c.status == hw::usb_status::stall) {
        auto r = co_await dev_->clear_halt(in_);
        if (!r)
          break;
      } else if (c.status != hw::usb_status::timeout) {
        break;
      }
    }
    detach();
  }

  /** @brief Transmit pump: line-coding updates and TX ring -> bulk OUT. */
  [[nodiscard]] reloco::task<void> run_tx() noexcept {
    while (attached()) {
      if (line_dirty_.exchange(false, std::memory_order_acq_rel)) {
        reloco::array<std::uint8_t, 7> lc = encode_line_coding(line_);
        hw::usb_setup_packet s{request_type::class_out_interface, cdc::req_set_line_coding, 0, comm_if_, 7};
        hw::usb_completion c = co_await dev_->controller().control(dev_->control_pipe(), s,
                                                                   reloco::span<std::uint8_t>(lc.data(), lc.size()));
        if (c.status == hw::usb_status::disconnected || c.status == hw::usb_status::cancelled)
          break;
        continue;
      }
      const std::size_t n = tx_.pop(tx_buf_.as_span().subspan(0, out_.max_packet));
      if (n == 0) {
        co_await tx_kick_.wait();
        continue;
      }
      hw::usb_completion c = co_await dev_->controller().out(out_, reloco::span<const std::uint8_t>(tx_buf_.as_span().subspan(0, n)));
      if (c.status == hw::usb_status::stall) {
        auto r = co_await dev_->clear_halt(out_);
        if (!r)
          break;
      } else if (!c.ok() && c.status != hw::usb_status::timeout) {
        break;
      }
    }
    detach();
  }

  // --- uart_traits plumbing (non-blocking, any context) ---

  reloco::result<void> configure(const hw::uart_config &cfg) noexcept {
    line_ = cfg;
    line_dirty_.store(true, std::memory_order_release);
    tx_kick_.notify();
    return {};
  }
  [[nodiscard]] bool tx_ready() const noexcept { return tx_.free() > 0; }
  [[nodiscard]] bool rx_ready() const noexcept { return !rx_.empty(); }
  reloco::result<void> put(std::uint8_t b) noexcept {
    if (!attached() || !tx_.push(b))
      return reloco::unexpected(reloco::error::try_again);
    tx_kick_.notify();
    return {};
  }
  reloco::result<std::uint8_t> get() noexcept {
    std::uint8_t b = 0;
    if (!rx_.pop(b))
      return reloco::unexpected(reloco::error::try_again);
    rx_space_.notify();
    return b;
  }

  /** @brief CDC PSTN line coding: dwDTERate, bCharFormat, bParityType, bDataBits. */
  [[nodiscard]] static reloco::array<std::uint8_t, 7> encode_line_coding(const hw::uart_config &c) noexcept {
    return {static_cast<std::uint8_t>(c.baud_rate),       static_cast<std::uint8_t>(c.baud_rate >> 8),
            static_cast<std::uint8_t>(c.baud_rate >> 16), static_cast<std::uint8_t>(c.baud_rate >> 24),
            static_cast<std::uint8_t>(c.stop_bits),       static_cast<std::uint8_t>(c.parity),
            static_cast<std::uint8_t>(c.data_bits)};
  }

private:
  usb_device *dev_ = nullptr;
  hw::usb_pipe in_{};
  hw::usb_pipe out_{};
  std::uint8_t comm_if_ = 0;
  hw::uart_config line_{};
  std::atomic<bool> attached_{false};
  std::atomic<bool> line_dirty_{false};
  byte_ring<RxCap> rx_;
  byte_ring<TxCap> tx_;
  event tx_kick_;
  event rx_space_;
  reloco::array<std::uint8_t, max_packet_limit> rx_buf_{};
  reloco::array<std::uint8_t, max_packet_limit> tx_buf_{};
};

} // namespace structo::usb

namespace structo::hw {

template <std::size_t RxCap, std::size_t TxCap> struct uart_traits<usb::cdc_acm<RxCap, TxCap>> {
  using port = usb::cdc_acm<RxCap, TxCap>;
  static reloco::result<void> configure(port &p, const uart_config &c) noexcept { return p.configure(c); }
  static reloco::result<bool> tx_ready(port &p) noexcept { return p.tx_ready(); }
  static reloco::result<bool> rx_ready(port &p) noexcept { return p.rx_ready(); }
  static reloco::result<void> try_put_byte(port &p, std::uint8_t b) noexcept { return p.put(b); }
  static reloco::result<std::uint8_t> try_get_byte(port &p) noexcept { return p.get(); }
};

} // namespace structo::hw

#endif // RELOCO_HAS_COROUTINES
