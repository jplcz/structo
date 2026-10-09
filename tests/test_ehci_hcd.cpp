// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

// Register-level tests of the generic EHCI driver. `ehci_model` below is a small EHCI 1.0 host controller
// model: capability/operational registers, port state machine, and a schedule engine that parses the
// driver's DMA structures (QH/qTD as defined by the EHCI spec, independently of the driver's own structs)
// and runs every packet it would put on the wire against the shared packet-level device `usb_sim::device`.
// Freed DMA memory is poisoned and the model flags any walk over a freed queue head or access to a freed buffer.

#include <gtest/gtest.h>
#include <structo/bootldr/usb_stack.hpp>
#include <structo/hw/ehci_hcd.hpp>
#include <structo/usb/usb_host.hpp>

#include <reloco/array.hpp>
#include <reloco/span.hpp>

#include "usb_hcd_sim.hpp"

using namespace structo;
using namespace structo::hw;

namespace {

constexpr std::size_t arena_size = 2 * 1024 * 1024;

// ---------------------------------------------------------------------------------------------
// DMA arena: bump allocator, freed blocks are poisoned and remembered.
// ---------------------------------------------------------------------------------------------

class arena {
public:
  static constexpr std::uint32_t base = 0x1000; ///< bus address of the first arena byte

  [[nodiscard]] usb_dma_buffer alloc(std::size_t size, std::size_t align) noexcept {
    const std::size_t off = (used_ + align - 1) & ~(align - 1);
    if (off + size > mem_.size())
      return {};
    used_ = off + size;
    usb_dma_buffer b;
    b.virt = at_off<std::uint8_t>(off, size);
    b.phys = base + off;
    b.size = size;
    ++allocs;
    return b;
  }

  void release(const usb_dma_buffer &b) noexcept {
    ++frees;
    const std::size_t off = static_cast<std::size_t>(b.phys - base);
    const reloco::span<std::uint8_t> whole = mem_.as_span();
    for (std::size_t i = 0; i < b.size; ++i)
      whole[off + i] = 0xDD;
    if (nfreed_ < freed_.size())
      freed_[nfreed_++] = range{static_cast<std::uint32_t>(b.phys), static_cast<std::uint32_t>(b.phys + b.size)};
  }

  /// True if [phys, phys+n) lies inside the arena.
  [[nodiscard]] bool valid(std::uint32_t phys, std::size_t n) const noexcept {
    return phys >= base && static_cast<std::size_t>(phys - base) + n <= mem_.size();
  }
  /// True if [phys, phys+n) overlaps a freed block.
  [[nodiscard]] bool freed(std::uint32_t phys, std::size_t n) const noexcept {
    for (std::size_t i = 0; i < nfreed_; ++i)
      if (phys < freed_[i].end && phys + n > freed_[i].begin)
        return true;
    return false;
  }

  template <typename T> [[nodiscard]] T *at(std::uint32_t phys) noexcept {
    return at_off<T>(phys - base, sizeof(T));
  }

  unsigned allocs = 0;
  unsigned frees = 0;

private:
  template <typename T> [[nodiscard]] T *at_off(std::size_t off, std::size_t n) noexcept {
    const reloco::span<std::uint8_t> whole = mem_.as_span();
    (void)whole.subspan(off, n);
    return reinterpret_cast<T *>(reinterpret_cast<std::uintptr_t>(mem_.data()) + off);
  }
  struct range {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
  };
  alignas(4096) reloco::array<std::uint8_t, arena_size> mem_{};
  std::size_t used_ = 0;
  reloco::array<range, 1024> freed_{};
  std::size_t nfreed_ = 0;
};

// ---------------------------------------------------------------------------------------------
// The EHCI model. Bit positions and layouts are taken from the EHCI 1.0 specification on purpose and
// not from the driver header.
// ---------------------------------------------------------------------------------------------

struct raw_qtd {
  reloco::array<std::uint32_t, 8> d; // 0 next, 1 alt next, 2 token, 3..7 buffer pointers
};
struct raw_qh {
  reloco::array<std::uint32_t, 12> d; // 0 horizontal, 1 ep char, 2 ep caps, 3 current qTD, 4..11 overlay qTD
};

enum class speed_kind { high, full, low };

class ehci_model {
public:
  static constexpr unsigned n_ports = 4;
  static constexpr std::uint32_t cap_length = 0x20;
  static constexpr std::uint32_t hcsparams = n_ports | (1u << 4) /*PPC*/ | (2u << 8) /*N_PCC*/ | (2u << 12) /*N_CC*/;
  static constexpr std::uint32_t hccparams = 1u /*64-bit*/ | (0xA0u << 8) /*EECP*/;

  struct port {
    bool connected = false;
    speed_kind kind = speed_kind::high;
    bool owner = true;
    bool enabled = false;
    bool csc = false;
    bool pr = false;
    bool pp = false;
  };

  ehci_model(arena &mem, usb_sim::device &dev) noexcept : mem_(&mem), dev_(&dev) {}

  // ---- knobs and observations for the tests ----
  bool stuck_hcreset = false;
  bool stuck_port_reset = false;
  bool inject_hse = false;
  bool recycle = false; ///< bulk loopback with unlimited capacity (see the transaction code)
  reloco::array<port, n_ports> ports{};
  std::uint32_t usbcmd = 0;
  std::uint32_t usbsts = 0;
  std::uint32_t usbintr = 0;
  std::uint32_t frindex = 0;
  std::uint32_t ctrldssegment = 0;
  std::uint32_t periodiclistbase = 0;
  std::uint32_t asynclistaddr = 0;
  std::uint32_t configflag = 0;
  unsigned iaad_rings = 0;
  unsigned iaa_raised = 0;
  unsigned freed_qh_walks = 0;
  unsigned freed_buffer_accesses = 0;
  unsigned bad_links = 0;
  unsigned bad_dma = 0;
  unsigned naks = 0;
  unsigned toggle_mismatches = 0;
  unsigned hcreset_count = 0;

  // ---- registers ----

  [[nodiscard]] std::uint32_t read32(std::size_t off) noexcept {
    if (off < cap_length) {
      switch (off) {
      case 0x00:
        return cap_length | (0x0100u << 16);
      case 0x04:
        return hcsparams;
      case 0x08:
        return hccparams;
      default:
        return 0;
      }
    }
    const std::size_t o = off - cap_length;
    switch (o) {
    case 0x00:
      return usbcmd;
    case 0x04:
      return status_value();
    case 0x08:
      return usbintr;
    case 0x0C:
      return frindex;
    case 0x10:
      return ctrldssegment;
    case 0x14:
      return periodiclistbase;
    case 0x18:
      return asynclistaddr;
    case 0x40:
      return configflag;
    default:
      break;
    }
    if (o >= 0x44 && o < 0x44 + 4 * n_ports)
      return port_value(ports[(o - 0x44) / 4]);
    return 0;
  }

  void write32(std::size_t off, std::uint32_t v) noexcept {
    if (off < cap_length)
      return;
    const std::size_t o = off - cap_length;
    switch (o) {
    case 0x00:
      write_cmd(v);
      return;
    case 0x04:
      usbsts &= ~(v & 0x3F);
      return;
    case 0x08:
      usbintr = v & 0x3F;
      return;
    case 0x0C:
      frindex = v & 0x3FFF;
      return;
    case 0x10:
      ctrldssegment = v;
      return;
    case 0x14:
      periodiclistbase = v & ~0xFFFu;
      return;
    case 0x18:
      asynclistaddr = v & ~0x1Fu;
      return;
    case 0x40:
      configflag = v & 1;
      for (auto &p : ports) {
        p.owner = configflag == 0;
        if (configflag && p.connected)
          p.csc = true;
      }
      return;
    default:
      break;
    }
    if (o >= 0x44 && o < 0x44 + 4 * n_ports)
      write_port(ports[(o - 0x44) / 4], v);
  }

