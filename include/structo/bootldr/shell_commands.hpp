// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file shell_commands.hpp
 * @brief A ready-made set of generic bootloader commands for `bootldr::shell`. C++20 only.
 *
 * | command | what it does |
 * |---------|--------------|
 * | `echo [words...]` | print the arguments |
 * | `sleep <ms>` | wait (needs `scheduler::set_clock`); other tasks keep running |
 * | `uptime` | milliseconds since the scheduler clock started |
 * | `md <addr> [len]` | hex dump `len` bytes (default 64) |
 * | `mw <addr> <value> [width]` | write a 1/2/4/8 byte value (default 4) |
 * | `cp <dst> <src> <len>` | copy memory (overlap-safe) |
 * | `cmp <a> <b> <len>` | compare memory; on a mismatch prints a `microfmt::mem_diff` of the differing rows |
 * | `fill <addr> <len> <byte>` | fill memory |
 * | `crc32 <addr> <len> [var]` | CRC-32 (IEEE, as in zlib) of memory; prints it and optionally stores it in variable
 * `var` | | `find <addr> <len> <value> [width]` | print every address in the range holding `value` (width 1/2/4/8,
 * default 1; scanned at `width` steps) | | `mtest <addr> <len>` | destructive RAM test of word-aligned memory (walking
 * patterns and address-as-data); fails with `io_error` on a mismatch | | `env [name]` | list the visible variables, or
 * print one | | `if <a> <op> <b> 'then' ['else']` | run `then` (or `else`) as a script; `op` is `== != < <= > >=`,
 * numbers compare unsigned, otherwise `==`/`!=` compare text | | `repeat <n> 'script'` | run `script` `n` times with
 * variable `i` = 0..n-1 in the current scope | | `source <addr> [len]` | run the script stored in memory (one line per
 * `\n`, `;` separates statements); without `len` it ends at the first NUL | | `go <addr>` | jump to an address (only if
 * a `go` hook is provided) | | `reset` | restart the board (only if a `reset` hook is provided) |
 *
 * Expression functions (usable inside `$((...))`, registered by `add_all()`/`add_functions()` next to the
 * built-in `min`/`max`/`align_up`/`align_down`):
 *
 * | function | value |
 * |----------|-------|
 * | `peek8(addr)`, `peek16(addr)`, `peek32(addr)`, `peek64(addr)` | the native-endian value at `addr`, read with the
 * `read_memory` hook (fails with `out_of_bounds` on a fault) | | `bit(n)` | `1 << n` (0 when `n >= 64`) | | `mask(n)` |
 * the `n` low bits set (all ones when `n >= 64`) |
 *
 * For example `set magic $((peek32(0x1000)))` or `mw $((align_up(addr, 8))) $((bit(3) | mask(2)))`.
 *
 * `help` is not part of this set: every `shell` has it built in.
 * Addresses and numbers accept decimal, `0x` hex, `0b` binary and octal (`parse_number`).
 * Memory commands touch the given addresses directly (volatile byte/word accesses), so they are
 * only as safe as the addresses you type; long copies/fills yield to other tasks every 4 KiB.
 *
 * ```cpp
 * structo::bootldr::shell sh{sched, uart};
 *
 * // Platform hooks are optional; a command whose hook is null is not registered.
 * structo::bootldr::generic_commands_hooks hooks;
 * hooks.reset = [](void *) noexcept { board_reset(); };                    // "reset"
 * hooks.go = [](void *, std::uint64_t addr) noexcept { jump_to(addr); };   // "go <addr>"
 * hooks.read_memory = probe_read;   // optional fault-safe reader for md/cmp: size_t(void *, uintptr_t, uint8_t *,
 * size_t) hooks.ctx = nullptr;                                                     // passed back to the hooks and the
 * reader
 *
 * structo::bootldr::generic_commands cmds{sh, hooks};   // owns the command objects; keep it alive as long as `sh`
 * (void)cmds.add_all();                                 // registers every command of the table above
 * (void)sh.start();
 * ```
 */

