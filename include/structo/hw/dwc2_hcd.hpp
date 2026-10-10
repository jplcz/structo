// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file dwc2_hcd.hpp
 * @brief `structo::hw::dwc2_hcd<Env>`: a generic Synopsys DesignWare USB 2.0 OTG (DWC2) host-mode driver
 * (Internal DMA mode). It only needs a board-provided `Env` (MMIO registers, coherent DMA memory, a
 * millisecond delay; see `usb_hcd_env.hpp`) and plugs into the USB stack through `usb_host_controller_ref`.
 * C++20 only (empty otherwise).
 *
 * @code
 * struct my_usb_env {                               // see usb_hcd_env.hpp; read32/write32 take byte offsets
 *   // ...                                          // from the start of the DWC2 register window (GOTGCTL = 0)
 * };
 * my_usb_env env;                                   // board layer: register window, DMA arena, timer
 * structo::hw::dwc2_config cfg;                     // board knobs: PHY interface, FIFO sizes, AHB burst
 * cfg.phy = structo::hw::dwc2_phy::utmi_8bit;
 * structo::hw::dwc2_hcd<my_usb_env> hcd{env, cfg};  // default capacity is 16 transfers in flight
 * if (!hcd.start())                                 // core soft reset, force host mode, FIFOs, DMA, port power
 *   return;
 * structo::hw::usb_host_controller_ref ref{hcd};    // type-erased handle used by usb::usb_host / bootldr::usb_stack
 * // ... from the controller interrupt handler (or a periodic poll loop):
 * hcd.irq();                                        // acknowledge, complete finished transfers, re-arm NAKed ones
 * @endcode
 *
 * Design:
 * - One root port; control, bulk and interrupt transfers (no isochronous), no hubs / split transactions:
 *   full/low-speed devices sit directly on the port, high speed is supported. Host mode only; slave (FIFO) mode
 *   is not supported, the core must be built with Internal DMA (`start()` fails with `unsupported_operation`).
 * - One host channel per in-flight transfer (so at most `min(NumHstChnl + 1, MaxTransfers)` transfers). A
 *   transfer is a sequence of channel runs: control = SETUP (PID SETUP/MDATA, 8 bytes), data stage (starts at
 *   DATA1), status stage (zero-length DATA1, opposite direction); bulk/interrupt = data chunks. A chunk is up
 *   to 1023 packets / 512 KiB (one packet for interrupt endpoints) and is a single `HCTSIZ`/`HCDMA` programming.
 * - Transfer data goes through a per-transfer DMA bounce block (setup/zero-length area + data); `HCDMA` is 32-bit,
 *   so the block must lie below 4 GiB. IN chunks are programmed in whole packets, a device that sends more than
 *   the caller's buffer completes with `usb_status::babble`. A transfer longer than one packet needs a max packet
 *   size that is a multiple of 4 (`HCDMA` alignment of the follow-up packets).
 * - Data toggles are tracked in software per (address, endpoint, direction) and programmed into `HCTSIZ.Pid` on
 *   every run. They restart at DATA0 after SET_ADDRESS, SET_CONFIGURATION, SET_INTERFACE (all endpoints of the
 *   device), CLEAR_FEATURE(ENDPOINT_HALT) (that endpoint) and `reset_data_toggle()`.
 * - NAK / NYET / transaction errors: a NAKed bulk/control channel is re-armed one (micro)frame later (the SOF
 *   interrupt is enabled only while such a retry is pending); a NAKed interrupt channel is re-armed at once for
 *   the next frame (`HCCHAR.OddFrm`). **Simplification:** `usb_pipe::interval` is ignored, an interrupt endpoint is
 *   polled every frame. High-speed OUT endpoints use the PING protocol after NAK/NYET. Three consecutive
 *   transaction errors fail the transfer (`timeout`, or `disconnected` when the root port went down).
 * - Only one transfer per (address, endpoint, direction) may be in flight (`error::busy` otherwise).
 *   `error::try_again` when every channel is busy.
 * - `cancel()` disables the channel and waits (bounded) for it to halt; if it does not halt in time the channel
 *   and bounce block stay allocated as a "zombie" until `irq()`/`poll()` sees it halted, so the controller never
 *   DMAs into freed memory.
 * - Errors map like `ohci_hcd`: STALL -> `stall`; no response -> `timeout` / `disconnected` (port down);
 *   babble -> `babble`; AHB/other -> `bus_error`.
 *
 * Board knobs (`dwc2_config`): `phy` (UTMI+ 8/16 bit, ULPI, dedicated full-speed serial), the three FIFO sizes in
 * 32-bit words (must fit `GHWCFG3.DfifoDepth`), `ahb_burst` (`GAHBCFG.HBstLen`) and `power_good_ms`. The register
 * window base, clocks, PHY power, VBUS and the interrupt controller are the `Env`'s / board's business.
 */

#include <reloco/coroutine.hpp>
#include <reloco/detail/compat.hpp>

#if RELOCO_HAS_COROUTINES

#include <reloco/array.hpp>
#include <reloco/error.hpp>
#include <reloco/span.hpp>

#include "usb_hcd_env.hpp"
#include "usb_host_controller_ref.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace structo::hw::dwc2 {

// ---- register offsets (bytes from the start of the MMIO window) ----
inline constexpr std::size_t reg_gotgctl = 0x000;
inline constexpr std::size_t reg_gahbcfg = 0x008;
inline constexpr std::size_t reg_gusbcfg = 0x00C;
inline constexpr std::size_t reg_grstctl = 0x010;
inline constexpr std::size_t reg_gintsts = 0x014;
inline constexpr std::size_t reg_gintmsk = 0x018;
inline constexpr std::size_t reg_grxfsiz = 0x024;
inline constexpr std::size_t reg_gnptxfsiz = 0x028;
inline constexpr std::size_t reg_gsnpsid = 0x040;
inline constexpr std::size_t reg_ghwcfg2 = 0x048;
inline constexpr std::size_t reg_ghwcfg3 = 0x04C;
inline constexpr std::size_t reg_hptxfsiz = 0x100;
inline constexpr std::size_t reg_hcfg = 0x400;
inline constexpr std::size_t reg_hfir = 0x404;
inline constexpr std::size_t reg_hfnum = 0x408;
inline constexpr std::size_t reg_haint = 0x414;
inline constexpr std::size_t reg_haintmsk = 0x418;
inline constexpr std::size_t reg_hprt = 0x440;
inline constexpr std::size_t reg_hc_base = 0x500; ///< Channel n registers start at reg_hc_base + n * reg_hc_stride.
inline constexpr std::size_t reg_hc_stride = 0x20;
inline constexpr std::size_t hc_hcchar = 0x00;
inline constexpr std::size_t hc_hcsplt = 0x04;
inline constexpr std::size_t hc_hcint = 0x08;
inline constexpr std::size_t hc_hcintmsk = 0x0C;
inline constexpr std::size_t hc_hctsiz = 0x10;
inline constexpr std::size_t hc_hcdma = 0x14;

