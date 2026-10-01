<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::arch::mm_asid_context<AsidTag, MaxCpus, CpuTag>`

`include/structo/arch/mm_asid_context.hpp`

The per-address-space ASID tracking fields a real `mm_context_t`-like
structure embeds: a cached [`tagged_asid<AsidTag>`](asid_allocator.md)
plus a lock-free [`cpu_mask<CpuTag, MaxCpus>`](cpu_mask.md) of every core
that may still hold a stale, tagged TLB entry for it. This turns
[`asid_allocator.md`](asid_allocator.md)'s "Bridging to an `mm_context`-
like structure" documentation cookbook into an actual, reusable,
unit-tested type instead of copy-pasted example code.

## Why this exists

`asid_allocator<Tag, MaxActive>` already tracks, internally, which
context is resident in each of its `MaxActive` "slot"s (one per core) --
but answering "which cores might still have a stale, tagged TLB entry for
*this specific* address space" from that requires an O(`MaxActive`) scan
of every slot (`asid_allocator.hpp`'s `cpu_mask_for_mm()` example), and
that scan is not safe to perform without the allocator's own external
lock, because it reads every slot's entry, not just the ones belonging to
this address space.

Page-table mutations (`munmap`/`mprotect`) need to ask this question far
more often than `allocate()`/`release()` are ever called, so forcing
every mapping change to contend on the same lock that guards ASID
*allocation* would be needless contention. Embedding a dedicated
`cpu_mask<CpuTag, MaxCpus>` directly in each address space's own tracking
structure answers the same question in O(`MaxCpus / 64`) atomic loads
(`cpu_mask::atomic_snapshot()`) with no lock at all.

## Synchronization contract

`activate()`/`release()` forward directly to the (deliberately
unsynchronized) underlying `asid_allocator`, so they inherit its exact
thread-safety contract verbatim (see [`asid_allocator.md`](asid_allocator.md)'s
"Thread safety" section): calls into the *same* `asid_allocator` instance
-- whether through this `mm_asid_context` or any other -- must be
externally serialized (e.g. `structo::sync::irq_locked<asid_allocator<...>>`
composed with a cross-core spinlock).

`cpu_targets()` is the deliberate exception: it is lock-free and safe to
call concurrently with an in-flight `activate()`/`release()` on another
core (or even this same instance), by design -- recomputing the
TLB-shootdown target set must not force contention on the allocator's
lock. A snapshot observed concurrently with an in-flight `activate()` may
be a subset that is about to grow (the activating core's bit may not be
visible yet) -- always safe, since a core that hasn't yet loaded the ASID
into hardware cannot yet hold a stale translation -- but never a superset
shrinking unexpectedly, since `activate()` only ever sets bits, never
clears them (deactivation is a pure no-op, by design).

## What this intentionally does NOT add: a second generation counter

The "generation" that tells a cached ASID it has gone stale is already
fully tracked inside `tagged_asid<Tag>`'s own opaque packed value and
compared internally by `asid_allocator::allocate()`'s `prev` fast path --
adding a second, separate generation counter here would only duplicate
state the allocator already owns and protects under its lock.
`mm_asid_context` deliberately adds exactly one new piece of state beyond
the cached `tagged_asid<AsidTag>` already described in `asid_allocator.hpp`'s
cookbook: the lock-free, embedded cpu mask.

## API

- `is_valid()` -- `true` once activated at least once.
- `asid()` -- the cached, opaque `context_id` (same thread-safety
  contract as `asid_allocator::active()`; see "Synchronization contract").
- `activate(allocator, cpu, mask_order = memory_order_release)` -- allocates
  (or revalidates) the ASID through `allocator` and records `cpu` into the
  lock-free cpu mask. Returns `flush_required` (whether a generation
  rollover just happened) or `error::invalid_argument` if `cpu >=
  MaxActive`.
- `release(allocator)` -- returns the ASID to `allocator`'s free pool.
  Performs no TLB action and does not clear `cpu_targets()` by design; the
  caller must flush (using `cpu_targets()`) *before* calling this.
- `cpu_targets(order = memory_order_acquire)` -- lock-free snapshot of
  every core that may still hold a stale, tagged entry for this address
  space.
- `clear_cpu_targets()` -- resets the cpu-residency mask to empty; only
  useful when recycling an instance for a brand-new address space after a
  full teardown.

## Example

```cpp
using allocator_type = structo::arch::asid_allocator<structo::arch::process_asid_tag, 8>;
using mm_context_type = structo::arch::mm_asid_context<structo::arch::process_asid_tag, 128>;

struct mm_context {
  mm_context_type asid_ctx; // replaces the raw `context_id asid` field
  // ... page tables, VMAs, etc.
};

// Activation: context switch INTO `mm` on logical core `core_id`.
// Caller must hold whatever lock serializes `allocator` (see above).
void activate_mm(allocator_type &allocator, mm_context &mm, std::size_t core_id) {
  auto flush_required = mm.asid_ctx.activate(allocator, core_id);
  if (!flush_required) { panic("ASID space exhausted"); } // unreachable in practice
  if (flush_required.value()) {
    arch_flush_tlb_all(); // global, non-tagged: a rollover just happened
  }
  arch_write_ttbr0_asid(mm.page_table_base, allocator.asid_of(mm.asid_ctx.asid()));
}

// In-place mapping change (munmap/mprotect) while `mm` stays resident on
// any number of cores -- no allocator lock needed at all.
void flush_mm_mappings_smp(const allocator_type &allocator, const mm_context &mm) {
  auto targets = mm.asid_ctx.cpu_targets(); // lock-free snapshot
  if (targets.none()) { return; } // never resident anywhere: nothing can be stale

  auto raw_asid = allocator.asid_of(mm.asid_ctx.asid());
  if constexpr (arch_has_broadcast_tlbi) {
    arch_flush_tlb_asid_broadcast(raw_asid); // e.g. ARM TLBI ...IS: one instruction, every core
  } else {
    if (targets.test(this_cpu())) {
      arch_flush_tlb_asid(raw_asid); // local, tagged
    }
    for (std::size_t cpu : targets) {
      if (cpu != this_cpu()) { arch_send_tlb_shootdown_ipi(cpu, raw_asid); }
    }
    arch_wait_for_shootdown_acks(targets);
  }
}

// Destruction: the address space itself is being torn down. Caller must
// hold the allocator's lock for release(); the preceding flush does not
// need it (same cpu_targets() call as above).
void mm_exit(allocator_type &allocator, mm_context &mm) {
  flush_mm_mappings_smp(allocator, mm); // tagged flush while the ASID is still valid
  mm.asid_ctx.release(allocator);       // requires the allocator's lock
  mm.asid_ctx.clear_cpu_targets();      // optional: only useful if `mm` is about to be recycled
}
```

See also: [`asid_allocator.md`](asid_allocator.md) (the ASID allocator and
`mm_context`-bridging cookbook this header implements), [`cpu_mask.md`](cpu_mask.md)
(the lock-free, tagged bitmask `cpu_targets()` wraps).