#include <structo/bootldr/shell.hpp>

#include <microfmt/formatters/hexdump.hpp>
#include <microfmt/inspector/memory_diff.hpp>
#include <microfmt/microfmt.hpp>

#include <reloco/array.hpp>
#include <reloco/error.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/sso_string.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if RELOCO_HAS_COROUTINES

namespace structo::bootldr {

/** @brief Platform actions behind `reset` and `go`; a null hook leaves its command unregistered. */
struct generic_commands_hooks {
  void (*reset)(void *ctx) noexcept = nullptr;                     ///< restart the board
  void (*go)(void *ctx, std::uint64_t address) noexcept = nullptr; ///< transfer control to `address`
  /// Fault-safe memory reader used by `md`/`cmp` (`microfmt::memory_reader_fn_t`: returns the bytes read, 0 on a
  /// fault); null = plain volatile reads. Receives `ctx`.
  microfmt::memory_reader_fn_t read_memory = nullptr;
  void *ctx = nullptr; ///< first argument of the hooks and the reader
};

/** @brief The generic command set; owns its `shell_command` objects (unregistered on destruction). */
class generic_commands {
public:
  explicit generic_commands(shell_base &sh, const generic_commands_hooks &hooks = {}) noexcept
      : sh_(&sh), hooks_(hooks), echo_("echo", "echo [words...]: print the arguments", &echo_cmd),
        sleep_("sleep", "sleep <ms>: wait", &sleep_cmd),
        uptime_("uptime", "uptime: milliseconds on the scheduler clock", &uptime_cmd),
        md_("md", "md <addr> [len=64]: hex dump memory", &md_cmd, this),
        mw_("mw", "mw <addr> <value> [width=4]: write memory (width 1/2/4/8)", &mw_cmd),
        cp_("cp", "cp <dst> <src> <len>: copy memory", &cp_cmd),
        cmp_("cmp", "cmp <a> <b> <len>: compare memory", &cmp_cmd, this),
        fill_("fill", "fill <addr> <len> <byte>: fill memory", &fill_cmd),
        source_("source", "source <addr> [len]: run the script (lines, ';'-separated statements) stored in memory",
                &source_cmd, this),
        crc32_("crc32", "crc32 <addr> <len> [var]: CRC-32 of memory, optionally stored in a variable", &crc32_cmd,
               this),
        find_("find", "find <addr> <len> <value> [width=1]: search memory for a value", &find_cmd, this),
        mtest_("mtest", "mtest <addr> <len>: destructive RAM test (word aligned)", &mtest_cmd),
        env_("env", "env [name]: list variables, or print one", &env_cmd),
        if_("if", "if <a> <op> <b> 'then' ['else']: op is == != < <= > >=", &if_cmd),
        repeat_("repeat", "repeat <n> 'script': run script n times, $i = 0..n-1", &repeat_cmd),
        go_("go", "go <addr>: jump to an address", &go_cmd, this),
        reset_("reset", "reset: restart the board", &reset_cmd, this) {}

  generic_commands(const generic_commands &) = delete;
  generic_commands &operator=(const generic_commands &) = delete;
  ~generic_commands() {
    for (const reloco::string_view n : function_names)
      (void)sh_->context().remove_function(n);
  }

  /**
   * @brief Registers the expression functions `peek8/16/32/64`, `bit` and `mask` in the shell's context
   * (unregistered on destruction). Calling it twice just replaces them.
   */
  [[nodiscard]] reloco::result<void> add_functions() noexcept {
    shell_context &ctx = sh_->context();
    constexpr reloco::array<shell_context::native_fn, 6> fns = {&peek8_fn,  &peek16_fn, &peek32_fn,
                                                                &peek64_fn, &bit_fn,    &mask_fn};
    for (std::size_t i = 0; i < function_names.size(); ++i)
      if (auto r = ctx.add_function(function_names[i], fns[i], this); !r)
        return r;
    return {};
  }

