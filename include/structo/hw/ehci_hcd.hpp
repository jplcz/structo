// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file ehci_hcd.hpp
 * @brief `structo::hw::ehci_hcd<Env, MaxTransfers>`: a generic EHCI 1.0 (USB 2.0 high-speed) host controller
 * driver. Header-only, allocation-free, C++20 only (empty otherwise). See docs/ehci_hcd.md.
 *
 * The driver only talks to the controller through an `Env` (see usb_hcd_env.hpp): MMIO registers, coherent
 * DMA memory, a barrier and a millisecond delay. It implements `usb_host_traits`, so a board needs only:
 *
 * @code
 * my_usb_env env{...};                           // board glue: register window, DMA arena, timer delay
 * structo::hw::ehci_hcd<my_usb_env, 16> hcd{env};// 16 = how many transfers may be in flight at once
 * if (!hcd.start()) { ... }                      // halt + reset the controller, build the schedules, run
 * structo::hw::usb_host_controller_ref ref{hcd}; // type-erased handle for usb::usb_host / bootldr::usb_stack
 * // ... from the controller's interrupt handler (or periodically from a polling loop):
 * hcd.irq();                                     // acknowledges the controller, completes finished transfers
 * @endcode
 *
 * Design (all of it is also summarised in docs/ehci_hcd.md):
 * - **Scope**: high-speed devices on the root ports; control, bulk and interrupt transfers. There are no
 *   split transactions and no hubs, so full/low-speed devices cannot be driven here: they are handed to
 *   the companion controller (OHCI/UHCI) by setting `PORT_OWNER`. Isochronous transfers are not supported.
 * - **BIOS handoff** (the EHCI legacy-support extended capability, `HCCPARAMS.EECP`) lives in PCI config
 *   space, which `Env` cannot reach: the platform must do it before `start()` (`eecp()` returns the offset).
 * - **Memory**: one 4 KiB aligned DMA block holds the 1024-entry periodic frame list (the default size,
 *   valid for fixed and programmable controllers), the async/periodic dummy queue heads and a pool of
 *   `2 * MaxTransfers` jobs (queue head + 16 qTDs + a setup-packet scratch area each). Every payload goes
 *   through a per-transfer bounce DMA buffer, so `usb_transfer::data` need not be DMA-capable. All DMA
 *   memory must lie below 4 GiB (the structures use 32-bit pointers; `CTRLDSSEGMENT` is programmed to 0
 *   on 64-bit capable controllers); `start()`/`submit()` fail with `unsupported_operation` otherwise.
 *   The qTD/QH layouts include the 64-bit extension dwords (always zero), which is why a qTD is 64 bytes
 *   and a QH 128 bytes in the pool.
 * - **Async schedule** (control/bulk): a permanently linked dummy QH with the H (head of reclamation) bit;
 *   each transfer owns one QH inserted right after it. Data toggles come from the qTDs (DTC = 1) driven by
 *   a software toggle table per (address, endpoint, direction). A transfer is split into qTDs of at most
 *   5 pages (<= 20 KiB), each non-final qTD a multiple of `max_packet`. A short IN packet in a multi-qTD
 *   control transfer skips to the status stage through the Alternate Next qTD pointer.
 * - **Periodic schedule** (interrupt): every frame list entry points to one dummy QH (S-mask 0) whose
 *   horizontal chain carries the interrupt QHs with S-mask 0x01. Each pending interrupt transfer is thus
 *   polled once per 1 ms frame regardless of the endpoint's interval (always at least as often as the
 *   endpoint asks for); the transfer is one-shot.
 * - **Safe unlinking**: finished or cancelled QHs are unlinked, then kept as zombies. Async QHs are freed
 *   after the Interrupt on Async Advance doorbell handshake (IAAD -> USBSTS.IAA), periodic QHs after
 *   FRINDEX moved to the next frame. The usb_transfer is never touched after `cancel()`.
 * - **Completion** is found by walking the qTDs of active transfers in `irq()`/`poll()`, so polled
 *   operation needs no interrupt at all.
 *
 * The descriptors are little-endian structures in coherent memory and are accessed through
 * `usb_dma_block::at<T>()`; the CPU must be little-endian.
 */

#include <reloco/coroutine.hpp>
#include <reloco/detail/compat.hpp>

#if RELOCO_HAS_COROUTINES

#include "usb_hcd_env.hpp"
#include "usb_host_controller_ref.hpp"

#include <reloco/array.hpp>
#include <reloco/error.hpp>
#include <reloco/span.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace structo::hw {

