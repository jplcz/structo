// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file xhci_hcd.hpp
 * @brief `structo::hw::xhci_hcd<Env>`: a generic xHCI (eXtensible Host Controller Interface 1.1/1.2, USB 3.x
 * and USB 2.0 root ports) host controller driver, header-only and freestanding friendly. Plug it into
 * `usb_host_controller_ref` and run `structo::usb::usb_host` / `bootldr::usb_stack` on top. C++20 only
 * (empty otherwise). See docs/xhci_hcd.md for the full guide.
 *
 * The driver knows nothing about the SoC or PCI: everything board specific goes through one `Env` class (see
 * `usb_hcd_env.hpp` for the contract). A complete, minimal example:
 *
 * @code
 * // A memory-mapped xHCI controller (e.g. behind a PCI BAR or a SoC DWC3 in host mode) plus a coherent DMA
 * // pool. `Env` is the only thing a board has to write; the driver never touches a raw address itself.
 * struct my_xhci_env {
 *   // Base of the register window = address of the first capability register (CAPLENGTH). Set by the board
 *   // (PCI BAR0 / device-tree reg), then every access below is relative to it.
 *   volatile std::uint32_t *mmio;
 *
 *   // 32-bit register read. `offset` is a BYTE offset from `mmio`; the driver only uses 4-byte aligned
 *   // offsets (64-bit registers are accessed as two 32-bit halves, low first).
 *   std::uint32_t read32(std::size_t offset) noexcept { return mmio[offset / 4]; }
 *   void write32(std::size_t offset, std::uint32_t value) noexcept { mmio[offset / 4] = value; }
 *
 *   // Zeroed, physically contiguous, coherent memory (or the platform keeps it coherent) aligned to `align`
 *   // (a power of two <= 4096). `phys` is the BUS address the controller must use (after IOMMU/offset). If
 *   // the controller lacks 64-bit addressing (HCCPARAMS1.AC64 = 0) the driver rejects memory above 4 GiB.
 *   structo::hw::usb_dma_buffer dma_alloc(std::size_t size, std::size_t align) noexcept {
 *     void *p = pool.allocate_zeroed(size, align);
 *     return {p, to_bus_address(p), size};            // {virt, phys, size}; virt == nullptr on failure
 *   }
 *   void dma_free(const structo::hw::usb_dma_buffer &b) noexcept { pool.deallocate(b.virt); }
 *
 *   // Full barrier: the driver calls it after writing a descriptor and before telling the controller about
 *   // it (doorbell/register write), and after observing a completion before reading what it covers.
 *   void barrier() noexcept { std::atomic_thread_fence(std::memory_order_seq_cst); }
 *
 *   // Sleeps `ms` milliseconds without blocking the CPU; used by the port reset (and recovery) sequence.
 *   reloco::task<void> delay_ms(unsigned ms) noexcept { return my_scheduler.sleep_for_ms(ms); }
 * };
 *
 * my_xhci_env env{...};                                   // MMIO window + DMA pool
 * structo::hw::xhci_hcd<my_xhci_env> hcd{env};            // 16 transfers in flight (default)
 * if (!hcd.start())                                       // reset, program rings, run (bounded waits)
 *   panic();
 * structo::hw::usb_host_controller_ref ref{hcd};          // what usb_host / usb_stack / class drivers take
 * // IRQ handler (or a polling loop, e.g. usb_stack::poll_with): hcd.irq();
 * @endcode
 *
 * Everything xHCI-specific that `usb::usb_host` cannot know is handled transparently inside `submit()`:
 * slot allocation (Enable Slot), the Address Device command that replaces the SET_ADDRESS the wire never
 * sees, endpoint configuration (Configure Endpoint, lazily on the first transfer), EP0 max-packet updates
 * (Evaluate Context), endpoint recovery after a STALL (Reset Endpoint + Set TR Dequeue Pointer), cancel
 * (Stop Endpoint + Set TR Dequeue Pointer) and Disable Slot on unplug.
 *
 * Address-0 rule: a pipe carries no port, but xHCI needs a device slot (bound to a root port) even for the
 * very first GET_DESCRIPTOR sent to the unaddressed device. The driver therefore binds every
 * `pipe.address == 0` transfer to the root port most recently passed to `reset_port()` (usb_host
 * enumerates one port at a time, resets it first and then talks to address 0).
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

