// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

// Register-level xHCI model + tests for structo::hw::xhci_hcd. The model implements the capability /
// operational / runtime / doorbell registers, the command ring, the event ring and the transfer rings
// (TRB by TRB, cycle bits, Link TRBs), and runs every packet against the shared usb_sim::device.
// It owns the data toggles (xHCI hides them from software) and validates every DMA access against the live
// allocations (freed memory is poisoned and any access to it is counted in `violations`).

#include <gtest/gtest.h>
#include <structo/bootldr/usb_stack.hpp>
#include <structo/hw/xhci_hcd.hpp>
#include <structo/usb/usb_host.hpp>

#include <reloco/array.hpp>
#include <reloco/coroutine.hpp>
#include <reloco/span.hpp>

#include "usb_hcd_sim.hpp"

using namespace structo;
using namespace structo::hw;

#if RELOCO_HAS_COROUTINES

namespace {

namespace xd = structo::hw::xhci_detail;

constexpr std::uint64_t phys_base = 0x1000;
constexpr std::size_t arena_size = std::size_t{16} << 20;
alignas(4096) reloco::array<std::uint8_t, arena_size> g_arena{};
reloco::array<std::uint8_t, 65536> g_scratch{};
// Views stored globally so that subspans of them are not flagged as dangling.
const reloco::span<std::uint8_t> g_all = g_arena.as_span();
const reloco::span<std::uint8_t> g_scr = g_scratch.as_span();

constexpr std::uint32_t u32(std::uint64_t v) noexcept { return static_cast<std::uint32_t>(v); }
constexpr std::uint8_t u8(std::uint64_t v) noexcept { return static_cast<std::uint8_t>(v); }
constexpr std::uint64_t u64(std::uint64_t v) noexcept { return v; }

// Register map of the model (offsets from the window start).
constexpr std::size_t m_caplen = 0x40;
constexpr std::size_t m_db = 0x1000;
constexpr std::size_t m_rt = 0x2000;
constexpr std::size_t m_ecp = 0x800;
constexpr unsigned m_ports = 4; // 1-2: USB2 group, 3-4: USB3 group
constexpr unsigned m_slots = 16;

struct mtrb {
  std::uint64_t param = 0;
  std::uint32_t status = 0;
  std::uint32_t control = 0;
  [[nodiscard]] unsigned type() const noexcept { return (control >> 10) & 0x3Fu; }
};

struct ep_model {
  bool valid = false;
  unsigned state = 0;
  unsigned type = 0;
  unsigned mps = 0;
  unsigned interval = 0;
  std::uint64_t deq = 0;
  bool dcs = true;
  bool rung = false;
  bool toggle = false;
  std::size_t td_off = 0;
};

struct slot_model {
  bool enabled = false;
  bool addressed = false;
  unsigned port = 0;
  unsigned speed = 0;
  unsigned xaddr = 0;
  reloco::array<ep_model, 32> ep{};
};

struct port_model {
  bool ccs = false;
  bool ped = false;
  bool pp = false;
  bool pr = false;
  unsigned pls = 5;
  unsigned speed = 0;
  bool csc = false, prc = false, wrc = false, pec = false;
  unsigned reset_left = 0;
  bool warm = false;
};

struct dma_rec {
  std::size_t off;
  std::size_t size;
  bool live;
};

struct tr_info {
  std::uint64_t ptr = 0;
  std::uint64_t buf = 0;
  std::uint32_t len = 0;
  std::uint32_t ctl = 0;
};

struct td_info {
  reloco::array<tr_info, 20> t{};
  std::size_t n = 0;
  std::uint64_t end_deq = 0;
  bool end_dcs = true;
};

// Test Env and xHCI model in one object: the driver sees it as its Env, the tests drive it through step().
struct sim_env {
  // ---- knobs ----
  bool csz64;
  bool ac64 = true;
  std::uint32_t hciversion = 0x0110;
  bool hcrst_stuck = false;
  bool cnr_stuck = false;
  unsigned hcrst_delay = 3;
  unsigned cnr_delay = 3;
  unsigned reset_ms = 10;
  bool reset_stuck = false;
  bool bypass = false; // ep1 bulk goes through the model's big FIFO (the sim device's loopback is only 8 KiB)
  std::size_t fail_alloc_at = 0; // 1-based allocation number that fails; 0 = never

  // ---- observations ----
  unsigned violations = 0;
  unsigned bad_commands = 0;
  unsigned ped_clobbers = 0;
  unsigned eval_count = 0;
  bool last_reset_warm = false;
  reloco::array<unsigned, 64> cmd_count{};
  usb_sim::device dev;

  explicit sim_env(bool csz) : csz64(csz) {}

  // ---- Env contract ----
  std::uint32_t read32(std::size_t off) noexcept { return reg_read(off); }
  void write32(std::size_t off, std::uint32_t v) noexcept { reg_write(off, v); }
  void barrier() noexcept {}
  reloco::task<void> delay_ms(unsigned ms) noexcept {
    tick_ms(ms);
    step();
    co_return;
  }
  usb_dma_buffer dma_alloc(std::size_t size, std::size_t align) noexcept {
    ++alloc_count_;
    if (fail_alloc_at != 0 && alloc_count_ == fail_alloc_at)
      return {};
    const std::size_t off = (bump_ + align - 1) & ~(align - 1);
    if (off + size > arena_size || n_blocks_ == blocks_.size())
      return {};
    bump_ = off + size;
    blocks_[n_blocks_++] = {off, size, true};
    for (auto &b : g_all.subspan(off, size))
      b = 0;
    return {&g_arena[off], phys_base + off, size};
  }
  void dma_free(const usb_dma_buffer &b) noexcept {
    const std::size_t off = static_cast<std::size_t>(b.phys - phys_base);
    for (std::size_t i = 0; i < n_blocks_; ++i) {
      if (blocks_[i].off == off && blocks_[i].live) {
        blocks_[i].live = false;
        for (auto &c : g_all.subspan(off, blocks_[i].size))
          c = 0xDE;
        return;
      }
    }
    ++violations; // double free / foreign pointer
  }
  [[nodiscard]] std::size_t live_blocks() const noexcept {
    std::size_t n = 0;
    for (std::size_t i = 0; i < n_blocks_; ++i)
      n += blocks_[i].live ? 1u : 0u;
    return n;
  }
  [[nodiscard]] bool is_live(std::uint64_t phys, std::size_t n) const noexcept {
    if (phys < phys_base)
      return false;
    const std::size_t off = static_cast<std::size_t>(phys - phys_base);
    for (std::size_t i = 0; i < n_blocks_; ++i)
      if (blocks_[i].live && off >= blocks_[i].off && off + n <= blocks_[i].off + blocks_[i].size)
        return true;
    return false;
  }

  // ---- scenario control ----
  void connect(unsigned port0, unsigned psiv) noexcept {
    port_model &p = ports_[port0];
    p.ccs = true;
    p.ped = false;
    p.pls = 5;
    p.speed = psiv;
    p.csc = true;
    dev.reset_bus();
    post_port_event(port0 + 1);
  }
  void disconnect(unsigned port0) noexcept {
    port_model &p = ports_[port0];
    p.ccs = false;
    p.ped = false;
    p.pls = 5;
    p.csc = true;
    post_port_event(port0 + 1);
  }
  [[nodiscard]] port_model &port(unsigned port0) noexcept { return ports_[port0]; }
  [[nodiscard]] slot_model &slot(unsigned s) noexcept { return slots_[s]; }
  [[nodiscard]] unsigned enabled_slots() const noexcept {
    unsigned n = 0;
    for (unsigned s = 1; s <= m_slots; ++s)
      n += slots_[s].enabled ? 1u : 0u;
    return n;
  }
  [[nodiscard]] bool running() const noexcept { return running_; }
  [[nodiscard]] bool inte() const noexcept { return inte_; }
  [[nodiscard]] unsigned config_slots() const noexcept { return config_; }
  [[nodiscard]] std::uint64_t dcbaap() const noexcept { return dcbaap_; }
  [[nodiscard]] unsigned erstsz() const noexcept { return erstsz_; }
  [[nodiscard]] std::uint32_t imod() const noexcept { return imod_; }
  [[nodiscard]] std::uint64_t crcr() const noexcept { return crcr_; }

  /** @brief Advances time: port resets finish. */
  void tick_ms(unsigned ms) noexcept {
    for (unsigned i = 0; i < m_ports; ++i) {
      port_model &p = ports_[i];
      if (p.reset_left == 0 || reset_stuck)
        continue;
      p.reset_left = p.reset_left > ms ? p.reset_left - ms : 0;
      if (p.reset_left == 0) {
        p.pr = false;
        p.ped = p.ccs;
        p.pls = 0;
        p.prc = true;
        p.wrc = p.warm;
        dev.reset_bus();
        post_port_event(i + 1);
      }
    }
  }

