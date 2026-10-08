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

structo::bootldr::shell<> sh{sched, uart};   // 128-char lines, 16 arguments; prompt "> "
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
- `help` lists all commands. Errors from a handler print `error: <code>`.
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
| `cmp <a> <b> <len>` | prints a microfmt `mem_diff` at the first difference and fails |

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