  /** @brief Registers every command (hook-backed ones only if their hook is set) and the expression functions.
   * Stops at the first failure. */
  [[nodiscard]] reloco::result<void> add_all() noexcept {
    if (auto r = add_functions(); !r)
      return r;
    shell_command *all[] = {&echo_,   &sleep_, &uptime_, &md_,    &mw_,  &cp_, &cmp_,   &fill_,
                            &source_, &crc32_, &find_,   &mtest_, &env_, &if_, &repeat_};
    for (shell_command *c : all)
      if (auto r = sh_->add(*c); !r)
        return r;
    if (hooks_.go)
      if (auto r = sh_->add(go_); !r)
        return r;
    if (hooks_.reset)
      if (auto r = sh_->add(reset_); !r)
        return r;
    return {};
  }

private:
  static constexpr std::size_t chunk = 4096;
  static constexpr std::uint64_t script_max = 64 * 1024;
  static constexpr reloco::array<reloco::string_view, 6> function_names{"peek8",  "peek16", "peek32",
                                                                        "peek64", "bit",    "mask"};

  // peekN(addr): reads `width` bytes through the reader (a hook may be fault-safe) as a native-endian integer.
  static reloco::result<std::uint64_t> peek(void *user, reloco::span<const std::uint64_t> args,
                                            std::size_t width) noexcept {
    if (args.size() != 1)
      return reloco::unexpected(reloco::error::invalid_argument);
    std::uint64_t value = 0; // little-endian hosts fill the low bytes; big-endian ones are shifted below
    std::uint8_t bytes[8]{};
    if (static_cast<generic_commands *>(user)->reader()(static_cast<generic_commands *>(user)->hooks_.ctx,
                                                        static_cast<std::uintptr_t>(args[0]), bytes, width) != width)
      return reloco::unexpected(reloco::error::out_of_bounds);
    RELOCO_BEGIN_UNSAFE_BUFFER_USAGE;
    std::memcpy(&value, bytes, width);
    RELOCO_END_UNSAFE_BUFFER_USAGE;
    if constexpr (std::endian::native == std::endian::big)
      value >>= (8 - width) * 8;
    return value;
  }
  static reloco::result<std::uint64_t> peek8_fn(void *u, reloco::span<const std::uint64_t> a) noexcept {
    return peek(u, a, 1);
  }
  static reloco::result<std::uint64_t> peek16_fn(void *u, reloco::span<const std::uint64_t> a) noexcept {
    return peek(u, a, 2);
  }
  static reloco::result<std::uint64_t> peek32_fn(void *u, reloco::span<const std::uint64_t> a) noexcept {
    return peek(u, a, 4);
  }
  static reloco::result<std::uint64_t> peek64_fn(void *u, reloco::span<const std::uint64_t> a) noexcept {
    return peek(u, a, 8);
  }
  static reloco::result<std::uint64_t> bit_fn(void *, reloco::span<const std::uint64_t> a) noexcept {
    if (a.size() != 1)
      return reloco::unexpected(reloco::error::invalid_argument);
    return a[0] >= 64 ? std::uint64_t{0} : std::uint64_t{1} << a[0];
  }
  static reloco::result<std::uint64_t> mask_fn(void *, reloco::span<const std::uint64_t> a) noexcept {
    if (a.size() != 1)
      return reloco::unexpected(reloco::error::invalid_argument);
    return a[0] >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << a[0]) - 1;
  }

  // Prints "usage: <help>" for the running command and returns the error to fail with.
  static reloco::error usage(command_call &call) noexcept {
    if (shell_command *c = call.sh().find(call.arg(0)))
      (void)call.print("usage: {}\n", c->help());
    return reloco::error::invalid_argument;
  }

