// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

// Linux demo of the bootloader network stack: a "board" that talks SLIP or PPP over a
// pseudo-terminal (or a real serial device), answers ping and echoes UDP on port 7.
// See docs/netstack_linux_demo.md for binding it to the host's network stack.
//
//   netstack_linux_demo [--ppp] [--ip A.B.C.D] [--port N] [-v] [DEVICE]
//
// Without DEVICE a pty is created and its slave path is printed. Every IPv4 datagram crossing the
// link is logged through a microfmt logger (-v: also a hexdump of each one).

#include <structo/bootldr/netstack.hpp>
#include <structo/bootldr/udp_socket.hpp>
#include <structo/hw/ppp_device.hpp>
#include <structo/hw/slip_device.hpp>

#include <reloco/array.hpp>

#include <microfmt/formatters/hexdump.hpp>
#include <microfmt/log/logger.hpp>

#if RELOCO_HAS_COROUTINES

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <reloco/lifetime.hpp>

// Example code indexes raw buffers freely; bounds are checked by the surrounding logic.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

using namespace structo;

namespace {

microfmt::log::stdout_color_sink<512> console;
microfmt::log::basic_logger<1, 1024> logger("netdemo", console.as_sink());
bool dump_hex = false;

constexpr std::size_t mtu = 1006;

volatile std::sig_atomic_t stop_requested = 0;
void on_signal(int) { stop_requested = 1; }

// A non-blocking tty as a polled UART. `rx_ready` has to peek, so one byte is read ahead.
struct linux_uart {
  int fd = -1;
  bool have = false;
  std::uint8_t byte = 0;
};

std::uint64_t now_ms(void *) noexcept {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000u + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000u;
}

// Sleeps until the tty has input (or 5 ms passed: timers still need servicing).
void idle(void *ctx) noexcept {
  pollfd p{static_cast<linux_uart *>(ctx)->fd, POLLIN, 0};
  (void)::poll(&p, 1, 5);
}

bool open_tty(linux_uart &u, const char *path, char *slave_out, std::size_t slave_cap) {
  int fd;
  if (path) {
    fd = ::open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
  } else {
    fd = ::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd >= 0 && (::grantpt(fd) != 0 || ::unlockpt(fd) != 0)) {
      ::close(fd);
      fd = -1;
    }
    if (fd >= 0) {
      const char *name = ::ptsname(fd);
      std::snprintf(slave_out, slave_cap, "%s", name ? name : "?");
    }
  }
  if (fd < 0)
    return false;
  termios t{};
  if (::tcgetattr(fd, &t) == 0) {
    ::cfmakeraw(&t);
    (void)::tcsetattr(fd, TCSANOW, &t);
  }
  u.fd = fd;
  return true;
}

// Echo task: every datagram sent to our port goes straight back to its sender.
reloco::task<void> echo_task(bootldr::udp_socket &s) noexcept {
  reloco::array<std::uint8_t, 512> buf{};
  for (;;) {
    auto rx = co_await s.receive_from(reloco::span<std::uint8_t>(buf));
    if (!rx)
      co_return;
    logger.info("udp echo: {} bytes from {}.{}.{}.{}:{}", rx->size, rx->source.octets[0], rx->source.octets[1],
                rx->source.octets[2], rx->source.octets[3], rx->source_port);
    (void)co_await s.send_to(rx->source, rx->source_port, reloco::span<const std::uint8_t>(buf.data(), rx->size));
  }
}

bool parse_ip(const char *s, net::ipv4_address &out) {
  unsigned a, b, c, d;
  if (std::sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
    return false;
  out = net::ipv4_address{static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(c),
                          static_cast<std::uint8_t>(d)};
  return true;
}

// Reports address/link changes once.
struct reporter {
  bootldr::netstack<mtu> *net;
  bool was_ready = false;
};

void report(void *ctx) noexcept {
  auto &r = *static_cast<reporter *>(ctx);
  if (r.net->ready() == r.was_ready)
    return;
  r.was_ready = r.net->ready();
  if (r.was_ready) {
    const auto &a = r.net->config().address.octets;
    logger.info("network up, address {}.{}.{}.{}", a[0], a[1], a[2], a[3]);
  } else {
    logger.warn("network down");
  }
  logger.flush();
}

} // namespace

