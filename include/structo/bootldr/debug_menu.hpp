// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file debug_menu.hpp
 * @brief `structo::bootldr::debug_menu`: a full-screen, keyboard-driven debug menu for the bootloader,
 * running over a `hw::uart_ref` (VT100/ANSI terminal) as a task of a `bootldr::scheduler`. C++20 only.
 *
 * The client registers `menu_item`s (like `shell_command`s: intrusive, owned by the client, no allocation).
 * The menu draws the list, the user moves with Up/Down/Home/End and runs an item with Enter or its hotkey
 * (1-9, then a-z; `q` is reserved). `q` or Ctrl-C leaves the menu. An item is a coroutine, so it can
 * `co_await` a TFTP download, a timer or the editor while the rest of the bootloader keeps running; when it
 * returns, the menu waits for a key and redraws. A menu item can run another `debug_menu` (a submenu) by
 * awaiting `debug_menu::run()`.
 *
 * `wait_for_key()` is the usual way to enter the menu from boot: wait a moment for a key and only then
 * show the menu.
 *
 * ```cpp
 * // An item handler: a noexcept coroutine taking the menu_context.
 * // `ctx.ctx()` is the pointer given at registration; `ctx.print()` formats with microfmt ("{}", "{:x}").
 * static reloco::task<void> item_regs(structo::bootldr::menu_context &ctx) noexcept {
 *   (void)ctx.print("SCTLR_EL1 = {:#x}\n", read_sctlr());
 *   co_return; // the menu then asks "press any key" and redraws
 * }
 *
 * structo::bootldr::debug_menu menu{sched, uart, "Bootloader debug"}; // scheduler, console, title (must outlive it)
 * structo::bootldr::menu_item regs{"Registers", "dump CPU registers", item_regs}; // label, one-line help, handler
 * (void)menu.add(regs);                       // must outlive its registration
 *
 * // Enter the menu only if a key is pressed within 2 seconds of boot (needs sched.set_clock()).
 * // 0 accepts any key; pass e.g. ' ' to require the space bar.
 * if (co_await co_await structo::bootldr::wait_for_key(sched, uart, 2000, 0))
 *   co_await menu.run(menu);                  // or (void)menu.start() to run it as its own task
 * ```
 */

#include <structo/bootldr/scheduler.hpp>
#include <structo/bootldr/text_editor.hpp>
#include <structo/hw/uart_ref.hpp>

#include <reloco/coroutine.hpp>
#include <reloco/error.hpp>
#include <reloco/intrusive_c_tailq.hpp>
#include <reloco/string_view.hpp>

#include <microfmt/formatters/reloco.hpp> // formats reloco::error by name
#include <microfmt/microfmt.hpp>

#include <cstddef>
#include <cstdint>

#if RELOCO_HAS_COROUTINES

namespace structo::bootldr {

class debug_menu;
class menu_context;

/**
 * @brief One entry of a `debug_menu`; owned by the client. Registering links it into the menu (no
 * allocation); destroying it or the menu unlinks it. `label` and `help` are not copied and must outlive it.
 * Do not remove or destroy an item while its handler runs.
 */
class menu_item {
public:
  /** @brief Handler coroutine. Fail with `co_await reloco::unexpected(err)` to make the menu print the error. */
  using handler_fn = reloco::task<void> (*)(menu_context &) noexcept;

  /** @param label text shown in the list. @param help one-line description shown after it. @param ctx returned by
   * `menu_context::ctx()`. */
  menu_item(reloco::string_view label, reloco::string_view help, handler_fn handler, void *ctx = nullptr) noexcept
      : label_(label), help_(help), handler_(handler), ctx_(ctx) {}
  menu_item(const menu_item &) = delete;
  menu_item &operator=(const menu_item &) = delete;
  inline ~menu_item();

