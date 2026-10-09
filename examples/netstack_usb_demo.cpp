// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

// Linux demo: bootldr::usb_stack hands a (fake) USB device to a driver coroutine that brings up a
// bootldr::netstack on it, so the "board" gets an IPv4 network over USB. The USB device is simulated by
// usb_linux_fake.hpp and bridged to the Linux host; the real stack code (hot-plug, enumeration, class
// drivers, netstack) runs unchanged. For controlled test/CI/development setups only.
//
//   netstack_usb_demo [--mode ecm|ppp] [--tap NAME] [--ip A.B.C.D] [--port N] [--cycle SEC] [-v]
//
// Modes:
//   ecm  CDC-ECM Ethernet adapter <-> a TAP interface (needs CAP_NET_ADMIN or a pre-created TAP):
//          ip tuntap add dev structo1 mode tap user $USER
//          ip addr add 192.168.78.1/24 dev structo1 && ip link set structo1 up
//          ./netstack_usb_demo --tap structo1 --ip 192.168.78.2
//          ping 192.168.78.2
//   ppp  CDC-ACM serial port <-> a pty, PPP (LCP/IPCP) on top; the board gets its address from the peer:
//          ./netstack_usb_demo --mode ppp          # prints the pty path
//          pppd /dev/pts/N 115200 192.168.7.1:192.168.7.2 noauth local nodetach
//          ping 192.168.7.2
//
// UDP services on N (default 7): N echo, N+1 upper-case.   Try:  echo hi | nc -u -w1 <board ip> 7
//
// Hot-plug: `kill -USR1 <pid>` toggles the fake device's cable; `--cycle SEC` unplugs/replugs it every SEC
// seconds. On unplug the driver coroutine sees its transfers fail, tears the network down by itself and
// returns; on replug the stack spawns a fresh one.

#include <structo/bootldr/netstack.hpp>
#include <structo/bootldr/udp_socket.hpp>
#include <structo/bootldr/usb_stack.hpp>
#include <structo/hw/ethernet_device.hpp>
#include <structo/hw/polled_net_device.hpp>
#include <structo/hw/ppp_device.hpp>
#include <structo/usb/cdc_acm.hpp>
#include <structo/usb/cdc_ecm.hpp>

#include <reloco/array.hpp>

#include <microfmt/log/logger.hpp>

#if RELOCO_HAS_COROUTINES

#include "usb_linux_fake.hpp"

#include <reloco/array.hpp>
#include <reloco/span.hpp>
#include <reloco/string_view.hpp>
#include <csignal>
#include <cstdlib>


using namespace structo;