namespace ehci {

static_assert(std::endian::native == std::endian::little, "EHCI descriptors are little-endian; the CPU must be too");

/// Capability register offsets (from the start of the MMIO window).
inline constexpr std::size_t cap_caplength = 0x00; ///< bits 7:0 CAPLENGTH, 31:16 HCIVERSION
inline constexpr std::size_t cap_hcsparams = 0x04;
inline constexpr std::size_t cap_hccparams = 0x08;

/// Operational register offsets (from CAPLENGTH).
inline constexpr std::size_t op_usbcmd = 0x00;
inline constexpr std::size_t op_usbsts = 0x04;
inline constexpr std::size_t op_usbintr = 0x08;
inline constexpr std::size_t op_frindex = 0x0C;
inline constexpr std::size_t op_ctrldssegment = 0x10;
inline constexpr std::size_t op_periodiclistbase = 0x14;
inline constexpr std::size_t op_asynclistaddr = 0x18;
inline constexpr std::size_t op_configflag = 0x40;
inline constexpr std::size_t op_portsc = 0x44; ///< PORTSC[0]; one dword per port.

inline constexpr std::uint32_t cmd_rs = 1u << 0;
inline constexpr std::uint32_t cmd_hcreset = 1u << 1;
inline constexpr std::uint32_t cmd_pse = 1u << 4;
inline constexpr std::uint32_t cmd_ase = 1u << 5;
inline constexpr std::uint32_t cmd_iaad = 1u << 6;
inline constexpr std::uint32_t cmd_itc_8 = 8u << 16; ///< Interrupt threshold: 8 micro-frames (default).

inline constexpr std::uint32_t sts_usbint = 1u << 0;
inline constexpr std::uint32_t sts_usberrint = 1u << 1;
inline constexpr std::uint32_t sts_pcd = 1u << 2;
inline constexpr std::uint32_t sts_flr = 1u << 3;
inline constexpr std::uint32_t sts_hse = 1u << 4;
inline constexpr std::uint32_t sts_iaa = 1u << 5;
inline constexpr std::uint32_t sts_int_mask = 0x3F;
inline constexpr std::uint32_t sts_halted = 1u << 12;
inline constexpr std::uint32_t sts_pss = 1u << 14;
inline constexpr std::uint32_t sts_ass = 1u << 15;

inline constexpr std::uint32_t hcs_ppc = 1u << 4;
inline constexpr std::uint32_t hcc_ac64 = 1u << 0;
inline constexpr std::uint32_t hcc_pflf = 1u << 1;

inline constexpr std::uint32_t port_ccs = 1u << 0;
inline constexpr std::uint32_t port_csc = 1u << 1;
inline constexpr std::uint32_t port_ped = 1u << 2;
inline constexpr std::uint32_t port_pedc = 1u << 3;
inline constexpr std::uint32_t port_occ = 1u << 5;
inline constexpr std::uint32_t port_pr = 1u << 8;
inline constexpr std::uint32_t port_ls_shift = 10;
inline constexpr std::uint32_t port_ls_mask = 3u << port_ls_shift;
inline constexpr std::uint32_t port_ls_k = 1u << port_ls_shift; ///< K state: a low-speed device.
inline constexpr std::uint32_t port_pp = 1u << 12;
inline constexpr std::uint32_t port_owner = 1u << 13;
inline constexpr std::uint32_t port_rwc_mask = port_csc | port_pedc | port_occ; ///< write-1-to-clear bits.

inline constexpr std::uint32_t link_terminate = 1;
inline constexpr std::uint32_t link_type_qh = 1u << 1;

inline constexpr std::uint32_t tok_mmf = 1u << 2;
inline constexpr std::uint32_t tok_xacterr = 1u << 3;
inline constexpr std::uint32_t tok_babble = 1u << 4;
inline constexpr std::uint32_t tok_dbe = 1u << 5;
inline constexpr std::uint32_t tok_halted = 1u << 6;
inline constexpr std::uint32_t tok_active = 1u << 7;
inline constexpr std::uint32_t pid_out = 0;
inline constexpr std::uint32_t pid_in = 1;
inline constexpr std::uint32_t pid_setup = 2;
inline constexpr std::uint32_t tok_pid_shift = 8;
inline constexpr std::uint32_t tok_cerr_3 = 3u << 10;
inline constexpr std::uint32_t tok_ioc = 1u << 15;
inline constexpr std::uint32_t tok_bytes_shift = 16;
inline constexpr std::uint32_t tok_bytes_mask = 0x7FFF;
inline constexpr std::uint32_t tok_dt = 1u << 31;

inline constexpr std::uint32_t qh_h = 1u << 15;       ///< Head of reclamation list.
inline constexpr std::uint32_t qh_dtc = 1u << 14;     ///< Data toggle comes from the qTD.
inline constexpr std::uint32_t qh_eps_high = 2u << 12;
inline constexpr std::uint32_t qh_rl_shift = 28;      ///< NAK count reload.
inline constexpr std::uint32_t qh_mult_1 = 1u << 30;

inline constexpr std::size_t frame_list_entries = 1024;
inline constexpr std::size_t qtd_max_pages = 5;
inline constexpr std::size_t page_size = 4096;

/** @brief Queue element transfer descriptor, including the 64-bit extension dwords; padded to 64 bytes. */
struct qtd {
  std::uint32_t next = link_terminate;
  std::uint32_t alt_next = link_terminate;
  std::uint32_t token = 0;
  reloco::array<std::uint32_t, 5> buffer{};
  reloco::array<std::uint32_t, 5> buffer_hi{};
  reloco::array<std::uint32_t, 3> pad{};
};
static_assert(sizeof(qtd) == 64 && alignof(qtd) == alignof(std::uint32_t) && std::is_standard_layout_v<qtd>);

/** @brief Queue head with the embedded qTD overlay and the 64-bit extension dwords; padded to 128 bytes. */
struct qh {
  std::uint32_t horizontal = link_terminate;
  std::uint32_t ep_char = 0;
  std::uint32_t ep_caps = 0;
  std::uint32_t current_qtd = 0;
  std::uint32_t ov_next = link_terminate;
  std::uint32_t ov_alt_next = link_terminate;
  std::uint32_t ov_token = 0;
  reloco::array<std::uint32_t, 5> ov_buffer{};
  reloco::array<std::uint32_t, 5> ov_buffer_hi{};
  reloco::array<std::uint32_t, 15> pad{};
};
static_assert(sizeof(qh) == 128 && alignof(qh) == alignof(std::uint32_t) && std::is_standard_layout_v<qh>);

/** @brief The periodic frame list. */
struct frame_list {
  reloco::array<std::uint32_t, frame_list_entries> entry{};
};
static_assert(sizeof(frame_list) == 4096);

namespace detail {

/// Reads a descriptor dword the controller may have changed behind the compiler's back.
[[nodiscard]] inline std::uint32_t load(const std::uint32_t &v) noexcept {
  return static_cast<const volatile std::uint32_t &>(v);
}
inline void store(std::uint32_t &d, std::uint32_t v) noexcept { static_cast<volatile std::uint32_t &>(d) = v; }

} // namespace detail

} // namespace ehci

/**
 * @brief Generic EHCI host controller driver.
 * @tparam Env the platform layer, see usb_hcd_env.hpp.
 * @tparam MaxTransfers number of transfers that may be in flight at once (an extra `MaxTransfers` job
 *   slots absorb unlinked queue heads that wait for the controller to let go of them).
 */
