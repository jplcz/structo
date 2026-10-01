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

## Bridging to an `mm_context`-like structure

> **A ready-made implementation of this pattern exists:** see
> [`mm_asid_context.md`](mm_asid_context.md) (`mm_asid_context<AsidTag,
> MaxCpus>`) for a tested type that embeds exactly the cached
> `context_id` plus the lock-free cpu-residency mask this section
> describes, instead of hand-rolling the fields below.

A per-address-space structure (Linux's `mm_context_t`, a hypervisor's
per-VM `vmid` field, etc.) should cache exactly one `context_id` across
its whole lifetime, driven through three events that are **not**
symmetric with `allocate()`/`release()`:

| Event | Allocator call | TLB action |
|---|---|---|
| **Activation** (context switch in / `vcpu_load`) | `allocate(slot, mm.asid)` -- pass the cached ID back as `prev` so a still-valid one is reused for free | **Global, non-tagged** flush, but only if `flush_required` came back `true` (a rollover just happened) |
| **Deactivation** (switching away to run something else) | none | none -- the whole point of a tagged TLB is that entries stay cached, ready for instant flush-free reactivation |
| **Destruction** (process/VM exit) | `release(mm.asid)` | **Tagged** (this-ASID-only) flush, so the now-reusable hardware ASID doesn't serve stale translations to its next owner |
| In-place mapping change (`munmap`/`mprotect`) while still resident | none | **Tagged** flush of this context's own ASID only -- every other address space's cached entries are untouched |

The distinction between the rollover's **global** flush and the
destruction/mapping-change **tagged** flush matters: a rollover may hand
some *other* address space the exact same raw ASID bits in the new
generation, so stale old-generation entries carrying that number must be
invalidated everywhere, not just locally; a lone `release()` or mapping
change, by contrast, only needs to clear this one context's own tagged
entries and must not disturb anyone else's.

```cpp
using allocator_type = structo::arch::asid_allocator<structo::arch::process_asid_tag, 8>;

struct mm_context {
  allocator_type::context_id asid{}; // default-constructed: invalid, never activated
  // ... page tables, VMAs, etc.
};

// Activation: context switch INTO `mm` on logical core `core_id`.
void activate_mm(mm_context &mm, std::size_t core_id) {
  auto alloc_res = allocator.allocate(core_id, mm.asid);
  if (!alloc_res) { panic("ASID space exhausted"); } // unreachable in practice
  mm.asid = alloc_res.value().id;
  if (alloc_res.value().flush_required) {
    arch_flush_tlb_all(); // global, non-tagged: a rollover just happened
  }
  arch_write_ttbr0_asid(mm.page_table_base, allocator.asid_of(mm.asid));
}

// Deactivation: switching away to run something else. No allocator
// call, no TLB flush -- `mm.asid` simply stays cached as-is.
void deactivate_mm(mm_context &) {}

// Destruction: the address space itself is being torn down.
void mm_exit(mm_context &mm) {
  auto asid = allocator.asid_of(mm.asid); // decode before releasing
  allocator.release(mm.asid);
  mm.asid = {};
  arch_flush_tlb_asid(asid); // tagged: only this now-reusable ASID's entries
}

// In-place mapping change (e.g. munmap) while `mm` stays resident.
void flush_mm_mappings(mm_context &mm) {
  arch_flush_tlb_asid(allocator.asid_of(mm.asid)); // tagged, this ASID only
}
```

## Finding which CPUs to flush on page-table mutation

