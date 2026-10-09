// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file cmdline.hpp
 * @brief Sans-IO command line tokenizer and number parser for the bootloader shell.
 *
 * `split_command_line` splits one line into arguments in place (no
 * allocation, works in C++17). Rules:
 *  - arguments are separated by spaces/tabs;
 *  - `"..."` groups words and understands `\"` and `\\`;
 *  - `'...'` groups words literally;
 *  - outside quotes a backslash escapes the next character;
 *  - `#` at the start of an argument ends the line (comment).
 *
 * `evaluate_expression` is the integer calculator behind the shell's `$((expression))` arguments.
 *
 * ```cpp
 * char line[] = "load kernel \"my image.bin\" 0x80000";  // writable, NUL-terminated
 * char *argv[8];
 * auto n = structo::bootldr::split_command_line(line, sizeof line - 1, reloco::span<char *>(argv));
 * // *n == 4: argv = {"load", "kernel", "my image.bin", "0x80000"}
 * ```
 */

#include <reloco/array.hpp>
#include <reloco/error.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/span.hpp>
#include <reloco/string_view.hpp>

#include <cstddef>
#include <cstdint>

namespace structo::bootldr {

// In-place tokenizer over a caller-provided (pointer, len) buffer; every
// index is bounded by `len`/`argv.size()` in the loop conditions.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

/**
 * @brief Splits `line` into NUL-terminated arguments, rewriting the buffer in place.
 *
 * @param line  writable text; `line[len]` must also be writable (it may receive the final NUL).
 * @param len   number of characters in `line`.
 * @param argv  receives pointers into `line`.
 * @return the argument count; `error::invalid_argument` for an unterminated quote or a trailing
 *         backslash, `error::out_of_range` if `argv` is too small.
 */
[[nodiscard]] inline reloco::result<std::size_t> split_command_line(char *line, std::size_t len,
                                                                    reloco::span<char *> argv) noexcept {
  std::size_t argc = 0;
  std::size_t r = 0; // read index
  std::size_t w = 0; // write index; never ahead of r
  while (r < len) {
    while (r < len && (line[r] == ' ' || line[r] == '\t'))
      ++r;
    if (r >= len || line[r] == '#')
      break;

    if (argc == argv.size())
      return reloco::unexpected(reloco::error::out_of_range);
    argv[argc++] = line + w;

    char quote = 0;
    while (r < len) {
      const char c = line[r];
      if (quote == 0 && (c == ' ' || c == '\t'))
        break;
      ++r;
      if (quote == '\'') {
        if (c == '\'')
          quote = 0;
        else
          line[w++] = c;
      } else if (c == '\\') {
        if (r >= len)
          return reloco::unexpected(reloco::error::invalid_argument);
        line[w++] = line[r++];
      } else if (quote == '"') {
        if (c == '"')
          quote = 0;
        else
          line[w++] = c;
      } else if (c == '"' || c == '\'') {
        quote = c;
      } else {
        line[w++] = c;
      }
    }
    if (quote != 0)
      return reloco::unexpected(reloco::error::invalid_argument);
    if (r < len)
      ++r; // step over the separator first: the NUL below may land on its position
    line[w++] = '\0';
  }
  return argc;
}

/**
 * @brief Parses an unsigned number: decimal, `0x` hex, `0b` binary or leading-`0` octal.
 * `error::invalid_argument` for empty or malformed text, `error::out_of_range` on overflow.
 */
[[nodiscard]] inline reloco::result<std::uint64_t> parse_number(reloco::string_view text) noexcept {
  const char *p = text.data();
  std::size_t n = text.size();
  if (n == 0)
    return reloco::unexpected(reloco::error::invalid_argument);

  std::uint64_t base = 10;
  if (n > 1 && p[0] == '0') {
    if (p[1] == 'x' || p[1] == 'X') {
      base = 16;
      p += 2;
      n -= 2;
    } else if (p[1] == 'b' || p[1] == 'B') {
      base = 2;
      p += 2;
      n -= 2;
    } else {
      base = 8;
      p += 1;
      n -= 1;
    }
  }
  if (n == 0)
    return reloco::unexpected(reloco::error::invalid_argument);

  std::uint64_t value = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const char c = p[i];
    std::uint64_t d;
    if (c >= '0' && c <= '9')
      d = static_cast<std::uint64_t>(c - '0');
    else if (c >= 'a' && c <= 'f')
      d = static_cast<std::uint64_t>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      d = static_cast<std::uint64_t>(c - 'A' + 10);
    else
      return reloco::unexpected(reloco::error::invalid_argument);
    if (d >= base)
      return reloco::unexpected(reloco::error::invalid_argument);
    if (value > (UINT64_MAX - d) / base)
      return reloco::unexpected(reloco::error::out_of_range);
    value = value * base + d;
  }
  return value;
}

