// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file mass_storage.hpp
 * @brief USB Mass Storage (Bulk-Only Transport + SCSI transparent command set) host driver. C++20 only.
 *
 * `mass_storage` offers coroutine `read_blocks`/`write_blocks`/`flush`. `usb_msc_block_device` wraps it as a
 * synchronous `hw::block_device_ref` backend for code that cannot await (e.g. a bootloader filesystem
 * reader): it starts the task and calls your `pump` (poll the controller's interrupt handler / wait for
 * interrupt) until the transfer completes.
 *
 * @code
 * structo::usb::mass_storage msc;                 // one per attached device (LUN 0)
 * co_await msc.attach(dev);                       // finds the BOT interface, waits for ready, reads capacity
 * std::uint64_t n = msc.block_count();            // capacity in blocks of msc.block_size() bytes
 * co_await msc.read_blocks(0, sector0);           // sector0.size() must be a multiple of block_size()
 *
 * // Synchronous use through block_device_ref:
 * structo::usb::usb_msc_block_device disk{msc, &pump_hcd, nullptr, 1000000}; // pump_hcd(ctx): service the controller
 * once structo::hw::block_device_ref blk{disk};        // read/write blocks without coroutines
 * @endcode
 */

#include "../hw/block_device_ref.hpp"
#include "usb_host.hpp"

#if RELOCO_HAS_COROUTINES

#include <cstddef>

namespace structo::usb {

class mass_storage {
public:
  static constexpr std::uint8_t subclass_scsi = 0x06;
  static constexpr std::uint8_t protocol_bot = 0x50;
  /** @brief Largest single READ/WRITE(10) data stage. */
  static constexpr std::size_t max_chunk_bytes = 16384;

  [[nodiscard]] reloco::task<void> attach(usb_device &dev) noexcept {
    auto itf = dev.config().find_interface(usb_class::mass_storage, subclass_scsi, protocol_bot);
    if (!itf)
      co_await reloco::unexpected(itf.error());
    const endpoint_descriptor *in = itf->find_endpoint(hw::usb_transfer_type::bulk, hw::usb_direction::in);
    const endpoint_descriptor *out = itf->find_endpoint(hw::usb_transfer_type::bulk, hw::usb_direction::out);
    if (!in || !out)
      co_await reloco::unexpected(reloco::error::not_found);
    dev_ = &dev;
    iface_ = itf->number;
    in_ = dev.pipe_for(*in);
    out_ = dev.pipe_for(*out);

    // GET_MAX_LUN: devices with a single LUN may STALL; we only ever use LUN 0 anyway.
    reloco::array<std::uint8_t, 1> lun{};
    hw::usb_setup_packet gm{request_type::class_in_interface, 0xFE, 0, iface_, 1};
    (void)co_await dev.control(gm, reloco::span<std::uint8_t>(lun.data(), lun.size()));

    auto r = co_await init();
    if (!r)
      co_await reloco::unexpected(r.error());
    ready_ = true;
  }

  [[nodiscard]] bool ready() const noexcept { return ready_; }
  [[nodiscard]] std::size_t block_size() const noexcept { return block_size_; }
  [[nodiscard]] std::uint64_t block_count() const noexcept { return block_count_; }
  void detach() noexcept { ready_ = false; }

  [[nodiscard]] reloco::task<void> read_blocks(std::uint64_t lba, reloco::span<std::uint8_t> dst) noexcept {
    return transfer_blocks(0x28, lba, dst, true);
  }

  [[nodiscard]] reloco::task<void> write_blocks(std::uint64_t lba, reloco::span<const std::uint8_t> src) noexcept {
    return transfer_blocks(0x2A, lba, reloco::span<std::uint8_t>(const_cast<std::uint8_t *>(src.data()), src.size()),
                           false);
  }

