// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file irqc_ref.hpp
 * @brief `structo::hw::irqc_ref`: a type-erased, non-owning handle over
 * the typical operations of a hardware interrupt controller (PIC) --
 * enabling/disabling an interrupt source, arming its trigger/polarity
 * and handler, mapping a bus-specific interrupt specifier onto a
 * registered source, and querying affinity/capabilities -- plus
 * `irq_source` (the per-line bookkeeping object a concrete controller
 * registers one of per interrupt line) and `irq_map_data` (the generic
 * "how do I route this raw specifier" payload passed to `map_intr`).
 *
 * ## Modeled after FreeBSD's `INTRNG`
 *
 * This header's shape is deliberately a simplified, allocation-free
 * port of FreeBSD's interrupt-framework design (`sys/kern/subr_intr.c`,
 * `sys/sys/intr.h`, the `PIC_IF` KOBJ interface every arm64/riscv PIC
 * driver implements) rather than a new invention:
 *
 * | FreeBSD | This header |
 * |---|---|
 * | `struct intr_irqsrc` | `irq_source` |
 * | `struct intr_map_data` (`_fdt`/`_acpi`/`_msi` variants) | `irq_map_data` (`fdt`/`msi`/`gsi` variants) |
 * | `PIC_ENABLE_INTR`/`PIC_DISABLE_INTR` | `irqc_ref::enable_intr`/`disable_intr` |
 * | `PIC_SETUP_INTR`/`PIC_TEARDOWN_INTR` (plus MI `intr_irq_add_handler`) | `irqc_ref::setup_intr`/`teardown_intr` |
 * | `PIC_MAP_INTR` | `irqc_ref::map_intr` |
 * | `PIC_BIND` (`assign_cpu`) | `irqc_ref::assign_cpu` |
 * | `PIC_PRE_ITHREAD`/`PIC_POST_ITHREAD`/`PIC_POST_FILTER` | `irqc_ref::pre_ithread`/`post_ithread`/`post_filter` |
 *
 * What is deliberately *not* ported: FreeBSD's ithread scheduling itself
 * (there is no kernel thread here to schedule -- `pre_ithread`/
 * `post_ithread`/`post_filter` are exposed purely as the three
 * mask/EOI-timing hooks a level-triggered-aware caller's own dispatch
 * loop may need, around whatever it does instead of scheduling a
 * thread), the MI interrupt-handler chain (one `irq_source` here has at
 * most one handler, not FreeBSD's shared-IRQ filter+ithread chain -- a
 * caller needing IRQ sharing composes that itself on top, e.g. by
 * registering one `irqc_ref`-level handler that fans out to several
 * subscribers), and ACPI-specific mapping payloads (only the
 * bus-agnostic "flat GSI number" shape `irq_map_data::gsi` is kept).
 *
 * ## Why `irq_source` is composed into a backend's own type, not derived from
 *
 * A concrete controller backend (a GICv3 distributor driver, an x86
 * IOAPIC/LAPIC pair, a RISC-V PLIC) almost always needs its own extra
 * per-line bookkeeping beyond what `irq_source` tracks (a GIC's raw
 * `INTID`, an IOAPIC's redirection-table index, ...). The natural C++
 * way to add that is public inheritance (`struct gic_irq_line : irq_source
 * { std::uint32_t intid; };`), but `structo` deliberately prefers
 * composition over inheritance for this kind of "caller adds fields on
 * top" extension -- exactly the same choice `callout.hpp` makes by
 * holding an `async_kernel_object` *member* rather than deriving from
 * it (see that header's own file-level docs) -- since it keeps
 * `irq_source` a small, non-polymorphic, final value type (no vtable,
 * no slicing hazard, no virtual-destructor tax for something that is
 * never heap-allocated through a base pointer) and keeps every
 * backend's extra state visibly local to its own struct instead of
 * silently inherited. A concrete backend instead **embeds** an
 * `irq_source` member and hands out references to it:
 *
 * @code
 * struct gic_irq_line {
 *   structo::hw::irq_source source; // common bookkeeping, composed in
 *   std::uint32_t intid;            // GIC-specific: the raw INTID
 *
 *   explicit gic_irq_line(std::uint32_t irq, std::uint32_t intid_) noexcept
 *       : source(irq), intid(intid_) {}
 * };
 * @endcode
 *
 * `irqc_traits<Backend>::map_intr` decodes an `irq_map_data` into
 * whichever `gic_irq_line` it names, and returns a reference to that
 * line's `.source` member -- every other `irqc_traits` operation then
 * takes/returns plain `irq_source &`, so the backend's own extra fields
 * never need to appear in `irqc_ref`'s type-erased surface at all.
 *
 * ## Rust-inspired API shape
 *
 * - **`irqc_ref::mask_scoped(src)` is a `[[nodiscard]]` RAII scope
 *   guard**, exactly the same shape `sync::irq_guard`/
 *   `sync::preemption_guard` use for *global* interrupt/preemption
 *   disable: it masks @p src for its lifetime (recording whether it was
 *   actually enabled beforehand) and restores that prior enabled state
 *   on destruction -- the "scope guard" idiom (Rust's `scopeguard`
 *   crate; C++'s own `std::lock_guard`) applied to a single interrupt
 *   line instead of a whole critical section, for a caller that needs
 *   to briefly quiesce just one source (e.g. while reconfiguring a
 *   shared data structure its handler touches) without the sledgehammer
 *   of disabling every interrupt globally.
 * - **`result<T>`/`error` throughout**, matching Rust's `Result<T, E>`
 *   (see `uart_ref.hpp`/`timer_ref.hpp`'s own file-level docs for why
 *   this library uses `reloco::result` instead of exceptions
 *   everywhere): every fallible operation returns one, including the
 *   optional operations a backend may not implement
 *   (`error::unsupported_operation`), exactly like every other `*_ref`
 *   handle in `structo`.
 *
 * ## Customizing: `irqc_traits<Backend>`
 *
 * Left undefined for any `Backend` that hasn't opted in, mirroring
 * `uart_traits`/`timer_traits`/`hw_rng_traits`. A specialization must
 * supply exactly five functions -- the mandatory "abstract operations"
 * contract:
 *
 * @code
 * template <> struct structo::hw::irqc_traits<my_pic> {
 *   static reloco::result<void> enable_intr(my_pic &, structo::hw::irq_source &) noexcept;
 *   static reloco::result<void> disable_intr(my_pic &, structo::hw::irq_source &) noexcept;
 *   static reloco::result<void> setup_intr(my_pic &, structo::hw::irq_source &, structo::hw::irq_config,
 *                                          reloco::function_ref<void(structo::hw::irq_source &)>) noexcept;
 *   static reloco::result<void> teardown_intr(my_pic &, structo::hw::irq_source &) noexcept;
 *   static reloco::result<std::reference_wrapper<structo::hw::irq_source>>
 *   map_intr(my_pic &, const structo::hw::irq_map_data &) noexcept;
 * };
 * @endcode
 *
 * Optionally, a backend may also supply any of `assign_cpu` (IRQ
 * affinity, FreeBSD's `PIC_BIND`), `pre_ithread`/`post_ithread`/
 * `post_filter` (the three mask/EOI-timing hooks around however the
 * caller's own dispatch loop handles a level-triggered source), and
 * `capabilities`:
 *
 * @code
 * static reloco::result<void> assign_cpu(my_pic &, structo::hw::irq_source &, std::size_t cpu) noexcept;
 * static reloco::result<void> pre_ithread(my_pic &, structo::hw::irq_source &) noexcept;
 * static reloco::result<void> post_ithread(my_pic &, structo::hw::irq_source &) noexcept;
 * static reloco::result<void> post_filter(my_pic &, structo::hw::irq_source &) noexcept;
 * static reloco::result<structo::hw::irqc_capabilities> capabilities(my_pic &) noexcept;
 * @endcode
 *
 * Each is detected independently via the same optional-member SFINAE
 * idiom `timer_traits::remaining`/`capabilities` use; if a given one is
 * absent, the matching `irqc_ref` method fails with
 * `error::unsupported_operation`.
 */

#include <reloco/array.hpp>
#include <reloco/detail/assert.hpp>
#include <reloco/detail/compat.hpp>
#include <reloco/error.hpp>
#include <reloco/function_ref.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/optional.hpp>
#include <reloco/span.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

namespace structo {

using namespace reloco;

namespace hw {

// ============================================================================
// Trigger / Polarity Configuration
// ============================================================================

/**
 * @brief How an interrupt source signals: edge-triggered (a transition
 * fires it) or level-triggered (the line being asserted fires it,
 * repeatedly, until deasserted/acknowledged) -- FreeBSD's
 * `INTR_TRIGGER_CONFORM`/`_EDGE`/`_LEVEL`.
 */
enum class irq_trigger : std::uint8_t {
  /** @brief Use the controller/wiring's own hardware default -- FreeBSD's `INTR_TRIGGER_CONFORM`. */
  conform,
  /** @brief Fires once per qualifying transition. */
  edge,
  /** @brief Fires continuously while the line is asserted; typically needs masking until acknowledged. */
  level,
};

/**
 * @brief Which signal level/transition direction is the "active" one --
 * FreeBSD's `INTR_POLARITY_CONFORM`/`_HIGH`/`_LOW`.
 */
enum class irq_polarity : std::uint8_t {
  /** @brief Use the controller/wiring's own hardware default -- FreeBSD's `INTR_POLARITY_CONFORM`. */
  conform,
  /** @brief Active-high (or rising-edge, combined with `irq_trigger::edge`). */
  active_high,
  /** @brief Active-low (or falling-edge, combined with `irq_trigger::edge`). */
  active_low,
};

/**
 * @brief The trigger/polarity pair a source is armed with, exactly the
 * two settings FreeBSD's `PIC_MAP_INTR`/`PIC_SETUP_INTR` negotiate
 * between a bus's devicetree/ACPI description and the PIC driver.
 */
struct irq_config {
  irq_trigger trigger = irq_trigger::conform;
  irq_polarity polarity = irq_polarity::conform;

  [[nodiscard]] friend constexpr bool operator==(const irq_config &a, const irq_config &b) noexcept {
    return a.trigger == b.trigger && a.polarity == b.polarity;
  }
  [[nodiscard]] friend constexpr bool operator!=(const irq_config &a, const irq_config &b) noexcept {
    return !(a == b);
  }
};

// ============================================================================
// Interrupt Mapping
// ============================================================================

/** @brief The maximum number of raw devicetree interrupt-specifier
 * cells `irq_map_data::try_from_fdt` accepts -- GICv2/v3's `#interrupt-
 * cells = <3>` (PPI/SPI type, number, flags) is the common real-world
 * case; 4 leaves headroom for a controller with one extra cell without
 * forcing every caller to pay for a larger buffer than any real binding
 * needs. See `fdt_reader.hpp`/`fdt_index.hpp` for pulling these cells
 * out of a devicetree blob's `interrupts`/`interrupts-extended`
 * property in the first place -- this header only carries them, it does
 * not parse FDT itself. */
inline constexpr std::size_t max_fdt_interrupt_cells = 4;

/**
 * @brief Which of `irq_map_data`'s payload fields is populated --
 * FreeBSD's `intr_map_data_type` (`INTR_MAP_DATA_FDT`/`_ACPI`/`_MSI`).
 */
enum class irq_map_kind : std::uint8_t {
  /** @brief Raw devicetree `interrupts`-property specifier cells (`fdt_cells`/`fdt_cell_count`). */
  fdt,
  /** @brief A message-signaled-interrupt vector/message number (`msi_vector`). */
  msi,
  /** @brief A flat, globally-unique interrupt number with no parent-relative cells, e.g. an
   * ACPI Global System Interrupt or x86 IOAPIC pin (`gsi`). */
  gsi,
};

/**
 * @brief Generic, bus-agnostic "how do I route this raw interrupt
 * specifier to a registered `irq_source`" payload passed to
 * `irqc_ref::map_intr` -- FreeBSD's `struct intr_map_data`.
 *
 * Aggregate, value-type, stack-constructed at the call site (not a
 * long-lived handle): every field is always present regardless of
 * `kind`, trading a few unused bytes for a plain, allocation-free
 * struct instead of a tagged union/`std::variant`.
 */
struct irq_map_data {
  irq_map_kind kind = irq_map_kind::gsi;

  /** @brief Raw `interrupts`-property cells, valid when `kind == irq_map_kind::fdt`;
   * only the first `fdt_cell_count` entries are meaningful. */
  array<std::uint32_t, max_fdt_interrupt_cells> fdt_cells{};
  /** @brief How many of `fdt_cells` are populated; valid when `kind == irq_map_kind::fdt`. */
  std::uint8_t fdt_cell_count = 0;
  /** @brief MSI vector/message number, valid when `kind == irq_map_kind::msi`. */
  std::uint32_t msi_vector = 0;
  /** @brief Flat global interrupt number, valid when `kind == irq_map_kind::gsi`. */
  std::uint32_t gsi = 0;

  /**
   * @brief Builds an `irq_map_kind::fdt` mapping from raw devicetree
   * specifier cells.
   * Fails with `error::out_of_range` if @p cells has more than
   * `max_fdt_interrupt_cells` entries.
   */
  [[nodiscard]] static result<irq_map_data> try_from_fdt(span<const std::uint32_t> cells) noexcept {
    if (cells.size() > max_fdt_interrupt_cells)
      return unexpected(error::out_of_range);
    irq_map_data data;
    data.kind = irq_map_kind::fdt;
    data.fdt_cell_count = static_cast<std::uint8_t>(cells.size());
    for (std::size_t i = 0; i < cells.size(); ++i)
      data.fdt_cells[i] = cells[i];
    return data;
  }

  /** @brief Builds an `irq_map_kind::msi` mapping from a raw MSI vector/message number. */
  [[nodiscard]] static constexpr irq_map_data from_msi(std::uint32_t vector) noexcept {
    irq_map_data data;
    data.kind = irq_map_kind::msi;
    data.msi_vector = vector;
    return data;
  }

  /** @brief Builds an `irq_map_kind::gsi` mapping from a flat global interrupt number. */
  [[nodiscard]] static constexpr irq_map_data from_gsi(std::uint32_t gsi) noexcept {
    irq_map_data data;
    data.kind = irq_map_kind::gsi;
    data.gsi = gsi;
    return data;
  }
};

// ============================================================================
// Interrupt Source
// ============================================================================

/**
 * @brief Per-line bookkeeping for one registered interrupt source --
 * FreeBSD's `struct intr_irqsrc` -- tracking its controller-local IRQ
 * number, negotiated trigger/polarity, enabled state, CPU affinity, and
 * at most one registered handler.
 *
 * Never copyable or movable: an `irq_source` is registered by address
 * (`irqc_ref::map_intr`/`setup_intr` hand out and operate on references
 * to a fixed instance), exactly like `async_kernel_object`'s own
 * identity requirement, so it must not change address while it may be
 * armed/looked-up. A concrete controller backend **composes** this
 * (embeds it as a member, does not derive from it) into its own
 * per-line type -- see the @file-level docs above for why, and the
 * worked `gic_irq_line` example there.
 */
class irq_source {
public:
  /** @brief Constructs an unarmed, disabled source for controller-local IRQ number @p irq. */
  constexpr explicit irq_source(std::uint32_t irq) noexcept : irq_(irq) {}

  irq_source(const irq_source &) = delete;
  irq_source &operator=(const irq_source &) = delete;
  irq_source(irq_source &&) = delete;
  irq_source &operator=(irq_source &&) = delete;

  /** @brief The controller-local IRQ number this source represents, fixed at construction. */
  [[nodiscard]] constexpr std::uint32_t irq() const noexcept { return irq_; }
  /** @brief The trigger/polarity this source was last `set_config`ured with. */
  [[nodiscard]] constexpr irq_config config() const noexcept { return config_; }
  /** @brief Whether this source is currently unmasked at the controller. */
  [[nodiscard]] constexpr bool is_enabled() const noexcept { return enabled_; }
  /** @brief The logical CPU this source is currently affine to (meaningful only once a
   * backend has actually implemented `irqc_traits::assign_cpu`; `0` otherwise). */
  [[nodiscard]] constexpr std::size_t target_cpu() const noexcept { return target_cpu_; }
  /** @brief Whether a handler is currently registered (`set_handler`ed, not yet `clear_handler`ed). */
  [[nodiscard]] bool has_handler() const noexcept { return handler_.has_value(); }

  /** @brief Records the trigger/polarity this source has been armed with. Bookkeeping
   * only -- does not itself touch hardware; a controller backend calls this from its own
   * `irqc_traits::setup_intr` alongside whatever register writes that requires. */
  void set_config(irq_config cfg) noexcept { config_ = cfg; }
  /** @brief Records this source's enabled/masked state. Bookkeeping only, see `set_config`. */
  void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
  /** @brief Records which logical CPU this source is affine to. Bookkeeping only, see `set_config`. */
  void set_target_cpu(std::size_t cpu) noexcept { target_cpu_ = cpu; }

  /**
   * @brief Registers @p handler, replacing whatever was previously
   * registered, to be invoked by `dispatch()`.
   * `handler` is a non-owning `function_ref`: the callable it was
   * constructed from must remain valid for as long as it stays
   * registered (until `clear_handler()`/this source's own destruction),
   * not merely until the end of the calling statement -- exactly
   * `timer_ref::set_callback`'s own lifetime caveat.
   */
  void set_handler(function_ref<void(irq_source &)> handler) noexcept { handler_ = handler; }

  /** @brief Unregisters whatever handler is currently set, if any. Idempotent. */
  void clear_handler() noexcept { handler_.reset(); }

  /**
   * @brief Invokes the currently-registered handler, passing this
   * source by reference -- called by whatever actually decoded "this
   * source fired" (a controller backend's own trap/exception entry
   * after reading back e.g. a GIC `IAR`/PLIC claim register); a no-op
   * (spurious interrupt) if no handler is currently registered.
   * The invoked handler must be interrupt-safe: short, non-blocking,
   * and performing no allocation, exactly the discipline any real ISR
   * handler requires.
   */
  void dispatch() noexcept {
    if (handler_.has_value())
      handler_.value()(*this);
  }

private:
  std::uint32_t irq_;
  irq_config config_{};
  bool enabled_ = false;
  std::size_t target_cpu_ = 0;
  optional<function_ref<void(irq_source &)>> handler_{};
};

// ============================================================================
// Controller Capabilities
// ============================================================================

/**
 * @brief The typical static capabilities of a hardware interrupt
 * controller backend: which `irq_map_data` kinds it can resolve and
 * which optional operations it implements.
 *
 * Aggregate, value-type metadata, mirroring `timer_capabilities`'s own
 * role for `timer_ref`.
 */
struct irqc_capabilities {
  /** @brief The total number of interrupt sources this controller manages; `0` means "unknown". */
  std::uint32_t max_sources = 0;
  /** @brief Whether `map_intr` can resolve `irq_map_kind::fdt` payloads. */
  bool supports_fdt_mapping = false;
  /** @brief Whether `map_intr` can resolve `irq_map_kind::msi` payloads. */
  bool supports_msi_mapping = false;
  /** @brief Whether `map_intr` can resolve `irq_map_kind::gsi` payloads. */
  bool supports_gsi_mapping = false;
  /** @brief Whether the optional `irqc_traits::assign_cpu` (IRQ affinity, FreeBSD's `PIC_BIND`) is implemented. */
  bool supports_affinity = false;
  /** @brief Whether this controller has per-CPU-private sources (e.g. a GIC's PPIs/SGIs,
   * each core seeing its own private banked instance of the same IRQ number) in addition to
   * shared ones (e.g. a GIC's SPIs) -- mirrors `timer_capabilities::is_per_cpu`. */
  bool supports_percpu_sources = false;

  [[nodiscard]] friend constexpr bool operator==(const irqc_capabilities &a, const irqc_capabilities &b) noexcept {
    return a.max_sources == b.max_sources && a.supports_fdt_mapping == b.supports_fdt_mapping &&
           a.supports_msi_mapping == b.supports_msi_mapping && a.supports_gsi_mapping == b.supports_gsi_mapping &&
           a.supports_affinity == b.supports_affinity && a.supports_percpu_sources == b.supports_percpu_sources;
  }
  [[nodiscard]] friend constexpr bool operator!=(const irqc_capabilities &a, const irqc_capabilities &b) noexcept {
    return !(a == b);
  }
};

// ============================================================================
// Customization Point
// ============================================================================

/**
 * @brief Opt-in customization point describing how to drive a concrete
 * interrupt controller backend through @ref irqc_ref.
 *
 * Intentionally left undefined for any `Backend` that hasn't been
 * adapted, mirroring `uart_traits`/`timer_traits`/`hw_rng_traits`. See
 * the @file-level docs above for the complete required/optional member
 * list.
 */
template <typename Backend> struct irqc_traits;

namespace detail {

template <typename Backend, typename = void> struct has_irqc_traits : std::false_type {};

template <typename Backend>
struct has_irqc_traits<
    Backend, std::void_t<decltype(irqc_traits<Backend>::enable_intr), decltype(irqc_traits<Backend>::disable_intr),
                         decltype(irqc_traits<Backend>::setup_intr), decltype(irqc_traits<Backend>::teardown_intr),
                         decltype(irqc_traits<Backend>::map_intr)>> : std::true_type {};

// Detects each independently-optional Traits member, mirroring
// timer_traits's timer_has_remaining/timer_has_capabilities idiom.
template <typename Traits, typename = void> struct irqc_has_assign_cpu : std::false_type {};
template <typename Traits>
struct irqc_has_assign_cpu<Traits, std::void_t<decltype(Traits::assign_cpu)>> : std::true_type {};

template <typename Traits, typename = void> struct irqc_has_pre_ithread : std::false_type {};
template <typename Traits>
struct irqc_has_pre_ithread<Traits, std::void_t<decltype(Traits::pre_ithread)>> : std::true_type {};

template <typename Traits, typename = void> struct irqc_has_post_ithread : std::false_type {};
template <typename Traits>
struct irqc_has_post_ithread<Traits, std::void_t<decltype(Traits::post_ithread)>> : std::true_type {};

template <typename Traits, typename = void> struct irqc_has_post_filter : std::false_type {};
template <typename Traits>
struct irqc_has_post_filter<Traits, std::void_t<decltype(Traits::post_filter)>> : std::true_type {};

template <typename Traits, typename = void> struct irqc_has_capabilities : std::false_type {};
template <typename Traits>
struct irqc_has_capabilities<Traits, std::void_t<decltype(Traits::capabilities)>> : std::true_type {};

} // namespace detail

// ============================================================================
// Type-Erased Interrupt Controller Handle
// ============================================================================

/**
 * @brief Type-erased, non-owning handle over the typical operations of
 * a hardware interrupt controller (PIC), for whatever concrete backend
 * it is bound to.
 *
 * Default-constructed (or copied from a default-constructed) refs are
 * *unbound*: every operation fails with `error::unsupported_operation`
 * rather than trapping, mirroring `uart_ref`/`timer_ref`/`hw_rng_ref`'s
 * null-safety convention.
 */
class RELOCO_POINTER irqc_ref {
public:
  /** @brief Fixed, per-bound-backend-type dispatch table. */
  struct vtable {
    result<void> (*enable_intr)(void *ctx, irq_source &src) noexcept;
    result<void> (*disable_intr)(void *ctx, irq_source &src) noexcept;
    result<void> (*setup_intr)(void *ctx, irq_source &src, irq_config cfg,
                               function_ref<void(irq_source &)> handler) noexcept;
    result<void> (*teardown_intr)(void *ctx, irq_source &src) noexcept;
    result<std::reference_wrapper<irq_source>> (*map_intr)(void *ctx, const irq_map_data &data) noexcept;
    result<void> (*assign_cpu)(void *ctx, irq_source &src, std::size_t cpu) noexcept;
    result<void> (*pre_ithread)(void *ctx, irq_source &src) noexcept;
    result<void> (*post_ithread)(void *ctx, irq_source &src) noexcept;
    result<void> (*post_filter)(void *ctx, irq_source &src) noexcept;
    result<irqc_capabilities> (*capabilities)(void *ctx) noexcept;
  };

  /** @brief Constructs an unbound ref. */
  constexpr irqc_ref() noexcept = default;

  /**
   * @brief Binds this ref to an existing, adapted backend.
   * @tparam Backend Concrete backend type, deduced. Must have an
   * @ref irqc_traits specialization.
   * @param b Backend to bind. Must outlive this handle and every copy of
   * it. Marked `explicit`: binding a backend is always a deliberate step,
   * never an implicit conversion.
   */
  template <typename Backend, std::enable_if_t<detail::has_irqc_traits<Backend>::value, int> = 0>
  constexpr explicit irqc_ref(Backend &b RELOCO_LIFETIMEBOUND RELOCO_LIFETIME_CAPTURE_BY_THIS) noexcept
      : ctx_(std::addressof(b)), vtbl_(&s_vtbl<Backend>) {}

  /** @brief Rejects rvalue/temporary backend bindings. */
  template <typename Backend, std::enable_if_t<!std::is_lvalue_reference_v<Backend>, int> = 0>
  irqc_ref(Backend &&) = delete;

  /** @brief Whether this ref is bound to a backend. */
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return vtbl_ != nullptr; }

  // --------------------------------------------------------------------
  // Mandatory backend operations (directly forwarded).
  // --------------------------------------------------------------------

  /** @brief Unmasks @p src at the controller, so it can fire; records `src.is_enabled() == true`. */
  [[nodiscard]] result<void> enable_intr(irq_source &src) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->enable_intr(ctx_, src);
  }

  /** @brief Masks @p src at the controller; records `src.is_enabled() == false`. Idempotent. */
  [[nodiscard]] result<void> disable_intr(irq_source &src) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->disable_intr(ctx_, src);
  }

  /**
   * @brief Arms @p src with trigger/polarity @p cfg and registers
   * @p handler (see `irq_source::set_handler`'s own lifetime caveat),
   * then enables it -- FreeBSD's `PIC_SETUP_INTR` plus MI
   * `intr_irq_add_handler` combined into one call, since this header has
   * no separate MI handler-chain layer (see the @file-level docs above).
   */
  [[nodiscard]] result<void> setup_intr(irq_source &src, irq_config cfg,
                                        function_ref<void(irq_source &)> handler) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->setup_intr(ctx_, src, cfg, handler);
  }

  /** @brief Disables @p src and unregisters its handler -- FreeBSD's `PIC_TEARDOWN_INTR`. */
  [[nodiscard]] result<void> teardown_intr(irq_source &src) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->teardown_intr(ctx_, src);
  }

  /**
   * @brief Resolves a bus-specific raw interrupt specifier to the
   * concrete, already-registered `irq_source` it names -- FreeBSD's
   * `PIC_MAP_INTR`.
   * Fails with `error::not_found` (or whatever the backend reports) if
   * @p data does not name any source this controller manages.
   */
  [[nodiscard]] result<std::reference_wrapper<irq_source>> map_intr(const irq_map_data &data) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->map_intr(ctx_, data);
  }

  // --------------------------------------------------------------------
  // Optional backend operations (directly forwarded, with a generic
  // `error::unsupported_operation` fallback when the backend does not
  // implement them).
  // --------------------------------------------------------------------

  /** @brief Rebinds @p src's affinity to logical CPU @p cpu -- FreeBSD's `PIC_BIND`.
   * Fails with `error::unsupported_operation` if this ref is unbound, or if the bound
   * backend does not implement the optional `irqc_traits::assign_cpu`. */
  [[nodiscard]] result<void> assign_cpu(irq_source &src, std::size_t cpu) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->assign_cpu(ctx_, src, cpu);
  }

  /** @brief Called before a level-triggered @p src's handler runs, typically to mask it
   * until acknowledged -- FreeBSD's `PIC_PRE_ITHREAD`. Fails with
   * `error::unsupported_operation` if unbound/unimplemented. */
  [[nodiscard]] result<void> pre_ithread(irq_source &src) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->pre_ithread(ctx_, src);
  }

  /** @brief Called after a level-triggered @p src's handler has finished, typically to
   * unmask it and/or EOI -- FreeBSD's `PIC_POST_ITHREAD`. Fails with
   * `error::unsupported_operation` if unbound/unimplemented. */
  [[nodiscard]] result<void> post_ithread(irq_source &src) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->post_ithread(ctx_, src);
  }

  /** @brief Called once an edge-triggered (or otherwise self-contained) @p src's handler
   * has fully handled it without needing the `pre_ithread`/`post_ithread` mask dance,
   * typically just to EOI -- FreeBSD's `PIC_POST_FILTER`. Fails with
   * `error::unsupported_operation` if unbound/unimplemented. */
  [[nodiscard]] result<void> post_filter(irq_source &src) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->post_filter(ctx_, src);
  }

  /** @brief The bound backend's static capabilities. Fails with
   * `error::unsupported_operation` if this ref is unbound, or if the bound backend does
   * not implement the optional `irqc_traits::capabilities`. */
  [[nodiscard]] result<irqc_capabilities> capabilities() const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->capabilities(ctx_);
  }

  // --------------------------------------------------------------------
  // Generic conveniences, synthesized purely from the mandatory
  // operations above -- no further backend support is required.
  // --------------------------------------------------------------------

  /**
   * @brief `[[nodiscard]]` RAII scope guard masking a single `irq_source`
   * for its lifetime (see the @file-level "Rust-inspired API shape"
   * docs above) -- `disable_intr(src)` on construction (recording
   * whether @p src was actually enabled beforehand), `enable_intr(src)`
   * again on destruction, but only if it was.
   */
  class [[nodiscard]] scoped_mask {
  public:
    scoped_mask(const scoped_mask &) = delete;
    scoped_mask &operator=(const scoped_mask &) = delete;

    /** @brief Transfers the mask to `other`; `other` is left disarmed (no-op on destruction). */
    scoped_mask(scoped_mask &&other) noexcept
        : ctx_(other.ctx_), vtbl_(other.vtbl_), src_(other.src_), was_enabled_(other.was_enabled_),
          armed_(std::exchange(other.armed_, false)) {}

    /** @brief Restores the prior enabled state first, then takes over `other`'s state; `other` is left disarmed. */
    scoped_mask &operator=(scoped_mask &&other) noexcept {
      if (this != &other) {
        restore();
        ctx_ = other.ctx_;
        vtbl_ = other.vtbl_;
        src_ = other.src_;
        was_enabled_ = other.was_enabled_;
        armed_ = std::exchange(other.armed_, false);
      }
      return *this;
    }

    /** @brief Restores @p src's prior enabled state, unless already `unlock()`ed/moved-from. */
    ~scoped_mask() noexcept { restore(); }

    /** @brief Restores the prior enabled state now, before scope exit. Idempotent. */
    void unlock() noexcept { restore(); }

  private:
    friend class irqc_ref;

    // Stores `ctx_`/`vtbl_` directly (rather than a full `irqc_ref` member)
    // since `irqc_ref` is still an incomplete type at this nested class's
    // own point of definition.
    scoped_mask(void *ctx, const vtable *vtbl, irq_source &src, bool was_enabled) noexcept
        : ctx_(ctx), vtbl_(vtbl), src_(&src), was_enabled_(was_enabled), armed_(true) {}

    void restore() noexcept {
      if (armed_) {
        if (was_enabled_ && vtbl_)
          (void)vtbl_->enable_intr(ctx_, *src_);
        armed_ = false;
      }
    }

    void *ctx_ = nullptr;
    const vtable *vtbl_ = nullptr;
    irq_source *src_ = nullptr;
    bool was_enabled_ = false;
    bool armed_ = false;
  };

  /**
   * @brief Masks @p src for the returned guard's lifetime, restoring
   * whatever enabled state it had beforehand once the guard is
   * destroyed/`unlock()`ed -- see `scoped_mask`/the @file-level
   * "Rust-inspired API shape" docs above.
   */
  [[nodiscard]] scoped_mask mask_scoped(irq_source &src) const noexcept {
    const bool was_enabled = src.is_enabled();
    (void)disable_intr(src);
    return scoped_mask(ctx_, vtbl_, src, was_enabled);
  }

