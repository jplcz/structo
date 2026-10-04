<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# Coding guide

Conventions for contributing to `jplcz_structo`'s public headers, examples,
and tests. See [docs/reference.md](reference.md) for the per-header API map
and the [README](../README.md) for the project's dependency-resolution
conventions.

`structo` is a consumer of [`jplcz_reloco`](https://github.com/jplcz/reloco)
and inherits its standards rather than defining its own in parallel. When in
doubt, follow `reloco`'s own documentation directly:

| Topic | `reloco` doc |
|---|---|
| Tri-tier (`checked`/`try_`/`unsafe_`) accessors, rvalue-access blocking, the full container template | [`container-contract.md`](https://github.com/jplcz/reloco/blob/master/docs/container-contract.md) |
| Why hardened/fallible containers exist; the `std::` -> `reloco::` type table | [`hardened-containers.md`](https://github.com/jplcz/reloco/blob/master/docs/hardened-containers.md) |
| `RELOCO_LIFETIMEBOUND`/`RELOCO_OWNER`/`RELOCO_POINTER`/`RELOCO_UNSAFE_BUFFER_USAGE` and the rest of the Clang/GCC safety-annotation macros | [`lifetime-safety.md`](https://github.com/jplcz/reloco/blob/master/docs/lifetime-safety.md) |
| `try_create`/factory-function patterns for types that cannot be default-constructed into a valid state | [`fallible-construction.md`](https://github.com/jplcz/reloco/blob/master/docs/fallible-construction.md) |
| `is_trivially_relocatable<T>` and why it matters for zero-overhead moves | [`relocatable.md`](https://github.com/jplcz/reloco/blob/master/docs/relocatable.md) |
| Sharing storage/dispatch logic behind a function-pointer-table engine resolved once per `T`/`Backend`, instead of duplicating it per instantiation | [`type-erased-base-containers.md`](https://github.com/jplcz/reloco/blob/master/docs/type-erased-base-containers.md) |

If a local path to a `reloco` checkout is available, the same docs live at
`$RELOCO_SOURCE_DIR/docs/*.md` (e.g.
`JPLCZ_STRUCTO_RELOCO_SOURCE_DIR/docs/container-contract.md`, see the
[README](../README.md)'s dependency-resolution section for how that variable
is resolved) -- prefer that copy if it may be newer than the link above.


## No `std::` containers in main code or examples

`include/structo/**` and `examples/**` must not reference non-trivial
`std::` containers -- `std::array`, `std::vector`, `std::string`,
`std::map`, `std::unordered_map`, `std::list`, `std::deque`, `std::set`,
and similar. Two reasons:

1. **Allocation-free by default.** structo targets kernel/hypervisor/
   trusted-boundary code where `std::vector`/`std::string`'s heap
   allocation is often simply unavailable (no working allocator yet at
   boot) or undesirable (unpredictable allocation in interrupt/trap
   context).
2. **No implicit libc dependency.** Several `std::` containers'
   bounds-checked accessors (e.g. `std::array::at`, `std::vector::at`,
   and some implementations' `operator[]` in debug/hardened modes) call
   through to `assert()`/`abort()`, pulling in glibc (or another libc)'s
   assert machinery -- exactly the kind of implicit host-OS dependency a
   header-only, bare-metal-targetable library needs to avoid.

Use `jplcz_reloco`'s equivalents instead, matched to the actual capacity/
ownership model needed:

| Need | Use instead of |
|---|---|
| `reloco::array<T, N>` | `std::array<T, N>` |
| `reloco::vector<T>` (allocator-backed), `reloco::inline_vector<T, N>` (fixed-capacity, no allocation), `reloco::external_vector<T>` (caller-owned storage), `reloco::outline_vector<T>`, `reloco::sso_vector<T>` | `std::vector<T>` |
| `reloco::string`, `reloco::inline_string<N>`, `reloco::sso_string` | `std::string` |
| `reloco::string_view` | `std::string_view` |
| `reloco::span<T>` | `std::span<T>` |
| `reloco::flat_map`, `reloco::inline_flat_map`, `reloco::sso_flat_map`, `reloco::flat_hash_map`, `reloco::tree_map` | `std::map`/`std::unordered_map` |
| `reloco::optional<T>` | `std::optional<T>` |

Tests should also avoid `std::` containers in favor of `reloco::` equivalents.
The same allocation-free, no-libc-dependency constraints apply to tests because
the test suite may run on bare-metal or with stripped-down C++ standard library
support. Plain value types, `std::size_t`/fixed-width integers,
`std::pair`/`std::tuple` of trivial types, and standard algorithms/type-traits
headers are fine everywhere -- this rule is specifically about *container* types
that own or bounds-check a buffer of elements.

## Prefer `TEST_F`/`TEST_P` for new GoogleTest tests

New test files should prefer `TEST_F` (fixture-based) or `TEST_P`
(parameterized) over a plain `TEST(...)`, even for a single test case,
so shared setup/teardown and fixture state have an obvious place to grow
into as a header's test coverage expands. This is a convention for *new*
tests going forward; it is not a mandate to retrofit every existing
`TEST(...)` in `tests/**`.

## Follow `reloco`'s memory-safe container patterns: the three-tier accessor system

This section summarizes the convention as applied in `structo`; see
`reloco`'s [`container-contract.md`](https://github.com/jplcz/reloco/blob/master/docs/container-contract.md)
for the authoritative copy-paste template/checklist and
[`hardened-containers.md`](https://github.com/jplcz/reloco/blob/master/docs/hardened-containers.md)
for the user-facing rationale -- `structo` follows these verbatim rather
than defining a competing convention.

Any class `structo` defines that owns, indexes into, or otherwise guards
access to data that *can* be invalid/absent/out-of-range (an empty slot,
an out-of-bounds index, a not-yet-initialized value, a moved-from
handle, ...) must follow the same tri-tier accessor convention
`jplcz_reloco`'s own containers use (see e.g. `reloco::optional<T>`,
`reloco::collection_view`, `reloco::cell`/`ref_cell`) rather than
inventing a different one. The three tiers, in the order callers should
reach for them:

1. **Checked (default).** The plain, unprefixed accessor (`value()`,
   `operator*`, `operator->`, `operator[]`, `get()`, ...) validates its
   precondition with `RELOCO_ASSERT`/`RELOCO_ASSERT_MSG` and traps
   (rather than invoking undefined behavior) if violated. This is what
   ordinary call sites should use.
2. **Fallible.** A `try_`-prefixed accessor (`try_value()`, `try_get()`,
   `try_push_back()`, ...) returns `reloco::result<T>` (or
   `reloco::optional<T>`/`reloco::result<std::reference_wrapper<T>>`
   where appropriate) instead of trapping, for call sites that need to
   handle the failure case as data rather than treat it as a
   programming-error bug.
3. **Unsafe.** An `unsafe_`-prefixed accessor (`unsafe_value()`,
   `unsafe_ptr()`, `unsafe_get()`, ...) skips the tier-1 check entirely
   (or downgrades it to a `RELOCO_DEBUG_ASSERT`, compiled out in
   release) for the rare hot-path call site that has already
   independently proven the precondition holds. It must be annotated
   `RELOCO_UNSAFE_BUFFER_USAGE` so Clang's `-Wunsafe-buffer-usage`
   flags any use of it, and its name must make the lack of a check
   obvious at the call site.

Any raw pointer/reference an accessor returns that aliases into the
object's own storage (rather than transferring ownership) must be
annotated `RELOCO_LIFETIMEBOUND` (from `reloco/lifetime.hpp`) so Clang
can flag a dangling use when the result outlives the object it was
borrowed from; a pointer that *does* transfer ownership should instead
be annotated `RELOCO_OWNER` at the point it is produced and
`RELOCO_POINTER`-style non-owning raw pointers used elsewhere, matching
`reloco`'s own usage of these annotations (see `reloco/lifetime.hpp` and
[`lifetime-safety.md`](https://github.com/jplcz/reloco/blob/master/docs/lifetime-safety.md)
for the full macro reference). A class exposing container-like,
`&`-aliasing accessors (`operator[]`, `front()`/`back()`, `data()`,
`begin()`/`end()`, or a `base()`-style escape hatch into an owned
container) should additionally ref-qualify those accessors `&`/`const &`
(or use `RELOCO_BLOCK_RVALUE_ACCESS`, from `reloco/rvalue_safety.hpp`, for
a type that exposes the exact member-name surface that macro targets) so
a borrow can never be taken from a temporary -- see `sg_list::base()` for
an example of the ref-qualified form.

Not every class needs all three tiers -- a storage-free resolver like
`per_cpu_ptr<Tag, T>` that only ever `static_cast`s an already-`void*`
slot has nothing to assert about and is exempt -- but any class that
*does* guard against an invalid/absent/out-of-range access must expose
at least the checked tier, and should add the fallible and/or unsafe
tiers once a real call site needs them (don't speculatively add an
`unsafe_` accessor nobody uses yet).

## Type-erase a `*_ref` handle's backend behind one `vtable`, not a template per call site

Every non-owning, runtime-polymorphic "erase the concrete backend"
handle in `structo` (`io_space_ref<SpaceTag>`, `hw::uart_ref`,
`hw::hw_rng_ref`, ...) follows the same shape `reloco`'s own vector
family uses for the same reason -- see
[`type-erased-base-containers.md`](https://github.com/jplcz/reloco/blob/master/docs/type-erased-base-containers.md)'s
"Layer 1" `vector_operations` table: a small `struct vtable` of plain
function pointers (`result<T> (*op)(void *ctx, ...) noexcept`),
resolved **once per concrete `Backend` type** via a
`template <typename Backend> static constexpr vtable s_vtbl`, never
re-resolved per call. The ref itself stores only a `void *ctx_` plus a
`const vtable *vtbl_` -- a two-word handle, no virtual base class, no
RTTI, no allocation -- and every public method is a short, non-template
forward through `vtbl_->op(ctx_, ...)`, falling back to
`error::unsupported_operation` when unbound (`vtbl_ == nullptr`).

This keeps per-`Backend` template bloat down to just the handful of
`*_entry<Backend>` trampoline functions and the `s_vtbl<Backend>` table
itself (mirroring `vector_operations`' `get_operations_for<T>()` and
`type_operations`' `get_type_operations_for<T>()`): every other method on
the ref -- any generic convenience built purely out of the mandatory
operations, like `uart_ref::write()`/`read_available()` or a future
`hw_rng_ref::try_fill()` -- is ordinary, non-template code compiled
once, not once per bound `Backend`. When adding a new `*_ref` handle,
follow `hw/uart_ref.hpp` as the concrete template (customization-point
trait with SFINAE-detected optional members, `vtable`, `s_vtbl<Backend>`,
unbound-safe forwarding methods) rather than inventing a new shape.

## Headers with a trait/policy template parameter must show an example

Any public header whose API is parameterized on a trait/policy type the
*caller* is expected to implement (e.g. `cpu_index<Tag>`,
`lazy_context<Traits>`, `irq_guard<Traits>`, `phys_translator<Policy>`,
`os_traits_base<Derived, OsPage>`) must include a complete, minimal,
compilable example implementation of that trait inside a Doxygen comment
block (an `@file` block or the class's own `@brief` block), not just a
prose description of the required member list. See
`include/structo/arch/lazy_context.hpp`'s `@file` block or
`include/structo/sync/irq_guard.hpp`'s `@file` block for the pattern.
`buddy_allocator.hpp`'s `free_list_archetype` (a declarations-only
"archetype" struct living in the header itself rather than a comment) is
an equally acceptable alternative when a declarations-only type can fully
capture the required shape.

