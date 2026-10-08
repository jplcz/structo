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
 * | `go <addr>` | jump to an address (only if a `go` hook is provided) |
 * | `reset` | restart the board (only if a `reset` hook is provided) |
 *
 * `help` is not part of this set: every `shell` has it built in.
 * Addresses and numbers accept decimal, `0x` hex, `0b` binary and octal (`parse_number`).
 * Memory commands touch the given addresses directly (volatile byte/word accesses), so they are
 * only as safe as the addresses you type; long copies/fills yield to other tasks every 4 KiB.
 *
 * ```cpp
 * structo::bootldr::shell<> sh{sched, uart};
 *
 * // Platform hooks are optional; a command whose hook is null is not registered.
 * structo::bootldr::generic_commands_hooks hooks;
 * hooks.reset = [](void *) noexcept { board_reset(); };                    // "reset"
 * hooks.go = [](void *, std::uint64_t addr) noexcept { jump_to(addr); };   // "go <addr>"
 * hooks.read_memory = probe_read;   // optional fault-safe reader for md/cmp: size_t(void *, uintptr_t, uint8_t *, size_t)
 * hooks.ctx = nullptr;                                                     // passed back to the hooks and the reader
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

#include <reloco/error.hpp>

#include <cstddef>
#include <cstdint>

#if RELOCO_HAS_COROUTINES

namespace structo::bootldr {

/** @brief Platform actions behind `reset` and `go`; a null hook leaves its command unregistered. */
struct generic_commands_hooks {
  void (*reset)(void *ctx) noexcept = nullptr;                      ///< restart the board
  void (*go)(void *ctx, std::uint64_t address) noexcept = nullptr;  ///< transfer control to `address`
  /// Fault-safe memory reader used by `md`/`cmp` (`microfmt::memory_reader_fn_t`: returns the bytes read, 0 on a
  /// fault); null = plain volatile reads. Receives `ctx`.
  microfmt::memory_reader_fn_t read_memory = nullptr;
  void *ctx = nullptr;                                              ///< first argument of the hooks and the reader
};

/** @brief The generic command set; owns its `shell_command` objects (unregistered on destruction). */
class generic_commands {
public:
  explicit generic_commands(shell_base &sh, const generic_commands_hooks &hooks = {}) noexcept
      : sh_(&sh), hooks_(hooks),
        echo_("echo", "echo [words...]: print the arguments", &echo_cmd),
        sleep_("sleep", "sleep <ms>: wait", &sleep_cmd),
        uptime_("uptime", "uptime: milliseconds on the scheduler clock", &uptime_cmd),
        md_("md", "md <addr> [len=64]: hex dump memory", &md_cmd, this),
        mw_("mw", "mw <addr> <value> [width=4]: write memory (width 1/2/4/8)", &mw_cmd),
        cp_("cp", "cp <dst> <src> <len>: copy memory", &cp_cmd),
        cmp_("cmp", "cmp <a> <b> <len>: compare memory", &cmp_cmd, this),
        fill_("fill", "fill <addr> <len> <byte>: fill memory", &fill_cmd),
        go_("go", "go <addr>: jump to an address", &go_cmd, this),
        reset_("reset", "reset: restart the board", &reset_cmd, this) {}

  /** @brief Registers every command (hook-backed ones only if their hook is set). Stops at the first failure. */
  [[nodiscard]] reloco::result<void> add_all() noexcept {
    shell_command *all[] = {&echo_, &sleep_, &uptime_, &md_, &mw_, &cp_, &cmp_, &fill_};
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

  // Prints "usage: <help>" for the running command and returns the error to fail with.
  static reloco::error usage(command_call &call) noexcept {
    if (shell_command *c = call.sh().find(call.arg(0)))
      (void)call.print("usage: {}\n", c->help());
    return reloco::error::invalid_argument;
  }

  // Default reader: plain volatile byte reads.
  static std::size_t volatile_read(void *, std::uintptr_t src, std::uint8_t *dst, std::size_t n) noexcept {
    const volatile std::uint8_t *p = reinterpret_cast<const volatile std::uint8_t *>(src);
    for (std::size_t i = 0; i < n; ++i)
      dst[i] = p[i];
    return n;
  }

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
      (void)call.print<160>("{}\n", microfmt::hexdump_checked(static_cast<std::uintptr_t>(*addr + off), n,
                                                                self.reader(), self.hooks_.ctx));
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
        (void)call.print<512>("{}", microfmt::mem_diff(microfmt::span<const std::uint8_t>(wa, got),
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

  static reloco::task<void> go_cmd(command_call &call) noexcept {
    auto &self = *static_cast<generic_commands *>(call.ctx());
    auto addr = call.argc() == 2 ? parse_number(call.arg(1)) : reloco::result<std::uint64_t>(reloco::unexpected(reloco::error::invalid_argument));
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
  shell_command echo_, sleep_, uptime_, md_, mw_, cp_, cmp_, fill_, go_, reset_;
};

} // namespace structo::bootldr

#endif // RELOCO_HAS_COROUTINES
