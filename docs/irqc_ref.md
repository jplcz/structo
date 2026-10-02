<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::irqc_ref`

`include/structo/hw/irqc_ref.hpp`

A type-erased, non-owning handle over the typical operations of a
hardware interrupt controller (PIC) -- enabling/disabling an interrupt
source, arming its trigger/polarity and handler, mapping a bus-specific
interrupt specifier onto a registered source, and querying affinity/
capabilities -- plus `irq_source` (the per-line bookkeeping object a
concrete controller registers one of per interrupt line) and
`irq_map_data` (the generic "how do I route this raw specifier" payload
passed to `map_intr`).

## Modeled after FreeBSD's `INTRNG`

This header's shape is deliberately a simplified, allocation-free port of
FreeBSD's interrupt-framework design (`sys/kern/subr_intr.c`,
`sys/sys/intr.h`, the `PIC_IF` KOBJ interface every arm64/riscv PIC
driver implements) rather than a new invention:

| FreeBSD | This header |
|---|---|
| `struct intr_irqsrc` | `irq_source` |
| `struct intr_map_data` (`_fdt`/`_acpi`/`_msi` variants) | `irq_map_data` (`fdt`/`msi`/`gsi` variants) |
| `PIC_ENABLE_INTR`/`PIC_DISABLE_INTR` | `irqc_ref::enable_intr`/`disable_intr` |
| `PIC_SETUP_INTR`/`PIC_TEARDOWN_INTR` (plus MI `intr_irq_add_handler`) | `irqc_ref::setup_intr`/`teardown_intr` |
| `PIC_MAP_INTR` | `irqc_ref::map_intr` |
| `PIC_BIND` (`assign_cpu`) | `irqc_ref::assign_cpu` |
| `PIC_PRE_ITHREAD`/`PIC_POST_ITHREAD`/`PIC_POST_FILTER` | `irqc_ref::pre_ithread`/`post_ithread`/`post_filter` |
| *(not a FreeBSD `INTRNG` concept)* | `irqc_ref::assign_security_domain`/`send_ipi` |

What is deliberately *not* ported: FreeBSD's ithread scheduling itself
(there is no kernel thread here to schedule -- `pre_ithread`/
`post_ithread`/`post_filter` are exposed purely as the three mask/
EOI-timing hooks a level-triggered-aware caller's own dispatch loop may
need, around whatever it does instead of scheduling a thread), the MI
interrupt-handler chain (one `irq_source` here has at most one handler,
not FreeBSD's shared-IRQ filter+ithread chain -- a caller needing IRQ
sharing composes that itself on top, e.g. by registering one
`irqc_ref`-level handler that fans out to several subscribers), and
ACPI-specific mapping payloads (only the bus-agnostic "flat GSI number"
shape `irq_map_data::gsi` is kept).

## Why `irq_source` is composed into a backend's own type, not derived from

A concrete controller backend (a GICv3 distributor driver, an x86
IOAPIC/LAPIC pair, a RISC-V PLIC) almost always needs its own extra
per-line bookkeeping beyond what `irq_source` tracks (a GIC's raw
`INTID`, an IOAPIC's redirection-table index, ...). The natural C++ way
to add that is public inheritance (`struct gic_irq_line : irq_source {
std::uint32_t intid; };`), but `structo` deliberately prefers composition
over inheritance for this kind of "caller adds fields on top" extension
-- exactly the same choice [`callout.hpp`](callout.md) makes by holding
an `async_kernel_object` *member* rather than deriving from it -- since
it keeps `irq_source` a small, non-polymorphic, final value type (no
vtable, no slicing hazard, no virtual-destructor tax for something that
is never heap-allocated through a base pointer) and keeps every
backend's extra state visibly local to its own struct instead of
silently inherited. A concrete backend instead **embeds** an
`irq_source` member and hands out references to it:

```cpp
struct gic_irq_line {
  structo::hw::irq_source source; // common bookkeeping, composed in
  std::uint32_t intid;            // GIC-specific: the raw INTID

