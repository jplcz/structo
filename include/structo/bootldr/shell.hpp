// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file shell.hpp
 * @brief `structo::bootldr::shell`: an interactive command line for the bootloader, running as a
 * task of a `bootldr::scheduler`, with command registration. C++20 only.
 *
 * The shell reads a line from a `hw::uart_ref` (polling, so other scheduler tasks such as the
 * netstack keep running between keystrokes), echoes it with basic editing, splits it with
 * `split_command_line` and runs the registered command with that name. Commands are coroutines:
 * a command can `co_await` a TFTP download or a timer while the rest of the bootloader keeps going.
 *
 * Commands are owned by the client and plug into the shell like `udp_socket`s plug into the
 * netstack: `shell_command` is an intrusive list node, so registering needs no allocation.
 * A built-in `help` command lists everything registered.
 *
 * Line editing: Backspace/DEL erase one character, Ctrl-U erases the line, Ctrl-C abandons it.
 *
 * ```cpp
 * // A command handler: a coroutine, noexcept, taking the command_call.
 * // `call.arg(0)` is the command name, `call.arg(1..)` its arguments, `call.ctx()` the pointer given at registration.
 * static reloco::task<void> cmd_peek(structo::bootldr::command_call &call) noexcept {
 *   if (call.argc() != 2)
 *     co_await reloco::unexpected(reloco::error::invalid_argument); // fail: the shell prints "error: N"
 *   auto addr = structo::bootldr::parse_number(call.arg(1));        // "0x1000", "4096", "0b101", ...
 *   if (!addr)
 *     co_await reloco::unexpected(addr.error());
 *   // print() formats with microfmt ("{}" fields, "{:x}" hex); \n is sent as \r\n.
 *   (void)call.print("{:#x}: {:#010x}\n", *addr, *reinterpret_cast<volatile std::uint32_t *>(*addr));
 * }
 *
 * structo::bootldr::shell<> sh{sched, uart};          // line buffer 128 chars, up to 16 arguments
 * structo::bootldr::shell_command peek{"peek", "peek <addr>: read a 32-bit word", cmd_peek};
 * (void)sh.add(peek);                                 // name, help text and handler; must outlive its registration
 * (void)sh.start();                                   // spawns the shell task; run the scheduler as usual
 * ```
 */

#include <structo/bootldr/cmdline.hpp>
#include <structo/bootldr/scheduler.hpp>
#include <structo/hw/uart_ref.hpp>

#include <reloco/array.hpp>
#include <reloco/coroutine.hpp>
#include <reloco/error.hpp>
#include <reloco/intrusive_c_tailq.hpp>
#include <reloco/span.hpp>
#include <reloco/string_view.hpp>

#include <microfmt/formatters/reloco.hpp> // formats reloco::error by name
#include <microfmt/microfmt.hpp>

#include <cstddef>
#include <cstdint>

#if RELOCO_HAS_COROUTINES

namespace structo::bootldr {

class shell_base;
class command_call;

/**
 * @brief A registrable command; owned by the client. Registering links it into the shell (no
 * allocation); destroying it or the shell unlinks it. `name` and `help` are not copied and must outlive it.
 */
class shell_command {
public:
  /** @brief Handler coroutine. Fail with `co_await reloco::unexpected(err)` to make the shell print the error. */
  using handler_fn = reloco::task<void> (*)(command_call &) noexcept;

  /** @param name single word without spaces. @param help one-line usage text for `help`. @param ctx returned by `command_call::ctx()`. */
  shell_command(reloco::string_view name, reloco::string_view help, handler_fn handler, void *ctx = nullptr) noexcept
      : name_(name), help_(help), handler_(handler), ctx_(ctx) {}
  shell_command(const shell_command &) = delete;
  shell_command &operator=(const shell_command &) = delete;
  inline ~shell_command();

  [[nodiscard]] reloco::string_view name() const noexcept { return name_; }
  [[nodiscard]] reloco::string_view help() const noexcept { return help_; }
  [[nodiscard]] bool registered() const noexcept { return shell_ != nullptr; }

private:
  friend class shell_base;
  template <std::size_t, std::size_t> friend class shell;

  // Same layout as FreeBSD TAILQ_ENTRY, as expected by reloco::c_tailq.
  struct {
    shell_command *next = nullptr;
    shell_command **prev = nullptr;
  } link_;

  reloco::string_view name_;
  reloco::string_view help_;
  handler_fn handler_;
  void *ctx_;
  shell_base *shell_ = nullptr;
};

/** @brief The non-template part of the shell: the command table and console output. */
class shell_base {
public:
  shell_base(scheduler &sched, hw::uart_ref uart) noexcept
      : sched_(&sched), uart_(uart), help_("help", "help [command]: list the commands or show one", &help_handler, this) {
    (void)add(help_);
  }
  shell_base(const shell_base &) = delete;
  shell_base &operator=(const shell_base &) = delete;
  ~shell_base() {
    while (shell_command *c = commands_.pop_front())
      c->shell_ = nullptr;
  }

  [[nodiscard]] scheduler &sched() const noexcept { return *sched_; }

