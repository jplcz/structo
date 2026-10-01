// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file cpu_index.hpp
 * @brief `structo::arch::cpu_index<Tag>`: a zero-overhead CRTP-style wrapper
 * resolving "which logical CPU is this" plus a handful of common
 * kernel/hypervisor core operations (yield, WFE/SEV, BSP detection,
 * hardware ID), all delegated to a caller-provided `Tag` policy type.
 *
 * `Tag` only needs to supply `static constexpr std::size_t max_cpus` plus
 * whichever of `current()`, `from_context(ctx)`, `hardware_id()`,
 * `is_bsp()`, `yield()`, `wait_for_event()`, `send_event()` it can
 * actually implement -- every method is individually SFINAE-gated, so a
 * `Tag` that only implements a subset still compiles; unsupported
 * operations simply never appear as callable members of `cpu_index<Tag>`.
 * `uniprocessor_tag` is the bundled zero-cost single-core implementation
 * (always core 0, always BSP, every operation a no-op).
 */

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace structo::arch {

// -------------------------------------------------------------------------
// Tag Capability Detection (SFINAE)
// -------------------------------------------------------------------------
/**
 * @brief Compile-time detectors for which optional members a `Tag` policy
 * implements, used to SFINAE-gate the corresponding `cpu_index<Tag>`
 * passthrough so unsupported operations are simply absent rather than a
 * hard compile error.
 */
namespace detail {
/** @brief True if `T` defines a `max_cpus` member (required on every Tag). */
template <typename T, typename = void> struct has_max_cpus : std::false_type {};

template <typename T> struct has_max_cpus<T, std::void_t<decltype(T::max_cpus)>> : std::true_type {};

/** @brief True if `T::current()` is callable (resolves the running CPU). */
template <typename T, typename = void> struct has_current : std::false_type {};

template <typename T> struct has_current<T, std::void_t<decltype(T::current())>> : std::true_type {};

/** @brief True if `T::from_context(ctx)` is callable for a given `Ctx`. */
template <typename T, typename Ctx, typename = void> struct has_from_context : std::false_type {};

template <typename T, typename Ctx>
struct has_from_context<T, Ctx, std::void_t<decltype(T::from_context(std::declval<const Ctx &>()))>> : std::true_type {
};
} // namespace detail

// -------------------------------------------------------------------------
// CPU Index Abstraction
// -------------------------------------------------------------------------
/**
 * @brief Resolves CPU indices and delegates kernel operations via Tag.
 * @tparam Tag Kernel/Hardware policy providing topology limits and core operations.
 */
template <typename Tag> class cpu_index {
  static_assert(detail::has_max_cpus<Tag>::value, "Tag must define 'static constexpr std::size_t max_cpus'");
  static_assert(Tag::max_cpus > 0, "Tag::max_cpus must be greater than zero");

public:
  using tag_type = Tag;

  static inline constexpr std::size_t max_cpus = Tag::max_cpus;

  /**
   * @brief Queries the index of the currently executing CPU.
   * Only present if `Tag::current()` is implemented (SFINAE-gated).
   * @return The resolved index, clamped to 0 if `Tag::current()` reported an
   * out-of-range value.
   */
  template <typename T = Tag>
  [[nodiscard]] static inline auto current() noexcept -> std::enable_if_t<detail::has_current<T>::value, std::size_t> {
    const std::size_t idx = Tag::current();
    return (idx < max_cpus) ? idx : 0;
  }

  /**
   * @brief Queries the CPU index from an explicit execution context (e.g. vcpu_context).
   * Only present if `Tag::from_context(ctx)` is implemented (SFINAE-gated).
   * @tparam Context Caller-defined execution-context type (e.g. a trap frame or vcpu handle).
   * @param ctx Context to resolve the owning CPU index from.
   * @return The resolved index, clamped to 0 if `Tag::from_context()` reported an
   * out-of-range value.
   */
  template <typename Context, typename T = Tag>
  [[nodiscard]] static inline auto from_context(const Context &ctx) noexcept
      -> std::enable_if_t<detail::has_from_context<T, Context>::value, std::size_t> {
    const std::size_t idx = Tag::from_context(ctx);
    return (idx < max_cpus) ? idx : 0;
  }

  /**
   * @brief Verifies whether a given CPU index is within bounds for this domain.
   * @param index Candidate CPU index.
   * @return `true` if `index < max_cpus`.
   */
  [[nodiscard]] static constexpr bool is_valid(std::size_t index) noexcept { return index < max_cpus; }

  // ---------------------------------------------------------------------
  // Kernel Operation Passthroughs (Invoked directly on the Tag)
  // ---------------------------------------------------------------------

  /**
   * @brief Access the raw architectural hardware ID (e.g. full MPIDR value).
   * Only present if `Tag::hardware_id()` is implemented (SFINAE-gated via `decltype`).
   * @return Whatever type `Tag::hardware_id()` returns, forwarded unchanged.
   */
  template <typename T = Tag> [[nodiscard]] static inline auto hardware_id() noexcept -> decltype(T::hardware_id()) {
    return Tag::hardware_id();
  }

  /**
   * @brief Determines if current execution is on the primary boot core (BSP).
   * Only present if `Tag::is_bsp()` is implemented (SFINAE-gated via `decltype`).
   * @return Whatever type `Tag::is_bsp()` returns, forwarded unchanged.
   */
  template <typename T = Tag> [[nodiscard]] static inline auto is_bsp() noexcept -> decltype(T::is_bsp()) {
    return Tag::is_bsp();
  }

  /**
   * @brief Hint to hardware that the current core is spin-waiting.
   * Only present if `Tag::yield()` is implemented (SFINAE-gated via `decltype`).
   */
  template <typename T = Tag> static inline auto yield() noexcept -> decltype(T::yield()) { Tag::yield(); }

  /**
   * @brief Suspend execution until an event occurs (Wait For Event).
   * Only present if `Tag::wait_for_event()` is implemented (SFINAE-gated via `decltype`).
   */
  template <typename T = Tag> static inline auto wait_for_event() noexcept -> decltype(T::wait_for_event()) {
    Tag::wait_for_event();
  }

  /**
   * @brief Send event signal to all cores in the multiprocessor system.
   * Only present if `Tag::send_event()` is implemented (SFINAE-gated via `decltype`).
   */
  template <typename T = Tag> static inline auto send_event() noexcept -> decltype(T::send_event()) {
    Tag::send_event();
  }
};

// -------------------------------------------------------------------------
// Concrete Tag: Static Uniprocessor (Zero Overhead / Single-Core VM)
// -------------------------------------------------------------------------
/**
 * @brief Bundled zero-cost `cpu_index` tag for single-core targets: always
 * core 0, always BSP, every kernel-operation passthrough a no-op.
 */
struct uniprocessor_tag {
  static inline constexpr std::size_t max_cpus = 1;

  [[nodiscard]] static constexpr std::size_t current() noexcept { return 0; }

  template <typename Context> [[nodiscard]] static constexpr std::size_t from_context(const Context &) noexcept {
    return 0;
  }

  [[nodiscard]] static constexpr uint32_t hardware_id() noexcept { return 0; }

  [[nodiscard]] static constexpr bool is_bsp() noexcept { return true; }

  static constexpr void yield() noexcept {}

  static constexpr void wait_for_event() noexcept {}

  static constexpr void send_event() noexcept {}
};

} // namespace structo::arch