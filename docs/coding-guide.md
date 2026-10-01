<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# Coding guide

Conventions for contributing to `jplcz_structo`'s public headers, examples,
and tests. See [docs/reference.md](reference.md) for the per-header API map
and the [README](../README.md) for the project's dependency-resolution
conventions.

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

`std::` containers are permitted in `tests/**` (host-only, linked against a
full C++ standard library and GoogleTest already) where bare-metal/
allocation-free constraints do not apply; prefer them there when they are
simpler than the matching `reloco::` type for a test's purposes. Plain
value types, `std::size_t`/fixed-width integers, `std::pair`/`std::tuple`
of trivial types, and standard algorithms/type-traits headers are fine
everywhere -- this rule is specifically about *container* types that own
or bounds-check a buffer of elements.

## Prefer `TEST_F`/`TEST_P` for new GoogleTest tests

New test files should prefer `TEST_F` (fixture-based) or `TEST_P`
(parameterized) over a plain `TEST(...)`, even for a single test case,
so shared setup/teardown and fixture state have an obvious place to grow
into as a header's test coverage expands. This is a convention for *new*
tests going forward; it is not a mandate to retrofit every existing
`TEST(...)` in `tests/**`.

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