namespace structo::hw {

/** @brief Hardware structures and register constants of xHCI (fixed-width little-endian, host byte order = LE). */
namespace xhci_detail {

/** @brief Transfer/command/event ring entry (xHCI 6.4). */
struct trb {
  std::uint64_t param;
  std::uint32_t status;
  std::uint32_t control;
};
static_assert(sizeof(trb) == 16 && alignof(trb) == 8);

/** @brief Event Ring Segment Table entry (xHCI 6.5). */
struct erst_entry {
  std::uint64_t base;
  std::uint32_t size;
  std::uint32_t reserved;
};
static_assert(sizeof(erst_entry) == 16 && alignof(erst_entry) == 8);

/** @brief Input Control Context (xHCI 6.2.5.1): first 32 bytes of the input context. */
struct input_control_context {
  std::uint32_t drop_flags;
  std::uint32_t add_flags;
  reloco::array<std::uint32_t, 5> reserved;
  std::uint32_t config;
};
static_assert(sizeof(input_control_context) == 32 && alignof(input_control_context) == 4);

/** @brief Slot Context (xHCI 6.2.2). */
struct slot_context {
  std::uint32_t dw0; ///< Route string, speed [23:20], MTT, hub, context entries [31:27].
  std::uint32_t dw1; ///< Max exit latency [15:0], root hub port number [23:16], number of ports [31:24].
  std::uint32_t dw2; ///< TT hub slot/port, interrupter target [31:22].
  std::uint32_t dw3; ///< USB device address [7:0], slot state [31:27].
  reloco::array<std::uint32_t, 4> reserved;
};
static_assert(sizeof(slot_context) == 32 && alignof(slot_context) == 4);

/** @brief Endpoint Context (xHCI 6.2.3). */
struct endpoint_context {
  std::uint32_t dw0; ///< EP state [2:0], mult, max primary streams, interval [23:16], max ESIT payload hi [31:24].
  std::uint32_t dw1; ///< CErr [2:1], EP type [5:3], max burst [15:8], max packet size [31:16].
  std::uint64_t tr_dequeue; ///< Transfer ring dequeue pointer | DCS (bit 0).
  std::uint32_t dw4;        ///< Average TRB length [15:0], max ESIT payload lo [31:16].
  reloco::array<std::uint32_t, 3> reserved;
};
static_assert(sizeof(endpoint_context) == 32 && alignof(endpoint_context) == 8);

// Capability registers (offsets from the window start).
inline constexpr std::size_t cap_hcsparams1 = 0x04;
inline constexpr std::size_t cap_hcsparams2 = 0x08;
inline constexpr std::size_t cap_hccparams1 = 0x10;
inline constexpr std::size_t cap_dboff = 0x14;
inline constexpr std::size_t cap_rtsoff = 0x18;

// Operational registers (offsets from the operational base = CAPLENGTH).
inline constexpr std::size_t op_usbcmd = 0x00;
inline constexpr std::size_t op_usbsts = 0x04;
inline constexpr std::size_t op_pagesize = 0x08;
inline constexpr std::size_t op_crcr = 0x18;
inline constexpr std::size_t op_dcbaap = 0x30;
inline constexpr std::size_t op_config = 0x38;
inline constexpr std::size_t op_portsc = 0x400;

// Runtime registers, interrupter 0 (offsets from the runtime base).
inline constexpr std::size_t rt_ir0 = 0x20;
inline constexpr std::size_t ir_iman = 0x00;
inline constexpr std::size_t ir_imod = 0x04;
inline constexpr std::size_t ir_erstsz = 0x08;
inline constexpr std::size_t ir_erstba = 0x10;
inline constexpr std::size_t ir_erdp = 0x18;

inline constexpr std::uint32_t cmd_rs = 1u << 0;
inline constexpr std::uint32_t cmd_hcrst = 1u << 1;
inline constexpr std::uint32_t cmd_inte = 1u << 2;
inline constexpr std::uint32_t cmd_hsee = 1u << 3;
inline constexpr std::uint32_t sts_hch = 1u << 0;
inline constexpr std::uint32_t sts_hse = 1u << 2;
inline constexpr std::uint32_t sts_eint = 1u << 3;
inline constexpr std::uint32_t sts_pcd = 1u << 4;
inline constexpr std::uint32_t sts_cnr = 1u << 11;
inline constexpr std::uint32_t iman_ip = 1u << 0;
inline constexpr std::uint32_t iman_ie = 1u << 1;
inline constexpr std::uint32_t erdp_ehb = 1u << 3;

// PORTSC bits. PED is write-1-to-DISABLE and the change bits are write-1-to-clear: never write back what was read.
inline constexpr std::uint32_t pc_ccs = 1u << 0;
inline constexpr std::uint32_t pc_ped = 1u << 1;
inline constexpr std::uint32_t pc_pr = 1u << 4;
inline constexpr std::uint32_t pc_pls_shift = 5;
inline constexpr std::uint32_t pc_pls_mask = 0xFu << pc_pls_shift;
inline constexpr std::uint32_t pc_pp = 1u << 9;
inline constexpr std::uint32_t pc_speed_shift = 10;
inline constexpr std::uint32_t pc_speed_mask = 0xFu << pc_speed_shift;
inline constexpr std::uint32_t pc_pic = 3u << 14;
inline constexpr std::uint32_t pc_lws = 1u << 16;
inline constexpr std::uint32_t pc_csc = 1u << 17;
inline constexpr std::uint32_t pc_pec = 1u << 18;
inline constexpr std::uint32_t pc_wrc = 1u << 19;
inline constexpr std::uint32_t pc_occ = 1u << 20;
inline constexpr std::uint32_t pc_prc = 1u << 21;
inline constexpr std::uint32_t pc_plc = 1u << 22;
inline constexpr std::uint32_t pc_cec = 1u << 23;
inline constexpr std::uint32_t pc_wce = 1u << 25;
inline constexpr std::uint32_t pc_wde = 1u << 26;
inline constexpr std::uint32_t pc_woe = 1u << 27;
inline constexpr std::uint32_t pc_wpr = 1u << 31;
/// Read/write bits that must be written back unchanged.
inline constexpr std::uint32_t pc_preserve = pc_pp | pc_pic | pc_wce | pc_wde | pc_woe;
/// Bits that clear when 1 is written (PED: disables the port).
inline constexpr std::uint32_t pc_w1c = pc_ped | pc_csc | pc_pec | pc_wrc | pc_occ | pc_prc | pc_plc | pc_cec;
/// Change bits the driver clears on its own (PRC/WRC belong to the reset sequence, CSC is latched first).
inline constexpr std::uint32_t pc_misc_changes = pc_csc | pc_pec | pc_occ | pc_plc | pc_cec;

// TRB types.
inline constexpr std::uint32_t trb_normal = 1;
inline constexpr std::uint32_t trb_setup = 2;
inline constexpr std::uint32_t trb_data = 3;
inline constexpr std::uint32_t trb_status = 4;
inline constexpr std::uint32_t trb_link = 6;
inline constexpr std::uint32_t trb_cmd_enable_slot = 9;
inline constexpr std::uint32_t trb_cmd_disable_slot = 10;
inline constexpr std::uint32_t trb_cmd_address_device = 11;
inline constexpr std::uint32_t trb_cmd_configure_endpoint = 12;
inline constexpr std::uint32_t trb_cmd_evaluate_context = 13;
inline constexpr std::uint32_t trb_cmd_reset_endpoint = 14;
inline constexpr std::uint32_t trb_cmd_stop_endpoint = 15;
inline constexpr std::uint32_t trb_cmd_set_tr_dequeue = 16;
inline constexpr std::uint32_t trb_cmd_no_op = 23;
inline constexpr std::uint32_t trb_ev_transfer = 32;
inline constexpr std::uint32_t trb_ev_command_completion = 33;
inline constexpr std::uint32_t trb_ev_port_status_change = 34;

// TRB control bits.
inline constexpr std::uint32_t trb_cycle = 1u << 0;
inline constexpr std::uint32_t trb_tc = 1u << 1;     ///< Link: toggle cycle.
inline constexpr std::uint32_t trb_isp = 1u << 2;    ///< Interrupt on short packet.
inline constexpr std::uint32_t trb_chain = 1u << 4;
inline constexpr std::uint32_t trb_ioc = 1u << 5;    ///< Interrupt on completion.
inline constexpr std::uint32_t trb_idt = 1u << 6;    ///< Immediate data (setup stage).
inline constexpr std::uint32_t trb_bsr = 1u << 9;    ///< Address Device: block set address.
inline constexpr std::uint32_t trb_dir_in = 1u << 16;

// Completion codes.
inline constexpr std::uint32_t cc_success = 1;
inline constexpr std::uint32_t cc_data_buffer_error = 2;
inline constexpr std::uint32_t cc_babble = 3;
inline constexpr std::uint32_t cc_transaction_error = 4;
inline constexpr std::uint32_t cc_trb_error = 5;
inline constexpr std::uint32_t cc_stall = 6;
inline constexpr std::uint32_t cc_no_slots = 9;
inline constexpr std::uint32_t cc_short_packet = 13;
inline constexpr std::uint32_t cc_context_state_error = 19;
inline constexpr std::uint32_t cc_missed_service = 23;
inline constexpr std::uint32_t cc_stopped = 26;

// Endpoint states (endpoint context dw0 [2:0]) and endpoint type codes.
inline constexpr std::uint32_t ep_state_disabled = 0;
inline constexpr std::uint32_t ep_state_running = 1;
inline constexpr std::uint32_t ep_state_halted = 2;
inline constexpr std::uint32_t ep_state_stopped = 3;
inline constexpr std::uint32_t ep_state_error = 4;
inline constexpr std::uint32_t ep_type_bulk_out = 2;
inline constexpr std::uint32_t ep_type_interrupt_out = 3;
inline constexpr std::uint32_t ep_type_control = 4;
inline constexpr std::uint32_t ep_type_bulk_in = 6;
inline constexpr std::uint32_t ep_type_interrupt_in = 7;

[[nodiscard]] constexpr std::uint32_t narrow32(std::uint64_t v) noexcept { return static_cast<std::uint32_t>(v); }
[[nodiscard]] constexpr std::uint8_t narrow8(std::uint64_t v) noexcept { return static_cast<std::uint8_t>(v); }
[[nodiscard]] constexpr std::uint16_t narrow16(std::uint64_t v) noexcept { return static_cast<std::uint16_t>(v); }

} // namespace xhci_detail

/**
 * @brief Generic xHCI host controller driver.
 * @tparam Env platform layer (`is_usb_hcd_env_v<Env>`).
 * @tparam MaxTransfers number of `usb_transfer`s that may be in flight/queued at once (`submit()` beyond that
 * fails with `error::try_again`).
 *
 * Compile-time limits: `max_slots` device slots (= attached devices; root ports only, no hubs),
 * `max_endpoints` configured endpoints per slot (including EP0) and `max_root_ports` root ports.
 * Not supported: hubs/transaction translators, isochronous endpoints, streams, SuperSpeed companion burst
 * tuning (max burst is programmed as 0).
 */
template <typename Env, std::size_t MaxTransfers = 16> class xhci_hcd {
  static_assert(is_usb_hcd_env_v<Env>, "Env must satisfy the usb_hcd_env contract (see usb_hcd_env.hpp)");
  static_assert(MaxTransfers > 0);

public:
  static constexpr unsigned max_slots = 8;
  static constexpr unsigned max_endpoints = 8;
  static constexpr unsigned max_root_ports = 16;

  explicit xhci_hcd(Env &env) noexcept : env_(&env) {}
  xhci_hcd(const xhci_hcd &) = delete;
  xhci_hcd &operator=(const xhci_hcd &) = delete;
  ~xhci_hcd() { stop(); }

  /**
   * @brief Resets and starts the controller: capability discovery, BIOS handoff, halt + HCRST (bounded spin
   * waits: `error::timed_out` if the controller never halts/resets/gets ready), DCBAA (+ scratchpad buffers
   * when the controller asks for them), command ring, one event ring segment, interrupter 0, supported
   * protocol walk (root port -> USB2/USB3 and speed IDs), RUN + interrupt enable.
   * Errors: `unsupported_operation` (not an xHCI 1.x, 4 KiB pages unsupported), `allocation_failed` (DMA memory
   * missing or above 4 GiB for a 32-bit controller), `timed_out`.
   */
  [[nodiscard]] reloco::result<void> start() noexcept {
    if (started_)
      return reloco::unexpected(reloco::error::invalid_state);
    auto r = start_impl();
    if (!r)
      release_all();
    return r;
  }

  /** @brief Halts the controller, completes every pending transfer with `cancelled` and frees all DMA memory. */
  void stop() noexcept {
    if (!started_) {
      release_all();
      return;
    }
    started_ = false;
    wr(op_ + xhci_detail::op_usbcmd, 0);
    (void)spin_until([&] { return (rd(op_ + xhci_detail::op_usbsts) & xhci_detail::sts_hch) != 0; });
    reloco::array<usb_transfer *, MaxTransfers> pending{};
    std::size_t n = 0;
    for (std::size_t i = 0; i < MaxTransfers; ++i) {
      auto &e = table_[i];
      if (e.xfer) {
        pending[n++] = e.xfer;
        table_.release(e);
      }
    }
    release_all();
    for (std::size_t i = 0; i < n; ++i)
      pending[i]->complete(usb_status::cancelled, 0);
  }

  /**
   * @brief Interrupt handler / poll step: acknowledges the controller, drains the event ring (transfer,
   * command completion, port status change events), starts queued work and rings doorbells. Completed
   * transfers are finished with `usb_transfer::complete()` AFTER the driver state is consistent, so the woken
   * coroutine may re-enter `submit()`/`cancel()`.
   */
  void irq() noexcept {
    using namespace xhci_detail;
    if (!started_ || in_irq_)
      return;
    in_irq_ = true;
    const std::uint32_t sts = rd(op_ + op_usbsts);
    if ((sts & (sts_eint | sts_hse | sts_pcd)) != 0)
      wr(op_ + op_usbsts, sts & (sts_eint | sts_hse | sts_pcd)); // write-1-to-clear
    wr(rt_ + rt_ir0 + ir_iman, iman_ie | iman_ip);
    drain_events();
    if (started_) {
      pump();
      kick();
    }
    in_irq_ = false;
  }

  /** @brief Alias of `irq()` for polled operation. */
  void poll() noexcept { irq(); }

  // ---- usb_host_traits backend ----

  [[nodiscard]] unsigned port_count() const noexcept {
    return started_ ? (max_ports_ < max_root_ports ? max_ports_ : max_root_ports) : 0u;
  }

  /** @brief Reads PORTSC and clears the change bits (without disabling the port: PED is write-1-to-disable). */
  [[nodiscard]] reloco::result<usb_port_status> port_status(unsigned port) noexcept {
    using namespace xhci_detail;
    if (!started_ || port >= port_count())
      return reloco::unexpected(reloco::error::invalid_argument);
    const unsigned p = port + 1;
    absorb_port(p);
    const std::uint32_t v = rd(portsc_off(p));
    usb_port_status s;
    s.connected = (v & pc_ccs) != 0;
    s.enabled = (v & pc_ped) != 0;
    s.speed = map_speed(p, (v & pc_speed_mask) >> pc_speed_shift);
    s.changed = changed_[p];
    changed_[p] = false;
    return s;
  }

  /**
   * @brief Resets root port `port` (0-based) and remembers it as the target of address-0 traffic. Any slot
   * still bound to the port is disabled first. USB2: PR, wait for PRC (bounded, 5 ms steps), clear PRC;
   * USB3: warm reset (WPR) if the link is Inactive/Compliance, hot reset (PR) otherwise. Afterwards waits the
   * 10 ms recovery time and verifies PED = 1 and PLS = U0.
   */
  [[nodiscard]] reloco::task<void> reset_port(unsigned port) noexcept {
    using namespace xhci_detail;
    if (!started_ || port >= port_count())
      co_await reloco::unexpected(reloco::error::invalid_argument);
    const unsigned p = port + 1;
    reset_port_ = narrow8(p);
    absorb_port(p);
    teardown_port(p, usb_status::disconnected);
    kick();
    for (unsigned i = 0; i < 100 && slot_on_port(p); ++i)
      (void)co_await env_->delay_ms(1);

    std::uint32_t v = rd(portsc_off(p));
    if ((v & pc_ccs) == 0)
      co_await reloco::unexpected(reloco::error::not_found);
    const proto_group *g = group_of(p);
    const bool usb3 = g != nullptr && g->major >= 3;
    const unsigned pls = (v & pc_pls_mask) >> pc_pls_shift;
    const bool warm = usb3 && (pls == 6 || pls == 10); // Inactive / Compliance Mode need a warm reset
    wr(portsc_off(p), (v & pc_preserve) | pc_prc | pc_wrc | (warm ? pc_wpr : pc_pr));
    bool done = false;
    for (unsigned i = 0; i < 100 && !done; ++i) {
      (void)co_await env_->delay_ms(5);
      v = rd(portsc_off(p));
      if ((v & pc_ccs) == 0)
        co_await reloco::unexpected(reloco::error::not_found);
      done = (v & (warm ? pc_wrc : pc_prc)) != 0;
    }
    if (!done)
      co_await reloco::unexpected(reloco::error::timed_out);
    wr(portsc_off(p), (v & pc_preserve) | pc_prc | pc_wrc);
    (void)co_await env_->delay_ms(10); // TRSTRCY

    v = rd(portsc_off(p));
    if ((v & pc_ccs) == 0)
      co_await reloco::unexpected(reloco::error::not_found);
    if (usb3 && (v & pc_ped) != 0 && ((v & pc_pls_mask) >> pc_pls_shift) != 0) {
      wr(portsc_off(p), (v & pc_preserve) | pc_lws); // request U0
      (void)co_await env_->delay_ms(5);
      v = rd(portsc_off(p));
    }
    if ((v & pc_ped) == 0 || ((v & pc_pls_mask) >> pc_pls_shift) != 0)
      co_await reloco::unexpected(reloco::error::io_error);
  }

  /**
   * @brief Queues a transfer. Interception rules (see the file docs): `SET_ADDRESS` on address 0 becomes
   * Enable Slot (if needed) + Address Device and completes successfully; other address-0 transfers use the
   * slot of the port last reset; the first transfer to an endpoint configures it. Errors: `not_found`
   * (unknown address / no port reset yet), `try_again` (table full), `invalid_argument`,
   * `unsupported_operation` (isochronous), `resource_exhausted` (endpoint table full), `out_of_range`
   * (transfer too large for one TD: ~830 KiB).
   */
  [[nodiscard]] reloco::result<void> submit(usb_transfer &t) noexcept {
    using namespace xhci_detail;
    if (!started_)
      return reloco::unexpected(reloco::error::invalid_state);
    const usb_pipe &pp = t.pipe;
    if (pp.type == usb_transfer_type::isochronous)
      return reloco::unexpected(reloco::error::unsupported_operation);
    if (pp.endpoint > 15 || (pp.type != usb_transfer_type::control && pp.endpoint == 0) || pp.address > 127)
      return reloco::unexpected(reloco::error::invalid_argument);
    if (t.length > max_td_bytes || (t.length != 0 && t.data == nullptr))
      return reloco::unexpected(reloco::error::out_of_range);

    const bool control = pp.type == usb_transfer_type::control;
    xfer_state st;
    st.addr0 = pp.address == 0;
    if (st.addr0) {
      if (!control || pp.endpoint != 0)
        return reloco::unexpected(reloco::error::invalid_argument);
      if (reset_port_ == 0)
        return reloco::unexpected(reloco::error::not_found);
      st.port = reset_port_;
      st.set_address = t.setup.request_type == 0x00 && t.setup.request == 0x05;
      st.dci = 1;
    } else {
      st.slot = addr_slot_[pp.address];
      if (st.slot == 0)
        return reloco::unexpected(reloco::error::not_found);
      st.dci = narrow8(control ? 1u : (static_cast<unsigned>(pp.endpoint) * 2u + (t.is_in() ? 1u : 0u)));
      slot_state &sl = slots_[st.slot];
      if (find_ep(sl, st.dci) == nullptr && alloc_ep(sl, st.dci, pp, t.is_in()) == nullptr)
        return reloco::unexpected(reloco::error::resource_exhausted);
    }
    auto *e = table_.acquire(t);
    if (e == nullptr)
      return reloco::unexpected(reloco::error::try_again);
    st.seq = ++seq_;
    e->st = st;
    pump();
    kick();
    return {};
  }

  /**
   * @brief Withdraws `t`: it is dropped from the queue; if its TD was already on the ring the endpoint is
   * stopped (Stop Endpoint) and its dequeue pointer moved past the TD (Set TR Dequeue Pointer). The bounce
   * buffer stays allocated until the controller reports the endpoint stopped. `t` is never touched afterwards.
   */
  void cancel(usb_transfer &t) noexcept {
    auto *e = table_.find(t);
    if (e == nullptr)
      return;
    const xfer_state st = e->st;
    if (st.phase == xphase::waiting_address) {
      for (unsigned s = 1; s <= max_slots; ++s)
        if (slots_[s].addr_xfer == &t)
          slots_[s].addr_xfer = nullptr;
    } else if (st.phase == xphase::active) {
      slot_state &sl = slots_[st.slot];
      if (endpoint_state *ep = find_ep(sl, st.dci)) {
        ep->active = nullptr;
        start_resync(*ep);
      }
    }
    table_.release(*e);
    kick();
  }

  /**
   * @brief Resets the data toggle of an endpoint. xHCI keeps toggles in hardware: they restart at DATA0 when
   * the endpoint is Reset (done automatically after a STALL) or re-added. Otherwise the endpoint is dropped
   * and re-added with two Configure Endpoint commands (run as soon as the endpoint is idle).
   */
  void reset_data_toggle(const usb_pipe &p) noexcept {
    if (!started_ || p.address == 0 || p.type == usb_transfer_type::control || p.endpoint == 0 || p.endpoint > 15)
      return;
    const unsigned s = addr_slot_[p.address];
    if (s == 0)
      return;
    const auto dci = xhci_detail::narrow8(static_cast<unsigned>(p.endpoint) * 2u +
                                          (p.direction == usb_direction::in ? 1u : 0u));
    if (endpoint_state *ep = find_ep(slots_[s], dci))
      if (ep->configured)
        ep->toggle_reset_req = true;
    kick();
  }

  // ---- introspection (tests, diagnostics) ----

  [[nodiscard]] bool running() const noexcept { return started_; }
  /** @brief Bytes per context structure the controller uses (32, or 64 when HCCPARAMS1.CSZ is set). */
  [[nodiscard]] unsigned context_size() const noexcept { return ctx_size_; }
  /** @brief Transfers currently queued or in flight. */
  [[nodiscard]] std::size_t transfers_in_use() const noexcept { return table_.in_use(); }
  /** @brief Maximum bytes of one transfer (one TD). */
  static constexpr std::size_t max_td_bytes = 13u * 0x10000u;

private:
  using trb = xhci_detail::trb;
  using dma_block = usb_dma_block<Env>;

  static constexpr unsigned ring_trbs = 16;     ///< Transfer ring segment: 15 TRBs + Link.
  static constexpr unsigned max_td_trbs = ring_trbs - 2;
  static constexpr unsigned cmd_ring_trbs = 32; ///< Command ring segment.
  static constexpr unsigned evt_ring_trbs = 64; ///< Event ring segment.
  static constexpr unsigned spin_limit = 200000;
  static constexpr unsigned max_groups = 4;
  static constexpr unsigned max_psi = 8;
  static_assert(max_slots + 1 <= 32, "the DCBAA must fit one 256-byte aligned block");

  enum class xphase : std::uint8_t { queued, waiting_address, active };
  enum class slot_phase : std::uint8_t { free, need_address, addressing, default_state, addressed, disable_pending, disabling };
  enum class ep_plan : std::uint8_t { none, configure, evaluate, resync, toggle };
  enum class cmd_kind : std::uint8_t {
    none,
    enable_slot,
    disable_slot,
    address_device,
    configure_endpoint,
    evaluate_context,
    stop_endpoint,
    reset_endpoint,
    set_tr_dequeue
  };

  struct xfer_state {
    xphase phase = xphase::queued;
    bool addr0 = false;
    bool set_address = false;
    std::uint8_t port = 0; ///< 1-based root port (address-0 transfers).
    std::uint8_t slot = 0;
    std::uint8_t dci = 0;
    std::uint8_t td_trbs = 0;
    std::uint16_t td_first = 0;
    std::uint32_t seq = 0;
    std::uint32_t ctl_actual = 0;
  };
  using table_type = usb_transfer_table<xfer_state, MaxTransfers>;
  using entry = typename table_type::slot;

  /** @brief A transfer ring segment (or the command ring): producer side. */
  struct ring {
    Env *env = nullptr;
    dma_block mem;
    unsigned size = 0;
    unsigned enq = 0;
    bool pcs = true;

    [[nodiscard]] trb *at(unsigned i) const noexcept { return mem.template at<trb>(i * sizeof(trb)); }
    [[nodiscard]] std::uint64_t phys(unsigned i) const noexcept { return mem.phys_at(i * sizeof(trb)); }
    [[nodiscard]] std::uint64_t dequeue_ptr() const noexcept { return phys(enq) | (pcs ? 1u : 0u); }

    /** @brief Writes a TRB; `hold` leaves its cycle bit at the "not owned" value until `set_cycle`. */
    unsigned push(std::uint64_t param, std::uint32_t status, std::uint32_t control, bool hold) noexcept {
      using namespace xhci_detail;
      const unsigned idx = enq;
      trb *t = at(idx);
      t->param = param;
      t->status = status;
      env->barrier();
      const bool c = hold ? !pcs : pcs;
      t->control = (control & ~trb_cycle) | (c ? trb_cycle : 0u);
      enq = idx + 1;
      if (enq == size - 1) { // eager Link TRB: follow the chain bit of the TD, toggle the cycle state
        trb *l = at(enq);
        l->param = phys(0);
        l->status = 0;
        l->control = (trb_link << 10) | trb_tc | (control & trb_chain) | (pcs ? trb_cycle : 0u);
        pcs = !pcs;
        enq = 0;
      }
      return idx;
    }

    void set_cycle(unsigned idx, bool cycle) noexcept {
      trb *t = at(idx);
      t->control = (t->control & ~xhci_detail::trb_cycle) | (cycle ? xhci_detail::trb_cycle : 0u);
    }

    [[nodiscard]] bool init(Env &e, unsigned trbs) noexcept {
      env = &e;
      if (!mem.allocate(e, trbs * sizeof(trb), trbs * sizeof(trb)))
        return false;
      size = trbs;
      enq = 0;
      pcs = true;
      trb *l = at(trbs - 1);
      l->param = phys(0);
      l->control = (xhci_detail::trb_link << 10) | xhci_detail::trb_tc;
      return true;
    }
  };

  struct endpoint_state {
    std::uint8_t dci = 0; ///< 0 = unused entry.
    bool configured = false;
    bool dead = false;
    bool toggle_reset_req = false;
    bool sent = false; ///< The plan's current command is in flight.
    bool deq_done = false;
    ep_plan plan = ep_plan::none;
    std::uint8_t plan_step = 0;
    std::uint8_t attempts = 0;
    std::uint8_t type = 0; ///< Endpoint type code for the context.
    std::uint8_t interval = 0;
    std::uint16_t max_packet = 8;
    ring tr;
    usb_transfer *active = nullptr;
    dma_block bounce; ///< Bounce buffer of the active TD; kept until the controller is done with it.
  };

  struct slot_state {
    slot_phase phase = slot_phase::free;
    bool addr_bsr = false;
    bool pending_final = false; ///< A SET_ADDRESS arrived while the BSR=1 Address Device was still running.
    std::uint8_t port = 0;      ///< 1-based root port.
    std::uint8_t speed_id = 0;
    std::uint8_t usb_addr = 0;
    usb_transfer *addr_xfer = nullptr;
    dma_block in_ctx;
    dma_block dev_ctx;
    reloco::array<endpoint_state, max_endpoints> eps{};
  };

  struct proto_group {
    std::uint8_t major = 0;
    std::uint8_t first = 0; ///< First 1-based root port.
    std::uint8_t count = 0;
    std::uint8_t psi_count = 0;
    reloco::array<std::uint32_t, max_psi> psi{};
  };

  struct inflight_cmd {
    cmd_kind kind = cmd_kind::none;
    std::uint8_t slot = 0;
    std::uint8_t dci = 0;
    std::uint64_t phys = 0;
  };

  struct enable_request {
    bool pending = false;
    bool sent = false;
    bool abort = false;
    bool bsr = true;
    std::uint8_t port = 0;
    std::uint16_t mps = 8;
  };

  // ---- register helpers ----

  [[nodiscard]] std::uint32_t rd(std::size_t off) noexcept { return env_->read32(off); }
  void wr(std::size_t off, std::uint32_t v) noexcept { env_->write32(off, v); }
  void wr64(std::size_t off, std::uint64_t v) noexcept {
    env_->write32(off, xhci_detail::narrow32(v));
    env_->write32(off + 4, xhci_detail::narrow32(v >> 32));
  }
  [[nodiscard]] std::size_t portsc_off(unsigned p) const noexcept { return op_ + xhci_detail::op_portsc + 0x10u * (p - 1u); }

  template <typename Pred> [[nodiscard]] static bool spin_until(Pred pred) noexcept {
    for (unsigned i = 0; i < spin_limit; ++i)
      if (pred())
        return true;
    return false;
  }

  [[nodiscard]] bool alloc(dma_block &b, std::size_t size, std::size_t align) noexcept {
    if (!b.allocate(*env_, size, align))
      return false;
    if (!ac64_ && ((b.phys() + size - 1) >> 32) != 0) { // 32-bit controller: the memory must be below 4 GiB
      b.reset();
      return false;
    }
    return true;
  }

  // ---- start ----

  [[nodiscard]] reloco::result<void> start_impl() noexcept {
    using namespace xhci_detail;
    const std::uint32_t cap0 = rd(0);
    const unsigned caplen = cap0 & 0xFFu;
    if (caplen < 0x20 || (cap0 >> 16) < 0x0100)
      return reloco::unexpected(reloco::error::unsupported_operation);
    const std::uint32_t hcs1 = rd(cap_hcsparams1);
    const std::uint32_t hcs2 = rd(cap_hcsparams2);
    const std::uint32_t hcc1 = rd(cap_hccparams1);
    op_ = caplen;
    db_ = rd(cap_dboff) & ~std::size_t{3};
    rt_ = rd(cap_rtsoff) & ~std::size_t{0x1F};
    ac64_ = (hcc1 & 1u) != 0;
    ctx_size_ = (hcc1 & 4u) != 0 ? 64u : 32u;
    const unsigned hw_slots = hcs1 & 0xFFu;
    max_ports_ = hcs1 >> 24;
    if (hw_slots == 0 || max_ports_ == 0)
      return reloco::unexpected(reloco::error::unsupported_operation);
    max_slots_en_ = hw_slots < max_slots ? hw_slots : max_slots;
    const unsigned scratch = (((hcs2 >> 21) & 0x1Fu) << 5) | ((hcs2 >> 27) & 0x1Fu);

    walk_extended_capabilities(hcc1);

    // Stop whatever the firmware left running, then reset.
    if ((rd(op_ + op_usbsts) & sts_hch) == 0) {
      wr(op_ + op_usbcmd, rd(op_ + op_usbcmd) & ~cmd_rs);
      if (!spin_until([&] { return (rd(op_ + op_usbsts) & sts_hch) != 0; }))
        return reloco::unexpected(reloco::error::timed_out);
    }
    wr(op_ + op_usbcmd, cmd_hcrst);
    if (!spin_until([&] { return (rd(op_ + op_usbcmd) & cmd_hcrst) == 0; }))
      return reloco::unexpected(reloco::error::timed_out);
    if (!spin_until([&] { return (rd(op_ + op_usbsts) & sts_cnr) == 0; }))
      return reloco::unexpected(reloco::error::timed_out);
    if ((rd(op_ + op_pagesize) & 1u) == 0) // only 4 KiB pages are used for scratchpad buffers
      return reloco::unexpected(reloco::error::unsupported_operation);

    if (!alloc(dcbaa_, (max_slots_en_ + 1u) * 8u, 256))
      return reloco::unexpected(reloco::error::allocation_failed);
    if (scratch != 0) {
      if (!alloc(scratch_arr_, scratch * 8u, 64) || !alloc(scratch_pages_, scratch * 4096u, 4096))
        return reloco::unexpected(reloco::error::allocation_failed);
      for (unsigned i = 0; i < scratch; ++i)
        *scratch_arr_.template at<std::uint64_t>(i * 8u) = scratch_pages_.phys_at(i * 4096u);
      *dcbaa_.template at<std::uint64_t>(0) = scratch_arr_.phys();
    }
    if (!cmd_ring_.init(*env_, cmd_ring_trbs) || !alloc(erst_, sizeof(erst_entry), 64) ||
        !alloc(evt_seg_, evt_ring_trbs * sizeof(trb), evt_ring_trbs * sizeof(trb)))
      return reloco::unexpected(reloco::error::allocation_failed);
    if (cmd_ring_.mem.phys() >> 32 != 0 && !ac64_)
      return reloco::unexpected(reloco::error::allocation_failed);
    erst_entry *seg = erst_.template at<erst_entry>(0);
    seg->base = evt_seg_.phys();
    seg->size = evt_ring_trbs;
    evt_deq_ = 0;
    evt_ccs_ = true;

    wr(op_ + op_config, max_slots_en_);
    wr64(op_ + op_dcbaap, dcbaa_.phys());
    wr64(op_ + op_crcr, cmd_ring_.phys(0) | 1u); // RCS = 1
    const std::size_t ir = rt_ + rt_ir0;
    wr(ir + ir_erstsz, 1);
    wr64(ir + ir_erdp, evt_seg_.phys());
    wr64(ir + ir_erstba, erst_.phys());
    wr(ir + ir_imod, 4000); // 1 ms moderation
    wr(ir + ir_iman, iman_ie | iman_ip);

    for (unsigned p = 1; p <= port_count(); ++p) { // PPC controllers need port power
      const std::uint32_t v = rd(portsc_off(p));
      if ((v & pc_pp) == 0)
        wr(portsc_off(p), (v & pc_preserve) | pc_pp);
    }
    env_->barrier();
    wr(op_ + op_usbcmd, cmd_rs | cmd_inte | cmd_hsee);
    if (!spin_until([&] { return (rd(op_ + op_usbsts) & sts_hch) == 0; }))
      return reloco::unexpected(reloco::error::timed_out);
    started_ = true;
    return {};
  }

  /** @brief USB legacy handoff (id 1) and supported protocol (id 2) capabilities. */
  void walk_extended_capabilities(std::uint32_t hcc1) noexcept {
    using namespace xhci_detail;
    n_groups_ = 0;
    std::size_t off = static_cast<std::size_t>(hcc1 >> 16) * 4u;
    for (unsigned guard = 0; off != 0 && guard < 64; ++guard) {
      const std::uint32_t h = rd(off);
      const std::uint32_t id = h & 0xFFu;
      if (id == 1 && (h & (1u << 16)) != 0) { // BIOS owns the controller: request OS ownership
        wr(off, h | (1u << 24));
        (void)spin_until([&] { return (rd(off) & (1u << 16)) == 0; });
      } else if (id == 2 && n_groups_ < max_groups) {
        const std::uint32_t dw2 = rd(off + 8);
        proto_group &g = groups_[n_groups_++];
        g.major = narrow8(h >> 24);
        g.first = narrow8(dw2 & 0xFFu);
        g.count = narrow8((dw2 >> 8) & 0xFFu);
        const unsigned psic = (dw2 >> 28) & 0xFu;
        g.psi_count = narrow8(psic < max_psi ? psic : max_psi);
        for (unsigned i = 0; i < g.psi_count; ++i)
          g.psi[i] = rd(off + 0x10u + 4u * i);
      }
      const std::size_t next = (h >> 8) & 0xFFu;
      if (next == 0)
        break;
      off += next * 4u;
    }
  }

  [[nodiscard]] const proto_group *group_of(unsigned port) const noexcept {
    for (unsigned i = 0; i < n_groups_; ++i)
      if (port >= groups_[i].first && port < static_cast<unsigned>(groups_[i].first) + groups_[i].count)
        return &groups_[i];
    return nullptr;
  }

  /** @brief Port speed ID (PSIV) -> usb_speed using the protocol's speed ID table; default table if unknown. */
  [[nodiscard]] usb_speed map_speed(unsigned port, unsigned psiv) const noexcept {
    const proto_group *g = group_of(port);
    if (g != nullptr) {
      for (unsigned i = 0; i < g->psi_count; ++i) {
        const std::uint32_t d = g->psi[i];
        if ((d & 0xFu) != psiv)
          continue;
        std::uint64_t rate = d >> 16;
        for (unsigned e = 0; e < ((d >> 4) & 3u); ++e)
          rate *= 1000u;
        if (g->major >= 3)
          return rate >= 10'000'000'000ull ? usb_speed::super_plus : usb_speed::super;
        return rate >= 400'000'000ull ? usb_speed::high : (rate >= 10'000'000ull ? usb_speed::full : usb_speed::low);
      }
    }
    switch (psiv) {
    case 2:
      return usb_speed::low;
    case 3:
      return usb_speed::high;
    case 4:
      return usb_speed::super;
    case 5:
      return usb_speed::super_plus;
    default:
      return usb_speed::full;
    }
  }

  // ---- ports ----

  /** @brief Latches a connect change and clears the miscellaneous change bits (not PRC/WRC, never PED). */
  void absorb_port(unsigned p) noexcept {
    using namespace xhci_detail;
    const std::uint32_t v = rd(portsc_off(p));
    if ((v & pc_csc) != 0)
      changed_[p] = true;
    if ((v & pc_misc_changes) != 0)
      wr(portsc_off(p), (v & pc_preserve) | (v & pc_misc_changes));
  }

  void teardown_port(unsigned p, usb_status status) noexcept {
    if (port_slot_[p] != 0)
      teardown_slot(port_slot_[p], status);
    else if (enable_req_.pending && enable_req_.port == p)
      enable_req_.abort = true;
  }

  [[nodiscard]] bool slot_on_port(unsigned p) const noexcept {
    for (unsigned s = 1; s <= max_slots; ++s)
      if (slots_[s].phase != slot_phase::free && slots_[s].port == p)
        return true;
    return enable_req_.pending && enable_req_.port == p;
  }

  void port_event(unsigned p) noexcept {
    using namespace xhci_detail;
    if (p == 0 || p > max_root_ports)
      return;
    const std::uint32_t v = rd(portsc_off(p));
    absorb_port(p);
    if (port_slot_[p] != 0 && ((v & pc_ccs) == 0 || (v & pc_csc) != 0))
      teardown_slot(port_slot_[p], usb_status::disconnected);
  }

  // ---- slots and endpoints ----

  [[nodiscard]] static endpoint_state *find_ep(slot_state &sl, unsigned dci) noexcept {
    for (auto &e : sl.eps)
      if (e.dci == dci)
        return &e;
    return nullptr;
  }

  [[nodiscard]] static std::uint8_t type_code(const usb_pipe &p, bool in) noexcept {
    using namespace xhci_detail;
    switch (p.type) {
    case usb_transfer_type::control:
      return narrow8(ep_type_control);
    case usb_transfer_type::bulk:
      return narrow8(in ? ep_type_bulk_in : ep_type_bulk_out);
    default:
      return narrow8(in ? ep_type_interrupt_in : ep_type_interrupt_out);
    }
  }

  /** @brief Endpoint context interval (2^n * 125 us) from the descriptor's bInterval. */
  [[nodiscard]] static std::uint8_t ctx_interval(const usb_pipe &p) noexcept {
    if (p.type != usb_transfer_type::interrupt)
      return 0;
    unsigned v = p.interval == 0 ? 1u : p.interval;
    if (p.speed >= usb_speed::high) // 2^(bInterval-1) microframes
      return xhci_detail::narrow8(v > 16 ? 15u : v - 1u);
    unsigned exp = 0; // frames: floor(log2(ms)) + 3
    while ((v >> (exp + 1)) != 0)
      ++exp;
    exp += 3;
    return xhci_detail::narrow8(exp > 10 ? 10u : exp);
  }

  [[nodiscard]] endpoint_state *alloc_ep(slot_state &sl, unsigned dci, const usb_pipe &p, bool in) noexcept {
    for (auto &e : sl.eps) {
      if (e.dci != 0)
        continue;
      e = endpoint_state{};
      e.dci = xhci_detail::narrow8(dci);
      e.type = type_code(p, in);
      e.max_packet = p.max_packet;
      e.interval = ctx_interval(p);
      if (!e.tr.init(*env_, ring_trbs)) {
        e = endpoint_state{};
        return nullptr;
      }
      return &e;
    }
    return nullptr;
  }

  [[nodiscard]] unsigned max_dci(const slot_state &sl, unsigned include, unsigned exclude) const noexcept {
    unsigned m = include;
    for (const auto &e : sl.eps)
      if (e.dci != 0 && e.dci != exclude && (e.configured || e.dci == include) && e.dci > m)
        m = e.dci;
    return m < 1 ? 1 : m;
  }

  [[nodiscard]] xhci_detail::input_control_context *in_ctrl(slot_state &sl) const noexcept {
    return sl.in_ctx.template at<xhci_detail::input_control_context>(0);
  }
  [[nodiscard]] xhci_detail::slot_context *in_slot(slot_state &sl) const noexcept {
    return sl.in_ctx.template at<xhci_detail::slot_context>(ctx_size_);
  }
  [[nodiscard]] xhci_detail::endpoint_context *in_ep(slot_state &sl, unsigned dci) const noexcept {
    return sl.in_ctx.template at<xhci_detail::endpoint_context>((dci + 1u) * ctx_size_);
  }
  [[nodiscard]] xhci_detail::endpoint_context *out_ep(slot_state &sl, unsigned dci) const noexcept {
    return sl.dev_ctx.template at<xhci_detail::endpoint_context>(dci * ctx_size_);
  }

  void fill_endpoint_context(xhci_detail::endpoint_context &c, const endpoint_state &ep) const noexcept {
    using namespace xhci_detail;
    c = endpoint_context{};
    const std::uint32_t avg = ep.type == ep_type_control ? 8u : (ep.type == ep_type_bulk_in || ep.type == ep_type_bulk_out
                                                                    ? 1024u
                                                                    : ep.max_packet);
    const bool periodic = ep.type == ep_type_interrupt_in || ep.type == ep_type_interrupt_out;
    const std::uint32_t esit = periodic ? ep.max_packet : 0u; // burst 0: one packet per service interval
    c.dw0 = (static_cast<std::uint32_t>(ep.interval) << 16) | (((esit >> 16) & 0xFFu) << 24);
    c.dw1 = (3u << 1) | (static_cast<std::uint32_t>(ep.type) << 3) | (static_cast<std::uint32_t>(ep.max_packet) << 16);
    c.tr_dequeue = ep.tr.dequeue_ptr();
    c.dw4 = avg | ((esit & 0xFFFFu) << 16);
  }

  void set_slot_entries(slot_state &sl, unsigned entries) const noexcept {
    xhci_detail::slot_context *c = in_slot(sl);
    c->dw0 = (c->dw0 & ~(0x1Fu << 27)) | (entries << 27);
  }

  void request_enable(const xfer_state &st, const usb_transfer &t) noexcept {
    if (enable_req_.pending)
      return;
    enable_req_ = enable_request{};
    enable_req_.pending = true;
    enable_req_.port = st.port;
    enable_req_.bsr = !st.set_address;
    enable_req_.mps = t.pipe.max_packet;
  }

  /** @brief Builds the slot after Enable Slot succeeded: contexts, EP0 ring, DCBAA entry. */
  [[nodiscard]] bool setup_slot(unsigned s, const enable_request &rq) noexcept {
    using namespace xhci_detail;
    slot_state &sl = slots_[s];
    sl = slot_state{};
    sl.port = rq.port;
    sl.addr_bsr = rq.bsr;
    sl.phase = slot_phase::need_address;
    port_slot_[rq.port] = narrow8(s);
    const std::uint32_t pv = rd(portsc_off(rq.port));
    sl.speed_id = narrow8((pv & pc_speed_mask) >> pc_speed_shift);
    const usb_speed spd = map_speed(rq.port, sl.speed_id);
    if (!alloc(sl.in_ctx, 33u * ctx_size_, 64) || !alloc(sl.dev_ctx, 32u * ctx_size_, 64))
      return false;
    endpoint_state &e0 = sl.eps[0];
    e0.dci = 1;
    e0.type = narrow8(ep_type_control);
    e0.max_packet = spd >= usb_speed::super ? std::uint16_t{512} : rq.mps;
    e0.configured = true;
    if (!e0.tr.init(*env_, ring_trbs))
      return false;
    *dcbaa_.template at<std::uint64_t>(s * 8u) = sl.dev_ctx.phys();
    return true;
  }

  void teardown_slot(unsigned s, usb_status status) noexcept {
    slot_state &sl = slots_[s];
    if (sl.phase == slot_phase::free || sl.phase == slot_phase::disable_pending || sl.phase == slot_phase::disabling)
      return;
    sl.phase = slot_phase::disable_pending;
    if (port_slot_[sl.port] == s)
      port_slot_[sl.port] = 0;
    for (auto &a : addr_slot_)
      if (a == s)
        a = 0;
    sl.addr_xfer = nullptr;
    sl.pending_final = false;
    reloco::array<usb_transfer *, MaxTransfers> done{};
    std::size_t n = 0;
    for (std::size_t i = 0; i < MaxTransfers; ++i) {
      auto &e = table_[i];
      if (!e.xfer || !belongs(e.st, s, sl))
        continue;
      if (e.st.phase == xphase::active)
        if (endpoint_state *ep = find_ep(sl, e.st.dci))
          ep->active = nullptr; // bounce stays until the slot is freed
      done[n++] = e.xfer;
      table_.release(e);
    }
    for (std::size_t i = 0; i < n; ++i)
      done[i]->complete(status, 0);
  }

  [[nodiscard]] static bool belongs(const xfer_state &st, unsigned s, const slot_state &sl) noexcept {
    return st.slot != 0 ? st.slot == s : (st.addr0 && st.port == sl.port);
  }

  void fail_entry(entry &e, usb_status status) noexcept {
    usb_transfer *t = e.xfer;
    table_.release(e);
    t->complete(status, 0);
  }

  void fail_queued_on_ep(unsigned s, unsigned dci, usb_status status) noexcept {
    reloco::array<usb_transfer *, MaxTransfers> done{};
    std::size_t n = 0;
    for (std::size_t i = 0; i < MaxTransfers; ++i) {
      auto &e = table_[i];
      if (e.xfer && e.st.phase == xphase::queued && e.st.dci == dci && e.st.slot == s) {
        done[n++] = e.xfer;
        table_.release(e);
      }
    }
    for (std::size_t i = 0; i < n; ++i)
      done[i]->complete(status, 0);
  }

  // ---- queue / transfer start ----

  void pump() noexcept {
    if (in_pump_)
      return;
    in_pump_ = true;
    bool progress = true;
    while (progress) {
      progress = false;
      std::uint32_t last = 0;
      for (;;) {
        entry *best = nullptr;
        for (std::size_t i = 0; i < MaxTransfers; ++i) {
          auto &e = table_[i];
          if (e.xfer && e.st.phase == xphase::queued && e.st.seq > last && (best == nullptr || e.st.seq < best->st.seq))
            best = &e;
        }
        if (best == nullptr)
          break;
        last = best->st.seq;
        if (try_start(*best))
          progress = true;
      }
    }
    in_pump_ = false;
  }

  /** @brief True if something changed (started, failed, or a plan was begun). */
  [[nodiscard]] bool try_start(entry &e) noexcept {
    using namespace xhci_detail;
    xfer_state &st = e.st;
    usb_transfer &t = *e.xfer;
    if (st.addr0) {
      st.slot = port_slot_[st.port];
      if (st.slot == 0) {
        request_enable(st, t);
        return false;
      }
    }
    slot_state &sl = slots_[st.slot];
    if (st.set_address)
      return begin_set_address(e, sl);
    const bool usable = st.addr0 ? (sl.phase == slot_phase::default_state || sl.phase == slot_phase::addressed)
                                 : sl.phase == slot_phase::addressed;
    if (!usable)
      return false;
    endpoint_state *ep = find_ep(sl, st.dci);
    if (ep == nullptr || ep->dead) {
      fail_entry(e, usb_status::bus_error);
      return true;
    }
    if (ep->plan != ep_plan::none || ep->active != nullptr)
      return false;
    if (!ep->configured) {
      ep->plan = ep_plan::configure;
      ep->plan_step = 0;
      ep->attempts = 0;
      return true;
    }
    if (st.dci == 1 && spd_of(sl) < usb_speed::super && t.pipe.max_packet != ep->max_packet &&
        (t.pipe.max_packet == 8 || t.pipe.max_packet == 16 || t.pipe.max_packet == 32 || t.pipe.max_packet == 64)) {
      ep->max_packet = t.pipe.max_packet;
      ep->plan = ep_plan::evaluate;
      ep->plan_step = 0;
      ep->attempts = 0;
      return true;
    }
    return start_td(e, sl, *ep);
  }

  [[nodiscard]] usb_speed spd_of(const slot_state &sl) const noexcept { return map_speed(sl.port, sl.speed_id); }

  [[nodiscard]] bool begin_set_address(entry &e, slot_state &sl) noexcept {
    usb_transfer &t = *e.xfer;
    const bool in_progress = sl.phase == slot_phase::need_address || sl.phase == slot_phase::addressing;
    if ((sl.phase != slot_phase::default_state && !in_progress) || sl.addr_xfer != nullptr) {
      fail_entry(e, usb_status::bus_error); // already addressed (or two SET_ADDRESS at once)
      return true;
    }
    sl.addr_xfer = &t;
    sl.usb_addr = xhci_detail::narrow8(t.setup.value & 0x7Fu);
    e.st.phase = xphase::waiting_address;
    if (in_progress) {
      sl.pending_final = true; // the BSR=1 command is still running; re-address when it is done
    } else {
      sl.phase = slot_phase::need_address;
      sl.addr_bsr = false;
      if (endpoint_state *ep0 = find_ep(sl, 1); ep0 != nullptr && spd_of(sl) < usb_speed::super) {
        const std::uint16_t m = t.pipe.max_packet;
        if (m == 8 || m == 16 || m == 32 || m == 64)
          ep0->max_packet = m; // the context in the Address Device command carries the real EP0 packet size
      }
    }
    return true;
  }

  /** @brief Number of TRBs a data buffer of `len` bytes at `phys` needs (no TRB crosses a 64 KiB boundary). */
  [[nodiscard]] static unsigned count_chunks(std::uint64_t phys, std::size_t len) noexcept {
    unsigned n = 0;
    std::size_t off = 0;
    while (off < len) {
      const std::size_t room = 0x10000u - ((phys + off) & 0xFFFFu);
      const std::size_t chunk = len - off < room ? len - off : room;
      off += chunk;
      ++n;
    }
    return n;
  }

  [[nodiscard]] static reloco::span<std::uint8_t> xfer_span(const usb_transfer &t, std::size_t n) noexcept {
    return reloco::span<std::uint8_t>(static_cast<std::uint8_t *>(t.data), n);
  }

  [[nodiscard]] bool start_td(entry &e, slot_state &, endpoint_state &ep) noexcept {
    using namespace xhci_detail;
    usb_transfer &t = *e.xfer;
    xfer_state &st = e.st;
    const bool in = t.is_in();
    const bool control = t.pipe.type == usb_transfer_type::control;
    const std::size_t len = t.length;
    if (len != 0) {
      std::size_t align = 64;
      while (align < len && align < 4096)
        align <<= 1;
      if (!alloc(ep.bounce, len, align)) {
        fail_entry(e, usb_status::bus_error);
        return true;
      }
      if (!in)
        ep.bounce.copy_in(0, reloco::span<const std::uint8_t>(xfer_span(t, len)));
    }
    const std::uint64_t bp = len != 0 ? ep.bounce.phys() : 0;
    const unsigned data_trbs = len == 0 ? (control ? 0u : 1u) : count_chunks(bp, len);
    const unsigned total = data_trbs + (control ? 2u : 0u);
    if (total > max_td_trbs) {
      ep.bounce.reset();
      fail_entry(e, usb_status::bus_error);
      return true;
    }

    const bool cycle0 = ep.tr.pcs;
    unsigned first = 0;
    unsigned count = 0;
    auto put = [&](std::uint64_t param, std::uint32_t status, std::uint32_t control_bits) {
      const unsigned idx = ep.tr.push(param, status, control_bits, count == 0);
      if (count++ == 0)
        first = idx;
    };
    if (control) {
      const auto sb = t.setup.to_bytes();
      std::uint64_t setup = 0;
      for (unsigned i = 0; i < 8; ++i)
        setup |= static_cast<std::uint64_t>(sb[i]) << (8u * i);
      const std::uint32_t trt = t.setup.length == 0 ? 0u : (in ? 3u : 2u);
      put(setup, 8, (trb_setup << 10) | trb_idt | (trt << 16));
    }
    if (len == 0) {
      if (!control)
        put(0, 0, (trb_normal << 10) | trb_ioc | (in ? trb_isp : 0u));
    } else {
      std::size_t off = 0;
      bool first_chunk = true;
      while (off < len) {
        const std::uint64_t ph = bp + off;
        const std::size_t room = 0x10000u - (ph & 0xFFFFu);
        const std::size_t chunk = len - off < room ? len - off : room;
        const bool last = off + chunk == len;
        std::uint32_t ctl = ((control && first_chunk) ? trb_data : trb_normal) << 10;
        if (control && first_chunk && in)
          ctl |= trb_dir_in;
        if (in)
          ctl |= trb_isp;
        if (!last || control)
          ctl |= trb_chain;
        else
          ctl |= trb_ioc;
        std::uint32_t td_size = 0;
        if (!control && !last) {
          const std::size_t after = len - off - chunk;
          const std::size_t pk = (after + ep.max_packet - 1u) / ep.max_packet;
          td_size = narrow32(pk > 31 ? 31 : pk);
        }
        put(ph, narrow32(chunk) | (td_size << 17), ctl);
        off += chunk;
        first_chunk = false;
      }
    }
    if (control) {
      const bool status_in = len == 0 || !in;
      put(0, 0, (trb_status << 10) | trb_ioc | (status_in ? trb_dir_in : 0u));
    }
    env_->barrier();
    ep.tr.set_cycle(first, cycle0); // hand the TD to the controller: first TRB last
    env_->barrier();
    st.phase = xphase::active;
    st.td_first = narrow16(first);
    st.td_trbs = narrow8(count);
    st.ctl_actual = narrow32(len);
    ep.active = &t;
    wr(db_ + 4u * st.slot, st.dci);
    return true;
  }

  // ---- events ----

  void drain_events() noexcept {
    using namespace xhci_detail;
    bool any = false;
    for (unsigned guard = 0; guard < evt_ring_trbs * 4 && started_; ++guard) {
      env_->barrier();
      const trb ev = *evt_seg_.template at<trb>(evt_deq_ * sizeof(trb));
      if (((ev.control & trb_cycle) != 0) != evt_ccs_)
        break;
      if (++evt_deq_ == evt_ring_trbs) {
        evt_deq_ = 0;
        evt_ccs_ = !evt_ccs_;
      }
      any = true;
      switch ((ev.control >> 10) & 0x3Fu) {
      case trb_ev_transfer:
        transfer_event(ev);
        break;
      case trb_ev_command_completion:
        command_completed(ev);
        break;
      case trb_ev_port_status_change:
        port_event(narrow32(ev.param >> 24) & 0xFFu);
        break;
      default:
        break;
      }
    }
    if (any && started_)
      wr64(rt_ + rt_ir0 + ir_erdp, evt_seg_.phys_at(evt_deq_ * sizeof(trb)) | erdp_ehb);
  }

  [[nodiscard]] static usb_status map_cc(std::uint32_t cc) noexcept {
    using namespace xhci_detail;
    switch (cc) {
    case cc_success:
    case cc_short_packet:
      return usb_status::ok;
    case cc_stall:
      return usb_status::stall;
    case cc_babble:
      return usb_status::babble;
    default:
      return usb_status::bus_error;
    }
  }

  /** @brief Position of ring index `idx` within the TD (td_trbs if it is not part of it). */
  [[nodiscard]] static unsigned td_position(const ring &r, const xfer_state &st, unsigned idx) noexcept {
    unsigned p = st.td_first;
    for (unsigned k = 0; k < st.td_trbs; ++k) {
      if (p == idx)
        return k;
      if (++p == r.size - 1)
        p = 0;
    }
    return st.td_trbs;
  }

  [[nodiscard]] static std::uint32_t trb_len_at(const ring &r, const xfer_state &st, unsigned pos) noexcept {
    unsigned p = st.td_first;
    for (unsigned k = 0; k < pos; ++k)
      if (++p == r.size - 1)
        p = 0;
    return r.at(p)->status & 0x1FFFFu;
  }

  void transfer_event(const xhci_detail::trb &ev) noexcept {
    using namespace xhci_detail;
    const unsigned s = ev.control >> 24;
    const unsigned dci = (ev.control >> 16) & 0x1Fu;
    const std::uint32_t cc = ev.status >> 24;
    const std::uint32_t residual = ev.status & 0xFFFFFFu;
    if (s == 0 || s > max_slots || cc == cc_stopped)
      return;
    slot_state &sl = slots_[s];
    if (sl.phase != slot_phase::default_state && sl.phase != slot_phase::addressed)
      return;
    endpoint_state *ep = find_ep(sl, dci);
    if (ep == nullptr || ep->active == nullptr)
      return;
    entry *e = table_.find(*ep->active);
    if (e == nullptr || e->st.phase != xphase::active)
      return;
    xfer_state &st = e->st;
    usb_transfer &t = *e->xfer;
    const std::uint64_t base = ep->tr.phys(0);
    if (ev.param < base || ev.param >= base + static_cast<std::uint64_t>(ring_trbs) * sizeof(trb))
      return;
    const unsigned k = td_position(ep->tr, st, static_cast<unsigned>((ev.param - base) / sizeof(trb)));
    if (k == st.td_trbs)
      return; // stale event for a TD that is gone

    const bool control = t.pipe.type == usb_transfer_type::control;
    const unsigned first_data = control ? 1u : 0u;
    const unsigned last_k = static_cast<unsigned>(st.td_trbs) - 1u;
    // Bytes moved by the data TRBs before `k`, plus the part of TRB `k` that was transferred.
    auto bytes_through = [&]() noexcept -> std::uint32_t {
      std::uint32_t sum = 0;
      for (unsigned j = first_data; j < k; ++j)
        sum += trb_len_at(ep->tr, st, j);
      if (k >= first_data && !(control && k == last_k)) {
        const std::uint32_t l = trb_len_at(ep->tr, st, k);
        sum += l > residual ? l - residual : 0u;
      }
      return sum;
    };

    if (control && k != last_k && cc == cc_short_packet) {
      st.ctl_actual = bytes_through(); // the status stage still follows and reports completion
      return;
    }
    if (control && k != last_k && cc == cc_success)
      return;
    std::uint32_t actual = 0;
    if (cc == cc_success || cc == cc_short_packet) {
      actual = control ? (k == last_k ? st.ctl_actual : bytes_through())
                       : (cc == cc_success ? narrow32(t.length) - residual : bytes_through());
    } else {
      actual = control && k == last_k ? 0u : bytes_through();
    }
    finish_transfer(*e, *ep, map_cc(cc), actual);
  }

  /** @brief Completes the active transfer: copies IN data, frees the bounce buffer, schedules endpoint recovery
   * after an error, and only then calls `complete()`. */
  void finish_transfer(entry &e, endpoint_state &ep, usb_status status, std::size_t actual) noexcept {
    usb_transfer *t = e.xfer;
    if (status == usb_status::ok && t->is_in() && actual != 0 && ep.bounce)
      ep.bounce.copy_out(0, xfer_span(*t, actual < t->length ? actual : t->length));
    ep.bounce.reset();
    ep.active = nullptr;
    table_.release(e);
    if (status != usb_status::ok)
      start_resync(ep);
    t->complete(status, status == usb_status::ok ? actual : 0);
  }

  void start_resync(endpoint_state &ep) noexcept {
    ep.plan = ep_plan::resync;
    ep.plan_step = 0;
    ep.attempts = 0;
    ep.deq_done = false;
    ep.sent = false;
  }

  // ---- commands ----

  void issue(cmd_kind kind, unsigned s, unsigned dci, std::uint64_t param, std::uint32_t status,
             std::uint32_t control) noexcept {
    const unsigned idx = cmd_ring_.push(param, status, control, false);
    inflight_.kind = kind;
    inflight_.slot = xhci_detail::narrow8(s);
    inflight_.dci = xhci_detail::narrow8(dci);
    inflight_.phys = cmd_ring_.phys(idx);
    env_->barrier();
    wr(db_, 0);
  }

  /** @brief Picks the next command to send (one at a time); disabling slots comes first. */
  void kick() noexcept {
    using namespace xhci_detail;
    if (!started_ || inflight_.kind != cmd_kind::none)
      return;
    for (unsigned s = 1; s <= max_slots; ++s) {
      slot_state &sl = slots_[s];
      if (sl.phase == slot_phase::disable_pending) {
        sl.phase = slot_phase::disabling;
        issue(cmd_kind::disable_slot, s, 0, 0, 0, (trb_cmd_disable_slot << 10) | (s << 24));
        return;
      }
    }
    for (unsigned s = 1; s <= max_slots; ++s) {
      slot_state &sl = slots_[s];
      if (sl.phase != slot_phase::default_state && sl.phase != slot_phase::addressed)
        continue;
      for (auto &ep : sl.eps) {
        if (ep.dci != 0 && ep.plan == ep_plan::none && ep.toggle_reset_req && ep.active == nullptr && ep.configured) {
          ep.toggle_reset_req = false;
          ep.plan = ep_plan::toggle;
          ep.plan_step = 0;
          ep.attempts = 0;
        }
        while (ep.dci != 0 && ep.plan != ep_plan::none && !ep.sent) {
          if (issue_ep_command(s, sl, ep))
            return;
        }
      }
    }
    if (enable_req_.pending && !enable_req_.sent) {
      enable_req_.sent = true;
      issue(cmd_kind::enable_slot, 0, 0, 0, 0, trb_cmd_enable_slot << 10);
      return;
    }
    for (unsigned s = 1; s <= max_slots; ++s) {
      slot_state &sl = slots_[s];
      if (sl.phase != slot_phase::need_address)
        continue;
      sl.phase = slot_phase::addressing;
      endpoint_state &e0 = sl.eps[0];
      input_control_context *c = in_ctrl(sl);
      *c = input_control_context{};
      c->add_flags = 0x3; // A0 (slot) | A1 (EP0)
      slot_context *sc = in_slot(sl);
      *sc = slot_context{};
      sc->dw0 = (1u << 27) | (static_cast<std::uint32_t>(sl.speed_id) << 20);
      sc->dw1 = static_cast<std::uint32_t>(sl.port) << 16;
      fill_endpoint_context(*in_ep(sl, 1), e0);
      env_->barrier();
      issue(cmd_kind::address_device, s, 1, sl.in_ctx.phys(),
            0, (trb_cmd_address_device << 10) | (sl.addr_bsr ? trb_bsr : 0u) | (s << 24));
      return;
    }
  }

  /** @brief Sends the next command of an endpoint plan; false if the plan completed without one. */
  [[nodiscard]] bool issue_ep_command(unsigned s, slot_state &sl, endpoint_state &ep) noexcept {
    using namespace xhci_detail;
    const unsigned dci = ep.dci;
    switch (ep.plan) {
    case ep_plan::configure:
    case ep_plan::toggle: {
      const bool drop = ep.plan == ep_plan::toggle && ep.plan_step == 0;
      input_control_context *c = in_ctrl(sl);
      *c = input_control_context{};
      if (drop) {
        c->drop_flags = 1u << dci;
        c->add_flags = 1;
        set_slot_entries(sl, max_dci(sl, 1, dci));
      } else {
        c->add_flags = 1u | (1u << dci);
        set_slot_entries(sl, max_dci(sl, dci, 0));
        fill_endpoint_context(*in_ep(sl, dci), ep);
      }
      env_->barrier();
      ep.sent = true;
      issue(cmd_kind::configure_endpoint, s, dci, sl.in_ctx.phys(), 0, (trb_cmd_configure_endpoint << 10) | (s << 24));
      return true;
    }
    case ep_plan::evaluate: {
      input_control_context *c = in_ctrl(sl);
      *c = input_control_context{};
      c->add_flags = 0x3;
      fill_endpoint_context(*in_ep(sl, 1), ep);
      env_->barrier();
      ep.sent = true;
      issue(cmd_kind::evaluate_context, s, dci, sl.in_ctx.phys(), 0, (trb_cmd_evaluate_context << 10) | (s << 24));
      return true;
    }
    case ep_plan::resync: {
      env_->barrier();
      const std::uint32_t state = out_ep(sl, dci)->dw0 & 7u;
      const std::uint32_t ids = (dci << 16) | (s << 24);
      if (state == ep_state_running) {
        ep.sent = true;
        issue(cmd_kind::stop_endpoint, s, dci, 0, 0, (trb_cmd_stop_endpoint << 10) | ids);
        return true;
      }
      if (state == ep_state_halted) {
        ep.sent = true;
        issue(cmd_kind::reset_endpoint, s, dci, 0, 0, (trb_cmd_reset_endpoint << 10) | ids);
        return true;
      }
      if ((state == ep_state_stopped || state == ep_state_error) && !ep.deq_done) {
        ep.sent = true;
        issue(cmd_kind::set_tr_dequeue, s, dci, ep.tr.dequeue_ptr(), 0, (trb_cmd_set_tr_dequeue << 10) | ids);
        return true;
      }
      plan_finished(sl, ep); // recovered (or the endpoint is gone)
      return false;
    }
    case ep_plan::none:
      break;
    }
    return false;
  }

  void plan_finished(slot_state &, endpoint_state &ep) noexcept {
    ep.plan = ep_plan::none;
    ep.sent = false;
    if (ep.active == nullptr)
      ep.bounce.reset(); // a cancelled TD's buffer: the controller no longer references it
  }

  void plan_failed(unsigned s, slot_state &sl, endpoint_state &ep, bool give_up) noexcept {
    ep.sent = false;
    if (give_up || ++ep.attempts > 6) {
      ep.plan = ep_plan::none;
      ep.dead = true;
      if (ep.active == nullptr)
        ep.bounce.reset();
      fail_queued_on_ep(s, ep.dci, usb_status::bus_error);
      (void)sl;
    }
  }

  void command_completed(const xhci_detail::trb &ev) noexcept {
    using namespace xhci_detail;
    if (inflight_.kind == cmd_kind::none || ev.param != inflight_.phys)
      return;
    const inflight_cmd cmd = inflight_;
    inflight_ = inflight_cmd{};
    const std::uint32_t cc = ev.status >> 24;
    const unsigned ev_slot = ev.control >> 24;
    const bool ok = cc == cc_success;

    if (cmd.kind == cmd_kind::enable_slot) {
      const enable_request rq = enable_req_;
      enable_req_ = enable_request{};
      if (!ok || ev_slot == 0 || ev_slot > max_slots || slots_[ev_slot].phase != slot_phase::free) {
        fail_port_transfers(rq.port, usb_status::bus_error);
        return;
      }
      const bool built = setup_slot(ev_slot, rq);
      if (!built || rq.abort)
        teardown_slot(ev_slot, built ? usb_status::disconnected : usb_status::bus_error);
      return;
    }

    slot_state &sl = slots_[cmd.slot];
    if (cmd.kind == cmd_kind::disable_slot) {
      *dcbaa_.template at<std::uint64_t>(cmd.slot * 8u) = 0;
      sl = slot_state{};
      return;
    }
    if (sl.phase == slot_phase::disable_pending || sl.phase == slot_phase::disabling || sl.phase == slot_phase::free)
      return; // the slot was torn down while the command ran

    if (cmd.kind == cmd_kind::address_device) {
      if (!ok) {
        teardown_slot(cmd.slot, usb_status::bus_error);
        return;
      }
      address_done(cmd.slot, sl);
      return;
    }

    endpoint_state *ep = find_ep(sl, cmd.dci);
    if (ep == nullptr)
      return;
    switch (cmd.kind) {
    case cmd_kind::configure_endpoint:
      if (!ok) {
        plan_failed(cmd.slot, sl, *ep, true);
      } else if (ep->plan == ep_plan::toggle && ep->plan_step == 0) {
        ep->configured = false;
        ep->plan_step = 1;
        ep->sent = false;
      } else {
        ep->configured = true;
        plan_finished(sl, *ep);
      }
      break;
    case cmd_kind::evaluate_context:
      plan_finished(sl, *ep); // on failure the old packet size stays in the context; the transfer is tried anyway
      break;
    case cmd_kind::stop_endpoint:
    case cmd_kind::reset_endpoint:
      ep->sent = false;
      if (!ok && cc != cc_context_state_error)
        plan_failed(cmd.slot, sl, *ep, false);
      else if (!ok && ++ep->attempts > 6)
        plan_failed(cmd.slot, sl, *ep, true);
      break;
    case cmd_kind::set_tr_dequeue:
      ep->sent = false;
      if (ok)
        ep->deq_done = true;
      else if (++ep->attempts > 6)
        plan_failed(cmd.slot, sl, *ep, true);
      break;
    default:
      break;
    }
  }

  void address_done(unsigned s, slot_state &sl) noexcept {
    const bool bsr = sl.addr_bsr;
    if (bsr) {
      sl.phase = slot_phase::default_state;
      if (sl.pending_final) {
        sl.pending_final = false;
        sl.phase = slot_phase::need_address;
        sl.addr_bsr = false;
      }
      return;
    }
    sl.phase = slot_phase::addressed;
    if (sl.usb_addr != 0)
      addr_slot_[sl.usb_addr] = xhci_detail::narrow8(s);
    if (usb_transfer *t = sl.addr_xfer) {
      sl.addr_xfer = nullptr;
      if (entry *e = table_.find(*t)) {
        table_.release(*e);
        t->complete(usb_status::ok, 0);
      }
    }
  }

  void fail_port_transfers(unsigned port, usb_status status) noexcept {
    reloco::array<usb_transfer *, MaxTransfers> done{};
    std::size_t n = 0;
    for (std::size_t i = 0; i < MaxTransfers; ++i) {
      auto &e = table_[i];
      if (e.xfer && e.st.addr0 && e.st.slot == 0 && e.st.port == port) {
        done[n++] = e.xfer;
        table_.release(e);
      }
    }
    for (std::size_t i = 0; i < n; ++i)
      done[i]->complete(status, 0);
  }

  void release_all() noexcept {
    for (auto &s : slots_)
      s = slot_state{};
    dcbaa_.reset();
    scratch_arr_.reset();
    scratch_pages_.reset();
    cmd_ring_ = ring{};
    erst_.reset();
    evt_seg_.reset();
    inflight_ = inflight_cmd{};
    enable_req_ = enable_request{};
    port_slot_.fill(0);
    addr_slot_.fill(0);
    changed_.fill(false);
    reset_port_ = 0;
  }

  Env *env_;
  bool started_ = false;
  bool in_irq_ = false;
  bool in_pump_ = false;
  bool ac64_ = false;
  unsigned ctx_size_ = 32;
  std::size_t op_ = 0;
  std::size_t db_ = 0;
  std::size_t rt_ = 0;
  unsigned max_slots_en_ = 0;
  unsigned max_ports_ = 0;
  dma_block dcbaa_;
  dma_block scratch_arr_;
  dma_block scratch_pages_;
  ring cmd_ring_;
  dma_block erst_;
  dma_block evt_seg_;
  unsigned evt_deq_ = 0;
  bool evt_ccs_ = true;
  reloco::array<proto_group, max_groups> groups_{};
  unsigned n_groups_ = 0;
  reloco::array<slot_state, max_slots + 1> slots_{};
  reloco::array<std::uint8_t, max_root_ports + 1> port_slot_{};
  reloco::array<bool, max_root_ports + 1> changed_{};
  reloco::array<std::uint8_t, 128> addr_slot_{};
  std::uint8_t reset_port_ = 0;
  inflight_cmd inflight_{};
  enable_request enable_req_{};
  table_type table_{};
  std::uint32_t seq_ = 0;
};

template <typename Env, std::size_t N> struct usb_host_traits<xhci_hcd<Env, N>> {
  using hcd = xhci_hcd<Env, N>;
  static unsigned port_count(hcd &h) noexcept { return h.port_count(); }
  static reloco::result<usb_port_status> port_status(hcd &h, unsigned p) noexcept { return h.port_status(p); }
  static reloco::task<void> reset_port(hcd &h, unsigned p) noexcept { return h.reset_port(p); }
  static reloco::result<void> submit(hcd &h, usb_transfer &t) noexcept { return h.submit(t); }
  static void cancel(hcd &h, usb_transfer &t) noexcept { h.cancel(t); }
  static void reset_data_toggle(hcd &h, const usb_pipe &p) noexcept { h.reset_data_toggle(p); }
};

} // namespace structo::hw

#endif // RELOCO_HAS_COROUTINES
