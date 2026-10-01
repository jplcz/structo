<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::arch::cpu_mask<Tag, MaxCpus>`

`include/structo/arch/cpu_mask.hpp`

A fixed-capacity, compile-time-sized bitmask over logical CPU (or vCPU)
indices `[0, MaxCpus)`, parameterized on a phantom `Tag` so a physical-core
mask and a vCPU mask can never be confused at the type level. Backing
storage is a small, fixed-size array of `std::uint64_t` words embedded
directly in the object -- no allocation, no `reloco::vector`, safe to use
in interrupt/trap context or before an allocator even exists.

## Why this exists

Kernels and hypervisors constantly need "which of these N things does
this apply to" bitmasks -- which cores to IPI during a TLB shootdown (see
[`asid_allocator.md`](asid_allocator.md)'s SMP flush guide), which cores a
task's affinity allows it to run on, which vCPUs of a VM need a virtual
interrupt delivered, which cores are currently online. `cpu_mask<Tag,
MaxCpus>` is the allocation-free, fixed-size bitmask for exactly that:
`MaxCpus` is a compile-time template parameter (matching `hw_id_lut`'s
`MaxCpus` and `asid_allocator`'s `MaxActive`).

## Why tagged

A physical-core bitmask and a virtual-CPU bitmask are both "a bitmask of
CPU-shaped things", but a vCPU index from one VM's mask must never be fed
into a physical-core API expecting a physical-core mask (or vice versa) --
the two index spaces mean completely different things even though
they're both just small integers. `cpu_mask<Tag, MaxCpus>` is
parameterized on a phantom `Tag` for exactly the reason `tagged_asid<Tag>`
is (see [`asid_allocator.md`](asid_allocator.md)): `physical_cpu_tag` and
`vcpu_tag` are bundled, and a `cpu_mask<physical_cpu_tag, 64>` and a
`cpu_mask<vcpu_tag, 64>` are simply unrelated types with no implicit
conversion between them, at zero runtime cost. Like `tagged_asid`, there
is deliberately no cross-tag cast -- a physical core index and a vCPU
index are never legitimately interchangeable.

## Rust `bitflags`-flavored set algebra

A CPU mask is structurally a *set* of flags, which is exactly the domain
Rust's `bitflags` crate targets -- unlike `target_ptr`'s arithmetic domain
(where Rust's `checked_add`/`wrapping_add`/`saturating_add` family is the
natural fit; see [`target_ptr.md`](target_ptr.md)), the natural fit here
is `bitflags`' set-algebra vocabulary: `contains()`, `intersects()`,
`union_with()`/`operator|`, `intersection()`/`operator&`,
`difference()`/`operator-`, `symmetric_difference()`/`operator^`, and
`complement()`/`operator~`, all UB-free (unused trailing bits in the last
word are permanently masked to zero so `complement()`/`filled()` can
never spuriously produce an out-of-range bit). Per-CPU access
(`set`/`clear`/`test`/`toggle`) instead follows `structo`'s usual
checked/`try_`/`unsafe_` tri-tier convention, since those operate on a
runtime index that can be out of range.

## Bit-scanning via compiler intrinsics

`count()`, `lowest_set()`/`lowest_set_from()`, `highest_set()`, and their
atomic counterparts are implemented with GCC/Clang's
`__builtin_popcountll`/`__builtin_ctzll`/`__builtin_clzll` intrinsics
rather than hand-rolled bit-by-bit loops -- these lower directly to a
single hardware instruction (`popcnt`/`bsf`/`bsr` or `tzcnt`/`lzcnt` on
x86, `rbit`+`clz` on AArch64) on every target this library supports,
rather than looping one bit at a time.

## Atomic per-CPU operations (lock-free, GCC/Clang only)

A single, shared `cpu_mask` instance (e.g. a kernel-wide "online CPUs"
mask each core sets its own bit in during bring-up) needs concurrent,
lock-free updates from multiple cores. `cpu_mask` provides exactly the
subset of `std::atomic<T>`'s API (and Linux's atomic bitops:
`set_bit`/`clear_bit`/`test_and_set_bit`/`find_next_bit`/...) that this
domain needs, each a single atomic load or read-modify-write of the one
word containing the requested bit, implemented directly with GCC/Clang's
`__atomic_*` builtins -- this library targets only those two compilers,
matching [`early_rendezvous_barrier.md`](early_rendezvous_barrier.md)'s
own convention. No `std::atomic<T>` storage is used, so the non-atomic
API stays trivially copyable and `constexpr`-usable; the header fails to
compile outright (`#error`) on any other compiler.

- `atomic_test`/`atomic_set`/`atomic_clear`/`atomic_toggle` -- single-bit
  atomic load/set/clear/flip, each with checked (traps out of range),
  `atomic_try_`-fallible, and `unsafe_atomic_`-unchecked tiers.
