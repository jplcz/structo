// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/bootldr/shell.hpp>
#include <structo/bootldr/shell_commands.hpp>
#include <structo/bootldr/text_editor.hpp>

#include <reloco/array.hpp>
#include <reloco/inline_string.hpp>

#include <reloco/lifetime.hpp>

// Test fixtures index raw buffers freely; bounds are checked by the assertions.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

using namespace structo;
using namespace structo::bootldr;

namespace {

struct fake_uart {
  reloco::array<std::uint8_t, 256> rxbuf{};
  std::size_t rx_head = 0, rx_tail = 0;
  reloco::inline_string<1024> tx;

  bool rx_empty() const { return rx_head == rx_tail; }
  void type(reloco::string_view s) {
    for (std::size_t i = 0; i < s.size(); ++i)
      rxbuf[rx_tail++] = static_cast<std::uint8_t>(s[i]);
  }
};

} // namespace

template <> struct structo::hw::uart_traits<fake_uart> {
  static reloco::result<void> configure(fake_uart &, const hw::uart_config &) noexcept { return {}; }
  static reloco::result<bool> tx_ready(fake_uart &) noexcept { return true; }
  static reloco::result<bool> rx_ready(fake_uart &b) noexcept { return !b.rx_empty(); }
  static reloco::result<void> try_put_byte(fake_uart &b, std::uint8_t v) noexcept {
    (void)b.tx.try_push_back(static_cast<char>(v));
    return {};
  }
  static reloco::result<std::uint8_t> try_get_byte(fake_uart &b) noexcept {
    if (b.rx_empty())
      return reloco::unexpected(reloco::error::try_again);
    return b.rxbuf[b.rx_head++];
  }
};

namespace {

// "sum a b": parses two numbers and prints the total.
reloco::task<void> cmd_sum(command_call &call) noexcept {
  if (call.argc() != 3)
    co_await reloco::unexpected(reloco::error::invalid_argument);
  auto a = parse_number(call.arg(1));
  auto b = parse_number(call.arg(2));
  if (!a || !b)
    co_await reloco::unexpected(reloco::error::invalid_argument);
  (void)call.print("{}\n", *a + *b);
}

// "slow": suspends on the scheduler before printing, showing commands are coroutines.
reloco::task<void> cmd_slow(command_call &call) noexcept {
  for (int i = 0; i < 3; ++i)
    co_await call.sh().sched().yield();
  (void)call.write("done\n");
}

reloco::task<void> cmd_ctx(command_call &call) noexcept {
  ++*static_cast<int *>(call.ctx());
  co_return;
}

class Shell : public ::testing::Test {
protected:
  fake_uart dev;
  scheduler sched;
  shell<64, 8> sh{sched, hw::uart_ref(dev)};
  shell_command sum{"sum", "sum <a> <b>: add two numbers", cmd_sum};
  shell_command slow{"slow", "wait a few rounds", cmd_slow};

  void SetUp() override {
    ASSERT_TRUE(sh.add(sum).has_value());
    ASSERT_TRUE(sh.add(slow).has_value());
    ASSERT_TRUE(sh.start().has_value());
  }

  void run(unsigned rounds = 80) {
    for (unsigned i = 0; i < rounds; ++i)
      sched.run_once();
  }
  bool out_has(reloco::string_view s) const { return dev.tx.contains(s); }
};

} // namespace

TEST_F(Shell, PrintsPromptAndRunsCommand) {
  run(2);
  EXPECT_TRUE(out_has("> "));
  dev.type("sum 0x10 5\r");
  run();
  EXPECT_TRUE(out_has("sum 0x10 5\r\n")); // echo
  EXPECT_TRUE(out_has("21\r\n"));
}

TEST_F(Shell, HelpListsRegisteredCommands) {
  dev.type("help\r");
  run();
  EXPECT_TRUE(out_has("help  help [command]: list the commands or show one"));
  EXPECT_TRUE(out_has("sum  sum <a> <b>: add two numbers"));
  EXPECT_TRUE(out_has("slow  wait a few rounds"));
}

