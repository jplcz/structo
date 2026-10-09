// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

// Register-level tests of structo::hw::dwc2_hcd: a test Env (static DMA arena), a DWC2 (Synopsys DesignWare
// USB 2.0 OTG) host-mode controller MODEL that executes a host channel when HCCHAR.ChEna is written, exactly
// like the hardware does in Internal DMA mode (it is written from the DWC2 databook / Linux dwc2 register layout
// and shares no constants with the driver) and the packet-level simulated USB device from usb_hcd_sim.hpp behind
// it. The model counts every programming mistake (misaligned HCDMA, wrong HCTSIZ, OddFrm parity, data without
// PING after NYET, ...) in `bad_programming`, which every test requires to stay 0.

#include <gtest/gtest.h>

#include <structo/bootldr/usb_stack.hpp>
#include <structo/hw/dwc2_hcd.hpp>
#include <structo/usb/usb_host.hpp>

#include <reloco/array.hpp>
#include <reloco/span.hpp>

#include "usb_hcd_sim.hpp"

#if RELOCO_HAS_COROUTINES

using namespace structo;
using namespace structo::hw;

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;

struct sim_dwc2;

// ---------------------------------------------------------------------------------------------
// Env: a 1 MiB arena behind phys = 0x1000 + offset. Every block is tracked; the model accesses memory only
// through window(), which counts a violation for anything outside a live block (use-after-free detector).
// ---------------------------------------------------------------------------------------------

struct test_env {
  static constexpr std::size_t arena_size = 1u << 20;
  static constexpr u32 base_phys = 0x1000;

  struct block {
    std::size_t off = 0;
    std::size_t len = 0;
    bool live = false;
  };

  alignas(4096) reloco::array<u8, arena_size> arena{};
  reloco::array<u8, 4096> junk{};
  reloco::span<u8> mem{arena.as_span()};
  reloco::span<u8> junk_mem{junk.as_span()};
  reloco::array<block, 128> blocks{};
  sim_dwc2 *hc = nullptr;
  unsigned violations = 0;
  unsigned delay_total_ms = 0;
  bool fail_alloc = false;

  u32 read32(std::size_t offset) noexcept;
  void write32(std::size_t offset, u32 value) noexcept;
  void barrier() noexcept {}
  reloco::task<void> delay_ms(unsigned ms) noexcept;

  usb_dma_buffer dma_alloc(std::size_t size, std::size_t align) noexcept {
    if (size == 0 || fail_alloc)
      return {};
    std::size_t cand = 0;
    for (;;) {
      cand = (cand + align - 1) & ~(align - 1);
      if (cand + size > arena_size)
        return {};
      std::size_t bump = 0;
      bool clash = false;
      for (const block &b : blocks) {
        if (b.live && cand < b.off + b.len && b.off < cand + size) {
          clash = true;
          bump = b.off + b.len > bump ? b.off + b.len : bump;
        }
      }
      if (!clash)
        break;
      cand = bump;
    }
    for (block &b : blocks) {
      if (!b.live) {
        b = block{cand, size, true};
        reloco::span<u8> s = mem.subspan(cand, size);
        for (std::size_t i = 0; i < size; ++i)
          s[i] = 0;
        return usb_dma_buffer{s.data(), base_phys + cand, size};
      }
    }
    return {};
  }

  void dma_free(const usb_dma_buffer &buf) noexcept {
    const std::size_t off = static_cast<std::size_t>(buf.phys - base_phys);
    for (block &b : blocks) {
      if (b.live && b.off == off) {
        b.live = false;
        reloco::span<u8> s = mem.subspan(b.off, b.len);
        for (std::size_t i = 0; i < b.len; ++i)
          s[i] = 0xDD;
        return;
      }
    }
    ++violations;
  }

  [[nodiscard]] std::size_t live_blocks() const noexcept {
    std::size_t n = 0;
    for (const block &b : blocks)
      n += b.live ? 1u : 0u;
    return n;
  }

  // Bytes [phys, phys + len) as the controller sees them.
  reloco::span<u8> window(u32 phys, std::size_t len) noexcept {
    if (phys >= base_phys) {
      const std::size_t off = phys - base_phys;
      for (const block &b : blocks)
        if (b.live && off >= b.off && off + len <= b.off + b.len)
          return mem.subspan(off, len);
    }
    ++violations;
    return junk_mem.subspan(0, len < junk_mem.size() ? len : junk_mem.size());
  }
};

static_assert(is_usb_hcd_env_v<test_env>);

enum class dspeed : u8 { hs, fs, ls };

// ---------------------------------------------------------------------------------------------
// The DWC2 controller model (Internal DMA, host mode).
// ---------------------------------------------------------------------------------------------

struct sim_dwc2 {
  static constexpr unsigned max_chan = 16;

  struct chan {
    u32 hcchar = 0, hcsplt = 0, hcint = 0, hcintmsk = 0, hctsiz = 0, hcdma = 0;
    unsigned halt_wait = 0; // steps until a requested disable takes effect
    bool halt_pending = false;
    bool need_ping = false; // high-speed bulk/control OUT endpoint answered NAK/NYET: the host must PING
  };

  test_env &env;
  usb_sim::device dev;

  // ---- knobs ----
  unsigned nchan = 8;
  u32 snpsid = 0x4F54330Au;
  u32 arch = 2; // GHWCFG2.OtgArch.. "architecture": 0 slave only, 1 external DMA, 2 internal DMA
  u32 dfifo_depth = 1024;
  bool reset_never_completes = false;
  bool host_mode_never = false;
  bool reset_leaves_port_disabled = false;
  bool stuck_channel_after_reset = false;
  unsigned halt_delay = 0;   // steps a requested channel disable takes
  unsigned xacterr_runs = 0; // next runs end with XactErr
  unsigned bblerr_runs = 0;
  bool ahb_error_next = false;
  unsigned out_naks = 0;       // next OUT data runs (first packet) are NAKed
  unsigned nyet_next = 0;      // next high-speed bulk OUT packets are accepted but answered NYET
  unsigned ping_naks = 0;      // next PINGs are answered NAK
  unsigned nak_at_packet = 0;  // one-shot: the n-th packet (1-based) of the next data run is NAKed

  // ---- observations ----
  unsigned bad_programming = 0;
  unsigned bad_access = 0;
  unsigned soft_resets = 0;
  unsigned short_resets = 0;
  unsigned port_resets = 0;
  unsigned last_reset_ms = 0;
  unsigned sw_disables = 0;
  unsigned runs = 0;
  unsigned pings = 0;
  unsigned chdis_writes = 0;
  unsigned rx_flushes = 0;
  u32 last_tx_flush = 0;
  u32 max_pktcnt = 0;
  unsigned ep2_polls = 0;
  reloco::array<u32, 128> ep2_frames{};

  // ---- registers ----
  u32 gotgctl = 0, gahbcfg = 0, gusbcfg = 0x1400;
  u32 gintmsk = 0, latched = 0; // latched: SOF (bit 3) and Disconnect (bit 29)
  u32 grxfsiz = 0x200, gnptxfsiz = 0x2000200, hptxfsiz = 0x2000400;
  u32 hcfg = 0, hfir = 0xEA60, haintmsk = 0;
  u32 frame = 0;
  bool host_mode = false;
  unsigned mode_wait = 0;
  bool rst_pending = false;
  unsigned rst_reads = 0;
  u32 flush_bits = 0;
  reloco::array<chan, max_chan> ch{};

  // root port
  bool plugged = false;
  dspeed speed = dspeed::fs;
  bool pwr = false, ena = false, rst = false;
  bool conn_det = false, en_chng = false, ovr_chng = false;
  u32 now_ms = 0, rst_start = 0;

  explicit sim_dwc2(test_env &e) noexcept : env(e) {}

  // ---- the outside world ----
  void plug(dspeed s) noexcept {
    speed = s;
    plugged = true;
    dev.connected_hs = s == dspeed::hs;
    dev.bulk_mps = s == dspeed::hs ? 512 : 64;
    dev.reset_bus();
    if (pwr)
      conn_det = true;
  }
  void unplug() noexcept {
    if (!plugged)
      return;
    plugged = false;
    if (pwr) {
      conn_det = true;
      if (ena)
        en_chng = true;
      ena = false;
      latched |= 1u << 29;
    }
  }
  void raise_overcurrent_change() noexcept { ovr_chng = true; }
  void drop_to_device_mode() noexcept { host_mode = false; }
  void time_passed(unsigned ms) noexcept { now_ms += ms; }

  [[nodiscard]] bool connected() const noexcept { return plugged && pwr; }
  [[nodiscard]] u32 hprt_value() const noexcept {
    u32 v = 0;
    if (connected())
      v |= 1u << 0;
    if (conn_det)
      v |= 1u << 1;
    if (ena)
      v |= 1u << 2;
    if (en_chng)
      v |= 1u << 3;
    if (ovr_chng)
      v |= 1u << 5;
    if (rst)
      v |= 1u << 8;
    if (pwr)
      v |= 1u << 12;
    if (ena && connected())
      v |= (speed == dspeed::hs ? 0u : speed == dspeed::fs ? 1u : 2u) << 17;
    return v;
  }
  [[nodiscard]] u32 haint() const noexcept {
    u32 v = 0;
    for (unsigned c = 0; c < nchan; ++c)
      if ((ch[c].hcint & ch[c].hcintmsk) != 0)
        v |= 1u << c;
    return v;
  }
  [[nodiscard]] u32 gintsts_value() const noexcept {
    u32 v = latched;
    if (host_mode)
      v |= 1u;
    if (conn_det || en_chng || ovr_chng)
      v |= 1u << 24;
    if ((haint() & haintmsk) != 0)
      v |= 1u << 25;
    return v;
  }

  // ---- registers ----
  u32 read32(std::size_t off) noexcept {
    switch (off) {
    case 0x000:
      return gotgctl;
    case 0x008:
      return gahbcfg;
    case 0x00C:
      return gusbcfg;
    case 0x010: {
      u32 v = 1u << 31; // AHBIdle
      if (rst_pending) {
        if (!reset_never_completes && --rst_reads == 0)
          finish_reset();
        else
          v |= 1u;
      }
      v |= flush_bits;
      flush_bits = 0;
      return v;
    }
    case 0x014:
      if (mode_wait != 0 && --mode_wait == 0)
        host_mode = true;
      return gintsts_value();
    case 0x018:
      return gintmsk;
    case 0x024:
      return grxfsiz;
    case 0x028:
      return gnptxfsiz;
    case 0x040:
      return snpsid;
    case 0x048:
      return (arch << 3) | ((nchan - 1u) << 14);
    case 0x04C:
      return dfifo_depth << 16;
    case 0x100:
      return hptxfsiz;
    case 0x400:
      return hcfg;
    case 0x404:
      return hfir;
    case 0x408:
      return frame & 0xFFFFu;
    case 0x414:
      return haint();
    case 0x418:
      return haintmsk;
    case 0x440:
      return hprt_value();
    default:
      break;
    }
    if (off >= 0x500 && off < 0x500 + max_chan * 0x20u) {
      const std::size_t c = (off - 0x500) / 0x20;
      if (c < nchan) {
        const chan &x = ch[c];
        switch ((off - 0x500) % 0x20) {
        case 0x00:
          return x.hcchar;
        case 0x04:
          return x.hcsplt;
        case 0x08:
          return x.hcint;
        case 0x0C:
          return x.hcintmsk;
        case 0x10:
          return x.hctsiz;
        case 0x14:
          return x.hcdma;
        default:
          break;
        }
      }
    }
    ++bad_access;
    return 0;
  }