namespace {

microfmt::log::stdout_color_sink<512> console;
microfmt::log::basic_logger<1, 1024> logger("usbnet", console.as_sink());

constexpr std::size_t ip_mtu = 1500;

volatile std::sig_atomic_t stop_requested = 0;
volatile std::sig_atomic_t toggle_requested = 0;
void on_stop(int) { stop_requested = 1; }
void on_usr1(int) { toggle_requested = 1; }

// Settings the driver coroutines need; handed to them through the usb_driver `ctx` pointer.
struct app_config {
  net::ipv4_address ip{192, 168, 78, 2}; // ecm mode only; ppp gets its address from the peer
  std::uint16_t port = 7;
};

// --- logging -----------------------------------------------------------------------------------------

void log_ipv4(const char *dir, reloco::span<const std::uint8_t> p) {
  const std::size_t len = p.size();
  if (len < 20) {
    logger.warn("{} runt datagram, {} bytes", dir, len);
    return;
  }
  const std::size_t ihl = static_cast<std::size_t>(p[0] & 0x0f) * 4u;
  const unsigned total = static_cast<unsigned>(p[2] << 8 | p[3]);
  if (p[9] == 17 && len >= ihl + 4)
    logger.info("{} UDP {}.{}.{}.{}:{} -> {}.{}.{}.{}:{} len {}", dir, p[12], p[13], p[14], p[15],
                static_cast<unsigned>(p[ihl] << 8 | p[ihl + 1]), p[16], p[17], p[18], p[19],
                static_cast<unsigned>(p[ihl + 2] << 8 | p[ihl + 3]), total);
  else
    logger.info("{} IPv4 {} {}.{}.{}.{} -> {}.{}.{}.{} len {}", dir, p[9] == 1 ? "ICMP" : "proto", p[12], p[13], p[14],
                p[15], p[16], p[17], p[18], p[19], total);
}

// Logged where the Ethernet frame crosses the fake USB wire.
void log_eth_frame(const char *dir, reloco::span<const std::uint8_t> f) {
  const std::size_t len = f.size();
  if (len < 14) {
    logger.warn("{} runt frame, {} bytes", dir, len);
    return;
  }
  const unsigned type = static_cast<unsigned>(f[12] << 8 | f[13]);
  const auto p = f.subspan(14);
  if (type == 0x0806 && len >= 14 + 28) {
    if (p[7] == 1)
      logger.info("{} ARP who-has {}.{}.{}.{} tell {}.{}.{}.{}", dir, p[24], p[25], p[26], p[27], p[14], p[15], p[16],
                  p[17]);
    else
      logger.info("{} ARP {}.{}.{}.{} is-at {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", dir, p[14], p[15], p[16], p[17],
                  p[8], p[9], p[10], p[11], p[12], p[13]);
  } else if (type == 0x0800) {
    log_ipv4(dir, p);
  } else {
    logger.info("{} ethertype {:04x} len {}", dir, type, len);
  }
}

// Wraps a polled backend (ppp_device) and logs the datagrams passing through it.
template <class Dev> class logging_device {
public:
  explicit logging_device(Dev &dev) noexcept : dev_(dev) {}
  [[nodiscard]] std::size_t mtu() const noexcept { return dev_.mtu(); }
  [[nodiscard]] reloco::result<bool> link_up() noexcept { return dev_.link_up(); }
  [[nodiscard]] reloco::result<void> try_send(reloco::span<const std::uint8_t> f) noexcept {
    auto r = dev_.try_send(f);
    if (r)
      log_ipv4("tx", f);
    return r;
  }
  [[nodiscard]] reloco::result<std::size_t> try_receive(reloco::span<std::uint8_t> f) noexcept {
    auto r = dev_.try_receive(f);
    if (r)
      log_ipv4("rx", f.first(*r));
    return r;
  }

private:
  Dev &dev_;
};

// --- UDP services ------------------------------------------------------------------------------------

reloco::task<void> echo_task(bootldr::udp_socket &s) noexcept {
  reloco::array<std::uint8_t, 512> buf{};
  for (;;) {
    auto rx = co_await s.receive_from(reloco::span<std::uint8_t>(buf));
    if (!rx)
      co_return;
    (void)co_await s.send_to(rx->source, rx->source_port, reloco::span<const std::uint8_t>(buf).first(rx->size));
  }
}

reloco::task<void> upper_task(bootldr::udp_socket &s) noexcept {
  reloco::array<std::uint8_t, 512> buf{};
  for (;;) {
    auto rx = co_await s.receive_from(reloco::span<std::uint8_t>(buf));
    if (!rx)
      co_return;
    for (std::size_t i = 0; i < rx->size; ++i)
      if (buf[i] >= 'a' && buf[i] <= 'z')
        buf[i] = static_cast<std::uint8_t>(buf[i] - 'a' + 'A');
    (void)co_await s.send_to(rx->source, rx->source_port, reloco::span<const std::uint8_t>(buf).first(rx->size));
  }
}

// Common tail of both drivers: bind the UDP services to the stack and park until the cable is pulled.
template <std::size_t Mtu>
reloco::task<void> serve_until_gone(bootldr::netstack<Mtu> &net, bootldr::scheduler &sched,
                                    bootldr::usb_attached_device &dev, std::uint16_t port) noexcept {
  bootldr::udp_socket echo_sock{net};
  bootldr::udp_socket upper_sock{net};
  if (!echo_sock.bind(port) || !upper_sock.bind(static_cast<std::uint16_t>(port + 1))) {
    logger.error("cannot bind UDP ports {}/{}", port, port + 1);
    co_return;
  }
  auto echo = sched.spawn(echo_task(echo_sock));
  auto upper = sched.spawn(upper_task(upper_sock));
  logger.info("UDP echo on {}, upper-case on {}", port, port + 1);

  // The device's gone_event is set on unplug; every transfer of this device fails from then on.
  co_await dev.gone_event().wait();

  // Tasks that reference the sockets must be gone before the sockets are destroyed.
  if (echo)
    (void)sched.cancel(*echo);
  if (upper)
    (void)sched.cancel(*upper);
}

void announce(bootldr::usb_attached_device &dev, const char *what) {
  const auto &d = dev.device().descriptor();
  logger.info("{} on port {}: {:04x}:{:04x}", what, dev.port(), d.vendor_id, d.product_id);
}

// --- driver 1: CDC-ECM -> Ethernet -> IPv4 -------------------------------------------------------------

bool match_ecm(void *, const usb::usb_device &d) noexcept {
  return d.config().find_interface(usb::usb_class::cdc, usb::cdc::subclass_ecm).has_value();
}

// Spawned by the usb_stack when an ECM adapter shows up. Everything it owns lives in this coroutine
// frame, so returning (after the unplug) tears the whole network down.
reloco::task<void> ecm_driver(void *ctx, bootldr::usb_stack &stack, bootldr::usb_device_ptr dev) noexcept {
  const auto &app = *static_cast<const app_config *>(ctx);
  usb::cdc_ecm ecm;
  if (auto r = co_await ecm.attach(dev->device()); !r) {
    logger.error("ECM attach failed: error {}", static_cast<int>(r.error()));
    co_return;
  }
  const auto mac = ecm.mac();
  logger.info("USB Ethernet up, MAC {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", mac[0], mac[1], mac[2], mac[3], mac[4],
              mac[5]);

  // Raw frames from the ECM adapter -> Ethernet layer (ARP, MAC filtering) -> IPv4 datagrams for the netstack.
  hw::net_device_ref raw{ecm};
  hw::ethernet_device<ip_mtu, 8> eth{raw, usbfake::now_ms, nullptr};
  hw::net_device_ref ip_side{eth};

  bootldr::netstack_config cfg;
  cfg.static_ip = net::ipv4_config::make_static(app.ip, net::ipv4_address{255, 255, 255, 0}, {});
  bootldr::netstack<ip_mtu> net{stack.sched(), ip_side, cfg};
  net.use_ethernet(eth);
  if (!net.start()) {
    ecm.detach();
    co_return;
  }
  logger.info("address {}.{}.{}.{}", app.ip.octets[0], app.ip.octets[1], app.ip.octets[2], app.ip.octets[3]);

  (void)co_await serve_until_gone(net, stack.sched(), *dev, app.port);

  logger.info("USB Ethernet gone, shutting the network down");
  net.stop();
  ecm.detach();
}

// --- driver 2: CDC-ACM -> UART -> PPP -> IPv4 ----------------------------------------------------------

bool match_acm(void *, const usb::usb_device &d) noexcept {
  return d.config().find_interface(usb::usb_class::cdc, usb::cdc::subclass_acm).has_value();
}

template <class P> reloco::task<void> poll_loop(P &pnd, bootldr::scheduler &sched) noexcept {
  for (;;) {
    pnd.poll(); // retries parked sends/receives of the polled device
    co_await sched.yield();
  }
}

reloco::task<void> ppp_driver(void *ctx, bootldr::usb_stack &stack, bootldr::usb_device_ptr dev) noexcept {
  const auto &app = *static_cast<const app_config *>(ctx);
  auto &sched = stack.sched();
  usb::cdc_acm<512, 512> acm;
  if (auto r = co_await acm.attach(dev->device()); !r) {
    logger.error("ACM attach failed: error {}", static_cast<int>(r.error()));
    co_return;
  }
  // The pumps move bytes between the bulk endpoints and the UART rings; they end when `acm` is detached.
  auto rx = sched.spawn(acm.run_rx(), bootldr::spawn_mode::joinable);
  auto tx = sched.spawn(acm.run_tx(), bootldr::spawn_mode::joinable);
  logger.info("USB serial up, starting PPP");

  {
    // The board is up long before pppd is started on the host, so keep sending Configure-Requests
    // (every second) instead of giving up after the default 10 tries.
    net::ppp_config pcfg;
    pcfg.restart_ms = 1000;
    pcfg.max_configure = ~0u;
    hw::ppp_device<ip_mtu> ppp{hw::uart_ref{acm}, usbfake::now_ms, nullptr, pcfg};
    logging_device<hw::ppp_device<ip_mtu>> logged{ppp};
    hw::polled_net_device<logging_device<hw::ppp_device<ip_mtu>>> pnd{logged};
    hw::net_device_ref nic{pnd};

    bootldr::netstack<ip_mtu> net{sched, nic};
    net.use_ppp(ppp.link()); // address and gateway come from IPCP
    auto poller = sched.spawn(poll_loop(pnd, sched));
    if (poller && net.start()) {
      (void)co_await serve_until_gone(net, sched, *dev, app.port);
      logger.info("USB serial gone, shutting the network down");
    }
    net.stop();
    if (poller)
      (void)sched.cancel(*poller);
  }

  acm.detach(); // wakes the pumps; they must finish before `acm` is destroyed
  if (rx)
    (void)co_await sched.join(*rx);
  if (tx)
    (void)co_await sched.join(*tx);
}

// --- demo plumbing -----------------------------------------------------------------------------------

void on_usb_event(void *, bootldr::usb_event_kind kind, unsigned port, bootldr::usb_attached_device *dev,
                  const reloco::result<void> &st) noexcept {
  switch (kind) {
  case bootldr::usb_event_kind::attached:
    announce(*dev, "device attached, driver started");
    break;
  case bootldr::usb_event_kind::unclaimed:
    announce(*dev, "device attached, no driver");
    break;
  case bootldr::usb_event_kind::detached:
    logger.warn("device detached from port {}", port);
    break;
  case bootldr::usb_event_kind::enumeration_failed:
    logger.error("enumeration failed on port {}: error {}", port, st ? 0 : static_cast<int>(st.error()));
    break;
  }
  logger.flush();
}

// Runs once per scheduler round: services the fake controller and plays with the cable.
struct board {
  usbfake::hcd *hcd;
  usbfake::device *dev;
  std::uint64_t cycle_ms = 0;
  std::uint64_t next_toggle = 0;
};

void service(void *p) noexcept {
  auto &b = *static_cast<board *>(p);
  const std::uint64_t now = usbfake::now_ms(nullptr);
  if (b.cycle_ms && now >= b.next_toggle) {
    toggle_requested = 1;
    b.next_toggle = now + b.cycle_ms;
  }
  if (toggle_requested) {
    toggle_requested = 0;
    if (b.hcd->plugged()) {
      logger.warn("-- cable pulled --");
      b.hcd->unplug();
    } else {
      logger.warn("-- cable plugged --");
      b.hcd->plug(*b.dev);
    }
    logger.flush();
  }
  b.hcd->pump();
}

// Parses "A.B.C.D" (trailing text is ignored, as sscanf did).
bool parse_ip(reloco::string_view text, net::ipv4_address &out) {
  reloco::array<std::uint8_t, 4> octets{};
  std::size_t pos = 0;
  for (std::size_t k = 0; k < octets.size(); ++k) {
    if (k > 0) {
      if (pos >= text.size() || text[pos] != '.')
        return false;
      ++pos;
    }
    unsigned v = 0;
    std::size_t digits = 0;
    for (; pos < text.size() && text[pos] >= '0' && text[pos] <= '9'; ++pos, ++digits) {
      v = v * 10 + static_cast<unsigned>(text[pos] - '0');
      if (v > 255)
        return false;
    }
    if (digits == 0)
      return false;
    octets[k] = static_cast<std::uint8_t>(v);
  }
  out = net::ipv4_address{octets[0], octets[1], octets[2], octets[3]};
  return true;
}

// Lenient decimal parse (stops at the first non-digit, like atoi for non-negative input).
std::uint64_t parse_uint(reloco::string_view text) noexcept {
  std::uint64_t v = 0;
  for (const char c : text) {
    if (c < '0' || c > '9')
      break;
    v = v * 10 + static_cast<std::uint64_t>(c - '0');
  }
  return v;
}

int usage(const char *argv0) {
  logger.error("usage: {} [--mode ecm|ppp] [--tap NAME] [--ip A.B.C.D] [--port N] [--cycle SEC] [-v]", argv0);
  logger.flush();
  return 2;
}

} // namespace