  /** @brief SYNCHRONIZE CACHE(10): flush the device's write cache. */
  [[nodiscard]] reloco::task<void> flush() noexcept {
    if (!ready_)
      co_await reloco::unexpected(reloco::error::not_initialized);
    const reloco::array<std::uint8_t, 10> cdb{0x35};
    auto r = co_await command(cdb.as_span(), {}, false);
    if (!r)
      co_await reloco::unexpected(r.error());
  }

private:
  [[nodiscard]] reloco::task<void> init() noexcept {
    bool ok = false;
    for (int attempt = 0; attempt < 5 && !ok; ++attempt) {
      const reloco::array<std::uint8_t, 6> tur{0x00};
      auto r = co_await command(tur.as_span(), {}, false);
      if (r) {
        ok = true;
        break;
      }
      if (r.error() == reloco::error::operation_canceled)
        co_await reloco::unexpected(r.error());
      // Clear the pending unit-attention/not-ready condition.
      const reloco::array<std::uint8_t, 6> rs{0x03, 0, 0, 0, 18, 0};
      reloco::array<std::uint8_t, 18> sense{};
      (void)co_await command(rs.as_span(), sense.as_span(), true);
    }
    if (!ok)
      co_await reloco::unexpected(reloco::error::io_error);

    const reloco::array<std::uint8_t, 10> rc{0x25};
    reloco::array<std::uint8_t, 8> cap{};
    auto r = co_await command(rc.as_span(), cap.as_span(), true);
    if (!r)
      co_await reloco::unexpected(r.error());
    const std::uint32_t last = be32(cap.as_span().subspan(0, 4));
    const std::uint32_t bs = be32(cap.as_span().subspan(4, 4));
    if (bs == 0 || bs > 4096)
      co_await reloco::unexpected(reloco::error::invalid_argument);
    block_count_ = std::uint64_t{last} + 1;
    block_size_ = bs;
  }

  [[nodiscard]] reloco::task<void> transfer_blocks(std::uint8_t opcode, std::uint64_t lba,
                                                   reloco::span<std::uint8_t> buf, bool in) noexcept {
    const std::size_t len = buf.size();
    if (!ready_)
      co_await reloco::unexpected(reloco::error::not_initialized);
    if (len % block_size_ != 0)
      co_await reloco::unexpected(reloco::error::invalid_argument);
    std::uint64_t blocks = len / block_size_;
    if (lba > block_count_ || blocks > block_count_ - lba)
      co_await reloco::unexpected(reloco::error::out_of_range);
    const std::size_t chunk_blocks = max_chunk_bytes >= block_size_ ? max_chunk_bytes / block_size_ : 1;
    while (blocks > 0) {
      const std::uint64_t n = blocks < chunk_blocks ? blocks : chunk_blocks;
      const reloco::array<std::uint8_t, 10> cdb{opcode,
                                                0,
                                                static_cast<std::uint8_t>(lba >> 24),
                                                static_cast<std::uint8_t>(lba >> 16),
                                                static_cast<std::uint8_t>(lba >> 8),
                                                static_cast<std::uint8_t>(lba),
                                                0,
                                                static_cast<std::uint8_t>(n >> 8),
                                                static_cast<std::uint8_t>(n),
                                                0};
      const std::size_t bytes = static_cast<std::size_t>(n) * block_size_;
      auto r = co_await command(cdb.as_span(), buf.subspan(0, bytes), in);
      if (!r)
        co_await reloco::unexpected(r.error());
      buf = buf.subspan(bytes);
      lba += n;
      blocks -= n;
    }
  }

  static std::uint32_t be32(reloco::span<const std::uint8_t> p) noexcept {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
  }

  /** @brief One BOT command: CBW, optional data stage (`data`, may be empty), CSW. `cdb` (<= 16 bytes) is copied. */
  [[nodiscard]] reloco::task<void> command(reloco::span<const std::uint8_t> cdb, reloco::span<std::uint8_t> data,
                                           bool in) noexcept {
    const std::size_t len = data.size();
    const auto &hc = dev_->controller();
    const std::uint32_t tag = ++tag_;
    reloco::array<std::uint8_t, 31> cbw{};
    cbw[0] = 'U';
    cbw[1] = 'S';
    cbw[2] = 'B';
    cbw[3] = 'C';
    put32(cbw.as_span().subspan(4, 4), tag);
    put32(cbw.as_span().subspan(8, 4), static_cast<std::uint32_t>(len));
    cbw[12] = (in && len > 0) ? 0x80 : 0x00;
    cbw[13] = 0; // LUN 0
    cbw[14] = static_cast<std::uint8_t>(cdb.size());
    for (std::size_t i = 0; i < cdb.size(); ++i)
      cbw[15 + i] = cdb[i];

    hw::usb_completion c = co_await hc.out(out_, reloco::span<const std::uint8_t>(cbw.data(), cbw.size()));
    if (!c.ok() || c.actual != cbw.size()) {
      if (c.status == hw::usb_status::disconnected || c.status == hw::usb_status::cancelled)
        co_await reloco::unexpected(reloco::error::operation_canceled);
      (void)co_await reset_recovery();
      co_await reloco::unexpected(reloco::error::io_error);
    }

    bool data_ok = true;
    if (len > 0) {
      const hw::usb_pipe &p = in ? in_ : out_;
      c = in ? co_await hc.in(p, data) : co_await hc.out(p, reloco::span<const std::uint8_t>(data));
      if (c.status == hw::usb_status::disconnected || c.status == hw::usb_status::cancelled)
        co_await reloco::unexpected(reloco::error::operation_canceled);
      if (c.status == hw::usb_status::stall) {
        (void)co_await dev_->clear_halt(p);
        data_ok = false;
      } else if (!c.ok() || c.actual != len) {
        (void)co_await reset_recovery();
        co_await reloco::unexpected(reloco::error::io_error);
      }
    }

    reloco::array<std::uint8_t, 13> csw{};
    for (int attempt = 0; attempt < 2; ++attempt) {
      c = co_await hc.in(in_, reloco::span<std::uint8_t>(csw.data(), csw.size()));
      if (c.status == hw::usb_status::disconnected || c.status == hw::usb_status::cancelled)
        co_await reloco::unexpected(reloco::error::operation_canceled);
      if (c.status == hw::usb_status::stall) {
        (void)co_await dev_->clear_halt(in_);
        continue;
      }
      break;
    }
    if (!c.ok() || c.actual != csw.size() || csw[0] != 'U' || csw[1] != 'S' || csw[2] != 'B' || csw[3] != 'S' ||
        be_tag(csw.as_span().subspan(4, 4)) != tag) {
      (void)co_await reset_recovery();
      co_await reloco::unexpected(reloco::error::io_error);
    }
    if (csw[12] == 2) {
      (void)co_await reset_recovery();
      co_await reloco::unexpected(reloco::error::io_error);
    }
    if (csw[12] != 0 || !data_ok)
      co_await reloco::unexpected(reloco::error::io_error);
  }