TEST_F(Shell, ReportsUnknownCommandSyntaxErrorAndCommandFailure) {
  dev.type("bogus 1\r");
  run();
  EXPECT_TRUE(out_has("unknown command: bogus\r\n"));
  dev.type("sum \"x\r");
  run();
  EXPECT_TRUE(out_has("syntax error\r\n"));
  dev.type("sum 1\r");
  run();
  EXPECT_TRUE(out_has("error: "));
}

TEST_F(Shell, EditingKeys) {
  dev.type("sumx");
  dev.rxbuf[dev.rx_tail++] = 0x7f; // DEL
  dev.type(" 1 2\r");
  run();
  EXPECT_TRUE(out_has("\b \b"));
  EXPECT_TRUE(out_has("3\r\n"));

  dev.type("garbage");
  dev.rxbuf[dev.rx_tail++] = 0x15; // Ctrl-U
  dev.type("sum 2 2\r");
  run();
  EXPECT_TRUE(out_has("4\r\n"));

  dev.type("sum 9 9");
  dev.rxbuf[dev.rx_tail++] = 0x03; // Ctrl-C drops the line
  dev.type("sum 1 1\r");
  run();
  EXPECT_TRUE(out_has("^C\r\n"));
  EXPECT_TRUE(out_has("2\r\n"));
  EXPECT_FALSE(out_has("18\r\n"));
}

TEST_F(Shell, CrLfIsOneLineEnd) {
  dev.type("sum 1 1\r\nsum 2 2\n");
  run();
  EXPECT_TRUE(out_has("2\r\n"));
  EXPECT_TRUE(out_has("4\r\n"));
  EXPECT_FALSE(out_has("error"));
}

TEST_F(Shell, CommandsAreCoroutinesAndOtherTasksKeepRunning) {
  int ticks = 0;
  struct ticker {
    static reloco::task<void> run(scheduler &s, int &n) {
      for (;;) {
        ++n;
        co_await s.yield();
      }
    }
  };
  ASSERT_TRUE(sched.spawn(ticker::run(sched, ticks)).has_value());
  dev.type("slow\r");
  run();
  EXPECT_TRUE(out_has("done\r\n"));
  EXPECT_GT(ticks, 10);
}

TEST_F(Shell, LineLengthIsBounded) {
  for (int i = 0; i < 100; ++i)
    dev.type("a");
  dev.type("\r");
  run(300);
  EXPECT_TRUE(out_has("\a")); // bell once the 64-char buffer is full
  EXPECT_TRUE(out_has("unknown command: "));
}

TEST_F(Shell, RegistrationRules) {
  int hits = 0;
  shell_command dup{"sum", "dup", cmd_ctx, &hits};
  EXPECT_EQ(sh.add(dup).error(), reloco::error::already_exists);
  EXPECT_EQ(sh.add(sum).error(), reloco::error::invalid_state);
  shell_command empty{"", "x", cmd_ctx};
  EXPECT_EQ(sh.add(empty).error(), reloco::error::invalid_argument);
  shell_command blank{"two words", "x", cmd_ctx};
  EXPECT_EQ(sh.add(blank).error(), reloco::error::invalid_argument);

  shell_command c{"hit", "count calls", cmd_ctx, &hits};
  ASSERT_TRUE(sh.add(c).has_value());
  dev.type("hit\rhit\r");
  run();
  EXPECT_EQ(hits, 2);

  sh.remove(c); // unregistered: unknown again
  dev.type("hit\r");
  run();
  EXPECT_EQ(hits, 2);
  EXPECT_TRUE(out_has("unknown command: hit"));
}

TEST_F(Shell, CommandUnregistersWhenDestroyed) {
  {
    shell_command tmp{"tmp", "temporary", cmd_ctx};
    ASSERT_TRUE(sh.add(tmp).has_value());
    EXPECT_NE(sh.find("tmp"), nullptr);
  }
  EXPECT_EQ(sh.find("tmp"), nullptr);
}

