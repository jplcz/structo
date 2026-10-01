// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file per_cpu_ptr.hpp
 * @brief `structo::arch::per_cpu_ptr<Tag, T>`: a thin, storage-free,
 * type-safe resolver for a per-CPU pointer, keyed by a caller-defined
 * `Tag` type so unrelated per-CPU pointers of different `T` never
 * collide even if the underlying kernel only has one real backing
 * mechanism to offer.
 *
 * This class owns no memory and implements no per-CPU storage itself:
 * `Tag` supplies the real per-CPU container -- a linear array indexed by
 * CPU, a compiler/ABI `__thread`-style mechanism, FreeBSD's
 * `PCPU_GET`/`PCPU_SET` macros, Linux's `this_cpu_ptr()`/`per_cpu_ptr()`,
 * or anything else a concrete kernel port wants -- via `get_ptr()`/
 * `set_ptr()`; `per_cpu_ptr<Tag, T>` only `static_cast`s the `Tag`'s
 * type-erased `void*` to/from `T*`, so the exact mechanism (and how
 * "current" vs. an explicit foreign CPU's slot is resolved) is entirely
 * `Tag`'s decision.
 *
 * `Tag` must provide:
 * - `static constexpr std::size_t max_cpus;`
 * - `static void *get_ptr(CpuId cpu) noexcept;`
 * - `static void set_ptr(CpuId cpu, void *ptr) noexcept;`
 *
 * `Tag` must additionally provide *either* a dedicated "current CPU" fast
 * path (used in preference when present):
 * - `static void *get_current_ptr() noexcept;`
 * - `static void set_current_ptr(void *ptr) noexcept;`
 *
 * *or*, lacking that fast path, a `current()` CPU-id resolver -- the same
 * contract `structo::arch::cpu_index<Tag>` requires (see
 * `cpu_index.hpp`) -- that `per_cpu_ptr<Tag, T>::get()`/`set()` combine
 * with `get_ptr()`/`set_ptr()` to resolve the running CPU's own slot:
 * - `static CpuId current() noexcept;`
 *
 * Example `Tag` (a flat array-backed resolver; a real kernel port would
 * instead forward to its own per-CPU mechanism):
 * @code
 * struct my_kernel_per_cpu_tag {
 *   using cpu_id_type = std::size_t;
 *   static inline constexpr std::size_t max_cpus = 64;
 *   static inline void *slots[max_cpus]{nullptr};
 *
 *   static void *get_ptr(std::size_t cpu) noexcept { return slots[cpu]; }
 *   static void set_ptr(std::size_t cpu, void *ptr) noexcept { slots[cpu] = ptr; }
 *
 *   static std::size_t current() noexcept { return hw_current_cpu_index(); }
 * };
 * @endcode
 */

#include <cstddef>
#include <type_traits>

namespace structo::arch {

// -------------------------------------------------------------------------
// Tag Capability Detection (SFINAE)
// -------------------------------------------------------------------------
/**
 * @brief Compile-time detectors for which optional "current CPU" fast-path
 * members a `Tag` policy implements, used to SFINAE-select between
 * `Tag::get_current_ptr()`/`set_current_ptr()` and falling back to
 * `Tag::current()` combined with `get_ptr()`/`set_ptr()`.
 */
namespace detail {

/** @brief Resolves `Tag`'s CPU-id type: `Tag::cpu_id_type` if provided, else `std::size_t`. */
template <typename Tag, typename = void> struct per_cpu_tag_cpu_id {
  using type = std::size_t;
};

template <typename Tag> struct per_cpu_tag_cpu_id<Tag, std::void_t<typename Tag::cpu_id_type>> {
  using type = typename Tag::cpu_id_type;
};

/** @brief True if `Tag::get_current_ptr()` is callable (dedicated current-CPU fast path). */
template <typename Tag, typename = void> struct has_get_current_ptr : std::false_type {};

template <typename Tag>
struct has_get_current_ptr<Tag, std::void_t<decltype(Tag::get_current_ptr())>> : std::true_type {};

/** @brief True if `Tag::set_current_ptr(ptr)` is callable (dedicated current-CPU fast path). */
template <typename Tag, typename = void> struct has_set_current_ptr : std::false_type {};

template <typename Tag>
struct has_set_current_ptr<Tag, std::void_t<decltype(Tag::set_current_ptr(std::declval<void *>()))>>
    : std::true_type {};

} // namespace detail

// -------------------------------------------------------------------------
// Per-CPU Typed Pointer Resolver
// -------------------------------------------------------------------------
/**
 * @brief Storage-free, type-safe resolver for a per-CPU `T*`, keyed by
 * `Tag` and backed entirely by `Tag`'s own per-CPU mechanism.
 *
 * @tparam Tag Policy supplying the real per-CPU container; see the
 * file-level docs above for the full required/optional interface and an
 * example implementation.
 * @tparam T   Pointee type of the per-CPU pointer this `Tag` resolves.
 */
template <typename Tag, typename T> class per_cpu_ptr {
public:
  using tag_type = Tag;
  using value_type = T;
  using cpu_id_type = typename detail::per_cpu_tag_cpu_id<Tag>::type;

  static inline constexpr std::size_t max_cpus = Tag::max_cpus;

  /** @brief Returns the `T*` registered for `cpu` (nullptr if none was ever set). */
  [[nodiscard]] static T *get(cpu_id_type cpu) noexcept { return static_cast<T *>(Tag::get_ptr(cpu)); }

  /** @brief Registers `ptr` as `cpu`'s slot (pass `nullptr` to clear it). */
  static void set(cpu_id_type cpu, T *ptr) noexcept { Tag::set_ptr(cpu, static_cast<void *>(ptr)); }

  /**
   * @brief Returns the running CPU's `T*`. Uses `Tag::get_current_ptr()`
   * directly if provided; otherwise resolves `Tag::current()` and
   * delegates to `get(cpu)`.
   */
  [[nodiscard]] static T *get() noexcept {
    if constexpr (detail::has_get_current_ptr<Tag>::value) {
      return static_cast<T *>(Tag::get_current_ptr());
    } else {
      return get(Tag::current());
    }
  }

  /**
   * @brief Registers `ptr` as the running CPU's slot. Uses
   * `Tag::set_current_ptr()` directly if provided; otherwise resolves
   * `Tag::current()` and delegates to `set(cpu, ptr)`.
   */
  static void set(T *ptr) noexcept {
    if constexpr (detail::has_set_current_ptr<Tag>::value) {
      Tag::set_current_ptr(static_cast<void *>(ptr));
    } else {
      set(Tag::current(), ptr);
    }
  }
};

} // namespace structo::arch