template <> struct structo::hw::uart_traits<linux_uart> {
  static reloco::result<void> configure(linux_uart &, const uart_config &) noexcept { return {}; }
  static reloco::result<bool> tx_ready(linux_uart &) noexcept { return true; }
  static reloco::result<bool> rx_ready(linux_uart &u) noexcept {
    if (u.have)
      return true;
    const ssize_t n = ::read(u.fd, &u.byte, 1);
    if (n == 1)
      u.have = true;
    return u.have;
  }
  static reloco::result<void> try_put_byte(linux_uart &u, std::uint8_t v) noexcept {
    for (;;) {
      const ssize_t n = ::write(u.fd, &v, 1);
      if (n == 1)
        return {};
      if (n < 0 && errno == EINTR)
        continue;
      if (n < 0 && (errno == EAGAIN || errno == EIO)) {
        // EIO: nobody has the pty slave open yet; drop the byte like an unplugged line would.
        if (errno == EAGAIN) {
          pollfd p{u.fd, POLLOUT, 0};
          (void)::poll(&p, 1, 10);
          continue;
        }
        return {};
      }
      return reloco::unexpected(reloco::error::invalid_state);
    }
  }
  static reloco::result<std::uint8_t> try_get_byte(linux_uart &u) noexcept {
    if (!u.have)
      return reloco::unexpected(reloco::error::try_again);
    u.have = false;
    return u.byte;
  }
};

