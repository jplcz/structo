// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

// Linux demo: a USB serial port (CDC-ACM) used as a log console. bootldr::usb_stack detects the (fake)
// device, spawns a driver coroutine, and that coroutine routes a microfmt logger to the device's UART, so
// every log message the "board" prints shows up on the host's serial terminal. The USB device is
// simulated by usb_linux_fake.hpp and bridged to a pty. For controlled test/CI/development setups only.
//
//   usb_serial_log_demo [--interval MS] [--cycle SEC]
//
//   ./usb_serial_log_demo                 # prints the pty path, e.g. /dev/pts/7
//   screen /dev/pts/7                     # (or minicom/picocom) shows the board's log lines
//
// Unplug/replug: `kill -USR1 <pid>` toggles the fake cable; `--cycle SEC` does it every SEC seconds. When the
// cable is pulled the driver coroutine notices through the device's gone_event and ends by itself; a new
// one starts on replug (the host terminal sees a fresh "console attached" banner).

#include <structo/bootldr/usb_stack.hpp>
#include <structo/hw/uart_ref.hpp>
#include <structo/usb/cdc_acm.hpp>

#include <microfmt/log/logger.hpp>

#if RELOCO_HAS_COROUTINES

#include "usb_linux_fake.hpp"

#include <csignal>
#include <cstdlib>
#include <reloco/array.hpp>
#include <reloco/span.hpp>
#include <reloco/string_view.hpp>

namespace {

// A microfmt log sink that writes each record to a UART as one "\r\n"-terminated line. Bytes the UART
// cannot take right now are dropped, like any console sink on a stalled link.
struct uart_log_sink_tag {};
const std::uint64_t boot_ms = usbfake::now_ms(nullptr);

class uart_log_sink {
public:
  explicit uart_log_sink(structo::hw::uart_ref uart) noexcept : uart_(uart) {}

  [[nodiscard]] microfmt::log::log_sink as_sink() noexcept;
  void log_impl(const microfmt::log::log_msg &msg) noexcept;

private:
  structo::hw::uart_ref uart_;
};

} // namespace

// The traits must be visible before as_sink() instantiates the sink handle, hence the out-of-line members below.
template <> struct microfmt::log::log_sink_traits<uart_log_sink_tag> {
  using context_type = uart_log_sink;
  static void log(value_ref<context_type> ctx, const log_msg &msg) noexcept { ctx->log_impl(msg); }
};

microfmt::log::log_sink uart_log_sink::as_sink() noexcept {
  return microfmt::log::log_sink(uart_log_sink_tag{}, *this);
}

void uart_log_sink::log_impl(const microfmt::log::log_msg &msg) noexcept {
  microfmt::buffer_sink<256> buf;
  auto out = buf.as_sink();
  // Board uptime, not wall-clock time: a bare-metal board usually has no RTC.
  const std::uint64_t up = usbfake::now_ms(nullptr) - boot_ms;
  microfmt::format_to(out, "[{:6}.{:03}] [{}] [{}] {}\r\n", up / 1000, up % 1000, msg.logger_name,
                      microfmt::log::to_short_string(msg.lvl), msg.payload);
  for (char c : buf.view())
    if (!uart_.try_put_byte(static_cast<std::uint8_t>(c)))
      break;
}

using namespace structo;