  static std::uint32_t be_tag(reloco::span<const std::uint8_t> p) noexcept {
    return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[3]} << 24);
  }

  static void put32(reloco::span<std::uint8_t> p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
  }

  /** @brief BOT reset recovery: class RESET request, then clear both bulk halts. */
  [[nodiscard]] reloco::task<void> reset_recovery() noexcept {
    hw::usb_setup_packet s{request_type::class_out_interface, 0xFF, 0, iface_, 0};
    (void)co_await dev_->control(s);
    (void)co_await dev_->clear_halt(in_);
    (void)co_await dev_->clear_halt(out_);
  }

  usb_device *dev_ = nullptr;
  hw::usb_pipe in_{};
  hw::usb_pipe out_{};
  std::uint8_t iface_ = 0;
  std::uint32_t tag_ = 0;
  std::size_t block_size_ = 512;
  std::uint64_t block_count_ = 0;
  bool ready_ = false;
};

/**
 * @brief Synchronous `block_device_traits` backend over `mass_storage`. Starts each operation and calls
 * `pump(ctx)` (service the controller: poll its IRQ status, or WFI) until it completes; gives up with
 * `error::timed_out` after `max_pumps` calls, cancelling the transfer.
 */
class usb_msc_block_device {
public:
  using pump_fn = void (*)(void *ctx) noexcept;

  usb_msc_block_device(mass_storage &msc, pump_fn pump, void *ctx, std::size_t max_pumps) noexcept
      : msc_(&msc), pump_(pump), ctx_(ctx), max_pumps_(max_pumps) {}

  [[nodiscard]] mass_storage &storage() const noexcept { return *msc_; }

  reloco::result<void> run(reloco::task<void> t) noexcept {
    t.resume();
    for (std::size_t i = 0; !t.done() && i < max_pumps_; ++i)
      pump_(ctx_);
    if (!t.done())
      return reloco::unexpected(reloco::error::timed_out);
    return t.take();
  }

private:
  mass_storage *msc_;
  pump_fn pump_;
  void *ctx_;
  std::size_t max_pumps_;
};

} // namespace structo::usb

namespace structo::hw {

template <> struct block_device_traits<usb::usb_msc_block_device> {
  using dev = usb::usb_msc_block_device;
  static std::size_t block_size(dev &d) noexcept { return d.storage().block_size(); }
  static std::uint64_t block_count(dev &d) noexcept { return d.storage().block_count(); }
  static bool is_available(dev &d) noexcept { return d.storage().ready(); }
  static reloco::result<void> try_read_blocks(dev &d, std::uint64_t lba, reloco::span<std::byte> dst) noexcept {
    return d.run(d.storage().read_blocks(
        lba, reloco::span<std::uint8_t>(reinterpret_cast<std::uint8_t *>(dst.data()), dst.size())));
  }
  static reloco::result<void> try_write_blocks(dev &d, std::uint64_t lba, reloco::span<const std::byte> src) noexcept {
    return d.run(d.storage().write_blocks(
        lba, reloco::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(src.data()), src.size())));
  }
  static reloco::result<void> try_flush(dev &d) noexcept { return d.run(d.storage().flush()); }
};

} // namespace structo::hw

#endif // RELOCO_HAS_COROUTINES
