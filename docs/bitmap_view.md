<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::bitmap_view`

`include/structo/bitmap_view.hpp`

A non-owning bitmap over a caller-supplied `reloco::span<unsigned long>`
plus an explicit bit count. It exposes the same `bitmap_ops` instance API
as [`fixed_bitmap<N>`](fixed_bitmap.md) and
[`dynamic_bitmap`](dynamic_bitmap.md) (`set`, `test`, `find_and_set`,
atomic variants and so on) without copying or allocating anything.

Reach for it when the words already live somewhere you do not own. For
example, a bitmap embedded in a shared-memory or firmware-owned
structure, or a slice of a larger bitmap that a subsystem should only
partly see.

`bitmap_view` is deliberately **non-copyable and non-movable**, unlike a
typical view type such as `reloco::span`. A copy or move would create a
second handle to memory the view does not own, which invites dangling and
aliasing mistakes. Construct a fresh `bitmap_view` in each scope that
needs one, rather than passing an existing one around.

## Usage

```cpp
void scan_segment(reloco::span<unsigned long> words, std::size_t nbits) {
  // `words` is the backing storage and stays owned by the caller. It must
  // outlive the view, and access to it must follow whatever locking the
  // caller needs. `nbits` is the logical bit count; it must be
  // <= words.size() * bitmap_utils::bits_per_word. The last word may have
  // unused tail bits.
  structo::bitmap_view view(words, nbits);

  // The full bitmap_ops API works on the borrowed words. This finds the
  // lowest clear bit and sets it, returning its index or an empty
  // optional.
  auto free_slot = view.find_and_set();
  if (free_slot) {
    use(*free_slot);
  }

  // Passing the view along must be by reference, since copying and moving
  // are deleted.
  report(view);
}
```

## API

| Member | Description |
|---|---|
| `constexpr bitmap_view(span<unsigned long> words, std::size_t nbits)` | Wraps `words` as a bitmap of `nbits` bits. No validation of `nbits` against the span size is performed, so the caller must respect the limit above. |
| copy / move ctor and assignment | All deleted. |
| `words()` | Mutable / const `span<unsigned long>`; returns the span given at construction. |
| `nbits()` | The bit count given at construction. |

All inherited operations are documented in the
[`fixed_bitmap` inherited API section](fixed_bitmap.md#inherited-api-bitmap_opsfixed_bitmapn).
Because a view is not copyable, the value-returning algebra
(`union_with`, `operator|`, and so on) is only on `fixed_bitmap`. Use
the in-place operators (`|=`, `&=`, `^=`, `-=`) and `invert()` here.
