// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file ohci_hcd.hpp
 * @brief `structo::hw::ohci_hcd<Env>`: a generic OpenHCI 1.0a (USB 1.1, full/low speed) host controller
 * driver. It only needs a board-provided `Env` (MMIO registers, coherent DMA memory, a millisecond delay;
 * see `usb_hcd_env.hpp`) and plugs into the USB stack through `usb_host_controller_ref`. C++20 only (empty
 * otherwise).
 *
 * @code
 * my_usb_env env;                                   // board layer: register window, DMA arena, timer
 * structo::hw::ohci_hcd<my_usb_env> hcd{env};       // the driver; default capacity is 16 transfers in flight
 * if (!hcd.start())                                 // reset the controller, build the schedule, power the root hub
 *   return;
 * structo::hw::usb_host_controller_ref ref{hcd};    // type-erased handle used by usb::usb_host / bootldr::usb_stack
 * // ... from the controller interrupt handler (or a periodic poll loop):
 * hcd.irq();                                        // acknowledge, complete finished transfers, sweep removals
 * @endcode
 *
 * Design:
 * - Root hub ports only; control, bulk and interrupt transfers (no isochronous).
 * - One endpoint descriptor (ED) per in-flight transfer. Control/bulk EDs hang off dummy list heads;
 *   interrupt EDs hang off a 63-node periodic tree, so a transfer is polled every `interval` ms (rounded
 *   down to a power of two, at most 32 ms). Interrupt transfers are one-shot like bulk ones.
 * - Transfer data goes through a per-transfer DMA bounce buffer, split into TDs of at most 4 KiB (a multiple
 *   of the max packet size). Bulk/interrupt transfers may span up to 32 TDs (128 KiB with 64-byte packets),
 *   control data stages are limited to 4 KiB.
 * - Data toggles are tracked in software per (address, endpoint, direction) and programmed into every TD.
 *   They restart at DATA0 after SET_ADDRESS, SET_CONFIGURATION, SET_INTERFACE (all endpoints of the device),
 *   CLEAR_FEATURE(ENDPOINT_HALT) (that endpoint) and `reset_data_toggle()`.
 * - Only one transfer per (address, endpoint, direction) may be in flight (`error::busy` otherwise).
 * - EDs/TDs the controller may still cache are never freed immediately: a finished or cancelled transfer's
 *   ED is first skipped, unlinked one start-of-frame later and recycled one more frame after that, so keep
 *   calling `irq()`/`poll()` (the driver enables the SOF interrupt only while such removals are pending).
 * - A transfer to an unplugged device completes with `usb_status::disconnected` when the controller reports
 *   DeviceNotResponding and a root port that `reset_port()` had brought up is no longer connected and enabled
 *   (otherwise `usb_status::timeout`). The root hub knows nothing about device addresses, so this is per port.
 */

#include <reloco/coroutine.hpp>
#include <reloco/detail/compat.hpp>

#if RELOCO_HAS_COROUTINES

#include <reloco/array.hpp>
#include <reloco/error.hpp>
#include <reloco/span.hpp>

#include "usb_hcd_env.hpp"
#include "usb_host_controller_ref.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace structo::hw::ohci {

// ---- operational register offsets (bytes from the start of the MMIO window) ----
inline constexpr std::size_t reg_revision = 0x00;
inline constexpr std::size_t reg_control = 0x04;
inline constexpr std::size_t reg_command_status = 0x08;
inline constexpr std::size_t reg_interrupt_status = 0x0C;
inline constexpr std::size_t reg_interrupt_enable = 0x10;
inline constexpr std::size_t reg_interrupt_disable = 0x14;
inline constexpr std::size_t reg_hcca = 0x18;
inline constexpr std::size_t reg_period_current_ed = 0x1C;
inline constexpr std::size_t reg_control_head_ed = 0x20;
inline constexpr std::size_t reg_control_current_ed = 0x24;
inline constexpr std::size_t reg_bulk_head_ed = 0x28;
inline constexpr std::size_t reg_bulk_current_ed = 0x2C;
inline constexpr std::size_t reg_done_head = 0x30;
inline constexpr std::size_t reg_fm_interval = 0x34;
inline constexpr std::size_t reg_fm_remaining = 0x38;
inline constexpr std::size_t reg_fm_number = 0x3C;
inline constexpr std::size_t reg_periodic_start = 0x40;
inline constexpr std::size_t reg_ls_threshold = 0x44;
inline constexpr std::size_t reg_rh_descriptor_a = 0x48;
inline constexpr std::size_t reg_rh_descriptor_b = 0x4C;
inline constexpr std::size_t reg_rh_status = 0x50;
inline constexpr std::size_t reg_rh_port_status = 0x54; ///< + 4 * port

// HcControl
inline constexpr std::uint32_t ctl_cbsr_3_1 = 3u;
inline constexpr std::uint32_t ctl_ple = 1u << 2;
inline constexpr std::uint32_t ctl_cle = 1u << 4;
inline constexpr std::uint32_t ctl_ble = 1u << 5;
inline constexpr unsigned ctl_hcfs_shift = 6;
inline constexpr std::uint32_t ctl_hcfs_mask = 3u << ctl_hcfs_shift;
inline constexpr std::uint32_t hcfs_operational = 2u;
inline constexpr std::uint32_t ctl_ir = 1u << 8;

// HcCommandStatus
inline constexpr std::uint32_t cmd_hcr = 1u << 0;
inline constexpr std::uint32_t cmd_clf = 1u << 1;
inline constexpr std::uint32_t cmd_blf = 1u << 2;
inline constexpr std::uint32_t cmd_ocr = 1u << 3;