  /** @brief Runs the controller: commands first, then every running endpoint with a doorbell. */
  void step() noexcept {
    if (!running_)
      return;
    if (cmd_rung_)
      process_commands();
    for (unsigned s = 1; s <= m_slots; ++s) {
      if (!slots_[s].enabled)
        continue;
      for (unsigned d = 1; d < 32; ++d) {
        ep_model &e = slots_[s].ep[d];
        if (!e.valid || e.state != xd::ep_state_running || !e.rung)
          continue;
        const bool idle = d == 1 ? run_control(s, e) : run_bulk(s, d, e);
        if (idle)
          e.rung = false;
      }
    }
  }

private:
  // ---- memory access (validated) ----
  reloco::span<std::uint8_t> mem(std::uint64_t phys, std::size_t n) noexcept {
    if (!is_live(phys, n)) {
      ++violations;
      return g_scr.subspan(0, n < g_scr.size() ? n : g_scr.size());
    }
    return g_all.subspan(static_cast<std::size_t>(phys - phys_base), n);
  }
  std::uint32_t ld32(std::uint64_t p) noexcept {
    auto s = mem(p, 4);
    return u32(s[0]) | (u32(s[1]) << 8) | (u32(s[2]) << 16) | (u32(s[3]) << 24);
  }
  std::uint64_t ld64(std::uint64_t p) noexcept { return u64(ld32(p)) | (u64(ld32(p + 4)) << 32); }
  void st32(std::uint64_t p, std::uint32_t v) noexcept {
    auto s = mem(p, 4);
    s[0] = u8(v);
    s[1] = u8(v >> 8);
    s[2] = u8(v >> 16);
    s[3] = u8(v >> 24);
  }
  void st64(std::uint64_t p, std::uint64_t v) noexcept {
    st32(p, u32(v));
    st32(p + 4, u32(v >> 32));
  }
  mtrb ld_trb(std::uint64_t p) noexcept { return {ld64(p), ld32(p + 8), ld32(p + 12)}; }

  // ---- registers ----
  [[nodiscard]] std::uint32_t port_read(unsigned i) const noexcept {
    const port_model &p = ports_[i];
    return (p.ccs ? 1u : 0u) | (p.ped ? 2u : 0u) | (p.pr ? 16u : 0u) | (p.pls << 5) | (p.pp ? (1u << 9) : 0u) |
           (p.speed << 10) | (p.csc ? (1u << 17) : 0u) | (p.pec ? (1u << 18) : 0u) | (p.wrc ? (1u << 19) : 0u) |
           (p.prc ? (1u << 21) : 0u);
  }

  void port_write(unsigned i, std::uint32_t v) noexcept {
    port_model &p = ports_[i];
    if ((v & xd::pc_ped) != 0) {
      p.ped = false; // write-1-to-disable: the driver must never do this
      ++ped_clobbers;
    }
    if ((v & xd::pc_csc) != 0)
      p.csc = false;
    if ((v & xd::pc_pec) != 0)
      p.pec = false;
    if ((v & xd::pc_wrc) != 0)
      p.wrc = false;
    if ((v & xd::pc_prc) != 0)
      p.prc = false;
    p.pp = (v & xd::pc_pp) != 0;
    const bool usb3 = i >= 2;
    if ((v & xd::pc_pr) != 0 && p.ccs && p.reset_left == 0) {
      p.pr = true;
      p.reset_left = reset_ms;
      p.warm = false;
      last_reset_warm = false;
    }
    if ((v & xd::pc_wpr) != 0 && usb3 && p.ccs && p.reset_left == 0) {
      p.pr = true;
      p.reset_left = reset_ms;
      p.warm = true;
      last_reset_warm = true;
    }
    if ((v & xd::pc_lws) != 0)
      p.pls = (v >> 5) & 0xFu;
  }

  [[nodiscard]] std::uint32_t ecp_read(std::size_t off) const noexcept {
    switch (off - m_ecp) {
    case 0x00:
      return 2u | (8u << 8) | (0u << 16) | (2u << 24); // USB 2.0, next capability 8 dwords away
    case 0x04:
      return 0x20425355u; // "USB "
    case 0x08:
      return 1u | (2u << 8); // first port 1, count 2, no speed IDs (default table)
    case 0x20:
      return 2u | (0u << 8) | (0u << 16) | (3u << 24); // USB 3.0, last capability
    case 0x24:
      return 0x20425355u;
    case 0x28:
      return 3u | (2u << 8) | (1u << 28); // first port 3, count 2, one speed ID
    case 0x30:
      return 4u | (3u << 4) | (5u << 16); // PSIV 4 = 5 Gb/s
    default:
      return 0;
    }
  }

  std::uint32_t reg_read(std::size_t off) noexcept {
    if (off >= m_ecp && off < m_ecp + 0x40)
      return ecp_read(off);
    if (off < m_caplen) {
      switch (off) {
      case 0x00:
        return (hciversion << 16) | m_caplen;
      case 0x04:
        return m_slots | (1u << 8) | (m_ports << 24);
      case 0x08:
        return 2u << 27; // two scratchpad buffers
      case 0x10:
        return (ac64 ? 1u : 0u) | (csz64 ? 4u : 0u) | (static_cast<std::uint32_t>(m_ecp / 4) << 16);
      case 0x14:
        return m_db;
      case 0x18:
        return m_rt;
      default:
        return 0;
      }
    }
    const std::size_t op = off - m_caplen;
    if (op < 0x400) {
      switch (op) {
      case 0x00: {
        std::uint32_t v = (running_ ? xd::cmd_rs : 0u) | (inte_ ? xd::cmd_inte : 0u);
        if (hcrst_stuck || hcrst_left_ > 0) {
          v |= xd::cmd_hcrst;
          if (!hcrst_stuck && hcrst_left_ > 0)
            --hcrst_left_;
        }
        return v;
      }
      case 0x04: {
        std::uint32_t v = (running_ ? 0u : xd::sts_hch) | (eint_ ? xd::sts_eint : 0u);
        if (cnr_stuck || (hcrst_left_ == 0 && cnr_left_ > 0)) {
          v |= xd::sts_cnr;
          if (!cnr_stuck && hcrst_left_ == 0)
            --cnr_left_;
        }
        return v;
      }
      case 0x08:
        return 1; // 4 KiB pages
      case 0x18:
        return u32(crcr_ & 1u);
      case 0x30:
        return u32(dcbaap_);
      case 0x34:
        return u32(dcbaap_ >> 32);
      case 0x38:
        return config_;
      default:
        return 0;
      }
    }
    if (op < 0x400 + 0x10 * m_ports) {
      const unsigned i = static_cast<unsigned>((op - 0x400) / 0x10);
      return (op - 0x400) % 0x10 == 0 ? port_read(i) : 0u;
    }
    if (off >= m_rt && off < m_rt + 0x40) {
      switch (off - m_rt) {
      case 0x20:
        return (ip_ ? 1u : 0u) | (ie_ ? 2u : 0u);
      case 0x24:
        return imod_;
      case 0x28:
        return erstsz_;
      case 0x30:
        return u32(erstba_);
      case 0x34:
        return u32(erstba_ >> 32);
      case 0x38:
        return u32(erdp_);
      case 0x3C:
        return u32(erdp_ >> 32);
      default:
        return 0;
      }
    }
    return 0;
  }

  void reg_write(std::size_t off, std::uint32_t v) noexcept {
    if (off >= m_db && off < m_db + 4 * (m_slots + 1)) {
      doorbell(static_cast<unsigned>((off - m_db) / 4), v);
      return;
    }
    if (off >= m_rt && off < m_rt + 0x40) {
      switch (off - m_rt) {
      case 0x20:
        if ((v & 1u) != 0)
          ip_ = false;
        ie_ = (v & 2u) != 0;
        break;
      case 0x24:
        imod_ = v;
        break;
      case 0x28:
        erstsz_ = v;
        break;
      case 0x30:
        erstba_ = (erstba_ & ~0xFFFFFFFFull) | v;
        evt_loaded_ = false;
        break;
      case 0x34:
        erstba_ = (erstba_ & 0xFFFFFFFFull) | (u64(v) << 32);
        evt_loaded_ = false;
        break;
      case 0x38:
        erdp_ = (erdp_ & ~0xFFFFFFFFull) | v;
        break;
      case 0x3C:
        erdp_ = (erdp_ & 0xFFFFFFFFull) | (u64(v) << 32);
        break;
      default:
        break;
      }
      return;
    }
    if (off < m_caplen)
      return;
    const std::size_t op = off - m_caplen;
    if (op >= 0x400) {
      if (op < 0x400 + 0x10 * m_ports && (op - 0x400) % 0x10 == 0)
        port_write(static_cast<unsigned>((op - 0x400) / 0x10), v);
      return;
    }
    switch (op) {
    case 0x00:
      if ((v & xd::cmd_hcrst) != 0) {
        reset_controller();
        return;
      }
      running_ = (v & xd::cmd_rs) != 0;
      inte_ = (v & xd::cmd_inte) != 0;
      break;
    case 0x04:
      if ((v & xd::sts_eint) != 0)
        eint_ = false;
      break;
    case 0x18:
      crcr_ = (crcr_ & ~0xFFFFFFFFull) | v;
      sync_cmd_ring();
      break;
    case 0x1C:
      crcr_ = (crcr_ & 0xFFFFFFFFull) | (u64(v) << 32);
      sync_cmd_ring();
      break;
    case 0x30:
      dcbaap_ = (dcbaap_ & ~0xFFFFFFFFull) | v;
      break;
    case 0x34:
      dcbaap_ = (dcbaap_ & 0xFFFFFFFFull) | (u64(v) << 32);
      break;
    case 0x38:
      config_ = v & 0xFFu;
      break;
    default:
      break;
    }
  }

