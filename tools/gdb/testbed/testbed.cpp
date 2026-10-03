// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

// Testbed for structo's GDB pretty printers. See tools/gdb/testbed/README.md
// for how to build/run this and how to add a case for a new printer.
//
// Every variable below carries a GDB_CHECK marker comment giving a variable
// name and an expected output substring, separated by "=>". `run.sh`
// extracts those comments, breaks at the GDB_BREAK marker near the end of
// main(), runs a print command for each variable, and checks that the
// printed output contains the expected substring.

#include <structo/sync/backoff.hpp>
#include <structo/sync/core_pin_guard.hpp>
#include <structo/sync/irq_guard.hpp>
#include <structo/sync/kernel_spin_lock.hpp>
#include <structo/sync/preemption_guard.hpp>
#include <structo/sync/queue_rw_spin_lock.hpp>
#include <structo/sync/queue_spin_lock.hpp>
#include <structo/sync/rw_spin_lock.hpp>
#include <structo/sync/softlock_detector.hpp>
#include <structo/sync/spinlock_entry_guard.hpp>
#include <structo/sync/ticket_spin_lock.hpp>

#include <cstddef>
#include <cstdint>

namespace {

// A fake "current thread" stamp shared by every lock-traits type below;
// its address (never 0) is all any of these locks need from `owner_type`.
struct fake_thread {
  int tag;
};
fake_thread g_thread_a{1};

struct test_lock_traits {
  using owner_type = std::uintptr_t;

  static owner_type current_owner() noexcept { return reinterpret_cast<std::uintptr_t>(&g_thread_a); }
};

struct test_preempt_traits {
  static void disable_preemption() noexcept {}
  static void enable_preemption() noexcept {}
};

struct test_irq_traits {
  using flags_type = std::uint32_t;

  static flags_type hw_save_irqs() noexcept { return 0x1234; }
  static void hw_restore_irqs(flags_type) noexcept {}
};

struct test_spinlock_entry_traits {
  static void spinlock_enter() noexcept {}
  static void spinlock_exit() noexcept {}
};

struct test_pin_traits {
  using cpu_id_type = std::size_t;

  static cpu_id_type pin() noexcept { return 3; }
  static void unpin() noexcept {}
};

} // namespace

int main() {
  // -- kernel_spin_lock ------------------------------------------------------
  structo::sync::kernel_spin_lock<test_lock_traits> kernel_lock_unlocked;
  // GDB_CHECK: kernel_lock_unlocked => structo::sync::kernel_spin_lock [unlocked]
  structo::sync::kernel_spin_lock<test_lock_traits> kernel_lock_locked;
  kernel_lock_locked.lock();
  // GDB_CHECK: kernel_lock_locked => structo::sync::kernel_spin_lock [locked by owner=

  // -- ticket_spin_lock --------------------------------------------------------
  structo::sync::ticket_spin_lock<test_lock_traits> ticket_lock;
  ticket_lock.lock();
  // GDB_CHECK: ticket_lock => structo::sync::ticket_spin_lock [locked by owner=

  // -- queue_spin_lock -----------------------------------------------------
  structo::sync::queue_spin_lock<test_lock_traits> queue_lock;
  structo::sync::queue_spin_lock<test_lock_traits>::node queue_node;
  queue_lock.lock(queue_node);
  // GDB_CHECK: queue_lock => structo::sync::queue_spin_lock [locked by owner=
  // GDB_CHECK: queue_node => structo::sync::queue_spin_lock::node [

  // -- rw_spin_lock ----------------------------------------------------------
  structo::sync::rw_spin_lock<test_lock_traits> rw_lock_read;
  rw_lock_read.read_lock();
  rw_lock_read.read_lock();
  // GDB_CHECK: rw_lock_read => structo::sync::rw_spin_lock [read-locked by 2 reader(s)]
  structo::sync::rw_spin_lock<test_lock_traits> rw_lock_write;
  rw_lock_write.write_lock();
  // GDB_CHECK: rw_lock_write => structo::sync::rw_spin_lock [write-locked by owner=

  // -- queue_rw_spin_lock ------------------------------------------------
  structo::sync::queue_rw_spin_lock<test_lock_traits> qrw_lock_read;
  qrw_lock_read.read_lock();
  // GDB_CHECK: qrw_lock_read => structo::sync::queue_rw_spin_lock [read-locked by 1 reader(s)]

  // -- backoff -----------------------------------------------------------------
  structo::sync::backoff backoff_default;
  // GDB_CHECK: backoff_default => structo::sync::backoff [spins=1

  // -- softlock_detector -------------------------------------------------
  structo::sync::softlock_detector softlock(1000);
  softlock.tick();
  softlock.tick();
  // GDB_CHECK: softlock => structo::sync::softlock_detector [count=2 / limit=1000]

  // -- preemption_guard ----------------------------------------------------
  structo::sync::preemption_guard<test_preempt_traits> preempt_guard;
  // GDB_CHECK: preempt_guard => structo::sync::preemption_guard [armed

  // -- irq_guard -------------------------------------------------------------
  structo::sync::irq_guard<test_irq_traits> irq_guard_armed;
  // GDB_CHECK: irq_guard_armed => structo::sync::irq_guard [armed, saved_flags=0x1234]

  // -- spinlock_entry_guard ------------------------------------------------
  structo::sync::spinlock_entry_guard<test_spinlock_entry_traits> entry_guard;
  // GDB_CHECK: entry_guard => structo::sync::spinlock_entry_guard [armed

  // -- core_pin_guard ------------------------------------------------------
  structo::sync::core_pin_guard<test_pin_traits> pin_guard;
  // GDB_CHECK: pin_guard => structo::sync::core_pin_guard [armed, pinned_cpu=3]

  // ADD_NEW_CASE_HERE: declare your new type's test variable above this
  // line, with its own `// GDB_CHECK:` comment, before the GDB_BREAK marker.

  int gdb_break_here = 0; // GDB_BREAK
  (void)gdb_break_here;
  return 0;
}
