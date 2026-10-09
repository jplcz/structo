<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# Guide: a command shell for the bootloader

C++20 only. `bootldr::shell` (`bootldr/shell.hpp`) is a scheduler task that reads
lines from a UART, splits them (`bootldr/cmdline.hpp`) and runs registered
commands. Commands are coroutines, so a long command (a TFTP download) does not
stop the netstack or other tasks.

```cpp
// A command handler: a noexcept coroutine taking the command_call.
// call.arg(0) is the command name, call.arg(1..) are its arguments, call.ctx() is the pointer given at registration.
reloco::task<void> cmd_tftp(structo::bootldr::command_call &call) noexcept {
  // usage: tftp <file> <load address>
  if (call.argc() != 3) {
    (void)call.print("usage: tftp <file> <addr>\n");
    co_await reloco::unexpected(reloco::error::invalid_argument); // the shell then prints "error: N"
  }
  auto addr = structo::bootldr::parse_number(call.arg(2)); // accepts 4096, 0x1000, 0b1, 017
  if (!addr)
    co_await reloco::unexpected(addr.error());

  auto &tftp = *static_cast<structo::bootldr::tftp_client *>(call.ctx()); // the ctx given to shell_command below
  auto dst = reloco::span<std::uint8_t>(reinterpret_cast<std::uint8_t *>(*addr), 8 * 1024 * 1024);
  // co_await the download: the shell task is suspended, everything else keeps running.
  auto n = co_await co_await tftp.get(server, call.arg(1), dst);
  (void)call.print("{} bytes loaded at {:#x}\n", n, *addr); // print() formats with microfmt
}

structo::bootldr::shell sh{sched, uart};      // lines up to 256 chars, heap-backed arguments; prompt "> "
structo::bootldr::shell_command tftp_cmd{    // owned by us; must outlive its registration
    "tftp",                                  // name typed by the user (no blanks)
    "tftp <file> <addr>: download a file",   // one line shown by "help"
    cmd_tftp,                                // handler
    &tftp};                                  // ctx() for the handler
(void)sh.add(tftp_cmd);                      // fails: already_exists / invalid_argument / invalid_state
(void)sh.start();                            // spawn the shell task; then run the scheduler
```

- Line editing: Backspace/DEL, Ctrl-U (erase line), Ctrl-C (drop line).
- Quoting: `"a b"`, `'a b'`, `a\ b`; `#` starts a comment.
- The evaluation state is a separate class, `bootldr::shell_context` (`bootldr/shell_context.hpp`, sans-IO,
  C++17; `sh.context()`): heap-backed variables, shell functions and native expression functions.