  void reset_controller() noexcept {
    running_ = false;
    inte_ = false;
    eint_ = false;
    ip_ = false;
    config_ = 0;
    dcbaap_ = 0;
    crcr_ = 0;
    erstba_ = 0;
    erdp_ = 0;
    erstsz_ = 0;
    cmd_rung_ = false;
    for (auto &s : slots_)
      s = slot_model{};
    hcrst_left_ = hcrst_delay;
    cnr_left_ = cnr_delay;
  }

  void sync_cmd_ring() noexcept {
    cmd_deq_ = crcr_ & ~0x3Full;
    cmd_ccs_ = (crcr_ & 1u) != 0;
  }

  void doorbell(unsigned n, std::uint32_t v) noexcept {
    if (n == 0) {
      cmd_rung_ = true;
      return;
    }
    const unsigned d = v & 0xFFu;
    if (n > m_slots || d == 0 || d >= 32)
      return;
    ep_model &e = slots_[n].ep[d];
    if (!slots_[n].enabled || !e.valid)
      return;
    if (e.state == xd::ep_state_stopped)
      set_ep_state(n, d, e, xd::ep_state_running);
    e.rung = true;
  }

  // ---- event ring ----
  void load_event_ring() noexcept {
    evt_base_ = ld64(erstba_);
    evt_size_ = ld32(erstba_ + 8);
    evt_enq_ = 0;
    evt_pcs_ = true;
    evt_loaded_ = true;
  }

  void post(std::uint64_t param, std::uint32_t status, std::uint32_t control) noexcept {
    if (!evt_loaded_)
      load_event_ring();
    const std::uint64_t at = evt_base_ + u64(evt_enq_) * 16u;
    st64(at, param);
    st32(at + 8, status);
    st32(at + 12, (control & ~1u) | (evt_pcs_ ? 1u : 0u));
    if (++evt_enq_ == evt_size_) {
      evt_enq_ = 0;
      evt_pcs_ = !evt_pcs_;
    }
    ip_ = true;
    eint_ = true;
  }

  void post_port_event(unsigned port1) noexcept {
    if (running_)
      post(u64(port1) << 24, 1u << 24, xd::trb_ev_port_status_change << 10);
  }

  void post_transfer(unsigned s, unsigned d, std::uint64_t trb, unsigned cc, std::uint32_t residual) noexcept {
    post(trb, (cc << 24) | (residual & 0xFFFFFFu), (xd::trb_ev_transfer << 10) | (d << 16) | (s << 24));
  }

  // ---- contexts ----
  [[nodiscard]] unsigned cs() const noexcept { return csz64 ? 64u : 32u; }

  void set_ep_state(unsigned s, unsigned d, ep_model &e, unsigned st) noexcept {
    e.state = st;
    const std::uint64_t out = ld64(dcbaap_ + 8u * s);
    const std::uint64_t a = out + u64(d) * cs();
    st32(a, (ld32(a) & ~7u) | st);
  }

  // ---- commands ----
  void process_commands() noexcept {
    for (unsigned guard = 0; guard < 64; ++guard) {
      const mtrb c = ld_trb(cmd_deq_);
      if (((c.control & 1u) != 0) != cmd_ccs_)
        break;
      if (c.type() == xd::trb_link) {
        if ((c.control & xd::trb_tc) != 0)
          cmd_ccs_ = !cmd_ccs_;
        cmd_deq_ = c.param & ~0xFull;
        continue;
      }
      const std::uint64_t at = cmd_deq_;
      cmd_deq_ += 16;
      if (c.type() < cmd_count.size())
        ++cmd_count[c.type()];
      unsigned slot_out = c.control >> 24;
      const unsigned cc = exec(c, slot_out);
      post(at, cc << 24, (xd::trb_ev_command_completion << 10) | (slot_out << 24));
    }
    cmd_rung_ = false;
  }

  unsigned bad() noexcept {
    ++bad_commands;
    return 17; // parameter error
  }

  unsigned exec(const mtrb &c, unsigned &slot_out) noexcept {
    const unsigned s = c.control >> 24;
    const unsigned d = (c.control >> 16) & 0x1Fu;
    if (c.type() == xd::trb_cmd_no_op)
      return xd::cc_success;
    if (c.type() == xd::trb_cmd_enable_slot) {
      for (unsigned i = 1; i <= config_ && i <= m_slots; ++i) {
        if (!slots_[i].enabled) {
          slots_[i] = slot_model{};
          slots_[i].enabled = true;
          slot_out = i;
          return xd::cc_success;
        }
      }
      slot_out = 0;
      return xd::cc_no_slots;
    }
    if (s == 0 || s > m_slots || !slots_[s].enabled) {
      ++bad_commands;
      return 11; // slot not enabled
    }
    slot_model &sm = slots_[s];
    switch (c.type()) {
    case xd::trb_cmd_disable_slot:
      sm = slot_model{};
      return xd::cc_success;
    case xd::trb_cmd_address_device:
      return cmd_address(c, s, sm);
    case xd::trb_cmd_configure_endpoint:
      return cmd_configure(c, s, sm);
    case xd::trb_cmd_evaluate_context:
      return cmd_evaluate(c, sm);
    case xd::trb_cmd_stop_endpoint:
    case xd::trb_cmd_reset_endpoint:
    case xd::trb_cmd_set_tr_dequeue:
      break;
    default:
      ++bad_commands;
      return xd::cc_trb_error;
    }
    if (d < 1 || !sm.ep[d].valid) {
      ++bad_commands;
      return 19;
    }
    ep_model &e = sm.ep[d];
    switch (c.type()) {
    case xd::trb_cmd_stop_endpoint:
      if (e.state != xd::ep_state_running)
        return xd::cc_context_state_error;
      set_ep_state(s, d, e, xd::ep_state_stopped);
      e.td_off = 0;
      return xd::cc_success;
    case xd::trb_cmd_reset_endpoint:
      if (e.state != xd::ep_state_halted)
        return xd::cc_context_state_error;
      set_ep_state(s, d, e, xd::ep_state_stopped);
      e.toggle = false; // a reset endpoint restarts at DATA0
      e.td_off = 0;
      return xd::cc_success;
    default: // set TR dequeue pointer
      if (e.state != xd::ep_state_stopped)
        return xd::cc_context_state_error;
      e.deq = c.param & ~0xFull;
      e.dcs = (c.param & 1u) != 0;
      e.td_off = 0;
      return xd::cc_success;
    }
  }

  unsigned cmd_address(const mtrb &c, unsigned s, slot_model &sm) noexcept {
    const std::uint64_t dctx = ld64(dcbaap_ + 8u * s);
    if (dctx == 0 || !is_live(dctx, 32u * cs()))
      return bad();
    const std::uint64_t in = c.param;
    if (ld32(in + 4) != 3u)
      return bad();
    const std::uint32_t sdw0 = ld32(in + cs());
    const unsigned port = (ld32(in + cs() + 4) >> 16) & 0xFFu;
    if ((sdw0 >> 27) < 1 || port < 1 || port > m_ports || !ports_[port - 1].ccs ||
        ((sdw0 >> 20) & 0xFu) != ports_[port - 1].speed)
      return bad();
    const std::uint64_t e0 = in + 2u * cs();
    const std::uint32_t edw1 = ld32(e0 + 4);
    ep_model &e = sm.ep[1];
    e = ep_model{};
    e.valid = true;
    e.type = (edw1 >> 3) & 7u;
    e.mps = edw1 >> 16;
    e.deq = ld64(e0 + 8) & ~0xFull;
    e.dcs = (ld64(e0 + 8) & 1u) != 0;
    if (e.type != xd::ep_type_control || e.mps == 0)
      return bad();
    sm.port = port;
    sm.speed = (sdw0 >> 20) & 0xFu;
    const bool bsr = (c.control & xd::trb_bsr) != 0;
    if (!bsr) {
      if (sm.addressed)
        return bad();
      const unsigned xaddr = 0x20u + s;
      const reloco::array<std::uint8_t, 8> req{0, 5, u8(xaddr), 0, 0, 0, 0, 0};
      (void)dev.setup(req.as_span(), true);
      reloco::array<std::uint8_t, 8> rx{};
      std::size_t n = 0;
      bool tg = false;
      (void)dev.in(0, rx.as_span(), n, tg);
      sm.xaddr = xaddr;
      sm.addressed = true;
    }
    st32(dctx, sdw0);
    st32(dctx + 12, sm.xaddr | ((sm.addressed ? 2u : 1u) << 27));
    st32(dctx + cs(), 1u); // EP0 running
    st32(dctx + cs() + 4, edw1);
    e.state = xd::ep_state_running;
    return xd::cc_success;
  }