  void write32(std::size_t off, u32 v) noexcept {
    switch (off) {
    case 0x000:
      gotgctl = v;
      return;
    case 0x008:
      gahbcfg = v;
      return;
    case 0x00C:
      gusbcfg = v;
      if ((v & (1u << 29)) != 0 && !host_mode && mode_wait == 0 && !host_mode_never)
        mode_wait = 3;
      return;
    case 0x010:
      write_grstctl(v);
      return;
    case 0x014:
      latched &= ~(v & ((1u << 3) | (1u << 29)));
      return;
    case 0x018:
      gintmsk = v;
      return;
    case 0x024:
      grxfsiz = v;
      return;
    case 0x028:
      gnptxfsiz = v;
      return;
    case 0x100:
      hptxfsiz = v;
      return;
    case 0x400:
      hcfg = v;
      return;
    case 0x404:
      hfir = v;
      return;
    case 0x418:
      haintmsk = v;
      return;
    case 0x440:
      write_hprt(v);
      return;
    default:
      break;
    }
    if (off >= 0x500 && off < 0x500 + max_chan * 0x20u) {
      const std::size_t c = (off - 0x500) / 0x20;
      if (c < nchan) {
        write_chan(c, (off - 0x500) % 0x20, v);
        return;
      }
    }
    ++bad_access;
  }

  void write_grstctl(u32 v) noexcept {
    if ((v & 1u) != 0) {
      rst_pending = true;
      rst_reads = 3;
    }
    if ((v & (1u << 5)) != 0) {
      flush_bits |= 1u << 5;
      last_tx_flush = (v >> 6) & 0x1Fu;
    }
    if ((v & (1u << 4)) != 0) {
      flush_bits |= 1u << 4;
      ++rx_flushes;
    }
  }

  void finish_reset() noexcept {
    rst_pending = false;
    ++soft_resets;
    gusbcfg &= ~((1u << 29) | (1u << 30));
    gintmsk = 0;
    latched = 0;
    gahbcfg = 0;
    hcfg = 0;
    hfir = 0xEA60;
    haintmsk = 0;
    host_mode = false;
    mode_wait = 0;
    for (chan &c : ch)
      c = chan{};
    if (stuck_channel_after_reset)
      ch[1].hcchar = 1u << 31;
    pwr = false;
    ena = false;
    rst = false;
    conn_det = en_chng = ovr_chng = false;
  }

  void write_hprt(u32 v) noexcept {
    if ((v & (1u << 1)) != 0)
      conn_det = false;
    if ((v & (1u << 3)) != 0)
      en_chng = false;
    if ((v & (1u << 5)) != 0)
      ovr_chng = false;
    if ((v & (1u << 2)) != 0 && ena) { // write-1-to-disable
      ena = false;
      en_chng = true;
      ++sw_disables;
    }
    const bool new_pwr = (v & (1u << 12)) != 0;
    if (new_pwr && !pwr && plugged)
      conn_det = true;
    if (!new_pwr && pwr)
      ena = false;
    pwr = new_pwr;
    const bool new_rst = (v & (1u << 8)) != 0;
    if (new_rst && !rst) {
      rst = true;
      rst_start = now_ms;
      ++port_resets;
      dev.reset_bus();
    } else if (!new_rst && rst) {
      rst = false;
      last_reset_ms = now_ms - rst_start;
      if (last_reset_ms < 50)
        ++short_resets;
      if (connected() && !reset_leaves_port_disabled) {
        ena = true;
        en_chng = true;
      }
    }
  }

  void write_chan(std::size_t c, std::size_t reg, u32 v) noexcept {
    chan &x = ch[c];
    switch (reg) {
    case 0x00:
      write_hcchar(c, v);
      return;
    case 0x04:
      x.hcsplt = v;
      return;
    case 0x08:
      x.hcint &= ~v;
      return;
    case 0x0C:
      x.hcintmsk = v;
      return;
    case 0x10:
      x.hctsiz = v;
      return;
    case 0x14:
      x.hcdma = v;
      return;
    default:
      ++bad_access;
      return;
    }
  }

  void write_hcchar(std::size_t c, u32 v) noexcept {
    chan &x = ch[c];
    const bool en = (v >> 31) != 0;
    const bool dis = ((v >> 30) & 1u) != 0;
    const bool running = (x.hcchar >> 31) != 0;
    if (en && dis) {
      ++chdis_writes;
      if (!running) {
        ++bad_programming;
        return;
      }
      if (halt_delay == 0) {
        halt(c, 0);
      } else {
        x.halt_pending = true;
        x.halt_wait = halt_delay;
      }
      return;
    }
    if (dis) {
      ++bad_programming; // ChDis without ChEna
      return;
    }
    if (!en) {
      x.hcchar = v;
      return;
    }
    if (running)
      ++bad_programming; // enabling a channel that is still enabled
    x.hcchar = v & ~(1u << 30);
    x.halt_pending = false;
    const u32 type = (v >> 18) & 3u;
    if (type == 3) {
      if (((v >> 29) & 1u) != ((frame + 1u) & 1u))
        ++bad_programming; // OddFrm must select the next frame
      return;                // runs from step() in the matching frame
    }
    execute(c);
  }

  void halt(std::size_t c, u32 bits) noexcept {
    chan &x = ch[c];
    x.hcchar &= ~((1u << 31) | (1u << 30));
    x.hcint |= bits | (1u << 1);
    x.halt_pending = false;
  }

  // One frame: periodic channels whose OddFrm matches the new frame run, pending disables count down.
  void step() noexcept {
    ++frame;
    latched |= 1u << 3;
    for (unsigned c = 0; c < nchan; ++c) {
      chan &x = ch[c];
      if ((x.hcchar >> 31) == 0)
        continue;
      if (x.halt_pending) {
        if (--x.halt_wait == 0)
          halt(c, 0);
        continue;
      }
      const u32 type = (x.hcchar >> 18) & 3u;
      if (type == 3 && ((x.hcchar >> 29) & 1u) == (frame & 1u))
        execute(c);
    }
  }

  // ---- channel execution ----
  [[nodiscard]] reloco::span<u8> mem(u32 phys, std::size_t len) noexcept { return env.window(phys, len); }

  void finish(std::size_t c, u32 bits, u32 xfer, u32 pkts, u32 pid, u32 dma) noexcept {
    chan &x = ch[c];
    x.hctsiz = (x.hctsiz & (1u << 31)) | xfer | (pkts << 19) | (pid << 29);
    x.hcdma = dma;
    halt(c, bits);
  }

  void execute(std::size_t c) noexcept {
    chan &x = ch[c];
    ++runs;
    const u32 mps = x.hcchar & 0x7FFu;
    const u32 ep = (x.hcchar >> 11) & 0xFu;
    const bool in = ((x.hcchar >> 15) & 1u) != 0;
    const bool lsdev = ((x.hcchar >> 17) & 1u) != 0;
    const u32 type = (x.hcchar >> 18) & 3u; // 0 control, 1 isochronous, 2 bulk, 3 interrupt
    const u32 mc = (x.hcchar >> 20) & 3u;
    const u32 addr = (x.hcchar >> 22) & 0x7Fu;
    u32 xfer = x.hctsiz & 0x7FFFFu;
    u32 pkts = (x.hctsiz >> 19) & 0x3FFu;
    u32 pid = (x.hctsiz >> 29) & 3u; // 0 DATA0, 1 DATA2, 2 DATA1, 3 MDATA / SETUP
    const bool ping = (x.hctsiz >> 31) != 0;
    u32 dma = x.hcdma;
    if (max_pktcnt < pkts)
      max_pktcnt = pkts;

    if (mps == 0 || pkts == 0 || mc != 1 || type == 1 || x.hcsplt != 0 || (dma & 3u) != 0 || (x.hcintmsk & 2u) == 0)
      ++bad_programming;
    if (in) {
      if (xfer % mps != 0 || xfer > pkts * mps)
        ++bad_programming;
    } else {
      const u32 want = xfer == 0 ? 1u : (xfer + mps - 1) / mps;
      if (pkts != want)
        ++bad_programming;
    }

    if (ahb_error_next) {
      ahb_error_next = false;
      finish(c, 1u << 2, xfer, pkts, pid, dma);
      return;
    }
    if (xacterr_runs != 0) {
      --xacterr_runs;
      finish(c, 1u << 7, xfer, pkts, pid, dma);
      return;
    }
    if (bblerr_runs != 0) {
      --bblerr_runs;
      finish(c, 1u << 8, xfer, pkts, pid, dma);
      return;
    }
    if (lsdev != (speed == dspeed::ls))
      ++bad_programming;
    const bool reach = connected() && ena && dev.accepts(addr) && lsdev == (speed == dspeed::ls);
    if (!reach) {
      finish(c, 1u << 7, xfer, pkts, pid, dma);
      return;
    }
    if (type == 3 && in && ep == 2) {
      if (ep2_polls < ep2_frames.size())
        ep2_frames[ep2_polls] = frame;
      ++ep2_polls;
    }

    const bool hs_ping_ep = speed == dspeed::hs && type != 3;
    u32 bits = 0;
    unsigned idx = 0;
    while (pkts != 0) {
      ++idx;
      const bool data1 = pid == 2;
      const bool nak_now = nak_at_packet != 0 && idx == nak_at_packet && type != 0;
      if (nak_now)
        nak_at_packet = 0;

      if (type == 0 && !in && pid == 3) { // SETUP: always DATA0
        if (xfer != 8)
          ++bad_programming;
        reloco::span<u8> s = mem(dma, 8);
        (void)dev.setup(reloco::span<const u8>(s), true);
        xfer = 0;
        pkts = 0;
        dma += 8;
        bits |= 1u | (1u << 5);
        break;
      }
      if (pid == 3 || pid == 1)
        ++bad_programming; // MDATA/DATA2 are not valid for these transfers

      if (!in) {
        if (idx == 1) {
          if (ping) {
            ++pings;
            if (!hs_ping_ep || type == 3)
              ++bad_programming;
            if (ping_naks != 0) {
              --ping_naks;
              bits |= 1u << 4;
            } else {
              x.need_ping = false;
              bits |= 1u << 5;
            }
            break;
          }
          if (x.need_ping)
            ++bad_programming; // high-speed OUT after NAK/NYET must start with PING
        }
        const u32 n = xfer < mps ? xfer : mps;
        if (nak_now || (out_naks != 0 && type != 0 && idx == 1)) {
          if (!nak_now)
            --out_naks;
          bits |= 1u << 4;
          if (hs_ping_ep)
            x.need_ping = true;
          break;
        }
        reloco::span<u8> w = n != 0 ? mem(dma, n) : reloco::span<u8>{};
        const usb_sim::reply r = dev.out(ep, reloco::span<const u8>(w), data1);
        if (r == usb_sim::reply::stall) {
          bits |= 1u << 3;
          break;
        }
        const bool nyet = hs_ping_ep && type == 2 && nyet_next != 0;
        if (nyet)
          --nyet_next;
        xfer -= n;
        --pkts;
        dma += n;
        pid = pid == 0 ? 2u : 0u;
        if (nyet) {
          x.need_ping = true;
          bits |= 1u << 6;
          if (pkts == 0)
            bits |= 1u;
          break;
        }
        if (pkts == 0)
          bits |= 1u | (1u << 5);
      } else {
        if (nak_now) {
          bits |= 1u << 4;
          break;
        }
        reloco::array<u8, 1024> pk{};
        std::size_t n = 0;
        bool tg = false;
        const usb_sim::reply r = dev.in(ep, pk.as_span().subspan(0, mps), n, tg);
        if (r == usb_sim::reply::nak) {
          bits |= 1u << 4;
          break;
        }
        if (r == usb_sim::reply::stall) {
          bits |= 1u << 3;
          break;
        }
        if (tg != data1) {
          dev.note_toggle_error();
          bits |= (1u << 10) | (1u << 5);
          break;
        }
        if (n > xfer) {
          bits |= 1u << 8;
          break;
        }
        if (n != 0) {
          reloco::span<u8> w = mem(dma, n);
          for (std::size_t i = 0; i < n; ++i)
            w[i] = pk[i];
        }
        xfer -= static_cast<u32>(n);
        --pkts;
        dma += static_cast<u32>(n);
        pid = pid == 0 ? 2u : 0u;
        if (n < mps || pkts == 0) {
          bits |= 1u | (1u << 5);
          break;
        }
      }
    }
    finish(c, bits, xfer, pkts, pid, dma);
  }
};

