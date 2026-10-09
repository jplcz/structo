// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

// Linux demo of the bootloader network stack as an Ethernet host on a TAP interface: the "board"
// gets its own MAC and IPv4 address, resolves neighbours with ARP, answers ping and echoes UDP.
// For controlled test/CI/development setups only; it needs CAP_NET_ADMIN (or a pre-created TAP
// owned by you).
//
//   netstack_tap_demo [--tap NAME] [--ip A.B.C.D] [--port N] [-v]
//
// Every Ethernet frame crossing the TAP is logged (-v: also a hexdump of each one).
//
// UDP services (N defaults to 7): N echo, N+1 upper-case (replies with the datagram upper-cased),
// N+2 uptime (any datagram is answered with the board's uptime in milliseconds as text).
//
// Setup (as root), then talk to the board from the host:
//   ip tuntap add dev structo0 mode tap user $USER
//   ip addr add 192.168.77.1/24 dev structo0 && ip link set structo0 up
//   ./netstack_tap_demo --tap structo0 --ip 192.168.77.2
//   ping 192.168.77.2
//   echo hi | nc -u -w1 192.168.77.2 7     # echo
//   echo hi | nc -u -w1 192.168.77.2 8     # HI
//   echo ? | nc -u -w1 192.168.77.2 9      # uptime

#include <structo/bootldr/netstack.hpp>
#include <structo/bootldr/udp_socket.hpp>
#include <structo/hw/ethernet_device.hpp>
#include <structo/hw/ethernet_nic.hpp>

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
#include <linux/if_tun.h>
#include <net/if.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <reloco/lifetime.hpp>

RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

using namespace structo;

namespace {

microfmt::log::stdout_color_sink<512> console;
microfmt::log::basic_logger<1, 1024> logger("tapdemo", console.as_sink());

constexpr std::size_t ip_mtu = 1500;
constexpr hw::net_mac_address board_mac{0x02, 0x53, 0x54, 0x52, 0x00, 0x01}; // locally administered

volatile std::sig_atomic_t stop_requested = 0;
void on_signal(int) { stop_requested = 1; }

std::uint64_t now_ms(void *) noexcept {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000u + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000u;
}

bool dump_hex = false;

// One log line per frame: direction, MACs, and ARP/IPv4 details.
void log_frame(const char *dir, reloco::span<const std::uint8_t> f) {
  auto eth = net::parse_ethernet(f);
  if (!eth) {
    logger.warn("{} runt frame, {} bytes", dir, f.size());
    return;
  }
  const std::uint8_t *d = eth->dst.data();
  const std::uint8_t *s = eth->src.data();
  const auto &p = eth->payload;
  if (eth->ethertype == net::arp_ethertype && p.size() >= 28) {
    if (p[7] == 1)
      logger.info("{} ARP who-has {}.{}.{}.{} tell {}.{}.{}.{}", dir, p[24], p[25], p[26], p[27], p[14], p[15], p[16],
                  p[17]);
    else
      logger.info("{} ARP {}.{}.{}.{} is-at {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", dir, p[14], p[15], p[16], p[17],
                  p[8], p[9], p[10], p[11], p[12], p[13]);
  } else if (eth->ethertype == net::ipv4_ethertype && p.size() >= 20) {
    const std::size_t ihl = static_cast<std::size_t>(p[0] & 0x0f) * 4u;
    const unsigned total = static_cast<unsigned>(p[2] << 8 | p[3]); // excludes Ethernet padding
    const char *proto = p[9] == 1 ? "ICMP" : p[9] == 6 ? "TCP" : p[9] == 17 ? "UDP" : "IP";
    if (p[9] == 17 && p.size() >= ihl + 4)
      logger.info("{} IPv4 UDP {}.{}.{}.{}:{} -> {}.{}.{}.{}:{} len {}", dir, p[12], p[13], p[14], p[15],
                  static_cast<unsigned>(p[ihl] << 8 | p[ihl + 1]), p[16], p[17], p[18], p[19],
                  static_cast<unsigned>(p[ihl + 2] << 8 | p[ihl + 3]), total);
    else
      logger.info("{} IPv4 {} {}.{}.{}.{} -> {}.{}.{}.{} len {}", dir, proto, p[12], p[13], p[14], p[15], p[16], p[17],
                  p[18], p[19], total);
  } else {
    logger.info("{} ethertype {:04x} {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x} -> "
                "{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x} len {}",
                dir, eth->ethertype, s[0], s[1], s[2], s[3], s[4], s[5], d[0], d[1], d[2], d[3], d[4], d[5], f.size());
  }
  if (dump_hex)
    logger.debug("\n{}", microfmt::hexdump(microfmt::span<const std::uint8_t>(f.data(), f.size()), 0));
}

// The "hardware driver": a TAP file descriptor behind the hw::ethernet_nic driver contract (see
// structo/hw/ethernet_nic.hpp). A real MAC driver implements the same five hooks on its descriptor rings.
struct tap_driver {
  static constexpr std::size_t max_frame_size = ip_mtu + net::ethernet_header_size;