  unsigned cmd_configure(const mtrb &c, unsigned s, slot_model &sm) noexcept {
    const std::uint64_t in = c.param;
    const std::uint32_t drop = ld32(in);
    const std::uint32_t add = ld32(in + 4);
    if ((drop & add) != 0 || (add & 1u) == 0 || (drop & 3u) != 0)
      return bad();
    const unsigned entries = ld32(in + cs()) >> 27;
    for (unsigned d = 2; d < 32; ++d) {
      if ((drop >> d & 1u) != 0) {
        if (!sm.ep[d].valid)
          return bad();
        set_ep_state(s, d, sm.ep[d], xd::ep_state_disabled);
        sm.ep[d] = ep_model{};
      }
    }
    for (unsigned d = 2; d < 32; ++d) {
      if ((add >> d & 1u) == 0)
        continue;
      const std::uint64_t ec = in + u64(d + 1) * cs();
      const std::uint32_t dw0 = ld32(ec), dw1 = ld32(ec + 4);
      ep_model &e = sm.ep[d];
      if (e.valid || entries < d || ((dw1 >> 3) & 7u) == 0 || (dw1 >> 16) == 0 || (dw0 >> 16 & 0xFFu) > 15)
        return bad();
      e.valid = true;
      e.type = (dw1 >> 3) & 7u;
      e.mps = dw1 >> 16;
      e.interval = (dw0 >> 16) & 0xFFu;
      e.deq = ld64(ec + 8) & ~0xFull;
      e.dcs = (ld64(ec + 8) & 1u) != 0;
      e.toggle = false;
      const std::uint64_t oc = ld64(dcbaap_ + 8u * s) + u64(d) * cs();
      st32(oc, xd::ep_state_running | (dw0 & ~7u));
      st32(oc + 4, dw1);
      e.state = xd::ep_state_running;
    }
    return xd::cc_success;
  }

  unsigned cmd_evaluate(const mtrb &c, slot_model &sm) noexcept {
    const std::uint64_t in = c.param;
    if ((ld32(in + 4) & 2u) == 0)
      return bad();
    const std::uint32_t dw1 = ld32(in + 2u * cs() + 4);
    if ((dw1 >> 16) == 0)
      return bad();
    sm.ep[1].mps = dw1 >> 16;
    ++eval_count;
    return xd::cc_success;
  }

  // ---- transfers ----
  bool collect(const ep_model &e, td_info &td, bool control) noexcept {
    std::uint64_t pos = e.deq;
    bool dcs = e.dcs;
    td.n = 0;
    bool closed = false;
    for (unsigned guard = 0; guard < 40; ++guard) {
      const mtrb t = ld_trb(pos);
      if (((t.control & 1u) != 0) != dcs)
        return false;
      if (t.type() == xd::trb_link) {
        if ((t.control & xd::trb_tc) != 0)
          dcs = !dcs;
        pos = t.param & ~0xFull;
        continue;
      }
      if (td.n == td.t.size())
        return false;
      td.t[td.n++] = {pos, t.param, t.status & 0x1FFFFu, t.control};
      pos += 16;
      if (control ? t.type() == xd::trb_status : (t.control & xd::trb_chain) == 0) {
        closed = true;
        break;
      }
    }
    if (!closed)
      return false;
    td.end_deq = pos;
    td.end_dcs = dcs;
    return true;
  }

  usb_sim::reply dev_in(unsigned epn, reloco::span<std::uint8_t> buf, std::size_t &n, bool &tg) noexcept {
    if (bypass && epn == 1) {
      if (fifo_rd_ == fifo_wr_) {
        n = 0;
        return usb_sim::reply::nak;
      }
      n = fifo_wr_ - fifo_rd_ < buf.size() ? fifo_wr_ - fifo_rd_ : buf.size();
      for (std::size_t i = 0; i < n; ++i)
        buf[i] = fifo_[fifo_rd_ + i];
      fifo_rd_ += n;
      if (fifo_rd_ == fifo_wr_)
        fifo_rd_ = fifo_wr_ = 0;
      tg = fifo_in_tg_;
      fifo_in_tg_ = !fifo_in_tg_;
      return usb_sim::reply::ack;
    }
    return dev.in(epn, buf, n, tg);
  }

  usb_sim::reply dev_out(unsigned epn, reloco::span<const std::uint8_t> data, bool tg) noexcept {
    if (bypass && epn == 1) {
      if (tg != fifo_out_tg_) {
        dev.note_toggle_error();
        return usb_sim::reply::ack;
      }
      for (std::size_t i = 0; i < data.size(); ++i)
        fifo_[fifo_wr_ + i] = data[i];
      fifo_wr_ += data.size();
      fifo_out_tg_ = !fifo_out_tg_;
      return usb_sim::reply::ack;
    }
    return dev.out(epn, data, tg);
  }

  void halt(unsigned s, unsigned d, ep_model &e, const tr_info &t, unsigned cc, std::size_t done_in_trb) noexcept {
    set_ep_state(s, d, e, xd::ep_state_halted);
    e.td_off = 0;
    post_transfer(s, d, t.ptr, cc, t.len > done_in_trb ? u32(t.len - done_in_trb) : 0u);
  }

  // Bulk/interrupt TD. Returns true if the ring is idle (no more work now), false if blocked or halted.
  bool run_bulk(unsigned s, unsigned d, ep_model &e) noexcept {
    for (unsigned td_guard = 0; td_guard < 64; ++td_guard) {
      td_info td;
      if (!collect(e, td, false))
        return true;
      std::size_t total = 0;
      for (std::size_t i = 0; i < td.n; ++i)
        total += td.t[i].len;
      const bool in = (d & 1u) != 0;
      const unsigned epn = d / 2;
      if (!dev.accepts(slots_[s].xaddr)) {
        halt(s, d, e, td.t[0], xd::cc_transaction_error, 0);
        return false;
      }
      bool first = true;
      bool short_ev = false;
      bool short_done = false;
      while ((first || e.td_off < total) && !short_done) {
        std::size_t idx = 0, base = 0;
        while (idx + 1 < td.n && e.td_off >= base + td.t[idx].len) {
          base += td.t[idx].len;
          ++idx;
        }
        const tr_info &t = td.t[idx];
        const std::size_t inoff = e.td_off - base;
        const std::size_t room = t.len - inoff;
        first = false;
        if (!in) {
          const std::size_t pk = room < e.mps ? room : e.mps;
          const reloco::span<std::uint8_t> data = pk != 0 ? mem(t.buf + inoff, pk) : reloco::span<std::uint8_t>{};
          const usb_sim::reply r = dev_out(epn, reloco::span<const std::uint8_t>(data), e.toggle);
          if (r == usb_sim::reply::nak) {
            first = e.td_off == 0;
            return false;
          }
          if (r == usb_sim::reply::stall) {
            halt(s, d, e, t, xd::cc_stall, inoff);
            return false;
          }
          e.toggle = !e.toggle;
          e.td_off += pk;
        } else {
          reloco::array<std::uint8_t, 1024> rx{};
          std::size_t n = 0;
          bool tg = false;
          const usb_sim::reply r = dev_in(epn, rx.as_span().subspan(0, e.mps), n, tg);
          if (r == usb_sim::reply::nak)
            return false;
          if (r == usb_sim::reply::stall) {
            halt(s, d, e, t, xd::cc_stall, inoff);
            return false;
          }
          if (tg != e.toggle)
            dev.note_toggle_error();
          e.toggle = !e.toggle;
          const std::size_t nn = n < room ? n : room;
          if (nn != 0) {
            const reloco::span<std::uint8_t> dst = mem(t.buf + inoff, nn);
            for (std::size_t i = 0; i < nn; ++i)
              dst[i] = rx[i];
          }
          e.td_off += nn;
          if (n < e.mps) {
            short_done = true;
            if ((t.ctl & xd::trb_isp) != 0) {
              short_ev = true;
              post_transfer(s, d, t.ptr, xd::cc_short_packet, u32(t.len - inoff - nn));
            }
          }
        }
      }
      const tr_info &last = td.t[td.n - 1];
      if (!short_ev && (last.ctl & xd::trb_ioc) != 0)
        post_transfer(s, d, last.ptr, xd::cc_success, 0);
      e.deq = td.end_deq;
      e.dcs = td.end_dcs;
      e.td_off = 0;
    }
    return false;
  }

