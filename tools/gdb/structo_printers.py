# SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
#
# SPDX-License-Identifier: BSD-2-Clause

"""GDB pretty printers for the structo header-only library.

This module can be used in three interchangeable ways:

  1. Sourced directly in a running GDB session::

         (gdb) source /path/to/structo/tools/gdb/structo_printers.py

  2. Auto-loaded for a specific binary by placing a ``<binary>-gdb.py``
     next to it (or anywhere on GDB's auto-load path) containing::

         import sys
         sys.path.insert(0, "/path/to/structo/tools/gdb")
         import structo_printers
         structo_printers.register_structo_printers()

  3. Embedded directly into the binary's ``.debug_gdb_scripts`` section via
     ``include/structo/sync/gdb_printers.hpp`` (see that header for
     details); GDB then loads and runs this exact source automatically
     when the binary is loaded, with no external file or path
     configuration required.

In all three cases the printers are registered against ``objfile`` (or the
global printer list, when there is no current object file) the moment this
module is loaded -- see the bottom of the file.

Coverage is currently limited to ``structo::sync`` (see
``docs/gdb-pretty-printers.md`` for the exact list and rationale); it can
grow to other `structo` namespaces later following the same pattern.
"""

try:
    import gdb
    import gdb.printing
except ImportError:  # pragma: no cover - only importable inside GDB.
    gdb = None


_ATOMIC_INT_TYPE_BY_SIZE = {1: "unsigned char", 2: "unsigned short", 4: "unsigned int", 8: "unsigned long"}


def _read_atomic(val):
    """Reads a `std::atomic<T>`'s value directly from memory, bypassing
    library-specific internal field layouts entirely.

    libstdc++ alone uses at least three different internal shapes
    depending on `T` and library version (`_M_i` directly via an
    anonymous `__atomic_base<T>` base, nested under a named `_M_base`/
    `_M_b` member, ...), and libc++ uses yet another (`__a_`/
    `__a_value`) -- enumerating and walking all of them is fragile and
    guaranteed to miss a future layout. Every mainstream implementation
    (libstdc++, libc++, MSVC) instead lays out a lock-free `atomic<T>`
    with no padding before `T`'s own bytes (required for
    `atomic<T>::is_lock_free()` interop with plain `T` in practice, even
    though the standard itself doesn't mandate it), so reinterpreting the
    whole object's storage as a plain unsigned integer of the same width
    -- via a raw pointer cast against its address, never by indexing a
    named field -- reads the same value every one of those layouts would,
    without caring which one is actually in play.
    """
    try:
        size = int(val.type.sizeof)
        addr = val.address
        if addr is None:
            return None
        type_name = _ATOMIC_INT_TYPE_BY_SIZE.get(size)
        if type_name is None:
            return None
        return int(addr.cast(gdb.lookup_type(type_name).pointer()).dereference())
    except gdb.error:
        return None


def _format_owner(owner):
    """Renders a raw `uintptr_t` owner stamp as `0 (unlocked)` or a hex address."""
    if owner == 0:
        return "0 (unlocked)"
    return "0x%x" % owner


class StructoKernelSpinLockPrinter:
    """Pretty printer for `structo::sync::kernel_spin_lock<Traits>`."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        owner = _read_atomic(self.val["owner_"])
        if owner is None:
            return "structo::sync::kernel_spin_lock [owner unreadable]"
        if owner == 0:
            return "structo::sync::kernel_spin_lock [unlocked]"
        return "structo::sync::kernel_spin_lock [locked by owner=%s]" % _format_owner(owner)


class StructoTicketSpinLockPrinter:
    """Pretty printer for `structo::sync::ticket_spin_lock<Traits>`."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        next_ticket = _read_atomic(self.val["next_ticket_"])
        now_serving = _read_atomic(self.val["now_serving_"])
        owner = _read_atomic(self.val["owner_"])
        if next_ticket is None or now_serving is None or owner is None:
            return "structo::sync::ticket_spin_lock [state unreadable]"
        waiting = max(0, next_ticket - now_serving - (1 if owner != 0 else 0))
        state = "unlocked" if owner == 0 else "locked by owner=%s" % _format_owner(owner)
        return "structo::sync::ticket_spin_lock [%s, now_serving=%d, next_ticket=%d, %d waiting]" % (
            state,
            now_serving,
            next_ticket,
            waiting,
        )