u32 test_env::read32(std::size_t offset) noexcept { return hc->read32(offset); }
void test_env::write32(std::size_t offset, u32 value) noexcept { hc->write32(offset, value); }
reloco::task<void> test_env::delay_ms(unsigned ms) noexcept {
  delay_total_ms += ms;
  hc->time_passed(ms);
  co_return;
}

// ---------------------------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------------------------

// A transfer the test submits by hand (to keep it pending, cancel it, ...).
struct xfer : usb_transfer {
  bool finished = false;
  usb_completion res{};
  unsigned completions = 0;

  xfer() noexcept {
    done_hook = &on_done;
    done_ctx = this;
  }
  static void on_done(void *, usb_transfer &t) noexcept {
    auto &x = static_cast<xfer &>(t);
    x.finished = true;
    x.res = x.result_;
    ++x.completions;
  }
};

struct job {
  usb_completion c{};
  bool done = false;
};

reloco::task<void> bulk_out_job(usb_host_controller_ref r, usb_pipe p, reloco::span<const u8> d, job &j) noexcept {
  j.c = co_await r.out(p, d);
  j.done = true;
}

reloco::task<void> bulk_in_job(usb_host_controller_ref r, usb_pipe p, reloco::span<u8> d, job &j) noexcept {
  j.c = co_await r.in(p, d);
  j.done = true;
}

reloco::task<void> control_job(usb_host_controller_ref r, usb_pipe p, usb_setup_packet s, reloco::span<u8> d,
                               job &j) noexcept {
  j.c = co_await r.control(p, s, d);
  j.done = true;
}

void fill_pattern(reloco::span<u8> b, unsigned seed) noexcept {
  for (std::size_t i = 0; i < b.size(); ++i)
    b[i] = static_cast<u8>((i * 7 + seed) & 0xFFu);
}

bool same(reloco::span<const u8> a, reloco::span<const u8> b) noexcept {
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (a[i] != b[i])
      return false;
  return true;
}

unsigned packets_for(std::size_t n, std::size_t mps) noexcept {
  return n == 0 ? 1u : static_cast<unsigned>((n + mps - 1) / mps);
}

class Dwc2Test : public ::testing::Test {
protected:
  using hcd_t = dwc2_hcd<test_env, 4>;

  test_env env;
  sim_dwc2 hc{env};
  hcd_t hcd{env};
  usb_host_controller_ref ref{hcd};
  usb::usb_host host{ref};
  reloco::array<u8, 512> cfg{};
  usb::usb_device udev{ref, reloco::span<u8>(cfg)};
  reloco::array<u8, 8192> tx{};
  reloco::array<u8, 8192> rx{};
  reloco::span<u8> txv{tx.as_span()};
  reloco::span<u8> rxv{rx.as_span()};

  Dwc2Test() { env.hc = &hc; }

  void TearDown() override {
    EXPECT_EQ(env.violations, 0u);
    EXPECT_EQ(hc.bad_programming, 0u);
    EXPECT_EQ(hc.bad_access, 0u);
    EXPECT_EQ(hc.sw_disables, 0u);
    EXPECT_EQ(hc.short_resets, 0u);
  }

  // Runs `n` frames: the controller works through its periodic schedule, then the driver's interrupt handler runs.
  void pump(unsigned n = 1) {
    for (unsigned i = 0; i < n; ++i) {
      hc.step();
      hcd.irq();
    }
  }

  bool wait(const bool &flag, unsigned max_frames = 3000) {
    for (unsigned i = 0; i < max_frames && !flag; ++i)
      pump();
    return flag;
  }

  template <typename T> reloco::result<T> run(reloco::task<T> t, unsigned max_frames = 3000) {
    t.resume();
    for (unsigned i = 0; i < max_frames && !t.done(); ++i)
      pump();
    EXPECT_TRUE(t.done());
    if (!t.done())
      return reloco::unexpected(reloco::error::timed_out);
    return t.take();
  }

  void start_and_plug(dspeed s = dspeed::fs) {
    ASSERT_TRUE(hcd.start().has_value());
    hc.plug(s);
    pump(2);
  }

  void enumerate(dspeed s = dspeed::fs, unsigned bulk_mps = 0) {
    ASSERT_TRUE(hcd.start().has_value());
    hc.plug(s);
    if (bulk_mps != 0)
      hc.dev.bulk_mps = bulk_mps;
    pump(2);
    ASSERT_TRUE(run(host.enumerate(0, udev)).has_value());
  }

  [[nodiscard]] unsigned bulk_mps() const { return hc.dev.bulk_mps; }
  [[nodiscard]] usb_pipe bulk_pipe(unsigned ep, usb_direction d) const {
    return usb_pipe{udev.address(), static_cast<u8>(ep), d, usb_transfer_type::bulk,
                    static_cast<std::uint16_t>(bulk_mps()), udev.speed(), 0};
  }
  [[nodiscard]] usb_pipe int_pipe(unsigned interval = 1) const {
    return usb_pipe{udev.address(), 2, usb_direction::in, usb_transfer_type::interrupt, 8, udev.speed(),
                    static_cast<u8>(interval)};
  }
  [[nodiscard]] usb_pipe ctl_pipe(u8 mps = 64) const {
    return usb_pipe{udev.address(), 0, usb_direction::out, usb_transfer_type::control, mps, udev.speed(), 0};
  }

  usb_completion do_out(const usb_pipe &p, reloco::span<const u8> d) {
    job j;
    auto t = bulk_out_job(ref, p, d, j);
    t.resume();
    EXPECT_TRUE(wait(j.done));
    return j.c;
  }
  usb_completion do_in(const usb_pipe &p, reloco::span<u8> d) {
    job j;
    auto t = bulk_in_job(ref, p, d, j);
    t.resume();
    EXPECT_TRUE(wait(j.done));
    return j.c;
  }
  usb_completion do_ctl(const usb_pipe &p, const usb_setup_packet &s, reloco::span<u8> d = {}) {
    job j;
    auto t = control_job(ref, p, s, d, j);
    t.resume();
    EXPECT_TRUE(wait(j.done));
    return j.c;
  }

  void loopback_sizes() {
    const usb_pipe po = bulk_pipe(1, usb_direction::out);
    const usb_pipe pi = bulk_pipe(1, usb_direction::in);
    const reloco::array<std::size_t, 7> sizes{0, 1, 63, 64, 65, 1000, 5000};
    unsigned out_pk = 0, in_pk = 0;
    for (const std::size_t n : sizes) {
      fill_pattern(tx.as_span().subspan(0, n), static_cast<unsigned>(n));
      usb_completion c = do_out(po, tx.as_span().subspan(0, n));
      ASSERT_TRUE(c.ok()) << n;
      EXPECT_EQ(c.actual, n);
      out_pk += packets_for(n, bulk_mps());
      if (n == 0)
        continue; // the device NAKs IN while its FIFO is empty
      c = do_in(pi, rx.as_span().subspan(0, n));
      ASSERT_TRUE(c.ok()) << n;
      EXPECT_EQ(c.actual, n);
      EXPECT_TRUE(same(tx.as_span().subspan(0, n), rx.as_span().subspan(0, n))) << n;
      in_pk += packets_for(n, bulk_mps());
    }
    EXPECT_EQ(hc.dev.toggle_errors, 0u);
    EXPECT_EQ(hc.dev.bulk_out_packets, out_pk);
    EXPECT_EQ(hc.dev.bulk_in_packets, in_pk);
    pump(8);
    EXPECT_EQ(hcd.resources_in_use(), 0u);
    EXPECT_EQ(env.live_blocks(), 0u);
  }
};

// ---------------------------------------------------------------------------------------------
// start() / stop()
// ---------------------------------------------------------------------------------------------