  // Control TD (Setup [Data...] Status), always executed atomically.
  bool run_control(unsigned s, ep_model &e) noexcept {
    td_info td;
    if (!collect(e, td, true))
      return true;
    if (!dev.accepts(slots_[s].xaddr)) {
      halt(s, 1, e, td.t[0], xd::cc_transaction_error, 0);
      return false;
    }
    const tr_info &st = td.t[td.n - 1];
    reloco::array<std::uint8_t, 8> sb{};
    for (unsigned i = 0; i < 8; ++i)
      sb[i] = u8(td.t[0].buf >> (8u * i));
    (void)dev.setup(sb.as_span(), true);
    bool tg = true;
    if (td.n > 2) {
      const bool in = (td.t[1].ctl & xd::trb_dir_in) != 0;
      std::size_t total = 0;
      for (std::size_t i = 1; i + 1 < td.n; ++i)
        total += td.t[i].len;
      std::size_t off = 0;
      while (off < total) {
        std::size_t idx = 1, base = 0;
        while (idx + 2 < td.n && off >= base + td.t[idx].len) {
          base += td.t[idx].len;
          ++idx;
        }
        const tr_info &t = td.t[idx];
        const std::size_t inoff = off - base;
        const std::size_t room = t.len - inoff;
        if (!in) {
          const std::size_t pk = room < e.mps ? room : e.mps;
          const reloco::span<std::uint8_t> data = mem(t.buf + inoff, pk);
          if (dev.out(0, reloco::span<const std::uint8_t>(data), tg) == usb_sim::reply::stall) {
            halt(s, 1, e, t, xd::cc_stall, inoff);
            return false;
          }
          tg = !tg;
          off += pk;
        } else {
          reloco::array<std::uint8_t, 1024> rx{};
          std::size_t n = 0;
          bool dtg = false;
          if (dev.in(0, rx.as_span().subspan(0, e.mps), n, dtg) == usb_sim::reply::stall) {
            halt(s, 1, e, t, xd::cc_stall, inoff);
            return false;
          }
          if (dtg != tg)
            dev.note_toggle_error();
          tg = !tg;
          const std::size_t nn = n < room ? n : room;
          const reloco::span<std::uint8_t> dst = mem(t.buf + inoff, nn);
          for (std::size_t i = 0; i < nn; ++i)
            dst[i] = rx[i];
          off += nn;
          if (n < e.mps) {
            if ((t.ctl & xd::trb_isp) != 0)
              post_transfer(s, 1, t.ptr, xd::cc_short_packet, u32(t.len - inoff - nn));
            break;
          }
        }
      }
    }
    if ((st.ctl & xd::trb_dir_in) != 0) {
      reloco::array<std::uint8_t, 1024> rx{};
      std::size_t n = 0;
      bool dtg = false;
      if (dev.in(0, rx.as_span().subspan(0, e.mps), n, dtg) == usb_sim::reply::stall) {
        halt(s, 1, e, st, xd::cc_stall, 0);
        return false;
      }
      if (!dtg || n != 0)
        dev.note_toggle_error();
    } else if (dev.out(0, reloco::span<const std::uint8_t>{}, true) == usb_sim::reply::stall) {
      halt(s, 1, e, st, xd::cc_stall, 0);
      return false;
    }
    if ((st.ctl & xd::trb_ioc) != 0)
      post_transfer(s, 1, st.ptr, xd::cc_success, 0);
    e.deq = td.end_deq;
    e.dcs = td.end_dcs;
    return false;
  }

  // ---- state ----
  std::size_t bump_ = 0;
  std::size_t alloc_count_ = 0;
  reloco::array<dma_rec, 8192> blocks_{};
  std::size_t n_blocks_ = 0;

  bool running_ = false, inte_ = false, eint_ = false, ip_ = false, ie_ = false;
  unsigned hcrst_left_ = 0, cnr_left_ = 0;
  unsigned config_ = 0;
  std::uint32_t imod_ = 0;
  unsigned erstsz_ = 0;
  std::uint64_t crcr_ = 0, dcbaap_ = 0, erstba_ = 0, erdp_ = 0;
  std::uint64_t cmd_deq_ = 0;
  bool cmd_ccs_ = true, cmd_rung_ = false;
  bool evt_loaded_ = false, evt_pcs_ = true;
  std::uint64_t evt_base_ = 0;
  unsigned evt_size_ = 0, evt_enq_ = 0;
  reloco::array<port_model, m_ports> ports_{};
  reloco::array<slot_model, m_slots + 1> slots_{};

  reloco::array<std::uint8_t, 131072> fifo_{};
  std::size_t fifo_wr_ = 0, fifo_rd_ = 0;
  bool fifo_out_tg_ = false, fifo_in_tg_ = false;
};

static_assert(is_usb_hcd_env_v<sim_env>);

using test_hcd = xhci_hcd<sim_env, 4>;

struct xfer : usb_transfer {
  [[nodiscard]] bool finished() const noexcept { return state_.load() == done; }
  [[nodiscard]] usb_completion res() const noexcept { return result_; }
};

class XhciTest : public ::testing::TestWithParam<bool> {
protected:
  sim_env env{GetParam()};
  test_hcd hcd{env};
  usb_host_controller_ref ref{hcd};
  usb::usb_host host{ref};
  reloco::array<std::uint8_t, 512> cfg{};
  usb::usb_device udev{ref, reloco::span<std::uint8_t>(cfg)};

  void TearDown() override {
    EXPECT_EQ(env.violations, 0u);
    EXPECT_EQ(env.bad_commands, 0u);
    EXPECT_EQ(env.ped_clobbers, 0u);
    EXPECT_EQ(env.dev.toggle_errors, 0u);
  }

  void up() { ASSERT_TRUE(hcd.start().has_value()); }
  void pump() {
    env.step();
    hcd.irq();
  }
  template <typename T> reloco::result<T> run(reloco::task<T> t) {
    t.resume();
    for (int i = 0; i < 5000 && !t.done(); ++i)
      pump();
    EXPECT_TRUE(t.done());
    return t.take();
  }
  bool wait(const xfer &x, int pumps = 3000) {
    for (int i = 0; i < pumps && !x.finished(); ++i)
      pump();
    return x.finished();
  }
  void settle(int n = 50) {
    for (int i = 0; i < n; ++i)
      pump();
  }

  bool enumerate(unsigned port, unsigned psiv) {
    env.connect(port, psiv);
    settle(4);
    return run(host.enumerate(port, udev)).has_value();
  }

  [[nodiscard]] usb_pipe bulk(unsigned ep, usb_direction d, unsigned mps = 64) const {
    return {udev.address(), static_cast<std::uint8_t>(ep), d, usb_transfer_type::bulk, static_cast<std::uint16_t>(mps),
            usb_speed::full, 0};
  }
  void prep(xfer &x, const usb_pipe &p, reloco::span<std::uint8_t> buf) {
    x.pipe = p;
    x.data = buf.data();
    x.length = buf.size();
  }
  usb_completion do_xfer(const usb_pipe &p, reloco::span<std::uint8_t> buf) {
    xfer x;
    prep(x, p, buf);
    EXPECT_TRUE(hcd.submit(x).has_value());
    EXPECT_TRUE(wait(x));
    return x.res();
  }
};

} // namespace

TEST_P(XhciTest, StartProgramsController) {
  up();
  EXPECT_TRUE(env.running());
  EXPECT_TRUE(env.inte());
  EXPECT_EQ(hcd.context_size(), GetParam() ? 64u : 32u);
  EXPECT_EQ(env.config_slots(), test_hcd::max_slots); // min(16 hardware, 8 driver)
  EXPECT_EQ(hcd.port_count(), 4u);
  EXPECT_EQ(env.erstsz(), 1u);
  EXPECT_NE(env.imod(), 0u);
  EXPECT_EQ(env.crcr() & 1u, 1u); // RCS
  EXPECT_NE(env.dcbaap(), 0u);
  EXPECT_FALSE(hcd.start().has_value()); // already started
}

TEST_P(XhciTest, StartFailsWhenResetNeverCompletes) {
  env.hcrst_stuck = true;
  auto r = hcd.start();
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::timed_out);
  EXPECT_FALSE(hcd.running());
  EXPECT_EQ(env.live_blocks(), 0u);
}

TEST_P(XhciTest, StartFailsWhenControllerNeverReady) {
  env.cnr_stuck = true;
  auto r = hcd.start();
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::timed_out);
  EXPECT_EQ(env.live_blocks(), 0u);
}

TEST_P(XhciTest, StartFailsOnAllocationFailureAndCleansUp) {
  for (std::size_t n = 1; n <= 6; ++n) {
    sim_env e{GetParam()};
    e.fail_alloc_at = n;
    test_hcd h{e};
    auto r = h.start();
    ASSERT_FALSE(r.has_value()) << n;
    EXPECT_EQ(r.error(), reloco::error::allocation_failed);
    EXPECT_EQ(e.live_blocks(), 0u) << n;
  }
}

TEST_P(XhciTest, StartRejectsOldController) {
  env.hciversion = 0x0090;
  auto r = hcd.start();
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::unsupported_operation);
}

TEST_P(XhciTest, StopFreesEverythingAndCancelsPending) {
  up();
  ASSERT_TRUE(enumerate(0, 1));
  reloco::array<std::uint8_t, 16> buf{};
  xfer x;
  prep(x, bulk(1, usb_direction::in), buf.as_span());
  ASSERT_TRUE(hcd.submit(x).has_value());
  settle(10);
  EXPECT_FALSE(x.finished());
  hcd.stop();
  ASSERT_TRUE(x.finished());
  EXPECT_EQ(x.res().status, usb_status::cancelled);
  EXPECT_EQ(env.live_blocks(), 0u);
  EXPECT_FALSE(hcd.running());
}