  explicit gic_irq_line(std::uint32_t irq, std::uint32_t intid_) noexcept
      : source(irq), intid(intid_) {}
};
```

`irqc_traits<Backend>::map_intr` decodes an `irq_map_data` into whichever
`gic_irq_line` it names, and returns a reference to that line's `.source`
member -- every other `irqc_traits` operation then takes/returns plain
`irq_source &`, so the backend's own extra fields never need to appear
in `irqc_ref`'s type-erased surface at all.

## Security domains and domain-targeted IPIs (ARM TrustZone-inspired)

Real-world PICs on TrustZone-capable cores (every ARM GICv2/v3) are not
a single flat namespace of interrupts: each source additionally belongs
to a *security domain* (GIC "Group 0"/secure, "Group 1"/non-secure, and
-- on an Arm CCA/RME-aware GIC -- further realm partitions), and
software running in one domain cannot unmask or redirect a source that
belongs to another. `irq_security_domain` is `structo`'s generalization
of that: a thin, type-safe wrapper around a single backend-defined
`std::uint8_t` domain identifier -- deliberately *not* a fixed
secure/non-secure `enum class`, since real controllers disagree on how
many domains exist and what each one means. A caller defines its own
named constants for whatever domains its platform actually has:

```cpp
namespace my_platform {
inline constexpr auto secure_domain = structo::hw::irq_security_domain{0};
inline constexpr auto non_secure_domain = structo::hw::irq_security_domain{1};
}
```

Every `irq_source` now carries one (`irq_source::security_domain()`,
default domain `0`). An `irqc_traits<Backend>` may optionally implement:

- `assign_security_domain(Backend &, irq_source &, irq_security_domain)`
  -- moves a source between domains at the controller (e.g. flipping a
  GIC's `GICD_IGROUPR`/`GICD_IGRPMODR` bit for that source's INTID).
- `send_ipi(Backend &, reloco::span<const std::uint64_t> target_mask, irq_security_domain, std::uint32_t ipi_id)`
  -- sends a domain-targeted inter-processor interrupt (a GIC Software
  Generated Interrupt, SGI) to every logical CPU set in `target_mask`.
  The mask is a `span` over raw bitmask words (word `i` holds CPUs
  `[i * 64, i * 64 + 64)`) -- the same raw, allocation-free IPI-target
  convention [`asid_allocator.hpp`](asid_allocator.md)'s own TLB
  shootdown guide uses, generalized to any CPU count instead of a
  per-CPU loop. `structo::arch::cpu_mask<Tag, MaxCpus>::words()` decays
  directly into it:
  ```cpp
  structo::arch::cpu_mask<my_physical_cpu_tag, 128> targets;
  targets.set(0);
  targets.set(3);
  ref.send_ipi(targets.words(), my_platform::secure_domain, ipi_id);
  ```
  A backend with hardware target-list/affinity-routing support (e.g. a
  GICv2 CPU target list, GICv3 `1-of-N`/list targeting) can turn this
  into a single broadcast write instead of one call per target CPU.

Both are independently optional, exactly like `assign_cpu`: a backend
with no TrustZone/security-domain concept at all simply does not
implement them, and every caller through them fails with
`error::unsupported_operation`. `irqc_capabilities::supports_security_domains`/`supports_ipi`
report whether a bound backend implements each.

## Why `structo::hw`

Like `uart_ref`/`timer_ref`/`hw_rng_ref`, `irqc_ref` lives in
`structo::hw`: device-*level* behavioral interfaces -- "this is an
interrupt controller, here's what it does" -- as opposed to the
lower-level register-access layer a concrete backend is commonly, but
not necessarily, built on top of.

## Why type-erased, like `uart_ref`/`timer_ref`

Exactly as `uart_ref`/`timer_ref` erase their concrete backend behind a
small, fixed vtable (a two-word handle: an untyped context pointer plus a
`const vtable *`, no virtual base class, no RTTI, no allocation of its
own), `irqc_ref` erases the concrete PIC backend the same way. This lets
generic interrupt-management code be written once against `irqc_ref` and
handed whatever concrete controller a given platform/test actually has,
decided at runtime.

## Rust-inspired API shape

- **`irqc_ref::mask_scoped(src)` is a `[[nodiscard]]` RAII scope guard**,
  exactly the same shape `sync::irq_guard`/`sync::preemption_guard` use
  for *global* interrupt/preemption disable: it masks `src` for its
  lifetime (recording whether it was actually enabled beforehand) and
  restores that prior enabled state on destruction -- the "scope guard"
  idiom (Rust's `scopeguard` crate; C++'s own `std::lock_guard`) applied
  to a single interrupt line instead of a whole critical section, for a
  caller that needs to briefly quiesce just one source (e.g. while
  reconfiguring a shared data structure its handler touches) without the
  sledgehammer of disabling every interrupt globally.
- **`result<T>`/`error` throughout**, matching Rust's `Result<T, E>` --
  every fallible operation returns one, including the optional
  operations a backend may not implement
  (`error::unsupported_operation`), exactly like every other `*_ref`
  handle in `structo`.

## Customizing: `irqc_traits<Backend>`

Left undefined for any `Backend` that hasn't opted in, mirroring
`uart_traits`/`timer_traits`/`hw_rng_traits`. A specialization must
supply exactly five mandatory functions:

```cpp
template <> struct structo::hw::irqc_traits<my_pic> {
  static reloco::result<void> enable_intr(my_pic &, structo::hw::irq_source &) noexcept;
  static reloco::result<void> disable_intr(my_pic &, structo::hw::irq_source &) noexcept;
  static reloco::result<void> setup_intr(my_pic &, structo::hw::irq_source &, structo::hw::irq_config,
                                         reloco::function_ref<void(structo::hw::irq_source &)>) noexcept;
  static reloco::result<void> teardown_intr(my_pic &, structo::hw::irq_source &) noexcept;
  static reloco::result<std::reference_wrapper<structo::hw::irq_source>>
  map_intr(my_pic &, const structo::hw::irq_map_data &) noexcept;
};
```

Optionally, a backend may also supply any of `assign_cpu` (IRQ affinity,
FreeBSD's `PIC_BIND`), `assign_security_domain`/`send_ipi` (TrustZone-style
security-domain switching and domain-targeted IPIs, see above),
`pre_ithread`/`post_ithread`/`post_filter` (the three mask/EOI-timing
hooks around however the caller's own dispatch loop handles a
level-triggered source), and `capabilities`:

```cpp
static reloco::result<void> assign_cpu(my_pic &, structo::hw::irq_source &, std::size_t cpu) noexcept;
static reloco::result<void> assign_security_domain(my_pic &, structo::hw::irq_source &,
                                                   structo::hw::irq_security_domain) noexcept;