TEST_F(Dwc2Test, StartProgramsController) {
  ASSERT_TRUE(hcd.start().has_value());
  EXPECT_EQ(hcd.port_count(), 1u);
  EXPECT_EQ(hcd.channel_count(), 4u); // min(8 in the core, 4 in MaxTransfers)
  EXPECT_EQ(hc.soft_resets, 1u);
  EXPECT_TRUE(hc.host_mode);
  EXPECT_NE(hc.gusbcfg & (1u << 29), 0u);        // ForceHstMode
  EXPECT_EQ(hc.gusbcfg & (1u << 30), 0u);        // not ForceDevMode
  EXPECT_EQ(hc.gusbcfg & 0x358u, 0u);            // PHY if/select, SRP and HNP bits off for the 8-bit UTMI+ PHY
  EXPECT_EQ((hc.gusbcfg >> 10) & 0xFu, 9u);      // USBTrdTim
  EXPECT_EQ(hc.gahbcfg, (1u << 5) | 1u);         // DMAEn | GlblIntrMsk, single bursts
  EXPECT_EQ(hc.grxfsiz, 512u);
  EXPECT_EQ(hc.gnptxfsiz, (256u << 16) | 512u);
  EXPECT_EQ(hc.hptxfsiz, (256u << 16) | 768u);
  EXPECT_EQ(hc.last_tx_flush, 0x10u); // all Tx FIFOs
  EXPECT_EQ(hc.rx_flushes, 1u);
  EXPECT_EQ(hc.hcfg, 0u);
  EXPECT_EQ(hc.haintmsk, 0xFu);
  EXPECT_EQ(hc.gintmsk, (1u << 24) | (1u << 25) | (1u << 29)); // PrtInt | HChInt | Disconnect
  EXPECT_TRUE(hc.pwr);
  for (unsigned c = 0; c < hc.nchan; ++c)
    EXPECT_EQ(hc.ch[c].hcchar >> 31, 0u);
}

TEST_F(Dwc2Test, StartFailsBoundedWhenResetNeverCompletes) {
  hc.reset_never_completes = true;
  auto r = hcd.start();
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::timed_out);
  EXPECT_EQ(hcd.port_count(), 0u);
}

TEST_F(Dwc2Test, StartFailsBoundedWhenHostModeIsNeverEntered) {
  hc.host_mode_never = true;
  auto r = hcd.start();
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::timed_out);
  EXPECT_EQ(hcd.port_count(), 0u);
}

TEST_F(Dwc2Test, StartRejectsUnknownCoreAndMissingInternalDma) {
  hc.snpsid = 0x12345678u;
  auto r = hcd.start();
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::unsupported_operation);
  hc.snpsid = 0x4F54330Au;
  hc.arch = 0; // slave-only core
  r = hcd.start();
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::unsupported_operation);
  hc.arch = 1; // external DMA
  EXPECT_EQ(hcd.start().error(), reloco::error::unsupported_operation);
  hc.arch = 2;
  hc.snpsid = 0x4F54294Au; // another revision of the same family
  EXPECT_TRUE(hcd.start().has_value());
}

TEST_F(Dwc2Test, StartRejectsInvalidConfiguration) {
  hc.dfifo_depth = 512;
  EXPECT_EQ(hcd.start().error(), reloco::error::invalid_argument); // 512 + 256 + 256 words do not fit
  hc.dfifo_depth = 1024;
  dwc2_config c;
  c.p_tx_fifo_words = 0;
  hcd_t zero{env, c};
  EXPECT_EQ(zero.start().error(), reloco::error::invalid_argument);
  c = dwc2_config{};
  c.ahb_burst = 16;
  hcd_t burst{env, c};
  EXPECT_EQ(burst.start().error(), reloco::error::invalid_argument);
  EXPECT_EQ(hc.soft_resets, 0u); // validated before the core was touched
}

TEST_F(Dwc2Test, PhyAndFifoConfigurationReachTheCore) {
  dwc2_config c;
  c.phy = dwc2_phy::utmi_16bit;
  c.ahb_burst = 5;
  c.rx_fifo_words = 300;
  c.np_tx_fifo_words = 100;
  c.p_tx_fifo_words = 200;
  {
    hcd_t h{env, c};
    ASSERT_TRUE(h.start().has_value());
    EXPECT_EQ(hc.gusbcfg & 0x358u, 1u << 3); // PHYIf16
    EXPECT_EQ((hc.gusbcfg >> 10) & 0xFu, 5u);
    EXPECT_EQ(hc.gahbcfg, (5u << 1) | (1u << 5) | 1u);
    EXPECT_EQ(hc.grxfsiz, 300u);
    EXPECT_EQ(hc.gnptxfsiz, (100u << 16) | 300u);
    EXPECT_EQ(hc.hptxfsiz, (200u << 16) | 400u);
  }
  c = dwc2_config{};
  c.phy = dwc2_phy::ulpi;
  {
    hcd_t h{env, c};
    ASSERT_TRUE(h.start().has_value());
    EXPECT_EQ(hc.gusbcfg & 0x358u, 1u << 4); // ULPI_UTMI_Sel
    EXPECT_EQ((hc.gusbcfg >> 10) & 0xFu, 9u);
  }
  c.phy = dwc2_phy::fs_serial;
  {
    hcd_t h{env, c};
    ASSERT_TRUE(h.start().has_value());
    EXPECT_EQ(hc.gusbcfg & 0x358u, 1u << 6); // PHYSel
    EXPECT_EQ(hc.hcfg, 1u | 4u);             // 48 MHz clock, FS/LS only
  }
}

TEST_F(Dwc2Test, ChannelCountFollowsTheCore) {
  hc.nchan = 5;
  dwc2_hcd<test_env, 16> wide{env};
  ASSERT_TRUE(wide.start().has_value());
  EXPECT_EQ(wide.channel_count(), 5u);
  EXPECT_EQ(hc.haintmsk, 0x1Fu);
  wide.stop();
  hc.nchan = 16;
  ASSERT_TRUE(wide.start().has_value());
  EXPECT_EQ(wide.channel_count(), 16u);
  EXPECT_EQ(hc.haintmsk, 0xFFFFu);
}

TEST_F(Dwc2Test, StartHaltsChannelsTheCoreLeftEnabled) {
  hc.stuck_channel_after_reset = true;
  ASSERT_TRUE(hcd.start().has_value());
  EXPECT_EQ(hc.chdis_writes, 1u);
  EXPECT_EQ(hc.ch[1].hcchar >> 31, 0u);
  EXPECT_EQ(hc.ch[1].hcint & 1u, 0u); // flags cleared
}

TEST_F(Dwc2Test, SubmitBeforeStartFails) {
  xfer t;
  t.pipe = usb_pipe{1, 1, usb_direction::in, usb_transfer_type::bulk, 64, usb_speed::full, 0};
  t.data = rxv.data();
  t.length = 8;
  auto r = ref.submit(t);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::invalid_state);
  EXPECT_EQ(hcd.port_count(), 0u);
}

TEST_F(Dwc2Test, StopMasksInterruptsAndResetsTheCore) {
  ASSERT_TRUE(hcd.start().has_value());
  hcd.stop();
  EXPECT_EQ(hc.gintmsk, 0u);
  EXPECT_EQ(hc.gahbcfg & 1u, 0u);
  EXPECT_EQ(hc.soft_resets, 2u);
  EXPECT_EQ(hcd.port_count(), 0u);
  hcd.stop(); // idempotent
  EXPECT_EQ(hc.soft_resets, 2u);
  EXPECT_TRUE(hcd.start().has_value()); // restartable
  EXPECT_TRUE(hc.host_mode);
}

// ---------------------------------------------------------------------------------------------
// Root port
// ---------------------------------------------------------------------------------------------

TEST_F(Dwc2Test, PortStatusReportsConnectionAndClearsChange) {
  ASSERT_TRUE(hcd.start().has_value());
  auto s = hcd.port_status(0);
  ASSERT_TRUE(s.has_value());
  EXPECT_FALSE(s->connected);
  EXPECT_FALSE(s->changed);

  hc.plug(dspeed::fs);
  s = hcd.port_status(0);
  ASSERT_TRUE(s.has_value());
  EXPECT_TRUE(s->connected);
  EXPECT_TRUE(s->changed);
  EXPECT_FALSE(s->enabled);
  s = hcd.port_status(0);
  EXPECT_FALSE(s->changed);
  EXPECT_FALSE(hc.conn_det); // acknowledged in the register

  hc.unplug();
  hc.plug(dspeed::ls);
  s = hcd.port_status(0);
  EXPECT_TRUE(s->changed);

  hc.unplug();
  s = hcd.port_status(0);
  EXPECT_TRUE(s->changed);
  EXPECT_FALSE(s->connected);
  EXPECT_FALSE(hcd.port_status(1).has_value());
  EXPECT_EQ(hc.sw_disables, 0u);
}

TEST_F(Dwc2Test, DeviceAttachedBeforeStartIsReportedAfterPowerUp) {
  hc.plug(dspeed::fs);
  EXPECT_FALSE(hc.connected()); // no VBUS yet
  ASSERT_TRUE(hcd.start().has_value());
  auto s = hcd.port_status(0);
  ASSERT_TRUE(s.has_value());
  EXPECT_TRUE(s->connected);
  EXPECT_TRUE(s->changed);
}

TEST_F(Dwc2Test, PortInterruptLatchesAndNotifiesHook) {
  ASSERT_TRUE(hcd.start().has_value());
  unsigned calls = 0;
  hcd.set_port_event_hook([](void *c) noexcept { ++*static_cast<unsigned *>(c); }, &calls);
  hc.plug(dspeed::fs);
  hcd.irq();
  EXPECT_EQ(calls, 1u);
  EXPECT_FALSE(hc.conn_det); // acknowledged by the interrupt handler
  EXPECT_EQ(hc.gintsts_value() & (1u << 24), 0u);
  auto s = hcd.port_status(0);
  EXPECT_TRUE(s->changed); // but still reported to the stack
  {
    auto s2 = hcd.port_status(0);
    EXPECT_FALSE(s2->changed);
  }
  hc.unplug();
  hcd.irq();
  EXPECT_EQ(calls, 2u);
  EXPECT_EQ(hc.latched & (1u << 29), 0u); // Disconnect interrupt acknowledged
}

TEST_F(Dwc2Test, PortRegisterWritesNeverClearPendingStatusBits) {
  start_and_plug();
  hc.raise_overcurrent_change();
  hc.conn_det = true;
  ASSERT_TRUE(run(ref.reset_port(0)).has_value());
  EXPECT_TRUE(hc.conn_det); // read-modify-writes of HPRT must not acknowledge them
  EXPECT_TRUE(hc.ovr_chng);
  EXPECT_TRUE(hc.ena);
  EXPECT_EQ(hc.sw_disables, 0u); // PrtEna was never written as 1
  EXPECT_FALSE(hc.en_chng);      // the enable change that the reset caused was acknowledged
  {
    auto s = hcd.port_status(0);
    EXPECT_TRUE(s->changed);
  }
  EXPECT_FALSE(hc.ovr_chng);
}