  // ---- events from the test ----

  void plug(speed_kind k, unsigned idx = 0) noexcept {
    port &p = ports[idx];
    p.connected = true;
    p.kind = k;
    p.csc = true;
    p.owner = configflag == 0;
    p.enabled = false;
    usbsts |= 1u << 2; // PCD
  }
  void unplug(unsigned idx = 0) noexcept {
    port &p = ports[idx];
    p.connected = false;
    p.enabled = false;
    p.csc = true;
    p.owner = configflag == 0;
    usbsts |= 1u << 2;
  }

  [[nodiscard]] bool running() const noexcept { return (usbcmd & 1u) != 0; }
  [[nodiscard]] bool reachable() const noexcept {
    return ports[0].connected && ports[0].enabled && !ports[0].owner;
  }

  /// Number of queue heads on the async ring (including the dummy), following the horizontal pointers.
  [[nodiscard]] std::size_t async_ring_length() noexcept {
    std::size_t n = 0;
    std::uint32_t a = asynclistaddr;
    do {
      if (!mem_->valid(a, sizeof(raw_qh)) || ++n > 64)
        return n;
      a = mem_->at<raw_qh>(a)->d[0] & ~0x1Fu;
    } while (a != asynclistaddr);
    return n;
  }
  /// Number of queue heads on the periodic chain of frame 0 (including the dummy).
  [[nodiscard]] std::size_t periodic_chain_length() noexcept {
    std::size_t n = 0;
    std::uint32_t link = *mem_->at<std::uint32_t>(periodiclistbase);
    while (!(link & 1) && n < 64) {
      ++n;
      link = mem_->at<raw_qh>(link & ~0x1Fu)->d[0];
    }
    return n;
  }
  [[nodiscard]] std::uint32_t frame_list_entry(std::size_t i) noexcept {
    return *mem_->at<std::uint32_t>(periodiclistbase + static_cast<std::uint32_t>(4 * i));
  }
  [[nodiscard]] const raw_qh &async_head() noexcept { return *mem_->at<raw_qh>(asynclistaddr); }

  /// One 1 ms frame passes (FRINDEX += 8): the periodic list is visited once (first micro-frame) and the
  /// async list once (each queue head gets up to `async_budget` transactions).
  void step() noexcept {
    if (!running())
      return;
    frindex = (frindex + 8) & 0x3FFF;
    if (inject_hse)
      usbsts |= 1u << 4;
    if (usbcmd & (1u << 4))
      walk_periodic();
    if (usbcmd & (1u << 5)) {
      walk_async();
      if (usbcmd & (1u << 6)) { // doorbell: the controller has advanced past the unlink
        usbcmd &= ~(1u << 6);
        usbsts |= 1u << 5;
        ++iaa_raised;
      }
    }
  }

  // Captured OUT data and the feed for the IN side of the "recycle" loopback.
  reloco::array<std::uint8_t, 65536> capture{};
  std::size_t capture_len = 0;
  std::size_t feed_pos = 0;
  void reset_capture() noexcept {
    capture_len = 0;
    feed_pos = 0;
  }

private:
  static constexpr unsigned async_budget = 16;

  [[nodiscard]] std::uint32_t status_value() const noexcept {
    std::uint32_t v = usbsts;
    if (!running())
      v |= 1u << 12;
    if (running() && (usbcmd & (1u << 5)))
      v |= 1u << 15;
    if (running() && (usbcmd & (1u << 4)))
      v |= 1u << 14;
    return v;
  }

  void write_cmd(std::uint32_t v) noexcept {
    if (v & 2u) { // HCRESET
      if (stuck_hcreset) {
        usbcmd |= 2u;
        return;
      }
      hc_reset();
      return;
    }
    if ((v & (1u << 6)) && !(usbcmd & (1u << 6)))
      ++iaad_rings;
    usbcmd = v;
  }

  void hc_reset() noexcept {
    ++hcreset_count;
    usbcmd = 0;
    usbsts = 0;
    usbintr = 0;
    frindex = 0;
    ctrldssegment = 0;
    periodiclistbase = 0;
    asynclistaddr = 0;
    configflag = 0;
    for (auto &p : ports) {
      p.owner = true;
      p.enabled = false;
      p.pr = false;
      p.pp = false;
    }
  }

  [[nodiscard]] static std::uint32_t port_value(const port &p) noexcept {
    std::uint32_t v = 0;
    const bool visible = p.connected && !p.owner;
    if (visible)
      v |= 1u << 0;
    if (p.csc)
      v |= 1u << 1;
    if (p.enabled)
      v |= 1u << 2;
    if (p.pr)
      v |= 1u << 8;
    if (visible && !p.enabled && !p.pr)
      v |= (p.kind == speed_kind::low ? 1u : 2u) << 10; // K for low speed, J otherwise
    if (p.pp)
      v |= 1u << 12;
    if (p.owner)
      v |= 1u << 13;
    return v;
  }

  void write_port(port &p, std::uint32_t v) noexcept {
    if (v & (1u << 1))
      p.csc = false;
    if (!(v & (1u << 2)))
      p.enabled = false;
    if ((v & (1u << 8)) && !p.pr) {
      p.pr = true;
      p.enabled = false;
    } else if (!(v & (1u << 8)) && p.pr && !stuck_port_reset) {
      p.pr = false;
      if (p.connected && !p.owner) {
        dev_->reset_bus();
        p.enabled = p.kind == speed_kind::high; // full-speed devices do not answer the high-speed chirp
      }
    }
    p.pp = (v & (1u << 12)) != 0;
    if ((v & (1u << 13)) && !p.owner) {
      p.owner = true;
      p.enabled = false;
    }
  }

  // ---- schedule walking ----

  /// Checks a queue head address; false (and a flag) if the controller must not walk it.
  [[nodiscard]] bool qh_ok(std::uint32_t addr) noexcept {
    if (!mem_->valid(addr, sizeof(raw_qh)) || (addr & 0x1F)) {
      ++bad_dma;
      return false;
    }
    if (mem_->freed(addr, sizeof(raw_qh)) || mem_->at<raw_qh>(addr)->d[1] == 0) {
      ++freed_qh_walks;
      return false;
    }
    return true;
  }

  void walk_async() noexcept {
    const std::uint32_t start = asynclistaddr;
    std::uint32_t addr = start;
    for (unsigned visits = 0; visits < 64; ++visits) {
      if (!qh_ok(addr))
        return;
      raw_qh *q = mem_->at<raw_qh>(addr);
      process_qh(*q, async_budget);
      const std::uint32_t next = q->d[0];
      if ((next & 1) || ((next >> 1) & 3) != 1) {
        ++bad_links;
        return;
      }
      addr = next & ~0x1Fu;
      if (addr == start)
        return;
    }
    ++bad_links; // ring never closed
  }

  void walk_periodic() noexcept {
    const std::uint32_t frame = (frindex >> 3) & 0x3FF;
    std::uint32_t link = frame_list_entry(frame);
    for (unsigned visits = 0; visits < 64 && !(link & 1); ++visits) {
      if (((link >> 1) & 3) != 1) {
        ++bad_links;
        return;
      }
      const std::uint32_t addr = link & ~0x1Fu;
      if (!qh_ok(addr))
        return;
      raw_qh *q = mem_->at<raw_qh>(addr);
      if (q->d[2] & 0x01) // S-mask: micro-frame 0
        process_qh(*q, 1);
      link = q->d[0];
    }
  }