// GAHBCFG
inline constexpr std::uint32_t ahb_glbl_intr_msk = 1u << 0;
inline constexpr unsigned ahb_hbstlen_shift = 1;
inline constexpr std::uint32_t ahb_dma_en = 1u << 5;

// GUSBCFG
inline constexpr std::uint32_t usb_phyif16 = 1u << 3;
inline constexpr std::uint32_t usb_ulpi_utmi_sel = 1u << 4;
inline constexpr std::uint32_t usb_physel = 1u << 6;
inline constexpr std::uint32_t usb_srp_cap = 1u << 8;
inline constexpr std::uint32_t usb_hnp_cap = 1u << 9;
inline constexpr unsigned usb_trdtim_shift = 10;
inline constexpr std::uint32_t usb_trdtim_mask = 0xFu << usb_trdtim_shift;
inline constexpr std::uint32_t usb_force_host = 1u << 29;
inline constexpr std::uint32_t usb_force_device = 1u << 30;

// GRSTCTL
inline constexpr std::uint32_t rst_csftrst = 1u << 0;
inline constexpr std::uint32_t rst_rxfflsh = 1u << 4;
inline constexpr std::uint32_t rst_txfflsh = 1u << 5;
inline constexpr unsigned rst_txfnum_shift = 6;
inline constexpr std::uint32_t rst_ahbidle = 1u << 31;

// GINTSTS / GINTMSK
inline constexpr std::uint32_t int_curmod_host = 1u << 0;
inline constexpr std::uint32_t int_sof = 1u << 3;
inline constexpr std::uint32_t int_prtint = 1u << 24;
inline constexpr std::uint32_t int_hchint = 1u << 25;
inline constexpr std::uint32_t int_disconn = 1u << 29;
/// Read-only status bits (writing 1 has no effect); everything else in GINTSTS is write-1-to-clear.
inline constexpr std::uint32_t int_read_only =
    int_curmod_host | (1u << 4) | (1u << 5) | int_prtint | int_hchint | (1u << 26);

// GHWCFG2 / GHWCFG3
inline constexpr unsigned hw2_arch_shift = 3;
inline constexpr std::uint32_t hw2_arch_internal_dma = 2;
inline constexpr unsigned hw2_numhstchnl_shift = 14;
inline constexpr unsigned hw3_dfifo_depth_shift = 16;

// HCFG
inline constexpr std::uint32_t hcfg_fslspclksel_mask = 3u;
inline constexpr std::uint32_t hcfg_fslssupp = 1u << 2;

// HPRT
inline constexpr std::uint32_t prt_conn_sts = 1u << 0;
inline constexpr std::uint32_t prt_conn_det = 1u << 1;
inline constexpr std::uint32_t prt_ena = 1u << 2;
inline constexpr std::uint32_t prt_en_chng = 1u << 3;
inline constexpr std::uint32_t prt_ovrcurr_chng = 1u << 5;
inline constexpr std::uint32_t prt_rst = 1u << 8;
inline constexpr std::uint32_t prt_pwr = 1u << 12;
inline constexpr unsigned prt_spd_shift = 17;
/// Bits that clear when 1 is written (PrtEna: written 1 DISABLES the port): masked in every read-modify-write.
inline constexpr std::uint32_t prt_w1c = prt_conn_det | prt_ena | prt_en_chng | prt_ovrcurr_chng;

// HCCHAR
inline constexpr unsigned chr_epnum_shift = 11;
inline constexpr std::uint32_t chr_epdir_in = 1u << 15;
inline constexpr std::uint32_t chr_lspddev = 1u << 17;
inline constexpr unsigned chr_eptype_shift = 18;
inline constexpr std::uint32_t chr_mc_one = 1u << 20;
inline constexpr unsigned chr_devaddr_shift = 22;
inline constexpr std::uint32_t chr_oddfrm = 1u << 29;
inline constexpr std::uint32_t chr_chdis = 1u << 30;
inline constexpr std::uint32_t chr_chena = 1u << 31;

// HCINT / HCINTMSK
inline constexpr std::uint32_t hci_xfercompl = 1u << 0;
inline constexpr std::uint32_t hci_chhltd = 1u << 1;
inline constexpr std::uint32_t hci_ahberr = 1u << 2;
inline constexpr std::uint32_t hci_stall = 1u << 3;
inline constexpr std::uint32_t hci_nak = 1u << 4;
inline constexpr std::uint32_t hci_ack = 1u << 5;
inline constexpr std::uint32_t hci_nyet = 1u << 6;
inline constexpr std::uint32_t hci_xacterr = 1u << 7;
inline constexpr std::uint32_t hci_bblerr = 1u << 8;
inline constexpr std::uint32_t hci_frmovrun = 1u << 9;
inline constexpr std::uint32_t hci_datatglerr = 1u << 10;

// HCTSIZ
inline constexpr std::uint32_t tsiz_xfersize_mask = 0x7FFFFu;
inline constexpr unsigned tsiz_pktcnt_shift = 19;
inline constexpr std::uint32_t tsiz_pktcnt_mask = 0x3FFu;
inline constexpr unsigned tsiz_pid_shift = 29;
inline constexpr std::uint32_t tsiz_dopng = 1u << 31;
inline constexpr std::uint32_t pid_data0 = 0;
inline constexpr std::uint32_t pid_data2 = 1;
inline constexpr std::uint32_t pid_data1 = 2;
inline constexpr std::uint32_t pid_setup = 3; ///< MDATA for non-control endpoints, SETUP for control.

} // namespace structo::hw::dwc2