  // Default reader: plain volatile byte reads; the caller guarantees [src, src + n) is readable and dst holds n bytes.
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  static std::size_t volatile_read(void *, std::uintptr_t src, std::uint8_t *dst, std::size_t n) noexcept {
    const volatile std::uint8_t *p = reinterpret_cast<const volatile std::uint8_t *>(src);
    for (std::size_t i = 0; i < n; ++i)
      dst[i] = p[i];
    return n;
  }
  RELOCO_END_UNSAFE_BUFFER_USAGE

  [[nodiscard]] microfmt::memory_reader_fn_t reader() const noexcept {
    return hooks_.read_memory ? hooks_.read_memory : &volatile_read;
  }

  static volatile std::uint8_t *ptr(std::uint64_t address) noexcept {
    return reinterpret_cast<volatile std::uint8_t *>(static_cast<std::uintptr_t>(address));
  }

  static reloco::task<void> echo_cmd(command_call &call) noexcept {
    for (std::size_t i = 1; i < call.argc(); ++i)
      (void)call.print(i == 1 ? "{}" : " {}", call.arg(i));
    (void)call.write("\n");
    co_return;
  }

  static reloco::task<void> sleep_cmd(command_call &call) noexcept {
    if (call.argc() != 2)
      co_await reloco::unexpected(usage(call));
    auto ms = parse_number(call.arg(1));
    if (!ms)
      co_await reloco::unexpected(usage(call));
    co_await co_await call.sh().sched().sleep_for(*ms); // fails with invalid_state without a clock
  }

  static reloco::task<void> uptime_cmd(command_call &call) noexcept {
    (void)call.print("{} ms\n", call.sh().sched().now_ms());
    co_return;
  }

  static reloco::task<void> md_cmd(command_call &call) noexcept {
    if (call.argc() < 2 || call.argc() > 3)
      co_await reloco::unexpected(usage(call));
    auto addr = parse_number(call.arg(1));
    auto len = call.argc() == 3 ? parse_number(call.arg(2)) : reloco::result<std::uint64_t>(std::uint64_t{64});
    if (!addr || !len)
      co_await reloco::unexpected(usage(call));
    auto &self = *static_cast<generic_commands *>(call.ctx());
    // One 16-byte row per print(); the reader is probed per row so a fault shows up as a short row.
    for (std::uint64_t off = 0; off < *len; off += 16) {
      const std::size_t n = *len - off < 16 ? static_cast<std::size_t>(*len - off) : 16;
      (void)call.print("{}\n", microfmt::hexdump_checked(static_cast<std::uintptr_t>(*addr + off), n, self.reader(),
                                                         self.hooks_.ctx));
      if ((off / 16) % 16 == 15)
        co_await call.sh().sched().yield();
    }
  }

  static reloco::task<void> mw_cmd(command_call &call) noexcept {
    if (call.argc() < 3 || call.argc() > 4)
      co_await reloco::unexpected(usage(call));
    auto addr = parse_number(call.arg(1));
    auto value = parse_number(call.arg(2));
    auto width = call.argc() == 4 ? parse_number(call.arg(3)) : reloco::result<std::uint64_t>(std::uint64_t{4});
    if (!addr || !value || !width)
      co_await reloco::unexpected(usage(call));
    // Whole-width accesses, so device registers see one bus access of the requested size.
    const auto p = static_cast<std::uintptr_t>(*addr);
    switch (*width) {
    case 1:
      *reinterpret_cast<volatile std::uint8_t *>(p) = static_cast<std::uint8_t>(*value);
      break;
    case 2:
      *reinterpret_cast<volatile std::uint16_t *>(p) = static_cast<std::uint16_t>(*value);
      break;
    case 4:
      *reinterpret_cast<volatile std::uint32_t *>(p) = static_cast<std::uint32_t>(*value);
      break;
    case 8:
      *reinterpret_cast<volatile std::uint64_t *>(p) = *value;
      break;
    default:
      co_await reloco::unexpected(usage(call));
    }
  }