TEST_P(XhciTest, PortConnectChangeAndSpeeds) {
  up();
  env.connect(0, 1); // USB2 port, full speed
  env.connect(1, 3); // USB2 port, high speed
  env.connect(2, 4); // USB3 port, SuperSpeed (5 Gb/s from the speed ID table)
  settle(3);
  auto a = hcd.port_status(0);
  ASSERT_TRUE(a.has_value());
  EXPECT_TRUE(a->connected);
  EXPECT_FALSE(a->enabled);
  EXPECT_TRUE(a->changed);
  EXPECT_EQ(a->speed, usb_speed::full);
  auto again = hcd.port_status(0);
  ASSERT_TRUE(again.has_value());
  EXPECT_FALSE(again->changed); // cleared by the read
  auto hs = hcd.port_status(1);
  auto ss = hcd.port_status(2);
  auto none = hcd.port_status(3);
  ASSERT_TRUE(hs.has_value() && ss.has_value() && none.has_value());
  EXPECT_EQ(hs->speed, usb_speed::high);
  EXPECT_EQ(ss->speed, usb_speed::super);
  EXPECT_FALSE(none->connected);
  EXPECT_FALSE(hcd.port_status(9).has_value());
}

TEST_P(XhciTest, ResetPortEnablesPortWithoutClobberingPed) {
  up();
  env.connect(0, 3);
  settle(3);
  ASSERT_TRUE(run(hcd.reset_port(0)).has_value());
  auto s = hcd.port_status(0);
  ASSERT_TRUE(s.has_value());
  EXPECT_TRUE(s->connected);
  EXPECT_TRUE(s->enabled);
  auto again = hcd.port_status(0);
  ASSERT_TRUE(again.has_value());
  EXPECT_FALSE(again->changed);
  EXPECT_TRUE(env.port(0).ped);
  EXPECT_EQ(env.port(0).pls, 0u);
  EXPECT_FALSE(env.port(0).prc);
  EXPECT_FALSE(env.last_reset_warm);
}

TEST_P(XhciTest, ResetPortUsb3) {
  up();
  env.connect(2, 4);
  settle(3);
  ASSERT_TRUE(run(hcd.reset_port(2)).has_value());
  EXPECT_TRUE(env.port(2).ped);
  EXPECT_FALSE(env.last_reset_warm);
  EXPECT_FALSE(env.port(2).wrc);
}

TEST_P(XhciTest, ResetPortUsb3InactiveUsesWarmReset) {
  up();
  env.connect(3, 4);
  env.port(3).pls = 6; // Inactive
  settle(3);
  ASSERT_TRUE(run(hcd.reset_port(3)).has_value());
  EXPECT_TRUE(env.last_reset_warm);
  EXPECT_FALSE(env.port(3).wrc);
  EXPECT_TRUE(env.port(3).ped);
}

TEST_P(XhciTest, ResetPortFailures) {
  up();
  auto none = run(hcd.reset_port(0));
  ASSERT_FALSE(none.has_value());
  EXPECT_EQ(none.error(), reloco::error::not_found);
  auto bad = run(hcd.reset_port(9));
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error(), reloco::error::invalid_argument);
  env.connect(0, 3);
  env.reset_stuck = true;
  auto stuck = run(hcd.reset_port(0));
  ASSERT_FALSE(stuck.has_value());
  EXPECT_EQ(stuck.error(), reloco::error::timed_out);
}

TEST_P(XhciTest, EnumerateThroughUsbHost) {
  up();
  ASSERT_TRUE(enumerate(0, 1));
  EXPECT_EQ(udev.address(), 1);
  EXPECT_TRUE(udev.configured());
  EXPECT_EQ(udev.descriptor().vendor_id, 0x1234);
  EXPECT_EQ(udev.speed(), usb_speed::full);
  EXPECT_EQ(env.dev.config, 1);
  EXPECT_EQ(env.cmd_count[xd::trb_cmd_enable_slot], 1u);
  // BSR=1 for the unaddressed GET_DESCRIPTOR, BSR=0 for SET_ADDRESS.
  EXPECT_EQ(env.cmd_count[xd::trb_cmd_address_device], 2u);
  EXPECT_EQ(env.enabled_slots(), 1u);
  // The wire address was chosen by the controller; the host-chosen address (1) is only a handle.
  EXPECT_EQ(env.dev.address, env.slot(1).xaddr);
  EXPECT_NE(env.dev.address, 1);
  EXPECT_EQ(env.slot(1).port, 1u);
  EXPECT_EQ(env.dev.set_config_count, 1u);
}

TEST_P(XhciTest, EnumerateHighSpeedPort) {
  up();
  ASSERT_TRUE(enumerate(1, 3));
  EXPECT_EQ(udev.speed(), usb_speed::high);
  EXPECT_EQ(env.slot(1).port, 2u);
}

TEST_P(XhciTest, EnumerateSuperSpeedForcesEp0PacketSize) {
  up();
  ASSERT_TRUE(enumerate(2, 4));
  EXPECT_EQ(udev.speed(), usb_speed::super);
  EXPECT_EQ(env.slot(1).ep[1].mps, 512u);
  EXPECT_EQ(env.slot(1).port, 3u);
}

TEST_P(XhciTest, ControlInAndOutDataStage) {
  up();
  ASSERT_TRUE(enumerate(0, 1));
  usb_pipe p{udev.address(), 0, usb_direction::out, usb_transfer_type::control, 64, usb_speed::full, 0};
  reloco::array<std::uint8_t, 64> in{};
  xfer a;
  a.pipe = p;
  a.setup = {0x80, 0x06, 0x0301, 0, 64};
  a.data = in.data();
  a.length = in.size();
  ASSERT_TRUE(hcd.submit(a).has_value());
  ASSERT_TRUE(wait(a));
  EXPECT_EQ(a.res().status, usb_status::ok);
  EXPECT_EQ(a.res().actual, 10u); // string descriptor "HCD1"
  EXPECT_EQ(in[0], 10);
  EXPECT_EQ(in[1], 3);

  reloco::array<std::uint8_t, 7> lc{1, 2, 3, 4, 5, 6, 7};
  xfer b;
  b.pipe = p;
  b.setup = {0x21, 0x20, 0, 0, 7};
  b.data = lc.data();
  b.length = lc.size();
  ASSERT_TRUE(hcd.submit(b).has_value());
  ASSERT_TRUE(wait(b));
  EXPECT_EQ(b.res().status, usb_status::ok);
  EXPECT_EQ(env.dev.last_ctl_out_len, 7u);
  EXPECT_EQ(env.dev.last_ctl_out[6], 7);
}

TEST_P(XhciTest, ControlShortPacketReportsActualLength) {
  up();
  ASSERT_TRUE(enumerate(0, 1));
  usb_pipe p{udev.address(), 0, usb_direction::out, usb_transfer_type::control, 64, usb_speed::full, 0};
  reloco::array<std::uint8_t, 128> in{};
  xfer a;
  a.pipe = p;
  a.setup = {0x80, 0x06, 0x0200, 0, 128}; // config descriptor is 39 bytes
  a.data = in.data();
  a.length = in.size();
  ASSERT_TRUE(hcd.submit(a).has_value());
  ASSERT_TRUE(wait(a));
  EXPECT_EQ(a.res().status, usb_status::ok);
  EXPECT_EQ(a.res().actual, 39u);
  EXPECT_EQ(in[1], 2);
}

TEST_P(XhciTest, BulkLoopbackSizes) {
  up();
  ASSERT_TRUE(enumerate(0, 1));
  const reloco::array<std::size_t, 6> sizes{0, 1, 63, 64, 65, 1000};
  for (std::size_t n : sizes) {
    reloco::array<std::uint8_t, 1000> out{}, in{};
    for (std::size_t i = 0; i < n; ++i)
      out[i] = static_cast<std::uint8_t>(i * 7 + n);
    auto o = do_xfer(bulk(1, usb_direction::out), out.as_span().subspan(0, n));
    ASSERT_EQ(o.status, usb_status::ok) << n;
    EXPECT_EQ(o.actual, n);
    if (n == 0)
      continue;
    auto i = do_xfer(bulk(1, usb_direction::in), in.as_span().subspan(0, n));
    ASSERT_EQ(i.status, usb_status::ok) << n;
    EXPECT_EQ(i.actual, n);
    for (std::size_t k = 0; k < n; ++k)
      ASSERT_EQ(in[k], out[k]) << n << " @" << k;
  }
}

TEST_P(XhciTest, BulkLoopbackLargeTransferUsesSeveralTrbsAndWrapsRing) {
  up();
  ASSERT_TRUE(enumerate(0, 1));
  env.bypass = true;
  reloco::array<std::uint8_t, 70000> out{}, in{};
  for (std::size_t i = 0; i < out.size(); ++i)
    out[i] = static_cast<std::uint8_t>(i * 31 + (i >> 8));
  for (int round = 0; round < 4; ++round) { // several TDs: the 16-TRB ring wraps through its Link TRB
    for (auto &b : in)
      b = 0;
    auto o = do_xfer(bulk(1, usb_direction::out), out.as_span());
    ASSERT_EQ(o.status, usb_status::ok);
    EXPECT_EQ(o.actual, out.size());
    auto i = do_xfer(bulk(1, usb_direction::in), in.as_span());
    ASSERT_EQ(i.status, usb_status::ok);
    EXPECT_EQ(i.actual, in.size());
    for (std::size_t k = 0; k < in.size(); ++k)
      ASSERT_EQ(in[k], out[k]) << k;
  }
}