TEST_F(Dwc2Test, ResetPortWaitsAndEnablesPort) {
  start_and_plug();
  ASSERT_TRUE(run(ref.reset_port(0)).has_value());
  EXPECT_EQ(hc.port_resets, 1u);
  EXPECT_GE(hc.last_reset_ms, 50u);
  EXPECT_GE(env.delay_total_ms, 20u + 60u); // power-good wait + reset
  auto s = hcd.port_status(0);
  ASSERT_TRUE(s.has_value());
  EXPECT_TRUE(s->connected);
  EXPECT_TRUE(s->enabled);
  EXPECT_EQ(s->speed, usb_speed::full);
  EXPECT_TRUE(hc.pwr);

  const unsigned before = env.delay_total_ms;
  ASSERT_TRUE(run(ref.reset_port(0)).has_value());
  EXPECT_LT(env.delay_total_ms - before, 100u); // the power-good wait happens only once
  EXPECT_EQ(hc.port_resets, 2u);
}

TEST_F(Dwc2Test, ResetPortProgramsClocksPerSpeed) {
  ASSERT_TRUE(hcd.start().has_value());
  hc.plug(dspeed::hs);
  ASSERT_TRUE(run(ref.reset_port(0)).has_value());
  EXPECT_EQ(hc.hcfg & 3u, 0u);
  EXPECT_EQ(hc.hfir, 7499u); // 125 us at 60 MHz
  EXPECT_EQ(hcd.port_status(0).value().speed, usb_speed::high);

  hc.unplug();
  hc.plug(dspeed::fs);
  ASSERT_TRUE(run(ref.reset_port(0)).has_value());
  EXPECT_EQ(hc.hcfg & 3u, 0u);
  EXPECT_EQ(hc.hfir, 59999u); // 1 ms at 60 MHz
  EXPECT_EQ(hcd.port_status(0).value().speed, usb_speed::full);

  hc.unplug();
  hc.plug(dspeed::ls);
  ASSERT_TRUE(run(ref.reset_port(0)).has_value());
  EXPECT_EQ(hc.hcfg & 3u, 2u); // 6 MHz
  EXPECT_EQ(hc.hfir, 5999u);
  EXPECT_EQ(hcd.port_status(0).value().speed, usb_speed::low);
}

TEST_F(Dwc2Test, ResetPortClocksFollowThePhy) {
  dwc2_config c;
  c.phy = dwc2_phy::utmi_16bit;
  hcd_t h16{env, c};
  usb_host_controller_ref r16{h16};
  ASSERT_TRUE(h16.start().has_value());
  hc.plug(dspeed::hs);
  {
    auto t = r16.reset_port(0);
    t.resume();
    for (unsigned i = 0; i < 20 && !t.done(); ++i)
      h16.irq();
    ASSERT_TRUE(t.done());
    EXPECT_TRUE(t.take().has_value());
  }
  EXPECT_EQ(hc.hfir, 3749u); // 125 us at 30 MHz
  h16.stop();

  c.phy = dwc2_phy::fs_serial;
  hcd_t hfs{env, c};
  usb_host_controller_ref rfs{hfs};
  ASSERT_TRUE(hfs.start().has_value());
  hc.plug(dspeed::fs);
  {
    auto t = rfs.reset_port(0);
    t.resume();
    ASSERT_TRUE(t.done());
    EXPECT_TRUE(t.take().has_value());
  }
  EXPECT_EQ(hc.hcfg & 3u, 1u);
  EXPECT_EQ(hc.hfir, 47999u); // 1 ms at 48 MHz
  hc.unplug();
  hc.plug(dspeed::ls);
  {
    auto t = rfs.reset_port(0);
    t.resume();
    ASSERT_TRUE(t.done());
    EXPECT_TRUE(t.take().has_value());
  }
  EXPECT_EQ(hc.hcfg & 3u, 2u);
  EXPECT_EQ(hc.hfir, 5999u);
}

TEST_F(Dwc2Test, ResetPortFailsWhenThePortStaysDisabled) {
  hc.reset_leaves_port_disabled = true;
  start_and_plug();
  auto r = run(ref.reset_port(0));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::io_error);
}

TEST_F(Dwc2Test, ResetPortFailsWithoutDeviceOrForBadPort) {
  ASSERT_TRUE(hcd.start().has_value());
  auto r = run(ref.reset_port(0));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::not_found);
  hc.plug(dspeed::fs);
  EXPECT_FALSE(run(ref.reset_port(1)).has_value());
  EXPECT_FALSE(run(ref.reset_port(7)).has_value());
  EXPECT_EQ(hc.port_resets, 0u);
}

// ---------------------------------------------------------------------------------------------
// Enumeration and control transfers
// ---------------------------------------------------------------------------------------------

TEST_F(Dwc2Test, EnumeratesFullSpeedDevice) {
  enumerate();
  EXPECT_EQ(udev.address(), 1);
  EXPECT_EQ(hc.dev.address, 1);
  EXPECT_EQ(hc.dev.config, 1);
  EXPECT_TRUE(udev.configured());
  EXPECT_EQ(udev.speed(), usb_speed::full);
  EXPECT_EQ(udev.descriptor().vendor_id, 0x1234);
  EXPECT_EQ(udev.descriptor().product_id, 0x5678);
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
  pump(8);
  EXPECT_EQ(hcd.resources_in_use(), 0u);
  EXPECT_EQ(env.live_blocks(), 0u);
}

TEST_F(Dwc2Test, EnumeratesLowSpeedDevice) {
  enumerate(dspeed::ls);
  EXPECT_EQ(udev.speed(), usb_speed::low);
  EXPECT_EQ(hc.dev.address, 1);
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
}

TEST_F(Dwc2Test, EnumeratesHighSpeedDevice) {
  enumerate(dspeed::hs);
  EXPECT_EQ(udev.speed(), usb_speed::high);
  EXPECT_EQ(hc.dev.address, 1);
  EXPECT_TRUE(udev.configured());
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
}

TEST_F(Dwc2Test, ControlInStopsOnShortPacket) {
  enumerate();
  const usb_setup_packet s{0x80, 6, 0x0301, 0, 64}; // GET_DESCRIPTOR(string 1): 10 bytes
  const usb_completion c = do_ctl(ctl_pipe(), s, rx.as_span().subspan(0, 64));
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(c.actual, 10u);
  EXPECT_EQ(rx[0], 10);
  EXPECT_EQ(rx[1], 3);
  EXPECT_EQ(rx[2], 'H');
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
}

TEST_F(Dwc2Test, ControlInWithSmallPacketsAndExactMultiple) {
  enumerate();
  const usb_pipe p = ctl_pipe(8);
  // 16 requested of an 18-byte descriptor: two full packets, no short one.
  usb_completion c = do_ctl(p, usb_setup_packet{0x80, 6, 0x0100, 0, 16}, rx.as_span().subspan(0, 16));
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(c.actual, 16u);
  EXPECT_EQ(rx[7], 64);
  // 64 requested: 8 + 8 + 2 bytes, the short packet ends the data stage.
  c = do_ctl(p, usb_setup_packet{0x80, 6, 0x0100, 0, 64}, rx.as_span().subspan(0, 64));
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(c.actual, 18u);
  EXPECT_EQ(rx[8], 0x34);
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
}

TEST_F(Dwc2Test, ControlOutDataStage) {
  enumerate();
  const reloco::array<u8, 5> payload{9, 8, 7, 6, 5};
  reloco::array<u8, 5> buf = payload;
  const usb_completion c = do_ctl(ctl_pipe(), usb_setup_packet{0x21, 0x20, 0, 0, 5}, buf.as_span());
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(hc.dev.last_ctl_out_len, 5u);
  EXPECT_TRUE(same(hc.dev.last_ctl_out.as_span().subspan(0, 5), payload.as_span()));
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
}

TEST_F(Dwc2Test, ControlWithoutDataStage) {
  enumerate();
  const unsigned before = hc.dev.set_config_count;
  const usb_completion c = do_ctl(ctl_pipe(), usb_setup_packet{0x00, 9, 1, 0, 0});
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(hc.dev.set_config_count, before + 1);
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
}

TEST_F(Dwc2Test, ControlStallsAreReportedAndTheEndpointKeepsWorking) {
  enumerate();
  usb_completion c = do_ctl(ctl_pipe(), usb_setup_packet{0xC0, 0x99, 0, 0, 8}, rx.as_span().subspan(0, 8));
  EXPECT_EQ(c.status, usb_status::stall);
  c = do_ctl(ctl_pipe(), usb_setup_packet{0x40, 0x99, 0, 0, 0});
  EXPECT_EQ(c.status, usb_status::stall);
  c = do_ctl(ctl_pipe(), usb_setup_packet{0x80, 6, 0x0301, 0, 64}, rx.as_span().subspan(0, 64));
  EXPECT_TRUE(c.ok());
  pump(8);
  EXPECT_EQ(hcd.resources_in_use(), 0u);
  EXPECT_EQ(env.live_blocks(), 0u);
}

TEST_F(Dwc2Test, ControlTransfersAtHighSpeed) {
  enumerate(dspeed::hs);
  const usb_completion c =
      do_ctl(ctl_pipe(), usb_setup_packet{0x80, 6, 0x0200, 0, 64}, rx.as_span().subspan(0, 64));
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(c.actual, 39u);
  EXPECT_EQ(rx[1], 2);
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
}

TEST_F(Dwc2Test, TransfersNeverSleep) {
  enumerate();
  const unsigned before = env.delay_total_ms;
  ASSERT_TRUE(do_ctl(ctl_pipe(), usb_setup_packet{0x80, 6, 0x0301, 0, 64}, rx.as_span().subspan(0, 64)).ok());
  ASSERT_TRUE(do_out(bulk_pipe(1, usb_direction::out), tx.as_span().subspan(0, 100)).ok());
  ASSERT_TRUE(do_in(bulk_pipe(1, usb_direction::in), rx.as_span().subspan(0, 100)).ok());
  EXPECT_EQ(env.delay_total_ms, before);
}

// ---------------------------------------------------------------------------------------------
// Bulk transfers
// ---------------------------------------------------------------------------------------------

TEST_F(Dwc2Test, BulkLoopbackSizesFullSpeed) {
  enumerate();
  loopback_sizes();
}

TEST_F(Dwc2Test, BulkLoopbackSizesHighSpeed) {
  enumerate(dspeed::hs);
  ASSERT_EQ(bulk_mps(), 512u);
  loopback_sizes();
}

TEST_F(Dwc2Test, BulkTransferLongerThanOneRunIsSplitIntoRuns) {
  enumerate(dspeed::fs, 8); // 8192 bytes = 1024 packets > the 1023 a single HCTSIZ can hold
  const usb_pipe po = bulk_pipe(1, usb_direction::out);
  const usb_pipe pi = bulk_pipe(1, usb_direction::in);
  fill_pattern(tx.as_span(), 9);
  usb_completion c = do_out(po, tx.as_span());
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(c.actual, 8192u);
  EXPECT_EQ(hc.max_pktcnt, 1023u);
  EXPECT_EQ(hc.dev.bulk_out_packets, 1024u);
  c = do_in(pi, rx.as_span());
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(c.actual, 8192u);
  EXPECT_TRUE(same(tx.as_span(), rx.as_span()));
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
}

