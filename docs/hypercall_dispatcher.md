<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `hypercall_dispatcher<Handlers, MaxCalls>`

`include/structo/hypervisor/hypercall_dispatcher.hpp`

A thin hypercall dispatcher for guest-initiated calls (`HVC`/`VMCALL`/
`SBI` ecalls), same shape as [`vm_exit_dispatcher.md`](vm_exit_dispatcher.md)
but keyed by a `std::uint64_t call_number` to match SMCCC-style
hypercall ABIs:

```cpp
struct my_hypercall_context { /* ... */ };

struct my_hypercall_handlers {
  using context_type = my_hypercall_context;
  static reloco::result<void> invoke(std::uint64_t call_number, context_type &ctx) noexcept {
    switch (call_number) {
      case PSCI_CPU_ON: return handle_cpu_on(ctx);
      default: return reloco::unexpected(reloco::error::not_supported);
    }
  }
};

using my_hypercalls = structo::hypervisor::hypercall_dispatcher<my_hypercall_handlers, 64>;

const reloco::result<void> outcome = my_hypercalls::dispatch(function_id, ctx);
```

## API

- `using context_type = typename Handlers::context_type;`
- `static reloco::result<void> dispatch(std::uint64_t call_number, context_type &ctx) noexcept` --
  forwards to `Handlers::invoke(call_number, ctx)`, then, if
  `call_number < MaxCalls`, increments that call number's counter
  (lock-free, `std::atomic<std::uint64_t>`, relaxed ordering). An
  out-of-range `call_number` is still forwarded to `Handlers::invoke()`
  -- only the counting is skipped.
- `static std::uint64_t count(std::uint64_t call_number) noexcept` --
  checked: traps if `call_number >= MaxCalls`.
- `static reloco::result<std::uint64_t> try_count(std::uint64_t call_number) noexcept` --
  fallible counterpart, `error::out_of_bounds` instead of trapping.

## Why this isn't just `vm_exit_dispatcher` with a wider type

`std::uint64_t -> std::size_t` is a narrowing conversion on 32-bit
hosts, which `-Wconversion -Werror` (enabled repo-wide) rejects --
unlike `vm_exit_dispatcher`'s `std::uint32_t` reason, which widens (or
stays equal) on every realistic target. `dispatch()`/`count()`/
`try_count()` therefore bounds-check `call_number` against `MaxCalls`
explicitly (widening `MaxCalls` to `uint64_t`, never narrowing) before
an explicit `static_cast<std::size_t>` into the backing
`reloco::array`, rather than relying on the array's own `operator[]`/
`try_at` to do it implicitly.
