<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::arch::fixed_asid_allocator<Tag, Capacity, SeqGroupSize>` / `fixed_asid<Tag>`

`include/structo/arch/fixed_asid_allocator.hpp`

A no-allocation, rollover-free ASID allocator for systems where the
number of tasks/VMs that will ever be simultaneously live is a
compile-time-known, power-of-two `Capacity` that never exceeds the
(also power-of-two) hardware ASID space -- every task is handed one
permanent ASID *slot* for its whole lifetime, so none of
[`asid_allocator<Tag, MaxActive>`](asid_allocator.md)'s generation
*rollover* machinery (the TLB-exhaustion escape hatch) is needed at
all. `fixed_asid<Tag>` is the opaque handle it hands back.

## Why this exists

`asid_allocator<Tag, MaxActive>` earns its generation-counted bitmap
complexity by handling the hard case: a hardware ASID space (e.g. an
8-bit PCID) far too small to assign one permanently to every task that
ever existed, so IDs must be recycled, under hardware still actively
using some of them, via a software generation counter and a global TLB
flush on rollover. Plenty of real systems never hit that hard case -- a
small embedded RTOS with a fixed, compile-time-bounded task count that
is itself smaller than the hardware ASID space (e.g. <= 256 tasks
against a 16-bit ASID register) never needs to reclaim an ASID out from
under a still-resident task, because there are always enough raw ASID
values to hand every concurrently-live task its own, forever. For that
system, `asid_allocator`'s rollover scan and per-slot `active[]`
residency tracking are pure overhead paid for a case (hardware ASID
exhaustion) that can provably never occur.
`fixed_asid_allocator<Tag, Capacity, SeqGroupSize>` is the allocator for
that system: a plain fixed-capacity free-slot pool, no rollover, no
`flush_required` flag -- `Capacity` is simply how many tasks/VMs may
hold a live ASID at once, and that many bits (not `2^asid_bits` bits) is
all the bitmap ever needs.

## Fixed, static bitmap sized by task count, not by ASID width

`asid_allocator`'s bitmap is sized from the runtime-probed `asid_bits`
(e.g. `ID_AA64MMFR0_EL1.ASIDBits`) because it must be able to track every
raw ASID value the hardware could ever produce -- a
`reloco::vector<std::uint64_t>` obtained through a caller-supplied
allocator. `fixed_asid_allocator` does not have that problem: since
`Capacity` (the task count) is always `<=` the hardware ASID space, it
is only ever necessary to track which of the `Capacity` *tasks*
currently hold a live slot, not which of the (potentially much larger)
`2^asid_bits` raw values are in use. The free-slot bitmap is therefore a
fixed-size `std::uint64_t[(Capacity + 63) / 64]` array embedded directly
in the object -- no allocator, no `reloco::vector`, safe to use in
interrupt/trap context or before an allocator even exists, exactly like
[`cpu_mask<Tag, MaxCpus>`](cpu_mask.md)'s own fixed-size storage.
`asid_bits` is still a constructor argument -- `try_create()` rejects it
unless the runtime-probed hardware ASID space is actually wide enough to
hold every slot index, so an undersized hardware ASID register is
caught at startup, not silently truncated into a colliding value later.

## Both `Capacity` and the hardware ASID space are powers of two

`Capacity` must be a power of two (`static_assert`-enforced), matching
how a task/VM table is sized in practice (and how the hardware ASID
space itself is always shaped: `2^asid_bits`), so the number of bits
needed to address a slot, `slot_bits = log2(Capacity)`, is always exact
-- no rounding, no wasted slot-index codespace, computed with a single
`__builtin_ctzll(Capacity)` (this codebase targets GCC/Clang
exclusively, so the builtin is preferred over a portable fallback).

## Spare hardware ASID bits become a cosmetic sequence counter

A real hardware ASID register is frequently wider than `log2(Capacity)`
-- e.g. a 16-bit ASID register (`asid_bits == 16`) against a `Capacity`
of 256 tasks (`slot_bits == 8`) leaves 8 bits of the register
permanently unused by the slot index alone. Rather than waste that
codespace, `fixed_asid_allocator` packs a small rolling sequence counter
into those spare top bits (`seq_bits = asid_bits - slot_bits`): every
`allocate()` bumps the counter for the slot's group and packs `(seq <<
slot_bits) | slot` into the returned handle, so the *same* task slot
visibly produces a *different* raw hardware ASID value each time it is
reallocated -- useful when staring at a register dump, a TLB-entry
trace, or a log line to tell "this is a fresh occupant of slot N" apart
from "this is the same occupant as before" at a glance.

This counter is **purely a diagnostic aid, not a correctness
mechanism**: `release()`/`allocate()` already require the caller to
flush that one ASID's tagged TLB entries on every reuse regardless (see
"Division of responsibility" below), so two allocations of the same
slot that happened to produce the exact same raw value (e.g. because
`seq_bits == 0`, the common case when `asid_bits == slot_bits` and there
are no spare bits at all) would be just as correct, merely harder to
tell apart by eye. Because of this, the sequence counter is deliberately
allowed to be coarser than "one independent counter per slot":
`SeqGroupSize` (a power-of-two template parameter, default 32) lets a
single counter be shared across a whole block of `SeqGroupSize`
neighboring slots, bumped on *any* allocation within the block --
trading a little extra, harmless "two different slots momentarily show
the same generation prefix" ambiguity for `Capacity / SeqGroupSize`
counters instead of `Capacity` of them. `slot_of()` recovers the stable
per-task slot index from a handle regardless of which sequence value
happens to be packed alongside it.

## Why not just reuse `tagged_asid<Tag>`

`asid_allocator`'s `tagged_asid<Tag>` packs a software generation counter
that `asid_allocator::allocate()`'s `prev`/fast-path and rollover logic
read back out via `generation_of()` to decide whether a cached ID is
still valid -- machinery that only makes sense paired with the allocator
that maintains and *interprets* that counter. `fixed_asid_allocator`'s
sequence counter, by contrast, is written but never read back by
anything (it is cosmetic only), so its handle, `fixed_asid<Tag>`, is
deliberately a separate, unrelated type: reusing `tagged_asid<Tag>` here
would let a handle produced by one allocator kind be silently accepted
by the other's `release()`/`asid_of()`/`generation_of()` (which would
then misinterpret this header's cosmetic sequence bits as a real,
rollover-tracked generation) -- exactly the kind of cross-space mixup
`tagged_asid<Tag>`'s own per-`Tag` phantom typing exists to rule out
between a process ASID and a VMID. `fixed_asid<Tag>` and
`tagged_asid<Tag>` are therefore unrelated types with no conversion
between them, even when instantiated on the exact same `Tag`
(`process_asid_tag`/`vmid_tag`, bundled in `asid_allocator.hpp`, are
reused here purely for their phantom-tagging meaning, not for any shared
handle representation).

## Division of responsibility: bookkeeping here, hardware effects at the caller

Like `asid_allocator`, `fixed_asid_allocator` never touches a TLB,
system register, or any other hardware state -- it is pure bookkeeping.
`allocate()` never requires a TLB flush of its own (there is no
generation rollover to flush after); `release()` hands a slot back to
the free pool, after which the caller must issue a **tagged**
(this-ASID-only) TLB invalidation before that slot's raw ASID value is
handed to a different task, so the new owner never observes the old
owner's stale translations -- exactly `asid_allocator`'s own
destruction-time flush rule, minus the generation-rollover row, which
never applies here.

## Thread safety

`fixed_asid_allocator` performs **no internal synchronization** --
`allocate()` and `release()` mutate the shared bitmap (and sequence
counters) with no locking or atomics whatsoever, exactly like
`asid_allocator` and every other bookkeeping header in this library. A
single instance may only be driven from one logical thread of execution
at a time; concurrent callers must serialize access themselves (e.g.
`structo::sync::irq_locked<fixed_asid_allocator<Tag, Capacity>>`
composed with a caller-supplied cross-core spinlock when shared across
cores).

## API

- `try_create(std::size_t asid_bits)` -- fallible construction. No
  allocator is involved (the bitmap is a fixed-size in-object array);
  `asid_bits` only needs validating, never sizing anything. Rejects
  `asid_bits` outside `[min_asid_bits, max_asid_bits]`, or narrower than
  `slot_bits` (the hardware ASID space is too narrow to give every one
  of `Capacity` tasks a distinct permanent slot).
- `reserve(raw_type slot)` -- permanently removes `slot` from circulation
  (e.g. an architecturally reserved identity-mapping ASID); call before
  any `allocate()`.
- `allocate()` -- hands out a fresh, permanent ASID for a task/VM that is
  about to start existing, packing a bumped cosmetic sequence counter
  into any spare hardware ASID bits above `slot_bits`. Returns
  `error::allocation_failed` once every one of `Capacity` slots is in
  use.
- `release(context_id id)` -- returns `id`'s slot to the free pool (a
  no-op for an invalid `id`).
- `asid_of(context_id id)` -- the sanctioned way to decode the raw,
  hardware-loadable ASID bits out of an opaque handle, including the
  cosmetic sequence prefix -- exactly the value to load into a hardware
  ASID register.
- `slot_of(context_id id)` -- the stable per-task slot index, stripped of
  the cosmetic sequence prefix; the same value across every
  reallocation of the same task's slot.
- `asid_bits()`, `used_count()` -- diagnostics.
- `slot_bits` (static) -- number of low bits of a raw ASID value
  dedicated to the slot index, `log2(Capacity)`.

## Example

```cpp
// Capacity known at compile time, a power of two: this kernel never
// schedules more than 64 tasks concurrently, far inside the 16-bit
// hardware ASID space probed below.
using allocator_type = structo::arch::fixed_asid_allocator<structo::arch::process_asid_tag, 64>;

std::size_t hw_asid_bits = probe_asid_bits(); // e.g. 16
auto maker = allocator_type::try_create(hw_asid_bits);
if (!maker) { panic("hardware ASID space too narrow for Capacity tasks"); }
auto allocator = std::move(maker.value());

// Task creation: hand it a permanent ASID for its whole lifetime.
auto alloc_res = allocator.allocate();
if (!alloc_res) { panic("fixed_asid_allocator exhausted: Capacity too small"); }
task.asid = alloc_res.value();
arch_write_ttbr0_asid(task.page_table_base, allocator.asid_of(task.asid));

// Task exit: return the slot to the pool and flush just that one tag.
auto raw_asid = allocator.asid_of(task.asid);
allocator.release(task.asid);
task.asid = {};
arch_flush_tlb_asid(raw_asid); // tagged: only this now-reusable ASID's entries
```

See also: [`asid_allocator.md`](asid_allocator.md) (the
generation-rollover allocator this header's "never needs to reclaim"
case trades away), [`cpu_mask.md`](cpu_mask.md) (the fixed-size,
allocation-free bitmask storage convention this header follows).