  [[nodiscard]] reloco::string_view label() const noexcept { return label_; }
  [[nodiscard]] reloco::string_view help() const noexcept { return help_; }
  [[nodiscard]] bool registered() const noexcept { return menu_ != nullptr; }

private:
  friend class debug_menu;

  // Same layout as FreeBSD TAILQ_ENTRY, as expected by reloco::c_tailq.
  struct {
    menu_item *next = nullptr;
    menu_item **prev = nullptr;
  } link_;

  reloco::string_view label_;
  reloco::string_view help_;
  handler_fn handler_;
  void *ctx_;
  debug_menu *menu_ = nullptr;
};

/**
 * @brief Waits up to `timeout_ms` for a byte on `uart`. Resolves to `true` as soon as `key` (or any byte
 * when `key == 0`) arrives, `false` on timeout. Other bytes are discarded while waiting. Needs the
 * scheduler clock (`set_clock()`); without one the wait ends at the first idle poll. Fails with the UART's error.
 */
[[nodiscard]] inline reloco::task<bool> wait_for_key(scheduler &sched, hw::uart_ref uart, std::uint64_t timeout_ms,
                                                     char key = 0) noexcept {
  const std::uint64_t deadline = sched.now_ms() + timeout_ms;
  for (;;) {
    auto ready = uart.rx_ready();
    if (!ready)
      co_await reloco::unexpected(ready.error());
    if (!*ready) {
      if (sched.now_ms() >= deadline)
        co_return false;
      co_await sched.yield();
      continue;
    }
    auto got = uart.try_get_byte();
    if (!got) {
      if (got.error() == reloco::error::try_again)
        continue;
      co_await reloco::unexpected(got.error());
    }
    if (key == 0 || *got == static_cast<std::uint8_t>(key))
      co_return true;
  }
}

/** @brief The interactive menu. Owned by the client; `start()` spawns it as a task, the destructor stops it. */
class debug_menu {
public:
  /** @param title shown on the first line; must outlive the menu. */
  debug_menu(scheduler &sched, hw::uart_ref uart, reloco::string_view title = "Debug menu") noexcept
      : sched_(&sched), uart_(uart), title_(title) {}
  debug_menu(const debug_menu &) = delete;
  debug_menu &operator=(const debug_menu &) = delete;
  ~debug_menu() {
    stop();
    while (menu_item *i = items_.pop_front())
      i->menu_ = nullptr;
  }

  [[nodiscard]] scheduler &sched() const noexcept { return *sched_; }
  [[nodiscard]] std::size_t size() const noexcept { return count_; }

  /**
   * @brief Registers `item` at the end of the list. `error::invalid_state` if it is already registered
   * (here or elsewhere), `error::invalid_argument` for an empty label.
   */
  [[nodiscard]] reloco::result<void> add(menu_item &item) noexcept {
    if (item.menu_)
      return reloco::unexpected(reloco::error::invalid_state);
    if (item.label_.empty())
      return reloco::unexpected(reloco::error::invalid_argument);
    items_.push_back(item);
    item.menu_ = this;
    ++count_;
    return {};
  }

  /** @brief Unregisters `item` (no-op if it is not registered here). */
  void remove(menu_item &item) noexcept {
    if (item.menu_ != this)
      return;
    items_.remove(item);
    item.menu_ = nullptr;
    --count_;
  }

  /** @brief Spawns the menu as a task. `error::invalid_state` if already running. */
  [[nodiscard]] reloco::result<void> start() noexcept {
    if (running_)
      return reloco::unexpected(reloco::error::invalid_state);
    auto id = sched_->spawn(run_task(*this));
    if (!id)
      return reloco::unexpected(id.error());
    id_ = *id;
    running_ = true;
    return {};
  }

  /** @brief Cancels the menu task (an item that is currently running is destroyed with it). */
  void stop() noexcept {
    if (!running_)
      return;
    running_ = false;
    (void)sched_->cancel(id_);
  }

  [[nodiscard]] bool running() const noexcept { return running_; }