  /**
   * @brief Registers `cmd`. `error::invalid_state` if it is already registered (here or elsewhere),
   * `error::invalid_argument` for an empty name or one containing blanks, `error::already_exists` for a duplicate name.
   */
  [[nodiscard]] reloco::result<void> add(shell_command &cmd) noexcept {
    if (cmd.shell_)
      return reloco::unexpected(reloco::error::invalid_state);
    const reloco::string_view n = cmd.name_;
    if (n.empty())
      return reloco::unexpected(reloco::error::invalid_argument);
    for (std::size_t i = 0; i < n.size(); ++i)
      if (n[i] == ' ' || n[i] == '\t')
        return reloco::unexpected(reloco::error::invalid_argument);
    if (find(n))
      return reloco::unexpected(reloco::error::already_exists);
    commands_.push_back(cmd);
    cmd.shell_ = this;
    return {};
  }

  /** @brief Unregisters `cmd` (no-op if it is not registered here). */
  void remove(shell_command &cmd) noexcept {
    if (cmd.shell_ != this)
      return;
    commands_.remove(cmd);
    cmd.shell_ = nullptr;
  }

  /** @brief The command called `name`, or null. */
  [[nodiscard]] shell_command *find(reloco::string_view name) noexcept {
    for (shell_command &c : commands_)
      if (c.name_ == name)
        return &c;
    return nullptr;
  }

  /** @brief Writes text to the console (`'\n'` becomes `"\r\n"`). */
  [[nodiscard]] reloco::result<void> write(reloco::string_view text) const noexcept {
    return uart_.write_string(text);
  }

  /**
   * @brief Formats with microfmt and writes the result (`'\n'` becomes `"\r\n"`). Output beyond `PrintMax`
   * characters (default 128) is truncated.
   */
  template <std::size_t PrintMax = 128, typename... Args>
  [[nodiscard]] reloco::result<void> print(microfmt::string_view fmt, const Args &...args) const noexcept {
    const auto text = microfmt::format<PrintMax>(fmt, args...);
    return uart_.write_string(text.view());
  }

protected:
  scheduler *sched_;
  hw::uart_ref uart_;

private:
  static reloco::task<void> help_handler(command_call &call) noexcept;

  reloco::c_tailq<shell_command, &shell_command::link_> commands_;
  shell_command help_;
};

inline shell_command::~shell_command() {
  if (shell_)
    shell_->remove(*this);
}

/** @brief What a command handler receives: its arguments, output and registration context. */
class command_call {
public:
  command_call(shell_base &sh, reloco::span<char *> argv, void *ctx) noexcept : sh_(&sh), argv_(argv), ctx_(ctx) {}

  [[nodiscard]] shell_base &sh() const noexcept { return *sh_; }
  /** @brief Number of words including the command name. */
  [[nodiscard]] std::size_t argc() const noexcept { return argv_.size(); }
  /** @brief Word `i` (0 = command name); empty if out of range. */
  [[nodiscard]] reloco::string_view arg(std::size_t i) const noexcept {
    return i < argv_.size() ? reloco::string_view(argv_[i]) : reloco::string_view();
  }
  /** @brief The pointer given to the `shell_command` constructor. */
  [[nodiscard]] void *ctx() const noexcept { return ctx_; }

