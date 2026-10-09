<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::dynamic_bitmap`

`include/structo/dynamic_bitmap.hpp`

A bitmap whose bit count is a runtime value (for example a page count
derived from boot-time-probed RAM size). It owns a heap-allocated
`unsigned long[]` obtained directly from a `reloco::allocator_ref`.
There is no growth, no capacity-vs-size distinction and no element
construction to track, so it does not build on `reloco::vector`.

Construction is fallible, following `reloco`'s fallible-construction
convention: `try_allocate()` and `try_create()` return a `result`. The
type is **move-only**. A move transfers the buffer and leaves the source
empty (`nbits() == 0`). Copying is deleted. Use `try_clone()` when you
really want a copy.

The whole per-bit / scan / atomic API comes from the `bitmap_ops` CRTP
base, documented under [`fixed_bitmap`](fixed_bitmap.md#inherited-api-bitmap_opsfixed_bitmapn).
For a compile-time size and no allocator use [`fixed_bitmap<N>`](fixed_bitmap.md).
To wrap storage you do not own use [`bitmap_view`](bitmap_view.md).

## Usage

```cpp
// try_create(nbits) allocates from reloco::default_allocator(), and
// `nbits` is the logical number of bits (here: one per physical page).
// All bits start clear. Allocation can fail, so check the result.
auto maker = structo::dynamic_bitmap::try_create(num_pages);
if (!maker) {
  panic("out of memory sizing the page bitmap");
}
// Move the bitmap out of the result; the type is move-only.
auto pages = std::move(maker.value());

// Claim the lowest free page: scans for the lowest clear bit, sets it and
// returns its index (empty optional if every page is taken). Not atomic;
// use atomic_find_and_set() when sharing without a lock.
auto page = pages.find_and_set();
if (page) {
  use_page(*page);
  pages.clear(*page); // release; the argument is the bit index
}

// Use a specific allocator instead of the default one.
// First argument: the allocator to take storage from; second: bit count.
auto r = structo::dynamic_bitmap::try_allocate(my_allocator, 4096);

// Explicit, fallible copy. With no argument it reuses this bitmap's own
// allocator; pass an allocator_ref to place the clone elsewhere.
auto copy = pages.try_clone();
```

## API

| Member | Description |
|---|---|
| `explicit dynamic_bitmap(allocator_ref alloc = default_allocator())` | Constexpr, never fails. Makes an empty bitmap with `nbits() == 0` and no storage, bound to `alloc`. |
| `static try_allocate(allocator_ref alloc, std::size_t nbits)` | Returns `result<dynamic_bitmap>`, with all bits clear. Fails with `error::allocation_failed` if `alloc` cannot supply the storage. `nbits == 0` succeeds without allocating. |
| `static try_create(std::size_t nbits)` | `try_allocate()` with `default_allocator()`. |
| `try_clone(allocator_ref alloc)` | Fallible copy whose storage comes from `alloc`. |
| `try_clone()` | Fallible copy using this bitmap's own allocator. |
| move ctor / move assign | Transfer the buffer. The source becomes empty. A move-assign first releases the destination's existing buffer. |
| copy ctor / copy assign | Deleted. |
| `~dynamic_bitmap()` | Returns the buffer to the allocator. |
| `words()` | Mutable / const `span<unsigned long>` over the backing words. |
| `nbits()` | Runtime logical bit count. |

Every method of `bitmap_ops` is also available (`set`, `test`,
`find_and_set`, the range and run scans, the atomic variants, `|=` and
friends, iteration). Unlike `fixed_bitmap`, there are no value-returning
set-algebra operators: use the in-place `|=`, `&=`, `^=`, `-=` and
`invert()` on a clone.
