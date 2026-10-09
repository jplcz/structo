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
 * Evaluation context (`shell_context`, `shell_base::context()`): every line is expanded before it is split.
 *  - Variables: `set NAME VALUE`, `set -g NAME VALUE` (global), `unset NAME`. `$NAME` / `${NAME}` are replaced,
 *    except inside `'...'` or after a backslash; an unknown variable expands to nothing, and a substituted value is
 *    always exactly one argument (blanks and quotes in it are not interpreted).
 *  - Expressions: `$((expr))` is the decimal value of an integer expression (`+ - * / % << >> & | ^ ~`, parentheses,
 *    numbers like `0x1000`, variable names, calls such as `align_up(x, 0x1000)` or `min(a, b)`), e.g.
 *    `md $((base + 0x40)) 16`. See `evaluate_expression`; native functions are added with `context().add_function`.
 *  - Functions: `function NAME 'cmd1 $1; cmd2 $2'` defines a named sequence of statements (`;` separated; use single
 *    quotes so `$1` is not expanded while defining) that is run like a command: `NAME a b`. Each call has its own
 *    dynamic scope: `$0` is the name, `$1`.. the arguments, `$#` their count; variables it `set`s are local to the
 *    call (they shadow, never overwrite, the caller's and vanish on return), while `$name` still sees the caller's
 *    variables. `set -g` writes the global scope. Calls nest up to 16 deep; a failing statement ends the function.
 *
 * Scripts: a line may hold several statements separated by `;` (outside quotes). `shell::execute_script(sh, text)`
 * and `shell_base::run_script(text)` run a multi-line buffer (one line per `'\n'`), stopping at the first failure.
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
 * structo::bootldr::shell sh{sched, uart};             // up to 256-char lines; arguments are heap-backed
 * structo::bootldr::shell_command peek{"peek", "peek <addr>: read a 32-bit word", cmd_peek};
 * (void)sh.add(peek);                                 // name, help text and handler; must outlive its registration
 * (void)sh.start();                                   // spawns the shell task; run the scheduler as usual
 * ```
 */

#include <structo/bootldr/cmdline.hpp>
#include <structo/bootldr/scheduler.hpp>
#include <structo/bootldr/shell_context.hpp>
#include <structo/hw/uart_ref.hpp>

#include <reloco/coroutine.hpp>
#include <reloco/error.hpp>
#include <reloco/inline_string.hpp>
#include <reloco/intrusive_c_tailq.hpp>
#include <reloco/span.hpp>
#include <reloco/sso_string.hpp>
#include <reloco/sso_vector.hpp>
#include <reloco/string_view.hpp>

#include <microfmt/formatters/reloco.hpp> // formats reloco::error by name
#include <microfmt/microfmt.hpp>

#include <cstddef>
#include <cstdint>

#if RELOCO_HAS_COROUTINES

namespace structo::bootldr {

class shell_base;
class shell;
class command_call;

/**
 * @brief A registrable command; owned by the client. Registering links it into the shell (no
 * allocation); destroying it or the shell unlinks it. `name` and `help` are not copied and must outlive it.
 */
class shell_command {
public:
  /** @brief Handler coroutine. Fail with `co_await reloco::unexpected(err)` to make the shell print the error. */
  using handler_fn = reloco::task<void> (*)(command_call &) noexcept;

  /** @param name single word without spaces. @param help one-line usage text for `help`. @param ctx returned by
   * `command_call::ctx()`. */
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
  friend class shell;

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

/** @brief The non-template part of the shell: the command table, the evaluation context and console output. */
class shell_base {
public:
  shell_base(scheduler &sched, hw::uart_ref uart) noexcept
      : sched_(&sched), uart_(uart), ctx_(sched.allocator()), expanded_(sched.allocator()), argv_(sched.allocator()),
        help_("help", "help [command]: list the commands or show one", &help_handler, this),
        set_("set", "set [-g] [name [value]]: list variables, show or assign one (-g: global); $name, $((expr)) expand",
             &set_handler, this),
        unset_("unset", "unset <name>: remove a variable of the current scope", &unset_handler, this),
        function_("function", "function [name [body]]: list, show or define a function; body: 'cmd $1; cmd2'",
                  &function_handler, this),
        unfunction_("unfunction", "unfunction <name>: remove a function", &unfunction_handler, this) {
    (void)add(help_);
    (void)add(set_);
    (void)add(unset_);
    (void)add(function_);
    (void)add(unfunction_);
  }
  shell_base(const shell_base &) = delete;
  shell_base &operator=(const shell_base &) = delete;
  ~shell_base() {
    while (shell_command *c = commands_.pop_front())
      c->shell_ = nullptr;
  }

  [[nodiscard]] scheduler &sched() const noexcept { return *sched_; }

  /** @brief Variables, shell-defined functions and expression functions the lines are evaluated against. */
  [[nodiscard]] shell_context &context() noexcept { return ctx_; }
  [[nodiscard]] const shell_context &context() const noexcept { return ctx_; }

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
  [[nodiscard]] reloco::result<void> write(reloco::string_view text) const noexcept { return uart_.write_string(text); }

  /**
   * @brief Formats with microfmt straight into the UART (`'\n'` becomes `"\r\n"`): no intermediate buffer, so
   * output length is unlimited. Returns the first UART error, if any.
   */
  template <typename... Args>
  [[nodiscard]] reloco::result<void> print(microfmt::string_view fmt, const Args &...args) const noexcept {
    struct uart_out {
      const hw::uart_ref *uart;
      reloco::result<void> status;
    } out{&uart_, {}};
    microfmt::format_to(microfmt::sink{&out,
                                       [](void *c, microfmt::string_view sv) noexcept {
                                         auto *o = static_cast<uart_out *>(c);
                                         if (o->status)
                                           o->status = o->uart->write_string(sv);
                                       }},
                        fmt, args...);
    return out.status;
  }

  /**
   * @brief Runs a script: one line per `'\n'` (a trailing `'\r'` is ignored), each line holding `;`-separated
   * statements. Runs in the current variable scope. Stops at the first failing statement and returns its error.
   * `script` must stay valid until the task finishes. Calls (functions, nested scripts) nest up to 16 deep, else
   * `error::out_of_range`.
   */
  [[nodiscard]] reloco::task<void> run_script(reloco::string_view script) noexcept {
    if (depth_ >= max_call_depth)
      co_await reloco::unexpected(reloco::error::out_of_range);
    depth_guard guard{depth_};
    std::size_t start = 0;
    for (std::size_t i = 0; i <= script.size(); ++i) {
      if (i != script.size() && script[i] != '\n')
        continue;
      std::size_t end = i;
      if (end > start && script[end - 1] == '\r')
        --end;
      co_await co_await run_statements(script.substr(start, end - start));
      start = i + 1;
    }
  }

protected:
  static constexpr int max_call_depth = 16;

  /**
   * @brief Runs one statement: expands it with the context, splits it into words and runs the command or shell
   * function named by the first one. Reports syntax errors and unknown commands on the console (`reported_`).
   * Reuses `expanded_`/`argv_`, so a caller must not rely on them across the `co_await`.
   */
  [[nodiscard]] reloco::task<void> run_statement(reloco::string_view text) noexcept;

  /**
   * @brief Runs `text` as statements separated by `;` outside quotes and backslash escapes; the first failure
   * ends it. `text` must stay valid while it runs.
   */
  [[nodiscard]] reloco::task<void> run_statements(reloco::string_view text) noexcept {
    std::size_t start = 0;
    char quote = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
      const bool end = i == text.size();
      const char c = end ? '\0' : text[i];
      if (!end && c == '\\' && quote != '\'') {
        ++i;
      } else if (!end && quote == 0 && (c == '"' || c == '\'')) {
        quote = c;
      } else if (!end && quote == c) {
        quote = 0;
      } else if (end || (c == ';' && quote == 0)) {
        co_await co_await run_statement(text.substr(start, i - start));
        start = i + 1;
      }
    }
  }

  scheduler *sched_;
  hw::uart_ref uart_;
  shell_context ctx_;
  reloco::sso_string expanded_;        // the statement after substitution; argv_ points into it
  reloco::sso_vector<char *, 8> argv_; // words of the current statement
  bool reported_ = false;              // the failure of the current line is already on the console

private:
  // Runs the function named argv_[0] with the words argv_[0..argc) as $0..: in its own dynamic scope, so its
  // variables and positional parameters vanish when it returns and cannot overwrite the caller's.
  [[nodiscard]] reloco::task<void> call_function(std::size_t argc) noexcept {
    if (depth_ >= max_call_depth)
      co_await reloco::unexpected(reloco::error::out_of_range);
    auto found = ctx_.function_body(reloco::string_view(argv_[0]));
    if (!found)
      co_await reloco::unexpected(found.error());
    // Copy the body: the function may be redefined or removed while it runs.
    auto body = reloco::sso_string::try_allocate(sched_->allocator(), *found);
    if (!body)
      co_await reloco::unexpected(body.error());

    shell_context::scope frame{ctx_};
    if (auto r = frame.set_args(reloco::span<char *>(argv_.data(), argc)); !r)
      co_await reloco::unexpected(r.error());
    frame.enter();
    depth_guard guard{depth_};

    co_await co_await run_statements(body->view());
  }

  struct depth_guard {
    int &depth;
    explicit depth_guard(int &d) noexcept : depth(d) { ++depth; }
    ~depth_guard() { --depth; }
  };

  static reloco::task<void> help_handler(command_call &call) noexcept;
  static reloco::task<void> set_handler(command_call &call) noexcept;
  static reloco::task<void> unset_handler(command_call &call) noexcept;
  static reloco::task<void> function_handler(command_call &call) noexcept;
  static reloco::task<void> unfunction_handler(command_call &call) noexcept;

  int depth_ = 0;
  reloco::c_tailq<shell_command, &shell_command::link_> commands_;
  shell_command help_;
  shell_command set_;
  shell_command unset_;
  shell_command function_;
  shell_command unfunction_;
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
  template <typename... Args>
  [[nodiscard]] reloco::result<void> print(microfmt::string_view fmt, const Args &...args) const noexcept {
    return sh_->print(fmt, args...);
  }

private:
  shell_base *sh_;
  reloco::span<char *> argv_;
  void *ctx_;
};

inline reloco::task<void> shell_base::run_statement(reloco::string_view text) noexcept {
  if (auto r = ctx_.expand(text, expanded_); !r)
    co_await reloco::unexpected(r.error());
  if (expanded_.empty())
    co_return;
  // A line of N characters has at most (N + 1) / 2 words.
  const std::size_t max_words = (expanded_.size() + 1) / 2;
  if (auto r = argv_.try_resize(max_words); !r)
    co_await reloco::unexpected(r.error());
  auto n = split_command_line(expanded_.data(), expanded_.size(), reloco::span<char *>(argv_.data(), max_words));
  if (!n) {
    (void)write("syntax error\n");
    reported_ = true;
    co_await reloco::unexpected(n.error());
  }
  if (*n == 0)
    co_return;

  const reloco::string_view name(argv_[0]);
  if (shell_command *cmd = find(name)) {
    command_call call{*this, reloco::span<char *>(argv_.data(), *n), cmd->ctx_};
    co_await co_await cmd->handler_(call);
    co_return;
  }
  if (ctx_.function_body(name).has_value()) {
    co_await co_await call_function(*n);
    co_return;
  }
  (void)print("unknown command: {}\n", name);
  reported_ = true;
  co_await reloco::unexpected(reloco::error::not_found);
}

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

inline reloco::task<void> shell_base::set_handler(command_call &call) noexcept {
  auto &self = *static_cast<shell_base *>(call.ctx());
  const bool global = call.argc() > 1 && call.arg(1) == "-g";
  const std::size_t argc = call.argc() - (global ? 1 : 0); // words without "-g"
  const std::size_t first = global ? 2 : 1;                // index of the name
  if (argc == 1) {
    self.ctx_.for_each_variable(
        [&](reloco::string_view k, reloco::string_view v) { (void)call.print("{}={}\n", k, v); });
    co_return;
  }
  if (argc == 2 && !global) {
    auto v = self.ctx_.get(call.arg(first));
    if (!v)
      co_await reloco::unexpected(v.error());
    (void)call.print("{}\n", *v);
    co_return;
  }
  if (argc != 3)
    co_await reloco::unexpected(reloco::error::invalid_argument);
  auto r = global ? self.ctx_.set_global(call.arg(first), call.arg(first + 1))
                  : self.ctx_.set(call.arg(first), call.arg(first + 1));
  if (!r)
    co_await reloco::unexpected(r.error());
}

inline reloco::task<void> shell_base::unset_handler(command_call &call) noexcept {
  auto &self = *static_cast<shell_base *>(call.ctx());
  if (call.argc() != 2)
    co_await reloco::unexpected(reloco::error::invalid_argument);
  if (auto r = self.ctx_.unset(call.arg(1)); !r)
    co_await reloco::unexpected(r.error());
}

inline reloco::task<void> shell_base::function_handler(command_call &call) noexcept {
  auto &self = *static_cast<shell_base *>(call.ctx());
  if (call.argc() == 1) {
    self.ctx_.for_each_function([&](reloco::string_view k, reloco::string_view) { (void)call.print("{}\n", k); });
    co_return;
  }
  if (call.argc() == 2) {
    auto body = self.ctx_.function_body(call.arg(1));
    if (!body)
      co_await reloco::unexpected(body.error());
    (void)call.print("{}\n", *body);
    co_return;
  }
  if (call.argc() != 3)
    co_await reloco::unexpected(reloco::error::invalid_argument);
  if (self.find(call.arg(1))) // a command of that name would hide the function
    co_await reloco::unexpected(reloco::error::already_exists);
  if (auto r = self.ctx_.define_function(call.arg(1), call.arg(2)); !r)
    co_await reloco::unexpected(r.error());
}

inline reloco::task<void> shell_base::unfunction_handler(command_call &call) noexcept {
  auto &self = *static_cast<shell_base *>(call.ctx());
  if (call.argc() != 2)
    co_await reloco::unexpected(reloco::error::invalid_argument);
  if (auto r = self.ctx_.undefine_function(call.arg(1)); !r)
    co_await reloco::unexpected(r.error());
}

/**
 * @brief The interactive shell. A typed line holds at most `line_capacity` characters (inline storage); the
 * expanded line and the word list grow on the heap (scheduler allocator).
 * Owned by the client; `start()` spawns it as a scheduler task, the destructor stops it.
 */
class shell : public shell_base {
public:
  /** @brief Longest line that can be typed or passed to `execute()` (before variable expansion). */
  static constexpr std::size_t line_capacity = 256;

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
    line_.clear();
  }

  [[nodiscard]] bool running() const noexcept { return running_; }

  /**
   * @brief Runs one command line without the console (boot scripts, tests). `line` is copied before the
   * first suspension, but the task is lazy: keep `line` alive until the task has been spawned/awaited.
   * Fails with `error::out_of_bounds` if it exceeds `line_capacity`, `error::busy` while a command runs or a line is
   * being typed, `error::invalid_argument` for a syntax error, `error::not_found` for an unknown command, or
   * the command's own error. Nothing is printed for those failures.
   */
  [[nodiscard]] static reloco::task<void> execute(shell &sh, reloco::string_view line) noexcept {
    if (sh.busy_ || !sh.line_.empty())
      co_await reloco::unexpected(reloco::error::busy);
    if (auto r = sh.line_.try_assign(line); !r)
      co_await reloco::unexpected(r.error());
    co_await co_await run_line(sh);
  }

  /**
   * @brief Runs a multi-line script without the console, like `execute()`: see `shell_base::run_script`.
   * Fails with `error::busy` while a command runs or a line is being typed. `script` must stay valid until the
   * task has finished.
   */
  [[nodiscard]] static reloco::task<void> execute_script(shell &sh, reloco::string_view script) noexcept {
    if (sh.busy_ || !sh.line_.empty())
      co_await reloco::unexpected(reloco::error::busy);
    busy_guard guard{sh.busy_};
    co_await co_await sh.run_script(script);
  }

private:
  struct busy_guard {
    bool &flag;
    explicit busy_guard(bool &f) noexcept : flag(f) { flag = true; }
    ~busy_guard() { flag = false; }
  };

  // Runs line_ as one statement (busy_ keeps execute() out meanwhile), then empties it.
  static reloco::task<void> run_line(shell &sh) noexcept {
    busy_guard guard{sh.busy_};
    auto r = co_await sh.run_statements(sh.line_.view());
    sh.line_.clear();
    if (!r)
      co_await reloco::unexpected(r.error());
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
        sh.reported_ = false;
        auto r = co_await run_line(sh);
        if (!r && !sh.reported_) {
          (void)sh.print("error: {}\n", r.error());
        }
        (void)sh.write(sh.prompt_);
      } else if (b == 0x7f || b == 0x08) {
        if (!sh.line_.empty()) {
          (void)sh.line_.try_pop_back();
          (void)sh.write("\b \b");
        }
      } else if (b == 0x15) { // Ctrl-U
        while (!sh.line_.empty()) {
          (void)sh.line_.try_pop_back();
          (void)sh.write("\b \b");
        }
      } else if (b == 0x03) { // Ctrl-C
        sh.line_.clear();
        (void)sh.write("^C\n");
        (void)sh.write(sh.prompt_);
      } else if (b >= 0x20 && b < 0x7f) {
        if (sh.line_.try_push_back(static_cast<char>(b))) {
          (void)sh.uart_.put_byte(b);
        } else {
          (void)sh.uart_.put_byte(0x07); // bell: line full
        }
      }
      co_await sh.sched_->yield();
    }
  }

  reloco::string_view prompt_;
  reloco::inline_string<line_capacity> line_; // the line being typed
  bool busy_ = false;
  bool running_ = false;
  task_id id_ = 0;
};

} // namespace structo::bootldr

#endif // RELOCO_HAS_COROUTINES