namespace structo::hw {

/** @brief PHY interface the core is wired to. */
enum class dwc2_phy : std::uint8_t {
  utmi_8bit,  ///< High-speed UTMI+, 8-bit data (60 MHz PHY clock).
  utmi_16bit, ///< High-speed UTMI+, 16-bit data (30 MHz PHY clock).
  ulpi,       ///< High-speed ULPI (60 MHz).
  fs_serial,  ///< Dedicated full-speed serial transceiver (48 MHz, no high speed).
};

/** @brief Board configuration of the core; every field has a value that suits a typical UTMI+ SoC. */
struct dwc2_config {
  dwc2_phy phy = dwc2_phy::utmi_8bit;
  std::uint16_t rx_fifo_words = 512;    ///< GRXFSIZ: shared receive FIFO depth in 32-bit words.
  std::uint16_t np_tx_fifo_words = 256; ///< GNPTXFSIZ: non-periodic transmit FIFO depth.
  std::uint16_t p_tx_fifo_words = 256;  ///< HPTXFSIZ: periodic transmit FIFO depth.
  std::uint8_t ahb_burst = 0;           ///< GAHBCFG.HBstLen (0 single, 1 INCR, 3 INCR4, 5 INCR8, 7 INCR16).
  unsigned power_good_ms = 20;          ///< Wait before the first port reset after VBUS was switched on.
};

/**
 * @brief DWC2 host controller driver over `Env` (see the file-level docs). At most `MaxTransfers` transfers
 * may be in flight; further `submit()` calls fail with `error::try_again`.
 */
template <typename Env, std::size_t MaxTransfers = 16> class dwc2_hcd {
  static_assert(is_usb_hcd_env_v<Env>, "Env does not satisfy the USB host controller environment contract");
  static_assert(MaxTransfers >= 1 && MaxTransfers <= 16, "the core has at most 16 host channels");

public:
  static constexpr std::size_t max_transfers = MaxTransfers;
  /** @brief Largest data stage / bulk / interrupt transfer. */
  static constexpr std::size_t max_transfer_length = 1u << 18;

  explicit dwc2_hcd(Env &env, const dwc2_config &cfg = {}) noexcept : env_(&env), cfg_(cfg) {}
  dwc2_hcd(const dwc2_hcd &) = delete;
  dwc2_hcd &operator=(const dwc2_hcd &) = delete;
  ~dwc2_hcd() { stop(); }

  /** @brief Called from `irq()` when the root port reports a status change (optional; the port is also polled). */
  void set_port_event_hook(void (*hook)(void *ctx) noexcept, void *ctx) noexcept {
    hook_ = hook;
    hook_ctx_ = ctx;
  }

  /**
   * @brief Resets the core, forces host mode, programs the FIFOs, enables DMA and interrupts and powers the
   * port. All register waits are bounded spins. Errors: `unsupported_operation` (not a DWC2 core / no Internal
   * DMA), `invalid_argument` (FIFO sizes do not fit), `timed_out` (reset, flush or mode switch never completed).
   */
  [[nodiscard]] reloco::result<void> start() noexcept {
    if (started_)
      return {};
    const std::uint32_t id = rd(dwc2::reg_gsnpsid);
    const std::uint32_t id_hi = id >> 16;
    if (id_hi != 0x4F54u && id_hi != 0x5531u && id_hi != 0x5532u)
      return reloco::unexpected(reloco::error::unsupported_operation);
    const std::uint32_t hw2 = rd(dwc2::reg_ghwcfg2);
    if (((hw2 >> dwc2::hw2_arch_shift) & 3u) != dwc2::hw2_arch_internal_dma)
      return reloco::unexpected(reloco::error::unsupported_operation);
    const std::size_t hw_channels = ((hw2 >> dwc2::hw2_numhstchnl_shift) & 0xFu) + 1u;
    channels_ = hw_channels < MaxTransfers ? hw_channels : MaxTransfers;
    hw_channels_ = hw_channels;

    const std::uint32_t depth = rd(dwc2::reg_ghwcfg3) >> dwc2::hw3_dfifo_depth_shift;
    const std::uint32_t fifo_total = std::uint32_t{cfg_.rx_fifo_words} + cfg_.np_tx_fifo_words + cfg_.p_tx_fifo_words;
    if (cfg_.rx_fifo_words == 0 || cfg_.np_tx_fifo_words == 0 || cfg_.p_tx_fifo_words == 0 ||
        (depth != 0 && fifo_total > depth) || cfg_.ahb_burst > 0xFu)
      return reloco::unexpected(reloco::error::invalid_argument);

    std::uint32_t usb = rd(dwc2::reg_gusbcfg);
    usb &= ~(dwc2::usb_phyif16 | dwc2::usb_ulpi_utmi_sel | dwc2::usb_physel | dwc2::usb_trdtim_mask |
             dwc2::usb_srp_cap | dwc2::usb_hnp_cap | dwc2::usb_force_device);
    std::uint32_t trdtim = 9;
    switch (cfg_.phy) {
    case dwc2_phy::utmi_8bit:
      break;
    case dwc2_phy::utmi_16bit:
      usb |= dwc2::usb_phyif16;
      trdtim = 5;
      break;
    case dwc2_phy::ulpi:
      usb |= dwc2::usb_ulpi_utmi_sel;
      break;
    case dwc2_phy::fs_serial:
      usb |= dwc2::usb_physel;
      break;
    }
    wr(dwc2::reg_gusbcfg, usb | (trdtim << dwc2::usb_trdtim_shift));
    if (auto r = core_reset(); !r)
      return r;

    wr(dwc2::reg_gusbcfg, (rd(dwc2::reg_gusbcfg) & ~dwc2::usb_force_device) | dwc2::usb_force_host);
    bool host = false;
    for (unsigned i = 0; i < mode_spin_limit && !host; ++i)
      host = (rd(dwc2::reg_gintsts) & dwc2::int_curmod_host) != 0;
    if (!host)
      return reloco::unexpected(reloco::error::timed_out);

    wr(dwc2::reg_gahbcfg, (std::uint32_t{cfg_.ahb_burst} << dwc2::ahb_hbstlen_shift) | dwc2::ahb_dma_en);
    wr(dwc2::reg_grxfsiz, cfg_.rx_fifo_words);
    wr(dwc2::reg_gnptxfsiz, (std::uint32_t{cfg_.np_tx_fifo_words} << 16) | cfg_.rx_fifo_words);
    wr(dwc2::reg_hptxfsiz,
       (std::uint32_t{cfg_.p_tx_fifo_words} << 16) | (std::uint32_t{cfg_.rx_fifo_words} + cfg_.np_tx_fifo_words));
    if (!flush_fifos())
      return reloco::unexpected(reloco::error::timed_out);

    const bool fs_only = cfg_.phy == dwc2_phy::fs_serial;
    wr(dwc2::reg_hcfg, fs_only ? (std::uint32_t{1} | dwc2::hcfg_fslssupp) : 0u);
    for (std::size_t c = 0; c < hw_channels_; ++c) {
      (void)halt_channel(c);
      wr(ch_reg(c, dwc2::hc_hcint), 0xFFFF'FFFFu);
      wr(ch_reg(c, dwc2::hc_hcintmsk), 0);
    }
    wr(dwc2::reg_haintmsk, (std::uint32_t{1} << channels_) - 1u);
    wr(dwc2::reg_gintsts, 0xFFFF'FFFFu);
    gintmsk_ = dwc2::int_prtint | dwc2::int_hchint | dwc2::int_disconn;
    wr(dwc2::reg_gintmsk, gintmsk_);
    wr(dwc2::reg_gahbcfg, rd(dwc2::reg_gahbcfg) | dwc2::ahb_glbl_intr_msk);
    wr(dwc2::reg_hprt, hprt_base() | dwc2::prt_pwr);

    power_wait_pending_ = cfg_.power_good_ms != 0;
    changed_ = false;
    last_conn_ = false;
    device_port_up_ = false;
    sof_enabled_ = false;
    dead_ = false;
    started_ = true;
    return {};
  }

  /** @brief Stops the controller, completes every transfer with `usb_status::cancelled` and frees all DMA memory. */
  void stop() noexcept {
    if (!started_)
      return;
    gintmsk_ = 0;
    wr(dwc2::reg_gintmsk, 0);
    wr(dwc2::reg_gahbcfg, rd(dwc2::reg_gahbcfg) & ~dwc2::ahb_glbl_intr_msk);
    for (std::size_t c = 0; c < hw_channels_; ++c)
      (void)halt_channel(c);
    (void)core_reset(); // stops any DMA a channel that refused to halt might still be doing
    started_ = false;
    for (std::size_t c = 0; c < channels_; ++c) {
      resource &r = res_[c];
      if (r.stage == chan_stage::active) {
        usb_transfer *t = r.slot->xfer;
        table_.release(*r.slot);
        r = resource{};
        t->complete(usb_status::cancelled, 0);
      } else {
        r = resource{};
      }
    }
  }

  /** @brief Interrupt handler body; may also be called periodically from a polled loop. */
  void irq() noexcept {
    if (!started_)
      return;
    const std::uint32_t st = rd(dwc2::reg_gintsts);
    if ((st & dwc2::int_curmod_host) == 0) {
      dead_ = true;
      fail_active(usb_status::bus_error);
      return;
    }
    wr(dwc2::reg_gintsts, st & ~dwc2::int_read_only);
    if ((st & (dwc2::int_prtint | dwc2::int_disconn)) != 0)
      scan_ports((st & dwc2::int_disconn) != 0);
    const std::uint32_t haint = rd(dwc2::reg_haint);
    for (std::size_t c = 0; c < channels_; ++c) {
      if (res_[c].stage == chan_stage::halting)
        finish_zombie(c);
      else if (res_[c].stage == chan_stage::active && ((haint >> c) & 1u) != 0)
        service_channel(c);
    }
    service_deferred();
    update_sof();
  }

  /** @brief Alias of `irq()` for polled setups. */
  void poll() noexcept { irq(); }

  [[nodiscard]] unsigned port_count() const noexcept { return started_ ? 1u : 0u; }

  /** @brief Reads the root port status register; clears the connect-status-change flag. */
  [[nodiscard]] reloco::result<usb_port_status> port_status(unsigned port) noexcept {
    if (!started_ || port != 0)
      return reloco::unexpected(reloco::error::invalid_argument);
    observe_port(rd(dwc2::reg_hprt));
    const std::uint32_t s = rd(dwc2::reg_hprt);
    usb_port_status out;
    out.connected = (s & dwc2::prt_conn_sts) != 0;
    out.enabled = (s & dwc2::prt_ena) != 0;
    out.speed = speed_of(s);
    out.changed = changed_;
    changed_ = false;
    return out;
  }

  /**
   * @brief Resets the root port: >= 50 ms of reset, waits for the core to enable the port, programs the clock
   * selection (`HCFG.FSLSPclkSel`) and the frame interval (`HFIR`) for the negotiated speed and waits the 10 ms
   * recovery time.
   */
  [[nodiscard]] reloco::task<void> reset_port(unsigned port) noexcept {
    if (!started_ || port != 0)
      co_await reloco::unexpected(reloco::error::invalid_argument);
    if (power_wait_pending_) {
      power_wait_pending_ = false;
      (void)co_await env_->delay_ms(cfg_.power_good_ms);
    }
    if ((rd(dwc2::reg_hprt) & dwc2::prt_conn_sts) == 0)
      co_await reloco::unexpected(reloco::error::not_found);

    wr(dwc2::reg_hprt, hprt_base() | dwc2::prt_rst);
    (void)co_await env_->delay_ms(reset_ms);
    wr(dwc2::reg_hprt, hprt_base() & ~dwc2::prt_rst);

    bool enabled = false;
    for (unsigned waited = 0; waited <= enable_wait_ms && !enabled; waited += 10) {
      enabled = (rd(dwc2::reg_hprt) & dwc2::prt_ena) != 0;
      if (!enabled)
        (void)co_await env_->delay_ms(10);
    }
    std::uint32_t s = rd(dwc2::reg_hprt);
    if ((s & dwc2::prt_conn_sts) == 0)
      co_await reloco::unexpected(reloco::error::not_found);
    if (!enabled)
      co_await reloco::unexpected(reloco::error::io_error);
    wr(dwc2::reg_hprt, (s & ~dwc2::prt_w1c) | (s & dwc2::prt_en_chng)); // acknowledge the enable change
    program_clocks(speed_of(s));
    device_port_up_ = true;
    (void)co_await env_->delay_ms(10);
  }

  /** @brief Queues `t`; see the file-level docs for the limits. */
  [[nodiscard]] reloco::result<void> submit(usb_transfer &t) noexcept {
    if (!started_ || dead_)
      return reloco::unexpected(reloco::error::invalid_state);
    const usb_pipe &p = t.pipe;
    if (p.type == usb_transfer_type::isochronous)
      return reloco::unexpected(reloco::error::unsupported_operation);
    if (p.address > 127 || p.endpoint > 15 || p.max_packet == 0 || p.max_packet > 1024 ||
        (t.length != 0 && t.data == nullptr) || (t.length > p.max_packet && (p.max_packet & 3u) != 0))
      return reloco::unexpected(reloco::error::invalid_argument);
    if (t.length > max_transfer_length)
      return reloco::unexpected(reloco::error::out_of_range);

    const bool ctl = p.type == usb_transfer_type::control;
    const bool in = t.is_in();
    const std::size_t mps = p.max_packet;
    const std::size_t key = toggle_key(p.address, ctl ? 0 : p.endpoint, ctl ? false : in);
    for (const resource &r : res_)
      if (r.stage != chan_stage::free && r.key == key)
        return reloco::unexpected(reloco::error::busy);

    std::size_t c = channels_;
    for (std::size_t i = 0; i < channels_ && c == channels_; ++i)
      if (res_[i].stage == chan_stage::free)
        c = i;
    if (c == channels_)
      return reloco::unexpected(reloco::error::try_again);
    table_slot *slot = table_.acquire(t);
    if (!slot)
      return reloco::unexpected(reloco::error::try_again);

    resource &r = res_[c];
    const std::size_t hdr = (mps + 63u) & ~std::size_t{63};
    const std::size_t data_bytes = in ? ((t.length + mps - 1) / mps) * mps : t.length;
    const std::size_t total = hdr + data_bytes;
    if (!r.bounce.allocate(*env_, total, 64) || r.bounce.phys_at(total - 1) >= 0x1'0000'0000ull) {
      r.bounce.reset();
      table_.release(*slot);
      return reloco::unexpected(reloco::error::allocation_failed);
    }
    if (ctl) {
      const auto setup_bytes = t.setup.to_bytes();
      r.bounce.copy_in(0, setup_bytes.as_span());
    }
    if (!in && t.length != 0)
      r.bounce.copy_in(hdr, reloco::span<const std::uint8_t>(static_cast<const std::uint8_t *>(t.data), t.length));

    slot->st.chan = static_cast<std::uint8_t>(c);
    r.stage = chan_stage::active;
    r.slot = slot;
    r.ctl = ctl;
    r.in = in;
    r.status_in = !(in && t.length != 0);
    r.hs = p.speed == usb_speed::high;
    r.ls = p.speed == usb_speed::low;
    r.type = p.type;
    r.addr = p.address;
    r.ep = ctl ? std::uint8_t{0} : p.endpoint;
    r.mps = static_cast<std::uint16_t>(mps);
    r.hdr = hdr;
    r.key = key;
    r.length = t.length;
    r.moved = 0;
    r.tog = ctl ? true : get_toggle(key);
    begin_phase(r, ctl ? phase::setup : phase::data);
    arm(r, c);
    return {};
  }

  /** @brief Withdraws `t`; its channel and memory are recycled once the channel halted. Never touches `t` afterwards.
   */
  void cancel(usb_transfer &t) noexcept {
    table_slot *slot = table_.find(t);
    if (!slot)
      return;
    const std::size_t c = slot->st.chan;
    resource &r = res_[c];
    r.slot = nullptr;
    table_.release(*slot);
    r.stage = chan_stage::halting;
    if (halt_channel(c))
      finish_zombie(c);
  }

  /** @brief Restarts the endpoint's data toggle at DATA0 (after CLEAR_FEATURE(HALT) / SET_INTERFACE). */
  void reset_data_toggle(const usb_pipe &p) noexcept {
    if (p.address > 127 || p.endpoint > 15)
      return;
    set_toggle(toggle_key(p.address, p.endpoint, p.direction == usb_direction::in), false);
  }

  /** @brief Channels busy with a transfer or still waiting to halt after a cancel. */
  [[nodiscard]] std::size_t resources_in_use() const noexcept {
    std::size_t n = 0;
    for (const resource &r : res_)
      n += r.stage != chan_stage::free;
    return n;
  }

  /** @brief Host channels this driver uses (`min(GHWCFG2.NumHstChnl + 1, MaxTransfers)`); valid after `start()`. */
  [[nodiscard]] std::size_t channel_count() const noexcept { return channels_; }

private:
  enum class chan_stage : std::uint8_t {
    free,
    active,  ///< Owned by a transfer.
    halting, ///< Transfer gone (cancelled/aborted); waiting for the channel to halt before recycling it.
  };
  enum class phase : std::uint8_t { setup, data, status };

  struct xstate {
    std::uint8_t chan = 0;
  };
  using table_type = usb_transfer_table<xstate, MaxTransfers>;
  using table_slot = typename table_type::slot;

  static constexpr unsigned spin_limit = 100000;
  static constexpr unsigned mode_spin_limit = 1000000;
  static constexpr unsigned reset_ms = 60;
  static constexpr unsigned enable_wait_ms = 100;
  static constexpr unsigned max_errors = 3;
  static constexpr std::size_t max_packets_per_run = 1023;

  struct resource {
    chan_stage stage = chan_stage::free;
    phase ph = phase::data;
    bool running = false;  ///< A run was armed and its halt has not been processed yet.
    bool deferred = false; ///< NAKed: re-arm once the frame number changed.
    bool ctl = false;
    bool in = false;     ///< Direction of the data stage.
    bool cur_in = false; ///< Direction of the current run.
    bool status_in = false;
    bool hs = false;
    bool ls = false;
    bool tog = false;      ///< Data toggle for the next packet.
    bool ping = false;     ///< Next OUT run starts with PING (high speed, after NAK/NYET).
    bool ping_run = false; ///< The run in flight started with PING.
    usb_transfer_type type = usb_transfer_type::bulk;
    std::uint8_t addr = 0;
    std::uint8_t ep = 0;
    std::uint8_t errors = 0;
    std::uint16_t mps = 0;
    std::uint16_t mark = 0; ///< HFNUM when the retry was deferred.
    std::size_t hdr = 0;    ///< Size of the setup / zero-length area in front of the data in the bounce block.
    std::size_t key = 0;
    std::size_t length = 0;    ///< Data stage size requested by the transfer.
    std::size_t phase_len = 0; ///< Bytes the current stage moves.
    std::size_t done = 0;      ///< Bytes of the current stage moved so far.
    std::size_t moved = 0;     ///< Data stage bytes moved so far.
    std::size_t prog_len = 0;  ///< XferSize of the run in flight.
    std::size_t prog_pkts = 0; ///< PktCnt of the run in flight.
    table_slot *slot = nullptr;
    usb_dma_block<Env> bounce;
  };

  // ---- register helpers ----

  [[nodiscard]] std::uint32_t rd(std::size_t off) noexcept { return env_->read32(off); }
  void wr(std::size_t off, std::uint32_t v) noexcept { env_->write32(off, v); }
  [[nodiscard]] static constexpr std::size_t ch_reg(std::size_t c, std::size_t reg) noexcept {
    return dwc2::reg_hc_base + c * dwc2::reg_hc_stride + reg;
  }
  /** @brief HPRT value safe to write back: the write-1-to-clear bits (and the write-1-to-disable PrtEna) are 0. */
  [[nodiscard]] std::uint32_t hprt_base() noexcept { return rd(dwc2::reg_hprt) & ~dwc2::prt_w1c; }
  [[nodiscard]] static usb_speed speed_of(std::uint32_t hprt) noexcept {
    switch ((hprt >> dwc2::prt_spd_shift) & 3u) {
    case 0:
      return usb_speed::high;
    case 1:
      return usb_speed::full;
    default:
      return usb_speed::low;
    }
  }

  template <typename Pred> [[nodiscard]] bool spin_until(Pred pred) noexcept {
    for (unsigned i = 0; i < spin_limit; ++i)
      if (pred())
        return true;
    return false;
  }

  [[nodiscard]] reloco::result<void> core_reset() noexcept {
    if (!spin_until([&] { return (rd(dwc2::reg_grstctl) & dwc2::rst_ahbidle) != 0; }))
      return reloco::unexpected(reloco::error::timed_out);
    wr(dwc2::reg_grstctl, dwc2::rst_csftrst);
    if (!spin_until([&] { return (rd(dwc2::reg_grstctl) & dwc2::rst_csftrst) == 0; }))
      return reloco::unexpected(reloco::error::timed_out);
    if (!spin_until([&] { return (rd(dwc2::reg_grstctl) & dwc2::rst_ahbidle) != 0; }))
      return reloco::unexpected(reloco::error::timed_out);
    return {};
  }

  [[nodiscard]] bool flush_fifos() noexcept {
    wr(dwc2::reg_grstctl, dwc2::rst_txfflsh | (0x10u << dwc2::rst_txfnum_shift));
    if (!spin_until([&] { return (rd(dwc2::reg_grstctl) & dwc2::rst_txfflsh) == 0; }))
      return false;
    wr(dwc2::reg_grstctl, dwc2::rst_rxfflsh);
    return spin_until([&] { return (rd(dwc2::reg_grstctl) & dwc2::rst_rxfflsh) == 0; });
  }

  /** @brief Disables channel `c` and waits (bounded) for it to halt; true if it is idle afterwards. */
  [[nodiscard]] bool halt_channel(std::size_t c) noexcept {
    const std::uint32_t ch = rd(ch_reg(c, dwc2::hc_hcchar));
    if ((ch & dwc2::chr_chena) == 0)
      return true;
    wr(ch_reg(c, dwc2::hc_hcchar), ch | dwc2::chr_chena | dwc2::chr_chdis);
    return spin_until([&] { return (rd(ch_reg(c, dwc2::hc_hcchar)) & dwc2::chr_chena) == 0; });
  }

  // ---- clocks ----

  [[nodiscard]] std::uint32_t phy_clock_mhz() const noexcept {
    switch (cfg_.phy) {
    case dwc2_phy::utmi_16bit:
      return 30;
    case dwc2_phy::fs_serial:
      return 48;
    default:
      return 60;
    }
  }

  void program_clocks(usb_speed speed) noexcept {
    std::uint32_t sel = 0;      // 0: 30/60 MHz PHY clock, 1: 48 MHz, 2: 6 MHz
    std::uint32_t frame_clocks; // PHY clocks per (micro)frame
    if (speed == usb_speed::low) {
      sel = 2;
      frame_clocks = 6000;
    } else if (speed == usb_speed::full) {
      sel = cfg_.phy == dwc2_phy::fs_serial ? 1u : 0u;
      frame_clocks = 1000u * phy_clock_mhz();
    } else {
      frame_clocks = 125u * phy_clock_mhz();
    }
    wr(dwc2::reg_hcfg, (rd(dwc2::reg_hcfg) & ~dwc2::hcfg_fslspclksel_mask) | sel);
    wr(dwc2::reg_hfir, frame_clocks - 1u);
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

  /** @brief Control requests that reset device-side toggles. */
  void observe_control(std::size_t addr, const usb_setup_packet &s) noexcept {
    if (s.request_type == 0x00 && s.request == 5)
      clear_device_toggles(s.value & 0x7Fu);
    else if ((s.request_type == 0x00 && s.request == 9) || (s.request_type == 0x01 && s.request == 11))
      clear_device_toggles(addr);
    else if (s.request_type == 0x02 && s.request == 1 && s.value == 0)
      set_toggle(toggle_key(addr, s.index & 0xFu, (s.index & 0x80u) != 0), false);
  }

  // ---- running channels ----

  void begin_phase(resource &r, phase ph) noexcept {
    r.ph = ph;
    r.done = 0;
    r.errors = 0;
    r.ping = false;
    r.ping_run = false;
    r.deferred = false;
    switch (ph) {
    case phase::setup:
      r.phase_len = 8;
      break;
    case phase::data:
      r.phase_len = r.length;
      r.tog = r.ctl ? true : r.tog;
      break;
    case phase::status:
      r.phase_len = 0;
      r.tog = true;
      break;
    }
  }

  [[nodiscard]] std::size_t chunk_limit(const resource &r) const noexcept {
    if (r.type == usb_transfer_type::interrupt)
      return r.mps;
    std::size_t pkts = dwc2::tsiz_xfersize_mask / r.mps;
    pkts = pkts < max_packets_per_run ? pkts : max_packets_per_run;
    return pkts * r.mps;
  }

  /** @brief Programs channel `c` for the next run of `r`'s current stage and enables it. */
  void arm(resource &r, std::size_t c) noexcept {
    std::size_t len = 0;
    std::uint32_t pid = dwc2::pid_data0;
    std::uint64_t phys = r.bounce.phys();
    switch (r.ph) {
    case phase::setup:
      r.cur_in = false;
      len = 8;
      pid = dwc2::pid_setup;
      break;
    case phase::data: {
      r.cur_in = r.in;
      const std::size_t rem = r.phase_len - r.done;
      const std::size_t lim = chunk_limit(r);
      len = rem < lim ? rem : lim;
      pid = r.tog ? dwc2::pid_data1 : dwc2::pid_data0;
      if (len != 0)
        phys = r.bounce.phys_at(r.hdr + r.done);
      break;
    }
    case phase::status:
      r.cur_in = r.status_in;
      pid = dwc2::pid_data1;
      break;
    }
    const std::size_t pkts = len == 0 ? 1 : (len + r.mps - 1) / r.mps;
    r.prog_pkts = pkts;
    r.prog_len = r.cur_in ? pkts * r.mps : len;
    r.ping_run = r.ping && r.ph != phase::setup && !r.cur_in;
    r.deferred = false;

    const bool periodic = r.type == usb_transfer_type::interrupt;
    std::uint32_t chr = r.mps | (std::uint32_t{r.ep} << dwc2::chr_epnum_shift) | (r.cur_in ? dwc2::chr_epdir_in : 0u) |
                        (r.ls ? dwc2::chr_lspddev : 0u) |
                        (static_cast<std::uint32_t>(r.type) << dwc2::chr_eptype_shift) | dwc2::chr_mc_one |
                        (std::uint32_t{r.addr} << dwc2::chr_devaddr_shift);
    if (periodic && (((rd(dwc2::reg_hfnum) + 1u) & 1u) != 0))
      chr |= dwc2::chr_oddfrm;

    wr(ch_reg(c, dwc2::hc_hcint), 0xFFFF'FFFFu);
    wr(ch_reg(c, dwc2::hc_hcintmsk), dwc2::hci_chhltd | dwc2::hci_ahberr);
    wr(ch_reg(c, dwc2::hc_hcsplt), 0);
    wr(ch_reg(c, dwc2::hc_hctsiz), static_cast<std::uint32_t>(r.prog_len) |
                                       (static_cast<std::uint32_t>(pkts) << dwc2::tsiz_pktcnt_shift) |
                                       (pid << dwc2::tsiz_pid_shift) | (r.ping_run ? dwc2::tsiz_dopng : 0u));
    wr(ch_reg(c, dwc2::hc_hcdma), static_cast<std::uint32_t>(phys));
    env_->barrier();
    r.running = true;
    wr(ch_reg(c, dwc2::hc_hcchar), chr | dwc2::chr_chena);
  }

  void defer(resource &r) noexcept {
    r.deferred = true;
    r.mark = static_cast<std::uint16_t>(rd(dwc2::reg_hfnum));
  }

  /** @brief Re-arms the NAKed non-periodic channels once a new (micro)frame has started. */
  void service_deferred() noexcept {
    const auto fn = static_cast<std::uint16_t>(rd(dwc2::reg_hfnum));
    for (std::size_t c = 0; c < channels_; ++c) {
      resource &r = res_[c];
      if (r.stage == chan_stage::active && r.deferred && r.mark != fn)
        arm(r, c);
    }
  }

  void update_sof() noexcept {
    bool want = false;
    for (std::size_t c = 0; c < channels_; ++c)
      want = want || (res_[c].stage == chan_stage::active && res_[c].deferred);
    if (want == sof_enabled_)
      return;
    sof_enabled_ = want;
    gintmsk_ = want ? (gintmsk_ | dwc2::int_sof) : (gintmsk_ & ~dwc2::int_sof);
    wr(dwc2::reg_gintmsk, gintmsk_);
  }

  [[nodiscard]] bool port_down() noexcept {
    return (rd(dwc2::reg_hprt) & (dwc2::prt_conn_sts | dwc2::prt_ena)) != (dwc2::prt_conn_sts | dwc2::prt_ena);
  }

  /** @brief Evaluates a halted channel: updates progress and either re-arms, advances the stage or completes. */
  void service_channel(std::size_t c) noexcept {
    resource &r = res_[c];
    if (!r.running)
      return;
    const std::uint32_t h = rd(ch_reg(c, dwc2::hc_hcint));
    if ((h & dwc2::hci_chhltd) == 0)
      return;
    wr(ch_reg(c, dwc2::hc_hcint), h);
    r.running = false;

    const std::uint32_t ts = rd(ch_reg(c, dwc2::hc_hctsiz));
    const std::size_t left_bytes = ts & dwc2::tsiz_xfersize_mask;
    const std::size_t left_pkts = (ts >> dwc2::tsiz_pktcnt_shift) & dwc2::tsiz_pktcnt_mask;
    const std::size_t got_pkts = left_pkts <= r.prog_pkts ? r.prog_pkts - left_pkts : 0;
    std::size_t got_bytes = 0;
    if (r.cur_in)
      got_bytes = left_bytes <= r.prog_len ? r.prog_len - left_bytes : 0;
    else
      got_bytes = left_pkts == 0 ? r.prog_len : (got_pkts * r.mps < r.prog_len ? got_pkts * r.mps : r.prog_len);

    if ((h & dwc2::hci_ahberr) != 0) {
      retire(c, usb_status::bus_error);
      return;
    }
    if (r.cur_in && r.ph == phase::data && r.done + got_bytes > r.phase_len) {
      retire(c, usb_status::babble);
      return;
    }
    r.done += got_bytes;
    if (r.ph == phase::data)
      r.moved += got_bytes;
    r.tog = r.tog != ((got_pkts & 1u) != 0);

    if ((h & dwc2::hci_stall) != 0) {
      retire(c, usb_status::stall);
      return;
    }
    if ((h & dwc2::hci_bblerr) != 0) {
      retire(c, usb_status::babble);
      return;
    }
    const bool explained = (h & (dwc2::hci_nak | dwc2::hci_nyet | dwc2::hci_ack | dwc2::hci_xfercompl)) != 0;
    const bool xact = (h & dwc2::hci_xacterr) != 0;
    const bool failed = xact || (h & (dwc2::hci_frmovrun | dwc2::hci_datatglerr)) != 0 || (!explained && got_pkts == 0);
    if (got_pkts != 0)
      r.errors = 0;
    if (failed) {
      if (++r.errors >= max_errors) {
        const bool gone = device_port_up_ && port_down();
        retire(c, gone ? usb_status::disconnected : (xact ? usb_status::timeout : usb_status::bus_error));
      } else {
        arm(r, c);
      }
      return;
    }
    r.errors = 0;

    const bool compl_ = (h & dwc2::hci_xfercompl) != 0;
    const bool short_in = compl_ && r.cur_in && got_bytes < r.prog_len;
    const bool stage_done = r.phase_len == 0 ? got_pkts != 0 : (short_in || r.done >= r.phase_len);
    if (stage_done) {
      advance(r, c);
      return;
    }

    const bool nak = (h & dwc2::hci_nak) != 0;
    const bool slow_out = !r.cur_in && r.hs && r.type != usb_transfer_type::interrupt && r.ph != phase::setup;
    if (slow_out && (nak || (h & dwc2::hci_nyet) != 0))
      r.ping = true;
    else if (r.ping_run && (h & dwc2::hci_ack) != 0 && got_pkts == 0)
      r.ping = false;
    if (nak && r.type != usb_transfer_type::interrupt)
      defer(r);
    else
      arm(r, c);
  }

  void advance(resource &r, std::size_t c) noexcept {
    if (!r.ctl || r.ph == phase::status) {
      retire(c, usb_status::ok);
      return;
    }
    if (r.ph == phase::setup && r.length != 0)
      begin_phase(r, phase::data);
    else
      begin_phase(r, phase::status);
    arm(r, c);
  }

  /** @brief Finishes bookkeeping for the transfer on halted channel `c`, then (last) completes it. */
  void retire(std::size_t c, usb_status status) noexcept {
    resource &r = res_[c];
    usb_transfer *t = r.slot->xfer;
    const std::size_t actual = r.moved;
    if (r.in && r.bounce && actual != 0)
      r.bounce.copy_out(r.hdr, reloco::span<std::uint8_t>(static_cast<std::uint8_t *>(t->data), actual));
    if (!r.ctl)
      set_toggle(r.key, r.tog);
    else if (status == usb_status::ok)
      observe_control(t->pipe.address, t->setup);
    table_.release(*r.slot);
    r = resource{};
    t->complete(status, actual);
  }

  /** @brief Recycles a cancelled/aborted channel once it is idle; keeps the toggle of the packets that got through. */
  void finish_zombie(std::size_t c) noexcept {
    resource &r = res_[c];
    if ((rd(ch_reg(c, dwc2::hc_hcchar)) & dwc2::chr_chena) != 0)
      return;
    if (!r.ctl) {
      bool tog = r.tog;
      if (r.running) {
        const std::uint32_t ts = rd(ch_reg(c, dwc2::hc_hctsiz));
        const std::size_t left = (ts >> dwc2::tsiz_pktcnt_shift) & dwc2::tsiz_pktcnt_mask;
        tog = tog != (((r.prog_pkts >= left ? r.prog_pkts - left : 0) & 1u) != 0);
      }
      set_toggle(r.key, tog);
    }
    wr(ch_reg(c, dwc2::hc_hcint), 0xFFFF'FFFFu);
    r = resource{};
  }

  /** @brief Completes every transfer in flight with `status`; their channels are halted first. */
  void fail_active(usb_status status) noexcept {
    for (std::size_t c = 0; c < channels_; ++c) {
      resource &r = res_[c];
      if (r.stage != chan_stage::active)
        continue;
      if (halt_channel(c)) {
        wr(ch_reg(c, dwc2::hc_hcint), 0xFFFF'FFFFu);
        r.running = false;
        retire(c, status);
      } else {
        // The channel did not halt in time: the transfer ends, the channel and its memory become a zombie.
        usb_transfer *t = r.slot->xfer;
        const std::size_t actual = r.moved;
        table_.release(*r.slot);
        r.slot = nullptr;
        r.stage = chan_stage::halting;
        t->complete(status, actual);
      }
    }
  }

  // ---- root port ----

  /** @brief Latches connect changes seen in an HPRT value. */
  void observe_port(std::uint32_t s) noexcept {
    const bool conn = (s & dwc2::prt_conn_sts) != 0;
    if ((s & dwc2::prt_conn_det) != 0 || conn != last_conn_)
      changed_ = true;
    last_conn_ = conn;
    const std::uint32_t ack = s & (dwc2::prt_conn_det | dwc2::prt_en_chng | dwc2::prt_ovrcurr_chng);
    if (ack != 0)
      wr(dwc2::reg_hprt, (s & ~dwc2::prt_w1c) | ack);
  }

  void scan_ports(bool disconnect_irq) noexcept {
    const std::uint32_t s = rd(dwc2::reg_hprt);
    observe_port(s);
    if (disconnect_irq)
      changed_ = true;
    if ((s & dwc2::prt_conn_sts) == 0)
      fail_active(usb_status::disconnected);
    if (hook_)
      hook_(hook_ctx_);
  }

  Env *env_;
  dwc2_config cfg_;
  table_type table_;
  reloco::array<resource, MaxTransfers> res_{};
  reloco::array<std::uint8_t, 512> toggles_{};
  std::size_t channels_ = 0;    ///< Channels the driver uses.
  std::size_t hw_channels_ = 0; ///< Channels the core has.
  std::uint32_t gintmsk_ = 0;
  bool changed_ = false;
  bool last_conn_ = false;
  bool device_port_up_ = false; ///< `reset_port` brought the port up at least once since `start()`.
  bool power_wait_pending_ = false;
  bool started_ = false;
  bool dead_ = false;
  bool sof_enabled_ = false;
  void (*hook_)(void *ctx) noexcept = nullptr;
  void *hook_ctx_ = nullptr;
};

/** @brief Plugs `dwc2_hcd` into `usb_host_controller_ref`. */
template <typename Env, std::size_t N> struct usb_host_traits<dwc2_hcd<Env, N>> {
  static unsigned port_count(dwc2_hcd<Env, N> &h) noexcept { return h.port_count(); }
  static reloco::result<usb_port_status> port_status(dwc2_hcd<Env, N> &h, unsigned port) noexcept {
    return h.port_status(port);
  }
  static reloco::task<void> reset_port(dwc2_hcd<Env, N> &h, unsigned port) noexcept { return h.reset_port(port); }
  static reloco::result<void> submit(dwc2_hcd<Env, N> &h, usb_transfer &t) noexcept { return h.submit(t); }
  static void cancel(dwc2_hcd<Env, N> &h, usb_transfer &t) noexcept { h.cancel(t); }
  static void reset_data_toggle(dwc2_hcd<Env, N> &h, const usb_pipe &p) noexcept { h.reset_data_toggle(p); }
};

} // namespace structo::hw

#endif // RELOCO_HAS_COROUTINES