template <typename Env, std::size_t MaxTransfers = 16> class ehci_hcd {
  static_assert(is_usb_hcd_env_v<Env>, "Env does not satisfy the USB host controller Env contract");
  static_assert(MaxTransfers >= 1 && MaxTransfers <= 128);

public:
  /// Number of qTDs in a job: a control transfer uses setup + data + status, so data is limited to
  /// `(max_qtds - 2) * 20 KiB` (control) or `max_qtds * 20 KiB` (bulk/interrupt) per transfer.
  static constexpr std::size_t max_qtds = 16;

  explicit ehci_hcd(Env &env) noexcept : env_(&env) {}
  ehci_hcd(const ehci_hcd &) = delete;
  ehci_hcd &operator=(const ehci_hcd &) = delete;
  ~ehci_hcd() { stop(); }

  /**
   * @brief Halts and resets the controller, builds both schedules and starts it; routes all ports to EHCI.
   * Errors: `unsupported_operation` (not an EHCI window, memory above 4 GiB), `timed_out` (the controller
   * did not halt/reset/start in a bounded number of register reads), `allocation_failed`, `invalid_state`
   * (already started).
   */
  [[nodiscard]] reloco::result<void> start() noexcept {
    if (started_)
      return reloco::unexpected(reloco::error::invalid_state);
    const std::uint32_t cap = env_->read32(ehci::cap_caplength);
    cap_length_ = cap & 0xFF;
    const std::uint32_t version = cap >> 16;
    hcs_ = env_->read32(ehci::cap_hcsparams);
    hcc_ = env_->read32(ehci::cap_hccparams);
    n_ports_ = hcs_ & 0xF;
    if (cap_length_ < 0x10 || version == 0 || n_ports_ == 0)
      return reloco::unexpected(reloco::error::unsupported_operation);

    // Halt: RS = 0 and wait for HCHalted.
    if (!(op_read(ehci::op_usbsts) & ehci::sts_halted)) {
      op_write(ehci::op_usbcmd, op_read(ehci::op_usbcmd) & ~ehci::cmd_rs);
      if (!wait_op(ehci::op_usbsts, ehci::sts_halted, ehci::sts_halted))
        return reloco::unexpected(reloco::error::timed_out);
    }
    // Reset: HCRESET self-clears; the controller comes back halted with every port owned by a companion.
    op_write(ehci::op_usbcmd, ehci::cmd_hcreset);
    if (!wait_op(ehci::op_usbcmd, ehci::cmd_hcreset, 0))
      return reloco::unexpected(reloco::error::timed_out);

    if (!mem_.allocate(*env_, total_size, ehci::page_size))
      return reloco::unexpected(reloco::error::allocation_failed);
    if (!below_4g(mem_.phys(), mem_.size())) {
      mem_.reset();
      return reloco::unexpected(reloco::error::unsupported_operation);
    }
    init_schedules();
    for (auto &j : jobs_)
      j = job{};
    for (auto &m : tog_in_)
      m = 0;
    for (auto &m : tog_out_)
      m = 0;
    active_ = 0;
    changed_mask_ = 0;
    iaa_pending_ = false;
    failed_ = false;

    if (hcc_ & ehci::hcc_ac64)
      op_write(ehci::op_ctrldssegment, 0);
    op_write(ehci::op_periodiclistbase, phys32(frame_list_off));
    op_write(ehci::op_asynclistaddr, phys32(async_dummy_off));
    env_->barrier();
    op_write(ehci::op_usbsts, ehci::sts_int_mask);
    op_write(ehci::op_usbintr, ehci::sts_usbint | ehci::sts_usberrint | ehci::sts_pcd | ehci::sts_hse | ehci::sts_iaa);
    op_write(ehci::op_usbcmd, ehci::cmd_itc_8 | ehci::cmd_ase | ehci::cmd_pse | ehci::cmd_rs);
    constexpr std::uint32_t running = ehci::sts_halted | ehci::sts_ass | ehci::sts_pss;
    if (!wait_op(ehci::op_usbsts, running, ehci::sts_ass | ehci::sts_pss)) {
      op_write(ehci::op_usbcmd, 0);
      mem_.reset();
      return reloco::unexpected(reloco::error::timed_out);
    }
    op_write(ehci::op_configflag, 1); // route all ports to EHCI
    if (hcs_ & ehci::hcs_ppc)
      for (unsigned p = 0; p < n_ports_; ++p)
        port_update(p, 0, ehci::port_pp, 0);
    started_ = true;
    return {};
  }

  /** @brief Stops the controller; pending transfers complete with `usb_status::cancelled`. Safe to call twice. */
  void stop() noexcept {
    if (!started_)
      return;
    started_ = false;
    op_write(ehci::op_usbintr, 0);
    op_write(ehci::op_usbcmd, 0);
    (void)wait_op(ehci::op_usbsts, ehci::sts_halted, ehci::sts_halted);
    op_write(ehci::op_configflag, 0);
    fail_all(usb_status::cancelled);
    mem_.reset();
  }

  /** @brief Interrupt handler body; also callable periodically from a polled loop. */
  void irq() noexcept {
    if (!started_ || in_irq_)
      return;
    in_irq_ = true;
    const std::uint32_t sts = op_read(ehci::op_usbsts) & ehci::sts_int_mask;
    if (sts)
      op_write(ehci::op_usbsts, sts);
    if (sts & ehci::sts_hse)
      failed_ = true;
    if (sts & ehci::sts_iaa)
      on_async_advance();
    if (sts & ehci::sts_pcd)
      scan_ports();
    reap_periodic();
    if (failed_)
      fail_all(usb_status::bus_error);
    else
      scan_jobs();
    in_irq_ = false;
  }
  /** @brief Alias of `irq()` for polled operation. */
  void poll() noexcept { irq(); }

  // ---- information ----

  [[nodiscard]] bool started() const noexcept { return started_; }
  /// True after a Host System Error: the controller is dead until `stop()` + `start()`.
  [[nodiscard]] bool failed() const noexcept { return failed_; }
  [[nodiscard]] std::size_t active_count() const noexcept { return active_; }
  /// Unlinked queue heads still waiting for the controller to let go of them.
  [[nodiscard]] std::size_t zombie_count() const noexcept {
    std::size_t n = 0;
    for (const auto &j : jobs_)
      n += j.state != job_state::free && j.state != job_state::active;
    return n;
  }
  /// HCSPARAMS.N_CC: number of companion controllers.
  [[nodiscard]] unsigned companion_controllers() const noexcept { return (hcs_ >> 12) & 0xF; }
  /// HCSPARAMS.N_PCC: ports per companion controller.
  [[nodiscard]] unsigned ports_per_companion() const noexcept { return (hcs_ >> 8) & 0xF; }
  [[nodiscard]] bool addressing_64() const noexcept { return (hcc_ & ehci::hcc_ac64) != 0; }
  /// HCCPARAMS.EECP: PCI config space offset of the extended capabilities (0 = none); BIOS handoff is the
  /// platform's job and must be done before `start()`.
  [[nodiscard]] unsigned eecp() const noexcept { return (hcc_ >> 8) & 0xFF; }

  // ---- usb_host_traits entry points ----

  [[nodiscard]] unsigned port_count() const noexcept { return n_ports_; }

  /** @brief Reads PORTSC; low-speed devices are released to the companion controller (and reported absent). */
  [[nodiscard]] reloco::result<usb_port_status> port_status(unsigned port) noexcept {
    if (!started_ || port >= n_ports_)
      return reloco::unexpected(reloco::error::invalid_argument);
    std::uint32_t sc = port_read(port);
    bool changed = latch_changed(port, sc);
    usb_port_status s;
    s.speed = usb_speed::high;
    if (release_if_low_speed(port, sc) || (sc & ehci::port_owner)) {
      s.changed = changed;
      return s;
    }
    s.connected = (sc & ehci::port_ccs) != 0;
    s.enabled = s.connected && (sc & ehci::port_ped) != 0;
    s.changed = changed;
    return s;
  }

  /**
   * @brief Resets a port: PR for 50 ms, release, <= 2 ms for the controller to finish, 10 ms recovery.
   * If the port is not enabled afterwards the device is full-speed: it is released to the companion
   * controller and the task fails with `unsupported_operation`.
   */
  [[nodiscard]] reloco::task<void> reset_port(unsigned port) noexcept {
    if (!started_ || port >= n_ports_)
      co_await reloco::unexpected(reloco::error::invalid_argument);
    std::uint32_t sc = port_read(port);
    if (!(sc & ehci::port_ccs) || (sc & ehci::port_owner))
      co_await reloco::unexpected(reloco::error::not_found);
    port_update(port, ehci::port_ped, ehci::port_pr, 0);
    (void)co_await env_->delay_ms(50);
    port_update(port, ehci::port_pr, 0, 0);
    for (unsigned i = 0; i < 6 && (port_read(port) & ehci::port_pr); ++i)
      (void)co_await env_->delay_ms(1);
    if (port_read(port) & ehci::port_pr)
      co_await reloco::unexpected(reloco::error::timed_out);
    (void)co_await env_->delay_ms(10);
    sc = port_read(port);
    if (!(sc & ehci::port_ccs))
      co_await reloco::unexpected(reloco::error::not_found);
    if (!(sc & ehci::port_ped)) {
      port_update(port, 0, ehci::port_owner, 0);
      co_await reloco::unexpected(reloco::error::unsupported_operation);
    }
    port_update(port, 0, 0, ehci::port_csc); // the reset itself is not a (dis)connect
  }

  /**
   * @brief Queues a control, bulk or interrupt transfer. Errors: `invalid_state`, `io_error` (controller
   * failed), `unsupported_operation` (not high-speed / isochronous / memory above 4 GiB), `invalid_argument`,
   * `capacity_exceeded` (needs more than `max_qtds` qTDs), `try_again` (MaxTransfers in flight),
   * `busy` (another bulk/interrupt transfer is in flight on the same endpoint), `allocation_failed`.
   */
  [[nodiscard]] reloco::result<void> submit(usb_transfer &t) noexcept {
    if (!started_)
      return reloco::unexpected(reloco::error::invalid_state);
    if (failed_)
      return reloco::unexpected(reloco::error::io_error);
    const usb_pipe &p = t.pipe;
    if (p.type == usb_transfer_type::isochronous || p.speed != usb_speed::high)
      return reloco::unexpected(reloco::error::unsupported_operation);
    const std::size_t mps = p.max_packet & 0x7FFu;
    if (p.address > 127 || p.endpoint > 15 || mps == 0 || (t.length > 0 && t.data == nullptr))
      return reloco::unexpected(reloco::error::invalid_argument);
    const bool control = p.type == usb_transfer_type::control;
    const bool in = t.is_in();
    std::size_t len = t.length;
    if (control)
      len = t.setup.length == 0 ? 0 : (len < t.setup.length ? len : std::size_t{t.setup.length});
    if (active_ >= MaxTransfers)
      return reloco::unexpected(reloco::error::try_again);
    if (!control && endpoint_busy(p.address, p.endpoint, in))
      return reloco::unexpected(reloco::error::busy);
    std::size_t ji = jobs_.size();
    for (std::size_t i = 0; i < jobs_.size(); ++i)
      if (jobs_[i].state == job_state::free) {
        ji = i;
        break;
      }
    if (ji == jobs_.size())
      return reloco::unexpected(reloco::error::try_again);

    job &j = jobs_[ji];
    j = job{};
    if (len > 0) {
      if (!j.bounce.allocate(*env_, len, ehci::page_size))
        return reloco::unexpected(reloco::error::allocation_failed);
      if (!below_4g(j.bounce.phys(), len)) {
        j.bounce.reset();
        return reloco::unexpected(reloco::error::unsupported_operation);
      }
      if (!in)
        j.bounce.copy_in(0, reloco::span<const std::uint8_t>(static_cast<const std::uint8_t *>(t.data), len));
    }
    const std::uint64_t base = len > 0 ? j.bounce.phys() : 0;
    const std::size_t data_qtds = count_chunks(base, len, mps, !control);
    if (data_qtds + (control ? 2 : 0) > max_qtds) {
      j = job{};
      return reloco::unexpected(reloco::error::capacity_exceeded);
    }

    j.periodic = p.type == usb_transfer_type::interrupt;
    j.control = control;
    j.in = in;
    j.address = p.address;
    j.endpoint = p.endpoint;
    j.max_packet = static_cast<std::uint16_t>(mps);
    j.length = len;
    j.data_first = control ? 1 : 0;
    j.data_count = static_cast<std::uint8_t>(data_qtds);
    j.qtd_count = static_cast<std::uint8_t>(data_qtds + (control ? 2 : 0));
    j.toggle_start = control || toggle(p.address, p.endpoint, in);
    j.xfer = &t;
    build_qtds(ji, t, base);
    build_qh(ji);
    env_->barrier();
    link_job(ji);
    j.state = job_state::active;
    ++active_;
    return {};
  }

  /** @brief Withdraws `t`: its queue head is unlinked and recycled once the controller has let go of it. */
  void cancel(usb_transfer &t) noexcept {
    if (!started_)
      return;
    for (std::size_t i = 0; i < jobs_.size(); ++i)
      if (jobs_[i].state == job_state::active && jobs_[i].xfer == &t) {
        unlink_job(i);
        return;
      }
  }

  /** @brief Resets the software data toggle of an endpoint (CLEAR_FEATURE(HALT)/SET_INTERFACE). */
  void reset_data_toggle(const usb_pipe &p) noexcept {
    if (p.address < 128 && p.endpoint < 16)
      set_toggle(p.address, p.endpoint, p.direction == usb_direction::in, false);
  }

private:
  enum class job_state : std::uint8_t {
    free,
    active,
    unlinked_async,   ///< off the async ring; waiting for the doorbell to be rung
    doorbell_async,   ///< doorbell rung; freed on USBSTS.IAA
    unlinked_periodic ///< off the periodic chain; freed once the frame number moved on
  };

  struct job {
    job_state state = job_state::free;
    bool periodic = false;
    bool control = false;
    bool in = false;
    bool toggle_start = false;
    std::uint8_t address = 0;
    std::uint8_t endpoint = 0;
    std::uint8_t qtd_count = 0;
    std::uint8_t data_first = 0;
    std::uint8_t data_count = 0;
    std::uint16_t max_packet = 0;
    std::uint16_t frame_stamp = 0;
    std::size_t length = 0;
    usb_transfer *xfer = nullptr;
    usb_dma_block<Env> bounce;
    reloco::array<std::uint16_t, max_qtds> qtd_len{};
  };

  struct outcome {
    bool done = false;
    usb_status status = usb_status::ok;
    std::size_t actual = 0;
    std::size_t packets = 0;
  };

  static constexpr std::size_t job_count = 2 * MaxTransfers;
  static constexpr std::size_t frame_list_off = 0;
  static constexpr std::size_t async_dummy_off = sizeof(ehci::frame_list);
  static constexpr std::size_t periodic_dummy_off = async_dummy_off + sizeof(ehci::qh);
  static constexpr std::size_t jobs_off = periodic_dummy_off + sizeof(ehci::qh);
  static constexpr std::size_t qtds_in_job_off = sizeof(ehci::qh);
  static constexpr std::size_t scratch_in_job_off = qtds_in_job_off + max_qtds * sizeof(ehci::qtd);
  static constexpr std::size_t job_stride = scratch_in_job_off + 64;
  static constexpr std::size_t total_size = jobs_off + job_count * job_stride;
  static constexpr std::size_t no_offset = ~std::size_t{0};
  static constexpr std::size_t spin_limit = 200000;
  static_assert(job_stride % 32 == 0 && jobs_off % 32 == 0);

  // ---- registers ----

  [[nodiscard]] std::uint32_t op_read(std::size_t reg) noexcept { return env_->read32(cap_length_ + reg); }
  void op_write(std::size_t reg, std::uint32_t v) noexcept { env_->write32(cap_length_ + reg, v); }
  [[nodiscard]] bool wait_op(std::size_t reg, std::uint32_t mask, std::uint32_t value) noexcept {
    for (std::size_t i = 0; i < spin_limit; ++i)
      if ((op_read(reg) & mask) == value)
        return true;
    return false;
  }
  [[nodiscard]] std::uint32_t port_read(unsigned port) noexcept { return op_read(ehci::op_portsc + 4u * port); }

  /// Read-modify-write of PORTSC: clears `clear`, sets `set`, acknowledges the write-1-to-clear bits in `ack`;
  /// the other write-1-to-clear bits are written as 0 so pending status changes are not lost.
  void port_update(unsigned port, std::uint32_t clear, std::uint32_t set, std::uint32_t ack) noexcept {
    std::uint32_t v = port_read(port);
    v &= ~(ehci::port_rwc_mask | clear);
    v |= set | ack;
    op_write(ehci::op_portsc + 4u * port, v);
  }

  /// Folds PORTSC.CSC and the latch set by irq() into one "changed" flag and clears both.
  [[nodiscard]] bool latch_changed(unsigned port, std::uint32_t sc) noexcept {
    const std::uint32_t bit = 1u << port;
    bool changed = (changed_mask_ & bit) != 0;
    changed_mask_ &= ~bit;
    if (sc & ehci::port_csc) {
      changed = true;
      port_update(port, 0, 0, ehci::port_csc);
    }
    return changed;
  }

  /// A device that is connected but not enabled and shows the K line state is low-speed: hand the port over.
  bool release_if_low_speed(unsigned port, std::uint32_t sc) noexcept {
    if ((sc & ehci::port_ccs) && !(sc & (ehci::port_ped | ehci::port_owner)) &&
        (sc & ehci::port_ls_mask) == ehci::port_ls_k) {
      port_update(port, 0, ehci::port_owner, 0);
      return true;
    }
    return false;
  }

  void scan_ports() noexcept {
    for (unsigned p = 0; p < n_ports_; ++p) {
      const std::uint32_t sc = port_read(p);
      if (sc & ehci::port_csc) {
        changed_mask_ |= 1u << p;
        port_update(p, 0, 0, ehci::port_csc);
      }
      (void)release_if_low_speed(p, sc);
    }
  }

  // ---- memory helpers ----

  [[nodiscard]] static bool below_4g(std::uint64_t phys, std::size_t size) noexcept {
    return phys + size <= (std::uint64_t{1} << 32);
  }
  [[nodiscard]] std::uint32_t phys32(std::size_t off) const noexcept {
    return static_cast<std::uint32_t>(mem_.phys_at(off));
  }
  [[nodiscard]] static std::size_t job_off(std::size_t ji) noexcept { return jobs_off + ji * job_stride; }
  [[nodiscard]] ehci::qh *qh_at(std::size_t ji) const noexcept { return mem_.template at<ehci::qh>(job_off(ji)); }
  [[nodiscard]] std::size_t qtd_off(std::size_t ji, std::size_t q) const noexcept {
    return job_off(ji) + qtds_in_job_off + q * sizeof(ehci::qtd);
  }
  [[nodiscard]] ehci::qtd *qtd_at(std::size_t ji, std::size_t q) const noexcept {
    return mem_.template at<ehci::qtd>(qtd_off(ji, q));
  }

  /// Byte offset in `mem_` of the structure a link pointer refers to, or `no_offset`.
  [[nodiscard]] std::size_t off_from_link(std::uint32_t link) const noexcept {
    if (link & ehci::link_terminate)
      return no_offset;
    const std::uint64_t a = link & ~std::uint32_t{0x1F};
    if (a < mem_.phys() || a - mem_.phys() >= mem_.size())
      return no_offset;
    return static_cast<std::size_t>(a - mem_.phys());
  }

  void init_schedules() noexcept {
    ehci::frame_list *fl = mem_.template at<ehci::frame_list>(frame_list_off);
    for (auto &e : fl->entry)
      e = phys32(periodic_dummy_off) | ehci::link_type_qh;

    // Both dummies are halted (never executed). The async one points at itself and carries the H bit; the
    // periodic one has S-mask 0 and ends the chain.
    ehci::qh *a = mem_.template at<ehci::qh>(async_dummy_off);
    *a = ehci::qh{};
    a->horizontal = phys32(async_dummy_off) | ehci::link_type_qh;
    a->ep_char = ehci::qh_h | ehci::qh_eps_high | (64u << 16);
    a->ep_caps = ehci::qh_mult_1;
    a->ov_token = ehci::tok_halted;
    ehci::qh *p = mem_.template at<ehci::qh>(periodic_dummy_off);
    *p = ehci::qh{};
    p->horizontal = ehci::link_terminate;
    p->ep_char = ehci::qh_eps_high | (64u << 16);
    p->ep_caps = ehci::qh_mult_1;
    p->ov_token = ehci::tok_halted;
  }

  // ---- data toggles ----

  [[nodiscard]] bool toggle(std::uint8_t addr, std::uint8_t ep, bool in) const noexcept {
    const std::uint16_t m = in ? tog_in_[addr] : tog_out_[addr];
    return ((m >> ep) & 1u) != 0;
  }
  void set_toggle(std::uint8_t addr, std::uint8_t ep, bool in, bool v) noexcept {
    std::uint16_t &m = in ? tog_in_[addr] : tog_out_[addr];
    const auto bit = static_cast<std::uint16_t>(1u << ep);
    m = v ? static_cast<std::uint16_t>(m | bit) : static_cast<std::uint16_t>(m & ~bit);
  }
  [[nodiscard]] bool endpoint_busy(std::uint8_t addr, std::uint8_t ep, bool in) const noexcept {
    for (const auto &j : jobs_)
      if (j.state == job_state::active && !j.control && j.address == addr && j.endpoint == ep && j.in == in)
        return true;
    return false;
  }

  // ---- transfer construction ----

  [[nodiscard]] static std::size_t packets_of(std::size_t bytes, std::size_t mps) noexcept {
    return bytes == 0 ? 1 : (bytes + mps - 1) / mps;
  }

  /// Bytes the qTD starting at bus address `addr` takes out of `remaining`: at most 5 pages, and every qTD but
  /// the last a multiple of the max packet size.
  [[nodiscard]] static std::size_t chunk_len(std::uint64_t addr, std::size_t remaining, std::size_t mps) noexcept {
    const std::size_t room = ehci::qtd_max_pages * ehci::page_size - static_cast<std::size_t>(addr & 0xFFF);
    if (remaining <= room)
      return remaining;
    return room - room % mps;
  }

  [[nodiscard]] static std::size_t count_chunks(std::uint64_t base, std::size_t len, std::size_t mps,
                                                bool at_least_one) noexcept {
    std::size_t n = 0;
    std::size_t off = 0;
    while (off < len) {
      off += chunk_len(base + off, len - off, mps);
      ++n;
    }
    return n == 0 && at_least_one ? 1 : n;
  }

  /// Fills a qTD; `len` bytes at bus address `addr` (0 when len == 0).
  static void fill_qtd(ehci::qtd &q, std::uint32_t pid, bool dt, std::size_t len, std::uint64_t addr,
                       bool ioc) noexcept {
    q = ehci::qtd{};
    q.token = ehci::tok_active | (pid << ehci::tok_pid_shift) | ehci::tok_cerr_3 | (ioc ? ehci::tok_ioc : 0u) |
              (static_cast<std::uint32_t>(len) << ehci::tok_bytes_shift) | (dt ? ehci::tok_dt : 0u);
    if (len == 0)
      return;
    q.buffer[0] = static_cast<std::uint32_t>(addr);
    std::size_t covered = ehci::page_size - static_cast<std::size_t>(addr & 0xFFF);
    for (std::size_t k = 1; covered < len; ++k, covered += ehci::page_size)
      q.buffer[k] = static_cast<std::uint32_t>((addr & ~std::uint64_t{0xFFF}) + k * ehci::page_size);
  }

  void build_qtds(std::size_t ji, const usb_transfer &t, std::uint64_t base) noexcept {
    job &j = jobs_[ji];
    std::size_t q = 0;
    if (j.control) {
      const reloco::array<std::uint8_t, 8> setup = t.setup.to_bytes();
      const std::size_t scratch = job_off(ji) + scratch_in_job_off;
      mem_.copy_in(scratch, setup.as_span());
      fill_qtd(*qtd_at(ji, q), ehci::pid_setup, false, 8, mem_.phys_at(scratch), false);
      j.qtd_len[q++] = 8;
    }
    bool dt = j.toggle_start;
    std::size_t off = 0;
    for (std::size_t d = 0; d < j.data_count; ++d) {
      const std::size_t n = chunk_len(base + off, j.length - off, j.max_packet);
      fill_qtd(*qtd_at(ji, q), j.in ? ehci::pid_in : ehci::pid_out, dt, n, n ? base + off : 0,
               !j.control && d + 1 == j.data_count);
      j.qtd_len[q++] = static_cast<std::uint16_t>(n);
      if ((packets_of(n, j.max_packet) & 1u) != 0)
        dt = !dt;
      off += n;
    }
    if (j.control) {
      // Status stage: opposite direction of the data stage (IN when there is none), zero length, DATA1.
      const bool status_in = j.length == 0 || !j.in;
      fill_qtd(*qtd_at(ji, q), status_in ? ehci::pid_in : ehci::pid_out, true, 0, 0, true);
      j.qtd_len[q] = 0;
      ++q;
    }
    for (std::size_t i = 0; i + 1 < q; ++i)
      qtd_at(ji, i)->next = phys32(qtd_off(ji, i + 1));
    if (j.control && j.in)
      for (std::size_t i = j.data_first; i < std::size_t{j.data_first} + j.data_count; ++i)
        qtd_at(ji, i)->alt_next = phys32(qtd_off(ji, q - 1)); // short IN packet: skip to the status stage
  }

  void build_qh(std::size_t ji) noexcept {
    job &j = jobs_[ji];
    ehci::qh *h = qh_at(ji);
    *h = ehci::qh{};
    h->ep_char = static_cast<std::uint32_t>(j.address) | (static_cast<std::uint32_t>(j.endpoint) << 8) |
                 ehci::qh_eps_high | ehci::qh_dtc | (static_cast<std::uint32_t>(j.max_packet) << 16) |
                 (j.periodic ? 0u : (4u << ehci::qh_rl_shift));
    h->ep_caps = ehci::qh_mult_1 | (j.periodic ? 0x01u : 0u);
    h->ov_next = phys32(qtd_off(ji, 0));
    h->ov_alt_next = ehci::link_terminate;
    h->ov_token = 0;
  }

  // ---- schedule linking ----

  void link_job(std::size_t ji) noexcept {
    const std::size_t head_off = jobs_[ji].periodic ? periodic_dummy_off : async_dummy_off;
    ehci::qh *head = mem_.template at<ehci::qh>(head_off);
    ehci::qh *h = qh_at(ji);
    ehci::detail::store(h->horizontal, ehci::detail::load(head->horizontal));
    env_->barrier();
    ehci::detail::store(head->horizontal, phys32(job_off(ji)) | ehci::link_type_qh);
  }

  void unlink_from_chain(std::size_t head_off, std::size_t target_off) noexcept {
    std::size_t off = head_off;
    for (std::size_t n = 0; n <= job_count + 1; ++n) {
      ehci::qh *cur = mem_.template at<ehci::qh>(off);
      const std::uint32_t next = ehci::detail::load(cur->horizontal);
      const std::size_t next_off = off_from_link(next);
      if (next_off == no_offset)
        return;
      if (next_off == target_off) {
        ehci::detail::store(cur->horizontal, ehci::detail::load(mem_.template at<ehci::qh>(next_off)->horizontal));
        return;
      }
      off = next_off;
    }
  }

  [[nodiscard]] std::uint16_t current_frame() noexcept {
    return static_cast<std::uint16_t>((op_read(ehci::op_frindex) >> 3) & 0x7FF);
  }

  /// Takes the job off the hardware schedule and parks it until the controller cannot be using it any more.
  void unlink_job(std::size_t ji) noexcept {
    job &j = jobs_[ji];
    if (j.periodic) {
      unlink_from_chain(periodic_dummy_off, job_off(ji));
      j.frame_stamp = current_frame();
      j.state = job_state::unlinked_periodic;
    } else {
      unlink_from_chain(async_dummy_off, job_off(ji));
      j.state = job_state::unlinked_async;
    }
    j.xfer = nullptr;
    --active_;
    env_->barrier();
    ring_doorbell();
  }

  void ring_doorbell() noexcept {
    if (iaa_pending_)
      return;
    bool any = false;
    for (auto &j : jobs_)
      if (j.state == job_state::unlinked_async) {
        j.state = job_state::doorbell_async;
        any = true;
      }
    if (!any)
      return;
    iaa_pending_ = true;
    op_write(ehci::op_usbcmd, op_read(ehci::op_usbcmd) | ehci::cmd_iaad);
  }

  void release_job(std::size_t ji) noexcept {
    *qh_at(ji) = ehci::qh{};
    jobs_[ji] = job{};
  }

  void on_async_advance() noexcept {
    iaa_pending_ = false;
    for (std::size_t i = 0; i < jobs_.size(); ++i)
      if (jobs_[i].state == job_state::doorbell_async)
        release_job(i);
    ring_doorbell();
  }

  void reap_periodic() noexcept {
    bool have = false;
    for (const auto &j : jobs_)
      have = have || j.state == job_state::unlinked_periodic;
    if (!have)
      return;
    const std::uint16_t frame = current_frame();
    for (std::size_t i = 0; i < jobs_.size(); ++i)
      if (jobs_[i].state == job_state::unlinked_periodic && jobs_[i].frame_stamp != frame)
        release_job(i);
  }

  // ---- completion ----

  [[nodiscard]] static usb_status map_error(std::uint32_t tok) noexcept {
    if (tok & ehci::tok_babble)
      return usb_status::babble;
    if (tok & (ehci::tok_xacterr | ehci::tok_dbe | ehci::tok_mmf))
      return usb_status::bus_error;
    return usb_status::stall; // halted without an error bit: the device STALLed
  }

  [[nodiscard]] outcome evaluate(std::size_t ji) const noexcept {
    const job &j = jobs_[ji];
    outcome o;
    std::size_t q = 0;
    while (q < j.qtd_count) {
      const std::uint32_t tok = ehci::detail::load(qtd_at(ji, q)->token);
      if (tok & ehci::tok_halted) {
        o.done = true;
        o.status = map_error(tok);
        return o;
      }
      if (tok & ehci::tok_active)
        return o;
      if (q >= j.data_first && q < static_cast<std::size_t>(j.data_first) + j.data_count) {
        const std::size_t left = (tok >> ehci::tok_bytes_shift) & ehci::tok_bytes_mask;
        const std::size_t moved = left < j.qtd_len[q] ? j.qtd_len[q] - left : 0;
        o.actual += moved;
        o.packets += packets_of(moved, j.max_packet);
        if (j.in && moved < j.qtd_len[q]) { // short packet: the remaining data qTDs are never executed
          if (!j.control)
            break;
          q = static_cast<std::size_t>(j.qtd_count) - 1;
          continue;
        }
      }
      ++q;
    }
    o.done = true;
    return o;
  }

  /// Updates the toggle table from what a successful control request does to the device's endpoints.
  void observe_control(const job &j, const usb_setup_packet &s) noexcept {
    if (s.request_type == 0x00 && s.request == 5) { // SET_ADDRESS: the new device starts at DATA0
      const std::uint8_t a = static_cast<std::uint8_t>(s.value & 0x7F);
      tog_in_[a] = 0;
      tog_out_[a] = 0;
    } else if (s.request_type == 0x00 && s.request == 9) { // SET_CONFIGURATION
      tog_in_[j.address] = 0;
      tog_out_[j.address] = 0;
    } else if (s.request_type == 0x02 && s.request == 1 && s.value == 0) { // CLEAR_FEATURE(ENDPOINT_HALT)
      set_toggle(j.address, static_cast<std::uint8_t>(s.index & 0x0F), (s.index & 0x80) != 0, false);
    }
    // SET_INTERFACE: usb_device::set_interface() calls reset_data_toggle() for the interface's endpoints.
  }

  void finish(std::size_t ji, const outcome &o) noexcept {
    job &j = jobs_[ji];
    usb_transfer *t = j.xfer;
    const bool ok = o.status == usb_status::ok;
    if (ok) {
      if (j.in && o.actual > 0)
        j.bounce.copy_out(0, reloco::span<std::uint8_t>(static_cast<std::uint8_t *>(t->data), o.actual));
      if (j.control)
        observe_control(j, t->setup);
      else
        set_toggle(j.address, j.endpoint, j.in, j.toggle_start != ((o.packets & 1u) != 0));
    }
    unlink_job(ji); // all driver state is final before the (possibly re-entrant) completion
    t->complete(o.status, ok ? o.actual : 0);
  }

  void scan_jobs() noexcept {
    for (std::size_t i = 0; i < jobs_.size() && started_; ++i) {
      if (jobs_[i].state != job_state::active)
        continue;
      const outcome o = evaluate(i);
      if (o.done)
        finish(i, o);
    }
  }

  /// Completes every active transfer with `status` (controller stopped or failed).
  void fail_all(usb_status status) noexcept {
    reloco::array<usb_transfer *, MaxTransfers> pending{};
    std::size_t n = 0;
    for (const auto &j : jobs_)
      if (j.state == job_state::active && n < pending.size())
        pending[n++] = j.xfer;
    if (started_) {
      for (std::size_t i = 0; i < jobs_.size(); ++i)
        if (jobs_[i].state == job_state::active)
          unlink_job(i);
    } else {
      for (auto &j : jobs_)
        j = job{};
      active_ = 0;
    }
    for (std::size_t i = 0; i < n; ++i)
      pending[i]->complete(status, 0);
  }

  Env *env_;
  usb_dma_block<Env> mem_;
  bool started_ = false;
  bool failed_ = false;
  bool in_irq_ = false;
  bool iaa_pending_ = false;
  std::uint32_t cap_length_ = 0;
  std::uint32_t hcs_ = 0;
  std::uint32_t hcc_ = 0;
  unsigned n_ports_ = 0;
  std::uint32_t changed_mask_ = 0;
  std::size_t active_ = 0;
  reloco::array<job, job_count> jobs_{};
  reloco::array<std::uint16_t, 128> tog_in_{};
  reloco::array<std::uint16_t, 128> tog_out_{};
};

/** @brief `usb_host_controller_ref{hcd}` support for `ehci_hcd`. */
template <typename Env, std::size_t MaxTransfers> struct usb_host_traits<ehci_hcd<Env, MaxTransfers>> {
  using hcd_type = ehci_hcd<Env, MaxTransfers>;
  static unsigned port_count(hcd_type &h) noexcept { return h.port_count(); }
  static reloco::result<usb_port_status> port_status(hcd_type &h, unsigned port) noexcept {
    return h.port_status(port);
  }
  static reloco::task<void> reset_port(hcd_type &h, unsigned port) noexcept { return h.reset_port(port); }
  static reloco::result<void> submit(hcd_type &h, usb_transfer &t) noexcept { return h.submit(t); }
  static void cancel(hcd_type &h, usb_transfer &t) noexcept { h.cancel(t); }
  static void reset_data_toggle(hcd_type &h, const usb_pipe &p) noexcept { h.reset_data_toggle(p); }
};

} // namespace structo::hw

#endif // RELOCO_HAS_COROUTINES