/**
 * @brief Evaluates an unsigned 64-bit integer expression (wrapping arithmetic).
 *
 * Operators, lowest to highest precedence: `|`, `^`, `&`, `<<` `>>`, `+` `-`, `*` `/` `%`, then unary
 * `+` `-` `~` and parentheses. Numbers use `parse_number` syntax (`42`, `0x1000`, `0b101`, `017`); a name
 * (optionally written `$name`) is looked up with `lookup` and its value parsed the same way; an unknown name is 0.
 * `name(arg, ...)` (at most 8 arguments) calls a function through `call`.
 *
 * @param lookup  callable `reloco::result<reloco::string_view>(reloco::string_view name)`; `error::not_found` means unset.
 * @param call    callable `reloco::result<std::uint64_t>(reloco::string_view name, reloco::span<const std::uint64_t> args)`;
 *                its error (e.g. `error::not_found` for an unknown function) fails the evaluation.
 * @return the value; `error::invalid_argument` for a syntax error, a non-numeric variable or a division by zero,
 *         `error::out_of_range` for overflowing literals, too many arguments or nesting deeper than 32.
 */
template <typename Lookup, typename Call>
[[nodiscard]] reloco::result<std::uint64_t> evaluate_expression(reloco::string_view text, const Lookup &lookup,
                                                                const Call &call) noexcept;

/** @brief `evaluate_expression` without functions: any `name(...)` fails with `error::not_found`. */
template <typename Lookup>
[[nodiscard]] reloco::result<std::uint64_t> evaluate_expression(reloco::string_view text, const Lookup &lookup) noexcept {
  return evaluate_expression(text, lookup, [](reloco::string_view, reloco::span<const std::uint64_t>) noexcept {
    return reloco::result<std::uint64_t>(reloco::unexpected(reloco::error::not_found));
  });
}