TEST_F(Dwc2Test, BulkInStaysPendingWhileDeviceNaks) {
  enumerate();
  job in;
  auto ti = bulk_in_job(ref, bulk_pipe(1, usb_direction::in), rx.as_span().subspan(0, 64), in);
  ti.resume();
  pump(20);
  EXPECT_FALSE(in.done);
  EXPECT_EQ(hcd.resources_in_use(), 1u);
  const unsigned runs = hc.runs;
  bool sof_unmasked = false;
  for (unsigned i = 0; i < 10; ++i) {
    pump();
    sof_unmasked = sof_unmasked || (hc.gintmsk & (1u << 3)) != 0; // SOF is unmasked while a NAKed channel waits
  }
  EXPECT_TRUE(sof_unmasked);
  EXPECT_GE(hc.runs - runs, 4u);
  EXPECT_LE(hc.runs - runs, 10u); // at most one retry per frame, no busy loop

  fill_pattern(tx.as_span().subspan(0, 10), 3);
  ASSERT_TRUE(do_out(bulk_pipe(1, usb_direction::out), tx.as_span().subspan(0, 10)).ok());
  ASSERT_TRUE(wait(in.done));
  EXPECT_TRUE(in.c.ok());
  EXPECT_EQ(in.c.actual, 10u);
  EXPECT_TRUE(same(tx.as_span().subspan(0, 10), rx.as_span().subspan(0, 10)));
  pump(2);
  EXPECT_EQ(hc.gintmsk & (1u << 3), 0u);
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
}

TEST_F(Dwc2Test, BulkInShortPacketEndsMultiPacketTransfer) {
  enumerate();
  fill_pattern(tx.as_span().subspan(0, 100), 5);
  ASSERT_TRUE(do_out(bulk_pipe(1, usb_direction::out), tx.as_span().subspan(0, 100)).ok());
  const usb_completion c = do_in(bulk_pipe(1, usb_direction::in), rx.as_span().subspan(0, 5000));
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(c.actual, 100u);
  EXPECT_TRUE(same(tx.as_span().subspan(0, 100), rx.as_span().subspan(0, 100)));
  pump(8);
  EXPECT_EQ(hcd.resources_in_use(), 0u);
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
}

TEST_F(Dwc2Test, BulkInBufferSmallerThanPacketIsBabble) {
  enumerate();
  ASSERT_TRUE(do_out(bulk_pipe(1, usb_direction::out), tx.as_span().subspan(0, 64)).ok());
  const usb_completion c = do_in(bulk_pipe(1, usb_direction::in), rx.as_span().subspan(0, 10));
  EXPECT_EQ(c.status, usb_status::babble);
}

TEST_F(Dwc2Test, BabbleErrorFromTheCoreIsReported) {
  enumerate();
  hc.bblerr_runs = 1;
  EXPECT_EQ(do_in(bulk_pipe(1, usb_direction::in), rx.as_span().subspan(0, 64)).status, usb_status::babble);
}

TEST_F(Dwc2Test, StalledEndpointReportsStall) {
  enumerate();
  EXPECT_EQ(do_out(bulk_pipe(3, usb_direction::out), tx.as_span().subspan(0, 8)).status, usb_status::stall);
  EXPECT_EQ(do_in(bulk_pipe(3, usb_direction::in), rx.as_span().subspan(0, 8)).status, usb_status::stall);
  pump(4);
  EXPECT_EQ(hcd.resources_in_use(), 0u);
}

TEST_F(Dwc2Test, TransferToUnknownAddressTimesOut) {
  enumerate();
  usb_pipe p = bulk_pipe(1, usb_direction::out);
  p.address = 55;
  const unsigned before = hc.runs;
  EXPECT_EQ(do_out(p, tx.as_span().subspan(0, 8)).status, usb_status::timeout);
  EXPECT_EQ(hc.runs - before, 3u); // three tries
}

TEST_F(Dwc2Test, TransactionErrorsAreRetriedThreeTimes) {
  enumerate();
  const usb_pipe po = bulk_pipe(1, usb_direction::out);
  hc.xacterr_runs = 2;
  fill_pattern(tx.as_span().subspan(0, 20), 1);
  EXPECT_TRUE(do_out(po, tx.as_span().subspan(0, 20)).ok());
  hc.xacterr_runs = 3;
  EXPECT_EQ(do_out(po, tx.as_span().subspan(0, 20)).status, usb_status::timeout);
  EXPECT_EQ(hc.xacterr_runs, 0u);
  EXPECT_TRUE(do_out(po, tx.as_span().subspan(0, 20)).ok()); // the endpoint is usable afterwards
  EXPECT_EQ(hc.dev.bulk_out_packets, 2u);
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
}

TEST_F(Dwc2Test, ErrorsOfDifferentRunsDoNotAccumulate) {
  enumerate();
  const usb_pipe po = bulk_pipe(1, usb_direction::out);
  fill_pattern(tx.as_span().subspan(0, 300), 2);
  hc.xacterr_runs = 2;
  EXPECT_TRUE(do_out(po, tx.as_span().subspan(0, 300)).ok());
  hc.xacterr_runs = 2;
  EXPECT_TRUE(do_out(po, tx.as_span().subspan(0, 300)).ok());
}

TEST_F(Dwc2Test, NakInTheMiddleOfARunResumesWhereItStopped) {
  enumerate();
  const usb_pipe po = bulk_pipe(1, usb_direction::out);
  const usb_pipe pi = bulk_pipe(1, usb_direction::in);
  fill_pattern(tx.as_span().subspan(0, 300), 4);
  hc.nak_at_packet = 3; // 300 bytes = 5 packets of 64 / 44: NAK on the third
  ASSERT_TRUE(do_out(po, tx.as_span().subspan(0, 300)).ok());
  EXPECT_EQ(hc.dev.bulk_out_packets, 5u);
  hc.nak_at_packet = 2;
  usb_completion c = do_in(pi, rx.as_span().subspan(0, 300));
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(c.actual, 300u);
  EXPECT_TRUE(same(tx.as_span().subspan(0, 300), rx.as_span().subspan(0, 300)));
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
}

TEST_F(Dwc2Test, HighSpeedBulkOutUsesPingAfterNakAndNyet) {
  enumerate(dspeed::hs);
  const usb_pipe po = bulk_pipe(1, usb_direction::out);
  const usb_pipe pi = bulk_pipe(1, usb_direction::in);
  fill_pattern(tx.as_span().subspan(0, 1500), 6);

  hc.out_naks = 1; // the endpoint is busy: NAK, then the host must PING until it is ready
  hc.ping_naks = 2;
  ASSERT_TRUE(do_out(po, tx.as_span().subspan(0, 1500)).ok());
  EXPECT_EQ(hc.pings, 3u);

  hc.nyet_next = 1; // data accepted but the endpoint is full: NYET, PING before the next packet
  hc.ping_naks = 1;
  ASSERT_TRUE(do_out(po, tx.as_span().subspan(0, 1500)).ok());
  EXPECT_EQ(hc.pings, 5u);

  const usb_completion c = do_in(pi, rx.as_span().subspan(0, 3000));
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(c.actual, 3000u);
  EXPECT_TRUE(same(tx.as_span().subspan(0, 1500), rx.as_span().subspan(0, 1500)));
  EXPECT_TRUE(same(tx.as_span().subspan(0, 1500), rx.as_span().subspan(1500, 1500)));
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
}

TEST_F(Dwc2Test, NyetOnTheLastPacketCompletesTheTransfer) {
  enumerate(dspeed::hs);
  const usb_pipe po = bulk_pipe(1, usb_direction::out);
  hc.nyet_next = 1;
  ASSERT_TRUE(do_out(po, tx.as_span().subspan(0, 100)).ok());
  EXPECT_EQ(hc.pings, 0u); // nothing left to send, so no PING
}

// ---------------------------------------------------------------------------------------------
// Data toggles
// ---------------------------------------------------------------------------------------------

TEST_F(Dwc2Test, ResetDataToggleRestartsAtDataZero) {
  enumerate();
  const usb_pipe po = bulk_pipe(1, usb_direction::out);
  ASSERT_TRUE(do_out(po, tx.as_span().subspan(0, 10)).ok());
  hc.dev.reset_toggles();
  ref.reset_data_toggle(po);
  ASSERT_TRUE(do_out(po, tx.as_span().subspan(0, 10)).ok());
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
  EXPECT_EQ(hc.dev.bulk_out_packets, 2u);
}

TEST_F(Dwc2Test, ToggleContinuesAcrossTransfersWithoutReset) {
  enumerate();
  const usb_pipe po = bulk_pipe(1, usb_direction::out);
  for (int i = 0; i < 3; ++i)
    ASSERT_TRUE(do_out(po, tx.as_span().subspan(0, 10)).ok());
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
  hc.dev.reset_toggles(); // the device restarts at DATA0 but the host did not: the model must notice
  ASSERT_TRUE(do_out(po, tx.as_span().subspan(0, 10)).ok());
  EXPECT_EQ(hc.dev.toggle_errors, 1u);
}

TEST_F(Dwc2Test, ToggleFollowsPacketCountOfMultiPacketRuns) {
  enumerate();
  const usb_pipe po = bulk_pipe(1, usb_direction::out);
  const usb_pipe pi = bulk_pipe(1, usb_direction::in);
  ASSERT_TRUE(do_out(po, tx.as_span().subspan(0, 3 * 64)).ok()); // an odd number of packets flips the toggle
  ASSERT_TRUE(do_out(po, tx.as_span().subspan(0, 64)).ok());
  usb_completion c = do_in(pi, rx.as_span().subspan(0, 4 * 64));
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(c.actual, 4u * 64);
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
}

TEST_F(Dwc2Test, ClearHaltRestartsTheEndpointToggle) {
  enumerate();
  const usb_pipe po = bulk_pipe(1, usb_direction::out);
  ASSERT_TRUE(do_out(po, tx.as_span().subspan(0, 10)).ok());
  ASSERT_TRUE(do_ctl(ctl_pipe(), usb_setup_packet{0x02, 1, 0, 0x01, 0}).ok()); // CLEAR_FEATURE(HALT) ep 1 OUT
  EXPECT_EQ(hc.dev.clear_halt_count, 1u);
  hc.dev.reset_toggles();
  ASSERT_TRUE(do_out(po, tx.as_span().subspan(0, 10)).ok());
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
}