int main(int argc, char **argv) {
  bool ppp = false;
  const char *tap_name = "structo1";
  unsigned cycle_s = 0;
  app_config app;
  const reloco::span<char *> args{argv, static_cast<std::size_t>(argc)};
  const char *argv0 = args.empty() ? "netstack_usb_demo" : args[0];
  for (std::size_t i = 1; i < args.size(); ++i) {
    const reloco::string_view arg{args[i]};
    const bool has_value = i + 1 < args.size();
    if (arg == "--mode" && has_value) {
      const reloco::string_view mode{args[++i]};
      if (mode == "ppp")
        ppp = true;
      else if (mode != "ecm")
        return usage(argv0);
    } else if (arg == "--tap" && has_value) {
      tap_name = args[++i];
    } else if (arg == "--ip" && has_value) {
      if (!parse_ip(args[++i], app.ip))
        return usage(argv0);
    } else if (arg == "--port" && has_value) {
      app.port = static_cast<std::uint16_t>(parse_uint(args[++i]));
    } else if (arg == "--cycle" && has_value) {
      cycle_s = static_cast<unsigned>(parse_uint(args[++i]));
    } else if (arg == "-v") {
      logger.set_level(microfmt::log::level::debug);
    } else {
      return usage(argv0);
    }
  }

  // The "wire" side of the fake device.
  reloco::array<char, 128> slave{};
  int fd = ppp ? usbfake::open_pty(slave) : usbfake::open_tap(tap_name);
  if (fd < 0) {
    logger.error("{} failed (errno {}){}", ppp ? "open pty" : "open TAP", errno,
                 ppp ? "" : ": needs /dev/net/tun and CAP_NET_ADMIN or a pre-created interface");
    logger.flush();
    return 1;
  }

  // Declaration order matters: the controller and device must outlive the scheduler (whose coroutines
  // still reference them) and the stack.
  usbfake::ecm_device ecm_dev{fd, "025354555342", &log_eth_frame};
  usbfake::acm_device acm_dev{fd};
  usbfake::device &dev = ppp ? static_cast<usbfake::device &>(acm_dev) : static_cast<usbfake::device &>(ecm_dev);
  usbfake::hcd hcd;
  hw::usb_host_controller_ref hcd_ref{hcd};

  bootldr::scheduler sched;
  sched.set_clock(usbfake::now_ms, nullptr);
  sched.set_idle(usbfake::wait_readable, &fd);

  bootldr::usb_stack_config ucfg;
  ucfg.poll_ms = 20;
  ucfg.debounce_ms = 50;
  bootldr::usb_stack stack{sched, hcd_ref, ucfg};
  stack.set_event_handler(&on_usb_event, nullptr);
  board brd{&hcd, &dev, cycle_s * 1000u, usbfake::now_ms(nullptr) + cycle_s * 1000u};
  if (!stack.add_driver({&match_ecm, &ecm_driver, nullptr, &app}) ||
      !stack.add_driver({&match_acm, &ppp_driver, nullptr, &app}) || !stack.poll_with(&service, &brd) ||
      !stack.start()) {
    logger.error("cannot start the USB stack");
    logger.flush();
    return 1;
  }

  std::signal(SIGINT, on_stop);
  std::signal(SIGTERM, on_stop);
  std::signal(SIGUSR1, on_usr1);
  {
    logger.info("============================================================");
    if (ppp) {
      logger.info(">>> NEXT STEP: run this in another terminal <<<");
      logger.info("    sudo pppd {} 115200 192.168.7.1:192.168.7.2 noauth local nodetach", slave.data());
      logger.info("    (the board keeps retrying until pppd answers)");
      logger.info("then test: echo hi | nc -u -w1 192.168.7.2 {}   # echo; {} = upper-case", app.port, app.port + 1);
    } else {
      logger.info(">>> NEXT STEP: run this in another terminal <<<");
      logger.info("    sudo ip addr add 192.168.78.1/24 dev {} && sudo ip link set {} up", tap_name, tap_name);
      logger.info("then test: echo hi | nc -u -w1 <--ip address> {}   # echo; {} = upper-case", app.port, app.port + 1);
    }
    logger.info("============================================================");
  }
  logger.info("plugging the fake USB {} adapter (kill -USR1 {} toggles the cable)", ppp ? "serial" : "Ethernet",
              ::getpid());
  logger.flush();
  hcd.plug(dev);

  while (!stop_requested) {
    sched.run_once();
    usbfake::wait_readable(&fd);
  }

  // Detach everything and give the driver coroutines a few rounds to clean up before the objects they use go away.
  stack.stop();
  for (int i = 0; i < 50; ++i)
    sched.run_once();
  logger.info("bye");
  logger.flush();
  ::close(fd);
  return 0;
}


#else

int main() { return 0; }

#endif
