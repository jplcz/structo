// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file per_domain_ptr.hpp
 * @brief `structo::arch::per_domain_ptr<Tag, T>`: a thin, storage-free,
 * type-safe resolver for a per-domain pointer, keyed by a caller-defined
 * `Tag` type so unrelated per-domain pointers of different `T` never
 * collide even if the underlying kernel only has one real backing
 * mechanism to offer.
 *
 * "Domain" here is an abstract, `Tag`-defined isolation boundary that is
 * neither a CPU (see `per_cpu_ptr.hpp`) nor a thread (see
 * `per_thread_ptr.hpp`), but some other small, fixed-size execution
 * context a kernel partitions state by -- e.g. Arm's Realm Management
 * Extension World ID (Secure / Non-secure / Realm / Root), Arm
 * TrustZone's Secure/Non-secure worlds, a RISC-V PMP/TEE domain, a
 * hypervisor's VM/guest id, or any other similarly-shaped "which
 * isolated context is this" index. Like CPUs (and unlike threads), the
 * domain set is typically small and fixed at boot, and resolving or
 * poking a *foreign* domain's slot from monitor/hypervisor code that
 * mediates between domains is a normal, well-defined operation -- so,
 * mirroring `per_cpu_ptr`, `per_domain_ptr` exposes both an explicit
 * `get(domain)`/`set(domain, ptr)` accessor and a "current domain"
 * `get()`/`set(ptr)` fast path.
 *
 * This class owns no memory and implements no per-domain storage
 * itself: `Tag` supplies the real per-domain container -- a linear
 * array indexed by domain id, a field inside each domain's saved
 * context structure, or anything else a concrete port wants -- via
 * `get_ptr()`/`set_ptr()`; `per_domain_ptr<Tag, T>` only `static_cast`s
 * the `Tag`'s type-erased `void*` to/from `T*`, so the exact mechanism
 * (and how "current" vs. an explicit foreign domain's slot is resolved)
 * is entirely `Tag`'s decision.
 *
 * `Tag` must provide:
 * - `static constexpr std::size_t max_domains;`
 * - `static void *get_ptr(DomainId domain) noexcept;`
 * - `static void set_ptr(DomainId domain, void *ptr) noexcept;`
 *
 * `Tag` must additionally provide *either* a dedicated "current domain"
 * fast path (used in preference when present):
 * - `static void *get_current_ptr() noexcept;`
 * - `static void set_current_ptr(void *ptr) noexcept;`
 *
 * *or*, lacking that fast path, a `current()` domain-id resolver that
 * `per_domain_ptr<Tag, T>::get()`/`set()` combine with `get_ptr()`/
 * `set_ptr()` to resolve the running domain's own slot:
 * - `static DomainId current() noexcept;`
 *
 * Example `Tag` (Arm RME World ID, flat array-backed; a real port would
 * instead forward to whatever structure the monitor already keeps
 * per-world state in):
 * @code
 * struct arm_rme_world_tag {
 *   enum class world_id : std::size_t { root = 0, secure = 1, realm = 2, normal = 3 };
 *   using domain_id_type = world_id;
 *
 *   static inline constexpr std::size_t max_domains = 4;
 *   static inline void *slots[max_domains]{nullptr};
 *
 *   static void *get_ptr(world_id w) noexcept { return slots[static_cast<std::size_t>(w)]; }
 *   static void set_ptr(world_id w, void *ptr) noexcept { slots[static_cast<std::size_t>(w)] = ptr; }
 *
 *   static world_id current() noexcept { return hw_current_world_id(); }
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
 * @brief Compile-time detectors for which optional "current domain"
 * fast-path members a `Tag` policy implements, used to SFINAE-select
 * between `Tag::get_current_ptr()`/`set_current_ptr()` and falling back
 * to `Tag::current()` combined with `get_ptr()`/`set_ptr()`.
 */
namespace detail {

/** @brief Resolves `Tag`'s domain-id type: `Tag::domain_id_type` if provided, else `std::size_t`. */
template <typename Tag, typename = void> struct per_domain_tag_domain_id {
  using type = std::size_t;
};

template <typename Tag>
struct per_domain_tag_domain_id<Tag, std::void_t<typename Tag::domain_id_type>> {
  using type = typename Tag::domain_id_type;
};

/** @brief True if `Tag::get_current_ptr()` is callable (dedicated current-domain fast path). */
template <typename Tag, typename = void> struct has_get_current_domain_ptr : std::false_type {};

template <typename Tag>
struct has_get_current_domain_ptr<Tag, std::void_t<decltype(Tag::get_current_ptr())>>
    : std::true_type {};

/** @brief True if `Tag::set_current_ptr(ptr)` is callable (dedicated current-domain fast path). */
template <typename Tag, typename = void> struct has_set_current_domain_ptr : std::false_type {};

template <typename Tag>
struct has_set_current_domain_ptr<Tag,
                                   std::void_t<decltype(Tag::set_current_ptr(std::declval<void *>()))>>
    : std::true_type {};

} // namespace detail

// -------------------------------------------------------------------------
// Per-Domain Typed Pointer Resolver
// -------------------------------------------------------------------------
/**
 * @brief Storage-free, type-safe resolver for a per-domain `T*`, keyed
 * by `Tag` and backed entirely by `Tag`'s own per-domain mechanism.
 *
 * @tparam Tag Policy supplying the real per-domain container; see the
 * file-level docs above for the full required/optional interface and an
 * example implementation.
 * @tparam T   Pointee type of the per-domain pointer this `Tag` resolves.
 */
template <typename Tag, typename T> class per_domain_ptr {
public:
  using tag_type = Tag;
  using value_type = T;
  using domain_id_type = typename detail::per_domain_tag_domain_id<Tag>::type;

  static inline constexpr std::size_t max_domains = Tag::max_domains;

  /** @brief Returns the `T*` registered for `domain` (nullptr if none was ever set). */
  [[nodiscard]] static T *get(domain_id_type domain) noexcept {
    return static_cast<T *>(Tag::get_ptr(domain));
  }

  /** @brief Registers `ptr` as `domain`'s slot (pass `nullptr` to clear it). */
  static void set(domain_id_type domain, T *ptr) noexcept {
    Tag::set_ptr(domain, static_cast<void *>(ptr));
  }

  /**
   * @brief Returns the running domain's `T*`. Uses `Tag::get_current_ptr()`
   * directly if provided; otherwise resolves `Tag::current()` and
   * delegates to `get(domain)`.
   */
  [[nodiscard]] static T *get() noexcept {
    if constexpr (detail::has_get_current_domain_ptr<Tag>::value) {
      return static_cast<T *>(Tag::get_current_ptr());
    } else {
      return get(Tag::current());
    }
  }

  /**
   * @brief Registers `ptr` as the running domain's slot. Uses
   * `Tag::set_current_ptr()` directly if provided; otherwise resolves
   * `Tag::current()` and delegates to `set(domain, ptr)`.
   */
  static void set(T *ptr) noexcept {
    if constexpr (detail::has_set_current_domain_ptr<Tag>::value) {
      Tag::set_current_ptr(static_cast<void *>(ptr));
    } else {
      set(Tag::current(), ptr);
    }
  }
};

} // namespace structo::arch