  // Raw memory-access commands: every address is user-supplied by design and the loops are bounded by the
  // user-supplied length; the shell is a debug tool that deliberately touches arbitrary memory.
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

  static reloco::task<void> cp_cmd(command_call &call) noexcept {
    if (call.argc() != 4)
      co_await reloco::unexpected(usage(call));
    auto dst = parse_number(call.arg(1));
    auto src = parse_number(call.arg(2));
    auto len = parse_number(call.arg(3));
    if (!dst || !src || !len)
      co_await reloco::unexpected(usage(call));
    volatile std::uint8_t *d = ptr(*dst);
    const volatile std::uint8_t *s = ptr(*src);
    if (*dst <= *src) {
      for (std::uint64_t i = 0; i < *len; ++i) {
        d[i] = s[i];
        if (i % chunk == chunk - 1)
          co_await call.sh().sched().yield();
      }
    } else { // destination overlaps the tail of the source: copy backwards
      for (std::uint64_t i = *len; i > 0; --i) {
        d[i - 1] = s[i - 1];
        if (i % chunk == 0)
          co_await call.sh().sched().yield();
      }
    }
    (void)call.print("{} bytes copied\n", *len);
  }

  static reloco::task<void> cmp_cmd(command_call &call) noexcept {
    if (call.argc() != 4)
      co_await reloco::unexpected(usage(call));
    auto a = parse_number(call.arg(1));
    auto b = parse_number(call.arg(2));
    auto len = parse_number(call.arg(3));
    if (!a || !b || !len)
      co_await reloco::unexpected(usage(call));
    auto &self = *static_cast<generic_commands *>(call.ctx());
    const volatile std::uint8_t *pa = ptr(*a);
    const volatile std::uint8_t *pb = ptr(*b);
    for (std::uint64_t i = 0; i < *len; ++i) {
      if (pa[i] != pb[i]) {
        // Show the rows around the first difference (up to 4 rows) as a microfmt memory diff.
        const std::uint64_t start = i & ~std::uint64_t{15};
        const std::size_t n = *len - start < 64 ? static_cast<std::size_t>(*len - start) : 64;
        std::uint8_t wa[64], wb[64];
        const std::size_t ra = self.reader()(self.hooks_.ctx, static_cast<std::uintptr_t>(*a + start), wa, n);
        const std::size_t rb = self.reader()(self.hooks_.ctx, static_cast<std::uintptr_t>(*b + start), wb, n);
        const std::size_t got = ra < rb ? ra : rb;
        (void)call.print("differ at offset {:#x}\n", i);
        (void)call.print("{}", microfmt::mem_diff(microfmt::span<const std::uint8_t>(wa, got),
                                                  microfmt::span<const std::uint8_t>(wb, got),
                                                  static_cast<std::uintptr_t>(*a + start)));
        co_await reloco::unexpected(reloco::error::invalid_state);
      }
      if (i % chunk == chunk - 1)
        co_await call.sh().sched().yield();
    }
    (void)call.print("equal ({} bytes)\n", *len);
  }

  static reloco::task<void> fill_cmd(command_call &call) noexcept {
    if (call.argc() != 4)
      co_await reloco::unexpected(usage(call));
    auto addr = parse_number(call.arg(1));
    auto len = parse_number(call.arg(2));
    auto value = parse_number(call.arg(3));
    if (!addr || !len || !value || *value > 0xff)
      co_await reloco::unexpected(usage(call));
    volatile std::uint8_t *p = ptr(*addr);
    for (std::uint64_t i = 0; i < *len; ++i) {
      p[i] = static_cast<std::uint8_t>(*value);
      if (i % chunk == chunk - 1)
        co_await call.sh().sched().yield();
    }
  }

  RELOCO_END_UNSAFE_BUFFER_USAGE