static reloco::result<void> send_ipi(my_pic &, reloco::span<const std::uint64_t> target_mask,
                                     structo::hw::irq_security_domain, std::uint32_t ipi_id) noexcept;
static reloco::result<void> pre_ithread(my_pic &, structo::hw::irq_source &) noexcept;
static reloco::result<void> post_ithread(my_pic &, structo::hw::irq_source &) noexcept;
static reloco::result<void> post_filter(my_pic &, structo::hw::irq_source &) noexcept;
static reloco::result<structo::hw::irqc_capabilities> capabilities(my_pic &) noexcept;
```

Each is detected independently via the same optional-member SFINAE idiom
`timer_traits::remaining`/`capabilities` use; if a given one is absent,
the matching `irqc_ref` method fails with `error::unsupported_operation`.

## `irq_source`

Per-line bookkeeping for one registered interrupt source -- FreeBSD's
`struct intr_irqsrc` -- tracking its controller-local IRQ number,
negotiated `irq_config` (trigger/polarity), enabled state, CPU affinity,
security domain (`irq_security_domain`, see above), and at most one
registered handler (`set_handler`/`clear_handler`, invoked via
`dispatch()`).

Never copyable or movable: an `irq_source` is registered by address
(`irqc_ref::map_intr`/`setup_intr` hand out and operate on references to
a fixed instance), exactly like `async_kernel_object`'s own identity
requirement.

`set_handler`'s callable has the same non-owning lifetime caveat as
`timer_ref::set_callback`'s: the referenced callable must remain valid
for as long as it stays registered (until `clear_handler()`/the source's
own destruction), not merely until the end of the calling statement.

## `irq_map_data` / `irq_map_kind`

A generic, bus-agnostic payload describing how to route a raw interrupt
specifier to a registered `irq_source` -- FreeBSD's `struct
intr_map_data`. Three named constructors build one from the bus-specific
shape it actually has:

- `irq_map_data::try_from_fdt(reloco::span<const std::uint32_t> cells)`
  -- raw `interrupts`-property specifier cells pulled from a devicetree
  blob (see `fdt_reader.hpp`/`fdt_index.hpp` for extracting them in the
  first place); fails with `error::out_of_range` if `cells` has more
  than `max_fdt_interrupt_cells` (`4`) entries.
- `irq_map_data::from_msi(std::uint32_t vector)` -- a message-signaled-
  interrupt vector/message number.
- `irq_map_data::from_gsi(std::uint32_t gsi)` -- a flat, globally-unique
  interrupt number with no parent-relative cells (an ACPI Global System
  Interrupt, an x86 IOAPIC pin).

## `irq_config` / `irq_trigger` / `irq_polarity`

The trigger (`conform`/`edge`/`level`) and polarity
(`conform`/`active_high`/`active_low`) pair a source is armed with --
FreeBSD's `INTR_TRIGGER_*`/`INTR_POLARITY_*` constants, kept as two
separate enums exactly as FreeBSD does.

## `irqc_capabilities`

The typical static capabilities of an interrupt controller backend:
`max_sources`, which `irq_map_data` kinds `map_intr` can resolve
(`supports_fdt_mapping`/`supports_msi_mapping`/`supports_gsi_mapping`),
whether `assign_cpu` is implemented (`supports_affinity`), whether
`assign_security_domain`/`send_ipi` are implemented
(`supports_security_domains`/`supports_ipi`), and whether the controller
has per-CPU-private sources in addition to shared ones
(`supports_percpu_sources`, e.g. a GIC's banked PPIs/SGIs vs. its shared
SPIs) -- mirroring `timer_capabilities`'s own role for `timer_ref`.

## Generic conveniences

Synthesized purely from the mandatory operations, needing no further
backend support:

- `mask_scoped(irq_source &src)` -- returns a `scoped_mask` RAII guard;
  see "Rust-inspired API shape" above.

An unbound (default-constructed) `irqc_ref` fails every operation with
`error::unsupported_operation`, mirroring `uart_ref`/`timer_ref`'s
null-safety convention.

## Example

```cpp
struct my_gic { /* ... GICD_*/GICR_* MMIO, ... */ };

template <> struct structo::hw::irqc_traits<my_gic> {
  // ... five mandatory functions, plus optionally assign_cpu/
  // pre_ithread/post_ithread/post_filter/capabilities ...
};

void arm_uart_irq(structo::hw::irqc_ref gic, structo::hw::irq_source &uart_irq,
                   reloco::function_ref<void(structo::hw::irq_source &)> on_rx) {
  gic.setup_intr(uart_irq, {structo::hw::irq_trigger::level, structo::hw::irq_polarity::active_high}, on_rx);
}

my_gic distributor;
auto mapped = structo::hw::irqc_ref(distributor).map_intr(structo::hw::irq_map_data::from_gsi(33));
```

See also: [`timer_ref.md`](timer_ref.md), [`uart_ref.md`](uart_ref.md)
(the other `structo::hw` type-erased device-level handles),
[`callout.md`](callout.md) (the composition-over-inheritance precedent
`irq_source` follows).