  enum class txn { progress, nak, stop };

  void process_qh(raw_qh &q, unsigned budget) noexcept {
    for (; budget > 0; --budget) {
      std::uint32_t tok = q.d[6];
      if (tok & 0x40) // halted
        return;
      if (!(tok & 0x80)) { // overlay idle: fetch the next qTD
        const std::uint32_t next = q.d[4];
        if (next & 1)
          return;
        const std::uint32_t qa = next & ~0x1Fu;
        if (!mem_->valid(qa, sizeof(raw_qtd)) || mem_->freed(qa, sizeof(raw_qtd))) {
          ++bad_dma;
          return;
        }
        const raw_qtd *t = mem_->at<raw_qtd>(qa);
        for (std::size_t k = 0; k < 8; ++k)
          q.d[4 + k] = t->d[k];
        q.d[3] = qa;
        tok = q.d[6];
        if (!(tok & 0x80))
          return; // an inactive qTD stops the queue
      }
      if (transaction(q) == txn::nak)
        return;
    }
  }

  // Buffer byte address `i` bytes after the qTD's current position.
  [[nodiscard]] std::uint32_t buf_addr(const raw_qh &q, std::size_t i) noexcept {
    const std::uint32_t tok = q.d[6];
    const std::size_t pos = (q.d[7] & 0xFFFu) + i;
    const std::size_t page = ((tok >> 12) & 7u) + pos / 4096;
    if (page > 4) {
      ++bad_dma;
      return 0;
    }
    return (q.d[7 + page] & ~0xFFFu) + static_cast<std::uint32_t>(pos % 4096);
  }

  [[nodiscard]] bool buf_ok(const raw_qh &q, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
      const std::uint32_t a = buf_addr(q, i);
      if (!mem_->valid(a, 1)) {
        ++bad_dma;
        return false;
      }
      if (mem_->freed(a, 1)) {
        ++freed_buffer_accesses;
        return false;
      }
    }
    return true;
  }

  std::uint8_t &buf_byte(const raw_qh &q, std::size_t i) noexcept { return *mem_->at<std::uint8_t>(buf_addr(q, i)); }

  static void set_bytes(raw_qh &q, std::uint32_t total) noexcept {
    q.d[6] = (q.d[6] & ~(0x7FFFu << 16)) | (total << 16);
  }

  // Moves the position `n` bytes forward (C_Page / current offset).
  static void advance(raw_qh &q, std::size_t n) noexcept {
    const std::size_t pos = (q.d[7] & 0xFFFu) + n;
    std::uint32_t cpage = (q.d[6] >> 12) & 7u;
    cpage += static_cast<std::uint32_t>(pos / 4096);
    q.d[6] = (q.d[6] & ~(7u << 12)) | (cpage << 12);
    q.d[7] = (q.d[7] & ~0xFFFu) | static_cast<std::uint32_t>(pos % 4096);
  }

  // Writes the overlay back into the qTD it came from.
  void write_back(const raw_qh &q) noexcept {
    const std::uint32_t qa = q.d[3];
    if (!mem_->valid(qa, sizeof(raw_qtd)) || mem_->freed(qa, sizeof(raw_qtd))) {
      ++bad_dma;
      return;
    }
    raw_qtd *t = mem_->at<raw_qtd>(qa);
    t->d[2] = q.d[6];
    for (std::size_t k = 3; k < 8; ++k)
      t->d[k] = q.d[4 + k];
  }

  // The qTD finished normally. `short_packet`: follow the alternate pointer.
  void retire(raw_qh &q, bool short_packet) noexcept {
    q.d[6] &= ~0x80u;
    write_back(q);
    if ((q.d[6] & (1u << 15)) || short_packet)
      usbsts |= 1u; // USBINT: IOC or short packet
    q.d[4] = short_packet ? q.d[5] : q.d[4];
  }

  void halt(raw_qh &q, std::uint32_t error_bits) noexcept {
    q.d[6] = (q.d[6] & ~0x80u) | 0x40u | error_bits;
    write_back(q);
    usbsts |= 1u << 1; // USBERRINT
    if (q.d[6] & (1u << 15))
      usbsts |= 1u;
  }

  txn bus_error(raw_qh &q) noexcept {
    std::uint32_t cerr = (q.d[6] >> 10) & 3u;
    if (cerr > 0)
      --cerr;
    q.d[6] = (q.d[6] & ~(3u << 10)) | (cerr << 10) | 0x08u; // XactErr
    if (cerr == 0)
      halt(q, 0x08u);
    return txn::progress;
  }

  txn transaction(raw_qh &q) noexcept {
    const std::uint32_t ch = q.d[1];
    const unsigned addr = ch & 0x7F;
    const unsigned ep = (ch >> 8) & 0xF;
    const std::size_t mps = (ch >> 16) & 0x7FF;
    // The driver must describe a high-speed endpoint with the data toggle taken from the qTD.
    if (((ch >> 12) & 3) != 2 || !(ch & (1u << 14)))
      ++bad_dma;
    const std::uint32_t tok = q.d[6];
    const std::uint32_t pid = (tok >> 8) & 3;
    const bool dt = (tok >> 31) != 0;
    const std::size_t total = (tok >> 16) & 0x7FFF;

    if (!reachable() || !dev_->accepts(addr))
      return bus_error(q);

    reloco::array<std::uint8_t, 1024> buf{};
    if (pid == 2) { // SETUP
      if (total != 8 || !buf_ok(q, 8))
        return halt(q, 0x20), txn::stop;
      for (std::size_t i = 0; i < 8; ++i)
        buf[i] = buf_byte(q, i);
      (void)dev_->setup(reloco::span<const std::uint8_t>(buf.data(), 8), !dt);
      advance(q, 8);
      set_bytes(q, 0);
      retire(q, false);
      return txn::progress;
    }
    if (pid == 0) { // OUT
      const std::size_t n = total < mps ? total : mps;
      if (!buf_ok(q, n))
        return halt(q, 0x20), txn::stop;
      for (std::size_t i = 0; i < n; ++i)
        buf[i] = buf_byte(q, i);
      const usb_sim::reply r = dev_->out(ep, reloco::span<const std::uint8_t>(buf.data(), n), dt);
      if (r == usb_sim::reply::nak) {
        ++naks;
        return txn::nak;
      }
      if (r == usb_sim::reply::stall) {
        halt(q, 0);
        return txn::stop;
      }
      if (recycle && ep == 1) {
        for (std::size_t i = 0; i < n && capture_len < capture.size(); ++i)
          capture[capture_len++] = buf[i];
        dev_->loop_wr = 0;
        dev_->loop_rd = 0;
      }
      advance(q, n);
      set_bytes(q, static_cast<std::uint32_t>(total - n));
      q.d[6] ^= 1u << 31;
      q.d[6] = (q.d[6] & ~(3u << 10)) | (3u << 10);
      if (total == n)
        retire(q, false);
      return txn::progress;
    }
    // IN
    if (recycle && ep == 1 && dev_->loop_wr == dev_->loop_rd && feed_pos < capture_len) {
      std::size_t n = capture_len - feed_pos;
      n = n < 4096 ? n : 4096;
      for (std::size_t i = 0; i < n; ++i)
        dev_->loop[i] = capture[feed_pos + i];
      dev_->loop_rd = 0;
      dev_->loop_wr = n;
      feed_pos += n;
    }
    std::size_t n = 0;
    bool toggle = false;
    const usb_sim::reply r = dev_->in(ep, reloco::span<std::uint8_t>(buf.data(), mps), n, toggle);
    if (r == usb_sim::reply::nak) {
      ++naks;
      return txn::nak;
    }
    if (r == usb_sim::reply::stall) {
      halt(q, 0);
      return txn::stop;
    }
    if (toggle != dt) { // the controller drops a packet with the wrong toggle
      dev_->note_toggle_error();
      ++toggle_mismatches;
      return txn::nak;
    }
    if (n > total) {
      halt(q, 0x10u); // babble
      return txn::stop;
    }
    if (!buf_ok(q, n))
      return halt(q, 0x20), txn::stop;
    for (std::size_t i = 0; i < n; ++i)
      buf_byte(q, i) = buf[i];
    advance(q, n);
    set_bytes(q, static_cast<std::uint32_t>(total - n));
    q.d[6] ^= 1u << 31;
    q.d[6] = (q.d[6] & ~(3u << 10)) | (3u << 10);
    const bool short_packet = total > n && n < mps;
    if (total == n || short_packet)
      retire(q, short_packet);
    return txn::progress;
  }

