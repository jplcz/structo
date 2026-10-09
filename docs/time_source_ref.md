<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::time_source_ref`

`include/structo/hw/time_source_ref.hpp`

A type-erased, non-owning handle over a free-running hardware counter (x86
TSC, ARM `CNTVCT_EL0`, RISC-V `mtime`/`rdtime`, ...), plus the
`time_source_traits<Backend>` customization point.

It is the complement of [`timer_ref`](timer_ref.md): a timer is armed and
fires; a time source is never armed and only answers "what is the raw count
now" via `try_now()`. That is what tickless timekeeping
([`time_manager`](time_manager.md), [`vm_time_manager`](vm_time_manager.md))
needs to correlate against a wall-clock reference. For plain elapsed time
with wrap handling, see also [`clock_ref`](clock_ref.md).

Unbound refs fail every call with `error::unsupported_operation`.

## Usage

```cpp
struct arm_cntvct_backend {};

template <> struct structo::hw::time_source_traits<arm_cntvct_backend> {
  // Sample the counter's raw value. Normally a single instruction; it
  // returns result<> only for consistency and for backends that can fail
  // (e.g. trapped away by a hypervisor).
  static reloco::result<structo::hw::cycles> try_now(arm_cntvct_backend &) noexcept {
    std::uint64_t val;
    asm volatile("mrs %0, cntvct_el0" : "=r"(val));
    return structo::hw::cycles{val};
  }

  // Static, infallible description of the counter; callers consult it
  // before reading.
  static structo::hw::time_source_capabilities capabilities(arm_cntvct_backend &) noexcept {
    std::uint64_t freq;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return {.clock_hz = freq,                         // counting frequency in Hz; 0 = unknown
                                                      // (disables duration conversion)
            .max_value = structo::hw::cycles{UINT64_MAX}, // largest raw value before wrapping to 0 (inclusive)
            .is_monotonic = true,                     // never runs backward on a given core
            .is_per_cpu = false};                     // one globally consistent count for all cores
  }
};

arm_cntvct_backend backend;
structo::hw::time_source_ref src{backend};    // explicit, non-owning: `backend` must outlive
                                              // src and its copies; rvalues are rejected

auto now = src.try_now();                     // result<cycles>
auto caps = src.capabilities();               // result<time_source_capabilities>
if (caps && !caps->is_per_cpu && caps->is_monotonic) {
  // Suitable as a single system-wide time source. The wrap period is
  // (max_value.raw() + 1) / clock_hz seconds.
}
```

## `time_source_capabilities`

| Field | Default | Meaning |
| --- | --- | --- |
| `clock_hz` | `0` | Counter frequency (pairs with `clock_cycles.hpp` conversions). |
| `max_value` | `cycles{UINT64_MAX}` | Largest raw value before wrapping to `0`. |
| `is_monotonic` | `true` | Never runs backward per read on the reading core. |
| `is_per_cpu` | `false` | Value is core-specific rather than globally consistent. |

Supports `==` and `!=`.

## API

| Member | Description |
| --- | --- |
| `time_source_ref()` | Unbound ref. |
| `explicit time_source_ref(Backend &)` | Binds a backend with `time_source_traits` (`try_now`, `capabilities`). Rvalues are deleted. |
| `explicit operator bool()` | Whether bound. |
| `try_now()` | `result<cycles>`; backend error or `unsupported_operation` if unbound. |
| `capabilities()` | `result<time_source_capabilities>`; `unsupported_operation` if unbound. |
