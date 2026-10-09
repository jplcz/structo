// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file shell_context.hpp
 * @brief `structo::bootldr::shell_context`: the state a shell command line is evaluated against: variables,
 * positional parameters, shell-defined functions and native expression functions. Sans-IO, heap-backed
 * (`reloco::flat_map` / `reloco::string` on the given allocator), works in C++17.
 *
 * Dynamic scoping: variables live in a chain of `scope`s. The context starts with one global scope; each
 * shell-function call enters a new `scope` (an RAII object owned by the caller), so
 *  - lookups (`$name`) search the innermost scope first, then the callers' scopes, up to the global one;
 *  - `set` creates or updates a variable in the innermost scope only, so a function cannot leak or overwrite
 *    its caller's variables (use `set_global` to write the global scope deliberately);
 *  - positional parameters (`$0` = function name, `$1`.., `$#`) belong to the innermost scope only;
 *  - leaving the scope drops everything it defined.
 *
 * Expansion (`expand`) replaces `$name`, `${name}`, `$1`, `${10}`, `$#` and `$((expression))` in a line.
 * Expressions are evaluated by `evaluate_expression`; `name(a, b)` inside them calls a native function added with
 * `add_function` (or the built-ins `min`, `max`, `align_up`, `align_down`).
 *
 * ```cpp
 * structo::bootldr::shell_context ctx{reloco::default_allocator()}; // every string/map allocates from here
 * (void)ctx.set("base", "0x80000000");                              // variable in the current (global) scope
 *
 * // A native function usable as $((crc(a, b))): receives the evaluated arguments, returns the value.
 * // `user` is the pointer given at registration.
 * (void)ctx.add_function("crc", +[](void *user, reloco::span<const std::uint64_t> args) noexcept
 *                                   -> reloco::result<std::uint64_t> { return args.size() == 2 ? args[0] ^ args[1] : 0; },
 *                        nullptr);
 *
 * {
 *   structo::bootldr::shell_context::scope call{ctx}; // a function call's own variables; left on destruction
 *   call.enter();                                     // now `set` and `$name` use it first, then the outer scopes
 *   (void)ctx.set("base", "1");                       // shadows the global `base` until `call` ends
 * }
 * ```
 */

#include <structo/bootldr/cmdline.hpp>

#include <reloco/allocator.hpp>
#include <reloco/error.hpp>
#include <reloco/flat_map.hpp>
#include <reloco/span.hpp>
#include <reloco/string.hpp>
#include <reloco/string_view.hpp>
#include <reloco/sso_string.hpp>
#include <reloco/vector.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace structo::bootldr {

/** @brief Orders names; lets the maps be searched with a `string_view`. */
struct variable_less {
  static reloco::string_view sv(reloco::string_view v) noexcept { return v; }
  static reloco::string_view sv(const reloco::string &v) noexcept { return v.view(); }
  template <typename A, typename B> bool operator()(const A &a, const B &b) const noexcept { return sv(a) < sv(b); }
};

/** @brief See the file comment. Not copyable or movable (scopes point back to it). */
class shell_context {
  using string_map = reloco::flat_map<reloco::string, reloco::string, variable_less>;

public:
  /** @brief A native function callable from expressions; `user` is the pointer given to `add_function`. */
  using native_fn = reloco::result<std::uint64_t> (*)(void *user, reloco::span<const std::uint64_t> args) noexcept;

  /**
   * @brief One level of the dynamic scope chain: variables and positional parameters. Owned by the caller
   * (typically a stack object in a function call); `enter()`/leaving must nest (LIFO).
   */
  class scope {
  public:
    explicit scope(shell_context &ctx) noexcept : ctx_(&ctx), vars_(ctx.alloc_), args_(ctx.alloc_) {}
    scope(const scope &) = delete;
    scope &operator=(const scope &) = delete;
    ~scope() { leave(); }

    /** @brief Makes this the innermost scope (no-op if already entered). */
    void enter() noexcept {
      if (active_)
        return;
      parent_ = ctx_->current_;
      ctx_->current_ = this;
      active_ = true;
    }
    /** @brief Pops this scope; its caller's scope becomes the innermost again. */
    void leave() noexcept {
      if (!active_)
        return;
      if (ctx_->current_ == this)
        ctx_->current_ = parent_;
      active_ = false;
    }