  // source <addr> [len]: copies the buffer to the heap (the script may overwrite the memory it came from),
  // then runs it as a script in the current scope. Without `len` it ends at the first NUL (at most script_max bytes).
  static reloco::task<void> source_cmd(command_call &call) noexcept {
    auto &self = *static_cast<generic_commands *>(call.ctx());
    if (call.argc() < 2 || call.argc() > 3)
      co_await reloco::unexpected(usage(call));
    auto addr = parse_number(call.arg(1));
    auto len = call.argc() == 3 ? parse_number(call.arg(2)) : reloco::result<std::uint64_t>(script_max);
    if (!addr || !len)
      co_await reloco::unexpected(usage(call));
    const bool explicit_len = call.argc() == 3;

    reloco::sso_string text(call.sh().sched().allocator());
    reloco::array<std::uint8_t, 64> part;
    std::uint64_t got = 0;
    while (got < *len) {
      const std::size_t want = *len - got < sizeof part ? static_cast<std::size_t>(*len - got) : sizeof part;
      const std::size_t n = self.reader()(self.hooks_.ctx, static_cast<std::uintptr_t>(*addr + got), part.data(), want);
      std::size_t keep = n;
      bool done = n != want;
      if (!explicit_len)
        for (std::size_t i = 0; i < n; ++i)
          if (part[i] == 0) {
            keep = i;
            done = true;
            break;
          }
      RELOCO_BEGIN_UNSAFE_BUFFER_USAGE;
      if (auto r = text.try_append(reloco::string_view(reinterpret_cast<const char *>(part.data()), keep)); !r)
        co_await reloco::unexpected(r.error());
      RELOCO_END_UNSAFE_BUFFER_USAGE;
      got += keep;
      if (done) {
        if (explicit_len && n != want) // fault inside the requested range
          co_await reloco::unexpected(reloco::error::out_of_bounds);
        break;
      }
      if (got % chunk == 0)
        co_await call.sh().sched().yield();
    }
    co_await co_await call.sh().run_script(text.view());
  }

  // CRC-32 (reflected, polynomial 0xEDB88320, init/xorout 0xFFFFFFFF), bitwise: no table, so no flash/RAM cost.
  static std::uint32_t crc32_update(std::uint32_t crc, const std::uint8_t *data, std::size_t n) noexcept {
    RELOCO_BEGIN_UNSAFE_BUFFER_USAGE;
    for (std::size_t i = 0; i < n; ++i) {
      crc ^= data[i];
      for (int b = 0; b < 8; ++b)
        crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
    }
    RELOCO_END_UNSAFE_BUFFER_USAGE;
    return crc;
  }

  // crc32 <addr> <len> [var]
  static reloco::task<void> crc32_cmd(command_call &call) noexcept {
    auto &self = *static_cast<generic_commands *>(call.ctx());
    if (call.argc() < 3 || call.argc() > 4)
      co_await reloco::unexpected(usage(call));
    auto addr = parse_number(call.arg(1));
    auto len = parse_number(call.arg(2));
    if (!addr || !len)
      co_await reloco::unexpected(usage(call));
    std::uint32_t crc = 0xFFFFFFFFu;
    std::uint8_t part[64];
    for (std::uint64_t off = 0; off < *len;) {
      const std::size_t want = *len - off < sizeof part ? static_cast<std::size_t>(*len - off) : sizeof part;
      if (self.reader()(self.hooks_.ctx, static_cast<std::uintptr_t>(*addr + off), part, want) != want)
        co_await reloco::unexpected(reloco::error::out_of_bounds);
      crc = crc32_update(crc, part, want);
      off += want;
      if (off % chunk == 0)
        co_await call.sh().sched().yield();
    }
    crc = ~crc;
    (void)call.print("{:#010x}\n", crc);
    if (call.argc() == 4) {
      const auto text = microfmt::format<24>("{:#x}", crc);
      if (auto r = call.sh().context().set(call.arg(3), text.view()); !r)
        co_await reloco::unexpected(r.error());
    }
  }

