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
 * ```cpp
 * char line[] = "load kernel \"my image.bin\" 0x80000";  // writable, NUL-terminated
 * char *argv[8];
 * auto n = structo::bootldr::split_command_line(line, sizeof line - 1, reloco::span<char *>(argv));
 * // *n == 4: argv = {"load", "kernel", "my image.bin", "0x80000"}
 * ```
 */

#include <reloco/error.hpp>
#include <reloco/span.hpp>
#include <reloco/string_view.hpp>

#include <cstddef>
#include <cstdint>

namespace structo::bootldr {

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

} // namespace structo::bootldr