    /** @brief Sets `$0..$n` to the given words (copied). `error::out_of_memory` on allocation failure. */
    [[nodiscard]] reloco::result<void> set_args(reloco::span<char *> words) noexcept {
      args_.clear();
      for (std::size_t i = 0; i < words.size(); ++i) {
        auto s = reloco::string::try_allocate(ctx_->alloc_, reloco::string_view(words[i]));
        if (!s)
          return reloco::unexpected(s.error());
        if (auto r = args_.try_push_back(std::move(*s)); !r)
          return r;
      }
      return {};
    }

  private:
    friend class shell_context;
    shell_context *ctx_;
    scope *parent_ = nullptr;
    bool active_ = false;
    string_map vars_;
    reloco::vector<reloco::string> args_;
  };

  explicit shell_context(reloco::allocator_ref alloc = reloco::default_allocator()) noexcept
      : alloc_(alloc), natives_(alloc), functions_(alloc), global_(*this), current_(&global_) {}
  shell_context(const shell_context &) = delete;
  shell_context &operator=(const shell_context &) = delete;

  [[nodiscard]] reloco::allocator_ref allocator() const noexcept { return alloc_; }

  /** @brief True for `[A-Za-z_][A-Za-z0-9_]*`: the names variables and functions may have. */
  [[nodiscard]] static bool valid_name(reloco::string_view n) noexcept {
    if (n.empty())
      return false;
    for (std::size_t i = 0; i < n.size(); ++i)
      if (!is_name_char(n[i], i == 0))
        return false;
    return true;
  }

  // ---- variables ----

  /**
   * @brief Sets variable `name` in the innermost scope (copied to the heap), creating or replacing it.
   * `error::invalid_argument` for an invalid name, `error::out_of_memory` on allocation failure.
   */
  [[nodiscard]] reloco::result<void> set(reloco::string_view name, reloco::string_view value) noexcept {
    if (!valid_name(name))
      return reloco::unexpected(reloco::error::invalid_argument);
    return assign(current_->vars_, name, value);
  }

  /** @brief Like `set`, but writes the global (outermost) scope. */
  [[nodiscard]] reloco::result<void> set_global(reloco::string_view name, reloco::string_view value) noexcept {
    if (!valid_name(name))
      return reloco::unexpected(reloco::error::invalid_argument);
    return assign(global_.vars_, name, value);
  }

  /** @brief Removes `name` from the innermost scope only; `error::not_found` if that scope does not define it. */
  [[nodiscard]] reloco::result<void> unset(reloco::string_view name) noexcept { return current_->vars_.try_remove(name); }

  /**
   * @brief The value of `name` searched from the innermost scope outwards, or `error::not_found`. A name made of
   * digits is a positional parameter of the innermost scope. Valid until the variable changes.
   */
  [[nodiscard]] reloco::result<reloco::string_view> get(reloco::string_view name) const noexcept {
    if (!name.empty() && name[0] >= '0' && name[0] <= '9')
      return positional(name);
    for (const scope *s = current_; s; s = s->parent_) {
      auto found = s->vars_.try_find(name);
      if (found)
        return found->get().second.view();
    }
    return reloco::unexpected(reloco::error::not_found);
  }

  /** @brief Calls `f(name, value)` for every visible variable, innermost scope first, skipping shadowed ones. */
  template <typename F> void for_each_variable(F &&f) const {
    for (const scope *s = current_; s; s = s->parent_)
      for (const auto &kv : s->vars_) {
        bool shadowed = false;
        for (const scope *inner = current_; inner != s; inner = inner->parent_)
          shadowed = shadowed || inner->vars_.contains(kv.first.view());
        if (!shadowed)
          f(kv.first.view(), kv.second.view());
      }
  }

  /** @brief Number of positional parameters of the innermost scope (`$#`), not counting `$0`. */
  [[nodiscard]] std::size_t positional_count() const noexcept {
    return current_->args_.empty() ? 0 : current_->args_.size() - 1;
  }

  // ---- shell-defined functions (named command sequences; run by the shell) ----

  /** @brief Defines or replaces function `name` with `body` (statements separated by `;`). */
  [[nodiscard]] reloco::result<void> define_function(reloco::string_view name, reloco::string_view body) noexcept {
    if (!valid_name(name))
      return reloco::unexpected(reloco::error::invalid_argument);
    return assign(functions_, name, body);
  }
  /** @brief Removes function `name`; `error::not_found` if it is not defined. */
  [[nodiscard]] reloco::result<void> undefine_function(reloco::string_view name) noexcept {
    return functions_.try_remove(name);
  }
  /** @brief The body of function `name`, or `error::not_found`. Valid until the function changes. */
  [[nodiscard]] reloco::result<reloco::string_view> function_body(reloco::string_view name) const noexcept {
    auto found = functions_.try_find(name);
    if (!found)
      return reloco::unexpected(found.error());
    return found->get().second.view();
  }
  /** @brief Calls `f(name, body)` for every defined function. */
  template <typename F> void for_each_function(F &&f) const {
    for (const auto &kv : functions_)
      f(kv.first.view(), kv.second.view());
  }