  arena *mem_;
  usb_sim::device *dev_;
};

// ---------------------------------------------------------------------------------------------
// Env backed by the arena and the model.
// ---------------------------------------------------------------------------------------------

class test_env {
public:
  test_env(arena &mem, ehci_model &hw) noexcept : mem_(&mem), hw_(&hw) {}

  std::uint32_t read32(std::size_t off) noexcept { return hw_->read32(off); }
  void write32(std::size_t off, std::uint32_t v) noexcept { hw_->write32(off, v); }
  usb_dma_buffer dma_alloc(std::size_t size, std::size_t align) noexcept {
    return fail_alloc ? usb_dma_buffer{} : mem_->alloc(size, align);
  }
  void dma_free(const usb_dma_buffer &b) noexcept { mem_->release(b); }
  void barrier() noexcept {}
  reloco::task<void> delay_ms(unsigned ms) noexcept {
    delayed += ms;
    co_return;
  }

  bool fail_alloc = false;
  unsigned delayed = 0;

private:
  arena *mem_;
  ehci_model *hw_;
};

// A transfer whose result the tests can look at.
struct test_xfer : usb_transfer {
  test_xfer() noexcept {
    done_hook = [](void *ctx, usb_transfer &) noexcept { ++*static_cast<unsigned *>(ctx); };
    done_ctx = &completions;
  }
  [[nodiscard]] bool finished() const noexcept { return state_.load() == done; }
  [[nodiscard]] usb_completion result() const noexcept { return result_; }
  unsigned completions = 0;
};

usb_pipe make_pipe(std::uint8_t addr, std::uint8_t ep, usb_direction dir, usb_transfer_type type,
                   std::uint16_t mps) noexcept {
  usb_pipe p;
  p.address = addr;
  p.endpoint = ep;
  p.direction = dir;
  p.type = type;
  p.max_packet = mps;
  p.speed = usb_speed::high;
  return p;
}

void fill(test_xfer &x, const usb_pipe &p, void *data, std::size_t len) noexcept {
  x.pipe = p;
  x.data = data;
  x.length = len;
}

template <std::size_t N> class rig {
public:
  usb_sim::device dev;
  arena mem;
  ehci_model model{mem, dev};
  test_env env{mem, model};
  ehci_hcd<test_env, N> hcd{env};
  usb_host_controller_ref ref{hcd};

  rig() { dev.bulk_mps = 512; }

  void tick() noexcept {
    model.step();
    hcd.poll();
  }
  void ticks(int n) noexcept {
    for (int i = 0; i < n; ++i)
      tick();
  }
  bool wait(const test_xfer &x, int max_ticks = 400) noexcept {
    for (int i = 0; i < max_ticks && !x.finished(); ++i)
      tick();
    return x.finished();
  }
  template <typename T> reloco::result<T> run(reloco::task<T> t) {
    t.resume();
    for (int i = 0; i < 5000 && !t.done(); ++i)
      tick();
    EXPECT_TRUE(t.done());
    return t.take();
  }

  // Starts the controller, plugs a high-speed device into port 0 and resets the port.
  void bring_up() {
    ASSERT_TRUE(hcd.start().has_value());
    model.plug(speed_kind::high);
    auto st = hcd.port_status(0);
    ASSERT_TRUE(st.has_value() && st->connected);
    auto r = run(hcd.reset_port(0));
    ASSERT_TRUE(r.has_value());
  }

  void check_invariants() const {
    EXPECT_EQ(model.freed_qh_walks, 0u);
    EXPECT_EQ(model.freed_buffer_accesses, 0u);
    EXPECT_EQ(model.bad_links, 0u);
    EXPECT_EQ(model.bad_dma, 0u);
    EXPECT_EQ(dev.toggle_errors, 0u);
  }
};

class EhciTest : public ::testing::Test {
protected:
  void TearDown() override { r.check_invariants(); }

  usb_pipe bulk_out() const { return make_pipe(0, 1, usb_direction::out, usb_transfer_type::bulk, 512); }
  usb_pipe bulk_in() const { return make_pipe(0, 1, usb_direction::in, usb_transfer_type::bulk, 512); }
  usb_pipe int_in() const { return make_pipe(0, 2, usb_direction::in, usb_transfer_type::interrupt, 8); }
  usb_pipe ctl(std::uint16_t mps = 64) const {
    return make_pipe(0, 0, usb_direction::out, usb_transfer_type::control, mps);
  }

  // Runs a bulk OUT of `n` bytes from tx_ and waits for it.
  bool send(std::size_t n, usb_completion *c = nullptr) {
    test_xfer x;
    fill(x, bulk_out(), tx_.data(), n);
    if (!r.hcd.submit(x) || !r.wait(x))
      return false;
    if (c)
      *c = x.result();
    return x.result().ok();
  }

  // Control transfer helper; `data` is the data stage buffer.
  usb_completion control(const usb_setup_packet &s, reloco::span<std::uint8_t> data, std::uint16_t mps = 64) {
    test_xfer x;
    fill(x, ctl(mps), data.data(), data.size());
    x.setup = s;
    EXPECT_TRUE(r.hcd.submit(x).has_value());
    EXPECT_TRUE(r.wait(x));
    EXPECT_EQ(x.completions, 1u);
    return x.result();
  }

  void pattern(std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
      tx_[i] = static_cast<std::uint8_t>(i * 7 + 3);
  }

  rig<16> r;
  reloco::array<std::uint8_t, 65536> tx_{};
  reloco::array<std::uint8_t, 65536> rx_{};
};

class EhciSmallTest : public ::testing::Test {
protected:
  void TearDown() override { r.check_invariants(); }
  rig<2> r;
  reloco::array<std::uint8_t, 512> buf_{};
};

} // namespace

// ---------------------------------------------------------------------------------------------
// start() / stop()
// ---------------------------------------------------------------------------------------------