TEST_F(Shell, ExecuteRunsALineWithoutTheConsole) {
  sh.stop(); // no interactive loop
  const reloco::string_view line = "sum 40 2";
  auto t = shell<64, 8>::execute(sh, line);
  ASSERT_TRUE(sched.spawn(std::move(t), spawn_mode::joinable).has_value());
  run(5);
  EXPECT_TRUE(out_has("42\r\n"));
}

namespace {

int g_resets = 0;
std::uint64_t g_go = 0;

class ShellCmds : public ::testing::Test {
protected:
  fake_uart dev;
  scheduler sched;
  shell<96, 8> sh{sched, hw::uart_ref(dev)};
  generic_commands_hooks hooks = make_hooks();
  generic_commands cmds{sh, hooks};
  alignas(8) std::uint8_t mem[64]{};
  alignas(8) std::uint8_t mem2[64]{};
  std::uint64_t now = 0;

  static generic_commands_hooks make_hooks() {
    generic_commands_hooks h;
    h.reset = [](void *) noexcept { ++g_resets; };
    h.go = [](void *, std::uint64_t a) noexcept { g_go = a; };
    return h;
  }

  void SetUp() override {
    g_resets = 0;
    g_go = 0;
    sched.set_clock([](void *c) noexcept { return *static_cast<std::uint64_t *>(c); }, &now);
    ASSERT_TRUE(cmds.add_all().has_value());
    ASSERT_TRUE(sh.start().has_value());
  }

  static std::uint64_t addr(const void *p) { return reinterpret_cast<std::uintptr_t>(p); }

  // Types "<fmt>\r" with the arguments substituted and runs the scheduler.
  template <typename... A> void cmd(microfmt::string_view fmt, const A &...a) {
    const auto line = microfmt::format<128>(fmt, a...);
    dev.type(line.view());
    dev.type("\r");
    for (int i = 0; i < 600; ++i) {
      now += 1;
      sched.run_once();
    }
  }
  bool out_has(reloco::string_view s) const { return dev.tx.contains(s); }
};

} // namespace

TEST_F(ShellCmds, EchoUptimeSleep) {
  cmd("echo hello \"big world\"");
  EXPECT_TRUE(out_has("hello big world\r\n"));
  now = 5000;
  cmd("uptime");
  EXPECT_TRUE(out_has(" ms\r\n"));
  cmd("sleep 20");
  EXPECT_FALSE(out_has("error"));
}

TEST_F(ShellCmds, MemoryWriteDumpFillCopy) {
  cmd("mw {} 0x04030201", addr(mem));
  EXPECT_EQ(mem[0], 1);
  EXPECT_EQ(mem[3], 4);
  cmd("mw {} 0xaa 1", addr(mem + 8));
  EXPECT_EQ(mem[8], 0xaa);
  cmd("mw {} 0x1122334455667788 8", addr(mem + 16));
  EXPECT_EQ(mem[16], 0x88);
  EXPECT_EQ(mem[23], 0x11);

  cmd("fill {} 8 0x5a", addr(mem + 32));
  for (int i = 32; i < 40; ++i)
    EXPECT_EQ(mem[i], 0x5a);
  EXPECT_EQ(mem[40], 0);

  cmd("md {} 16", addr(mem));
  EXPECT_TRUE(out_has("01 02 03 04"));

  cmd("cp {} {} 24", addr(mem2), addr(mem));
  for (int i = 0; i < 24; ++i)
    EXPECT_EQ(mem2[i], mem[i]);
  // Overlapping copy towards higher addresses must behave like memmove.
  cmd("cp {} {} 8", addr(mem + 2), addr(mem));
  EXPECT_EQ(mem[2], 1);
  EXPECT_EQ(mem[5], 4);
}