`flush_mm_mappings()` above is only correct for a strictly single-core
target. On SMP, a page-table mutation must reach the TLB of **every**
core that could hold a stale, tagged translation for `mm` -- not just
"the core currently running it": `active(slot)` reports the context most
recently loaded into tracking slot `slot` and deactivation never touches
it (deactivation is a pure no-op by design -- see "Bridging to an
`mm_context`-like structure" above), so a slot whose owner was merely
switched away from, not released, still correctly reports `mm`'s ID
there, flagging that core's TLB as potentially still carrying `mm`'s
tagged entries from its last residency.

Iterating every slot `[0, MaxActive)` and comparing against `mm.asid`
(via `tagged_asid`'s `operator==`) is therefore the right -- and only --
way to compute the shootdown target set. There is no separate "is `mm`
active anywhere" query: this iteration already answers it. An empty
target set means no core's TLB can contain `mm`'s entries, so nothing
needs flushing at all. Note `allocate()`'s fast-path reuse of a
still-valid `prev` performs no flush of its own, so callers that mutate
mappings must always flush explicitly as shown below.

Delivering the flush to the target set is architecture-specific:

- **Broadcast-capable ISAs** (e.g. ARM's inner-shareable `TLBI ...IS`
  forms) only need a single instruction on any one core; every core in
  the shareability domain observes it in hardware, with no software IPI
  involved.
- **Non-broadcast ISAs** (plain `INVLPG`/`INVPCID` on x86-64, or ARM's
  non-`IS` forms) require the classic TLB-shootdown pattern: flush
  locally in-line if the local core is in the target set, then send an
  inter-processor interrupt to every *other* target core asking it to run
  the same local, tagged flush on itself, and wait for every remote core
  to acknowledge before returning.

This is exactly the same shootdown machinery a generation rollover's
`arch_flush_tlb_all()` needs, too -- a rollover's stale entries can be
resident on *any* core, not just the one that observed `flush_required`,
so that flush must also reach every core (typically by unconditionally
targeting the whole cpu mask, rather than computing one from `active()`).

```cpp
// Returns a bitmask of logical core indices that may be holding a stale,
// tagged TLB entry for `mm` -- empty if `mm` has never been resident on
// any core, or was fully flushed since its last residency.
std::uint64_t cpu_mask_for_mm(const mm_context &mm) {
  std::uint64_t mask = 0;
  if (!mm.asid.is_valid()) { return mask; } // never activated: nothing to flush
  for (std::size_t slot = 0; slot < allocator_type::max_active; ++slot) {
    if (allocator.active(slot) == mm.asid) {
      mask |= (std::uint64_t{1} << slot); // assumes slot == logical core index
    }
  }
  return mask;
}

// Call after mutating `mm`'s page tables (munmap/mprotect/etc.) while it
// may be resident -- current or past -- on any number of cores.
void flush_mm_mappings_smp(mm_context &mm) {
  std::uint64_t targets = cpu_mask_for_mm(mm);
  if (targets == 0) { return; } // not resident anywhere: nothing can be stale

  auto raw_asid = allocator.asid_of(mm.asid);
  if constexpr (arch_has_broadcast_tlbi) {
    arch_flush_tlb_asid_broadcast(raw_asid); // e.g. ARM TLBI ...IS: one instruction, every core
  } else {
    if (targets & (std::uint64_t{1} << this_cpu())) {
      arch_flush_tlb_asid(raw_asid); // local, tagged
    }
    arch_send_tlb_shootdown_ipi(targets & ~(std::uint64_t{1} << this_cpu()), raw_asid);
    arch_wait_for_shootdown_acks(targets); // block until every remote core has flushed
  }
}
```

## Fine-grained flushes after unmapping a single page

`flush_mm_mappings_smp()` above invalidates **every** TLB entry tagged
with `mm`'s ASID -- correct, but wasteful after e.g. a single `munmap()`
of one page: every other still-mapped page's cached translation is
thrown away too, only to be refetched by a page-table walk on its next
access. Most ISAs provide a by-address (optionally still ASID-tagged)
invalidation form precisely for this case (ARM's `TLBI VAE1IS`, x86's
single-address `INVLPG`/`INVPCID` type 0) -- use it instead of the
whole-ASID form whenever the set of unmapped pages is small, following
exactly the same target-cpu-mask computation as above (the *scope* of
what gets invalidated changes; *where* it needs to be invalidated does
not).

```cpp
// Call after unmapping exactly one page at `vaddr` from `mm`.
void flush_mm_page(mm_context &mm, std::uintptr_t vaddr) {
  std::uint64_t targets = cpu_mask_for_mm(mm);
  if (targets == 0) { return; }

  auto raw_asid = allocator.asid_of(mm.asid);
  if constexpr (arch_has_broadcast_tlbi) {
    arch_flush_tlb_page_asid_broadcast(raw_asid, vaddr); // by-address, tagged, one instruction
  } else {
    if (targets & (std::uint64_t{1} << this_cpu())) {
      arch_flush_tlb_page_asid(raw_asid, vaddr); // local, by-address, tagged
    }
    arch_send_tlb_shootdown_ipi_page(targets & ~(std::uint64_t{1} << this_cpu()), raw_asid, vaddr);
    arch_wait_for_shootdown_acks(targets);
  }
}
```

Unmapping a short *run* of pages (e.g. a small `munmap()` range) extends
naturally: loop over each page in the range, reusing one
`cpu_mask_for_mm()` computation and one shootdown IPI/ack round-trip for
the whole range rather than per page, issuing a by-address invalidation
(or, on ISAs that provide one, a single hardware range-invalidation
instruction, e.g. ARM's `TLBI RVAE1IS`) for each page. Past some
range-size threshold, though, the per-page loop's cumulative cost
exceeds a single full-ASID flush -- real kernels (e.g. Linux's
`tlb_flush_mmu()`) fall back to the whole-ASID form once the unmapped
range spans more than a small, architecture-tuned number of pages,
rather than looping indefinitely:

```cpp
// Call after unmapping [start, end) from `mm` (end exclusive, both
// page-aligned). Falls back to a full-ASID flush past a small
// range-size threshold, matching real kernels' amortization heuristic.
constexpr std::size_t max_pages_for_fine_grained_flush = 33; // architecture-tuned, e.g. Linux arm64's default

void flush_mm_range(mm_context &mm, std::uintptr_t start, std::uintptr_t end) {
  std::size_t num_pages = (end - start) / page_size;
  if (num_pages > max_pages_for_fine_grained_flush) {
    flush_mm_mappings_smp(mm); // cheaper than num_pages individual invalidations
    return;
  }

  std::uint64_t targets = cpu_mask_for_mm(mm);
  if (targets == 0) { return; }
  auto raw_asid = allocator.asid_of(mm.asid);
  for (std::uintptr_t vaddr = start; vaddr < end; vaddr += page_size) {
    if constexpr (arch_has_broadcast_tlbi) {
      arch_flush_tlb_page_asid_broadcast(raw_asid, vaddr);
    } else {
      if (targets & (std::uint64_t{1} << this_cpu())) { arch_flush_tlb_page_asid(raw_asid, vaddr); }
      arch_send_tlb_shootdown_ipi_page(targets & ~(std::uint64_t{1} << this_cpu()), raw_asid, vaddr);
    }
  }
  if constexpr (!arch_has_broadcast_tlbi) { arch_wait_for_shootdown_acks(targets); } // once, for the whole range
}
```

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

See also: [`mm_asid_context.md`](mm_asid_context.md) (the ready-made
`mm_context`-bridging type built on this allocator), [`hw_id_map.md`](hw_id_map.md)
(the analogous hardware-ID-to-logical-index lookup this header's
`MaxActive` convention is modeled after), [`io_space_ref.md`](io_space_ref.md)
(the same "bookkeeping here, hardware effects at the caller" division of
responsibility).