  [[nodiscard]] reloco::result<void> write(reloco::string_view text) const noexcept { return sh_->write(text); }
  /** @brief Formats with microfmt and writes the result; see `shell_base::print`. */
  template <std::size_t PrintMax = 128, typename... Args>
  [[nodiscard]] reloco::result<void> print(microfmt::string_view fmt, const Args &...args) const noexcept {
    return sh_->template print<PrintMax>(fmt, args...);
  }

private:
  shell_base *sh_;
  reloco::span<char *> argv_;
  void *ctx_;
};

inline reloco::task<void> shell_base::help_handler(command_call &call) noexcept {
  auto &self = *static_cast<shell_base *>(call.ctx());
  if (call.argc() == 2) { // "help <command>"
    shell_command *c = self.find(call.arg(1));
    if (!c)
      co_await reloco::unexpected(reloco::error::not_found);
    (void)call.print("{}\n", c->help_);
    co_return;
  }
  for (shell_command &c : self.commands_) {
    if (auto r = call.write(c.name_); !r)
      co_await reloco::unexpected(r.error());
    (void)call.print("  {}\n", c.help_);
  }
}

/**
 * @brief The interactive shell. `LineMax` is the longest line, `MaxArgs` the most words per line.
 * Owned by the client; `start()` spawns it as a scheduler task, the destructor stops it.
 */
template <std::size_t LineMax = 128, std::size_t MaxArgs = 16> class shell : public shell_base {
public:
  /** @param prompt printed before each line; must outlive the shell. */
  shell(scheduler &sched, hw::uart_ref uart, reloco::string_view prompt = "> ") noexcept
      : shell_base(sched, uart), prompt_(prompt) {}
  ~shell() { stop(); }

  /** @brief Spawns the shell task. `error::invalid_state` if already running. */
  [[nodiscard]] reloco::result<void> start() noexcept {
    if (running_)
      return reloco::unexpected(reloco::error::invalid_state);
    auto id = sched_->spawn(loop(*this));
    if (!id)
      return reloco::unexpected(id.error());
    id_ = *id;
    running_ = true;
    return {};
  }

  /** @brief Cancels the shell task (a command that is currently running is destroyed with it). */
  void stop() noexcept {
    if (!running_)
      return;
    running_ = false;
    (void)sched_->cancel(id_);
    busy_ = false;
    len_ = 0;
  }

  [[nodiscard]] bool running() const noexcept { return running_; }

  /**
   * @brief Runs one command line without the console (boot scripts, tests). `line` is copied before the
   * first suspension, but the task is lazy: keep `line` alive until the task has been spawned/awaited.
   * Fails with `error::out_of_range` if it exceeds `LineMax`, `error::busy` while a command runs or a line is
   * being typed, `error::invalid_argument` for a syntax error, `error::not_found` for an unknown command, or
   * the command's own error. Nothing is printed for those failures.
   */
  [[nodiscard]] static reloco::task<void> execute(shell &sh,
                                                  reloco::string_view line) noexcept {
    if (sh.busy_ || sh.len_ != 0)
      co_await reloco::unexpected(reloco::error::busy);
    if (line.size() > LineMax)
      co_await reloco::unexpected(reloco::error::out_of_range);
    for (std::size_t i = 0; i < line.size(); ++i)
      sh.line_[i] = line[i];
    co_await co_await run_line(sh, line.size());
  }

private:
  struct busy_guard {
    bool &flag;
    explicit busy_guard(bool &f) noexcept : flag(f) { flag = true; }
    ~busy_guard() { flag = false; }
  };

  // Tokenizes line_[0..len) and runs the command; the buffer must stay untouched while it runs (busy_).
  static reloco::task<void> run_line(shell &sh,
                                     std::size_t len) noexcept {
    busy_guard guard{sh.busy_};
    auto n = split_command_line(sh.line_.data(), len, reloco::span<char *>(sh.argv_.data(), MaxArgs));
    if (!n) {
      (void)sh.write("syntax error\n");
      sh.reported_ = true;
      co_await reloco::unexpected(n.error());
    }
    if (*n == 0)
      co_return;

    shell_command *cmd = sh.find(reloco::string_view(sh.argv_[0]));
    if (!cmd) {
      (void)sh.print("unknown command: {}\n", reloco::string_view(sh.argv_[0]));
      sh.reported_ = true;
      co_await reloco::unexpected(reloco::error::not_found);
    }
    command_call call{sh, reloco::span<char *>(sh.argv_.data(), *n), cmd->ctx_};
    co_await co_await cmd->handler_(call);
  }

  static reloco::task<void> loop(shell &sh) noexcept {
    (void)sh.write(sh.prompt_);
    bool last_cr = false;
    for (;;) {
      auto ready = sh.uart_.rx_ready();
      if (!ready)
        co_await reloco::unexpected(ready.error());
      if (!*ready) {
        co_await sh.sched_->yield();
        continue;
      }
      auto got = sh.uart_.try_get_byte();
      if (!got) {
        if (got.error() == reloco::error::try_again)
          continue;
        co_await reloco::unexpected(got.error());
      }
      const std::uint8_t b = *got;
      const bool was_cr = last_cr;
      last_cr = b == '\r';

      if (b == '\r' || (b == '\n' && !was_cr)) {
        (void)sh.write("\n");
        const std::size_t len = sh.len_;
        sh.len_ = 0;
        sh.reported_ = false;
        auto r = co_await run_line(sh, len);
        if (!r && !sh.reported_) {
          (void)sh.print("error: {}\n", r.error());
        }
        (void)sh.write(sh.prompt_);
      } else if (b == 0x7f || b == 0x08) {
        if (sh.len_ > 0) {
          --sh.len_;
          (void)sh.write("\b \b");
        }
      } else if (b == 0x15) { // Ctrl-U
        while (sh.len_ > 0) {
          --sh.len_;
          (void)sh.write("\b \b");
        }
      } else if (b == 0x03) { // Ctrl-C
        sh.len_ = 0;
        (void)sh.write("^C\n");
        (void)sh.write(sh.prompt_);
      } else if (b >= 0x20 && b < 0x7f) {
        if (sh.len_ < LineMax) {
          sh.line_[sh.len_++] = static_cast<char>(b);
          (void)sh.uart_.put_byte(b);
        } else {
          (void)sh.uart_.put_byte(0x07); // bell: line full
        }
      }
      co_await sh.sched_->yield();
    }
  }

  reloco::string_view prompt_;
  reloco::array<char, LineMax + 1> line_{};
  reloco::array<char *, MaxArgs> argv_{};
  std::size_t len_ = 0;
  bool busy_ = false;
  bool reported_ = false;
  bool running_ = false;
  task_id id_ = 0;
};

} // namespace structo::bootldr

#endif // RELOCO_HAS_COROUTINES