namespace {

microfmt::log::stdout_color_sink<512> console;
microfmt::log::basic_logger<1, 1024> logger("usbhost", console.as_sink());

volatile std::sig_atomic_t stop_requested = 0;
volatile std::sig_atomic_t toggle_requested = 0;
void on_stop(int) { stop_requested = 1; }
void on_usr1(int) { toggle_requested = 1; }

struct app_config {
  std::uint64_t interval_ms = 1000;
};

bool match_acm(void *, const usb::usb_device &d) noexcept {
  return d.config().find_interface(usb::usb_class::cdc, usb::cdc::subclass_acm).has_value();
}

// Spawned by the usb_stack for every CDC-ACM device. Owns the UART and its logger; returns after the unplug.
reloco::task<void> console_driver(void *ctx, bootldr::usb_stack &stack, bootldr::usb_device_ptr dev) noexcept {
  const auto &app = *static_cast<const app_config *>(ctx);
  auto &sched = stack.sched();

  usb::cdc_acm<256, 1024> serial;
  if (auto r = co_await serial.attach(dev->device()); !r) {
    logger.error("ACM attach failed: error {}", static_cast<int>(r.error()));
    co_return;
  }
  // The pumps move bytes between the bulk endpoints and the UART rings; they end when `serial` is detached.
  auto rx = sched.spawn(serial.run_rx(), bootldr::spawn_mode::joinable);
  auto tx = sched.spawn(serial.run_tx(), bootldr::spawn_mode::joinable);

  {
    hw::uart_ref uart{serial};
    uart_log_sink sink{uart};
    microfmt::log::basic_logger<1, 512> board("board", sink.as_sink());

    board.info("console attached: {}", "structo USB serial log demo");
    const std::uint64_t start = usbfake::now_ms(nullptr);
    unsigned n = 1;
    for (;; ++n) {
      const std::uint64_t up = usbfake::now_ms(nullptr) - start;
      board.info("heartbeat #{}, uptime {} ms", n, up);
      if (n % 5 == 0)
        board.warn("sample warning after {} heartbeats", n);
      if (n % 7 == 0)
        board.error("sample error after {} heartbeats", n);
      board.flush();

      // Sleep for one interval, or wake early when the device is unplugged (gone_event set).
      auto woke = co_await dev->gone_event().wait_for(app.interval_ms);
      if (woke)
        break;

      // Anything the host types is discarded: this console is output-only.
      while (uart.try_get_byte()) {
      }
    }
    logger.info("USB console gone after {} heartbeats", n);
  }

  serial.detach(); // wakes the pumps; they must finish before `serial` is destroyed
  if (rx)
    (void)co_await sched.join(*rx);
  if (tx)
    (void)co_await sched.join(*tx);
}

void on_usb_event(void *, bootldr::usb_event_kind kind, unsigned port, bootldr::usb_attached_device *dev,
                  const reloco::result<void> &st) noexcept {
  switch (kind) {
  case bootldr::usb_event_kind::attached: {
    const auto &d = dev->device().descriptor();
    logger.info("device attached on port {}: {:04x}:{:04x}, console driver started", port, d.vendor_id, d.product_id);
    break;
  }
  case bootldr::usb_event_kind::unclaimed:
    logger.info("device attached on port {}, no driver", port);
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

struct board_env {
  usbfake::hcd *hcd;
  usbfake::device *dev;
  std::uint64_t cycle_ms = 0;
  std::uint64_t next_toggle = 0;
};

// Runs once per scheduler round: services the fake controller and plays with the cable.
void service(void *p) noexcept {
  auto &b = *static_cast<board_env *>(p);
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
  logger.error("usage: {} [--interval MS] [--cycle SEC]", argv0);
  logger.flush();
  return 2;
}

} // namespace

int main(int argc, char **argv) {
  app_config app;
  unsigned cycle_s = 0;
  const reloco::span<char *> args{argv, static_cast<std::size_t>(argc)};
  const char *argv0 = args.empty() ? "usb_serial_log_demo" : args[0];
  for (std::size_t i = 1; i < args.size(); ++i) {
    const reloco::string_view arg{args[i]};
    if (arg == "--interval" && i + 1 < args.size())
      app.interval_ms = parse_uint(args[++i]);
    else if (arg == "--cycle" && i + 1 < args.size())
      cycle_s = static_cast<unsigned>(parse_uint(args[++i]));
    else
      return usage(argv0);
  }
  if (app.interval_ms == 0)
    return usage(argv0);

  reloco::array<char, 128> slave{};
  int fd = usbfake::open_pty(slave);
  if (fd < 0) {
    logger.error("open pty failed (errno {})", errno);
    logger.flush();
    return 1;
  }

  // Declaration order matters: the controller and device must outlive the scheduler and the stack.
  usbfake::acm_device dev{fd};
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
  board_env env{&hcd, &dev, cycle_s * 1000u, usbfake::now_ms(nullptr) + cycle_s * 1000u};
  if (!stack.add_driver({&match_acm, &console_driver, nullptr, &app}) || !stack.poll_with(&service, &env) ||
      !stack.start()) {
    logger.error("cannot start the USB stack");
    logger.flush();
    return 1;
  }

  std::signal(SIGINT, on_stop);
  std::signal(SIGTERM, on_stop);
  std::signal(SIGUSR1, on_usr1);
  logger.info("============================================================");
  logger.info(">>> NEXT STEP: run this in another terminal <<<");
  logger.info("    screen {} 115200", slave.data());
  logger.info("    (log lines from the fake USB UART appear there)");
  logger.info("============================================================");
  logger.info("plugging the fake USB serial device (kill -USR1 {} toggles the cable)", ::getpid());
  logger.flush();
  hcd.plug(dev);

  while (!stop_requested) {
    sched.run_once();
    usbfake::wait_readable(&fd);
  }

  // Detach everything and give the driver coroutine a few rounds to clean up before the objects it uses go away.
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
