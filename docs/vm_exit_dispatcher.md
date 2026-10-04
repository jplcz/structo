<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `vm_exit_dispatcher<Handlers, MaxReasons>`

`include/structo/hypervisor/vm_exit_dispatcher.hpp`

A thin VM-exit trap dispatcher forwarding to a compile-time-resolved
`Handlers` trait, plus optional lock-free per-reason exit counters --
mirroring `arch/ipi_dispatcher.hpp`'s philosophy ("IPIs are boot-time-
fixed, never configured dynamically") for VM exits: dispatch logic
lives entirely in `Handlers::invoke()`, never in a runtime-mutable
registration table, because trap-context code cannot tolerate a nested
trap contending for a lock protecting such a table.

```cpp
struct my_vcpu_exit_context { /* ... */ };

struct my_vm_exit_handlers {
  using context_type = my_vcpu_exit_context;
  static reloco::result<void> invoke(std::uint32_t reason, context_type &ctx) noexcept {
    switch (reason) {
      case VMX_EXIT_REASON_CPUID: return handle_cpuid(ctx);
      case VMX_EXIT_REASON_HLT: return handle_hlt(ctx);
      default: return reloco::unexpected(reloco::error::not_supported);
    }
  }
};

using my_vm_exits = structo::hypervisor::vm_exit_dispatcher<my_vm_exit_handlers, 64>;

const reloco::result<void> outcome = my_vm_exits::dispatch(read_exit_reason(), ctx);
```

## API

- `using context_type = typename Handlers::context_type;`
- `static reloco::result<void> dispatch(std::uint32_t reason, context_type &ctx) noexcept` --
  forwards to `Handlers::invoke(reason, ctx)`, then, if `reason <
  MaxReasons`, increments that reason's counter (lock-free,
  `std::atomic<std::uint64_t>`, relaxed ordering). An out-of-range
  `reason` is still forwarded to `Handlers::invoke()` -- it is only the
  counting that is skipped.
- `static std::uint64_t count(std::uint32_t reason) noexcept` --
  checked: traps (via `reloco::array::operator[]`'s own assert) if
  `reason >= MaxReasons`.
- `static reloco::result<std::uint64_t> try_count(std::uint32_t reason) noexcept` --
  fallible counterpart, `error::out_of_bounds` instead of trapping.

`reason` is `std::uint32_t` (matching VMX exit-reason, Arm `ESR_EL2`,
and RISC-V `scause` widths), which converts to `reloco::array`'s
`size_t` index without narrowing on any realistic target.
