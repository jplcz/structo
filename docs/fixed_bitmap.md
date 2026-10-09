<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::fixed_bitmap<N>`

`include/structo/fixed_bitmap.hpp`

A fixed-capacity bitmap of exactly `N` bits (a compile-time constant),
stored inline in the object as an `unsigned long` array. No allocator,
no `reloco::vector`: it is safe in interrupt/trap context or before any
allocator exists, and it is trivially copyable.

It is the generic counterpart of [`cpu_mask`](cpu_mask.md): the same
"array embedded in the object" storage and the same checked / `try_` /
`unsafe_` per-bit accessor convention, but untagged. It is meant for
"a fixed pool of N slots" bookkeeping rather than CPU-index domains, and
adds the free-slot scans (`lowest_clear*()`, `find_and_set*()`) that
`cpu_mask` does not need. [`slot_map_ptr`](slot_map_ptr.md) uses it for
its slot-busy tables.

`fixed_bitmap<N>` only supplies storage plus `words()`/`nbits()`. The
rest of the instance API comes from the `bitmap_ops<Derived>` CRTP base
(`include/structo/bitmap_ops.hpp`), shared with
[`dynamic_bitmap`](dynamic_bitmap.md) and
[`bitmap_view`](bitmap_view.md). All three therefore behave identically.

## Usage

```cpp
// A pool of 128 slots, stored inline. The template argument is the
// number of bits; the default constructor leaves every bit clear
// (clear == slot free).
structo::fixed_bitmap<128> slots;

// find_and_set() scans for the lowest CLEAR bit, sets it and returns its
// index. It returns an empty reloco::optional when the pool is full. It
// is NOT atomic: hold whatever lock protects `slots`, or use
// atomic_find_and_set() instead.
auto slot = slots.find_and_set();
if (slot) {
  use(*slot);        // *slot is the claimed index in [0, 128)
  slots.clear(*slot); // give it back; the argument is the bit index
                      // (traps via RELOCO_ASSERT if >= size())
}

// Value-returning set algebra: unlike |= and friends, these produce a new
// fixed_bitmap and leave the operands untouched.
structo::fixed_bitmap<128> reserved = structo::fixed_bitmap<128>::empty();
reserved.set(0); // bit 0 is permanently reserved
auto usable = slots.difference(reserved); // bits in `slots` not in `reserved`
auto all_ones = structo::fixed_bitmap<128>::filled(); // all 128 bits set,
                                                      // tail padding kept clear
```

## Members

| Member | Description |
|---|---|
| `bit_count` | `static constexpr std::size_t`, equal to `N`. |
| `word_count` | `static constexpr std::size_t`, number of `unsigned long` words backing `N` bits. |
| `fixed_bitmap()` | Constexpr, all bits clear. |
| `empty()` | Static; an all-clear bitmap (same as the default constructor). |
| `filled()` | Static; a bitmap with all `N` bits set (tail padding bits in the last word stay clear). |
| `words()` | Mutable / const `span<unsigned long>` over the backing words (the `bitmap_ops` contract). |
| `nbits()` | Static constexpr; returns `N`. |

### Set algebra returning a new bitmap

Each takes any `bitmap_ops`-based bitmap with the same bit count. A
mismatch in `nbits()` traps.

| Method / operator | Result |
|---|---|
| `union_with(other)` / `a \| b` | Bits set in either. |
| `intersection(other)` / `a & b` | Bits set in both. |
| `difference(other)` / `a - b` | Bits set in `*this` but not in `other`. |
| `symmetric_difference(other)` / `a ^ b` | Bits set in exactly one. |
| `complement()` / `~a` | Every bit flipped. |
| `a == b` / `a != b` | Bitwise equality. |

The operators take `fixed_bitmap` operands only. The named methods are
templates that accept any `bitmap_ops`-based type. These live in
`fixed_bitmap` rather than in `bitmap_ops` because building a fresh
value needs a freely copyable type, and `dynamic_bitmap` and
`bitmap_view` are not.

## Inherited API (`bitmap_ops<fixed_bitmap<N>>`)

All methods are `noexcept`. "Checked" forms trap through `RELOCO_ASSERT`
on an out-of-range index. `try_*` forms return `reloco::result<>`
instead. `unsafe_*` forms are unchecked, and using one with an index that
does not fit is undefined behaviour.

| Group | Methods |
|---|---|
| Size | `size()` (logical bit count) |
| Per-bit | `set`, `clear`, `test`, `toggle`, each with `try_*` and `unsafe_*` variants |
| Whole bitmap | `clear_all`, `fill`, `count`, `any`, `none`, `all`, `invert` |
| Scans | `lowest_set[_from]`, `lowest_clear[_from]`, `highest_set` |
| Claim a slot | `find_and_set[_from]` (non-atomic) |
| Ranges (inclusive `[start, stop]`) | `set_range`, `clear_range`, `count_range`, `all_set_in_range`, `all_clear_in_range`, each with `try_*` / `unsafe_*` variants |
| Runs | `lowest_clear_run[_from]`, `lowest_set_run[_from]`, `find_and_set_run[_from]` |
| Atomic | `atomic_test`, `atomic_set`, `atomic_clear`, `atomic_toggle` (each also `atomic_try_*` / `atomic_unsafe_*`, taking a `std::memory_order`, default `seq_cst`); `atomic_lowest_set[_from]`, `atomic_find_and_set[_from]` |
| Predicates | `contains(other)`, `intersects(other)`, `equals(other)` |
| In-place algebra | `\|=`, `&=`, `^=`, `-=` (all take another bitmap of equal `nbits()`) |
| Iteration | `begin()` / `end()`: forward iteration over set-bit indices, ascending |

```cpp
structo::fixed_bitmap<64> bits;
bits.set(3);
bits.set(40);
// Range-for yields the INDEX of each set bit, in ascending order.
for (std::size_t index : bits) {
  visit(index); // 3, then 40
}
```