  // find <addr> <len> <value> [width]
  static reloco::task<void> find_cmd(command_call &call) noexcept {
    auto &self = *static_cast<generic_commands *>(call.ctx());
    if (call.argc() < 4 || call.argc() > 5)
      co_await reloco::unexpected(usage(call));
    auto addr = parse_number(call.arg(1));
    auto len = parse_number(call.arg(2));
    auto value = parse_number(call.arg(3));
    auto width = call.argc() == 5 ? parse_number(call.arg(4)) : reloco::result<std::uint64_t>(std::uint64_t{1});
    if (!addr || !len || !value || !width || (*width != 1 && *width != 2 && *width != 4 && *width != 8))
      co_await reloco::unexpected(usage(call));
    const std::size_t w = static_cast<std::size_t>(*width);
    for (std::uint64_t off = 0; off + w <= *len; off += w) {
      std::uint8_t bytes[8]{};
      if (self.reader()(self.hooks_.ctx, static_cast<std::uintptr_t>(*addr + off), bytes, w) != w)
        co_await reloco::unexpected(reloco::error::out_of_bounds);
      std::uint64_t got = 0;
      RELOCO_BEGIN_UNSAFE_BUFFER_USAGE;
      std::memcpy(&got, bytes, w);
      RELOCO_END_UNSAFE_BUFFER_USAGE;
      if constexpr (std::endian::native == std::endian::big)
        got >>= (8 - w) * 8;
      if (got == *value)
        (void)call.print("{:#x}\n", *addr + off);
      if ((off / w) % chunk == chunk - 1)
        co_await call.sh().sched().yield();
    }
  }

  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

  // mtest <addr> <len>: destroys the contents. Passes: all-zero, all-ones, 0x55../0xAA.. and address-as-data,
  // each written over the whole range and then verified (the last one catches address-line aliasing).
  static reloco::task<void> mtest_cmd(command_call &call) noexcept {
    if (call.argc() != 3)
      co_await reloco::unexpected(usage(call));
    auto addr = parse_number(call.arg(1));
    auto len = parse_number(call.arg(2));
    if (!addr || !len || *addr % 4 != 0)
      co_await reloco::unexpected(usage(call));
    const std::uint64_t words = *len / 4;
    auto *mem = reinterpret_cast<volatile std::uint32_t *>(static_cast<std::uintptr_t>(*addr));
    constexpr std::uint32_t fixed[] = {0u, 0xFFFFFFFFu, 0x55555555u, 0xAAAAAAAAu};
    std::uint64_t errors = 0;
    for (std::size_t pass = 0; pass < 5; ++pass) {
      const auto expected = [&](std::uint64_t i) noexcept {
        return pass < 4 ? fixed[pass] : static_cast<std::uint32_t>(*addr + i * 4);
      };
      for (std::uint64_t i = 0; i < words; ++i) {
        mem[i] = expected(i);
        if (i % (chunk / 4) == chunk / 4 - 1)
          co_await call.sh().sched().yield();
      }
      for (std::uint64_t i = 0; i < words; ++i) {
        const std::uint32_t got = mem[i];
        if (got != expected(i)) {
          if (++errors <= 8)
            (void)call.print("{:#x}: wrote {:#010x}, read {:#010x}\n", *addr + i * 4, expected(i), got);
        }
        if (i % (chunk / 4) == chunk / 4 - 1)
          co_await call.sh().sched().yield();
      }
    }
    if (errors == 0) {
      (void)call.write("ok\n");
    } else {
      (void)call.print("{} error(s)\n", errors);
      co_await reloco::unexpected(reloco::error::io_error);
    }
  }

  RELOCO_END_UNSAFE_BUFFER_USAGE