  /**
   * @brief Runs the menu until the user quits (`q`/Ctrl-C), for awaiting from another task (boot code, a
   * parent menu's item). Fails with the UART's error. Do not run the same menu twice at once.
   */
  [[nodiscard]] static reloco::task<void> run(debug_menu &m) noexcept {
    key_decoder dec;
    std::size_t sel = 0;
    bool redraw = true;
    for (;;) {
      if (redraw) {
        if (auto r = m.render(sel); !r)
          co_await reloco::unexpected(r.error());
        redraw = false;
      }
      auto k = co_await read_key(*m.sched_, m.uart_, dec);
      if (!k)
        co_await reloco::unexpected(k.error());
      const key_event ev = *k;

      std::size_t run_idx = m.count_; // != count_ means "run this item"
      switch (ev.code) {
      case key_code::up:
        if (m.count_ != 0)
          sel = sel == 0 ? m.count_ - 1 : sel - 1;
        redraw = true;
        break;
      case key_code::down:
        if (m.count_ != 0)
          sel = sel + 1 >= m.count_ ? 0 : sel + 1;
        redraw = true;
        break;
      case key_code::home:
      case key_code::page_up:
        sel = 0;
        redraw = true;
        break;
      case key_code::end:
      case key_code::page_down:
        sel = m.count_ == 0 ? 0 : m.count_ - 1;
        redraw = true;
        break;
      case key_code::redraw:
        redraw = true;
        break;
      case key_code::enter:
        if (m.count_ != 0)
          run_idx = sel;
        break;
      case key_code::character: {
        if (ev.ch == 'q' || ev.ch == 'Q') {
          (void)m.uart_.write_string("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
          co_return;
        }
        for (std::size_t i = 0; i < m.count_; ++i) {
          if (hotkey(i) == ev.ch) {
            sel = i;
            run_idx = i;
            break;
          }
        }
        break;
      }
      case key_code::cancel: // Ctrl-C (mapped by read_key) or Ctrl-X
        (void)m.uart_.write_string("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
        co_return;
      default:
        break;
      }

      if (run_idx != m.count_) {
        co_await co_await run_item(m, run_idx, dec);
        redraw = true;
      }
    }
  }

private:
  friend class menu_context;

  // Ctrl-C is not a `key_code`; report it as `cancel`.
  static reloco::task<key_event> read_key(scheduler &sched, hw::uart_ref uart, key_decoder &dec) noexcept {
    for (;;) {
      auto ready = uart.rx_ready();
      if (!ready)
        co_await reloco::unexpected(ready.error());
      if (!*ready) {
        co_await sched.yield();
        continue;
      }
      auto got = uart.try_get_byte();
      if (!got) {
        if (got.error() == reloco::error::try_again)
          continue;
        co_await reloco::unexpected(got.error());
      }
      if (*got == 0x03)
        co_return key_event{key_code::cancel, 0};
      const key_event ev = dec.feed(*got);
      if (ev.code != key_code::none)
        co_return ev;
    }
  }

  // Item `i`'s hotkey: '1'..'9', then 'a'..'z' without 'q'; 0 beyond that.
  static char hotkey(std::size_t i) noexcept {
    if (i < 9)
      return static_cast<char>('1' + i);
    char c = static_cast<char>('a' + (i - 9));
    if (i - 9 >= 16) // skip 'q'
      c = static_cast<char>(c + 1);
    return (c >= 'a' && c <= 'z') ? c : char{0};
  }

  [[nodiscard]] menu_item *nth(std::size_t n) noexcept {
    for (menu_item &i : items_) {
      if (n-- == 0)
        return &i;
    }
    return nullptr;
  }

  [[nodiscard]] reloco::result<void> render(std::size_t sel) noexcept {
    if (auto r = uart_.write_string("\x1b[0m\x1b[2J\x1b[H\x1b[?25l"); !r)
      return r;
    if (auto r = print("\x1b[1m{}\x1b[0m\n", title_); !r)
      return r;
    if (auto r = write("------------------------------\n"); !r)
      return r;
    std::size_t idx = 0;
    for (menu_item &it : items_) {
      const char hk = hotkey(idx);
      const bool selected = idx == sel;
      if (selected)
        (void)write("\x1b[7m");
      auto r = print(" {} {}  {}\x1b[0m\n", hk != 0 ? hk : ' ', it.label_, it.help_);
      if (!r)
        return r;
      ++idx;
    }
    return write("\nUp/Down select, Enter run, hotkey run, q quit\n");
  }

  // Runs one item, then waits for a key so its output stays on screen.
  // Defined after menu_context, which it needs complete.
  static inline reloco::task<void> run_item(debug_menu &m, std::size_t idx, key_decoder &dec) noexcept;

  static reloco::task<void> run_task(debug_menu &m) noexcept {
    co_await co_await run(m);
    m.running_ = false;
  }

  [[nodiscard]] reloco::result<void> write(reloco::string_view text) const noexcept { return uart_.write_string(text); }

  template <std::size_t PrintMax = 128, typename... Args>
  [[nodiscard]] reloco::result<void> print(microfmt::string_view fmt, const Args &...args) const noexcept {
    const auto text = microfmt::format<PrintMax>(fmt, args...);
    return uart_.write_string(text.view());
  }

  scheduler *sched_;
  hw::uart_ref uart_;
  reloco::string_view title_;
  reloco::c_tailq<menu_item, &menu_item::link_> items_;
  std::size_t count_ = 0;
  task_id id_{};
  bool running_ = false;
};

inline menu_item::~menu_item() {
  if (menu_)
    menu_->remove(*this);
}

/** @brief What an item handler receives: console output, key input and its registration context. */
class menu_context {
public:
  menu_context(debug_menu &m, key_decoder &dec, void *ctx) noexcept : menu_(&m), dec_(&dec), ctx_(ctx) {}

  [[nodiscard]] debug_menu &menu() const noexcept { return *menu_; }
  [[nodiscard]] scheduler &sched() const noexcept { return menu_->sched(); }
  /** @brief The pointer given to the `menu_item` constructor. */
  [[nodiscard]] void *ctx() const noexcept { return ctx_; }

  /** @brief Writes text to the console (`'\n'` becomes `"\r\n"`). */
  [[nodiscard]] reloco::result<void> write(reloco::string_view text) const noexcept { return menu_->write(text); }
  /** @brief Formats with microfmt and writes the result; output beyond `PrintMax` characters is truncated. */
  template <std::size_t PrintMax = 128, typename... Args>
  [[nodiscard]] reloco::result<void> print(microfmt::string_view fmt, const Args &...args) const noexcept {
    return menu_->template print<PrintMax>(fmt, args...);
  }

  /** @brief Waits for the next key (Ctrl-C arrives as `key_code::cancel`); other tasks run meanwhile. */
  [[nodiscard]] reloco::task<key_event> read_key() const noexcept {
    return debug_menu::read_key(*menu_->sched_, menu_->uart_, *dec_);
  }

private:
  debug_menu *menu_;
  key_decoder *dec_;
  void *ctx_;
};

inline reloco::task<void> debug_menu::run_item(debug_menu &m, std::size_t idx, key_decoder &dec) noexcept {
  menu_item *it = m.nth(idx);
  if (!it)
    co_return;
  (void)m.write("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
  (void)m.print("== {} ==\n\n", it->label_);
  menu_context ctx{m, dec, it->ctx_};
  auto r = co_await it->handler_(ctx);
  if (!r)
    (void)m.print("\nerror: {}\n", r.error());
  (void)m.write("\n[press any key to return]");
  auto k = co_await read_key(*m.sched_, m.uart_, dec);
  if (!k)
    co_await reloco::unexpected(k.error());
}

} // namespace structo::bootldr

#endif // RELOCO_HAS_COROUTINES
