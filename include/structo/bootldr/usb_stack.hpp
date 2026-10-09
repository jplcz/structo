// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file usb_stack.hpp
 * @brief `structo::bootldr::usb_stack`: USB host hot-plug management running on a `bootldr::scheduler`, in the
 * spirit of `netstack`. C++20 only. It watches the root ports, enumerates what is plugged in, finds a registered
 * *driver* for it by calling its `match` callback, and **spawns the driver's coroutine** to manage the device.
 * When the device is unplugged the coroutine is not killed: all its transfers fail with
 * `error::operation_canceled`, `usb_attached_device::gone()` turns true and `gone_event()` is set, so the
 * coroutine can clean up by itself and return.
 *
 * Memory comes from a heap allocator (`reloco::allocator_ref`, default: the scheduler's): every attached device is a
 * `reloco::shared_ptr<usb_attached_device>`. The stack holds one reference while the device is plugged in and each
 * driver coroutine gets its own, so the device object (descriptors, pipes) stays valid until the coroutine ends,
 * even after the unplug.
 *
 * The controller driver must complete transfers from the scheduler thread (e.g. from a poller registered with
 * `poll_with()`), because the scheduler has no locking and a completion resumes the waiting coroutine inline.
 * The controller handle passed in must outlive the stack and every device handle.
 *
 * @code
 * // A driver: `match` looks at the descriptors, `attach` returns the coroutine that manages the device.
 * bool match_serial(void *, const structo::usb::usb_device &d) noexcept {
 *   return d.config().find_interface(structo::usb::usb_class::cdc, structo::usb::cdc::subclass_acm).has_value();
 * }
 * reloco::task<void> serial_task(void *, structo::bootldr::usb_stack &stack,
 *                                structo::bootldr::usb_device_ptr dev) noexcept {  // the ptr keeps `dev` alive
 *   structo::usb::cdc_acm<256, 256> serial;
 *   if (!co_await serial.attach(dev->device()))              // any transfer error: give up
 *     co_return;
 *   auto &sched = stack.sched();
 *   auto rx = sched.spawn(serial.run_rx(), structo::bootldr::spawn_mode::joinable); // pumps end by themselves
 *   auto tx = sched.spawn(serial.run_tx(), structo::bootldr::spawn_mode::joinable); // when transfers fail
 *   // ... use structo::hw::uart_ref{serial} while the device is present ...
 *   co_await dev->gone_event().wait();                        // set when the device is unplugged
 *   serial.detach();                                          // wake the pumps...
 *   if (rx) (void)co_await sched.join(*rx);                   // ...and wait for them before `serial` dies
 *   if (tx) (void)co_await sched.join(*tx);
 * }
 *
 * structo::bootldr::usb_stack usb{sched, hcd_ref};            // hcd_ref: hw::usb_host_controller_ref
 * usb.add_driver({&match_serial, &serial_task});              // register as many drivers as you like
 * usb.poll_with(&service_hcd, &my_hcd);                       // service the controller once per round
 * usb.start();                                                // spawn one watcher task per root port
 * sched.run();
 * @endcode
 */

#include "scheduler.hpp"

#include "../usb/usb_host.hpp"

#if RELOCO_HAS_COROUTINES

#include <reloco/allocator.hpp>
#include <reloco/shared_ptr.hpp>
#include <reloco/vector.hpp>

#include <new>

namespace structo::bootldr {

/**
 * @brief Per-device pass-through controller: remembers the transfers in flight so that, on unplug, they can be
 * completed with `disconnected` even if the controller driver does not do it, and rejects new ones.
 */
class usb_device_gate {
public:
  usb_device_gate(hw::usb_host_controller_ref real, reloco::allocator_ref alloc) noexcept
      : real_(real), pending_(alloc) {}
  usb_device_gate(const usb_device_gate &) = delete;
  usb_device_gate &operator=(const usb_device_gate &) = delete;

  [[nodiscard]] unsigned port_count() const noexcept { return real_.port_count(); }
  [[nodiscard]] reloco::result<hw::usb_port_status> port_status(unsigned port) const noexcept {
    return real_.port_status(port);
  }
  [[nodiscard]] reloco::task<void> reset_port(unsigned port) const noexcept { return real_.reset_port(port); }
  void reset_data_toggle(const hw::usb_pipe &p) const noexcept { real_.reset_data_toggle(p); }

  reloco::result<void> submit(hw::usb_transfer &t) noexcept {
    if (gone_)
      return reloco::unexpected(reloco::error::not_found);
    if (auto r = pending_.try_push_back(&t); !r)
      return r;
    t.done_hook = &on_done;
    t.done_ctx = this;
    auto r = real_.submit(t);
    if (!r) {
      forget(&t);
      t.done_hook = nullptr;
    }
    return r;
  }

  void cancel(hw::usb_transfer &t) noexcept {
    forget(&t);
    real_.cancel(t);
  }

  /** @brief Marks the device gone and fails every transfer in flight (their coroutines resume with an error). */
  void disconnect() noexcept {
    gone_ = true;
    while (pending_.size() != 0) {
      hw::usb_transfer *t = pending_[pending_.size() - 1];
      forget(t);
      real_.cancel(*t);
      t->complete(hw::usb_status::disconnected, 0);
    }
  }

  [[nodiscard]] bool gone() const noexcept { return gone_; }

private:
  static void on_done(void *self, hw::usb_transfer &t) noexcept { static_cast<usb_device_gate *>(self)->forget(&t); }

  void forget(hw::usb_transfer *t) noexcept {
    for (std::size_t i = 0; i < pending_.size(); ++i)
      if (pending_[i] == t) {
        (void)pending_.try_erase_at(i);
        return;
      }
  }

  hw::usb_host_controller_ref real_;
  reloco::vector<hw::usb_transfer *> pending_;
  bool gone_ = false;
};

} // namespace structo::bootldr

template <> struct structo::hw::usb_host_traits<structo::bootldr::usb_device_gate> {
  using gate = structo::bootldr::usb_device_gate;
  static unsigned port_count(gate &g) noexcept { return g.port_count(); }
  static reloco::result<usb_port_status> port_status(gate &g, unsigned p) noexcept { return g.port_status(p); }
  static reloco::task<void> reset_port(gate &g, unsigned p) noexcept { return g.reset_port(p); }
  static reloco::result<void> submit(gate &g, usb_transfer &t) noexcept { return g.submit(t); }
  static void cancel(gate &g, usb_transfer &t) noexcept { g.cancel(t); }
  static void reset_data_toggle(gate &g, const usb_pipe &p) noexcept { g.reset_data_toggle(p); }
};

namespace structo::bootldr {

class usb_stack;

/** @brief A device the stack enumerated. Shared between the stack and the coroutine(s) managing it. */
class usb_attached_device {
public:
  usb_attached_device(scheduler &sched, hw::usb_host_controller_ref real, reloco::allocator_ref alloc,
                      unsigned port) noexcept
      : gate_(real, alloc), dev_(hw::usb_host_controller_ref{gate_}, {}), cfg_(alloc), gone_event_(sched), port_(port) {
  }
  usb_attached_device(const usb_attached_device &) = delete;
  usb_attached_device &operator=(const usb_attached_device &) = delete;

  /** @brief The enumerated device: descriptors, `control()`, pipes, string reads. Transfers fail once it is gone. */
  [[nodiscard]] usb::usb_device &device() noexcept { return dev_; }
  [[nodiscard]] unsigned port() const noexcept { return port_; }
  /** @brief True after the device was unplugged. */
  [[nodiscard]] bool gone() const noexcept { return gate_.gone(); }
  /** @brief Set when the device is unplugged: `co_await dev->gone_event().wait()` in a driver coroutine. */
  [[nodiscard]] scheduler::event &gone_event() noexcept { return gone_event_; }

private:
  friend class usb_stack;

  reloco::result<void> init(std::size_t config_bytes) noexcept {
    if (auto r = cfg_.try_resize(config_bytes, std::uint8_t{0}); !r)
      return r;
    dev_.set_config_storage(reloco::span<std::uint8_t>(cfg_.data(), cfg_.size()));
    return {};
  }

  usb_device_gate gate_;
  usb::usb_device dev_;
  reloco::vector<std::uint8_t> cfg_;
  scheduler::event gone_event_;
  unsigned port_;
};

using usb_device_ptr = reloco::shared_ptr<usb_attached_device>;

/** @brief A device driver: recognises devices and supplies the coroutine that manages one. */
struct usb_driver {
  /** @brief True if this driver handles `dev` (look at `dev.descriptor()` / `dev.config()`). */
  using match_fn = bool (*)(void *ctx, const usb::usb_device &dev) noexcept;
  /** @brief Returns the coroutine managing `dev`; the stack spawns it. Keep `dev` (a shared handle) in the frame. */
  using attach_fn = reloco::task<void> (*)(void *ctx, usb_stack &stack, usb_device_ptr dev) noexcept;
  /** @brief Optional, synchronous: called right after unplug, before the coroutine resumes with errors. */
  using detach_fn = void (*)(void *ctx, usb_attached_device &dev) noexcept;

  match_fn match = nullptr;
  attach_fn attach = nullptr;
  detach_fn detached = nullptr;
  void *ctx = nullptr;
};

enum class usb_event_kind : std::uint8_t {
  attached,          ///< Enumerated and handed to a driver.
  unclaimed,         ///< Enumerated but no driver matched; the device stays configured until unplugged.
  detached,          ///< Unplugged.
  enumeration_failed ///< Something was plugged in but could not be enumerated (the error is passed along).
};

/** @brief Static configuration of a `usb_stack`. */
struct usb_stack_config {
  std::uint32_t poll_ms = 100;     ///< Port status poll period (also the latency if `notify_port_change` is unused).
  std::uint32_t debounce_ms = 100; ///< Wait after a connect before resetting the port.
  std::size_t max_config_bytes = 1024; ///< Configuration descriptor buffer per device (larger ones fail to enumerate).
};

class usb_stack {
public:
  /** @brief Observer of plug/unplug events. `dev` is null for `enumeration_failed`; `status` is the error then. */
  using event_fn = void (*)(void *ctx, usb_event_kind kind, unsigned port, usb_attached_device *dev,
                            const reloco::result<void> &status) noexcept;

  usb_stack(scheduler &sched, hw::usb_host_controller_ref hcd, const usb_stack_config &cfg = {},
            reloco::allocator_ref alloc = reloco::default_allocator()) noexcept
      : sched_(&sched), hcd_(hcd), cfg_(cfg), alloc_(alloc), host_(hcd, &sleep_ms, this), drivers_(alloc),
        ports_(alloc), tasks_(alloc), change_(sched) {}
  usb_stack(const usb_stack &) = delete;
  usb_stack &operator=(const usb_stack &) = delete;
  ~usb_stack() { stop(); }

  /** @brief The scheduler, for driver coroutines that need to spawn or sleep. */
  [[nodiscard]] scheduler &sched() noexcept { return *sched_; }

  /** @brief Registers a driver (first match wins, in registration order). May be called while running. */
  [[nodiscard]] reloco::result<void> add_driver(const usb_driver &d) noexcept {
    if (!d.match || !d.attach)
      return reloco::unexpected(reloco::error::invalid_argument);
    return drivers_.try_push_back(d);
  }

  void set_event_handler(event_fn fn, void *ctx) noexcept {
    event_ = fn;
    event_ctx_ = ctx;
  }

  /** @brief Calls `fn(ctx)` at the start of every scheduler round: service the controller (complete finished
   * transfers). */
  [[nodiscard]] reloco::result<void> poll_with(scheduler::hook_fn fn, void *ctx) noexcept {
    return sched_->add_poller(fn, ctx);
  }

  /** @brief Spawns one watcher task per root port. Needs the scheduler clock. `invalid_state` if already started. */
  [[nodiscard]] reloco::result<void> start() noexcept {
    if (running_)
      return reloco::unexpected(reloco::error::invalid_state);
    const unsigned n = hcd_.port_count();
    if (auto r = ports_.try_resize(n); !r)
      return r;
    for (unsigned p = 0; p < n; ++p) {
      auto id = sched_->spawn(port_loop(*this, p));
      if (!id) {
        stop();
        return reloco::unexpected(id.error());
      }
      (void)tasks_.try_push_back(*id);
    }
    running_ = true;
    return {};
  }

  /** @brief Cancels the watchers and detaches every device (their coroutines get errors). Also done by the destructor.
   */
  void stop() noexcept {
    for (std::size_t i = 0; i < tasks_.size(); ++i)
      (void)sched_->cancel(tasks_[i]);
    while (tasks_.size() != 0)
      tasks_.pop_back();
    for (unsigned p = 0; p < ports_.size(); ++p)
      detach_port(p);
    running_ = false;
  }

  /** @brief Wake the watchers now (call from your root-hub/port-change handling, on the scheduler thread). */
  void notify_port_change() noexcept { change_.set(); }

  /** @brief The device currently attached to `port` (null if none). */
  [[nodiscard]] usb_device_ptr attached(unsigned port) const noexcept {
    return port < ports_.size() ? ports_[port].dev : usb_device_ptr{};
  }

private:
  struct port_state {
    usb_device_ptr dev;
    const usb_driver *driver = nullptr;
    bool failed = false;
  };

  static reloco::task<void> sleep_ms(void *self, unsigned ms) noexcept {
    (void)co_await static_cast<usb_stack *>(self)->sched_->sleep_for(ms);
  }

  void emit(usb_event_kind k, unsigned port, usb_attached_device *d, const reloco::result<void> &st) noexcept {
    if (event_)
      event_(event_ctx_, k, port, d, st);
  }

  void detach_port(unsigned port) noexcept {
    port_state &ps = ports_[port];
    if (!ps.dev)
      return;
    usb_device_ptr dev = std::move(ps.dev);
    const usb_driver *drv = ps.driver;
    ps.dev = usb_device_ptr{};
    ps.driver = nullptr;
    host_.release(dev->device()); // the address is free for the next device right away
    if (drv && drv->detached)
      drv->detached(drv->ctx, *dev);
    emit(usb_event_kind::detached, port, dev.get(), {});
    dev->gate_.disconnect(); // fails transfers in flight: driver coroutines resume with errors
    dev->gone_event_.set();
  }

  reloco::task<void> attach_port(unsigned port) noexcept {
    while (enumerating_) // enumeration uses address 0: one device at a time
      co_await sched_->yield();
    enumerating_ = true;
    reloco::result<void> st{};
    usb_device_ptr dev;
    do {
      auto p = reloco::try_allocate_shared<usb_attached_device>(alloc_, *sched_, hcd_, alloc_, port);
      if (!p) {
        st = reloco::unexpected(p.error());
        break;
      }
      dev = std::move(*p);
      if (st = dev->init(cfg_.max_config_bytes); !st)
        break;
      st = co_await host_.enumerate(port, dev->device());
      if (!st)
        host_.release(dev->device());
    } while (false);
    enumerating_ = false;
    port_state &ps = ports_[port];
    if (!st) {
      ps.failed = true; // do not retry until the device is unplugged
      emit(usb_event_kind::enumeration_failed, port, nullptr, st);
      co_return;
    }

    ps.dev = dev;
    for (std::size_t i = 0; i < drivers_.size(); ++i) {
      const usb_driver &d = drivers_[i];
      if (!d.match(d.ctx, dev->device()))
        continue;
      auto id = sched_->spawn(d.attach(d.ctx, *this, dev));
      if (!id) {
        detach_port(port);
        ps.failed = true;
        emit(usb_event_kind::enumeration_failed, port, nullptr, reloco::unexpected(id.error()));
        co_return;
      }
      ps.driver = &d;
      emit(usb_event_kind::attached, port, dev.get(), {});
      co_return;
    }
    emit(usb_event_kind::unclaimed, port, dev.get(), {});
  }

  static reloco::task<void> port_loop(usb_stack &s, unsigned port) noexcept {
    for (;;) {
      auto st = s.hcd_.port_status(port);
      if (st) {
        port_state &ps = s.ports_[port];
        // A change flag while a device is present means unplug (maybe followed by a quick re-plug).
        if (ps.dev && (!st->connected || st->changed))
          s.detach_port(port);
        if (!st->connected)
          ps.failed = false;
        if (st->connected && !ps.dev && !ps.failed) {
          (void)co_await s.sched_->sleep_for(s.cfg_.debounce_ms);
          auto again = s.hcd_.port_status(port);
          if (again && again->connected)
            (void)co_await s.attach_port(port);
        }
      }
      auto w = co_await s.change_.wait_for(s.cfg_.poll_ms);
      if (!w && w.error() != reloco::error::timed_out)
        co_await s.sched_->yield(); // no clock: do not spin faster than once per round
      s.change_.reset();
    }
  }

  scheduler *sched_;
  hw::usb_host_controller_ref hcd_;
  usb_stack_config cfg_;
  reloco::allocator_ref alloc_;
  usb::usb_host host_;
  reloco::vector<usb_driver> drivers_;
  reloco::vector<port_state> ports_;
  reloco::vector<task_id> tasks_;
  scheduler::event change_;
  event_fn event_ = nullptr;
  void *event_ctx_ = nullptr;
  bool running_ = false;
  bool enumerating_ = false;
};

} // namespace structo::bootldr

#endif // RELOCO_HAS_COROUTINES