TEST_F(EhciTest, StartProgramsController) {
  ASSERT_TRUE(r.hcd.start().has_value());
  EXPECT_TRUE(r.hcd.started());
  EXPECT_EQ(r.model.hcreset_count, 1u);
  EXPECT_EQ(r.model.usbcmd & 0x31u, 0x31u); // RS | PSE | ASE
  EXPECT_EQ(r.model.configflag, 1u);
  EXPECT_EQ(r.model.ctrldssegment, 0u);
  EXPECT_EQ(r.model.periodiclistbase & 0xFFF, 0u);
  EXPECT_NE(r.model.periodiclistbase, 0u);
  EXPECT_EQ(r.model.asynclistaddr & 0x1F, 0u);
  EXPECT_NE(r.model.asynclistaddr, 0u);
  EXPECT_EQ(r.model.usbintr & 0x37u, 0x37u); // USBINT, USBERRINT, PCD, HSE, IAA
  for (const auto &p : r.model.ports)
    EXPECT_TRUE(p.pp && !p.owner);

  // Async list: one self-linked dummy head with the H bit; periodic list: every slot reaches a QH.
  const raw_qh &head = r.model.async_head();
  EXPECT_EQ(head.d[0] & ~0x1Fu, r.model.asynclistaddr);
  EXPECT_TRUE(head.d[1] & (1u << 15));
  EXPECT_EQ(r.model.async_ring_length(), 1u);
  for (std::size_t i = 0; i < 1024; ++i)
    ASSERT_EQ(r.model.frame_list_entry(i) & 7u, 2u); // QH, not terminated
  EXPECT_EQ(r.model.periodic_chain_length(), 1u);

  EXPECT_EQ(r.hcd.port_count(), 4u);
  EXPECT_EQ(r.hcd.companion_controllers(), 2u);
  EXPECT_EQ(r.hcd.ports_per_companion(), 2u);
  EXPECT_TRUE(r.hcd.addressing_64());
  EXPECT_EQ(r.hcd.eecp(), 0xA0u);
  EXPECT_EQ(r.hcd.start().error(), reloco::error::invalid_state);
}

TEST_F(EhciTest, StartFailsBoundedWhenResetNeverClears) {
  r.model.stuck_hcreset = true;
  auto st = r.hcd.start();
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error(), reloco::error::timed_out);
  EXPECT_FALSE(r.hcd.started());
  EXPECT_EQ(r.mem.allocs, 0u);
  EXPECT_FALSE(r.hcd.port_status(0).has_value());
}

TEST_F(EhciTest, StartReportsAllocationFailure) {
  r.env.fail_alloc = true;
  auto st = r.hcd.start();
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error(), reloco::error::allocation_failed);
  EXPECT_FALSE(r.hcd.started());
}

TEST_F(EhciTest, StopCompletesPendingAndReleasesMemory) {
  r.bring_up();
  test_xfer x;
  fill(x, bulk_in(), rx_.data(), 512);
  ASSERT_TRUE(r.hcd.submit(x).has_value());
  r.ticks(3);
  EXPECT_FALSE(x.finished());
  r.hcd.stop();
  ASSERT_TRUE(x.finished());
  EXPECT_EQ(x.completions, 1u);
  EXPECT_EQ(x.result().status, usb_status::cancelled);
  EXPECT_FALSE(r.model.running());
  EXPECT_EQ(r.model.configflag, 0u);
  EXPECT_EQ(r.mem.allocs, r.mem.frees);
  EXPECT_FALSE(r.hcd.submit(x).has_value());
}

TEST_F(EhciTest, HostSystemErrorFailsTransfersAndController) {
  r.bring_up();
  test_xfer x;
  fill(x, bulk_in(), rx_.data(), 512);
  ASSERT_TRUE(r.hcd.submit(x).has_value());
  r.model.inject_hse = true;
  r.tick();
  EXPECT_TRUE(r.hcd.failed());
  ASSERT_TRUE(x.finished());
  EXPECT_EQ(x.result().status, usb_status::bus_error);
  test_xfer y;
  fill(y, bulk_in(), rx_.data(), 512);
  EXPECT_EQ(r.hcd.submit(y).error(), reloco::error::io_error);
}

// ---------------------------------------------------------------------------------------------
// Ports
// ---------------------------------------------------------------------------------------------

TEST_F(EhciTest, PortConnectIsReportedOnceAndResetEnablesIt) {
  ASSERT_TRUE(r.hcd.start().has_value());
  auto st = r.hcd.port_status(0);
  ASSERT_TRUE(st.has_value());
  EXPECT_FALSE(st->connected);
  EXPECT_FALSE(st->changed);
  EXPECT_FALSE(r.hcd.port_status(9).has_value());

  r.model.plug(speed_kind::high);
  st = r.hcd.port_status(0);
  ASSERT_TRUE(st.has_value());
  EXPECT_TRUE(st->connected);
  EXPECT_FALSE(st->enabled);
  EXPECT_TRUE(st->changed);
  st = r.hcd.port_status(0);
  EXPECT_FALSE(st->changed);

  auto rr = r.run(r.hcd.reset_port(0));
  ASSERT_TRUE(rr.has_value());
  EXPECT_GE(r.env.delayed, 60u); // 50 ms reset + 10 ms recovery
  EXPECT_FALSE(r.model.ports[0].pr);
  st = r.hcd.port_status(0);
  EXPECT_TRUE(st->connected && st->enabled);
  EXPECT_FALSE(st->changed); // the reset is not a connect change
  EXPECT_EQ(st->speed, usb_speed::high);
}

TEST_F(EhciTest, PortChangeInterruptLatchesChangedFlag) {
  ASSERT_TRUE(r.hcd.start().has_value());
  r.model.plug(speed_kind::high);
  r.hcd.irq(); // PCD: the driver acknowledges CSC and remembers the change
  EXPECT_FALSE(r.model.ports[0].csc);
  auto st = r.hcd.port_status(0);
  ASSERT_TRUE(st.has_value());
  EXPECT_TRUE(st->connected && st->changed);
  EXPECT_FALSE(r.hcd.port_status(0).value().changed);

  r.model.unplug();
  st = r.hcd.port_status(0);
  EXPECT_FALSE(st->connected);
  EXPECT_TRUE(st->changed);
}

TEST_F(EhciTest, LowSpeedDeviceIsHandedToCompanionImmediately) {
  ASSERT_TRUE(r.hcd.start().has_value());
  r.model.plug(speed_kind::low);
  auto st = r.hcd.port_status(0);
  ASSERT_TRUE(st.has_value());
  EXPECT_FALSE(st->connected);
  EXPECT_TRUE(r.model.ports[0].owner);
  r.model.unplug();
  EXPECT_FALSE(r.model.ports[0].owner); // a disconnect gives the port back to EHCI
  r.model.plug(speed_kind::low);
  r.hcd.irq();
  EXPECT_TRUE(r.model.ports[0].owner);
}

TEST_F(EhciTest, FullSpeedDeviceIsReleasedAfterFailedReset) {
  ASSERT_TRUE(r.hcd.start().has_value());
  r.model.plug(speed_kind::full);
  auto st = r.hcd.port_status(0);
  ASSERT_TRUE(st.has_value());
  EXPECT_TRUE(st->connected); // looks like J state: only the reset tells full from high speed
  EXPECT_FALSE(r.model.ports[0].owner);
  auto rr = r.run(r.hcd.reset_port(0));
  ASSERT_FALSE(rr.has_value());
  EXPECT_EQ(rr.error(), reloco::error::unsupported_operation);
  EXPECT_TRUE(r.model.ports[0].owner);
  EXPECT_FALSE(r.hcd.port_status(0).value().connected);
}

TEST_F(EhciTest, ResetFailsBoundedWhenPortStaysInReset) {
  ASSERT_TRUE(r.hcd.start().has_value());
  r.model.plug(speed_kind::high);
  r.model.stuck_port_reset = true;
  auto rr = r.run(r.hcd.reset_port(0));
  ASSERT_FALSE(rr.has_value());
  EXPECT_EQ(rr.error(), reloco::error::timed_out);
}

// ---------------------------------------------------------------------------------------------
// Enumeration through the generic stack
// ---------------------------------------------------------------------------------------------

