// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file per_thread_ptr.hpp
 * @brief `structo::arch::per_thread_ptr<Tag, T>`: a thin, storage-free,
 * type-safe resolver for the *running thread's own* pointer, keyed by a
 * caller-defined `Tag` type so unrelated per-thread pointers of
 * different `T` never collide even if the underlying kernel only has
 * one real backing mechanism to offer.
 *
 * This is `per_cpu_ptr<Tag, T>`'s per-thread sibling (see
 * `per_cpu_ptr.hpp`), with the same storage-free philosophy: it owns no
 * memory and implements no per-thread storage itself. Unlike
 * `per_cpu_ptr`, it deliberately exposes **no** explicit-thread
 * accessor -- a CPU set is small, fixed-size, and reading/poking a
 * different CPU's slot from afar is a routine, well-defined kernel
 * operation (e.g. IPI targeting), but reaching into an *arbitrary other
 * thread's* private slot without that thread's own synchronization is
 * rarely safe and easy to get wrong. `per_thread_ptr<Tag, T>` therefore
 * only ever resolves the calling thread's own slot via `get()`/`set(ptr)`.
 *
 * `Tag` must provide *either* a dedicated "current thread" fast path
 * (used in preference when present -- the natural choice when the
 * backing storage is itself thread-local, e.g. `__thread`/TLS):
 * - `static void *get_current_ptr() noexcept;`
 * - `static void set_current_ptr(void *ptr) noexcept;`
 *
 * *or*, lacking that fast path, a `current()` thread-id resolver plus
 * `get_ptr(tid)`/`set_ptr(tid, ptr)` that `per_thread_ptr<Tag,
 * T>::get()`/`set()` combine internally to resolve the running thread's
 * own slot (e.g. an intrusive field inside the kernel's own task/thread
 * control block, resolved via the scheduler's own "current thread"
 * lookup -- never used to reach into any *other* thread's slot):
 * - `static ThreadId current() noexcept;`
 * - `static void *get_ptr(ThreadId tid) noexcept;`
 * - `static void set_ptr(ThreadId tid, void *ptr) noexcept;`
 *
 * Example `Tag` (an intrusive-field resolver over a caller-defined task
 * control block; a real kernel port would resolve `current()` via its
 * own scheduler, e.g. a per-CPU "current task" pointer):
 * @code
 * struct my_tcb { void *per_thread_slot = nullptr; };
 *
 * struct my_kernel_per_thread_tag {
 *   using thread_id_type = my_tcb *;
 *
 *   static my_tcb *current() noexcept { return hw_current_task_control_block(); }
 *
 *   static void *get_ptr(my_tcb *tid) noexcept { return tid->per_thread_slot; }
 *   static void set_ptr(my_tcb *tid, void *ptr) noexcept { tid->per_thread_slot = ptr; }
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
 * @brief Compile-time detectors for which optional "current thread" fast-path
 * members a `Tag` policy implements, used to SFINAE-select between
 * `Tag::get_current_ptr()`/`set_current_ptr()` and falling back to
 * `Tag::current()` combined with `get_ptr()`/`set_ptr()`.
 */
namespace detail {

/** @brief Resolves `Tag`'s thread-id type: `Tag::thread_id_type` if provided, else `std::size_t`. */
template <typename Tag, typename = void> struct per_thread_tag_thread_id {
  using type = std::size_t;
};

template <typename Tag> struct per_thread_tag_thread_id<Tag, std::void_t<typename Tag::thread_id_type>> {
  using type = typename Tag::thread_id_type;
};

/** @brief True if `Tag::get_current_ptr()` is callable (dedicated current-thread fast path). */
template <typename Tag, typename = void> struct has_get_current_thread_ptr : std::false_type {};

template <typename Tag>
struct has_get_current_thread_ptr<Tag, std::void_t<decltype(Tag::get_current_ptr())>> : std::true_type {};

/** @brief True if `Tag::set_current_ptr(ptr)` is callable (dedicated current-thread fast path). */
template <typename Tag, typename = void> struct has_set_current_thread_ptr : std::false_type {};

template <typename Tag>
struct has_set_current_thread_ptr<Tag, std::void_t<decltype(Tag::set_current_ptr(std::declval<void *>()))>>
    : std::true_type {};

} // namespace detail

// -------------------------------------------------------------------------
// Per-Thread Typed Pointer Resolver (Current Thread Only)
// -------------------------------------------------------------------------
/**
 * @brief Storage-free, type-safe resolver for the *running thread's own*
 * `T*`, keyed by `Tag` and backed entirely by `Tag`'s own per-thread
 * mechanism.
 *
 * Deliberately exposes no explicit-thread-id accessor: only the calling
 * thread's own slot is ever reachable through this class (see the
 * file-level docs above for why).
 *
 * @tparam Tag Policy supplying the real per-thread container; see the
 * file-level docs above for the full required/optional interface and an
 * example implementation.
 * @tparam T   Pointee type of the per-thread pointer this `Tag` resolves.
 */
template <typename Tag, typename T> class per_thread_ptr {
public:
  using tag_type = Tag;
  using value_type = T;
  using thread_id_type = typename detail::per_thread_tag_thread_id<Tag>::type;

  /**
   * @brief Returns the calling thread's `T*` (nullptr if none was ever
   * set). Uses `Tag::get_current_ptr()` directly if provided; otherwise
   * resolves `Tag::current()` and delegates to `Tag::get_ptr(tid)`.
   */
  [[nodiscard]] static T *get() noexcept {
    if constexpr (detail::has_get_current_thread_ptr<Tag>::value) {
      return static_cast<T *>(Tag::get_current_ptr());
    } else {
      return static_cast<T *>(Tag::get_ptr(Tag::current()));
    }
  }

  /**
   * @brief Registers `ptr` as the calling thread's slot (pass `nullptr`
   * to clear it). Uses `Tag::set_current_ptr()` directly if provided;
   * otherwise resolves `Tag::current()` and delegates to
   * `Tag::set_ptr(tid, ptr)`.
   */
  static void set(T *ptr) noexcept {
    if constexpr (detail::has_set_current_thread_ptr<Tag>::value) {
      Tag::set_current_ptr(static_cast<void *>(ptr));
    } else {
      Tag::set_ptr(Tag::current(), static_cast<void *>(ptr));
    }
  }
};

} // namespace structo::arch