  // env [name]
  static reloco::task<void> env_cmd(command_call &call) noexcept {
    const shell_context &ctx = call.sh().context();
    if (call.argc() > 2)
      co_await reloco::unexpected(usage(call));
    if (call.argc() == 2) {
      auto v = ctx.get(call.arg(1));
      if (!v)
        co_await reloco::unexpected(v.error());
      (void)call.print("{}\n", *v);
      co_return;
    }
    ctx.for_each_variable(
        [&](reloco::string_view name, reloco::string_view value) { (void)call.print("{}={}\n", name, value); });
  }

  // `a op b`: numbers compare unsigned; otherwise only == and != are allowed and compare the text.
  static reloco::result<bool> compare(reloco::string_view a, reloco::string_view op, reloco::string_view b) noexcept {
    const auto x = parse_number(a);
    const auto y = parse_number(b);
    if (x && y) {
      if (op == "==")
        return *x == *y;
      if (op == "!=")
        return *x != *y;
      if (op == "<")
        return *x < *y;
      if (op == "<=")
        return *x <= *y;
      if (op == ">")
        return *x > *y;
      if (op == ">=")
        return *x >= *y;
    } else {
      if (op == "==")
        return a == b;
      if (op == "!=")
        return a != b;
    }
    return reloco::unexpected(reloco::error::invalid_argument);
  }

  // if <a> <op> <b> 'then' ['else']: the script is copied first because running it reuses the argument storage.
  static reloco::task<void> if_cmd(command_call &call) noexcept {
    if (call.argc() < 5 || call.argc() > 6)
      co_await reloco::unexpected(usage(call));
    auto cond = compare(call.arg(1), call.arg(2), call.arg(3));
    if (!cond)
      co_await reloco::unexpected(usage(call));
    const reloco::string_view chosen = *cond ? call.arg(4) : call.arg(5);
    reloco::sso_string text(call.sh().sched().allocator());
    if (auto r = text.try_assign(chosen); !r)
      co_await reloco::unexpected(r.error());
    if (!text.empty())
      co_await co_await call.sh().run_script(text.view());
  }

  // repeat <n> 'script'
  static reloco::task<void> repeat_cmd(command_call &call) noexcept {
    if (call.argc() != 3)
      co_await reloco::unexpected(usage(call));
    auto n = parse_number(call.arg(1));
    if (!n)
      co_await reloco::unexpected(usage(call));
    reloco::sso_string text(call.sh().sched().allocator());
    if (auto r = text.try_assign(call.arg(2)); !r)
      co_await reloco::unexpected(r.error());
    for (std::uint64_t i = 0; i < *n; ++i) {
      const auto num = microfmt::format<24>("{}", i);
      if (auto r = call.sh().context().set("i", num.view()); !r)
        co_await reloco::unexpected(r.error());
      co_await co_await call.sh().run_script(text.view());
      co_await call.sh().sched().yield();
    }
  }

  static reloco::task<void> go_cmd(command_call &call) noexcept {
    auto &self = *static_cast<generic_commands *>(call.ctx());
    auto addr = call.argc() == 2 ? parse_number(call.arg(1))
                                 : reloco::result<std::uint64_t>(reloco::unexpected(reloco::error::invalid_argument));
    if (!addr)
      co_await reloco::unexpected(usage(call));
    (void)call.print("jumping to {:#x}\n", *addr);
    self.hooks_.go(self.hooks_.ctx, *addr);
  }

  static reloco::task<void> reset_cmd(command_call &call) noexcept {
    auto &self = *static_cast<generic_commands *>(call.ctx());
    (void)call.write("resetting\n");
    self.hooks_.reset(self.hooks_.ctx);
    co_return;
  }

  shell_base *sh_;
  generic_commands_hooks hooks_;
  shell_command echo_, sleep_, uptime_, md_, mw_, cp_, cmp_, fill_, source_, crc32_, find_, mtest_, env_, if_, repeat_,
      go_, reset_;
};

} // namespace structo::bootldr

#endif // RELOCO_HAS_COROUTINES