namespace {

const char *proto_name(std::uint8_t p) {
  switch (p) {
  case 1:
    return "ICMP";
  case 6:
    return "TCP";
  case 17:
    return "UDP";
  default:
    return "IP";
  }
}

// One log line per IPv4 datagram: direction, endpoints, protocol details.
void log_packet(const char *dir, reloco::span<const std::uint8_t> p) {
  if (p.size() < 20) {
    logger.warn("{} runt datagram, {} bytes", dir, p.size());
    return;
  }
  const std::size_t ihl = static_cast<std::size_t>(p[0] & 0x0f) * 4u;
  const std::uint8_t proto = p[9];
  if (proto == 17 && p.size() >= ihl + 8) {
    const unsigned sp = static_cast<unsigned>(p[ihl] << 8 | p[ihl + 1]);
    const unsigned dp = static_cast<unsigned>(p[ihl + 2] << 8 | p[ihl + 3]);
    logger.info("{} UDP {}.{}.{}.{}:{} -> {}.{}.{}.{}:{} len {}", dir, p[12], p[13], p[14], p[15], sp, p[16], p[17], p[18],
                p[19], dp, p.size());
  } else if (proto == 1 && p.size() >= ihl + 2) {
    logger.info("{} ICMP type {} {}.{}.{}.{} -> {}.{}.{}.{} len {}", dir, p[ihl], p[12], p[13], p[14], p[15], p[16], p[17],
                p[18], p[19], p.size());
  } else {
    logger.info("{} {} {}.{}.{}.{} -> {}.{}.{}.{} len {}", dir, proto_name(proto), p[12], p[13], p[14], p[15], p[16], p[17],
                p[18], p[19], p.size());
  }
  if (dump_hex)
    logger.debug("\n{}", microfmt::hexdump(microfmt::span<const std::uint8_t>(p.data(), p.size()), 0));
}

// Wraps a polled backend (slip_device/ppp_device) and logs the datagrams passing through it.
template <class Dev> class logging_device {
public:
  explicit logging_device(Dev &dev) noexcept : dev_(dev) {}
  [[nodiscard]] std::size_t mtu() const noexcept { return dev_.mtu(); }
  [[nodiscard]] reloco::result<bool> link_up() noexcept { return dev_.link_up(); }
  [[nodiscard]] reloco::result<void> try_send(reloco::span<const std::uint8_t> f) noexcept {
    auto r = dev_.try_send(f);
    if (r)
      log_packet("tx", f);
    return r;
  }
  [[nodiscard]] reloco::result<std::size_t> try_receive(reloco::span<std::uint8_t> f) noexcept {
    auto r = dev_.try_receive(f);
    if (r)
      log_packet("rx", reloco::span<const std::uint8_t>(f.data(), r.value()));
    return r;
  }

private:
  Dev &dev_;
};

// Runs the stack over whichever framing was chosen; `Dev` is slip_device or ppp_device.
template <class Dev>
int run(Dev &dev, linux_uart &uart, bool ppp, net::ipv4_config static_ip, std::uint16_t port) {
  logging_device<Dev> logged{dev};
  hw::polled_net_device<logging_device<Dev>> pnd{logged};
  hw::net_device_ref nic{pnd};

  bootldr::scheduler sched;
  sched.set_clock(now_ms, nullptr);
  sched.set_idle(idle, &uart);

  bootldr::netstack_config cfg;
  if (!ppp)
    cfg.static_ip = static_ip;
  bootldr::netstack<mtu> net{sched, nic, cfg};
  if (!net.poll_with(pnd))
    return 1;
  if constexpr (requires { dev.link(); })
    net.use_ppp(dev.link());
  if (!net.start())
    return 1;

  bootldr::udp_socket sock{net};
  auto bound = sock.bind(port);
  if (!bound) {
    logger.error("bind failed");
    return 1;
  }
  if (!sched.spawn(echo_task(sock)))
    return 1;

  reporter rep{&net};
  if (!sched.add_poller(report, &rep))
    return 1;

  while (!stop_requested) {
    sched.run_once();
    idle(&uart);
  }
  logger.info("bye");
  net.stop();
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  bool ppp = false;
  net::ipv4_config ip{net::ipv4_address{192, 168, 7, 2}, net::ipv4_address{255, 255, 255, 255},
                      net::ipv4_address{192, 168, 7, 1}};
  std::uint16_t port = 7;
  const char *device = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--ppp")) {
      ppp = true;
    } else if (!std::strcmp(argv[i], "--ip") && i + 1 < argc) {
      if (!parse_ip(argv[++i], ip.address)) {
        std::fprintf(stderr, "bad address\n");
        return 2;
      }
    } else if (!std::strcmp(argv[i], "-v") || !std::strcmp(argv[i], "-vv")) {
      dump_hex = true;
    } else if (!std::strcmp(argv[i], "--port") && i + 1 < argc) {
      port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
    } else if (argv[i][0] != '-') {
      device = argv[i];
    } else {
      std::fprintf(stderr, "usage: %s [--ppp] [--ip A.B.C.D] [--port N] [-v] [DEVICE]\n", argv[0]);
      return 2;
    }
  }

  linux_uart uart;
  char slave[128] = "";
  if (!open_tty(uart, device, slave, sizeof slave)) {
    std::perror("open tty");
    return 1;
  }
  logger.set_level(dump_hex ? microfmt::log::level::debug : microfmt::log::level::info);
  logger.info("{} over {}, UDP echo on port {}", ppp ? "PPP" : "SLIP", device ? device : slave, port);
  logger.flush();

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  int rc;
  if (ppp) {
    hw::ppp_device<mtu> dev{hw::uart_ref{uart}, now_ms, nullptr};
    rc = run(dev, uart, true, ip, port);
  } else {
    hw::slip_device<mtu> dev{hw::uart_ref{uart}};
    rc = run(dev, uart, false, ip, port);
  }
  ::close(uart.fd);
  return rc;
}

RELOCO_END_UNSAFE_BUFFER_USAGE

#else

int main() { return 0; }

#endif