// HcInterruptStatus / Enable / Disable
inline constexpr std::uint32_t int_wdh = 1u << 1;
inline constexpr std::uint32_t int_sf = 1u << 2;
inline constexpr std::uint32_t int_ue = 1u << 4;
inline constexpr std::uint32_t int_rhsc = 1u << 6;
inline constexpr std::uint32_t int_mie = 1u << 31;
inline constexpr std::uint32_t int_all = 0xC000007Fu;

// HcRhDescriptorA
inline constexpr std::uint32_t rha_ndp_mask = 0xFFu;
inline constexpr std::uint32_t rha_nps = 1u << 9;

// HcRhStatus
inline constexpr std::uint32_t rhs_lpsc = 1u << 16; ///< SetGlobalPower

// HcRhPortStatus
inline constexpr std::uint32_t ps_ccs = 1u << 0;
inline constexpr std::uint32_t ps_pes = 1u << 1;
inline constexpr std::uint32_t ps_prs = 1u << 4;
inline constexpr std::uint32_t ps_pps = 1u << 8;
inline constexpr std::uint32_t ps_lsda = 1u << 9;
inline constexpr std::uint32_t ps_csc = 1u << 16;
inline constexpr std::uint32_t ps_pesc = 1u << 17;
inline constexpr std::uint32_t ps_pssc = 1u << 18;
inline constexpr std::uint32_t ps_ocic = 1u << 19;
inline constexpr std::uint32_t ps_prsc = 1u << 20;

// TD condition codes
inline constexpr std::uint32_t cc_no_error = 0;
inline constexpr std::uint32_t cc_stall = 4;
inline constexpr std::uint32_t cc_device_not_responding = 5;
inline constexpr std::uint32_t cc_data_overrun = 8;
inline constexpr std::uint32_t cc_data_underrun = 9;
inline constexpr std::uint32_t cc_buffer_overrun = 12;
inline constexpr std::uint32_t cc_not_accessed = 14; ///< 14 and 15: not yet processed by the controller.

/** @brief A little-endian 32-bit word in DMA memory (hardware descriptors are little-endian on every host). */
struct le32 {
  std::uint32_t raw = 0;

  [[nodiscard]] constexpr std::uint32_t get() const noexcept {
    if constexpr (std::endian::native == std::endian::little)
      return raw;
    else
      return swap(raw);
  }
  constexpr void set(std::uint32_t v) noexcept {
    if constexpr (std::endian::native == std::endian::little)
      raw = v;
    else
      raw = swap(v);
  }

private:
  static constexpr std::uint32_t swap(std::uint32_t v) noexcept {
    return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
  }
};

/** @brief Endpoint descriptor (16 bytes, 16-byte aligned). */
struct ed {
  static constexpr std::uint32_t skip = 1u << 14;
  static constexpr std::uint32_t low_speed = 1u << 13;
  static constexpr std::uint32_t head_halted = 1u << 0;
  static constexpr std::uint32_t head_carry = 1u << 1;
  static constexpr std::uint32_t ptr_mask = ~0xFu;

  le32 flags;     ///< FA[0:6] EN[7:10] D[11:12] S[13] K[14] F[15] MPS[16:26]
  le32 tail_p;    ///< Dummy TD where the list ends.
  le32 head_p;    ///< Next TD | carry[1] | halted[0].
  le32 next_ed;
};

/** @brief General transfer descriptor (16 bytes, 16-byte aligned). */
struct td {
  static constexpr std::uint32_t dp_setup = 0;
  static constexpr std::uint32_t dp_out = 1;
  static constexpr std::uint32_t dp_in = 2;

  le32 flags;   ///< R[18] DP[19:20] DI[21:23] T[24:25] EC[26:27] CC[28:31]
  le32 cbp;     ///< Current buffer pointer (0 = zero-length / transferred completely).
  le32 next_td;
  le32 be;      ///< Last byte of the buffer.
};

/** @brief Host controller communications area: 256 bytes, 256-byte aligned. */
struct hcca {
  reloco::array<le32, 32> interrupt_table;
  le32 frame_and_pad;
  le32 done_head;
  reloco::array<std::uint8_t, 120> reserved;
};

static_assert(sizeof(le32) == 4 && alignof(le32) == 4);
static_assert(sizeof(ed) == 16 && alignof(ed) == 4);
static_assert(sizeof(td) == 16 && alignof(td) == 4);
static_assert(sizeof(hcca) == 256 && alignof(hcca) == 4);

} // namespace structo::hw::ohci

