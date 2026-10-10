// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

// Register-level tests of structo::hw::ohci_hcd: a test Env (static DMA arena), an OHCI controller MODEL that
// walks the driver's HCCA/ED/TD structures exactly like the hardware (it is written from the OpenHCI 1.0a
// specification and shares no constants with the driver) and the packet-level simulated USB device from
// usb_hcd_sim.hpp behind it.

#include <gtest/gtest.h>

#include <structo/bootldr/usb_stack.hpp>
#include <structo/hw/ohci_hcd.hpp>
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

struct sim_ohci;

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
  sim_ohci *hc = nullptr;
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

  u8 get8(u32 phys) noexcept {
    reloco::span<u8> w = window(phys, 1);
    return w[0];
  }
  void put8(u32 phys, u8 v) noexcept {
    reloco::span<u8> w = window(phys, 1);
    w[0] = v;
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

// ---------------------------------------------------------------------------------------------
// The OHCI controller model.
// ---------------------------------------------------------------------------------------------

struct sim_ohci {
  static constexpr unsigned nports = 2;
  // OpenHCI 1.0a numbers, spelled out independently of the driver.
  static constexpr u32 ps_ccs = 1u << 0, ps_pes = 1u << 1, ps_prs = 1u << 4, ps_pps = 1u << 8, ps_lsda = 1u << 9;
  static constexpr u32 ps_csc = 1u << 16, ps_prsc = 1u << 20, ps_change_mask = 0x1F0000u;
  static constexpr u32 int_wdh = 1u << 1, int_sf = 1u << 2, int_ue = 1u << 4, int_rhsc = 1u << 6;

  test_env &env;
  usb_sim::device dev;
  unsigned plug_port = 0;

  // knobs
  u32 revision = 0x10;
  bool hcr_never_clears = false;
  bool smm_owned = false;
  bool smm_never_releases = false;
  bool reset_leaves_port_disabled = false;
  unsigned tds_per_pass = 64; // TDs one ED may retire per frame

  // observations
  unsigned bad_td = 0;
  unsigned ocr_writes = 0;
  unsigned hcr_writes = 0;
  unsigned sf_enable_writes = 0;
  unsigned port_resets = 0;
  unsigned power_cleared = 0;
  unsigned ep2_polls = 0;
  reloco::array<u32, 128> ep2_frames{};
  bool fit_toggled = false;

  // registers
  u32 control = 0, int_status = 0, int_enable = 0, hcca = 0, ctrl_head = 0, bulk_head = 0, fm_interval = 0x2EDF;
  u32 fm_number = 0, periodic_start = 0, ls_threshold = 0, rh_status = 0;
  bool hcr_pending = false, clf = false, blf = false;
  reloco::array<u32, nports> ports{};
  reloco::array<unsigned, nports> reset_ms{};
  u32 done_chain = 0; // internal done queue head (not yet written to the HCCA)

  explicit sim_ohci(test_env &e) noexcept : env(e) {}

  // ---- memory ----
  u32 rd(u32 phys) noexcept {
    reloco::span<u8> w = env.window(phys, 4);
    return static_cast<u32>(w[0]) | (static_cast<u32>(w[1]) << 8) | (static_cast<u32>(w[2]) << 16) |
           (static_cast<u32>(w[3]) << 24);
  }
  void wr(u32 phys, u32 v) noexcept {
    reloco::span<u8> w = env.window(phys, 4);
    w[0] = static_cast<u8>(v);
    w[1] = static_cast<u8>(v >> 8);
    w[2] = static_cast<u8>(v >> 16);
    w[3] = static_cast<u8>(v >> 24);
  }

  // ---- registers ----
  u32 read32(std::size_t off) noexcept {
    switch (off) {
    case 0x00:
      return revision;
    case 0x04:
      return control | (smm_owned ? 0x100u : 0u);
    case 0x08:
      return (hcr_pending ? 1u : 0u) | (clf ? 2u : 0u) | (blf ? 4u : 0u);
    case 0x0C:
      return int_status;
    case 0x10:
    case 0x14:
      return int_enable;
    case 0x18:
      return hcca;
    case 0x20:
      return ctrl_head;
    case 0x28:
      return bulk_head;
    case 0x34:
      return fm_interval;
    case 0x3C:
      return fm_number;
    case 0x40:
      return periodic_start;
    case 0x44:
      return ls_threshold;
    case 0x48:
      return nports | (1u << 8) | (1u << 24); // NDP, PSM, POTPGT = 2 ms
    case 0x50:
      return rh_status;
    default:
      break;
    }
    if (off >= 0x54 && off < 0x54 + 4 * nports)
      return ports[(off - 0x54) / 4];
    return 0;
  }

  void write32(std::size_t off, u32 v) noexcept {
    switch (off) {
    case 0x04:
      control = v & ~0x100u;
      break;
    case 0x08:
      if (v & 1u) {
        ++hcr_writes;
        if (hcr_never_clears)
          hcr_pending = true;
        else
          do_reset();
      }
      if (v & 2u)
        clf = true;
      if (v & 4u)
        blf = true;
      if ((v & 8u) != 0) {
        ++ocr_writes;
        if (!smm_never_releases)
          smm_owned = false;
      }
      break;
    case 0x0C:
      int_status &= ~v;
      break;
    case 0x10:
      int_enable |= v;
      if (v & int_sf)
        ++sf_enable_writes;
      break;
    case 0x14:
      int_enable &= ~v;
      break;
    case 0x18:
      hcca = v;
      break;
    case 0x20:
      ctrl_head = v;
      break;
    case 0x28:
      bulk_head = v;
      break;
    case 0x34:
      fit_toggled = ((v ^ fm_interval) >> 31) != 0;
      fm_interval = v;
      break;
    case 0x40:
      periodic_start = v;
      break;
    case 0x44:
      ls_threshold = v;
      break;
    case 0x50:
      if (v & (1u << 16)) // SetGlobalPower
        for (u32 &p : ports)
          p |= ps_pps;
      break;
    default:
      if (off >= 0x54 && off < 0x54 + 4 * nports)
        write_port((off - 0x54) / 4, v);
      break;
    }
  }

  void do_reset() noexcept {
    hcr_pending = false;
    control = 3u << 6; // USBSUSPEND
    int_status = 0;
    int_enable = 0;
    hcca = ctrl_head = bulk_head = 0;
    fm_interval = 0x2EDF;
    clf = blf = false;
    done_chain = 0;
  }

  void write_port(std::size_t p, u32 v) noexcept {
    u32 &s = ports[p];
    s &= ~(v & ps_change_mask);
    if (v & 1u)
      s &= ~ps_pes;
    if ((v & 2u) && (s & ps_ccs))
      s |= ps_pes;
    if ((v & 0x10u) && (s & ps_ccs)) {
      s |= ps_prs;
      reset_ms[p] = 0;
    }
    if (v & 0x100u)
      s |= ps_pps;
    if (v & 0x200u) {
      s &= ~ps_pps;
      ++power_cleared;
    }
  }

  // ---- the outside world ----
  void time_passed(unsigned ms) noexcept {
    for (std::size_t p = 0; p < nports; ++p) {
      if (!(ports[p] & ps_prs))
        continue;
      reset_ms[p] += ms;
      if (reset_ms[p] < 10)
        continue;
      ports[p] &= ~ps_prs;
      ports[p] |= ps_prsc;
      if (!reset_leaves_port_disabled)
        ports[p] |= ps_pes;
      int_status |= int_rhsc;
      ++port_resets;
      if (p == plug_port)
        dev.reset_bus();
    }
  }

  void plug(bool low_speed = false) noexcept {
    ports[plug_port] |= ps_ccs | ps_csc | (low_speed ? ps_lsda : 0u);
    int_status |= int_rhsc;
  }
  void unplug() noexcept {
    ports[plug_port] &= ~(ps_ccs | ps_pes | ps_lsda);
    ports[plug_port] |= ps_csc;
    int_status |= int_rhsc;
  }
  void raise_unrecoverable_error() noexcept { int_status |= int_ue; }

  // One 1 ms frame.
  void step() noexcept {
    fm_number = (fm_number + 1) & 0xFFFFu;
    int_status |= int_sf;
    if (((control >> 6) & 3u) != 2u || hcca == 0)
      return;
    wr(hcca + 0x80, fm_number);
    if (control & (1u << 2)) {
      u32 ed = rd(hcca + 4 * (fm_number % 32u));
      for (unsigned guard = 0; ed != 0 && guard < 256; ++guard) {
        (void)process_ed(ed);
        ed = rd(ed + 12) & ~0xFu;
      }
    }
    if ((control & (1u << 4)) && clf)
      clf = walk(ctrl_head);
    if ((control & (1u << 5)) && blf)
      blf = walk(bulk_head);
    deliver_done();
  }

  bool walk(u32 head) noexcept {
    bool ready = false;
    u32 ed = head & ~0xFu;
    for (unsigned guard = 0; ed != 0 && guard < 256; ++guard) {
      ready = process_ed(ed) || ready;
      ed = rd(ed + 12) & ~0xFu;
    }
    return ready;
  }

  void deliver_done() noexcept {
    if (done_chain == 0 || (int_status & int_wdh))
      return;
    const bool more = (int_status & int_enable & 0x7Du) != 0;
    wr(hcca + 0x84, done_chain | (more ? 1u : 0u));
    done_chain = 0;
    int_status |= int_wdh;
  }

  // ---- ED / TD execution ----
  enum class state { pending, done, halted };
  struct outcome {
    state st = state::done;
    bool carry = false;
  };

  // Returns true if the ED had a TD to work on.
  bool process_ed(u32 ed) noexcept {
    const u32 flags = rd(ed);
    if (flags & (1u << 14)) // skip
      return false;
    const u32 tail = rd(ed + 4) & ~0xFu;
    bool ready = false;
    for (unsigned n = 0; n < tds_per_pass; ++n) {
      const u32 headp = rd(ed + 8);
      if (headp & 1u)
        return ready;
      const u32 head = headp & ~0xFu;
      if (head == tail)
        return ready;
      ready = true;
      const outcome o = exec_td(flags, head, (headp & 2u) != 0);
      if (o.st == state::pending)
        return ready;
      const u32 next = rd(head + 8) & ~0xFu;
      wr(head + 8, done_chain);
      done_chain = head;
      wr(ed + 8, next | (o.carry ? 2u : 0u) | (o.st == state::halted ? 1u : 0u));
      if (o.st == state::halted)
        return ready;
    }
    return ready;
  }

  void finish_td(u32 td, u32 tf, u32 cc, u32 cbp) noexcept {
    wr(td, (tf & 0x0FFFFFFFu) | (cc << 28));
    wr(td + 4, cbp);
  }

  outcome exec_td(u32 flags, u32 td, bool carry) noexcept {
    const u32 tf = rd(td);
    const u32 cbp = rd(td + 4);
    const u32 be = rd(td + 12);
    const u32 fa = flags & 0x7Fu;
    const unsigned ep = (flags >> 7) & 0xFu;
    const u32 d = (flags >> 11) & 3u;
    const bool low = (flags & (1u << 13)) != 0;
    const std::size_t mps = (flags >> 16) & 0x7FFu;
    const u32 dp = d == 1 ? 1u : (d == 2 ? 2u : ((tf >> 19) & 3u));
    const bool round = (tf & (1u << 18)) != 0;
    bool toggle = (tf & (1u << 25)) ? (tf & (1u << 24)) != 0 : carry;

    std::size_t total = 0, first = 0;
    if (cbp != 0) {
      if ((cbp >> 12) == (be >> 12)) {
        if (be < cbp)
          ++bad_td;
        else
          total = first = be - cbp + 1;
      } else {
        if ((be >> 12) != (cbp >> 12) + 1)
          ++bad_td;
        first = 0x1000 - (cbp & 0xFFFu);
        total = first + (be & 0xFFFu) + 1;
      }
    }
    auto addr = [&](std::size_t i) -> u32 {
      return i < first ? cbp + static_cast<u32>(i) : (be & ~0xFFFu) + static_cast<u32>(i - first);
    };
    auto cur = [&](std::size_t done) -> u32 { return done < total ? addr(done) : 0u; };

    bool reach = false;
    for (std::size_t p = 0; p < nports; ++p)
      if (p == plug_port && (ports[p] & ps_ccs) && (ports[p] & ps_pes) && ((ports[p] & ps_lsda) != 0) == low)
        reach = dev.accepts(fa);
    if (!reach) {
      finish_td(td, tf, 5, cbp);
      return {state::halted, toggle};
    }
    if (mps == 0 || mps > 1023) {
      ++bad_td;
      finish_td(td, tf, 7, cbp);
      return {state::halted, toggle};
    }

    reloco::array<u8, 1024> pk{};
    const reloco::span<u8> pkv{pk.as_span()};
    const reloco::span<const u8> pkc = pkv;
    if (dp == 0) { // SETUP
      if (total != 8)
        ++bad_td;
      for (std::size_t i = 0; i < 8; ++i)
        pk[i] = env.get8(addr(i));
      const reloco::span<const u8> view = pkc.subspan(0, 8);
      (void)dev.setup(view, !toggle); // the device takes `data0`
      finish_td(td, tf, 0, 0);
      return {state::done, !toggle};
    }

    std::size_t done = 0;
    bool once = true;
    while (once || done < total) {
      once = false;
      const std::size_t n_out = (total - done) < mps ? total - done : mps;
      if (dp == 1) {
        for (std::size_t i = 0; i < n_out; ++i)
          pk[i] = env.get8(addr(done + i));
        const reloco::span<const u8> view = pkc.subspan(0, n_out);
        const usb_sim::reply r = dev.out(ep, view, toggle);
        if (r == usb_sim::reply::nak) {
          wr(td + 4, cur(done));
          return {state::pending, toggle};
        }
        if (r == usb_sim::reply::stall) {
          finish_td(td, tf, 4, cur(done));
          return {state::halted, toggle};
        }
        done += n_out;
        toggle = !toggle;
        continue;
      }
      std::size_t n = 0;
      bool dtog = false;
      const usb_sim::reply r = dev.in(ep, pkv.subspan(0, mps), n, dtog);
      if (ep == 2) {
        if (ep2_polls < ep2_frames.size())
          ep2_frames[ep2_polls] = fm_number;
        ++ep2_polls;
      }
      if (r == usb_sim::reply::nak) {
        wr(td + 4, cur(done));
        return {state::pending, toggle};
      }
      if (r == usb_sim::reply::stall) {
        finish_td(td, tf, 4, cur(done));
        return {state::halted, toggle};
      }
      if (dtog != toggle)
        dev.note_toggle_error();
      if (n > total - done) {
        finish_td(td, tf, 8, cur(done));
        return {state::halted, toggle};
      }
      for (std::size_t i = 0; i < n; ++i)
        env.put8(addr(done + i), pk[i]);
      done += n;
      toggle = !toggle;
      if (n < mps && done < total) {
        if (round) {
          finish_td(td, tf, 0, cur(done));
          return {state::done, toggle};
        }
        finish_td(td, tf, 9, cur(done));
        return {state::halted, toggle};
      }
    }
    finish_td(td, tf, 0, 0);
    return {state::done, toggle};
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

class OhciTest : public ::testing::Test {
protected:
  using hcd_t = ohci_hcd<test_env, 4>;

  test_env env;
  sim_ohci hc{env};
  hcd_t hcd{env};
  usb_host_controller_ref ref{hcd};
  usb::usb_host host{ref};
  reloco::array<u8, 512> cfg{};
  usb::usb_device udev{ref, reloco::span<u8>(cfg)};
  reloco::array<u8, 8192> tx{};
  reloco::array<u8, 8192> rx{};
  reloco::span<u8> txv{tx.as_span()};
  reloco::span<u8> rxv{rx.as_span()};

  OhciTest() { env.hc = &hc; }

  void TearDown() override {
    EXPECT_EQ(env.violations, 0u);
    EXPECT_EQ(hc.bad_td, 0u);
  }

  // Runs `n` frames: the controller works through its schedule, then the driver's interrupt handler runs.
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

  void start_and_plug(bool low_speed = false) {
    ASSERT_TRUE(hcd.start().has_value());
    hc.plug(low_speed);
    pump(2);
  }

  void enumerate(bool low_speed = false) {
    start_and_plug(low_speed);
    ASSERT_TRUE(run(host.enumerate(0, udev)).has_value());
  }

  [[nodiscard]] usb_pipe bulk_pipe(unsigned ep, usb_direction d) const {
    return usb_pipe{udev.address(), static_cast<u8>(ep), d, usb_transfer_type::bulk, 64, usb_speed::full, 0};
  }
  [[nodiscard]] usb_pipe ctl_pipe(u8 mps = 64) const {
    return usb_pipe{udev.address(), 0, usb_direction::out, usb_transfer_type::control, mps, usb_speed::full, 0};
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
};

// ---------------------------------------------------------------------------------------------
// start() / stop()
// ---------------------------------------------------------------------------------------------

TEST_F(OhciTest, StartProgramsController) {
  ASSERT_TRUE(hcd.start().has_value());
  EXPECT_NE(hc.hcca, 0u);
  EXPECT_EQ(hc.hcca % 256, 0u);
  EXPECT_LT(hc.hcca, 0x1'0000u * 16);
  EXPECT_NE(hc.ctrl_head, 0u);
  EXPECT_NE(hc.bulk_head, 0u);
  EXPECT_EQ((hc.control >> 6) & 3u, 2u); // USBOPERATIONAL
  EXPECT_EQ(hc.control & 0x34u, 0x34u);  // PLE | CLE | BLE
  const u32 enabled = (1u << 31) | (1u << 1) | (1u << 6) | (1u << 4);
  EXPECT_EQ(hc.int_enable & enabled, enabled);
  EXPECT_TRUE(hc.fit_toggled);
  EXPECT_EQ(hc.fm_interval & 0x3FFFu, 0x2EDFu);
  EXPECT_EQ(hc.periodic_start, 0x2EDFu * 9 / 10);
  EXPECT_EQ(hc.ls_threshold, 0x628u);
  EXPECT_EQ(hcd.port_count(), 2u);
  EXPECT_NE(hc.ports[0] & sim_ohci::ps_pps, 0u);
  EXPECT_NE(hc.ports[1] & sim_ohci::ps_pps, 0u);
  EXPECT_EQ(env.live_blocks(), 1u);
  EXPECT_EQ(hc.hcr_writes, 1u);

  hcd.stop();
  EXPECT_EQ(env.live_blocks(), 0u);
  EXPECT_NE((hc.control >> 6) & 3u, 2u);
}

TEST_F(OhciTest, StartFailsBoundedWhenResetNeverCompletes) {
  hc.hcr_never_clears = true;
  auto r = hcd.start();
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::timed_out);
  EXPECT_EQ(env.live_blocks(), 0u);
}

TEST_F(OhciTest, StartTakesOwnershipFromSmm) {
  hc.smm_owned = true;
  ASSERT_TRUE(hcd.start().has_value());
  EXPECT_EQ(hc.ocr_writes, 1u);
  EXPECT_FALSE(hc.smm_owned);
}

TEST_F(OhciTest, StartFailsWhenSmmNeverReleases) {
  hc.smm_owned = true;
  hc.smm_never_releases = true;
  auto r = hcd.start();
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::timed_out);
  EXPECT_EQ(env.live_blocks(), 0u);
}

TEST_F(OhciTest, StartRejectsUnknownRevision) {
  hc.revision = 0x11;
  auto r = hcd.start();
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::unsupported_operation);
}

TEST_F(OhciTest, StartFailsWhenDmaMemoryUnavailable) {
  env.fail_alloc = true;
  auto r = hcd.start();
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::allocation_failed);
}

TEST_F(OhciTest, SubmitBeforeStartFails) {
  xfer t;
  t.pipe = bulk_pipe(1, usb_direction::in);
  t.data = rxv.data();
  t.length = 8;
  EXPECT_FALSE(ref.submit(t).has_value());
}

// ---------------------------------------------------------------------------------------------
// Root hub
// ---------------------------------------------------------------------------------------------

TEST_F(OhciTest, PortStatusReportsConnectionAndClearsChange) {
  ASSERT_TRUE(hcd.start().has_value());
  auto s = hcd.port_status(0);
  ASSERT_TRUE(s.has_value());
  EXPECT_FALSE(s->connected);
  EXPECT_FALSE(s->changed);

  hc.plug(false);
  s = hcd.port_status(0);
  ASSERT_TRUE(s.has_value());
  EXPECT_TRUE(s->connected);
  EXPECT_TRUE(s->changed);
  EXPECT_FALSE(s->enabled);
  EXPECT_EQ(s->speed, usb_speed::full);
  s = hcd.port_status(0);
  EXPECT_FALSE(s->changed);

  hc.unplug();
  hc.plug(true);
  s = hcd.port_status(0);
  EXPECT_TRUE(s->changed);
  EXPECT_EQ(s->speed, usb_speed::low);

  hc.unplug();
  s = hcd.port_status(0);
  EXPECT_TRUE(s->changed);
  EXPECT_FALSE(s->connected);
  EXPECT_FALSE(hcd.port_status(2).has_value());
}

TEST_F(OhciTest, RootHubStatusChangeLatchesAndNotifiesHook) {
  ASSERT_TRUE(hcd.start().has_value());
  unsigned calls = 0;
  hcd.set_port_event_hook([](void *c) noexcept { ++*static_cast<unsigned *>(c); }, &calls);
  hc.plug(false);
  hcd.irq();
  EXPECT_EQ(calls, 1u);
  EXPECT_EQ(hc.ports[0] & sim_ohci::ps_csc, 0u); // acknowledged by the interrupt handler
  EXPECT_EQ(hc.int_status & sim_ohci::int_rhsc, 0u);
  auto s = hcd.port_status(0);
  EXPECT_TRUE(s->changed); // but still reported to the stack
  {
    auto s2 = hcd.port_status(0);
    EXPECT_FALSE(s2->changed);
  }
}

TEST_F(OhciTest, ResetPortWaitsAndEnablesPort) {
  start_and_plug();
  ASSERT_TRUE(run(ref.reset_port(0)).has_value());
  EXPECT_GE(env.delay_total_ms, 50u);
  EXPECT_EQ(hc.port_resets, 1u);
  auto s = hcd.port_status(0);
  ASSERT_TRUE(s.has_value());
  EXPECT_TRUE(s->connected);
  EXPECT_TRUE(s->enabled);
  EXPECT_EQ(hc.ports[0] & sim_ohci::ps_prsc, 0u); // PRSC cleared
  EXPECT_EQ(hc.power_cleared, 0u);
}

TEST_F(OhciTest, ResetPortEnablesPortTheControllerLeftDisabled) {
  hc.reset_leaves_port_disabled = true;
  start_and_plug();
  ASSERT_TRUE(run(ref.reset_port(0)).has_value());
  {
    auto s = hcd.port_status(0);
    ASSERT_TRUE(s.has_value());
    EXPECT_TRUE(s->enabled);
  }
}

TEST_F(OhciTest, ResetPortFailsWithoutDeviceOrForBadPort) {
  start_and_plug();
  EXPECT_FALSE(run(ref.reset_port(1)).has_value());
  EXPECT_FALSE(run(ref.reset_port(7)).has_value());
  EXPECT_EQ(hc.port_resets, 0u);
}

// ---------------------------------------------------------------------------------------------
// Enumeration and control transfers
// ---------------------------------------------------------------------------------------------

TEST_F(OhciTest, EnumeratesFullSpeedDevice) {
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
}

TEST_F(OhciTest, EnumeratesLowSpeedDevice) {
  enumerate(true);
  EXPECT_EQ(udev.speed(), usb_speed::low);
  EXPECT_EQ(hc.dev.address, 1);
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
}

TEST_F(OhciTest, ControlInStopsOnShortPacket) {
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

TEST_F(OhciTest, ControlInWithSmallPacketsAndExactMultiple) {
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

TEST_F(OhciTest, ControlOutDataStage) {
  enumerate();
  const reloco::array<u8, 5> payload{9, 8, 7, 6, 5};
  reloco::array<u8, 5> buf = payload;
  const usb_completion c = do_ctl(ctl_pipe(), usb_setup_packet{0x21, 0x20, 0, 0, 5}, buf.as_span());
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(hc.dev.last_ctl_out_len, 5u);
  EXPECT_TRUE(same(hc.dev.last_ctl_out.as_span().subspan(0, 5), payload.as_span()));
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
}

TEST_F(OhciTest, ControlWithoutDataStage) {
  enumerate();
  const unsigned before = hc.dev.set_config_count;
  const usb_completion c = do_ctl(ctl_pipe(), usb_setup_packet{0x00, 9, 1, 0, 0});
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(hc.dev.set_config_count, before + 1);
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
}

TEST_F(OhciTest, ControlStallsAreReportedAndTheEndpointKeepsWorking) {
  enumerate();
  usb_completion c = do_ctl(ctl_pipe(), usb_setup_packet{0xC0, 0x99, 0, 0, 8}, rx.as_span().subspan(0, 8));
  EXPECT_EQ(c.status, usb_status::stall);
  c = do_ctl(ctl_pipe(), usb_setup_packet{0x40, 0x99, 0, 0, 0});
  EXPECT_EQ(c.status, usb_status::stall);
  c = do_ctl(ctl_pipe(), usb_setup_packet{0x80, 6, 0x0301, 0, 64}, rx.as_span().subspan(0, 64));
  EXPECT_TRUE(c.ok());
  pump(8);
  EXPECT_EQ(hcd.resources_in_use(), 0u);
}

TEST_F(OhciTest, ControlSlowPathOneTdPerFrame) {
  hc.tds_per_pass = 1;
  enumerate();
  EXPECT_EQ(udev.address(), 1);
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
}

// ---------------------------------------------------------------------------------------------
// Bulk transfers
// ---------------------------------------------------------------------------------------------

TEST_F(OhciTest, BulkLoopbackSizes) {
  enumerate();
  const usb_pipe po = bulk_pipe(1, usb_direction::out);
  const usb_pipe pi = bulk_pipe(1, usb_direction::in);
  const reloco::array<std::size_t, 7> sizes{0, 1, 63, 64, 65, 1000, 5000};
  for (const std::size_t n : sizes) {
    fill_pattern(tx.as_span().subspan(0, n), static_cast<unsigned>(n));
    usb_completion c = do_out(po, tx.as_span().subspan(0, n));
    ASSERT_TRUE(c.ok()) << n;
    EXPECT_EQ(c.actual, n);
    if (n == 0)
      continue; // the device NAKs IN while its FIFO is empty
    c = do_in(pi, rx.as_span().subspan(0, n));
    ASSERT_TRUE(c.ok()) << n;
    EXPECT_EQ(c.actual, n);
    EXPECT_TRUE(same(tx.as_span().subspan(0, n), rx.as_span().subspan(0, n))) << n;
  }
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
  EXPECT_EQ(hc.dev.bulk_out_packets, 1u + 1 + 1 + 1 + 2 + 16 + 79);
  EXPECT_EQ(hc.dev.bulk_in_packets, 1u + 1 + 1 + 2 + 16 + 79);
  pump(8);
  EXPECT_EQ(hcd.resources_in_use(), 0u);
  EXPECT_EQ(env.live_blocks(), 1u);
}

TEST_F(OhciTest, BulkInStaysPendingWhileDeviceNaks) {
  enumerate();
  job in;
  auto ti = bulk_in_job(ref, bulk_pipe(1, usb_direction::in), rx.as_span().subspan(0, 64), in);
  ti.resume();
  pump(20);
  EXPECT_FALSE(in.done);
  EXPECT_EQ(hcd.resources_in_use(), 1u);

  fill_pattern(tx.as_span().subspan(0, 10), 3);
  ASSERT_TRUE(do_out(bulk_pipe(1, usb_direction::out), tx.as_span().subspan(0, 10)).ok());
  ASSERT_TRUE(wait(in.done));
  EXPECT_TRUE(in.c.ok());
  EXPECT_EQ(in.c.actual, 10u);
  EXPECT_TRUE(same(tx.as_span().subspan(0, 10), rx.as_span().subspan(0, 10)));
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
}

TEST_F(OhciTest, BulkInShortPacketEndsMultiTdTransfer) {
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

TEST_F(OhciTest, BulkInBufferSmallerThanPacketIsBabble) {
  enumerate();
  ASSERT_TRUE(do_out(bulk_pipe(1, usb_direction::out), tx.as_span().subspan(0, 64)).ok());
  const usb_completion c = do_in(bulk_pipe(1, usb_direction::in), rx.as_span().subspan(0, 10));
  EXPECT_EQ(c.status, usb_status::babble);
}

TEST_F(OhciTest, StalledEndpointReportsStall) {
  enumerate();
  EXPECT_EQ(do_out(bulk_pipe(3, usb_direction::out), tx.as_span().subspan(0, 8)).status, usb_status::stall);
  EXPECT_EQ(do_in(bulk_pipe(3, usb_direction::in), rx.as_span().subspan(0, 8)).status, usb_status::stall);
}

TEST_F(OhciTest, TransferToUnknownAddressTimesOut) {
  enumerate();
  usb_pipe p = bulk_pipe(1, usb_direction::out);
  p.address = 55;
  EXPECT_EQ(do_out(p, tx.as_span().subspan(0, 8)).status, usb_status::timeout);
}

TEST_F(OhciTest, ResetDataToggleRestartsAtDataZero) {
  enumerate();
  const usb_pipe po = bulk_pipe(1, usb_direction::out);
  ASSERT_TRUE(do_out(po, tx.as_span().subspan(0, 10)).ok());
  hc.dev.reset_toggles();
  ref.reset_data_toggle(po);
  ASSERT_TRUE(do_out(po, tx.as_span().subspan(0, 10)).ok());
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
  EXPECT_EQ(hc.dev.bulk_out_packets, 2u);
}

TEST_F(OhciTest, ToggleContinuesAcrossTransfersWithoutReset) {
  enumerate();
  const usb_pipe po = bulk_pipe(1, usb_direction::out);
  for (int i = 0; i < 3; ++i)
    ASSERT_TRUE(do_out(po, tx.as_span().subspan(0, 10)).ok());
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
  hc.dev.reset_toggles(); // the device restarts at DATA0 but the host did not: the model must notice
  ASSERT_TRUE(do_out(po, tx.as_span().subspan(0, 10)).ok());
  EXPECT_EQ(hc.dev.toggle_errors, 1u);
}

TEST_F(OhciTest, ClearHaltRestartsTheEndpointToggle) {
  enumerate();
  const usb_pipe po = bulk_pipe(1, usb_direction::out);
  ASSERT_TRUE(do_out(po, tx.as_span().subspan(0, 10)).ok());
  ASSERT_TRUE(do_ctl(ctl_pipe(), usb_setup_packet{0x02, 1, 0, 0x01, 0}).ok()); // CLEAR_FEATURE(HALT) ep 1 OUT
  EXPECT_EQ(hc.dev.clear_halt_count, 1u);
  hc.dev.reset_toggles();
  ASSERT_TRUE(do_out(po, tx.as_span().subspan(0, 10)).ok());
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
}

TEST_F(OhciTest, SetConfigurationRestartsToggles) {
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

TEST_F(OhciTest, InterruptInDeliversData) {
  enumerate();
  hc.dev.arm_interrupt();
  const usb_pipe p{udev.address(), 2, usb_direction::in, usb_transfer_type::interrupt, 8, usb_speed::full, 4};
  const usb_completion c = do_in(p, rx.as_span().subspan(0, 8));
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(c.actual, 8u);
  EXPECT_EQ(rx[0], 1);
  EXPECT_EQ(rx[7], 8);
  EXPECT_EQ(hc.dev.toggle_errors, 0u);
  pump(8);
  EXPECT_EQ(hcd.resources_in_use(), 0u);
}

TEST_F(OhciTest, InterruptEndpointIsPolledAtItsInterval) {
  enumerate();
  struct row {
    unsigned interval;
    u32 expected;
  };
  const reloco::array<row, 7> rows{row{1, 1}, row{4, 4}, row{8, 8}, row{32, 32}, row{12, 8}, row{0, 1}, row{100, 32}};
  for (const row &r : rows) {
    hc.ep2_polls = 0;
    xfer t;
    t.pipe = usb_pipe{udev.address(),
                      2,
                      usb_direction::in,
                      usb_transfer_type::interrupt,
                      8,
                      usb_speed::full,
                      static_cast<u8>(r.interval)};
    t.data = rxv.data();
    t.length = 8;
    ASSERT_TRUE(ref.submit(t).has_value());
    pump(100);
    ASSERT_GE(hc.ep2_polls, 3u) << r.interval;
    const unsigned n = hc.ep2_polls < hc.ep2_frames.size() ? hc.ep2_polls : static_cast<unsigned>(hc.ep2_frames.size());
    for (unsigned i = 1; i < n; ++i)
      EXPECT_EQ((hc.ep2_frames[i] - hc.ep2_frames[i - 1]) & 0xFFFFu, r.expected) << r.interval;
    EXPECT_FALSE(t.finished);
    ref.cancel(t);
    pump(6);
    EXPECT_EQ(hcd.resources_in_use(), 0u);
  }
}

// ---------------------------------------------------------------------------------------------
// Cancel, unplug, capacity, errors
// ---------------------------------------------------------------------------------------------

TEST_F(OhciTest, CancelRecyclesMemoryOnlyAfterStartOfFrames) {
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
  EXPECT_EQ(hcd.resources_in_use(), 1u); // the controller may still hold it: kept as a zombie
  pump(1);
  EXPECT_EQ(hcd.resources_in_use(), 1u);
  pump(4);
  EXPECT_EQ(hcd.resources_in_use(), 0u);
  EXPECT_EQ(env.live_blocks(), blocks);
  EXPECT_EQ(t.completions, 0u);
  EXPECT_EQ(hc.int_enable & sim_ohci::int_sf, 0u); // SOF interrupt only while a removal is pending
  pump(10);
  EXPECT_EQ(t.completions, 0u);
}

TEST_F(OhciTest, DestroyingTheAwaitingCoroutineCancelsTheTransfer) {
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
  // The endpoint can be used again right away.
  ASSERT_TRUE(do_out(bulk_pipe(1, usb_direction::out), tx.as_span().subspan(0, 4)).ok());
  EXPECT_TRUE(do_in(bulk_pipe(1, usb_direction::in), rx.as_span().subspan(0, 4)).ok());
}

TEST_F(OhciTest, CancelThenResubmitSameEndpoint) {
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

TEST_F(OhciTest, UnplugCompletesPendingTransfersAsDisconnected) {
  enumerate();
  job in;
  auto t = bulk_in_job(ref, bulk_pipe(1, usb_direction::in), rx.as_span().subspan(0, 64), in);
  t.resume();
  pump(5);
  EXPECT_FALSE(in.done);
  hc.unplug();
  ASSERT_TRUE(wait(in.done, 50));
  EXPECT_EQ(in.c.status, usb_status::disconnected);
  // New transfers to the gone device fail the same way.
  EXPECT_EQ(do_out(bulk_pipe(1, usb_direction::out), tx.as_span().subspan(0, 4)).status, usb_status::disconnected);
  pump(8);
  EXPECT_EQ(hcd.resources_in_use(), 0u);
}

TEST_F(OhciTest, SubmitBeyondCapacityFailsCleanly) {
  enumerate();
  reloco::array<xfer, 5> x{};
  x[0].pipe = ctl_pipe();
  x[0].setup = usb_setup_packet{0x80, 6, 0x0301, 0, 64};
  x[0].data = rxv.data();
  x[0].length = 64;
  x[1].pipe = bulk_pipe(1, usb_direction::in);
  x[1].data = rxv.data();
  x[1].length = 8;
  x[2].pipe = bulk_pipe(3, usb_direction::out); // stalls: completes without feeding the IN loopback
  x[2].data = txv.data();
  x[2].length = 8;
  x[3].pipe = usb_pipe{udev.address(), 2, usb_direction::in, usb_transfer_type::interrupt, 8, usb_speed::full, 8};
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

  pump(60);
  EXPECT_TRUE(x[0].finished);
  EXPECT_TRUE(x[2].finished);
  EXPECT_FALSE(x[1].finished);
  EXPECT_FALSE(x[3].finished);
  ref.cancel(x[1]);
  ref.cancel(x[3]);
  pump(6);
  EXPECT_EQ(hcd.resources_in_use(), 0u);
  EXPECT_TRUE(ref.submit(x[4]).has_value()); // capacity is back (ep 3 stalls and completes)
  pump(20);
  EXPECT_TRUE(x[4].finished);
  EXPECT_EQ(x[4].res.status, usb_status::stall);
}

TEST_F(OhciTest, RejectsInvalidRequests) {
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
  big.length = 33 * 4096;
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

TEST_F(OhciTest, UnrecoverableErrorFailsTransfers) {
  enumerate();
  job in;
  auto t = bulk_in_job(ref, bulk_pipe(1, usb_direction::in), rx.as_span().subspan(0, 64), in);
  t.resume();
  pump(3);
  hc.raise_unrecoverable_error();
  hcd.irq();
  ASSERT_TRUE(in.done);
  EXPECT_EQ(in.c.status, usb_status::bus_error);
  EXPECT_EQ(do_out(bulk_pipe(1, usb_direction::out), tx.as_span().subspan(0, 4)).status, usb_status::bus_error);
}

TEST_F(OhciTest, StopCancelsPendingTransfersAndFreesMemory) {
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

// ---------------------------------------------------------------------------------------------
// bootldr::usb_stack hot plug on top of the driver
// ---------------------------------------------------------------------------------------------

class OhciStackTest : public OhciTest {
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

  OhciStackTest() : stack(sched, ref, config()) {
    sched.set_clock(&clock, this);
    EXPECT_TRUE(sched.add_poller(&service, this).has_value());
    stack.set_event_handler(&on_event, this);
  }

  static std::uint64_t clock(void *c) noexcept { return static_cast<OhciStackTest *>(c)->now; }
  static void service(void *c) noexcept {
    auto *t = static_cast<OhciStackTest *>(c);
    t->now += 10;
    t->pump();
  }
  static void on_event(void *c, bootldr::usb_event_kind k, unsigned, bootldr::usb_attached_device *,
                       const reloco::result<void> &) noexcept {
    auto *t = static_cast<OhciStackTest *>(c);
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
  auto *t = static_cast<OhciStackTest *>(ctx);
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

TEST_F(OhciStackTest, HotPlugRunsDriverCoroutineUntilUnplug) {
  ASSERT_TRUE(hcd.start().has_value());
  ASSERT_TRUE(stack.add_driver({&match_any, &loopback_driver, nullptr, this}).has_value());
  ASSERT_TRUE(stack.start().has_value());

  rounds(5);
  EXPECT_EQ(started, 0u);

  hc.plug(false);
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
  hc.dev.reset_bus();
  hc.plug(false);
  rounds(40);
  EXPECT_EQ(attached, 2u);
  EXPECT_EQ(started, 2u);
}

#endif // RELOCO_HAS_COROUTINES