TEST_P(XhciTest, BulkInNakThenDataArrivesLater) {
  up();
  ASSERT_TRUE(enumerate(0, 1));
  reloco::array<std::uint8_t, 32> in{};
  xfer a;
  prep(a, bulk(1, usb_direction::in), in.as_span());
  ASSERT_TRUE(hcd.submit(a).has_value());
  settle(100);
  EXPECT_FALSE(a.finished());
  reloco::array<std::uint8_t, 20> out{};
  for (std::size_t i = 0; i < out.size(); ++i)
    out[i] = static_cast<std::uint8_t>(0xA0 + i);
  auto o = do_xfer(bulk(1, usb_direction::out), out.as_span());
  EXPECT_EQ(o.status, usb_status::ok);
  ASSERT_TRUE(wait(a));
  EXPECT_EQ(a.res().status, usb_status::ok);
  EXPECT_EQ(a.res().actual, 20u); // short packet: 20 of 32
  EXPECT_EQ(in[19], 0xA0 + 19);
}

TEST_P(XhciTest, ShortBulkInSpanningTrbs) {
  up();
  ASSERT_TRUE(enumerate(0, 1));
  reloco::array<std::uint8_t, 300> out{};
  for (std::size_t i = 0; i < out.size(); ++i)
    out[i] = static_cast<std::uint8_t>(i);
  ASSERT_EQ(do_xfer(bulk(1, usb_direction::out), out.as_span()).status, usb_status::ok);
  reloco::array<std::uint8_t, 4000> in{};
  auto r = do_xfer(bulk(1, usb_direction::in), in.as_span());
  EXPECT_EQ(r.status, usb_status::ok);
  EXPECT_EQ(r.actual, 300u);
  EXPECT_EQ(in[299], static_cast<std::uint8_t>(299));
}

TEST_P(XhciTest, InterruptIn) {
  up();
  ASSERT_TRUE(enumerate(0, 1));
  usb_pipe p{udev.address(), 2, usb_direction::in, usb_transfer_type::interrupt, 8, usb_speed::full, 4};
  reloco::array<std::uint8_t, 8> in{};
  xfer a;
  prep(a, p, in.as_span());
  ASSERT_TRUE(hcd.submit(a).has_value());
  settle(30);
  EXPECT_FALSE(a.finished());
  EXPECT_EQ(env.slot(1).ep[5].interval, 5u); // full speed 4 ms -> 2^(5-3) frames
  env.dev.arm_interrupt();
  ASSERT_TRUE(wait(a));
  EXPECT_EQ(a.res().status, usb_status::ok);
  EXPECT_EQ(a.res().actual, env.dev.int_len);
}

TEST_P(XhciTest, StallRecoversEndpoint) {
  up();
  ASSERT_TRUE(enumerate(0, 1));
  reloco::array<std::uint8_t, 8> buf{1, 2, 3, 4, 5, 6, 7, 8};
  for (int round = 0; round < 3; ++round) {
    auto s = do_xfer(bulk(3, usb_direction::out), buf.as_span().subspan(0, 4));
    EXPECT_EQ(s.status, usb_status::stall) << round;
    auto ok = do_xfer(bulk(1, usb_direction::out), buf.as_span());
    EXPECT_EQ(ok.status, usb_status::ok);
  }
  settle(30); // the last recovery runs in the background
  EXPECT_GE(env.cmd_count[xd::trb_cmd_reset_endpoint], 3u);
  EXPECT_GE(env.cmd_count[xd::trb_cmd_set_tr_dequeue], 3u);
  // Set TR Dequeue leaves the endpoint Stopped until the next doorbell.
  EXPECT_EQ(env.slot(1).ep[6].state, xd::ep_state_stopped);
}

TEST_P(XhciTest, ControlStallRecoversEndpointZero) {
  up();
  ASSERT_TRUE(enumerate(0, 1));
  usb_pipe p{udev.address(), 0, usb_direction::out, usb_transfer_type::control, 64, usb_speed::full, 0};
  reloco::array<std::uint8_t, 8> in{};
  xfer a;
  a.pipe = p;
  a.setup = {0xC0, 99, 0, 0, 8}; // unsupported request: the device STALLs the data stage
  a.data = in.data();
  a.length = in.size();
  ASSERT_TRUE(hcd.submit(a).has_value());
  ASSERT_TRUE(wait(a));
  EXPECT_EQ(a.res().status, usb_status::stall);
  xfer b;
  b.pipe = p;
  b.setup = {0x80, 0x06, 0x0301, 0, 8};
  b.data = in.data();
  b.length = in.size();
  ASSERT_TRUE(hcd.submit(b).has_value());
  ASSERT_TRUE(wait(b));
  EXPECT_EQ(b.res().status, usb_status::ok);
  EXPECT_EQ(b.res().actual, 8u);
}

TEST_P(XhciTest, CancelPendingTransferIsSafe) {
  up();
  ASSERT_TRUE(enumerate(0, 1));
  {
    reloco::array<std::uint8_t, 256> in{};
    xfer a;
    prep(a, bulk(1, usb_direction::in), in.as_span());
    ASSERT_TRUE(hcd.submit(a).has_value());
    settle(20);
    hcd.cancel(a);
    settle(40);
    EXPECT_FALSE(a.finished());
    EXPECT_EQ(hcd.transfers_in_use(), 0u);
  }
  EXPECT_GE(env.cmd_count[xd::trb_cmd_stop_endpoint], 1u);
  EXPECT_GE(env.cmd_count[xd::trb_cmd_set_tr_dequeue], 1u);
  // The endpoint works again afterwards and the stale TD never completes.
  reloco::array<std::uint8_t, 10> out{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, in{};
  ASSERT_EQ(do_xfer(bulk(1, usb_direction::out), out.as_span()).status, usb_status::ok);
  auto r = do_xfer(bulk(1, usb_direction::in), in.as_span());
  EXPECT_EQ(r.status, usb_status::ok);
  EXPECT_EQ(r.actual, 10u);
  EXPECT_EQ(in[9], 10);
}

TEST_P(XhciTest, CancelQueuedTransferBehindActiveOne) {
  up();
  ASSERT_TRUE(enumerate(0, 1));
  reloco::array<std::uint8_t, 16> b1{}, b2{};
  xfer a, b;
  prep(a, bulk(1, usb_direction::in), b1.as_span());
  prep(b, bulk(1, usb_direction::in), b2.as_span());
  ASSERT_TRUE(hcd.submit(a).has_value());
  ASSERT_TRUE(hcd.submit(b).has_value());
  settle(10);
  hcd.cancel(b); // still queued: never reached the ring
  EXPECT_EQ(hcd.transfers_in_use(), 1u);
  reloco::array<std::uint8_t, 4> out{9, 8, 7, 6};
  ASSERT_EQ(do_xfer(bulk(1, usb_direction::out), out.as_span()).status, usb_status::ok);
  ASSERT_TRUE(wait(a));
  EXPECT_EQ(a.res().actual, 4u);
  EXPECT_FALSE(b.finished());
}

TEST_P(XhciTest, UnplugWithPendingTransferThenReplugReusesSlot) {
  up();
  ASSERT_TRUE(enumerate(0, 1));
  reloco::array<std::uint8_t, 16> in{};
  xfer a;
  prep(a, bulk(1, usb_direction::in), in.as_span());
  ASSERT_TRUE(hcd.submit(a).has_value());
  settle(10);
  env.disconnect(0);
  ASSERT_TRUE(wait(a));
  EXPECT_EQ(a.res().status, usb_status::disconnected);
  settle(10);
  EXPECT_EQ(env.cmd_count[xd::trb_cmd_disable_slot], 1u);
  EXPECT_EQ(env.enabled_slots(), 0u);
  auto s = hcd.port_status(0);
  ASSERT_TRUE(s.has_value());
  EXPECT_FALSE(s->connected);
  EXPECT_TRUE(s->changed);
  // The old address no longer resolves.
  xfer late;
  prep(late, bulk(1, usb_direction::in), in.as_span());
  auto r = hcd.submit(late);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::not_found);

  // Replug and enumerate again: slot 1 is reused.
  ASSERT_TRUE(enumerate(0, 1));
  EXPECT_EQ(env.enabled_slots(), 1u);
  EXPECT_TRUE(env.slot(1).enabled);
  reloco::array<std::uint8_t, 6> out{1, 2, 3, 4, 5, 6};
  EXPECT_EQ(do_xfer(bulk(1, usb_direction::out), out.as_span()).status, usb_status::ok);
  EXPECT_EQ(env.cmd_count[xd::trb_cmd_enable_slot], 2u);
}