namespace structo::hw {

/**
 * @brief OHCI host controller driver over `Env` (see the file-level docs). At most `MaxTransfers` transfers
 * may be in flight; further `submit()` calls fail with `error::try_again`.
 */
template <typename Env, std::size_t MaxTransfers = 16> class ohci_hcd {
  static_assert(is_usb_hcd_env_v<Env>, "Env does not satisfy the USB host controller environment contract");
  static_assert(MaxTransfers >= 1 && MaxTransfers <= 64);

public:
  static constexpr std::size_t max_transfers = MaxTransfers;
  /** @brief Largest data stage of a control transfer. */
  static constexpr std::size_t max_control_data = 4096;
  /** @brief TDs available to the data stage of one bulk/interrupt transfer (each at most 4 KiB). */
  static constexpr std::size_t max_data_tds = 32;

  explicit ohci_hcd(Env &env) noexcept : env_(&env) {}
  ohci_hcd(const ohci_hcd &) = delete;
  ohci_hcd &operator=(const ohci_hcd &) = delete;
  ~ohci_hcd() { stop(); }

  /** @brief Called from `irq()` when the root hub reports a status change (optional; ports are also polled). */
  void set_port_event_hook(void (*hook)(void *ctx) noexcept, void *ctx) noexcept {
    hook_ = hook;
    hook_ctx_ = ctx;
  }

  /**
   * @brief Takes the controller over (SMM/BIOS hand-off), resets it, builds the schedule, starts it and powers
   * the root hub. All register waits are bounded spins. Errors: `unsupported_operation` (not OHCI 1.0),
   * `allocation_failed`, `timed_out` (hand-off or reset never completed), `io_error`.
   */
  [[nodiscard]] reloco::result<void> start() noexcept {
    if (started_)
      return {};
    if ((rd(ohci::reg_revision) & 0xFFu) != 0x10u)
      return reloco::unexpected(reloco::error::unsupported_operation);
    if (!sched_.allocate(*env_, total_size, 256))
      return reloco::unexpected(reloco::error::allocation_failed);
    if (sched_.phys() + total_size > 0x1'0000'0000ull) {
      sched_.reset();
      return reloco::unexpected(reloco::error::out_of_range);
    }
    if (auto r = take_ownership(); !r) {
      sched_.reset();
      return r;
    }

    // The frame interval survives the reset only if we restore it; FIT must toggle for the HC to latch it.
    std::uint32_t fi = rd(ohci::reg_fm_interval) & 0x3FFFu;
    if (fi == 0)
      fi = 0x2EDFu;
    wr(ohci::reg_command_status, ohci::cmd_hcr);
    bool reset_done = false;
    for (unsigned i = 0; i < spin_limit && !reset_done; ++i)
      reset_done = (rd(ohci::reg_command_status) & ohci::cmd_hcr) == 0;
    if (!reset_done) {
      sched_.reset();
      return reloco::unexpected(reloco::error::timed_out);
    }

    build_schedule();
    wr(ohci::reg_interrupt_disable, ohci::int_all);
    wr(ohci::reg_interrupt_status, ohci::int_all);
    wr(ohci::reg_hcca, static_cast<std::uint32_t>(sched_.phys()));
    wr(ohci::reg_control_head_ed, ed_phys(ctl_head));
    wr(ohci::reg_control_current_ed, 0);
    wr(ohci::reg_bulk_head_ed, ed_phys(bulk_head));
    wr(ohci::reg_bulk_current_ed, 0);
    const std::uint32_t fit = (rd(ohci::reg_fm_interval) & 0x8000'0000u) ^ 0x8000'0000u;
    const std::uint32_t fsmps = ((fi - 210u) * 6u) / 7u;
    wr(ohci::reg_fm_interval, fit | (fsmps << 16) | fi);
    wr(ohci::reg_periodic_start, (fi * 9u) / 10u);
    wr(ohci::reg_ls_threshold, 0x628u);
    env_->barrier();
    wr(ohci::reg_control, ohci::ctl_cle | ohci::ctl_ble | ohci::ctl_ple | ohci::ctl_cbsr_3_1 |
                              (ohci::hcfs_operational << ohci::ctl_hcfs_shift));
    if (((rd(ohci::reg_control) & ohci::ctl_hcfs_mask) >> ohci::ctl_hcfs_shift) != ohci::hcfs_operational) {
      wr(ohci::reg_control, 0);
      sched_.reset();
      return reloco::unexpected(reloco::error::io_error);
    }
    wr(ohci::reg_interrupt_enable, ohci::int_mie | ohci::int_wdh | ohci::int_rhsc | ohci::int_ue);

    const std::uint32_t rha = rd(ohci::reg_rh_descriptor_a);
    ports_ = rha & ohci::rha_ndp_mask;
    if (ports_ > max_ports)
      ports_ = max_ports;
    if ((rha & ohci::rha_nps) == 0) {
      wr(ohci::reg_rh_status, ohci::rhs_lpsc);
      for (unsigned p = 0; p < ports_; ++p)
        wr(port_reg(p), ohci::ps_pps);
    }
    power_good_ms_ = ((rha >> 24) & 0xFFu) * 2u;
    power_wait_pending_ = power_good_ms_ != 0;
    changed_ = 0;
    device_ports_ = 0;
    sf_enabled_ = false;
    dead_ = false;
    started_ = true;
    return {};
  }

  /** @brief Stops the controller, completes every transfer with `usb_status::cancelled` and frees all DMA memory. */
  void stop() noexcept {
    if (!started_)
      return;
    wr(ohci::reg_interrupt_disable, ohci::int_all);
    wr(ohci::reg_control, 0);
    env_->barrier();
    started_ = false;
    fail_all(usb_status::cancelled);
    for (auto &r : res_)
      r = resource{};
    sched_.reset();
  }

  /** @brief Interrupt handler body; may also be called periodically from a polled loop. */
  void irq() noexcept {
    if (!started_)
      return;
    const std::uint32_t st = rd(ohci::reg_interrupt_status);
    if (st != 0) {
      wr(ohci::reg_interrupt_status, st & ~ohci::int_wdh);
      if ((st & ohci::int_ue) != 0)
        dead_ = true;
      if ((st & ohci::int_rhsc) != 0)
        scan_ports();
      if ((st & ohci::int_wdh) != 0) {
        collect_done();
        env_->barrier();
        wr(ohci::reg_interrupt_status, ohci::int_wdh);
      }
    }
    sweep_removals();
    if (dead_) {
      wr(ohci::reg_control, 0);
      fail_all(usb_status::bus_error);
      return;
    }
    finish_touched();
  }

  /** @brief Alias of `irq()` for polled setups. */
  void poll() noexcept { irq(); }

  [[nodiscard]] unsigned port_count() const noexcept { return ports_; }

  /** @brief Reads the root port status register; clears the connect-status-change flag. */
  [[nodiscard]] reloco::result<usb_port_status> port_status(unsigned port) noexcept {
    if (!started_ || port >= ports_)
      return reloco::unexpected(reloco::error::invalid_argument);
    const std::uint32_t s = rd(port_reg(port));
    if ((s & ohci::ps_csc) != 0) {
      wr(port_reg(port), ohci::ps_csc);
      changed_ |= 1u << port;
    }
    usb_port_status out;
    out.connected = (s & ohci::ps_ccs) != 0;
    out.enabled = (s & ohci::ps_pes) != 0;
    out.speed = (s & ohci::ps_lsda) != 0 ? usb_speed::low : usb_speed::full;
    out.changed = (changed_ & (1u << port)) != 0;
    changed_ &= ~(1u << port);
    return out;
  }

  /** @brief Resets a root port (>= 50 ms), makes sure it is enabled and waits the 10 ms recovery time. */
  [[nodiscard]] reloco::task<void> reset_port(unsigned port) noexcept {
    if (!started_ || port >= ports_)
      co_await reloco::unexpected(reloco::error::invalid_argument);
    if (power_wait_pending_) {
      power_wait_pending_ = false;
      (void)co_await env_->delay_ms(power_good_ms_);
    }
    if ((rd(port_reg(port)) & ohci::ps_ccs) == 0)
      co_await reloco::unexpected(reloco::error::not_found);

    wr(port_reg(port), ohci::ps_prs);
    unsigned waited = 0;
    bool reset_seen = false;
    // The root hub times the reset itself (~10 ms); keep the port in reset for at least 50 ms overall.
    while (waited < reset_timeout_ms && !(reset_seen && waited >= reset_min_ms)) {
      (void)co_await env_->delay_ms(10);
      waited += 10;
      reset_seen = reset_seen || (rd(port_reg(port)) & ohci::ps_prsc) != 0;
    }
    if (!reset_seen)
      co_await reloco::unexpected(reloco::error::timed_out);
    wr(port_reg(port), ohci::ps_prsc);

    std::uint32_t s = rd(port_reg(port));
    if ((s & ohci::ps_ccs) == 0)
      co_await reloco::unexpected(reloco::error::not_found);
    if ((s & ohci::ps_pes) == 0) {
      wr(port_reg(port), ohci::ps_pes);
      s = rd(port_reg(port));
      if ((s & ohci::ps_pes) == 0)
        co_await reloco::unexpected(reloco::error::io_error);
    }
    device_ports_ |= 1u << port;
    (void)co_await env_->delay_ms(10);
  }

  /** @brief Queues `t`; see the file-level docs for the limits. */
  [[nodiscard]] reloco::result<void> submit(usb_transfer &t) noexcept {
    if (!started_ || dead_)
      return reloco::unexpected(reloco::error::invalid_state);
    const usb_pipe &p = t.pipe;
    if (p.type == usb_transfer_type::isochronous)
      return reloco::unexpected(reloco::error::unsupported_operation);
    if (p.address > 127 || p.endpoint > 15 || p.max_packet == 0 || p.max_packet > 1023 ||
        (t.length != 0 && t.data == nullptr))
      return reloco::unexpected(reloco::error::invalid_argument);

    const bool ctl = p.type == usb_transfer_type::control;
    const bool in = t.is_in();
    const std::size_t mps = p.max_packet;
    const std::size_t chunk = (4096 / mps) * mps;
    std::size_t n_data = 0;
    if (ctl) {
      if (t.length > max_control_data)
        return reloco::unexpected(reloco::error::out_of_range);
      n_data = t.length != 0 ? 1 : 0;
    } else {
      n_data = t.length == 0 ? 1 : (t.length + chunk - 1) / chunk;
      if (n_data > max_data_tds)
        return reloco::unexpected(reloco::error::out_of_range);
    }
    const std::size_t key = toggle_key(p.address, ctl ? 0 : p.endpoint, ctl ? false : in);
    for (const resource &r : res_)
      if (r.stage == removal::active && r.key == key)
        return reloco::unexpected(reloco::error::busy);

    std::size_t ri = res_count;
    for (std::size_t i = 0; i < res_count && ri == res_count; ++i)
      if (res_[i].stage == removal::free)
        ri = i;
    if (ri == res_count)
      return reloco::unexpected(reloco::error::try_again);
    table_slot *slot = table_.acquire(t);
    if (!slot)
      return reloco::unexpected(reloco::error::try_again);

    resource &r = res_[ri];
    if (t.length != 0) {
      const std::size_t align = t.length > 4096 ? 4096 : std::bit_ceil(t.length < 16 ? std::size_t{16} : t.length);
      if (!r.bounce.allocate(*env_, t.length, align) || r.bounce.phys_at(t.length - 1) >= 0x1'0000'0000ull) {
        r.bounce.reset();
        table_.release(*slot);
        return reloco::unexpected(reloco::error::allocation_failed);
      }
      if (!in)
        r.bounce.copy_in(0, reloco::span<const std::uint8_t>(static_cast<const std::uint8_t *>(t.data), t.length));
    }

    r.stage = removal::active;
    r.slot = slot;
    slot->st.res = static_cast<std::uint8_t>(ri);
    r.touched = false;
    r.ctl = ctl;
    r.in = in;
    r.key = key;
    r.mps = static_cast<std::uint16_t>(mps);
    r.toggle0 = ctl ? false : get_toggle(key);
    build_tds(r, ri, t, chunk, n_data);

    const std::size_t edi = res_ed_base + ri;
    ohci::ed *e = ed_at(edi);
    std::uint32_t flags = p.address | (static_cast<std::uint32_t>(p.endpoint) << 7) |
                          (static_cast<std::uint32_t>(p.max_packet) << 16);
    if (p.speed == usb_speed::low)
      flags |= ohci::ed::low_speed;
    e->flags.set(flags);
    e->tail_p.set(td_phys(ri, r.n_tds));
    e->head_p.set(td_phys(ri, 0));

    if (p.type == usb_transfer_type::interrupt) {
      unsigned period = p.interval == 0 ? 1u : p.interval;
      period = period > 32 ? 32u : std::bit_floor(period);
      r.list_head = static_cast<std::uint8_t>(node_ed(period));
    } else {
      r.list_head = static_cast<std::uint8_t>(ctl ? ctl_head : bulk_head);
    }
    ohci::ed *head = ed_at(r.list_head);
    e->next_ed.set(head->next_ed.get());
    env_->barrier();
    head->next_ed.set(ed_phys(edi));
    env_->barrier();
    if (ctl)
      wr(ohci::reg_command_status, ohci::cmd_clf);
    else if (p.type == usb_transfer_type::bulk)
      wr(ohci::reg_command_status, ohci::cmd_blf);
    return {};
  }

  /** @brief Withdraws `t`; its memory is recycled a couple of frames later. Never touches `t` afterwards. */
  void cancel(usb_transfer &t) noexcept {
    table_slot *slot = table_.find(t);
    if (!slot)
      return;
    const std::size_t ri = slot->st.res;
    resource &r = res_[ri];
    r.slot = nullptr;
    table_.release(*slot);
    begin_removal(r, ri);
  }

  /** @brief Restarts the endpoint's data toggle at DATA0 (after CLEAR_FEATURE(HALT) / SET_INTERFACE). */
  void reset_data_toggle(const usb_pipe &p) noexcept {
    if (p.address > 127 || p.endpoint > 15)
      return;
    set_toggle(toggle_key(p.address, p.endpoint, p.direction == usb_direction::in), false);
  }

  /** @brief Number of ED/TD sets not yet recycled (transfers in flight plus removals waiting for a frame). */
  [[nodiscard]] std::size_t resources_in_use() const noexcept {
    std::size_t n = 0;
    for (const resource &r : res_)
      n += r.stage != removal::free;
    return n;
  }

private:
  enum class removal : std::uint8_t {
    free,
    active,   ///< On the schedule, owned by a transfer.
    skipped,  ///< Skip bit set, still linked; waiting for a start of frame.
    unlinked, ///< Unlinked; waiting for one more start of frame (and an empty done queue).
  };
  enum class role : std::uint8_t { setup, data, status };

  struct xstate {
    std::uint8_t res = 0;
  };
  using table_type = usb_transfer_table<xstate, MaxTransfers>;
  using table_slot = typename table_type::slot;

  static constexpr std::size_t res_count = 2 * MaxTransfers; ///< Finished EDs linger for two frames.
  static constexpr std::size_t tds_per_res = 36;             // setup + 32 data + status + tail dummy, with slack
  static constexpr std::size_t max_ports = 15;
  static constexpr unsigned spin_limit = 100000;
  static constexpr unsigned reset_min_ms = 50;
  static constexpr unsigned reset_timeout_ms = 200;

  // ED indexes: 0 control head, 1 bulk head, node k (1..63) of the periodic tree at k + 1, then one per resource.
  static constexpr std::size_t ctl_head = 0;
  static constexpr std::size_t bulk_head = 1;
  static constexpr std::size_t res_ed_base = 65;
  static constexpr std::size_t ed_count = res_ed_base + res_count;

  static constexpr std::size_t hcca_off = 0;
  static constexpr std::size_t ed_off = sizeof(ohci::hcca);
  static constexpr std::size_t td_off = ed_off + ed_count * sizeof(ohci::ed);
  static constexpr std::size_t setup_off = td_off + res_count * tds_per_res * sizeof(ohci::td);
  static constexpr std::size_t total_size = setup_off + res_count * 8;

  struct resource {
    removal stage = removal::free;
    bool touched = false; ///< A TD of this transfer appeared on the done queue.
    bool ctl = false;
    bool in = false;
    bool toggle0 = false;
    std::uint8_t list_head = 0;
    std::uint8_t n_tds = 0; ///< Live TDs (the tail dummy follows them).
    std::uint16_t mps = 0;
    std::uint16_t mark = 0; ///< HcFmNumber when the current removal stage began.
    std::size_t key = 0;
    table_slot *slot = nullptr;
    reloco::array<std::uint16_t, tds_per_res> td_len{};
    usb_dma_block<Env> bounce;
  };

  struct outcome {
    usb_status status = usb_status::ok;
    std::size_t actual = 0;
    std::size_t flips = 0;
  };

  // ---- register / memory helpers ----

  [[nodiscard]] std::uint32_t rd(std::size_t off) noexcept { return env_->read32(off); }
  void wr(std::size_t off, std::uint32_t v) noexcept { env_->write32(off, v); }
  [[nodiscard]] static constexpr std::size_t port_reg(unsigned port) noexcept {
    return ohci::reg_rh_port_status + 4u * port;
  }

  [[nodiscard]] ohci::hcca *hcca_at() noexcept { return sched_.template at<ohci::hcca>(hcca_off); }
  [[nodiscard]] ohci::ed *ed_at(std::size_t i) noexcept { return sched_.template at<ohci::ed>(ed_off + i * sizeof(ohci::ed)); }
  [[nodiscard]] std::uint32_t ed_phys(std::size_t i) const noexcept {
    return static_cast<std::uint32_t>(sched_.phys_at(ed_off + i * sizeof(ohci::ed)));
  }
  [[nodiscard]] static constexpr std::size_t td_index(std::size_t r, std::size_t i) noexcept {
    return r * tds_per_res + i;
  }
  [[nodiscard]] ohci::td *td_at(std::size_t r, std::size_t i) noexcept {
    return sched_.template at<ohci::td>(td_off + td_index(r, i) * sizeof(ohci::td));
  }
  [[nodiscard]] std::uint32_t td_phys(std::size_t r, std::size_t i) const noexcept {
    return static_cast<std::uint32_t>(sched_.phys_at(td_off + td_index(r, i) * sizeof(ohci::td)));
  }
  [[nodiscard]] static constexpr std::size_t node_ed(std::size_t node) noexcept { return node + 1; }

  /** @brief ED index of a bus address within the ED area, or `ed_count` if it is not one of ours. */
  [[nodiscard]] std::size_t ed_index_of(std::uint32_t phys) const noexcept {
    const std::uint64_t base = sched_.phys_at(ed_off);
    if (phys < base || (phys - base) % sizeof(ohci::ed) != 0)
      return ed_count;
    const std::uint64_t i = (phys - base) / sizeof(ohci::ed);
    return i < ed_count ? static_cast<std::size_t>(i) : ed_count;
  }

  // ---- start-up ----

  [[nodiscard]] reloco::result<void> take_ownership() noexcept {
    if ((rd(ohci::reg_control) & ohci::ctl_ir) == 0)
      return {};
    wr(ohci::reg_command_status, ohci::cmd_ocr);
    for (unsigned i = 0; i < spin_limit; ++i)
      if ((rd(ohci::reg_control) & ohci::ctl_ir) == 0)
        return {};
    return reloco::unexpected(reloco::error::timed_out);
  }

  void build_schedule() noexcept {
    for (std::size_t i = 0; i < ed_count; ++i) {
      ohci::ed *e = ed_at(i);
      e->flags.set(i < res_ed_base ? ohci::ed::skip : 0u);
      e->tail_p.set(0);
      e->head_p.set(0);
      e->next_ed.set(0);
    }
    // Node k's successor is node k/2: frame f visits the nodes of period 32, 16, ... 1 on its way to the root.
    for (std::size_t k = 2; k < 64; ++k)
      ed_at(node_ed(k))->next_ed.set(ed_phys(node_ed(k / 2)));
    ohci::hcca *h = hcca_at();
    for (std::size_t f = 0; f < 32; ++f) {
      std::size_t rev = 0;
      for (unsigned b = 0; b < 5; ++b)
        rev |= ((f >> b) & 1u) << (4 - b);
      h->interrupt_table[f].set(ed_phys(node_ed(32 + rev)));
    }
  }

  // ---- toggles ----

  [[nodiscard]] static constexpr std::size_t toggle_key(std::size_t addr, std::size_t ep, bool in) noexcept {
    return (addr << 5) | (ep << 1) | (in ? 1u : 0u);
  }
  [[nodiscard]] bool get_toggle(std::size_t key) const noexcept {
    return ((toggles_[key >> 3] >> (key & 7u)) & 1u) != 0;
  }
  void set_toggle(std::size_t key, bool v) noexcept {
    const auto bit = static_cast<std::uint8_t>(1u << (key & 7u));
    if (v)
      toggles_[key >> 3] = static_cast<std::uint8_t>(toggles_[key >> 3] | bit);
    else
      toggles_[key >> 3] = static_cast<std::uint8_t>(toggles_[key >> 3] & ~bit);
  }
  void clear_device_toggles(std::size_t addr) noexcept {
    for (std::size_t i = 0; i < 4; ++i)
      toggles_[addr * 4 + i] = 0;
  }

  // ---- building and evaluating transfers ----

  [[nodiscard]] role role_of(const resource &r, std::size_t i) const noexcept {
    if (r.ctl && i == 0)
      return role::setup;
    if (r.ctl && i + 1 == r.n_tds)
      return role::status;
    return role::data;
  }

  void put_td(std::size_t ri, std::size_t i, std::uint32_t dp, bool rounding, bool toggle, std::uint64_t buf,
              std::size_t len) noexcept {
    ohci::td *d = td_at(ri, i);
    d->flags.set((rounding ? 1u << 18 : 0u) | (dp << 19) | ((2u | (toggle ? 1u : 0u)) << 24) | (0xFu << 28));
    d->cbp.set(len != 0 ? static_cast<std::uint32_t>(buf) : 0u);
    d->next_td.set(td_phys(ri, i + 1));
    d->be.set(len != 0 ? static_cast<std::uint32_t>(buf + len - 1) : 0u);
    res_[ri].td_len[i] = static_cast<std::uint16_t>(len);
  }

  void build_tds(resource &r, std::size_t ri, const usb_transfer &t, std::size_t chunk, std::size_t n_data) noexcept {
    std::size_t n = 0;
    if (r.ctl) {
      const auto setup_bytes = t.setup.to_bytes();
      sched_.copy_in(setup_off + ri * 8, setup_bytes.as_span());
      put_td(ri, n++, ohci::td::dp_setup, false, false, sched_.phys_at(setup_off + ri * 8), 8);
      if (n_data != 0)
        put_td(ri, n++, r.in ? ohci::td::dp_in : ohci::td::dp_out, r.in, true, r.bounce.phys(), t.length);
      put_td(ri, n++, r.in && t.length != 0 ? ohci::td::dp_out : ohci::td::dp_in, false, true, 0, 0);
    } else {
      bool tog = r.toggle0;
      std::size_t off = 0;
      for (std::size_t k = 0; k < n_data; ++k) {
        const std::size_t len = t.length - off < chunk ? t.length - off : chunk;
        put_td(ri, n++, r.in ? ohci::td::dp_in : ohci::td::dp_out, r.in, tog, len != 0 ? r.bounce.phys_at(off) : 0, len);
        tog = tog != ((packets(len, len, r.in, r.mps) & 1u) != 0);
        off += len;
      }
    }
    r.n_tds = static_cast<std::uint8_t>(n);
    ohci::td *tail = td_at(ri, n);
    tail->flags.set(0);
    tail->cbp.set(0);
    tail->next_td.set(0);
    tail->be.set(0);
  }

  /** @brief Wire packets a TD of `len` bytes consumed when `got` bytes moved (a zero-length packet counts). */
  [[nodiscard]] static std::size_t packets(std::size_t len, std::size_t got, bool in, std::size_t mps) noexcept {
    if (len == 0)
      return 1;
    std::size_t p = (got + mps - 1) / mps;
    if (in && got < len && got % mps == 0)
      ++p; // a short IN TD that ended on a packet boundary ended with a zero-length packet
    return p;
  }

  /** @brief True if a port we enabled for a device (see `reset_port`) is no longer connected and enabled. */
  [[nodiscard]] bool device_port_down() noexcept {
    for (unsigned p = 0; p < ports_; ++p)
      if ((device_ports_ & (1u << p)) != 0 &&
          (rd(port_reg(p)) & (ohci::ps_ccs | ohci::ps_pes)) != (ohci::ps_ccs | ohci::ps_pes))
        return true;
    return false;
  }

  [[nodiscard]] usb_status map_cc(std::uint32_t cc) noexcept {
    switch (cc) {
    case ohci::cc_no_error:
    case ohci::cc_data_underrun:
      return usb_status::ok;
    case ohci::cc_stall:
      return usb_status::stall;
    case ohci::cc_device_not_responding:
      return device_port_down() ? usb_status::disconnected : usb_status::timeout;
    case ohci::cc_data_overrun:
    case ohci::cc_buffer_overrun:
      return usb_status::babble;
    default:
      return usb_status::bus_error;
    }
  }

  /** @brief Inspects the retired TDs of `r`; false while the transfer is still in progress. */
  [[nodiscard]] bool evaluate(resource &r, std::size_t ri, outcome &o) noexcept {
    for (std::size_t i = 0; i < r.n_tds; ++i) {
      ohci::td *d = td_at(ri, i);
      const std::uint32_t cc = d->flags.get() >> 28;
      if (cc >= ohci::cc_not_accessed)
        return false;
      if (cc != ohci::cc_no_error && cc != ohci::cc_data_underrun) {
        o.status = map_cc(cc);
        return true;
      }
      if (role_of(r, i) != role::data)
        continue;
      const std::size_t len = r.td_len[i];
      const std::uint32_t cbp = d->cbp.get();
      std::size_t remaining = 0;
      if (cbp != 0) {
        const std::uint32_t be = d->be.get();
        remaining = be >= cbp ? static_cast<std::size_t>(be - cbp) + 1 : 0;
        remaining = remaining > len ? len : remaining;
      }
      const std::size_t got = len - remaining;
      o.actual += got;
      o.flips += packets(len, got, r.in, r.mps);
      if (r.ctl) {
        // A short control data stage must still run its status stage: resume an ED the HC halted on underrun.
        ohci::ed *e = ed_at(res_ed_base + ri);
        if ((e->head_p.get() & ohci::ed::head_halted) != 0 && cc == ohci::cc_data_underrun) {
          e->head_p.set(e->head_p.get() & ~ohci::ed::head_halted);
          env_->barrier();
          wr(ohci::reg_command_status, ohci::cmd_clf);
        }
      } else if (r.in && remaining > 0) {
        return true; // short packet ends a bulk/interrupt transfer
      }
    }
    return true;
  }

  // ---- completion ----

  void collect_done() noexcept {
    ohci::hcca *h = hcca_at();
    std::uint32_t p = h->done_head.get() & ~1u;
    h->done_head.set(0);
    const std::uint64_t base = sched_.phys_at(td_off);
    for (std::size_t guard = 0; p != 0 && guard <= res_count * tds_per_res; ++guard) {
      if (p < base || (p - base) % sizeof(ohci::td) != 0 || (p - base) / sizeof(ohci::td) >= res_count * tds_per_res)
        break;
      const std::size_t idx = static_cast<std::size_t>((p - base) / sizeof(ohci::td));
      resource &r = res_[idx / tds_per_res];
      if (r.stage == removal::active)
        r.touched = true;
      const ohci::td *d = sched_.template at<ohci::td>(td_off + idx * sizeof(ohci::td));
      p = d->next_td.get() & ~0xFu;
    }
  }

  /** @brief Completes every transfer whose TDs retired; each completion may re-enter submit()/cancel(). */
  void finish_touched() noexcept {
    for (;;) {
      std::size_t ri = res_count;
      for (std::size_t i = 0; i < res_count && ri == res_count; ++i)
        if (res_[i].stage == removal::active && res_[i].touched)
          ri = i;
      if (ri == res_count)
        return;
      resource &r = res_[ri];
      r.touched = false;
      outcome o;
      if (evaluate(r, ri, o))
        retire(r, ri, o);
    }
  }

  /** @brief Finishes bookkeeping for `r`, then (last) completes its transfer. */
  void retire(resource &r, std::size_t ri, const outcome &o) noexcept {
    usb_transfer *t = r.slot->xfer;
    if (r.in && r.bounce && o.actual != 0)
      r.bounce.copy_out(0, reloco::span<std::uint8_t>(static_cast<std::uint8_t *>(t->data), o.actual));
    if (!r.ctl) {
      set_toggle(r.key, r.toggle0 != ((o.flips & 1u) != 0));
    } else if (o.status == usb_status::ok) {
      observe_control(t->pipe.address, t->setup);
    }
    table_.release(*r.slot);
    r.slot = nullptr;
    begin_removal(r, ri);
    t->complete(o.status, o.actual);
  }

  /** @brief Control requests that reset device-side toggles. */
  void observe_control(std::size_t addr, const usb_setup_packet &s) noexcept {
    if (s.request_type == 0x00 && s.request == 5)
      clear_device_toggles(s.value & 0x7Fu);
    else if ((s.request_type == 0x00 && s.request == 9) || (s.request_type == 0x01 && s.request == 11))
      clear_device_toggles(addr);
    else if (s.request_type == 0x02 && s.request == 1 && s.value == 0)
      set_toggle(toggle_key(addr, s.index & 0xFu, (s.index & 0x80u) != 0), false);
  }

  void begin_removal(resource &r, std::size_t ri) noexcept {
    ohci::ed *e = ed_at(res_ed_base + ri);
    e->flags.set(e->flags.get() | ohci::ed::skip);
    env_->barrier();
    r.stage = removal::skipped;
    r.mark = static_cast<std::uint16_t>(rd(ohci::reg_fm_number));
    if (!sf_enabled_) {
      sf_enabled_ = true;
      wr(ohci::reg_interrupt_enable, ohci::int_sf);
    }
  }

  void unlink(resource &r, std::size_t ri) noexcept {
    const std::uint32_t target = ed_phys(res_ed_base + ri);
    ohci::ed *pred = ed_at(r.list_head);
    for (std::size_t n = 0; n <= ed_count; ++n) {
      const std::uint32_t next = pred->next_ed.get();
      if (next == target) {
        pred->next_ed.set(ed_at(res_ed_base + ri)->next_ed.get());
        env_->barrier();
        return;
      }
      const std::size_t ni = ed_index_of(next);
      if (ni == ed_count)
        return;
      pred = ed_at(ni);
    }
  }

  /** @brief Advances removals once a start of frame has passed; recycles resources the HC can no longer touch. */
  void sweep_removals() noexcept {
    if (!sf_enabled_)
      return;
    const auto fm = static_cast<std::uint16_t>(rd(ohci::reg_fm_number));
    bool pending = false;
    for (std::size_t i = 0; i < res_count; ++i) {
      resource &r = res_[i];
      if (r.stage == removal::skipped && fm != r.mark) {
        unlink(r, i);
        r.stage = removal::unlinked;
        r.mark = fm;
      } else if (r.stage == removal::unlinked && fm != r.mark && rd(ohci::reg_done_head) == 0) {
        r.bounce.reset();
        r.stage = removal::free;
      }
      pending = pending || r.stage == removal::skipped || r.stage == removal::unlinked;
    }
    if (!pending) {
      sf_enabled_ = false;
      wr(ohci::reg_interrupt_disable, ohci::int_sf);
    }
  }

  void scan_ports() noexcept {
    for (unsigned p = 0; p < ports_; ++p) {
      const std::uint32_t s = rd(port_reg(p));
      const std::uint32_t ack = s & (ohci::ps_csc | ohci::ps_pesc | ohci::ps_pssc | ohci::ps_ocic);
      if ((s & ohci::ps_csc) != 0)
        changed_ |= 1u << p;
      if (ack != 0)
        wr(port_reg(p), ack);
    }
    if (hook_)
      hook_(hook_ctx_);
  }

  /** @brief Completes every transfer in flight with `status` (controller stopped or failed). */
  void fail_all(usb_status status) noexcept {
    for (;;) {
      std::size_t ri = res_count;
      for (std::size_t i = 0; i < res_count && ri == res_count; ++i)
        if (res_[i].stage == removal::active)
          ri = i;
      if (ri == res_count)
        return;
      resource &r = res_[ri];
      usb_transfer *t = r.slot->xfer;
      table_.release(*r.slot);
      r.slot = nullptr;
      r.bounce.reset();
      r.stage = removal::free;
      t->complete(status, 0);
    }
  }

  Env *env_;
  usb_dma_block<Env> sched_;
  table_type table_;
  reloco::array<resource, res_count> res_{};
  reloco::array<std::uint8_t, 512> toggles_{};
  unsigned ports_ = 0;
  std::uint32_t changed_ = 0;
  std::uint32_t device_ports_ = 0; ///< Ports `reset_port` brought up at least once since `start()`.
  unsigned power_good_ms_ = 0;
  bool power_wait_pending_ = false;
  bool started_ = false;
  bool dead_ = false;
  bool sf_enabled_ = false;
  void (*hook_)(void *ctx) noexcept = nullptr;
  void *hook_ctx_ = nullptr;
};

/** @brief Plugs `ohci_hcd` into `usb_host_controller_ref`. */
template <typename Env, std::size_t N> struct usb_host_traits<ohci_hcd<Env, N>> {
  static unsigned port_count(ohci_hcd<Env, N> &h) noexcept { return h.port_count(); }
  static reloco::result<usb_port_status> port_status(ohci_hcd<Env, N> &h, unsigned port) noexcept {
    return h.port_status(port);
  }
  static reloco::task<void> reset_port(ohci_hcd<Env, N> &h, unsigned port) noexcept { return h.reset_port(port); }
  static reloco::result<void> submit(ohci_hcd<Env, N> &h, usb_transfer &t) noexcept { return h.submit(t); }
  static void cancel(ohci_hcd<Env, N> &h, usb_transfer &t) noexcept { h.cancel(t); }
  static void reset_data_toggle(ohci_hcd<Env, N> &h, const usb_pipe &p) noexcept { h.reset_data_toggle(p); }
};

} // namespace structo::hw

#endif // RELOCO_HAS_COROUTINES