private:
  template <typename Backend> static result<void> enable_intr_entry(void *ctx, irq_source &src) noexcept {
    return irqc_traits<Backend>::enable_intr(*static_cast<Backend *>(ctx), src);
  }

  template <typename Backend> static result<void> disable_intr_entry(void *ctx, irq_source &src) noexcept {
    return irqc_traits<Backend>::disable_intr(*static_cast<Backend *>(ctx), src);
  }

  template <typename Backend>
  static result<void> setup_intr_entry(void *ctx, irq_source &src, irq_config cfg,
                                       function_ref<void(irq_source &)> handler) noexcept {
    return irqc_traits<Backend>::setup_intr(*static_cast<Backend *>(ctx), src, cfg, handler);
  }

  template <typename Backend> static result<void> teardown_intr_entry(void *ctx, irq_source &src) noexcept {
    return irqc_traits<Backend>::teardown_intr(*static_cast<Backend *>(ctx), src);
  }

  template <typename Backend>
  static result<std::reference_wrapper<irq_source>> map_intr_entry(void *ctx, const irq_map_data &data) noexcept {
    return irqc_traits<Backend>::map_intr(*static_cast<Backend *>(ctx), data);
  }

  template <typename Backend>
  static result<void> assign_cpu_entry(void *ctx, irq_source &src, std::size_t cpu) noexcept {
    using traits = irqc_traits<Backend>;
    if constexpr (detail::irqc_has_assign_cpu<traits>::value) {
      return traits::assign_cpu(*static_cast<Backend *>(ctx), src, cpu);
    } else {
      (void)ctx;
      (void)src;
      (void)cpu;
      return unexpected(error::unsupported_operation);
    }
  }

  template <typename Backend> static result<void> pre_ithread_entry(void *ctx, irq_source &src) noexcept {
    using traits = irqc_traits<Backend>;
    if constexpr (detail::irqc_has_pre_ithread<traits>::value) {
      return traits::pre_ithread(*static_cast<Backend *>(ctx), src);
    } else {
      (void)ctx;
      (void)src;
      return unexpected(error::unsupported_operation);
    }
  }

  template <typename Backend> static result<void> post_ithread_entry(void *ctx, irq_source &src) noexcept {
    using traits = irqc_traits<Backend>;
    if constexpr (detail::irqc_has_post_ithread<traits>::value) {
      return traits::post_ithread(*static_cast<Backend *>(ctx), src);
    } else {
      (void)ctx;
      (void)src;
      return unexpected(error::unsupported_operation);
    }
  }

  template <typename Backend> static result<void> post_filter_entry(void *ctx, irq_source &src) noexcept {
    using traits = irqc_traits<Backend>;
    if constexpr (detail::irqc_has_post_filter<traits>::value) {
      return traits::post_filter(*static_cast<Backend *>(ctx), src);
    } else {
      (void)ctx;
      (void)src;
      return unexpected(error::unsupported_operation);
    }
  }

  template <typename Backend> static result<irqc_capabilities> capabilities_entry(void *ctx) noexcept {
    using traits = irqc_traits<Backend>;
    if constexpr (detail::irqc_has_capabilities<traits>::value) {
      return traits::capabilities(*static_cast<Backend *>(ctx));
    } else {
      (void)ctx;
      return unexpected(error::unsupported_operation);
    }
  }

  template <typename Backend>
  static constexpr vtable s_vtbl{&enable_intr_entry<Backend>, &disable_intr_entry<Backend>,
                                 &setup_intr_entry<Backend>,  &teardown_intr_entry<Backend>,
                                 &map_intr_entry<Backend>,    &assign_cpu_entry<Backend>,
                                 &pre_ithread_entry<Backend>, &post_ithread_entry<Backend>,
                                 &post_filter_entry<Backend>, &capabilities_entry<Backend>};

  void *ctx_ = nullptr;
  const vtable *vtbl_ = nullptr;
};

} // namespace hw
} // namespace structo