TEST_P(XhciTest, ReEnumerateAfterPortResetDisablesOldSlot) {
  up();
  ASSERT_TRUE(enumerate(0, 1));
  ASSERT_TRUE(run(hcd.reset_port(0)).has_value()); // e.g. a driver resetting a hung device
  EXPECT_EQ(env.cmd_count[xd::trb_cmd_disable_slot], 1u);
  EXPECT_EQ(env.enabled_slots(), 0u);
  ASSERT_TRUE(run(host.enumerate(0, udev)).has_value());
  EXPECT_EQ(env.enabled_slots(), 1u);
}

TEST_P(XhciTest, SubmitBeyondMaxTransfersAndBadArguments) {
  up();
  ASSERT_TRUE(enumerate(0, 1));
  reloco::array<std::uint8_t, 8> b0{}, b1{}, b2{}, b3{}, b4{};
  xfer x0, x1, x2, x3, x4;
  prep(x0, bulk(1, usb_direction::in), b0.as_span());
  prep(x1, bulk(1, usb_direction::in), b1.as_span());
  prep(x2, bulk(1, usb_direction::in), b2.as_span());
  prep(x3, bulk(1, usb_direction::in), b3.as_span());
  prep(x4, bulk(1, usb_direction::in), b4.as_span());
  EXPECT_TRUE(hcd.submit(x0).has_value());
  EXPECT_TRUE(hcd.submit(x1).has_value());
  EXPECT_TRUE(hcd.submit(x2).has_value());
  EXPECT_TRUE(hcd.submit(x3).has_value());
  auto r = hcd.submit(x4);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::try_again);
  hcd.cancel(x3);
  hcd.cancel(x2);
  hcd.cancel(x1);
  hcd.cancel(x0);
  settle(40);

  xfer bad;
  prep(bad, bulk(1, usb_direction::in), b0.as_span());
  bad.pipe.address = 9;
  EXPECT_EQ(hcd.submit(bad).error(), reloco::error::not_found);
  bad.pipe.address = udev.address();
  bad.pipe.type = usb_transfer_type::isochronous;
  EXPECT_EQ(hcd.submit(bad).error(), reloco::error::unsupported_operation);
  bad.pipe.type = usb_transfer_type::bulk;
  bad.pipe.endpoint = 16;
  EXPECT_EQ(hcd.submit(bad).error(), reloco::error::invalid_argument);
  bad.pipe.endpoint = 1;
  bad.length = test_hcd::max_td_bytes + 1;
  EXPECT_EQ(hcd.submit(bad).error(), reloco::error::out_of_range);
}

TEST_P(XhciTest, AddressZeroNeedsAPortReset) {
  up();
  env.connect(0, 1);
  xfer a;
  reloco::array<std::uint8_t, 8> in{};
  a.pipe = {0, 0, usb_direction::out, usb_transfer_type::control, 8, usb_speed::full, 0};
  a.setup = {0x80, 6, 0x0100, 0, 8};
  a.data = in.data();
  a.length = in.size();
  auto r = hcd.submit(a);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::not_found);
}

TEST_P(XhciTest, EvaluateContextWhenEp0PacketSizeChanges) {
  up();
  ASSERT_TRUE(enumerate(0, 1));
  const unsigned before = env.eval_count;
  usb_pipe p{udev.address(), 0, usb_direction::out, usb_transfer_type::control, 32, usb_speed::full, 0};
  reloco::array<std::uint8_t, 18> in{};
  xfer a;
  a.pipe = p;
  a.setup = {0x80, 0x06, 0x0100, 0, 18};
  a.data = in.data();
  a.length = in.size();
  ASSERT_TRUE(hcd.submit(a).has_value());
  ASSERT_TRUE(wait(a));
  EXPECT_EQ(a.res().status, usb_status::ok);
  EXPECT_EQ(a.res().actual, 18u);
  EXPECT_EQ(env.eval_count, before + 1);
  EXPECT_EQ(env.slot(1).ep[1].mps, 32u);
}

TEST_P(XhciTest, ResetDataToggleReconfiguresEndpoint) {
  up();
  ASSERT_TRUE(enumerate(0, 1));
  reloco::array<std::uint8_t, 4> out{1, 2, 3, 4};
  ASSERT_EQ(do_xfer(bulk(1, usb_direction::out), out.as_span()).status, usb_status::ok);
  EXPECT_TRUE(env.slot(1).ep[2].toggle); // DATA1 next
  env.dev.reset_toggles();               // what a CLEAR_FEATURE(HALT) does on the device side
  ref.reset_data_toggle(bulk(1, usb_direction::out));
  settle(20);
  EXPECT_FALSE(env.slot(1).ep[2].toggle);
  EXPECT_EQ(do_xfer(bulk(1, usb_direction::out), out.as_span()).status, usb_status::ok);
}

INSTANTIATE_TEST_SUITE_P(ContextSize, XhciTest, ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool> &i) { return i.param ? "Csz64" : "Csz32"; });

// ---------------------------------------------------------------------------------------------
// bootldr::usb_stack hot plug on top of the driver
// ---------------------------------------------------------------------------------------------

namespace {

struct stack_fixture {
  sim_env env{false};
  test_hcd hcd{env};
  std::uint64_t now = 0;
  bootldr::scheduler sched;
  usb_host_controller_ref ref{hcd};
  bootldr::usb_stack stack;
  unsigned attached = 0, detached = 0, started = 0, saw_disconnect = 0, cleaned = 0;

  static bootldr::usb_stack_config config() {
    bootldr::usb_stack_config c;
    c.poll_ms = 10;
    c.debounce_ms = 20;
    c.max_config_bytes = 1024;
    return c;
  }

  stack_fixture() : stack(sched, ref, config()) {
    sched.set_clock(&clock, this);
    EXPECT_TRUE(sched.add_poller(&service, this).has_value());
    stack.set_event_handler(&on_event, this);
  }
  ~stack_fixture() {
    EXPECT_EQ(env.violations, 0u);
    EXPECT_EQ(env.bad_commands, 0u);
    EXPECT_EQ(env.ped_clobbers, 0u);
  }

  static std::uint64_t clock(void *c) noexcept { return static_cast<stack_fixture *>(c)->now; }
  static void service(void *c) noexcept {
    auto *e = static_cast<stack_fixture *>(c);
    e->now += 10;
    e->env.tick_ms(10);
    e->env.step();
    e->hcd.irq();
  }
  static void on_event(void *c, bootldr::usb_event_kind k, unsigned, bootldr::usb_attached_device *,
                       const reloco::result<void> &) noexcept {
    auto *e = static_cast<stack_fixture *>(c);
    if (k == bootldr::usb_event_kind::attached)
      ++e->attached;
    else if (k == bootldr::usb_event_kind::detached)
      ++e->detached;
  }
  void rounds(int n) {
    for (int i = 0; i < n; ++i)
      sched.run_once();
  }
};

bool match_any(void *, const usb::usb_device &) noexcept { return true; }

reloco::task<void> blocked_driver(void *ctx, bootldr::usb_stack &, bootldr::usb_device_ptr dev) noexcept {
  auto *e = static_cast<stack_fixture *>(ctx);
  ++e->started;
  const usb_pipe in{dev->device().address(), 1, usb_direction::in, usb_transfer_type::bulk, 64, usb_speed::full, 0};
  reloco::array<std::uint8_t, 64> buf{};
  usb_completion c = co_await dev->device().controller().in(in, reloco::span<std::uint8_t>(buf));
  if (c.status == usb_status::disconnected)
    ++e->saw_disconnect;
  co_await dev->gone_event().wait();
  ++e->cleaned;
}

} // namespace

TEST(XhciUsbStack, HotPlugSpawnsDriverAndUnplugFailsItsTransfer) {
  stack_fixture f;
  ASSERT_TRUE(f.hcd.start().has_value());
  ASSERT_TRUE(f.stack.add_driver({&match_any, &blocked_driver, nullptr, &f}).has_value());
  ASSERT_TRUE(f.stack.start().has_value());
  f.rounds(5);
  EXPECT_EQ(f.started, 0u);

  f.env.connect(0, 1);
  f.rounds(40);
  EXPECT_EQ(f.attached, 1u);
  EXPECT_EQ(f.started, 1u);
  EXPECT_EQ(f.hcd.transfers_in_use(), 1u); // the driver is parked on its bulk IN
  EXPECT_EQ(f.env.enabled_slots(), 1u);

  f.env.disconnect(0);
  f.rounds(60);
  EXPECT_EQ(f.detached, 1u);
  EXPECT_EQ(f.saw_disconnect, 1u);
  EXPECT_EQ(f.cleaned, 1u);
  EXPECT_EQ(f.hcd.transfers_in_use(), 0u);
  EXPECT_EQ(f.env.enabled_slots(), 0u);

  f.env.connect(0, 1); // replug: new slot, new driver coroutine
  f.rounds(60);
  EXPECT_EQ(f.attached, 2u);
  EXPECT_EQ(f.started, 2u);
  EXPECT_EQ(f.env.dev.toggle_errors, 0u);
}

#endif // RELOCO_HAS_COROUTINES