class StructoQueueSpinLockPrinter:
    """Pretty printer for `structo::sync::queue_spin_lock<Traits>`."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        tail = _read_atomic(self.val["tail_"])
        owner = _read_atomic(self.val["owner_"])
        if tail is None or owner is None:
            return "structo::sync::queue_spin_lock [state unreadable]"
        if owner == 0:
            return "structo::sync::queue_spin_lock [unlocked]"
        tail_desc = "none" if tail == 0 else "0x%x" % tail
        return "structo::sync::queue_spin_lock [locked by owner=%s, tail=%s]" % (_format_owner(owner), tail_desc)


class StructoQueueSpinLockNodePrinter:
    """Pretty printer for `structo::sync::queue_spin_lock<Traits>::node`."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        waiting = _read_atomic(self.val["waiting_"])
        next_node = _read_atomic(self.val["next_"])
        if waiting is None or next_node is None:
            return "structo::sync::queue_spin_lock::node [state unreadable]"
        status = "waiting" if waiting else ("linked" if next_node != 0 else "idle")
        return "structo::sync::queue_spin_lock::node [%s, next=%s]" % (
            status,
            "none" if next_node == 0 else "0x%x" % next_node,
        )


def _decode_rw_state(state):
    """Shared bit layout for `rw_spin_lock`/`queue_rw_spin_lock`'s `state_` word.

    bit 31 `writer_bit`, bit 30 `writer_waiting_bit`, bits 0-29 `reader_mask`;
    see either header's top-level docs for the full invariants.
    """
    writer_bit = 0x8000_0000
    writer_waiting_bit = 0x4000_0000
    reader_mask = 0x3FFF_FFFF
    return {
        "write_locked": (state & writer_bit) != 0,
        "writer_waiting": (state & writer_waiting_bit) != 0,
        "readers": state & reader_mask,
    }


def _rw_state_to_string(type_name, state, owner):
    decoded = _decode_rw_state(state)
    if decoded["write_locked"]:
        body = "write-locked by owner=%s" % _format_owner(owner)
    elif decoded["readers"] != 0:
        body = "read-locked by %d reader(s)" % decoded["readers"]
    else:
        body = "unlocked"
    if decoded["writer_waiting"]:
        body += ", writer waiting"
    return "%s [%s]" % (type_name, body)


class StructoRwSpinLockPrinter:
    """Pretty printer for `structo::sync::rw_spin_lock<Traits>`."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        state = _read_atomic(self.val["state_"])
        owner = _read_atomic(self.val["owner_"])
        if state is None or owner is None:
            return "structo::sync::rw_spin_lock [state unreadable]"
        return _rw_state_to_string("structo::sync::rw_spin_lock", state, owner)


class StructoQueueRwSpinLockPrinter:
    """Pretty printer for `structo::sync::queue_rw_spin_lock<Traits>`."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        state = _read_atomic(self.val["state_"])
        owner = _read_atomic(self.val["owner_"])
        if state is None or owner is None:
            return "structo::sync::queue_rw_spin_lock [state unreadable]"
        return _rw_state_to_string("structo::sync::queue_rw_spin_lock", state, owner)

    def children(self):
        yield ("writer_queue_", self.val["writer_queue_"])


class StructoBackoffPrinter:
    """Pretty printer for `structo::sync::backoff`."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        initial = int(self.val["initial_spins_"])
        maxs = int(self.val["max_spins_"])
        current = int(self.val["spins_"])
        return "structo::sync::backoff [spins=%d, initial=%d, max=%d]" % (current, initial, maxs)


class StructoSoftlockDetectorPrinter:
    """Pretty printer for `structo::sync::softlock_detector`."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        count = int(self.val["count_"])
        limit = int(self.val["limit_"])
        return "structo::sync::softlock_detector [count=%d / limit=%d]" % (count, limit)


