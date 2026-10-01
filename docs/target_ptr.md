<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `target_ptr<T, SpaceTag, PtrType>`

`include/structo/target_ptr.hpp`

A `target_ptr` names a location in *another* virtual address space -- a
userland process's, the kernel's own, a guest VM's, an Arm CCA Realm's, a
TrustZone Secure World's, or any caller-defined one -- that can never be
dereferenced directly through the host CPU's own load/store instructions.
It is the virtual-address-space counterpart to
[`phys_addr`/`dmap_ptr`](phys_addr.md): `dmap_ptr` is for physical
addresses that *are* directly reachable once translated through a direct
map, while `target_ptr` models the classic syscall-handler shape, where a
kernel thread receives a `void *` argument that is only meaningful inside
the calling *user* process's own page tables, or a hypervisor receives a
guest-virtual pointer that only means something once walked through the
guest's own Stage-1 page tables.

## "Current" context only

A `target_ptr` carries no process ID, `vmid`/VCPU handle, or any other
explicit address-space reference. It is only ever valid for whichever
address space is "current" at the moment a `try_materialize*`/
`try_store*` call is actually made -- exactly the contract behind Linux's
`copy_from_user`/`copy_to_user` or FreeBSD's `copyin`/`copyout`. Stashing
a `raw_address()` integer and coming back to it after a context switch
means something else entirely (or nothing at all). Resolving what
"current" means (reading `%cr3`/`TTBR0_EL1`, `curthread`, the active VCPU,
...) is the one thing this header leaves to the embedder, through a
`target_ptr_space_traits<SpaceTag>` specialization.

## Why there is no `cast_space()`

Unlike `phys_addr`, which offers an explicit (unsafe) `cast_space()`
escape hatch because two *physical* address spaces can coincide (e.g. an
identity-mapped IOMMU window), `target_ptr` has no such method at all, not
even an unsafe one. A user address and a kernel address are both plain
integers, but they index entries in *different page tables* -- the same
bit pattern essentially never names the same byte of memory in two
different `target_ptr` spaces. Only a real translation, performed by the
kernel's own address-space/VMA/page-table machinery, can turn a
`target_ptr<T, user_space>` into anything meaningful in `kernel_space`,
and that is squarely outside what a header-only type can express.
`cast_type<U>()` is still available to reinterpret the pointed-to type
while staying in the same space.

## The `target_ptr_space_traits<SpaceTag>` contract

`target_ptr` is unusable for anything beyond holding/comparing/casting a
raw address until the embedder specializes `target_ptr_space_traits` for
their own tag:

```cpp
template <> struct structo::target_ptr_space_traits<my_user_space_tag> {
  using address_type = std::uintptr_t;

  // Optional: the valid address range for this space (e.g. a process's
  // TASK_SIZE on a given architecture). Omitted entirely if the whole
  // address_type range is potentially valid.
  static constexpr address_type min_value = 0x1000;
  static constexpr address_type max_value = 0x0000'7FFF'FFFF'FFFFull;

  // May block resolving a page fault (demand paging/swap-in) against
  // the *current* task's address space.
  static reloco::result<void> try_read(address_type addr, reloco::span<std::byte> dst) noexcept;
  static reloco::result<void> try_write(address_type addr, reloco::span<const std::byte> src) noexcept;

  // Must never block on a page fault: fail with reloco::error::page_fault
  // instead of resolving one (interrupt/NMI/spinlock-held context).
  static reloco::result<void> try_read_nofault(address_type addr, reloco::span<std::byte> dst) noexcept;
  static reloco::result<void> try_write_nofault(address_type addr, reloco::span<const std::byte> src) noexcept;
};
```

All four functions operate against whichever address space is "current";
none of them take an explicit process/VCPU handle. When declared, the
optional `min_value`/`max_value` bounds are checked by `is_in_range()`
(and internally before every materialize/store call) without ever calling
into `try_read`/`try_write` for an address known to be outside them.

## `_nofault` accessors

Every materialize/store operation comes in two flavors:

- The plain accessor (`try_materialize`, `try_store`, ...) may block
  resolving a page fault against the target address space (demand
  paging, swapped-out pages, lazily-populated guest memory, ...), exactly
  like an ordinary `copy_from_user`/`copy_to_user`.
