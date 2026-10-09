// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

// Shared by the Linux USB demos: a fake USB host controller with one root port and two simulated
// devices whose "wire" side is a Linux file descriptor:
//
//   ecm_device  CDC-ECM Ethernet adapter. Frames the board sends become frames written to a TAP
//               interface; frames read from the TAP are delivered to the board.
//   acm_device  CDC-ACM serial port. Bytes the board sends are written to a pty master; bytes typed
//               into the pty slave (minicom, screen, pppd ...) are delivered to the board.
//
// A real controller driver implements the same hooks (usb_host_traits) on its transfer descriptors and
// completes them from an interrupt; here `hcd::pump()` plays that role and is called once per
// scheduler round. For controlled test/CI/development setups only.

#include <reloco/span.hpp>
#include <structo/hw/usb_host_controller_ref.hpp>
#include <structo/usb/usb_defs.hpp>

#if RELOCO_HAS_COROUTINES

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <reloco/array.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/span.hpp>
#include <reloco/string_view.hpp>


namespace usbfake {

using namespace structo;

inline std::uint64_t now_ms(void *) noexcept {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000u + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000u;
}

// Waits until `fd` is readable or `ms` passed: the scheduler idle hook, so the demo does not spin.
inline void wait_readable(void *fd_ptr) noexcept {
  pollfd p{*static_cast<int *>(fd_ptr), POLLIN, 0};
  (void)::poll(&p, 1, 5);
}

// The only place a raw pointer is handed to the OS: read()/write() on a byte span.
inline ::ssize_t fd_read(int fd, reloco::span<std::uint8_t> buf) noexcept { return ::read(fd, buf.data(), buf.size()); }
inline void fd_write(int fd, reloco::span<const std::uint8_t> buf) noexcept { (void)!::write(fd, buf.data(), buf.size()); }

// The data stage buffer of a transfer as a span.
inline reloco::span<std::uint8_t> transfer_buf(const hw::usb_transfer &t) noexcept {
  return {static_cast<std::uint8_t *>(t.data), t.length};
}

// Copies `src` into `dst` as a NUL-terminated string, truncating if needed.
inline void copy_cstr(reloco::span<char> dst, reloco::string_view src) noexcept {
  if (dst.empty())
    return;
  std::size_t n = 0;
  for (const char c : src) {
    if (n + 1 >= dst.size())
      break;
    dst[n++] = c;
  }
  dst[n] = '\0';
}

// Opens a TAP interface (needs CAP_NET_ADMIN, or an interface pre-created for this user).
inline int open_tap(const char *name) {
  const int fd = ::open("/dev/net/tun", O_RDWR | O_NONBLOCK);
  if (fd < 0)
    return -1;
  ifreq req{};
  req.ifr_flags = IFF_TAP | IFF_NO_PI;
  copy_cstr(reloco::span<char>{req.ifr_name}, name);
  if (::ioctl(fd, TUNSETIFF, &req) < 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

// Opens a pty master in raw mode and returns the path of its slave in `slave`.
inline int open_pty(reloco::span<char> slave) {
  const int fd = ::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0)
    return -1;
  if (::grantpt(fd) != 0 || ::unlockpt(fd) != 0) {
    ::close(fd);
    return -1;
  }
  const char *name = ::ptsname(fd);
  copy_cstr(slave, name ? name : "?");
  termios t{};
  if (::tcgetattr(fd, &t) == 0) {
    ::cfmakeraw(&t);
    (void)::tcsetattr(fd, TCSANOW, &t);
  }
  return fd;
}

// Outcome of one transfer attempt: `nak` means "the device has nothing yet", the controller retries later.
struct reply {
  bool nak = false;
  hw::usb_status status = hw::usb_status::ok;
  std::size_t actual = 0;
};

// A simulated device: standard control requests (descriptors, address, configuration ...) are handled
// here, subclasses add their class requests and bulk endpoints.
class device {
public:
  virtual ~device() = default;

  // Called when the device is (re)plugged: it starts unaddressed and unconfigured again.
  void on_plug() noexcept {
    address_ = 0;
    config_ = 0;
  }

  [[nodiscard]] reply process(hw::usb_transfer &t) {
    return t.pipe.type == hw::usb_transfer_type::control ? control(t) : bulk(t);
  }

protected:
  device(std::uint16_t vid, std::uint16_t pid) noexcept {
    dev_desc_ = {{18,
                                1,
                                0x00,
                                0x02,
                                0,
                                0,
                                0,
                                64,
                                static_cast<std::uint8_t>(vid),
                                static_cast<std::uint8_t>(vid >> 8),
                                static_cast<std::uint8_t>(pid),
                                static_cast<std::uint8_t>(pid >> 8),
                                0x00,
                                0x01,
                                1,
                                2,
                                3,
                                1}};
    
    set_string(1, "structo");
    set_string(3, "0001");
  }

  // Strings are stored as UTF-16LE string descriptors (ASCII only here).
  void set_string(std::size_t index, const char *ascii) noexcept {
    auto &s = strings_[index];
    std::size_t n = 2;
    for (const reloco::string_view text{ascii}; const char c : text) {
      if (c == '\0' || n + 2 > s.size())
        break;
      s[n++] = static_cast<std::uint8_t>(c);
      s[n++] = 0;
    }
    s[0] = static_cast<std::uint8_t>(n);
    s[1] = 3;
  }

  template <typename... Bytes> void add_config(Bytes... bytes) noexcept {
    ((cfg_[cfg_len_++] = static_cast<std::uint8_t>(bytes)), ...);
    cfg_[2] = static_cast<std::uint8_t>(cfg_len_); // wTotalLength
  }

  void begin_config(std::uint8_t interfaces) noexcept {
    cfg_len_ = 0;
    add_config(9, 2, 0, 0, interfaces, 1, 0, 0x80, 50);
  }

  virtual reply class_request(hw::usb_transfer &) { return {false, hw::usb_status::stall, 0}; }
  virtual reply bulk(hw::usb_transfer &) { return {false, hw::usb_status::stall, 0}; }

private:
  reply control(hw::usb_transfer &t) {
    const hw::usb_setup_packet &s = t.setup;
    const auto buf = transfer_buf(t);
    auto send = [&](reloco::span<const std::uint8_t> src) {
      std::size_t n = src.size() < s.length ? src.size() : s.length;
      n = n < t.length ? n : t.length;
      std::copy_n(src.begin(), n, buf.begin());
      return reply{false, hw::usb_status::ok, n};
    };
    if (s.request_type == 0x80 && s.request == usb::request::get_descriptor) {
      const std::uint8_t type = static_cast<std::uint8_t>(s.value >> 8);
      const std::uint8_t idx = static_cast<std::uint8_t>(s.value);
      if (type == usb::descriptor_type::device)
        return send(dev_desc_);
      if (type == usb::descriptor_type::configuration)
        return send(reloco::span<const std::uint8_t>{cfg_}.first(cfg_len_));
      if (type == usb::descriptor_type::string && idx < 6 && strings_[idx][0] != 0)
        return send(reloco::span<const std::uint8_t>{strings_[idx]}.first(strings_[idx][0]));
      return {false, hw::usb_status::stall, 0};
    }
    if (s.request_type == 0x00 && s.request == usb::request::set_address) {
      address_ = static_cast<std::uint8_t>(s.value);
      return {};
    }
    if (s.request_type == 0x00 && s.request == usb::request::set_configuration) {
      config_ = static_cast<std::uint8_t>(s.value);
      return {};
    }
    if ((s.request_type == 0x01 && s.request == usb::request::set_interface) ||
        (s.request_type == 0x02 && s.request == usb::request::clear_feature))
      return {};
    return class_request(t);
  }

  reloco::array<std::uint8_t, 18> dev_desc_{};
  reloco::array<std::uint8_t, 128> cfg_{};
  std::size_t cfg_len_ = 0;
  reloco::array<reloco::array<std::uint8_t, 40>, 6> strings_{}; // by string index; [0] == 0 means "not present"
  std::uint8_t address_ = 0;
  std::uint8_t config_ = 0;
};

// CDC-ECM adapter whose Ethernet side is a TAP fd. `mac_hex` is the iMACAddress string: 12 hex digits.
class ecm_device final : public device {
public:
  using frame_log_fn = void (*)(const char *dir, reloco::span<const std::uint8_t> frame);

  ecm_device(int tap_fd, const char *mac_hex, frame_log_fn log = nullptr) noexcept
      : device(0x1d6b, 0x0102), fd_(tap_fd), log_(log) {
    set_string(2, "structo fake USB Ethernet");
    set_string(4, mac_hex);
    begin_config(2);
    add_config(9, 4, 0, 0, 1, 2, 6, 0, 0);                          // comm interface, ECM
    add_config(13, 0x24, 0x0F, 4, 0, 0, 0, 0, 0xDC, 0x05, 0, 0, 0); // Ethernet descriptor: MAC string 4, MTU 1500
    add_config(7, 5, 0x81, 3, 16, 0, 16);                           // notification endpoint
    add_config(9, 4, 1, 0, 0, 0x0A, 0, 0, 0);                       // data interface, alt 0: no endpoints
    add_config(9, 4, 1, 1, 2, 0x0A, 0, 0, 0);                       // data interface, alt 1: bulk pair
    add_config(7, 5, 0x02, 2, 64, 0, 0);                            // bulk OUT (host -> device)
    add_config(7, 5, 0x83, 2, 64, 0, 0);                            // bulk IN (device -> host)
  }

private:
  reply class_request(hw::usb_transfer &t) override {
    if (t.setup.request == 0x43) // SET_ETHERNET_PACKET_FILTER
      return {};
    return {false, hw::usb_status::stall, 0};
  }

  // OUT: the board sends one frame -> write it to the TAP. IN: a frame from the TAP -> the board.
  reply bulk(hw::usb_transfer &t) override {
    const auto buf = transfer_buf(t);
    if (t.pipe.endpoint == 2 && !t.is_in()) {
      if (log_)
        log_("tx", buf);
      fd_write(fd_, buf);
      return {false, hw::usb_status::ok, t.length};
    }
    if (t.pipe.endpoint == 3 && t.is_in()) {
      const ssize_t n = fd_read(fd_, buf);
      if (n <= 0)
        return {true, hw::usb_status::ok, 0};
      if (log_)
        log_("rx", buf.first(static_cast<std::size_t>(n)));
      return {false, hw::usb_status::ok, static_cast<std::size_t>(n)};
    }
    return {false, hw::usb_status::stall, 0};
  }

  int fd_;
  frame_log_fn log_;
};

// CDC-ACM serial port whose UART side is a pty master fd.
class acm_device final : public device {
public:
  explicit acm_device(int pty_fd) noexcept : device(0x1d6b, 0x0104), fd_(pty_fd) {
    set_string(2, "structo fake USB Serial");
    begin_config(2);
    add_config(9, 4, 0, 0, 1, 2, 2, 1, 0);    // comm interface, ACM
    add_config(5, 0x24, 0, 0x10, 0x01);       // CDC header functional descriptor
    add_config(7, 5, 0x83, 3, 8, 0, 16);      // notification endpoint
    add_config(9, 4, 1, 0, 2, 0x0A, 0, 0, 0); // data interface
    add_config(7, 5, 0x01, 2, 64, 0, 0);      // bulk OUT (host -> device)
    add_config(7, 5, 0x82, 2, 64, 0, 0);      // bulk IN (device -> host)
  }

private:
  reply class_request(hw::usb_transfer &t) override {
    if (t.setup.request == 0x20) // SET_LINE_CODING: data stage already carries the 7 bytes
      return {false, hw::usb_status::ok, t.length};
    if (t.setup.request == 0x22) // SET_CONTROL_LINE_STATE (DTR/RTS)
      return {};
    return {false, hw::usb_status::stall, 0};
  }

  // OUT: serial bytes from the board -> pty (dropped while nobody has the slave open).
  // IN: bytes typed on the pty -> the board.
  reply bulk(hw::usb_transfer &t) override {
    const auto buf = transfer_buf(t);
    if (t.pipe.endpoint == 1 && !t.is_in()) {
      fd_write(fd_, buf);
      return {false, hw::usb_status::ok, t.length};
    }
    if (t.pipe.endpoint == 2 && t.is_in()) {
      const ssize_t n = fd_read(fd_, buf);
      if (n <= 0)
        return {true, hw::usb_status::ok, 0};
      return {false, hw::usb_status::ok, static_cast<std::size_t>(n)};
    }
    return {false, hw::usb_status::stall, 0};
  }

  int fd_;
};

// Fake host controller: one root port, one pluggable device. Transfers queue up and are completed by pump().
class hcd {
public:
  static constexpr std::size_t slots = 8;

  void plug(device &d) noexcept {
    dev_ = &d;
    d.on_plug();
    reset_done_ = false;
    changed_ = true;
  }
  void unplug() noexcept {
    dev_ = nullptr;
    reset_done_ = false;
    changed_ = true;
  }
  [[nodiscard]] bool plugged() const noexcept { return dev_ != nullptr; }

  // The "interrupt handler": completes what the device can answer now. NAKed transfers stay queued.
  void pump() noexcept {
    for (std::size_t i = 0; i < slots && dev_; ++i) {
      hw::usb_transfer *t = pending_[i];
      if (!t)
        continue;
      const reply r = dev_->process(*t);
      if (r.nak)
        continue;
      pending_[i] = nullptr;
      t->complete(r.status, r.actual); // resumes the waiting coroutine
    }
  }

  device *dev_ = nullptr;
  bool reset_done_ = false;
  bool changed_ = false;
  reloco::array<hw::usb_transfer *, slots> pending_{};
};

} // namespace usbfake

template <> struct structo::hw::usb_host_traits<usbfake::hcd> {
  static unsigned port_count(usbfake::hcd &) noexcept { return 1; }
  static reloco::result<usb_port_status> port_status(usbfake::hcd &h, unsigned) noexcept {
    usb_port_status s;
    s.connected = h.dev_ != nullptr;
    s.enabled = h.reset_done_;
    s.speed = usb_speed::high;
    s.changed = h.changed_;
    h.changed_ = false; // reading clears the change flag
    return s;
  }
  static reloco::task<void> reset_port(usbfake::hcd &h, unsigned) noexcept {
    h.reset_done_ = true;
    co_return;
  }
  static reloco::result<void> submit(usbfake::hcd &h, usb_transfer &t) noexcept {
    if (!h.dev_)
      return reloco::unexpected(reloco::error::not_found);
    for (auto &slot : h.pending_)
      if (!slot) {
        slot = &t;
        return {};
      }
    return reloco::unexpected(reloco::error::busy);
  }
  static void cancel(usbfake::hcd &h, usb_transfer &t) noexcept {
    for (auto &slot : h.pending_)
      if (slot == &t)
        slot = nullptr;
  }
  static void reset_data_toggle(usbfake::hcd &, const usb_pipe &) noexcept {}
};


#endif // RELOCO_HAS_COROUTINES