class StructoPreemptionGuardPrinter:
    """Pretty printer for `structo::sync::preemption_guard<Traits>`."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        armed = bool(self.val["m_armed"])
        return "structo::sync::preemption_guard [%s]" % ("armed (preemption disabled)" if armed else "disarmed")


class StructoIrqGuardPrinter:
    """Pretty printer for `structo::sync::irq_guard<Traits>`."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        armed = bool(self.val["m_armed"])
        flags = int(self.val["m_flags"])
        if not armed:
            return "structo::sync::irq_guard [disarmed]"
        return "structo::sync::irq_guard [armed, saved_flags=0x%x]" % flags


class StructoSpinlockEntryGuardPrinter:
    """Pretty printer for `structo::sync::spinlock_entry_guard<Traits>`."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        armed = bool(self.val["m_armed"])
        return "structo::sync::spinlock_entry_guard [%s]" % ("armed (entered)" if armed else "disarmed")


class StructoCorePinGuardPrinter:
    """Pretty printer for `structo::sync::core_pin_guard<Traits>`."""

    def __init__(self, val):
        self.val = val

    def to_string(self):
        armed = bool(self.val["m_armed"])
        if not armed:
            return "structo::sync::core_pin_guard [disarmed]"
        return "structo::sync::core_pin_guard [armed, pinned_cpu=%s]" % str(self.val["m_cpu"])


def _build_pretty_printer():
    pp = gdb.printing.RegexpCollectionPrettyPrinter("structo")
    pp.add_printer("structo::sync::kernel_spin_lock", r"^structo::sync::kernel_spin_lock<.*>$", StructoKernelSpinLockPrinter)
    pp.add_printer("structo::sync::ticket_spin_lock", r"^structo::sync::ticket_spin_lock<.*>$", StructoTicketSpinLockPrinter)
    pp.add_printer("structo::sync::queue_spin_lock", r"^structo::sync::queue_spin_lock<.*>$", StructoQueueSpinLockPrinter)
    pp.add_printer(
        "structo::sync::queue_spin_lock::node",
        r"^structo::sync::queue_spin_lock<.*>::node$",
        StructoQueueSpinLockNodePrinter,
    )
    pp.add_printer("structo::sync::rw_spin_lock", r"^structo::sync::rw_spin_lock<.*>$", StructoRwSpinLockPrinter)
    pp.add_printer(
        "structo::sync::queue_rw_spin_lock", r"^structo::sync::queue_rw_spin_lock<.*>$", StructoQueueRwSpinLockPrinter
    )
    pp.add_printer("structo::sync::backoff", r"^structo::sync::backoff$", StructoBackoffPrinter)
    pp.add_printer("structo::sync::softlock_detector", r"^structo::sync::softlock_detector$", StructoSoftlockDetectorPrinter)
    pp.add_printer("structo::sync::preemption_guard", r"^structo::sync::preemption_guard<.*>$", StructoPreemptionGuardPrinter)
    pp.add_printer("structo::sync::irq_guard", r"^structo::sync::irq_guard<.*>$", StructoIrqGuardPrinter)
    pp.add_printer(
        "structo::sync::spinlock_entry_guard", r"^structo::sync::spinlock_entry_guard<.*>$", StructoSpinlockEntryGuardPrinter
    )
    pp.add_printer("structo::sync::core_pin_guard", r"^structo::sync::core_pin_guard<.*>$", StructoCorePinGuardPrinter)
    return pp


def register_structo_printers(objfile=None):
    """Registers the structo pretty printers with GDB.

    Idempotent: re-registering (e.g. because this module was sourced twice,
    or embedded in several object files) simply replaces the previous
    registration under the same name.
    """
    if gdb is None:
        return

    target = objfile if objfile is not None else gdb
    gdb.printing.register_pretty_printer(target, _build_pretty_printer(), replace=True)


if gdb is not None:
    register_structo_printers(gdb.current_objfile())