  // ---- native expression functions ----

  /** @brief Registers `fn` as `name(...)` for expressions, replacing an earlier one with the same name. */
  [[nodiscard]] reloco::result<void> add_function(reloco::string_view name, native_fn fn, void *user = nullptr) noexcept {
    if (!valid_name(name))
      return reloco::unexpected(reloco::error::invalid_argument);
    if (auto existing = natives_.try_at(name)) {
      existing->get() = native_entry{fn, user};
      return {};
    }
    auto k = reloco::string::try_allocate(alloc_, name);
    if (!k)
      return reloco::unexpected(k.error());
    auto ins = natives_.try_insert(std::move(*k), native_entry{fn, user});
    if (!ins)
      return reloco::unexpected(ins.error());
    return {};
  }
  /** @brief Unregisters native function `name`; `error::not_found` if unknown. */
  [[nodiscard]] reloco::result<void> remove_function(reloco::string_view name) noexcept { return natives_.try_remove(name); }

  /**
   * @brief Calls expression function `name`: registered natives first, then the built-ins `min(a,b)`, `max(a,b)`,
   * `align_up(x,a)`, `align_down(x,a)`. `error::not_found` for an unknown name, `error::invalid_argument` for a
   * wrong argument count or a zero alignment.
   */
  [[nodiscard]] reloco::result<std::uint64_t> call(reloco::string_view name,
                                                   reloco::span<const std::uint64_t> args) const noexcept {
    if (auto found = natives_.try_find(name))
      return found->get().second.fn(found->get().second.user, args);
    const bool two = args.size() == 2;
    if (name == "min" || name == "max") {
      if (!two)
        return reloco::unexpected(reloco::error::invalid_argument);
      const bool lt = args[0] < args[1];
      return (name == "min") == lt ? args[0] : args[1];
    }
    if (name == "align_up" || name == "align_down") {
      if (!two || args[1] == 0)
        return reloco::unexpected(reloco::error::invalid_argument);
      const std::uint64_t down = args[0] - args[0] % args[1];
      return name == "align_down" || down == args[0] ? down : down + args[1];
    }
    return reloco::unexpected(reloco::error::not_found);
  }

  // ---- evaluation ----

  /** @brief Evaluates an integer expression with this context's variables and functions (see `evaluate_expression`). */
  [[nodiscard]] reloco::result<std::uint64_t> evaluate(reloco::string_view text) const noexcept {
    return evaluate_expression(
        text, [this](reloco::string_view n) noexcept { return get(n); },
        [this](reloco::string_view n, reloco::span<const std::uint64_t> a) noexcept { return call(n, a); });
  }

  /**
   * @brief Copies `in` to `out` (replacing its contents) with `$name`, `${name}`, `$N`, `${N}`, `$#` and
   * `$((expression))` substituted. Nothing is substituted inside `'...'` or after a backslash. Variable values are
   * backslash-escaped so `split_command_line` sees each as one word; an unknown variable expands to nothing.
   * `error::invalid_argument` for an unterminated or invalid expression, `error::out_of_memory` on allocation failure.
   */
  [[nodiscard]] reloco::result<void> expand(reloco::string_view in, reloco::sso_string &out) const noexcept {
    out.clear();
    char quote = 0;
    std::size_t i = 0;
    while (i < in.size()) {
      const char c = in[i];
      if (c == '\\' && quote != '\'') {
        if (auto r = out.try_append(in.substr(i, 2)); !r)
          return r;
        i += 2;
        continue;
      }
      if (quote == 0 && (c == '"' || c == '\'')) {
        quote = c;
      } else if (quote == c) {
        quote = 0;
      } else if (c == '$' && quote != '\'') {
        std::size_t used = 0;
        auto r = expand_dollar(in.substr(i), quote, out, used);
        if (!r)
          return r;
        if (used != 0) {
          i += used;
          continue;
        }
      }
      if (auto r = out.try_push_back(c); !r)
        return r;
      ++i;
    }
    return {};
  }

private:
  struct native_entry {
    native_fn fn;
    void *user;
  };

