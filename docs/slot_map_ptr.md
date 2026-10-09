<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::slot_map_ptr<T, Mapper, PhysInt>`

`include/structo/slot_map_ptr.hpp`

The dynamically-remapped counterpart of `dmap_ptr`/`dmap_mapper` (see
[`phys_addr`](phys_addr.md)). It is for targets where a permanent,
whole-range direct map is unavailable or undesirable. Examples are a
32-bit system with more physical memory than spare virtual address space,
and a TEE/secure-world image that should only expose a small on-demand
window into Non-secure RAM.

`slot_map_ptr` stores only a physical address and has no persistent
virtual address. `try_map()` borrows one of a small fixed pool of virtual
"slots", programs it to point at the physical address, and returns an
RAII, move-only `guard`. The guard is the only way to get a `T*`. The
slot is unmapped when the guard dies, so the mapping cannot outlive its
scope. This is the C++ form of Linux `kmap_atomic()`/`kunmap_atomic()` and
FreeBSD `pmap_quick_enter_page()`/`pmap_quick_remove_page()`.

The header also ships two ready-made `Mapper` policies:
`slot_map_mapper` (per-CPU, non-atomic) and `shared_slot_map_mapper`
(lock-protected, refcounted sharing of identical pages, optional
blocking). Both build their busy tables on
[`fixed_bitmap`](fixed_bitmap.md). Each slot maps exactly one
`slot_size`-aligned page, and a request that would cross a slot boundary
fails with `error::out_of_range`. When used with the
[`compat_sg`](compat_sg.md) codecs, `ArchHooks::slot_size` must equal the
codec's `PageTraits::page_size`.

## Usage

```cpp
// ArchHooks: the architecture-specific part. Everything here is static.
struct ns_peek_hooks {
  // Bytes mapped per slot. Must be a power of two; each slot maps ONE
  // slot_size-aligned physical page.
  static constexpr std::size_t slot_size = 4096;

  // Fixed virtual address of slot `slot` (its window within a reserved VA
  // range). Must be stable.
  static void *slot_base(std::size_t slot) noexcept {
    return reinterpret_cast<void *>(NS_PEEK_WINDOW_BASE + slot * slot_size);
  }
  // Point slot `slot`'s VA window at the slot_size-aligned physical page
  // `phys_aligned`: write the PTE and do the TLB invalidation.
  static reloco::result<void> program(std::size_t slot, std::uint64_t phys_aligned) noexcept {
    return arch_map_ns_page(slot_base(slot), phys_aligned);
  }
  // Tear the mapping down again. Must be safe on an already-unmapped slot.
  static void unprogram(std::size_t slot) noexcept { arch_unmap_page(slot_base(slot)); }
};

// Template arguments: 4 = number of slots in the pool; ns_peek_hooks = the
// ArchHooks above; nonsecure_phys_space = the phys_addr space tag this mapper
// accepts. The fourth (PhysInt) defaults to uint64_t. This mapper is NOT
// thread-safe: use it per CPU with IRQs disabled around map/unmap.
using ns_peek_mapper = structo::slot_map_mapper<4, ns_peek_hooks, structo::nonsecure_phys_space>;

// from_paddr(): wrap a physical address (a phys_addr<uint32_t, space_tag>).
// A null address gives a null slot_map_ptr; an address that
// Mapper::validate_phys() rejects fails with error::security_violation.
auto ptr = structo::slot_map_ptr<uint32_t, ns_peek_mapper>::from_paddr(ns_phys);
if (ptr) {
  // try_map() with no argument maps sizeof(T) bytes. Pass an explicit byte
  // size to map more (or for T = void), as long as the range stays inside one
  // slot. It fails with error::busy if every slot is in use.
  auto guard = (*ptr).try_map();
  if (guard) {
    uint32_t value = **guard; // *guard dereferences the T; get() gives T*
  } // the slot is unmapped here, including on early return
}

// Shared variant: concurrent users of the SAME physical page share one slot
// via a reference count, and the slot is torn down when the last guard
// goes. It needs ArchHooks::phys_of(slot), which returns the physical
// address last passed to program() for that slot.
using shared_mapper =
    structo::shared_slot_map_mapper<64, // EntryCount: number of rows reserved up front
                                    ns_peek_hooks, // ArchHooks (+ phys_of())
                                    reloco::spin_lock, // Lock: needs lock()/unlock()
                                    structo::slot_map_no_wait_policy, // fail with busy when full
                                    structo::nonsecure_phys_space>;   // space tag