TEST_F(ShellCmds, CompareReportsDifference) {
  cmd("cmp {} {} 32", addr(mem), addr(mem2));
  EXPECT_TRUE(out_has("equal (32 bytes)"));
  mem2[9] = 0x77;
  cmd("cmp {} {} 32", addr(mem), addr(mem2));
  EXPECT_TRUE(out_has("differ at offset 0x9"));
  EXPECT_TRUE(out_has("error: invalid_state"));
}

TEST_F(ShellCmds, UsageErrorsAndErrorNames) {
  cmd("md");
  EXPECT_TRUE(out_has("usage: md <addr>"));
  EXPECT_TRUE(out_has("error: invalid_argument"));
  cmd("mw {} 1 3", addr(mem));
  EXPECT_TRUE(out_has("usage: mw"));
  cmd("fill {} 4 300", addr(mem));
  EXPECT_TRUE(out_has("usage: fill"));
}

TEST_F(ShellCmds, HooksAndHelp) {
  cmd("go 0x80000");
  EXPECT_EQ(g_go, 0x80000u);
  cmd("reset");
  EXPECT_EQ(g_resets, 1);
  cmd("help md");
  EXPECT_TRUE(out_has("md <addr> [len=64]: hex dump memory"));
  cmd("help nothere");
  EXPECT_TRUE(out_has("error: not_found"));
}

TEST(ShellCmdsRegistration, HookCommandsAreOptional) {
  fake_uart dev;
  scheduler sched;
  shell<> sh{sched, hw::uart_ref(dev)};
  generic_commands cmds{sh};
  ASSERT_TRUE(cmds.add_all().has_value());
  EXPECT_NE(sh.find("md"), nullptr);
  EXPECT_EQ(sh.find("go"), nullptr);
  EXPECT_EQ(sh.find("reset"), nullptr);
  EXPECT_EQ(cmds.add_all().error(), reloco::error::invalid_state);
}

namespace {

struct edit_result {
  bool done = false;
  bool saved = false;
  reloco::error err{};
};

reloco::task<void> run_editor(scheduler &s, fake_uart &dev, reloco::string &buf, edit_result &out) {
  auto r = co_await edit_text(s, hw::uart_ref(dev), buf);
  out.done = true;
  if (r)
    out.saved = *r;
  else
    out.err = r.error();
}

} // namespace

TEST(EditText, SaveKeepsEditsAndCancelRestores) {
  fake_uart dev;
  scheduler sched;
  reloco::string buf(sched.allocator());
  ASSERT_TRUE(buf.try_assign("boot\n").has_value());
  edit_result res;
  ASSERT_TRUE(sched.spawn(run_editor(sched, dev, buf, res)).has_value());
  for (int i = 0; i < 3; ++i)
    sched.run_once();
  dev.type("\x1b[Bgo\x13"); // down, type "go", Ctrl-S
  for (int i = 0; i < 20; ++i)
    sched.run_once();
  ASSERT_TRUE(res.done);
  EXPECT_TRUE(res.saved);
  EXPECT_EQ(buf.view(), "boot\ngo");
  EXPECT_TRUE(dev.tx.contains("Ln 1"));

  // Cancel: the first Ctrl-X asks for confirmation, the second discards.
  fake_uart dev2;
  edit_result res2;
  ASSERT_TRUE(sched.spawn(run_editor(sched, dev2, buf, res2)).has_value());
  for (int i = 0; i < 3; ++i)
    sched.run_once();
  dev2.type("zzz\x18");
  for (int i = 0; i < 20; ++i)
    sched.run_once();
  EXPECT_FALSE(res2.done);
  EXPECT_TRUE(dev2.tx.contains("unsaved changes"));
  dev2.type("\x18");
  for (int i = 0; i < 20; ++i)
    sched.run_once();
  ASSERT_TRUE(res2.done);
  EXPECT_FALSE(res2.saved);
  EXPECT_EQ(buf.view(), "boot\ngo");
}

RELOCO_END_UNSAFE_BUFFER_USAGE