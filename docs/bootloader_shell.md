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