// REQUIRED once, before the first acquire. It allocates the hash table, and
// the optional argument is the allocator (default: default_allocator()).
// Repeated calls are no-ops.
if (!shared_mapper::try_init()) { /* handle allocation failure */ }
```

## `slot_map_ptr`

| Member | Description |
|---|---|
| `space_tag`, `phys_type` | `Mapper::space_tag` and `phys_addr<T, space_tag, PhysInt>`. |
| `slot_map_ptr()` / `slot_map_ptr(nullptr)` | Null pointer. |
| `phys()` | The stored physical address. |
| `is_null()` / `explicit operator bool` | Null test (`operator bool` is true when non-null). |
| `static from_paddr(phys_type)` | `result<slot_map_ptr>`. A null address gives a null pointer. `Mapper::validate_phys()` returning false gives `error::security_violation`. |
| `try_map(std::size_t size)` | `result<guard>` mapping `[phys(), phys()+size)`. A null pointer gives `error::invalid_argument`. Otherwise it returns whatever `Mapper::acquire()` reports. |
| `try_map()` | Same, with `size = sizeof(T)`. Ill-formed for `T = void`. |

`sizeof(slot_map_ptr)` equals `sizeof(std::uint64_t)` (checked by a
`static_assert`). The pointer carries no overhead beyond the physical
address.

### `slot_map_ptr::guard`

A move-only, `[[nodiscard]]` RAII handle for one live slot. Copying is
deleted. A moved-from guard is empty.

| Member | Description |
|---|---|
| `guard()` | Empty guard. |
| `is_null()` / `explicit operator bool` | Whether it holds a slot (`operator bool` is true when it does). |
| `get()`, `operator->()`, `operator*()` | Access the mapped `T`. They are lifetime-bound to the guard. |
| `reset()` | Early unmap through `Mapper::release()`. Idempotent. |
| destructor / move-assign | Release the slot (move-assign first releases its current one). |

## `Mapper` contract

```cpp
using space_tag = /* a phys_addr SpaceTag */;
struct mapped_slot { std::size_t index; void *vaddr; };
template <typename T> static bool validate_phys(phys_addr<T, space_tag, PhysInt>) noexcept;
static result<mapped_slot> acquire(phys_addr<void, space_tag, PhysInt> phys, std::size_t size) noexcept;
static void release(std::size_t slot) noexcept;
```

## `slot_map_mapper<SlotCount, ArchHooks, ExpectedSpace = default_phys_space, PhysInt = std::uint64_t>`

Fixed pool of `SlotCount` slots. Slot state is a plain
`fixed_bitmap<SlotCount>` held in a function-local static, so each
distinct instantiation has its own pool. It is **not thread-safe**. It is
sound only per-CPU with IRQs disabled, like `kmap_atomic()`.

`acquire(phys, size)` returns `error::out_of_range` if `size == 0`, if
`size > slot_size`, or if the range crosses a slot boundary. It returns
`error::busy` if no slot is free, or whatever `ArchHooks::program()`
fails with. On success `vaddr` already includes the offset of `phys`
within its page. `release(slot)` calls `ArchHooks::unprogram()` and frees
the slot. `validate_phys()` always returns true.

`ArchHooks` requires: `static constexpr std::size_t slot_size`,
`slot_base(slot)`, `program(slot, phys_aligned)` and `unprogram(slot)`.

## `shared_slot_map_mapper<EntryCount, ArchHooks, Lock, WaitPolicy, ExpectedSpace, PhysInt>`

Defaults: `Lock = reloco::spin_lock`,
`WaitPolicy = slot_map_no_wait_policy`,
`ExpectedSpace = default_phys_space`, `PhysInt = std::uint64_t`.

Like `slot_map_mapper`, but guarded by `Lock` and backed by a
`reloco::flat_hash_map` keyed by the slot-aligned physical address with a
refcount per row. A request for an already-mapped page bumps the refcount
and reuses its slot, and a new slot is programmed only for an unmapped
page. The slot is unprogrammed when the last user releases it.

| Member | Description |
|---|---|
| `try_init(allocator_ref alloc = default_allocator())` | Allocates and reserves `EntryCount` rows. Call before any `acquire()`/`release()`. Idempotent after a success. |
| `acquire(phys, size)` | Same `out_of_range` and `program()` errors as `slot_map_mapper`. When every row is taken it calls `WaitPolicy::wait()` instead of failing outright, and it can also return a hash-table insertion error. `acquire()` asserts that `try_init()` has run. |
| `release(slot)` | Drops one reference. At zero it removes the row, unprograms the slot and calls `WaitPolicy::notify_all()`. |
| `validate_phys()` | Always true. |

Extra `ArchHooks` requirement: `static PhysInt phys_of(std::size_t slot)`,
the aligned physical address last programmed into `slot`.

`Lock` needs `lock()`/`unlock()`.

### `WaitPolicy`

```cpp
template <typename LockT> static result<void> wait(LockT &lock) noexcept;
static void notify_all() noexcept;
```

`wait()` is called with `lock` held when no free row exists. It either
returns an error without touching the lock (`acquire()` then unlocks and
propagates the error), or releases the lock, blocks, relocks and returns
success (`acquire()` then re-scans). `notify_all()` is called by
`release()`, with the lock held, after a row is freed.

`slot_map_no_wait_policy` is the default: `wait()` returns `error::busy`
and `notify_all()` does nothing. Never use a blocking policy from
interrupt context.
