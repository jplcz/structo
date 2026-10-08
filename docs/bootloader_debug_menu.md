<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# Bootloader debug menu

`structo/bootldr/debug_menu.hpp` (C++20) is a full-screen, keyboard-driven menu over a `hw::uart_ref`
(VT100/ANSI terminal) that runs as a `bootldr::scheduler` task. Items are coroutines, so they can
await timers, downloads or the editor while the rest of the bootloader keeps running.

```cpp
// A handler: noexcept coroutine receiving the menu_context.
// ctx.ctx() is the pointer given to the menu_item; ctx.print() formats with microfmt.
static reloco::task<void> item_regs(structo::bootldr::menu_context &ctx) noexcept {
  (void)ctx.print("SCTLR_EL1 = {:#x}\n", read_sctlr());
  co_return; // an error here (co_await reloco::unexpected(e)) is printed by the menu
}

// scheduler, console, title (the title must outlive the menu).
structo::bootldr::debug_menu menu{sched, uart, "Bootloader debug"};
// label, one-line help, handler; owned by you, unlinks itself when destroyed.
structo::bootldr::menu_item regs{"Registers", "dump CPU registers", item_regs};
(void)menu.add(regs);

// Enter only if SPACE is pressed within 2 s of boot (needs sched.set_clock()); key 0 = any key.
auto pressed = co_await structo::bootldr::wait_for_key(sched, uart, 2000, ' ');
if (pressed && *pressed)
  (void)co_await structo::bootldr::debug_menu::run(menu); // or menu.start() to run it as a task
```

Keys: Up/Down/Home/End select, Enter runs the selected item, hotkeys `1`-`9` then `a`-`z` (without `q`)
run an item directly, `q` or Ctrl-C leaves. After an item returns the menu prints any error, waits for a
key and redraws. A submenu is an item that awaits `debug_menu::run()` of another menu.

See `examples/debug_menu_linux_demo.cpp` and `tests/test_debug_menu.cpp`.
