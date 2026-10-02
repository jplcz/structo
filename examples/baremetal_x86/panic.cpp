// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "panic.hpp"

#include <cstddef>
#include <cstdint>

namespace {

constexpr std::uintptr_t vga_base = 0xB8000;
constexpr std::size_t vga_columns = 80;
constexpr std::size_t vga_rows = 25;
// White-on-red: unmistakably distinct from kmain's own banner colors.
constexpr std::uint8_t panic_attribute = 0x4F;

// No libc/`<cstring>` here deliberately -- this path must still work if
// the rest of the kernel's state (including whatever heap/allocator the
// panicking assertion itself may have been guarding) is suspect.
std::size_t raw_strlen(const char *s) noexcept {
  std::size_t n = 0;
  while (s != nullptr && s[n] != '\0')
    ++n;
  return n;
}

// Writes `text` starting at row `row`, column 0, truncated to the
// screen width; wraps to the next row only when the caller advances
// `row` itself (kept deliberately simple -- a panic handler is not the
// place for `vt100_terminal`'s line-wrap/scroll logic).
void put_line(std::size_t row, const char *text) noexcept {
  if (row >= vga_rows)
    return;
  auto *cell = reinterpret_cast<volatile std::uint16_t *>(vga_base + row * vga_columns * 2);
  std::size_t len = raw_strlen(text);
  std::size_t col = 0;
  for (; col < vga_columns && col < len; ++col)
    cell[col] = static_cast<std::uint16_t>(static_cast<unsigned char>(text[col])) |
               (static_cast<std::uint16_t>(panic_attribute) << 8);
  for (; col < vga_columns; ++col)
    cell[col] = static_cast<std::uint16_t>(' ') | (static_cast<std::uint16_t>(panic_attribute) << 8);
}

// Decimal, unsigned only (line numbers are never negative) -- writes
// into `buf` (must be at least 11 bytes: up to 10 digits plus NUL) and
// returns it.
const char *itoa_unsigned(unsigned value, char *buf, std::size_t buf_size) noexcept {
  if (buf_size == 0)
    return buf;
  std::size_t pos = buf_size - 1;
  buf[pos] = '\0';
  if (value == 0) {
    buf[--pos] = '0';
    return &buf[pos];
  }
  while (value != 0 && pos > 0) {
    buf[--pos] = static_cast<char>('0' + (value % 10));
    value /= 10;
  }
  return &buf[pos];
}

} // namespace

extern "C" [[noreturn]] void baremetal_panic(const char *expression, const char *file, int line,
                                             const char *message) noexcept {
  char line_buf[11];
  put_line(0, "*** KERNEL PANIC ***");
  put_line(1, expression != nullptr ? expression : "(no expression)");
  put_line(2, file != nullptr ? file : "(no file)");
  put_line(3, itoa_unsigned(line < 0 ? 0u : static_cast<unsigned>(line), line_buf, sizeof(line_buf)));
  put_line(4, message != nullptr && message[0] != '\0' ? message : "(no message)");

  for (;;) {
    asm volatile("cli; hlt");
  }
}