- The `_nofault` accessor (`try_materialize_nofault`, `try_store_nofault`,
  ...) forbids that: if servicing the access would require taking a page
  fault, it fails immediately with `reloco::error::page_fault` instead of
  resolving it. This is the `copyin_nofault`/`copyout_nofault` (FreeBSD)
  or `pagefault_disable()`-wrapped `__get_user`/`__put_user` (Linux)
  shape -- use it from interrupt handlers, code already holding a
  spinlock, NMI/machine-check context, or anywhere else sleeping to page
  something in would be unsound.

## Materialize / store API

For a concrete (non-`void`) `T`:

- `try_materialize_to(span<T>)` / `try_materialize_to_nofault(span<T>)` --
  reads `buf.size()` elements into a caller-supplied buffer.
- `try_store(span<const T>)` / `try_store_nofault(span<const T>)`, plus a
  single-element `try_store(const T &)` / `try_store_nofault(const T &)`
  convenience overload.
- `try_materialize()` / `try_materialize_nofault()` -- reads and returns a
  single `T` by value.
- `try_materialize_ptr(alloc)` / `try_materialize_ptr_nofault(alloc)` --
  heap-allocates a `T` (via `reloco::unique_ptr<T>`) and reads into it;
  requires a default-constructible `T`.
- `try_as_bytes(alloc)` / `try_as_bytes_nofault(alloc)` -- copies
  `sizeof(T)` bytes into a fresh, immutable `reloco::bytes` snapshot.
- `try_as_bytes_mut(alloc)` / `try_as_bytes_mut_nofault(alloc)` -- copies
  `sizeof(T)` bytes into a fresh, mutable `reloco::bytes_mut` snapshot.

These are unavailable for `target_ptr<void, ...>` (`cast_type<U>()` to a
concrete type first) and require `T` to be `void` or trivially copyable,
since materialize/store memcpy raw bytes across a trust boundary.

Internally, all of them read through a small scratch buffer that stays on
the stack for "small" `T` (up to 32 bytes -- the common case for
syscall-sized arguments and structs) and falls back to a short-lived heap
allocation for anything larger, so a big, caller-controlled-size `T` can
never blow up a caller's stack frame.

## Comparisons

All six comparison operators (`==`, `!=`, `<`, `<=`, `>`, `>=`) are
defined, but only between two `target_ptr`s instantiated with the exact
same `T`/`SpaceTag`/`PtrType` -- there is no cross-space overload to
accidentally call, so mixing e.g. a `user_space` pointer with a
`kernel_space` one is a compile error, not a latent logic bug.

## Rust-flavored pointer arithmetic

Unlike a raw `T *`, `target_ptr` never silently wraps or invokes undefined
behavior on overflow: there is no `operator+`/`operator-` at all. Instead:

- `checked_add(n)` / `checked_sub(n)` -- fail with
  `reloco::error::integer_overflow` instead of wrapping.
- `wrapping_add(n)` / `wrapping_sub(n)` -- well-defined modulo-2^N
  wraparound, always succeed.
- `saturating_add(n)` / `saturating_sub(n)` -- clamp to the representable
  `address_type` range instead of overflowing.
- `checked_offset_from(origin)` -- Rust's `offset_from`: the signed
  element-wise distance from `origin` to `*this`, failing with
  `reloco::error::invalid_argument` if the byte distance isn't an exact
  multiple of `element_size`, or `reloco::error::integer_overflow` if it
  doesn't fit `difference_type`.

These mirror `<reloco/int_ops.hpp>`'s `checked_add`/`wrapping_add`/
`saturating_add` family and Rust's own `<*const T>::checked_add`/
`wrapping_add`/... -- the caller states up front what should happen on
overflow, rather than relying on whatever the compiler's optimizer
decided was convenient for a well-defined program.

## Zero overhead

`target_ptr<T, SpaceTag, PtrType>` is exactly `sizeof(PtrType)` (default
`std::uintptr_t`) with standard layout, regardless of `T` or `SpaceTag` --
it carries no extra state beyond the raw address.

See also: [`phys_addr.md`](phys_addr.md) for the physical-address-space
counterpart.