  int fd = -1;
  reloco::array<std::uint8_t, max_frame_size> rx_buf{};
  std::size_t rx_len = 0; // > 0: a frame is held for rx_peek()/rx_release()

  hw::net_mac_address read_mac() noexcept { return board_mac; }
  bool link_up() noexcept { return true; }

  // TX: hand one frame (already padded by ethernet_nic) to the "hardware".
  reloco::result<void> tx_submit(reloco::span<const std::uint8_t> frame) noexcept {
    const ssize_t n = ::write(fd, frame.data(), frame.size());
    if (n == static_cast<ssize_t>(frame.size())) {
      log_frame("tx", frame);
      return {};
    }
    return reloco::unexpected(n < 0 && errno == EAGAIN ? reloco::error::try_again : reloco::error::invalid_state);
  }

  // RX: view the oldest completed frame; stays valid until rx_release().
  reloco::result<reloco::span<const std::uint8_t>> rx_peek() noexcept {
    if (rx_len == 0) {
      const ssize_t n = ::read(fd, rx_buf.data(), rx_buf.size());
      if (n <= 0)
        return reloco::unexpected(reloco::error::try_again);
      rx_len = static_cast<std::size_t>(n);
      log_frame("rx", reloco::span<const std::uint8_t>(rx_buf.data(), rx_len));
    }
    return reloco::span<const std::uint8_t>(rx_buf.data(), rx_len);
  }