TEST_F(Dwc2Test, SetConfigurationRestartsToggles) {
  enumerate();
  const usb_pipe po = bulk_pipe(1, usb_direction::out);
  ASSERT_TRUE(do_out(po, tx.as_span().subspan(0, 10)).ok());
  ASSERT_TRUE(do_ctl(ctl_pipe(), usb_setup_packet{0x00, 9, 1, 0, 0}).ok());
  hc.dev.reset_toggles();
  ASSERT_TRUE(do_out(po, tx.as_span().subspan(0, 10)).ok());
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
}

// ---------------------------------------------------------------------------------------------
// Interrupt transfers
// ---------------------------------------------------------------------------------------------

TEST_F(Dwc2Test, InterruptInDeliversData) {
  enumerate();
  hc.dev.arm_interrupt();
  const usb_completion c = do_in(int_pipe(4), rx.as_span().subspan(0, 8));
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(c.actual, 8u);
  EXPECT_EQ(rx[0], 1);
  EXPECT_EQ(rx[7], 8);
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
  pump(8);
  EXPECT_EQ(hcd.resources_in_use(), 0u);
}

TEST_F(Dwc2Test, InterruptEndpointIsPolledEveryFrame) {
  enumerate();
  hc.ep2_polls = 0;
  xfer t;
  t.pipe = int_pipe(8); // bInterval is ignored: the endpoint is polled in every frame
  t.data = rxv.data();
  t.length = 8;
  ASSERT_TRUE(ref.submit(t).has_value());
  pump(40);
  ASSERT_GE(hc.ep2_polls, 30u);
  for (unsigned i = 1; i < 30; ++i)
    EXPECT_EQ(hc.ep2_frames[i] - hc.ep2_frames[i - 1], 1u);
  EXPECT_FALSE(t.finished);
  hc.dev.arm_interrupt();
  pump(3);
  EXPECT_TRUE(t.finished);
  EXPECT_TRUE(t.res.ok());
  EXPECT_EQ(rx[3], 4);
  EXPECT_EQ(hcd.resources_in_use(), 0u);
}

TEST_F(Dwc2Test, InterruptEndpointTogglesAlternate) {
  enumerate();
  for (int i = 0; i < 3; ++i) {
    hc.dev.arm_interrupt();
    ASSERT_TRUE(do_in(int_pipe(), rx.as_span().subspan(0, 8)).ok());
  }
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
}

TEST_F(Dwc2Test, InterruptEndpointAtHighSpeed) {
  enumerate(dspeed::hs);
  hc.dev.arm_interrupt();
  const usb_completion c = do_in(int_pipe(), rx.as_span().subspan(0, 8));
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(c.actual, 8u);
}

// ---------------------------------------------------------------------------------------------
// Cancel, unplug, capacity, errors
// ---------------------------------------------------------------------------------------------

TEST_F(Dwc2Test, CancelOfIdleChannelRecyclesAtOnce) {
  enumerate();
  pump(8);
  const std::size_t blocks = env.live_blocks();
  xfer t;
  t.pipe = bulk_pipe(1, usb_direction::in);
  t.data = rxv.data();
  t.length = 64;
  ASSERT_TRUE(ref.submit(t).has_value());
  pump(5);
  EXPECT_EQ(hcd.resources_in_use(), 1u);
  EXPECT_EQ(env.live_blocks(), blocks + 1); // the bounce buffer
  ref.cancel(t);
  EXPECT_EQ(hcd.resources_in_use(), 0u); // the channel was idle (NAK-waiting): nothing for the core to finish
  EXPECT_EQ(env.live_blocks(), blocks);
  pump(3);
  EXPECT_EQ(hc.gintmsk & (1u << 3), 0u); // SOF masked again
  EXPECT_EQ(t.completions, 0u);
}

TEST_F(Dwc2Test, CancelKeepsRunningChannelAsZombieUntilItHalts) {
  enumerate();
  pump(4);
  const std::size_t blocks = env.live_blocks();
  hc.halt_delay = 4; // the disable takes a few frames to be effective
  xfer t;
  t.pipe = int_pipe();
  t.data = rxv.data();
  t.length = 8;
  ASSERT_TRUE(ref.submit(t).has_value());
  pump(5);
  EXPECT_EQ(hcd.resources_in_use(), 1u);
  EXPECT_EQ(env.live_blocks(), blocks + 1);
  ref.cancel(t);
  EXPECT_EQ(hcd.resources_in_use(), 1u); // the core may still DMA: the channel and the memory stay allocated
  EXPECT_EQ(env.live_blocks(), blocks + 1);
  pump(2);
  EXPECT_EQ(hcd.resources_in_use(), 1u);
  pump(4);
  EXPECT_EQ(hcd.resources_in_use(), 0u);
  EXPECT_EQ(env.live_blocks(), blocks);
  EXPECT_EQ(t.completions, 0u);
  pump(6);
  EXPECT_EQ(t.completions, 0u);
}

TEST_F(Dwc2Test, DestroyingTheAwaitingCoroutineCancelsTheTransfer) {
  enumerate();
  pump(8);
  const std::size_t blocks = env.live_blocks();
  job in;
  {
    auto t = bulk_in_job(ref, bulk_pipe(1, usb_direction::in), rx.as_span().subspan(0, 64), in);
    t.resume();
    pump(4);
    EXPECT_EQ(hcd.resources_in_use(), 1u);
  }
  pump(6);
  EXPECT_FALSE(in.done);
  EXPECT_EQ(hcd.resources_in_use(), 0u);
  EXPECT_EQ(env.live_blocks(), blocks);
  ASSERT_TRUE(do_out(bulk_pipe(1, usb_direction::out), tx.as_span().subspan(0, 4)).ok());
  EXPECT_TRUE(do_in(bulk_pipe(1, usb_direction::in), rx.as_span().subspan(0, 4)).ok());
}

TEST_F(Dwc2Test, CancelThenResubmitSameEndpoint) {
  enumerate();
  xfer a;
  a.pipe = bulk_pipe(1, usb_direction::in);
  a.data = rxv.data();
  a.length = 64;
  ASSERT_TRUE(ref.submit(a).has_value());
  pump(2);
  ref.cancel(a);
  xfer b;
  b.pipe = a.pipe;
  b.data = rxv.data();
  b.length = 64;
  ASSERT_TRUE(ref.submit(b).has_value());
  ASSERT_TRUE(do_out(bulk_pipe(1, usb_direction::out), tx.as_span().subspan(0, 7)).ok());
  for (unsigned i = 0; i < 100 && !b.finished; ++i)
    pump();
  EXPECT_TRUE(b.finished);
  EXPECT_EQ(b.res.actual, 7u);
  EXPECT_EQ(a.completions, 0u);
}

TEST_F(Dwc2Test, EndpointOfAZombieChannelIsBusyUntilItHalts) {
  enumerate();
  hc.halt_delay = 3;
  xfer a;
  a.pipe = int_pipe();
  a.data = rxv.data();
  a.length = 8;
  ASSERT_TRUE(ref.submit(a).has_value());
  pump(3);
  ref.cancel(a);
  xfer b;
  b.pipe = a.pipe;
  b.data = rxv.data();
  b.length = 8;
  auto r = ref.submit(b);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::busy);
  pump(5);
  EXPECT_TRUE(ref.submit(b).has_value());
  ref.cancel(b);
  pump(6);
  EXPECT_EQ(hcd.resources_in_use(), 0u);
}

TEST_F(Dwc2Test, UnplugCompletesPendingTransfersAsDisconnected) {
  enumerate();
  job in;
  auto t = bulk_in_job(ref, bulk_pipe(1, usb_direction::in), rx.as_span().subspan(0, 64), in);
  t.resume();
  pump(5);
  EXPECT_FALSE(in.done);
  hc.unplug();
  ASSERT_TRUE(wait(in.done, 50));
  EXPECT_EQ(in.c.status, usb_status::disconnected);
  EXPECT_EQ(do_out(bulk_pipe(1, usb_direction::out), tx.as_span().subspan(0, 4)).status, usb_status::disconnected);
  pump(8);
  EXPECT_EQ(hcd.resources_in_use(), 0u);
  EXPECT_EQ(env.live_blocks(), 0u);
}

TEST_F(Dwc2Test, UnplugOfARunningChannelThatDoesNotHaltLeavesAZombie) {
  enumerate();
  pump(4);
  hc.halt_delay = 100000000; // never within the driver's bounded wait
  xfer t;
  t.pipe = int_pipe();
  t.data = rxv.data();
  t.length = 8;
  ASSERT_TRUE(ref.submit(t).has_value());
  pump(3);
  hc.halt_delay = 6;
  hc.unplug();
  hcd.irq();
  EXPECT_TRUE(t.finished); // the transfer ends at once...
  EXPECT_EQ(t.res.status, usb_status::disconnected);
  EXPECT_EQ(hcd.resources_in_use(), 1u); // ...but the channel and its memory stay allocated
  EXPECT_GE(env.live_blocks(), 1u);
  pump(10);
  EXPECT_EQ(hcd.resources_in_use(), 0u);
  EXPECT_EQ(env.live_blocks(), 0u);
}

TEST_F(Dwc2Test, SubmitBeyondCapacityFailsCleanly) {
  enumerate();
  reloco::array<xfer, 5> x{};
  x[0].pipe = ctl_pipe();
  x[0].setup = usb_setup_packet{0x80, 6, 0x0301, 0, 64};
  x[0].data = rxv.data();
  x[0].length = 64;
  x[1].pipe = bulk_pipe(1, usb_direction::in);
  x[1].data = rxv.data();
  x[1].length = 8;
  x[2].pipe = bulk_pipe(3, usb_direction::out);
  x[2].data = txv.data();
  x[2].length = 8;
  x[3].pipe = int_pipe(8);
  x[3].data = rxv.data();
  x[3].length = 8;
  for (std::size_t i = 0; i < 4; ++i)
    ASSERT_TRUE(ref.submit(x[i]).has_value()) << i;
  x[4].pipe = bulk_pipe(3, usb_direction::in);
  x[4].data = rxv.data();
  x[4].length = 8;
  auto r = ref.submit(x[4]);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::try_again);
  EXPECT_EQ(hcd.resources_in_use(), 4u);

  pump(60);
  EXPECT_TRUE(x[0].finished);
  EXPECT_TRUE(x[2].finished);
  EXPECT_FALSE(x[1].finished);
  EXPECT_FALSE(x[3].finished);
  ref.cancel(x[1]);
  ref.cancel(x[3]);
  pump(6);
  EXPECT_EQ(hcd.resources_in_use(), 0u);
  EXPECT_TRUE(ref.submit(x[4]).has_value());
  pump(20);
  EXPECT_TRUE(x[4].finished);
  EXPECT_EQ(x[4].res.status, usb_status::stall);
}