- `atomic_test_and_set`/`atomic_test_and_clear`/`atomic_test_and_toggle`
  -- atomic read-modify-write returning the *previous* bit state, mirroring
  Linux's `test_and_set_bit()`/`test_and_clear_bit()`/`test_and_change_bit()`.
- `atomic_word`/`atomic_try_word` -- atomic load of one raw backing word.
- `atomic_lowest_set_from`/`atomic_lowest_set` -- atomic find-first-set,
  mirroring Linux's `find_next_bit()`: scans word-by-word with atomic
  loads plus `__builtin_ctzll`, no bit-by-bit loop.
- `atomic_find_and_set_from`/`atomic_find_and_set` -- atomic find-first-
  **clear**-and-set, a lock-free "claim a free slot" bitmap-allocator
  primitive built on an `__atomic_compare_exchange_n()` retry loop;
  returns the claimed index, or `nullopt` if every CPU is already set.

This gives per-bit atomicity, **not** whole-mask atomicity: a mask
spanning more than one word (`MaxCpus > 64`) cannot be observed or
updated as a single indivisible unit -- a concurrent reader can observe
one word already updated and another not yet, the same inherent
limitation real kernels' `cpumask_t` has. `atomic_find_and_set` is the one
exception that *is* safe against concurrent contention on the same word:
its compare-and-swap retry loop guarantees two racing callers never claim
the same bit, even when their candidate bits land in the same word.
Mixing an `atomic_*` call with a plain, non-atomic accessor on the same
instance from different threads without external synchronization is a
data race like any other.

## API

- `empty()` / default constructor -- no CPUs set.
- `filled()` -- every CPU in `[0, MaxCpus)` set.
- `single(cpu)` (checked) / `try_single(cpu)` (fallible) -- a mask with
  only `cpu` set.
- `set`/`clear`/`test`/`toggle` (checked/`try_`/`unsafe_` tri-tier) --
  per-CPU accessors.
- `word(index)`/`try_word(index)` -- raw backing-word access.
- `count()`, `any()`, `none()`, `all()` -- whole-mask queries.
- `lowest_set()`/`lowest_set_from(start)`, `highest_set()` -- bit-scan
  queries, `reloco::optional<std::size_t>`-returning.
- `contains`, `intersects`, `union_with`/`operator|`,
  `intersection`/`operator&`, `difference`/`operator-`,
  `symmetric_difference`/`operator^`, `complement`/`operator~`, in-place
  `operator|=`/`&=`/`^=`/`-=`, `operator==`/`!=` -- Rust `bitflags`-style
  set algebra.
- `begin()`/`end()` -- forward iterator yielding set CPU indices,
  ascending.
- `atomic_test`/`atomic_set`/`atomic_clear`/`atomic_toggle`,
  `atomic_test_and_set`/`atomic_test_and_clear`/`atomic_test_and_toggle`,
  `atomic_word`/`atomic_try_word`, `atomic_lowest_set[_from]`,
  `atomic_find_and_set[_from]` -- lock-free atomic subset (see above).

## Example

```cpp
using core_mask = structo::arch::cpu_mask<structo::arch::physical_cpu_tag, 128>;

core_mask online; // starts empty
online.set(0);
online.set(1);
online.set(2);

core_mask siblings = core_mask::single(1) | core_mask::single(2);
if (online.contains(siblings)) {
  // every core in `siblings` is also online
}

for (std::size_t cpu : online) {
  arch_send_ipi(cpu); // iterates 0, 1, 2
}

core_mask offline = online.complement(); // every core NOT in `online`
```

```cpp
core_mask online_cpus; // shared, e.g. a global/static instance

// Called independently by each core during its own bring-up path --
// safe without a lock, even for cores whose bits land in the same word.
void mark_this_cpu_online(std::size_t cpu_id) {
  online_cpus.atomic_set(cpu_id, std::memory_order_release);
}

bool is_cpu_online(std::size_t cpu_id) {
  return online_cpus.atomic_test(cpu_id, std::memory_order_acquire);
}

// Lock-free "claim a free vCPU slot" bitmap allocator, racing against
// other cores calling the same function concurrently.
structo::result<std::size_t> allocate_vcpu_slot(core_mask &free_slots) {
  auto slot = free_slots.atomic_find_and_set();
  if (!slot.has_value()) {
    return structo::unexpected(structo::error::resource_exhausted);
  }
  return slot.value();
}
```

See also: [`asid_allocator.md`](asid_allocator.md) (the SMP TLB-shootdown
guide `cpu_mask` is a natural fit for computing/tracking target cpu
sets), [`early_rendezvous_barrier.md`](early_rendezvous_barrier.md) (the
GCC/Clang `__atomic_*`-builtin, plain-integer-storage convention
`cpu_mask`'s atomic section follows).
