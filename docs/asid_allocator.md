<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::arch::asid_allocator<Tag, MaxActive>` / `tagged_asid<Tag>`

`include/structo/arch/asid_allocator.hpp`

A software ASID (Address Space IDentifier) allocator: the generation-
counted bitmap scheme real kernels (Linux's arm64 `mm/context.c`,
FreeBSD's ASID/TLB-shootdown path) use to hand out a small hardware
ASID/VMID/PCID register value to tasks/VMs and safely reuse it once the
hardware ID space is exhausted. `tagged_asid<Tag>` is the opaque 32-bit
handle it hands back, parameterized on a phantom `Tag` so a process ASID
can never be confused with a VMID at the type level.

## Why this exists

Hardware ASID-style fields are small (ARM's `TTBR0_EL1.ASID` is 8 or 16
bits depending on `ID_AA64MMFR0_EL1.ASIDBits`; x86-64 PCID is 12 bits; ARM
VMID and Intel VPID are narrower still) -- far too small to assign one
permanently to every task/VM that ever existed. The standard fix, used by
every production kernel with hardware ASIDs, is a *generation-counted*
bitmap: hand out raw ASID values from a bitmap while they last, and when
it's exhausted, bump a software generation counter, flush the TLB
(invalidating every ASID from the outgoing generation at once), and start
reusing bits from zero -- except for any ASID that is still the *active*,
currently-resident ID on some core/VM, which survives the sweep into the
new generation unchanged. `asid_allocator` implements exactly this as
architecture-agnostic bookkeeping.

## Runtime ASID width, compile-time active-slot count

Unlike `phys_addr`/`io_address`/`target_ptr`, the hardware ASID width is
**not** a compile-time constant here: it depends on the actual CPU
model/mode a kernel ends up booted on (`ID_AA64MMFR0_EL1.ASIDBits` is a
runtime system-register read, not something the compiler can know), so
`asid_bits` is a constructor argument supplied once the caller has probed
it. `MaxActive` (how many simultaneous "this ASID is the live one right
now" slots to track -- one per core for a process-ASID allocator, or one
per currently-scheduled VM for a VMID allocator) remains a compile-time
template parameter, matching `hw_id_lut<HwId, MaxCpus, ...>`'s `MaxCpus`:
it bounds a small, fixed-size array, not the (potentially large,
runtime-sized) ASID bitmap itself.

## Allocator-backed bitmap storage

The ASID bitmap is sized from the runtime `asid_bits`, so it is not a
fixed in-class array -- it is a `reloco::vector<std::uint64_t>` obtained
through a caller-supplied `reloco::allocator_ref` at construction time
(`try_allocate`/`try_create`, following `reloco`'s fallible-construction
convention). Once constructed, no further allocation ever occurs --
`allocate()`/`release()` only flip bits in the already-sized bitmap.

## `tagged_asid<Tag>`: ASID vs VMID at the type level

The same bitmap-plus-generation scheme applies equally to a per-task
ASID allocator and a per-VM VMID allocator -- they are the same algorithm
over a differently-sized ID space. To stop a VMID from ever being
accidentally passed where a process ASID was expected (or vice versa),
the ID type is `tagged_asid<Tag>`, parameterized on a phantom `Tag`
(`process_asid_tag`/`vmid_tag` are bundled; define your own tag struct
for further subdivisions, e.g. a secure-world ASID space distinct from a
normal-world one). `tagged_asid<TagA>` and `tagged_asid<TagB>` are
unrelated types with **no** conversion between them -- unlike
`phys_addr`/`io_address`'s `cast_space()`, there is deliberately no
cross-tag cast here: an ASID and a VMID are never legitimately the same
identifier wearing a different hat, so converting one into the other is
a bug, not a cast. `tagged_asid<Tag>` is otherwise a fully opaque 32-bit
handle -- its packed generation/ASID layout is a private implementation
detail of whichever `asid_allocator` produced it. The one sanctioned way
to extract the raw hardware-loadable ASID bits is `asid_allocator::
asid_of()`.

## Division of responsibility: bookkeeping here, hardware effects at the caller

`asid_allocator` never touches a TLB, system register, or any other
hardware state -- it is pure bookkeeping, matching every other header in
this library. `allocate()` returns an `allocation` with a
`flush_required` flag: `true` exactly when a generation rollover just
happened, meaning the caller **must** perform at least a local TLB
invalidation (a global/all-core shootdown, in a multi-core system) before
the newly allocated ASID can be safely loaded into hardware. Exactly
which invalidation instruction/hypercall to issue is architecture-specific
and deliberately out of scope here, matching `io_space_ref.hpp`'s divide
between address-tagging/bookkeeping and the backend-specific access
itself.

## Thread safety

`asid_allocator` performs **no internal synchronization** -- `allocate()`,
`release()`, and any generation rollover they trigger mutate shared state
(the bitmap, the generation counter, the `active[]` tracking array) with
no locking or atomics whatsoever, matching every other bookkeeping header
in this library. A single instance may only be driven from one logical
thread of execution at a time; concurrent callers (multiple cores
context-switching at once, or an interrupt reentering a context switch
already in progress) must serialize access themselves -- for instance by
wrapping the instance in `structo::sync::irq_locked<asid_allocator<Tag,
MaxActive>>` (single-core interrupt exclusion) composed with a
caller-supplied cross-core spinlock when the allocator is shared across
cores. This mirrors how real kernels guard their own ASID bookkeeping
(e.g. Linux arm64's `cpu_asid_lock`) and keeps this header from forcing a
synchronization policy -- or its cost -- onto callers (including
single-core targets) that don't need one.

## API

- `try_allocate(allocator_ref alloc, std::size_t asid_bits)` /
  `try_create(std::size_t asid_bits)` -- fallible construction. Rejects
  `asid_bits` outside `[min_asid_bits, max_asid_bits]` (`max_asid_bits` is
  24, leaving >= 8 generation bits in the packed 32-bit raw value) or too
  narrow to ever outnumber `MaxActive` live contexts.
- `reserve(raw_type asid)` -- permanently removes `asid` from circulation
  (e.g. an architecturally reserved identity-mapping ASID); call before
  any `allocate()`.
- `allocate(std::size_t slot, context_id prev = {})` -- allocates (or, if
  `prev` is still valid in the current generation, instantly revalidates)
  the ASID for a task/VM about to become resident in `slot`. Returns an
  `allocation{ id, flush_required }`.
- `release(context_id id)` -- returns `id`'s bit to the free pool if still
  live in the current generation (a no-op otherwise).
- `asid_of(context_id id)` -- the one sanctioned way to decode the raw,
  hardware-loadable ASID bits out of an opaque handle.
- `generation_of(context_id id)`, `generation()`, `asid_bits()`,
  `num_asids()`, `used_count()` -- diagnostics.
- `active(std::size_t slot)` (checked, traps out of range) /
  `try_active(std::size_t slot)` (fallible) -- the context currently
  resident in a tracking slot.

## Example

```cpp
// Probed once at boot, e.g. from ID_AA64MMFR0_EL1.ASIDBits.
std::size_t hw_asid_bits = probe_asid_bits();

auto maker = structo::arch::asid_allocator<structo::arch::process_asid_tag, 8>::try_create(hw_asid_bits);
if (!maker) { panic("out of memory sizing the ASID bitmap"); }
auto allocator = std::move(maker.value());

// On a context switch into `task` on logical core `core_id`:
auto alloc_res = allocator.allocate(core_id, task.asid);
if (!alloc_res) { panic("ASID space exhausted"); } // unreachable in practice
task.asid = alloc_res.value().id;
if (alloc_res.value().flush_required) {
  arch_flush_tlb_all(); // architecture-specific, not this header's concern
}
arch_write_ttbr0_asid(allocator.asid_of(task.asid)); // the one sanctioned decode
```

See also: [`hw_id_map.md`](hw_id_map.md) (the analogous hardware-ID-to-
logical-index lookup this header's `MaxActive` convention is modeled
after), [`io_space_ref.md`](io_space_ref.md) (the same "bookkeeping here,
hardware effects at the caller" division of responsibility).