TEST_F(Dwc2Test, SubmitFailsWhenTheCoreHasNoFreeChannel) {
  hc.nchan = 2;
  dwc2_hcd<test_env, 16> h{env};
  usb_host_controller_ref r{h};
  ASSERT_TRUE(h.start().has_value());
  hc.plug(dspeed::fs);
  ASSERT_TRUE(h.port_status(0).has_value());
  reloco::array<xfer, 3> x{};
  x[0].pipe = usb_pipe{0, 1, usb_direction::in, usb_transfer_type::bulk, 64, usb_speed::full, 0};
  x[1].pipe = usb_pipe{0, 2, usb_direction::in, usb_transfer_type::bulk, 64, usb_speed::full, 0};
  x[2].pipe = usb_pipe{0, 3, usb_direction::in, usb_transfer_type::bulk, 64, usb_speed::full, 0};
  for (xfer &t : x) {
    t.data = rxv.data();
    t.length = 8;
  }
  EXPECT_TRUE(r.submit(x[0]).has_value());
  EXPECT_TRUE(r.submit(x[1]).has_value());
  auto res = r.submit(x[2]);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), reloco::error::try_again);
  r.cancel(x[0]);
  r.cancel(x[1]);
  EXPECT_EQ(h.resources_in_use(), 0u);
}

TEST_F(Dwc2Test, RejectsInvalidRequests) {
  enumerate();
  xfer a;
  a.pipe = usb_pipe{udev.address(), 1, usb_direction::in, usb_transfer_type::isochronous, 64, usb_speed::full, 1};
  a.data = rxv.data();
  a.length = 8;
  auto r = ref.submit(a);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::unsupported_operation);

  xfer big;
  big.pipe = bulk_pipe(1, usb_direction::out);
  big.data = txv.data();
  big.length = (1u << 18) + 1;
  r = ref.submit(big);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::out_of_range);

  xfer addr;
  addr.pipe = bulk_pipe(1, usb_direction::out);
  addr.pipe.address = 200;
  addr.data = txv.data();
  addr.length = 8;
  r = ref.submit(addr);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::invalid_argument);

  xfer mps;
  mps.pipe = bulk_pipe(1, usb_direction::out);
  mps.pipe.max_packet = 0;
  mps.data = txv.data();
  mps.length = 8;
  r = ref.submit(mps);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::invalid_argument);

  // A max packet size that is not a multiple of 4 only works for single-packet transfers (HCDMA alignment).
  xfer odd;
  odd.pipe = bulk_pipe(1, usb_direction::out);
  odd.pipe.max_packet = 6;
  odd.data = txv.data();
  odd.length = 12;
  r = ref.submit(odd);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::invalid_argument);

  xfer dup1, dup2;
  dup1.pipe = bulk_pipe(1, usb_direction::in);
  dup1.data = rxv.data();
  dup1.length = 8;
  dup2.pipe = dup1.pipe;
  dup2.data = rxv.data();
  dup2.length = 8;
  ASSERT_TRUE(ref.submit(dup1).has_value());
  r = ref.submit(dup2);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::busy);
  ref.cancel(dup1);
  pump(6);
  EXPECT_EQ(hcd.resources_in_use(), 0u);
}

TEST_F(Dwc2Test, SinglePacketTransferWithOddMaxPacketSizeWorks) {
  enumerate();
  xfer t;
  t.pipe = usb_pipe{udev.address(), 3, usb_direction::out, usb_transfer_type::bulk, 6, usb_speed::full, 0};
  t.data = txv.data();
  t.length = 5;
  ASSERT_TRUE(ref.submit(t).has_value());
  pump(10);
  EXPECT_TRUE(t.finished);
  EXPECT_EQ(t.res.status, usb_status::stall);
}

TEST_F(Dwc2Test, AllocationFailureFailsSubmitWithoutLeaks) {
  enumerate();
  env.fail_alloc = true;
  xfer t;
  t.pipe = bulk_pipe(1, usb_direction::out);
  t.data = txv.data();
  t.length = 8;
  auto r = ref.submit(t);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::allocation_failed);
  env.fail_alloc = false;
  EXPECT_EQ(hcd.resources_in_use(), 0u);
  EXPECT_TRUE(do_out(bulk_pipe(1, usb_direction::out), tx.as_span().subspan(0, 8)).ok());
}

TEST_F(Dwc2Test, AhbErrorFailsTheTransfer) {
  enumerate();
  hc.ahb_error_next = true;
  EXPECT_EQ(do_out(bulk_pipe(1, usb_direction::out), tx.as_span().subspan(0, 8)).status, usb_status::bus_error);
  EXPECT_TRUE(do_out(bulk_pipe(1, usb_direction::out), tx.as_span().subspan(0, 8)).ok());
}

TEST_F(Dwc2Test, LosingHostModeFailsTransfers) {
  enumerate();
  job in;
  auto t = bulk_in_job(ref, bulk_pipe(1, usb_direction::in), rx.as_span().subspan(0, 64), in);
  t.resume();
  pump(3);
  hc.drop_to_device_mode();
  hcd.irq();
  ASSERT_TRUE(in.done);
  EXPECT_EQ(in.c.status, usb_status::bus_error);
  xfer x;
  x.pipe = bulk_pipe(1, usb_direction::out);
  x.data = txv.data();
  x.length = 4;
  auto r = ref.submit(x);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::invalid_state);
}

TEST_F(Dwc2Test, StopCancelsPendingTransfersAndFreesMemory) {
  enumerate();
  job in;
  auto t = bulk_in_job(ref, bulk_pipe(1, usb_direction::in), rx.as_span().subspan(0, 64), in);
  t.resume();
  pump(3);
  hcd.stop();
  ASSERT_TRUE(in.done);
  EXPECT_EQ(in.c.status, usb_status::cancelled);
  EXPECT_EQ(env.live_blocks(), 0u);
  EXPECT_EQ(hcd.resources_in_use(), 0u);
}

TEST_F(Dwc2Test, StopFreesMemoryOfChannelsThatNeverHalt) {
  enumerate();
  pump(4);
  hc.halt_delay = 100000000;
  xfer t;
  t.pipe = int_pipe();
  t.data = rxv.data();
  t.length = 8;
  ASSERT_TRUE(ref.submit(t).has_value());
  pump(3);
  ref.cancel(t);
  EXPECT_EQ(hcd.resources_in_use(), 1u);
  hcd.stop(); // the soft reset stops the DMA, so the memory may go
  EXPECT_EQ(hcd.resources_in_use(), 0u);
  EXPECT_EQ(env.live_blocks(), 0u);
  EXPECT_EQ(t.completions, 0u);
}

// ---------------------------------------------------------------------------------------------
// bootldr::usb_stack hot plug on top of the driver
// ---------------------------------------------------------------------------------------------

class Dwc2StackTest : public Dwc2Test {
public:
  std::uint64_t now = 0;
  bootldr::scheduler sched;
  bootldr::usb_stack stack;
  unsigned attached = 0, detached = 0, failed = 0;
  unsigned started = 0, loopback_ok = 0, saw_disconnect = 0, cleaned = 0;

  static bootldr::usb_stack_config config() {
    bootldr::usb_stack_config c;
    c.poll_ms = 10;
    c.debounce_ms = 20;
    return c;
  }

  Dwc2StackTest() : stack(sched, ref, config()) {
    sched.set_clock(&clock, this);
    EXPECT_TRUE(sched.add_poller(&service, this).has_value());
    stack.set_event_handler(&on_event, this);
  }

  static std::uint64_t clock(void *c) noexcept { return static_cast<Dwc2StackTest *>(c)->now; }
  static void service(void *c) noexcept {
    auto *t = static_cast<Dwc2StackTest *>(c);
    t->now += 10;
    t->pump();
  }
  static void on_event(void *c, bootldr::usb_event_kind k, unsigned, bootldr::usb_attached_device *,
                       const reloco::result<void> &) noexcept {
    auto *t = static_cast<Dwc2StackTest *>(c);
    if (k == bootldr::usb_event_kind::attached)
      ++t->attached;
    else if (k == bootldr::usb_event_kind::detached)
      ++t->detached;
    else if (k == bootldr::usb_event_kind::enumeration_failed)
      ++t->failed;
  }

  void rounds(int n) {
    for (int i = 0; i < n; ++i)
      sched.run_once();
  }
};

bool match_any(void *, const usb::usb_device &) noexcept { return true; }

reloco::task<void> loopback_driver(void *ctx, bootldr::usb_stack &, bootldr::usb_device_ptr dev) noexcept {
  auto *t = static_cast<Dwc2StackTest *>(ctx);
  ++t->started;
  const hw::usb_host_controller_ref &c = dev->device().controller();
  const u8 addr = dev->device().address();
  const usb_pipe po{addr, 1, usb_direction::out, usb_transfer_type::bulk, 64, usb_speed::full, 0};
  const usb_pipe pi{addr, 1, usb_direction::in, usb_transfer_type::bulk, 64, usb_speed::full, 0};
  reloco::array<u8, 100> out{};
  reloco::array<u8, 100> in{};
  fill_pattern(out.as_span(), 11);
  usb_completion w = co_await c.out(po, out.as_span());
  usb_completion r = co_await c.in(pi, in.as_span());
  if (w.ok() && r.ok() && r.actual == 100 && same(out.as_span(), in.as_span()))
    ++t->loopback_ok;
  usb_completion parked = co_await c.in(pi, in.as_span()); // nothing to read: stays pending until unplug
  if (parked.status == usb_status::disconnected || parked.status == usb_status::cancelled)
    ++t->saw_disconnect;
  co_await dev->gone_event().wait();
  ++t->cleaned;
}

} // namespace

TEST_F(Dwc2StackTest, HotPlugRunsDriverCoroutineUntilUnplug) {
  ASSERT_TRUE(hcd.start().has_value());
  ASSERT_TRUE(stack.add_driver({&match_any, &loopback_driver, nullptr, this}).has_value());
  ASSERT_TRUE(stack.start().has_value());

  rounds(5);
  EXPECT_EQ(started, 0u);

  hc.plug(dspeed::fs);
  rounds(40);
  EXPECT_EQ(attached, 1u);
  EXPECT_EQ(started, 1u);
  EXPECT_EQ(loopback_ok, 1u);
  EXPECT_EQ(hcd.resources_in_use(), 1u); // the parked IN
  EXPECT_EQ(hc.dev.toggle_errors, 0u);

  hc.unplug();
  rounds(60);
  EXPECT_EQ(detached, 1u);
  EXPECT_EQ(saw_disconnect, 1u);
  EXPECT_EQ(cleaned, 1u);
  EXPECT_EQ(hcd.resources_in_use(), 0u);
  EXPECT_EQ(failed, 0u);

  // A second device works the same way.
  hc.plug(dspeed::fs);
  rounds(40);
  EXPECT_EQ(attached, 2u);
  EXPECT_EQ(started, 2u);
}

#endif // RELOCO_HAS_COROUTINES