- Variables: `set NAME VALUE` (`set NAME` shows it, `set` lists all, `set -g` writes the global scope),
  `unset NAME`, or from C++ `sh.context().set("name", "value")` / `get` / `unset`. `$NAME` / `${NAME}` expand
  before the line is split (not inside `'...'` or after `\`); unknown names expand to nothing and a
  value is always exactly one argument. Names match `[A-Za-z_][A-Za-z0-9_]*`.
- `$((expr))` expands to the decimal value of an integer expression: `+ - * / % << >> & | ^ ~`,
  parentheses, literals as in `parse_number` (`0x1000`, `0b101`), variable names
  (`set base 0x80000000`, then `md $((base + 0x40)) 16`) and function calls. Arithmetic is unsigned 64-bit; an
  unset variable counts as 0; a syntax error or division by zero fails the line (`error: invalid_argument`).
  Built-in functions: `min(a, b)`, `max(a, b)`, `align_up(x, a)`, `align_down(x, a)`; add your own with
  `sh.context().add_function("crc", fn, user)` (`fn(user, args)` returns the value).
  `bootldr::evaluate_expression(text, lookup, call)` in `cmdline.hpp` is the sans-IO evaluator.
- Shell functions: `function NAME 'stmt1 $1; stmt2'` (single quotes, so `$1` is expanded at call time, not
  when defining), `function` lists, `function NAME` shows the body, `unfunction NAME` removes it. `NAME a b` runs
  the statements (`;` separated; the first failure stops it) with `$0`=name, `$1`.. and `$#`. Every call has its
  **own dynamic scope**: variables it sets are local (they shadow the caller's and vanish on return; `unset` and
  `set` touch only that scope), but lookups still see the callers' variables, innermost first. `set -g` writes the
  global scope. Calls nest up to 16 deep; a function cannot take the name of a command.
- `help` lists all commands. Errors from a handler print `error: <code>`.
- A line may hold several statements separated by `;` (outside quotes). Scripts: `shell::execute_script(sh, text)`
  (or `shell_base::run_script(text)` inside a command) runs a multi-line buffer, one line per `\n`, in the current
  variable scope and stops at the first failing statement; `text` must outlive the task.
- `shell::execute(sh, "tftp kernel.bin 0x80000")` runs a line
  without the console (boot scripts); it fails with `busy` while a command runs.

## Generic commands

`bootldr/shell_commands.hpp` provides a ready-made set (`help` is already built
into every shell, `help <command>` shows one usage line):

```cpp
// Platform actions; commands whose hook is null are simply not registered.
structo::bootldr::generic_commands_hooks hooks;
hooks.reset = [](void *) noexcept { board_reset(); };                  // enables "reset"
hooks.go = [](void *, std::uint64_t a) noexcept { jump_to(a); };       // enables "go <addr>"
hooks.read_memory = probe_read; // optional fault-safe reader for md/cmp (microfmt::memory_reader_fn_t)

structo::bootldr::generic_commands cmds{sh, hooks}; // owns the command objects; keep alive with `sh`
(void)cmds.add_all();                               // echo sleep uptime md mw cp cmp fill [go] [reset]
```

| command | description |
|---------|-------------|
| `echo [words...]`, `sleep <ms>`, `uptime` | text, timer (needs `scheduler::set_clock`) |
| `md <addr> [len]` | hex dump (microfmt `hexdump_checked`) |
| `mw <addr> <value> [width]` | write 1/2/4/8 bytes |
| `cp <dst> <src> <len>`, `fill <addr> <len> <byte>` | overlap-safe copy, fill; yield every 4 KiB |
| `crc32 <addr> <len> [var]` | CRC-32 (IEEE, as zlib) of memory; prints it, optionally stores it in `var` |
| `find <addr> <len> <value> [width]` | prints each address (stepping by `width` 1/2/4/8, default 1) holding `value` |
| `mtest <addr> <len>` | destructive word-aligned RAM test (0, ones, 0x55/0xAA, address-as-data); `io_error` on mismatch |
| `env [name]` | lists the visible variables, or prints one |
| `if <a> <op> <b> 'then' ['else']` | runs `then` or `else` as a script; `op` is `== != < <= > >=` (numbers unsigned, else `==`/`!=` compare text) |
| `repeat <n> 'script'` | runs `script` `n` times with `i` = 0..n-1 set in the current scope |
| `source <addr> [len]` | runs the script stored in memory (copied first); without `len` it ends at the first NUL |
| `cmp <a> <b> <len>` | prints a microfmt `mem_diff` at the first difference and fails |

`add_all()` also registers expression functions for `$((...))`: `peek8/16/32/64(addr)` (native-endian
read through the `read_memory` hook; `out_of_bounds` on a fault), `bit(n)` and `mask(n)`; e.g.
`set magic $((peek32(0x1000)))`.

Failing commands print `error: <name>` (microfmt formats `reloco::error` by name).

## Guide: an `xmodem` command and sending files from a PC

The shell and XMODEM ([`xmodem.md`](xmodem.md)) share one UART. This works because the shell has already
consumed the typed line before the handler runs, and the handler then owns the UART until it returns.

### 1. The command

```cpp
#include <structo/bootldr/shell.hpp>
#include <structo/bootldr/xmodem.hpp>

// State the command needs; passed to the handler through call.ctx().
struct xmodem_ctx {
  structo::hw::uart_ref uart;   // the same UART the shell console uses
  std::uint8_t *load_base;      // where images may be loaded (RAM region reserved for them)
  std::size_t load_capacity;    // size of that region in bytes
};

// usage: xmodem <load address> [max length]
// The handler is a noexcept coroutine; the transfer itself is a blocking call, so no other
// scheduler task runs until the transfer ends (the UART is polled).
reloco::task<void> cmd_xmodem(structo::bootldr::command_call &call) noexcept {
  auto &ctx = *static_cast<xmodem_ctx *>(call.ctx());   // the pointer given at registration
  if (call.argc() < 2 || call.argc() > 3) {
    (void)call.print("usage: xmodem <addr> [max length]\n");
    co_await reloco::unexpected(reloco::error::invalid_argument);
  }
  auto addr = structo::bootldr::parse_number(call.arg(1)); // 4096, 0x1000, 0b1, 017
  if (!addr)
    co_await reloco::unexpected(addr.error());
  std::uint64_t cap = ctx.load_capacity;                   // optional 2nd argument lowers the limit
  if (call.argc() == 3) {
    auto len = structo::bootldr::parse_number(call.arg(2));
    if (!len)
      co_await reloco::unexpected(len.error());
    cap = *len < cap ? *len : cap;
  }

  auto *dst = reinterpret_cast<std::uint8_t *>(*addr);
  std::size_t used = 0;
  // Called once per in-order 128/1024-byte packet. Returning false aborts the transfer (CAN sent).
  auto sink = [&](reloco::span<const std::uint8_t> payload) {
    if (used + payload.size() > cap)
      return false;                                        // image larger than the buffer
    std::memcpy(dst + used, payload.data(), payload.size());
    used += payload.size();
    return true;
  };

  (void)call.print("waiting for XMODEM sender (Ctrl-C x2 to abort)...\n");
  structo::bootldr::xmodem_config cfg;                     // defaults: CRC-16, 10 retries, 1K packets
  auto r = structo::bootldr::receive(
      ctx.uart, reloco::function_ref<bool(reloco::span<const std::uint8_t>)>(sink), cfg);
  if (!r)
    co_await reloco::unexpected(r.error());                // the shell prints "error: <name>"

  // XMODEM has no length field: the last packet is padded with 0x1A (SUB), so `used` is
  // rounded up to 128 bytes. Keep the exact size yourself if the format needs it.
  (void)call.print("received {} bytes at {:#x}\n", used, *addr);
  // Publish the size as $filesize for scripts; format() renders the number as decimal text.
  auto size = microfmt::format<24>("{}", used);
  (void)call.sh().context().set("filesize", size.view());
}
```

Register it like any command:

```cpp
static xmodem_ctx xctx{uart, reinterpret_cast<std::uint8_t *>(0x80000000), 64u << 20};
static structo::bootldr::shell_command xmodem_cmd{
    "xmodem",                                    // name typed by the user
    "xmodem <addr> [max]: receive a file over XMODEM into memory",
    cmd_xmodem,                                  // handler above
    &xctx};                                      // call.ctx()
(void)sh.add(xmodem_cmd);
```

### 2. Sending a file from the PC

Run `xmodem 0x80000000` on the target. The target then sends `C` every few seconds; start the sender within that time. The terminal program must be **disconnected from the
keyboard** while sending (it must stop echoing and pass bytes through), so use its built-in transfer:

| Tool | How |
|------|-----|
| minicom | `Ctrl-A S`, choose `xmodem`, pick the file (minicom runs `sx`; edit the protocol in `Ctrl-A O` → *File transfer protocols* to add `-k`) |
| picocom | `picocom -b 115200 --send-cmd "sx -k" /dev/ttyUSB0`, then type `Ctrl-A Ctrl-S` and the file name |
| lrzsz | `sx -k --xmodem kernel.bin < /dev/ttyUSB0 > /dev/ttyUSB0` (after `stty -F /dev/ttyUSB0 115200 raw -echo`, with the terminal program closed) |
| screen | `Ctrl-A :exec !! sx -k kernel.bin` |
| Windows | Tera Term: *File → Transfer → XMODEM → Send* (choose *CRC* or *1K*); PuTTY has no XMODEM, use Tera Term |

`sx -k` sends 1024-byte packets (XMODEM-1K); without `-k` it sends 128-byte packets. Both work with the
default configuration. The receiver asks for CRC-16 first and falls back to the 8-bit checksum after a few
unanswered requests, so very old senders work too.

### 3. Using the result

```
> xmodem 0x80000000
waiting for XMODEM sender (Ctrl-C x2 to abort)...
received 4096 bytes at 0x80000000
> crc32 0x80000000 4096                  # compare with `crc32 kernel.bin` / `cksum` on the PC
> go 0x80000000
```

Verify the transfer with `crc32` on both sides (the padding bytes are included in the target-side
length, so compare with the file size rounded up to 128, or pass the real size if you know it).
`xmodem` also composes with scripts, e.g. `function load 'xmodem $1; crc32 $1 $filesize'`.

If a transfer fails with `timed_out` or `io_error`, raise the UART get-byte spin budget for your CPU, check
that nothing else (a logger, another task) writes to the same UART during the transfer, and lower the
baud rate. A PC-side `sx` that reports "Retry 0: NAK on sector" repeatedly means line noise or a wrong
baud rate.

## Text editor

```cpp
// A shell command handler that edits a boot script stored in a heap string.
// `script` is the dynamic buffer being edited (owned by the caller).
// `sched` provides yielding so other tasks keep running; `uart` is the console.
reloco::task<void> edit_cmd(scheduler &sched, hw::uart_ref uart, reloco::string &script) {
  // Returns true when saved (Ctrl-S), false when discarded (Ctrl-X, confirmed
  // if modified); on false `script` keeps its original contents.
  bool saved = co_await co_await edit_text(sched, uart, script);
  (void)saved;
}
```

Keys: arrows, Home/End, PgUp/PgDn, Delete, Backspace, Tab (spaces), Ctrl-K kill line,
Ctrl-S save, Ctrl-X cancel. `text_editor` is also usable sans-IO via `handle(key_event)`.

Try it in a terminal: `build/examples/text_editor_linux_demo [FILE]` (see
`examples/text_editor_linux_demo.cpp`; the file is only read, the result is printed on exit).