TEST_F(EhciTest, EnumerationThroughUsbHost) {
  ASSERT_TRUE(r.hcd.start().has_value());
  r.model.plug(speed_kind::high);
  usb::usb_host host{r.ref};
  reloco::array<std::uint8_t, 512> cfg{};
  usb::usb_device dev{r.ref, cfg.as_span()};
  auto rr = r.run(host.enumerate(0, dev));
  ASSERT_TRUE(rr.has_value());
  EXPECT_EQ(dev.address(), 1);
  EXPECT_EQ(r.dev.address, 1);
  EXPECT_EQ(r.dev.config, 1);
  EXPECT_EQ(dev.speed(), usb_speed::high);
  EXPECT_EQ(dev.descriptor().vendor_id, 0x1234);
  EXPECT_EQ(dev.descriptor().product_id, 0x5678);
  EXPECT_EQ(r.hcd.active_count(), 0u);

  r.model.recycle = true;
  const usb_pipe out = make_pipe(1, 1, usb_direction::out, usb_transfer_type::bulk, 512);
  const usb_pipe in = make_pipe(1, 1, usb_direction::in, usb_transfer_type::bulk, 512);
  pattern(700);
  test_xfer a;
  fill(a, out, tx_.data(), 700);
  ASSERT_TRUE(r.hcd.submit(a).has_value());
  ASSERT_TRUE(r.wait(a));
  test_xfer b;
  fill(b, in, rx_.data(), 700);
  ASSERT_TRUE(r.hcd.submit(b).has_value());
  ASSERT_TRUE(r.wait(b));
  EXPECT_EQ(b.result().actual, 700u);
  for (std::size_t i = 0; i < 700; ++i)
    ASSERT_EQ(rx_[i], tx_[i]);
}

// ---------------------------------------------------------------------------------------------
// Control transfers
// ---------------------------------------------------------------------------------------------

TEST_F(EhciTest, ControlInDataStageAndShortPacket) {
  r.bring_up();
  const usb_setup_packet get_dev{0x80, 6, 0x0100, 0, 64};
  const usb_completion c = control(get_dev, rx_.as_span().subspan(0, 64));
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(c.actual, 18u);
  EXPECT_EQ(rx_[0], 18);
  EXPECT_EQ(rx_[1], 1);
  EXPECT_EQ(r.hcd.active_count(), 0u);
}

TEST_F(EhciTest, ControlOutDataStage) {
  r.bring_up();
  reloco::array<std::uint8_t, 7> payload{9, 8, 7, 6, 5, 4, 3};
  const usb_setup_packet set_line{0x21, 0x20, 0, 0, 7};
  const usb_completion c = control(set_line, payload.as_span());
  ASSERT_TRUE(c.ok());
  ASSERT_EQ(r.dev.last_ctl_out_len, 7u);
  for (std::size_t i = 0; i < 7; ++i)
    EXPECT_EQ(r.dev.last_ctl_out[i], payload[i]);
}

TEST_F(EhciTest, ControlWithoutDataStage) {
  r.bring_up();
  const usb_completion c = control(usb_setup_packet{0x00, 9, 1, 0, 0}, {});
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(c.actual, 0u);
}

TEST_F(EhciTest, ControlStallIsReported) {
  r.bring_up();
  const usb_completion c = control(usb_setup_packet{0x80, 0x99, 0, 0, 0}, {});
  EXPECT_EQ(c.status, usb_status::stall);
  EXPECT_EQ(r.hcd.active_count(), 0u);
  EXPECT_TRUE(control(usb_setup_packet{0x00, 9, 1, 0, 0}, {}).ok());
}

// ---------------------------------------------------------------------------------------------
// Bulk transfers
// ---------------------------------------------------------------------------------------------

TEST_F(EhciTest, BulkLoopbackAllSizes) {
  r.bring_up();
  r.model.recycle = true; // lets the sim device loop back more than its small FIFO
  const reloco::array<std::size_t, 7> sizes{0, 1, 511, 512, 513, 1000, 30000};
  for (const std::size_t n : sizes) {
    SCOPED_TRACE(n);
    r.model.reset_capture();
    pattern(n);
    usb_completion c;
    ASSERT_TRUE(send(n, &c));
    EXPECT_EQ(c.actual, n);
    if (n == 0)
      continue; // the device never answers a zero-length IN
    for (std::size_t i = 0; i < n; ++i)
      rx_[i] = 0xEE;
    test_xfer x;
    fill(x, bulk_in(), rx_.data(), n);
    ASSERT_TRUE(r.hcd.submit(x).has_value());
    ASSERT_TRUE(r.wait(x));
    ASSERT_TRUE(x.result().ok());
    EXPECT_EQ(x.result().actual, n);
    for (std::size_t i = 0; i < n; ++i)
      ASSERT_EQ(rx_[i], tx_[i]) << "byte " << i;
  }
  EXPECT_EQ(r.hcd.active_count(), 0u);
  EXPECT_EQ(r.model.toggle_mismatches, 0u);
}

TEST_F(EhciTest, BulkTooLargeForQtdPoolIsRejected) {
  r.bring_up();
  test_xfer x;
  fill(x, bulk_out(), tx_.data(), 20480 * 16 + 1);
  EXPECT_FALSE(r.hcd.submit(x).has_value());
  EXPECT_EQ(r.hcd.active_count(), 0u);
}

TEST_F(EhciTest, BulkInNakThenDataOnALaterStep) {
  r.bring_up();
  test_xfer in;
  fill(in, bulk_in(), rx_.data(), 512);
  ASSERT_TRUE(r.hcd.submit(in).has_value());
  r.ticks(5);
  EXPECT_FALSE(in.finished());
  EXPECT_GT(r.model.naks, 0u);
  EXPECT_EQ(r.hcd.active_count(), 1u);

  pattern(100);
  ASSERT_TRUE(send(100));
  ASSERT_TRUE(r.wait(in));
  EXPECT_EQ(in.completions, 1u);
  ASSERT_TRUE(in.result().ok());
  EXPECT_EQ(in.result().actual, 100u);
  for (std::size_t i = 0; i < 100; ++i)
    ASSERT_EQ(rx_[i], tx_[i]);
}

TEST_F(EhciTest, BulkInShortPackets) {
  r.bring_up();
  r.model.recycle = true;
  const reloco::array<std::size_t, 4> have{700, 5, 1000, 30000};
  const reloco::array<std::size_t, 4> ask{2000, 512, 4096, 50000};
  for (std::size_t k = 0; k < have.size(); ++k) {
    SCOPED_TRACE(have[k]);
    r.model.reset_capture();
    pattern(have[k]);
    ASSERT_TRUE(send(have[k]));
    test_xfer x;
    fill(x, bulk_in(), rx_.data(), ask[k]);
    ASSERT_TRUE(r.hcd.submit(x).has_value());
    ASSERT_TRUE(r.wait(x));
    ASSERT_TRUE(x.result().ok());
    EXPECT_EQ(x.result().actual, have[k]);
    for (std::size_t i = 0; i < have[k]; ++i)
      ASSERT_EQ(rx_[i], tx_[i]);
  }
}

TEST_F(EhciTest, BulkStallOnHaltedEndpoint) {
  r.bring_up();
  test_xfer in;
  fill(in, make_pipe(0, 3, usb_direction::in, usb_transfer_type::bulk, 512), rx_.data(), 64);
  ASSERT_TRUE(r.hcd.submit(in).has_value());
  ASSERT_TRUE(r.wait(in));
  EXPECT_EQ(in.result().status, usb_status::stall);
  test_xfer out;
  fill(out, make_pipe(0, 3, usb_direction::out, usb_transfer_type::bulk, 512), tx_.data(), 64);
  ASSERT_TRUE(r.hcd.submit(out).has_value());
  ASSERT_TRUE(r.wait(out));
  EXPECT_EQ(out.result().status, usb_status::stall);
  EXPECT_EQ(r.hcd.active_count(), 0u);
}

// ---------------------------------------------------------------------------------------------
// Data toggles
// ---------------------------------------------------------------------------------------------