  static bool is_name_char(char c, bool first) noexcept {
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (!first && c >= '0' && c <= '9');
  }
  static bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

  template <typename Map>
  [[nodiscard]] reloco::result<void> assign(Map &map, reloco::string_view name, reloco::string_view value) noexcept {
    if (auto existing = map.try_at(name)) {
      reloco::string &slot = existing->get();
      return slot.try_assign(value);
    }
    auto k = reloco::string::try_allocate(alloc_, name);
    if (!k)
      return reloco::unexpected(k.error());
    auto v = reloco::string::try_allocate(alloc_, value);
    if (!v)
      return reloco::unexpected(v.error());
    auto ins = map.try_insert(std::move(*k), std::move(*v));
    if (!ins)
      return reloco::unexpected(ins.error());
    return {};
  }

  [[nodiscard]] reloco::result<reloco::string_view> positional(reloco::string_view digits) const noexcept {
    std::size_t n = 0;
    for (std::size_t i = 0; i < digits.size(); ++i) {
      if (!is_digit(digits[i]) || n > 100000)
        return reloco::unexpected(reloco::error::not_found);
      n = n * 10 + static_cast<std::size_t>(digits[i] - '0');
    }
    if (n >= current_->args_.size())
      return reloco::unexpected(reloco::error::not_found);
    return current_->args_[n].view();
  }

  [[nodiscard]] static reloco::result<void> append_number(std::uint64_t v, reloco::sso_string &out) noexcept {
    char digits[20];
    std::size_t d = sizeof digits;
    do {
      digits[--d] = static_cast<char>('0' + v % 10);
      v /= 10;
    } while (v != 0);
    return out.try_append(reloco::string_view(digits + d, sizeof digits - d));
  }

  [[nodiscard]] reloco::result<void> append_value(reloco::string_view name, char quote,
                                                  reloco::sso_string &out) const noexcept {
    auto v = get(name);
    if (!v || v->empty()) // an empty value outside quotes must still be one (empty) word
      return quote == 0 && v ? out.try_append("\"\"") : reloco::result<void>{};
    for (std::size_t i = 0; i < v->size(); ++i) {
      const char ch = (*v)[i];
      if (ch == ' ' || ch == '\t' || ch == '"' || ch == '\'' || ch == '\\' || ch == '#')
        if (auto r = out.try_push_back('\\'); !r)
          return r;
      if (auto r = out.try_push_back(ch); !r)
        return r;
    }
    return {};
  }

  // `text` starts with '$'. Appends the substitution and sets `used` to the characters consumed, or leaves
  // `used` 0 when the '$' is literal.
  [[nodiscard]] reloco::result<void> expand_dollar(reloco::string_view text, char quote, reloco::sso_string &out,
                                                   std::size_t &used) const noexcept {
    if (text.starts_with("$((")) {
      // The expression ends at the "))" that closes the opening "((".
      std::size_t e = 3;
      int depth = 0;
      while (e < text.size() && !(text[e] == ')' && depth == 0)) {
        depth += text[e] == '(' ? 1 : text[e] == ')' ? -1 : 0;
        ++e;
      }
      if (e + 1 >= text.size() || text[e + 1] != ')')
        return reloco::unexpected(reloco::error::invalid_argument);
      auto value = evaluate(text.substr(3, e - 3));
      if (!value)
        return reloco::unexpected(value.error());
      used = e + 2;
      return append_number(*value, out);
    }
    std::size_t b = 1;
    const bool braced = b < text.size() && text[b] == '{';
    if (braced)
      ++b;
    if (!braced && b < text.size() && text[b] == '#') {
      used = b + 1;
      return append_number(positional_count(), out);
    }
    std::size_t e = b;
    if (e < text.size() && is_digit(text[e])) {
      ++e; // "$1" is one digit; "${10}" takes them all
      while (braced && e < text.size() && is_digit(text[e]))
        ++e;
    } else {
      while (e < text.size() && is_name_char(text[e], e == b))
        ++e;
    }
    const bool closed = braced && e < text.size() && text[e] == '}';
    if (e == b || (braced && !closed))
      return {};
    used = closed ? e + 1 : e;
    return append_value(text.substr(b, e - b), quote, out);
  }

  reloco::allocator_ref alloc_;
  reloco::flat_map<reloco::string, native_entry, variable_less> natives_;
  string_map functions_;
  scope global_;
  scope *current_;
};

} // namespace structo::bootldr