namespace detail {

template <typename Lookup, typename Call> class expression_parser {
public:
  expression_parser(reloco::string_view text, const Lookup &lookup, const Call &call) noexcept
      : s_(text), lookup_(lookup), call_(call) {}

  reloco::result<std::uint64_t> parse() noexcept {
    auto v = binary(0, 0);
    if (!v)
      return v;
    skip();
    if (i_ != s_.size())
      return reloco::unexpected(reloco::error::invalid_argument);
    return v;
  }

private:
  static constexpr int max_depth = 32;
  static constexpr int top_level = 5;

  char at(std::size_t k) const noexcept { return k < s_.size() ? s_[k] : '\0'; }
  void skip() noexcept {
    while (at(i_) == ' ' || at(i_) == '\t')
      ++i_;
  }

  // Operator of the given precedence level at the cursor: stores it in op_ and returns its length (0 if none).
  std::size_t match(int level) noexcept {
    const char c = at(i_);
    switch (level) {
    case 0:
      return c == '|' ? (op_ = c, 1) : 0;
    case 1:
      return c == '^' ? (op_ = c, 1) : 0;
    case 2:
      return c == '&' ? (op_ = c, 1) : 0;
    case 3:
      return (c == '<' || c == '>') && at(i_ + 1) == c ? (op_ = c, 2) : 0;
    case 4:
      return c == '+' || c == '-' ? (op_ = c, 1) : 0;
    default:
      return c == '*' || c == '/' || c == '%' ? (op_ = c, 1) : 0;
    }
  }

  reloco::result<std::uint64_t> binary(int level, int depth) noexcept {
    if (level > top_level)
      return unary(depth);
    auto lhs = binary(level + 1, depth);
    if (!lhs)
      return lhs;
    for (;;) {
      skip();
      const std::size_t len = match(level);
      if (len == 0)
        return lhs;
      const char op = op_;
      i_ += len;
      auto rhs = binary(level + 1, depth);
      if (!rhs)
        return rhs;
      const std::uint64_t a = *lhs, b = *rhs;
      switch (op) {
      case '|': lhs = a | b; break;
      case '^': lhs = a ^ b; break;
      case '&': lhs = a & b; break;
      case '<': lhs = b >= 64 ? 0 : a << b; break;
      case '>': lhs = b >= 64 ? 0 : a >> b; break;
      case '+': lhs = a + b; break;
      case '-': lhs = a - b; break;
      case '*': lhs = a * b; break;
      default:
        if (b == 0)
          return reloco::unexpected(reloco::error::invalid_argument);
        lhs = op == '/' ? a / b : a % b;
      }
    }
  }

  reloco::result<std::uint64_t> unary(int depth) noexcept {
    if (depth > max_depth)
      return reloco::unexpected(reloco::error::out_of_range);
    skip();
    const char c = at(i_);
    if (c == '-' || c == '+' || c == '~') {
      ++i_;
      auto v = unary(depth + 1);
      if (!v)
        return v;
      return c == '-' ? std::uint64_t{0} - *v : c == '~' ? ~*v : *v;
    }
    if (c == '(') {
      ++i_;
      auto v = binary(0, depth + 1);
      if (!v)
        return v;
      skip();
      if (at(i_) != ')')
        return reloco::unexpected(reloco::error::invalid_argument);
      ++i_;
      return v;
    }
    const bool dollar = c == '$';
    const std::size_t b = dollar ? i_ + 1 : i_;
    std::size_t e = b;
    while (is_word(at(e)))
      ++e;
    if (e == b)
      return reloco::unexpected(reloco::error::invalid_argument);
    const reloco::string_view word = s_.substr(b, e - b);
    i_ = e;
    if (!dollar) {
      skip();
      if (at(i_) == '(')
        return call_function(word, depth);
    }
    const char first = word[0];
    if (!dollar && first >= '0' && first <= '9')
      return parse_number(word);
    auto value = lookup_(word);
    if (!value)
      return value.error() == reloco::error::not_found ? reloco::result<std::uint64_t>(std::uint64_t{0})
                                                       : reloco::result<std::uint64_t>(reloco::unexpected(value.error()));
    if (value->empty())
      return std::uint64_t{0};
    return parse_number(*value);
  }

  reloco::result<std::uint64_t> call_function(reloco::string_view name, int depth) noexcept {
    reloco::array<std::uint64_t, 8> args{};
    std::size_t n = 0;
    ++i_; // '('
    skip();
    if (at(i_) == ')') {
      ++i_;
    } else {
      for (;;) {
        if (n == args.size())
          return reloco::unexpected(reloco::error::out_of_range);
        auto v = binary(0, depth + 1);
        if (!v)
          return v;
        args[n++] = *v;
        skip();
        const char c = at(i_++);
        if (c == ')')
          break;
        if (c != ',')
          return reloco::unexpected(reloco::error::invalid_argument);
      }
    }
    return call_(name, reloco::span<const std::uint64_t>(args.data(), n));
  }

  static bool is_word(char c) noexcept {
    return c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
  }

  reloco::string_view s_;
  const Lookup &lookup_;
  const Call &call_;
  std::size_t i_ = 0;
  char op_ = 0;
};

} // namespace detail

template <typename Lookup, typename Call>
[[nodiscard]] reloco::result<std::uint64_t> evaluate_expression(reloco::string_view text, const Lookup &lookup,
                                                                const Call &call) noexcept {
  return detail::expression_parser<Lookup, Call>(text, lookup, call).parse();
}

RELOCO_END_UNSAFE_BUFFER_USAGE

} // namespace structo::bootldr