TEST_F(EhciTest, ResetDataToggleRestartsAtData0) {
  r.bring_up();
  pattern(1);
  ASSERT_TRUE(send(1)); // DATA0; the next one would be DATA1
  r.dev.reset_toggles(); // the device side restarts at DATA0
  r.hcd.reset_data_toggle(bulk_out());
  ASSERT_TRUE(send(1));
  ASSERT_TRUE(send(1));
  EXPECT_EQ(r.dev.toggle_errors, 0u);
}

TEST_F(EhciTest, ClearHaltAndSetConfigurationControlsResetToggles) {
  r.bring_up();
  pattern(1);
  ASSERT_TRUE(send(1));
  r.dev.reset_toggles();
  ASSERT_TRUE(control(usb_setup_packet{0x02, 1, 0, 0x01, 0}, {}).ok()); // CLEAR_FEATURE(HALT), endpoint 1 OUT
  ASSERT_TRUE(send(1));
  EXPECT_EQ(r.dev.toggle_errors, 0u);

  r.dev.reset_toggles();
  ASSERT_TRUE(control(usb_setup_packet{0x00, 9, 1, 0, 0}, {}).ok());
  ASSERT_TRUE(send(1));
  EXPECT_EQ(r.dev.toggle_errors, 0u);
}

// ---------------------------------------------------------------------------------------------
// Interrupt transfers
// ---------------------------------------------------------------------------------------------

TEST_F(EhciTest, InterruptInThroughPeriodicSchedule) {
  r.bring_up();
  r.dev.arm_interrupt();
  test_xfer x;
  fill(x, int_in(), rx_.data(), 8);
  ASSERT_TRUE(r.hcd.submit(x).has_value());
  EXPECT_EQ(r.model.periodic_chain_length(), 2u); // dummy + the interrupt QH
  ASSERT_TRUE(r.wait(x));
  ASSERT_TRUE(x.result().ok());
  EXPECT_EQ(x.result().actual, 8u);
  for (std::size_t i = 0; i < 8; ++i)
    EXPECT_EQ(rx_[i], i + 1);
  EXPECT_EQ(x.completions, 1u);
  EXPECT_EQ(r.model.periodic_chain_length(), 1u);
  r.ticks(2);
  EXPECT_EQ(r.hcd.zombie_count(), 0u);

  test_xfer y;
  fill(y, int_in(), rx_.data(), 8);
  ASSERT_TRUE(r.hcd.submit(y).has_value());
  r.ticks(5);
  EXPECT_FALSE(y.finished());
  r.dev.arm_interrupt();
  ASSERT_TRUE(r.wait(y));
  EXPECT_TRUE(y.result().ok());
  EXPECT_EQ(r.dev.toggle_errors, 0u);
}

// ---------------------------------------------------------------------------------------------
// Cancel and unlink safety
// ---------------------------------------------------------------------------------------------

TEST_F(EhciTest, CancelAsyncWaitsForAsyncAdvanceBeforeFreeing) {
  r.bring_up();
  test_xfer x;
  fill(x, bulk_in(), rx_.data(), 512);
  ASSERT_TRUE(r.hcd.submit(x).has_value());
  r.ticks(3);
  EXPECT_EQ(r.model.async_ring_length(), 2u);
  const unsigned frees_before = r.mem.frees;

  r.hcd.cancel(x);
  EXPECT_EQ(r.model.async_ring_length(), 1u);
  EXPECT_EQ(r.hcd.active_count(), 0u);
  EXPECT_EQ(r.hcd.zombie_count(), 1u);
  EXPECT_EQ(r.model.iaad_rings, 1u);
  EXPECT_EQ(r.mem.frees, frees_before);
  r.hcd.poll();
  EXPECT_EQ(r.hcd.zombie_count(), 1u);

  r.tick(); // the model raises Interrupt on Async Advance
  EXPECT_EQ(r.hcd.zombie_count(), 0u);
  EXPECT_GT(r.mem.frees, frees_before);
  r.ticks(5);
  EXPECT_FALSE(x.finished());
  EXPECT_EQ(x.completions, 0u);

  pattern(10);
  test_xfer y;
  fill(y, bulk_in(), rx_.data(), 512);
  ASSERT_TRUE(r.hcd.submit(y).has_value());
  ASSERT_TRUE(send(10));
  ASSERT_TRUE(r.wait(y));
  EXPECT_EQ(y.result().actual, 10u);
}

TEST_F(EhciTest, CancelsOverlappingTheDoorbellAreFreedInTwoRounds) {
  r.bring_up();
  test_xfer a;
  fill(a, bulk_in(), rx_.data(), 512);
  ASSERT_TRUE(r.hcd.submit(a).has_value());
  r.hcd.cancel(a);
  test_xfer b;
  fill(b, bulk_in(), rx_.data(), 512);
  ASSERT_TRUE(r.hcd.submit(b).has_value());
  r.hcd.cancel(b);
  EXPECT_EQ(r.hcd.zombie_count(), 2u);
  EXPECT_EQ(r.model.iaad_rings, 1u);
  r.tick();
  EXPECT_EQ(r.hcd.zombie_count(), 1u);
  EXPECT_EQ(r.model.iaad_rings, 2u);
  r.tick();
  EXPECT_EQ(r.hcd.zombie_count(), 0u);
  r.ticks(3);
}

TEST_F(EhciTest, CancelPeriodicWaitsOneFrame) {
  r.bring_up();
  test_xfer x;
  fill(x, int_in(), rx_.data(), 8);
  ASSERT_TRUE(r.hcd.submit(x).has_value());
  r.ticks(3);
  EXPECT_EQ(r.model.periodic_chain_length(), 2u);
  r.hcd.cancel(x);
  EXPECT_EQ(r.model.periodic_chain_length(), 1u);
  EXPECT_EQ(r.hcd.zombie_count(), 1u);
  r.hcd.poll(); // same frame
  EXPECT_EQ(r.hcd.zombie_count(), 1u);
  r.tick(); // frame number advanced
  EXPECT_EQ(r.hcd.zombie_count(), 0u);
  r.ticks(4);
  EXPECT_EQ(x.completions, 0u);
}

TEST_F(EhciTest, CancelAfterCompletionIsHarmless) {
  r.bring_up();
  pattern(4);
  ASSERT_TRUE(send(4));
  test_xfer x;
  fill(x, bulk_in(), rx_.data(), 4);
  ASSERT_TRUE(r.hcd.submit(x).has_value());
  ASSERT_TRUE(r.wait(x));
  r.hcd.cancel(x);
  EXPECT_EQ(x.completions, 1u);
}

TEST_F(EhciTest, UnplugWithPendingTransferFailsItWithABusError) {
  r.bring_up();
  test_xfer x;
  fill(x, bulk_in(), rx_.data(), 512);
  ASSERT_TRUE(r.hcd.submit(x).has_value());
  r.ticks(2);
  r.model.unplug();
  ASSERT_TRUE(r.wait(x, 20)); // three retries, then the queue halts with XactErr
  EXPECT_EQ(x.completions, 1u);
  EXPECT_EQ(x.result().status, usb_status::bus_error);
  auto st = r.hcd.port_status(0);
  ASSERT_TRUE(st.has_value());
  EXPECT_FALSE(st->connected);
  EXPECT_TRUE(st->changed);
  r.ticks(3);
  EXPECT_EQ(r.hcd.zombie_count(), 0u);
}

