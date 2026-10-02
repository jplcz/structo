// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "monotonic_allocator.hpp"
#include "panic.hpp"

#include <reloco/allocator.hpp>
#include <reloco/default_allocator.hpp>
#include <reloco/stack_allocator.hpp>

#include <cstddef>

namespace {

// 256 KiB is far more than this demo's handful of allocations (the
// `reloco::default_allocator()` override below, plus `kmain.cpp`'s
// "boot proof" object) ever need -- generous headroom over tightness,
// since this allocator never frees anything back.
constexpr std::size_t heap_size = 256 * 1024;

// A single, process-wide arena shared by both `operator new` below and
// `reloco::default_allocator()`'s override (see `monotonic_allocator.hpp`).
// A function-local `static` (rather than a namespace-scope `constinit`
// object) deliberately: `reloco::stack_allocator_context`'s constructor
// takes a `void*`, and converting back from `void*` to `std::byte*`
// inside it is not usable in a constant expression before C++26 (see
// P2738), so it cannot be `constinit`-initialized under this demo's
// `-std=c++20`. A function-local static's lazy, guarded initialization
// sidesteps that entirely -- the guard itself is a plain, non-atomic
// flag (see this demo's `-fno-threadsafe-statics`, in `CMakeLists.txt`:
// there is exactly one CPU core and no threads here, so the usual
// `__cxa_guard_acquire`/`release` machinery -- unavailable anyway in
// this `-nostdlib` build -- would be pure overhead).
reloco::stack_allocator &heap_allocator() noexcept {
  alignas(std::max_align_t) static std::byte heap[heap_size];
  static reloco::stack_allocator alloc(reloco::stack_allocator_context(heap, heap_size));
  return alloc;
}

void *monotonic_alloc(std::size_t size, std::size_t align) noexcept {
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  auto block = heap_allocator().ref().allocate(size, align);
  RELOCO_END_UNSAFE_BUFFER_USAGE
  if (!block)
    baremetal_panic("monotonic_alloc", __FILE__, __LINE__, "monotonic allocator arena exhausted");
  return block->ptr;
}

} // namespace

// --- `reloco::default_allocator()` override (RELOCO_DEFAULT_ALLOCATOR_CUSTOM) ---
// See `reloco/default_allocator.hpp`'s own doc comment for the override
// contract this must satisfy: exactly one definition of this exact
// out-of-line signature, reachable by every translation unit that calls
// `reloco::default_allocator()`, which this single `.cpp` file provides.
reloco::allocator_ref reloco::reloco_global_alloc::default_allocator() noexcept { return heap_allocator().ref(); }

// --- Global operator new/delete (plain C++ `new`-expressions) ---

void *operator new(std::size_t size) { return monotonic_alloc(size, alignof(std::max_align_t)); }

void *operator new[](std::size_t size) { return monotonic_alloc(size, alignof(std::max_align_t)); }

void operator delete(void *) noexcept {
  // Monotonic: individual blocks are never reclaimed. The whole arena
  // could be dropped at once (`heap_allocator().context()->reset()`),
  // but this demo never does -- it halts long before that would matter.
}

void operator delete[](void *) noexcept {
  // See operator delete(void*) above.
}

void operator delete(void *, std::size_t) noexcept {
  // See operator delete(void*) above.
}

void operator delete[](void *, std::size_t) noexcept {
  // See operator delete(void*) above.
}