  // RX: give the frame's slot back.
  void rx_release() noexcept { rx_len = 0; }
};

bool open_tap(tap_driver &nic, const char *name) {
  const int fd = ::open("/dev/net/tun", O_RDWR | O_NONBLOCK);
  if (fd < 0)
    return false;
  ifreq req{};
  req.ifr_flags = IFF_TAP | IFF_NO_PI;
  std::snprintf(req.ifr_name, IFNAMSIZ, "%s", name);
  if (::ioctl(fd, TUNSETIFF, &req) < 0) {
    ::close(fd);
    return false;
  }
  nic.fd = fd;
  return true;
}

// Sleeps until a frame arrives (or 5 ms passed: timers still need servicing).
void idle(void *ctx) noexcept {
  pollfd p{static_cast<tap_driver *>(ctx)->fd, POLLIN, 0};
  (void)::poll(&p, 1, 5);
}

// Echo task: every datagram sent to our port goes straight back to its sender.
reloco::task<void> echo_task(bootldr::udp_socket &s) noexcept {
  reloco::array<std::uint8_t, 1024> buf{};
  for (;;) {
    auto rx = co_await s.receive_from(reloco::span<std::uint8_t>(buf));
    if (!rx)
      co_return;
    (void)co_await s.send_to(rx->source, rx->source_port, reloco::span<const std::uint8_t>(buf.data(), rx->size));
  }
}

// Upper-case task: replies with the datagram converted to upper case (ASCII only).
reloco::task<void> upper_task(bootldr::udp_socket &s) noexcept {
  reloco::array<std::uint8_t, 1024> buf{};
  for (;;) {
    auto rx = co_await s.receive_from(reloco::span<std::uint8_t>(buf));
    if (!rx)
      co_return;
    for (std::size_t i = 0; i < rx->size; ++i)
      if (buf[i] >= 'a' && buf[i] <= 'z')
        buf[i] = static_cast<std::uint8_t>(buf[i] - 'a' + 'A');
    (void)co_await s.send_to(rx->source, rx->source_port, reloco::span<const std::uint8_t>(buf.data(), rx->size));
  }
}

// Uptime task: any datagram is answered with the milliseconds since start, as decimal text.
reloco::task<void> uptime_task(bootldr::udp_socket &s, std::uint64_t start_ms) noexcept {
  reloco::array<std::uint8_t, 16> req{};
  for (;;) {
    auto rx = co_await s.receive_from(reloco::span<std::uint8_t>(req));
    if (!rx)
      co_return;
    reloco::array<std::uint8_t, 24> out{};
    std::size_t n = 0;
    std::uint64_t v = now_ms(nullptr) - start_ms;
    do {
      out[n++] = static_cast<std::uint8_t>('0' + v % 10);
      v /= 10;
    } while (v != 0);
    for (std::size_t i = 0; i < n / 2; ++i) {
      const std::uint8_t t = out[i];
      out[i] = out[n - 1 - i];
      out[n - 1 - i] = t;
    }
    out[n++] = '\n';
    (void)co_await s.send_to(rx->source, rx->source_port, reloco::span<const std::uint8_t>(out.data(), n));
  }
}

// Logs when the stack's address becomes active (or is lost).
struct reporter {
  bootldr::netstack<ip_mtu> *net;
  bool was_ready = false;
};

void report(void *ctx) noexcept {
  auto &r = *static_cast<reporter *>(ctx);
  if (r.net->ready() == r.was_ready)
    return;
  r.was_ready = r.net->ready();
  const auto &a = r.net->config().address.octets;
  if (r.was_ready)
    logger.info("network up, address {}.{}.{}.{}", a[0], a[1], a[2], a[3]);
  else
    logger.warn("network down");
  logger.flush();
}

bool parse_ip(const char *s, net::ipv4_address &out) {
  unsigned a, b, c, d;
  if (std::sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
    return false;
  out = net::ipv4_address{static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(c),
                          static_cast<std::uint8_t>(d)};
  return true;
}

} // namespace

int main(int argc, char **argv) {
  const char *tap_name = "structo0";
  net::ipv4_address ip{192, 168, 77, 2};
  std::uint16_t port = 7;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--tap") && i + 1 < argc) {
      tap_name = argv[++i];
    } else if (!std::strcmp(argv[i], "--ip") && i + 1 < argc) {
      if (!parse_ip(argv[++i], ip)) {
        logger.error("bad address");
        logger.flush();
        return 2;
      }
    } else if (!std::strcmp(argv[i], "-v")) {
      dump_hex = true;
    } else if (!std::strcmp(argv[i], "--port") && i + 1 < argc) {
      port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
    } else {
      logger.error("usage: {} [--tap NAME] [--ip A.B.C.D] [--port N] [-v]", argv[0]);
      logger.flush();
      return 2;
    }
  }

  logger.set_level(dump_hex ? microfmt::log::level::debug : microfmt::log::level::info);
  tap_driver drv;
  if (!open_tap(drv, tap_name)) {
    logger.error("open TAP failed (errno {}): needs /dev/net/tun and CAP_NET_ADMIN or a pre-created interface", errno);
    logger.flush();
    return 1;
  }

  // Raw frames from the TAP -> Ethernet layer (ARP, MAC filtering) -> IPv4 datagrams for the netstack.
  hw::ethernet_nic<tap_driver> nic{drv}; // validates/pads frames, strips FCS, counts
  hw::polled_net_device<decltype(nic)> raw_pnd{nic};
  hw::net_device_ref raw{raw_pnd};
  hw::ethernet_device<ip_mtu, 8> eth{raw, now_ms, nullptr};
  hw::net_device_ref ip_side{eth};

  bootldr::scheduler sched;
  sched.set_clock(now_ms, nullptr);
  sched.set_idle(idle, &drv);

  bootldr::netstack_config cfg;
  cfg.static_ip =
      net::ipv4_config::make_static(ip, net::ipv4_address{255, 255, 255, 0}, {}); // no gateway: same subnet only
  bootldr::netstack<ip_mtu> net{sched, ip_side, cfg};
  if (!net.poll_with(raw_pnd))
    return 1;
  net.use_ethernet(eth);
  if (!net.start())
    return 1;

  bootldr::udp_socket echo_sock{net};
  bootldr::udp_socket upper_sock{net};
  bootldr::udp_socket uptime_sock{net};
  if (!echo_sock.bind(port) || !upper_sock.bind(static_cast<std::uint16_t>(port + 1)) ||
      !uptime_sock.bind(static_cast<std::uint16_t>(port + 2)))
    return 1;
  if (!sched.spawn(echo_task(echo_sock)) || !sched.spawn(upper_task(upper_sock)) ||
      !sched.spawn(uptime_task(uptime_sock, now_ms(nullptr))))
    return 1;

  reporter rep{&net};
  if (!sched.add_poller(report, &rep))
    return 1;

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  logger.info("{}: {}.{}.{}.{}, UDP echo/upper-case/uptime on ports {}/{}/{}", tap_name, ip.octets[0], ip.octets[1],
              ip.octets[2], ip.octets[3], port, port + 1, port + 2);
  logger.flush();

  while (!stop_requested) {
    sched.run_once();
    idle(&drv);
  }
  logger.info("bye");
  logger.flush();
  net.stop();
  ::close(drv.fd);
  return 0;
}

RELOCO_END_UNSAFE_BUFFER_USAGE

#else

int main() { return 0; }

#endif