TEST_F(EhciTest, UnplugThenCancelFreesEverything) {
  r.bring_up();
  test_xfer x;
  fill(x, int_in(), rx_.data(), 8);
  test_xfer y;
  fill(y, bulk_in(), rx_.data(), 512);
  ASSERT_TRUE(r.hcd.submit(x).has_value());
  ASSERT_TRUE(r.hcd.submit(y).has_value());
  r.model.unplug();
  r.hcd.cancel(x);
  r.hcd.cancel(y);
  r.ticks(4);
  EXPECT_EQ(r.hcd.zombie_count(), 0u);
  EXPECT_EQ(r.hcd.active_count(), 0u);
  EXPECT_EQ(x.completions + y.completions, 0u);
}

// ---------------------------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------------------------

TEST_F(EhciSmallTest, SubmitBeyondMaxTransfersFailsCleanly) {
  ASSERT_TRUE(r.hcd.start().has_value());
  r.model.plug(speed_kind::high);
  ASSERT_TRUE(r.run(r.hcd.reset_port(0)).has_value());
  test_xfer a;
  fill(a, make_pipe(0, 1, usb_direction::in, usb_transfer_type::bulk, 512), buf_.data(), 512);
  test_xfer b;
  fill(b, make_pipe(0, 2, usb_direction::in, usb_transfer_type::interrupt, 8), buf_.data(), 8);
  test_xfer c;
  fill(c, make_pipe(0, 5, usb_direction::in, usb_transfer_type::bulk, 512), buf_.data(), 512);
  ASSERT_TRUE(r.hcd.submit(a).has_value());
  ASSERT_TRUE(r.hcd.submit(b).has_value());
  auto third = r.hcd.submit(c);
  ASSERT_FALSE(third.has_value());
  EXPECT_EQ(third.error(), reloco::error::try_again);
  EXPECT_EQ(r.hcd.active_count(), 2u);

  test_xfer dup;
  fill(dup, make_pipe(0, 1, usb_direction::in, usb_transfer_type::bulk, 512), buf_.data(), 512);
  r.hcd.cancel(b);
  EXPECT_FALSE(r.hcd.submit(dup).has_value()); // same endpoint as `a`: busy

  EXPECT_EQ(r.hcd.zombie_count(), 1u);
  ASSERT_TRUE(r.hcd.submit(c).has_value());
  r.hcd.cancel(a);
  r.hcd.cancel(c);
  r.ticks(4);
  EXPECT_EQ(r.hcd.zombie_count(), 0u);
}

TEST_F(EhciTest, SubmitRejectsUnsupportedRequests) {
  r.bring_up();
  test_xfer x;
  fill(x, make_pipe(0, 1, usb_direction::in, usb_transfer_type::isochronous, 512), rx_.data(), 8);
  EXPECT_EQ(r.hcd.submit(x).error(), reloco::error::unsupported_operation);
  usb_pipe full = bulk_in();
  full.speed = usb_speed::full;
  fill(x, full, rx_.data(), 8);
  EXPECT_EQ(r.hcd.submit(x).error(), reloco::error::unsupported_operation);
  usb_pipe bad = bulk_in();
  bad.address = 200;
  fill(x, bad, rx_.data(), 8);
  EXPECT_EQ(r.hcd.submit(x).error(), reloco::error::invalid_argument);
  EXPECT_EQ(r.hcd.active_count(), 0u);
}

// ---------------------------------------------------------------------------------------------
// bootldr::usb_stack hot plug on top of the EHCI driver
// ---------------------------------------------------------------------------------------------

namespace {

class EhciStackTest : public ::testing::Test {
public:
  EhciStackTest() : stack(sched, r.ref, config()) {
    sched.set_clock(&clock, this);
    EXPECT_TRUE(sched.add_poller(&service, this).has_value());
    stack.set_event_handler(&on_event, this);
    EXPECT_TRUE(r.hcd.start().has_value());
  }
  void TearDown() override { r.check_invariants(); }

  static bootldr::usb_stack_config config() {
    bootldr::usb_stack_config c;
    c.poll_ms = 10;
    c.debounce_ms = 20;
    c.max_config_bytes = 1024;
    return c;
  }
  static std::uint64_t clock(void *c) noexcept { return static_cast<EhciStackTest *>(c)->now; }
  static void service(void *c) noexcept {
    auto *t = static_cast<EhciStackTest *>(c);
    t->now += 10;
    t->r.tick();
  }
  static void on_event(void *c, bootldr::usb_event_kind k, unsigned, bootldr::usb_attached_device *,
                       const reloco::result<void> &) noexcept {
    auto *t = static_cast<EhciStackTest *>(c);
    switch (k) {
    case bootldr::usb_event_kind::attached:
      ++t->attached;
      break;
    case bootldr::usb_event_kind::unclaimed:
      ++t->unclaimed;
      break;
    case bootldr::usb_event_kind::detached:
      ++t->detached;
      break;
    case bootldr::usb_event_kind::enumeration_failed:
      ++t->failed;
      break;
    }
  }
  void rounds(int n) {
    for (int i = 0; i < n; ++i)
      sched.run_once();
  }

  rig<16> r;
  std::uint64_t now = 0;
  bootldr::scheduler sched;
  bootldr::usb_stack stack;
  unsigned attached = 0;
  unsigned unclaimed = 0;
  unsigned detached = 0;
  unsigned failed = 0;
  unsigned started = 0;
  unsigned cleaned = 0;
  usb_status last_status = usb_status::ok;
};

bool match_any(void *, const usb::usb_device &) noexcept { return true; }

// Parks on a bulk IN the device never answers; unplugging must fail it.
reloco::task<void> parked_driver(void *ctx, bootldr::usb_stack &, bootldr::usb_device_ptr dev) noexcept {
  auto *t = static_cast<EhciStackTest *>(ctx);
  ++t->started;
  const usb_pipe in{dev->device().address(), 1, usb_direction::in, usb_transfer_type::bulk, 512, usb_speed::high, 0};
  reloco::array<std::uint8_t, 512> buf{};
  const usb_completion c = co_await dev->device().controller().in(in, reloco::span<std::uint8_t>(buf));
  t->last_status = c.status;
  co_await dev->gone_event().wait();
  ++t->cleaned;
}

} // namespace

TEST_F(EhciStackTest, HotPlugEnumeratesAndUnplugCancelsTransfers) {
  ASSERT_TRUE(stack.add_driver({&match_any, &parked_driver, nullptr, this}).has_value());
  ASSERT_TRUE(stack.start().has_value());
  rounds(5);
  EXPECT_EQ(started, 0u);

  r.model.plug(speed_kind::high);
  rounds(30);
  EXPECT_EQ(attached, 1u);
  EXPECT_EQ(started, 1u);
  EXPECT_EQ(r.hcd.active_count(), 1u); // the driver is parked on its IN transfer
  EXPECT_EQ(r.dev.address, 1);

  r.model.unplug();
  rounds(40);
  EXPECT_EQ(detached, 1u);
  // The controller errors the transfer (bus error) before the stack gets to cancel it; either is a failure.
  EXPECT_TRUE(last_status == usb_status::disconnected || last_status == usb_status::bus_error);
  EXPECT_EQ(cleaned, 1u);
  EXPECT_EQ(r.hcd.active_count(), 0u);
  EXPECT_EQ(r.hcd.zombie_count(), 0u);

  // The next device reuses address 1 with a clean toggle state.
  r.model.plug(speed_kind::high);
  rounds(30);
  EXPECT_EQ(attached, 2u);
  EXPECT_EQ(started, 2u);
  EXPECT_EQ(r.dev.address, 1);
}

TEST_F(EhciStackTest, LowSpeedDeviceStaysWithTheCompanion) {
  ASSERT_TRUE(stack.start().has_value());
  r.model.plug(speed_kind::low);
  rounds(20);
  EXPECT_EQ(unclaimed + attached + failed, 0u);
  EXPECT_TRUE(r.model.ports[0].owner);
}
