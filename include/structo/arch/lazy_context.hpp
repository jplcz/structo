// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file lazy_context.hpp
 * @brief `structo::arch::lazy_context<Traits, CpuId>` +
 * `lazy_context_switcher<Traits, CpuId>`: a lazy/deferred hardware-
 * context switching framework for per-thread coprocessor/extension state
 * (FPU, vector-register files, debug registers, ...) that is expensive
 * to save/restore on every context switch and so should only be
 * saved/restored when a thread actually traps into using it.
 *
 * See the ASCII-art state-machine diagram below for the full state
 * transition map (`UNINITIALIZED` -> `ACTIVE_SILICON` <->
 * `CACHED_SILICON` -> `EVICTED_SYNC`/`EVICTED_DIRTY`). `static_per_cpu_storage`
 * is the bundled zero-allocation `Traits::get_active_context()` /
 * `set_active_context()` / `clear_active_context()` implementation (a
 * flat `Context*[MaxCpus]` array) most `Traits` types can simply inherit
 * from instead of reimplementing per-CPU residency tracking themselves.
 *
 * `Traits` must provide a `state_type` (the raw hardware register
 * block) plus `is_enabled()`, `enable()`, `disable()`,
 * `save_context(state_type&)`, `restore_context(state_type&)`, and the
 * three `*_active_context()` residency hooks (inherit
 * `static_per_cpu_storage` for these, or implement them directly).
 * Everything else -- `matches_trap()` (trap cascading to the correct
 * extension handler), `init_context()` (distinct first-touch
 * initialization vs. ordinary restore), `on_migrate()` (cross-CPU
 * migration notification), `on_thread_construct()`/`on_lazy_construct()`
 * (eager vs. deferred allocation) and both `destroy_context()` overloads
 * -- is optional and individually SFINAE-detected.
 *
 * Example `Traits` (a toy single-register "FPU" with eager construction,
 * no migration/init hooks -- see `lazy_context_switcher`'s own method
 * docs below for what each optional hook does):
 * @code
 * struct toy_fpu_traits : structo::arch::static_per_cpu_storage<8, void, std::size_t> {
 *   struct state_type {
 *     uint64_t fpcr = 0;
 *   };
 *
 *   static inline constexpr std::size_t invalid_cpu = static_cast<std::size_t>(-1);
 *
 *   static bool is_enabled() noexcept { return hw_fpu_is_enabled(); }
 *   static void enable() noexcept { hw_fpu_enable(); }
 *   static void disable() noexcept { hw_fpu_disable(); }
 *
 *   static void save_context(state_type &s) noexcept { s.fpcr = hw_fpu_read_fpcr(); }
 *   static void restore_context(const state_type &s) noexcept { hw_fpu_write_fpcr(s.fpcr); }
 * };
 * @endcode
 */

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include <reloco/allocator.hpp>
#include <reloco/array.hpp>
#include <reloco/error.hpp>

/*
                              [ Thread Creation ]
                                      |
                                      | on_thread_construct() (eager)
                                      v
                             +-----------------+
                             |  UNINITIALIZED  | <----+
                             | (RAM Allocated) |      | on_thread_remote_sync()
                             +-----------------+      | (marks dirty=true,
                                      |               |  keeps initialized=false)
                                      | on_trap()     |
             +------------------------+---------------+
             |
             | [First Use: init_context() / restore if dirty]
             v
+------------------------+      on_thread_leave()       +------------------------+
|     ACTIVE_SILICON     | ---------------------------> |     CACHED_SILICON     |
|   (HW Enabled on C)    |                              |   (HW Disabled on C)   |
|                        | <--------------------------- |                        |
| - active_on_cpu[C]==ctx|  on_thread_enter() on core C | - active_on_cpu[C]==ctx|
| - last_cpu == C        |  OR on_trap() on core C      | - last_cpu == C        |
| - dirty == false       |  (ZERO register reload!)     | - RAM is 100% in sync  |
+------------------------+                              +------------------------+
    |           ^                                                    |
    |           |                                                    | Another thread traps
    |           | on_trap() on core K                                | on core C (eviction)
    |           | (or after migration)                               | OR migration to core K
    |           | [restore_context()]                                v
    |           |                                       +------------------------+
    |           +-------------------------------------- |      EVICTED_SYNC      |
    |                                                   |    (Valid in Memory)   |
    | on_thread_local_sync()                            |                        |
    | (saves to RAM, disables HW,                       | - active_on_cpu[C]!=ctx|
    |  sets last_cpu = invalid)                         |   (or last_cpu==inv)   |
    |                                                   | - dirty == false       |
    +-------------------------------------------------> +------------------------+
                                                                     |
                                                                     | on_thread_remote_sync()
                                                                     | (e.g. ptrace / debugger)
                                                                     v
                                                        +------------------------+
                                                        |     EVICTED_DIRTY      |
                                                        |    (Memory Mutated)    |
                                                        |                        |
                                                        | - dirty == true        |
                                                        | - last_cpu == invalid  |
                                                        +------------------------+
                                                                     |
                                  on_trap()                          |
                                  [restore_context(), dirty = false] |
                                  +----------------------------------+
                                  |
                                  v
                        [ ACTIVE_SILICON ]

*/

namespace structo::arch {

template <typename Traits, typename CpuId> class lazy_context_switcher;

// -------------------------------------------------------------------------
// SFINAE Feature Detection for Traits & Hardware Context
// -------------------------------------------------------------------------
/**
 * @brief Compile-time detectors for which optional `Traits` members are
 * implemented, used to SFINAE-gate `lazy_context_switcher`'s matching
 * optional behavior (migration notification, dynamic construction,
 * distinct init-vs-restore, allocator-aware destruction, trap cascading)
 * so a `Traits` that only implements the required subset still compiles.
 */
namespace detail {

/** @brief Resolves `CpuId`'s sentinel "no CPU" value: `Traits::invalid_cpu` if provided, else `static_cast<CpuId>(-1)`.
 */
template <typename T, typename CpuId, typename = void> struct get_invalid_cpu {
  static constexpr CpuId value = static_cast<CpuId>(-1);
};

template <typename T, typename CpuId> struct get_invalid_cpu<T, CpuId, std::void_t<decltype(T::invalid_cpu)>> {
  static constexpr CpuId value = static_cast<CpuId>(T::invalid_cpu);
};

/** @brief True if `state_type` itself (rather than `Traits`) exposes `try_construct(alloc)`. */
template <typename S, typename Alloc = reloco::allocator_ref, typename = void>
struct state_has_try_construct : std::false_type {};

template <typename S, typename Alloc>
struct state_has_try_construct<S, Alloc,
                               std::void_t<decltype(std::declval<S &>().try_construct(std::declval<Alloc &>()))>>
    : std::true_type {};

/** @brief True if `Traits::try_construct(state, alloc)` is implemented. */
template <typename T, typename S, typename Alloc = reloco::allocator_ref, typename = void>
struct traits_has_try_construct : std::false_type {};

template <typename T, typename S, typename Alloc>
struct traits_has_try_construct<T, S, Alloc,
                                std::void_t<decltype(T::try_construct(std::declval<S &>(), std::declval<Alloc &>()))>>
    : std::true_type {};

/** @brief True if `Traits::on_thread_construct(state, alloc)` is implemented (eager per-thread construction hook). */
template <typename T, typename S, typename Alloc = reloco::allocator_ref, typename = void>
struct traits_has_on_thread_construct : std::false_type {};

template <typename T, typename S, typename Alloc>
struct traits_has_on_thread_construct<
    T, S, Alloc, std::void_t<decltype(T::on_thread_construct(std::declval<S &>(), std::declval<Alloc &>()))>>
    : std::true_type {};

/** @brief True if `Traits::on_lazy_construct(state, alloc)` is implemented (deferred, first-touch construction hook).
 */
template <typename T, typename S, typename Alloc = reloco::allocator_ref, typename = void>
struct traits_has_on_lazy_construct : std::false_type {};

template <typename T, typename S, typename Alloc>
struct traits_has_on_lazy_construct<
    T, S, Alloc, std::void_t<decltype(T::on_lazy_construct(std::declval<S &>(), std::declval<Alloc &>()))>>
    : std::true_type {};

/** @brief True if `Traits::destroy_context(state, alloc)` (allocator-aware overload) is implemented. */
template <typename T, typename S, typename Alloc = reloco::allocator_ref, typename = void>
struct traits_has_destroy_alloc : std::false_type {};

template <typename T, typename S, typename Alloc>
struct traits_has_destroy_alloc<T, S, Alloc,
                                std::void_t<decltype(T::destroy_context(std::declval<S &>(), std::declval<Alloc &>()))>>
    : std::true_type {};

/** @brief True if `Traits::destroy_context(state)` (plain, no-allocator overload) is implemented. */
template <typename T, typename S, typename = void> struct traits_has_destroy_plain : std::false_type {};

template <typename T, typename S>
struct traits_has_destroy_plain<T, S, std::void_t<decltype(T::destroy_context(std::declval<S &>()))>> : std::true_type {
};

/** @brief True if `Traits::init_context(state)` is implemented (distinct first-touch init vs. `restore_context()`). */
template <typename T, typename S, typename = void> struct traits_has_init_context : std::false_type {};

template <typename T, typename S>
struct traits_has_init_context<T, S, std::void_t<decltype(T::init_context(std::declval<S &>()))>> : std::true_type {};

/** @brief True if `Traits::on_migrate(ctx, from_cpu, to_cpu)` is implemented (cross-CPU migration notification). */
template <typename T, typename Ctx, typename Cpu, typename = void> struct traits_has_on_migrate : std::false_type {};

template <typename T, typename Ctx, typename Cpu>
struct traits_has_on_migrate<
    T, Ctx, Cpu, std::void_t<decltype(T::on_migrate(std::declval<Ctx &>(), std::declval<Cpu>(), std::declval<Cpu>()))>>
    : std::true_type {};

/** @brief True if `Traits`/`State` provides any form of deferred/on-demand construction (any of the three detectors
 * above). */
template <typename Traits, typename State> struct is_dynamically_constructed {
  static constexpr bool value = traits_has_try_construct<Traits, State>::value ||
                                traits_has_on_lazy_construct<Traits, State>::value ||
                                state_has_try_construct<State>::value;
};

/** @brief Dispatches to `Traits::matches_trap(fault_ctx)` if implemented; otherwise always matches (single-extension
 * systems). */
template <typename T, typename FC, typename = void> struct trap_matcher {
  static reloco::result<bool> match(const FC &) noexcept {
    // If trait doesn't provide a matcher, assume this block matches unconditionally
    return true;
  }
};

template <typename T, typename FC>
struct trap_matcher<T, FC, std::void_t<decltype(T::matches_trap(std::declval<const FC &>()))>> {
  static reloco::result<bool> match(const FC &fc) noexcept {
    using Ret = decltype(T::matches_trap(fc));
    if constexpr (std::is_same_v<Ret, bool>) {
      return T::matches_trap(fc);
    } else {
      return T::matches_trap(fc);
    }
  }
};

} // namespace detail

// -------------------------------------------------------------------------
// Lazy Context Storage Container
// -------------------------------------------------------------------------
/**
 * @brief Per-entity context structure embedded in a task or vcpu.
 *
 * Tracks the state machine from the file-level diagram above: whether
 * the backing `state_type` has been constructed at all
 * (`is_constructed()`), whether hardware registers have ever been
 * programmed from it (`is_initialized()`), which CPU last held it
 * resident (`last_cpu()`), and whether memory has been mutated out from
 * under active silicon (`is_dirty()`).
 *
 * @tparam Traits Policy defining hardware operations and state_type.
 * @tparam CpuId  Type representing the CPU core index.
 */
template <typename Traits, typename CpuId = std::size_t> class RELOCO_OWNER lazy_context {
public:
  using traits_type = Traits;
  using cpu_id_type = CpuId;
  using state_type = typename Traits::state_type;

  static inline constexpr CpuId invalid_cpu = detail::get_invalid_cpu<Traits, CpuId>::value;

  constexpr lazy_context() noexcept = default;

  // Context instances are pinned to physical thread structures
  lazy_context(const lazy_context &) = delete;
  lazy_context &operator=(const lazy_context &) = delete;
  lazy_context(lazy_context &&) = delete;
  lazy_context &operator=(lazy_context &&) = delete;

  /** @brief Which CPU last held this context resident (silicon or cached); `invalid_cpu` if never/severed. */
  [[nodiscard]] constexpr CpuId last_cpu() const noexcept { return m_last_cpu; }
  /** @brief Sets the recorded last-resident CPU (see `last_cpu()`). */
  constexpr void set_last_cpu(CpuId cpu) noexcept { m_last_cpu = cpu; }

  /** @brief Whether hardware registers have ever been programmed from `state()` (first touch has occurred). */
  [[nodiscard]] constexpr bool is_initialized() const noexcept { return m_initialized; }
  /** @brief Sets the initialized flag (see `is_initialized()`). */
  constexpr void set_initialized(bool init = true) noexcept { m_initialized = init; }

  /** @brief Whether `state()` itself has been constructed (relevant only for dynamically-constructed `Traits`). */
  [[nodiscard]] constexpr bool is_constructed() const noexcept { return m_constructed; }
  /** @brief Sets the constructed flag (see `is_constructed()`). */
  constexpr void set_constructed(bool constructed = true) noexcept { m_constructed = constructed; }

  /** @brief Whether `state()` has been mutated (e.g. by a debugger) since hardware was last synced from it. */
  [[nodiscard]] constexpr bool is_dirty() const noexcept { return m_dirty; }
  /** @brief Sets the dirty flag (see `is_dirty()`). */
  constexpr void set_dirty(bool dirty = true) noexcept { m_dirty = dirty; }

  /** @brief Access to the raw hardware state block (`Traits::state_type`). */
  [[nodiscard]] constexpr state_type &state() noexcept RELOCO_LIFETIMEBOUND { return m_state; }
  /** @brief `const`-qualified overload of `state()`. */
  [[nodiscard]] constexpr const state_type &state() const noexcept RELOCO_LIFETIMEBOUND { return m_state; }

  /**
   * @brief Resets the context back to its just-constructed state: severs
   * CPU residency, clears initialized/dirty, and restores `is_constructed()`
   * to its default for this `Traits` (eager `Traits` start constructed;
   * dynamically-constructed `Traits` start unconstructed).
   */
  constexpr void reset() noexcept {
    m_last_cpu = invalid_cpu;
    m_initialized = false;
    m_dirty = false;
    m_constructed = !detail::is_dynamically_constructed<Traits, state_type>::value;
  }

private:
  state_type m_state{};
  CpuId m_last_cpu{invalid_cpu};
  bool m_initialized{false};
  bool m_constructed{!detail::is_dynamically_constructed<Traits, state_type>::value};
  bool m_dirty{false};
};

// -------------------------------------------------------------------------
// Lazy Context Switcher Manager
// -------------------------------------------------------------------------
/**
 * @brief Stateless dispatcher implementing the lazy context-switching state
 * machine (see the file-level ASCII diagram) over a `lazy_context<Traits,
 * CpuId>`: every member is `static` and operates purely on the `context_type&`
 * (or `context_type*`) passed in, so one `lazy_context_switcher` instantiation
 * drives every thread's context for a given `Traits`.
 *
 * @note Invariant: every pointer this class reads back via
 * `Traits::get_active_context()` is used strictly for pointer-identity
 * comparison (`== &ctx`/`== ctx`) against the caller-supplied context, and
 * is never dereferenced -- the "active" context for a CPU may legitimately
 * be stale (already torn down, reused, or on another thread's stack) by the
 * time a given method runs, so only its identity, never its contents, may
 * be assumed valid. Keep this invariant in mind if extending any method
 * below: a method would need to become non-static and receive its context
 * by pointer (so a null/invalid context can be represented and checked)
 * only if it ever needed to dereference a `get_active_context()` result
 * directly; none currently do.
 *
 * @tparam Traits Policy defining hardware operations and `state_type`; see
 * the file-level docs above for the full required/optional interface and an
 * example implementation.
 * @tparam CpuId  Type representing the CPU core index.
 */
template <typename Traits, typename CpuId = std::size_t> class lazy_context_switcher {
public:
  using traits_type = Traits;
  using cpu_id_type = CpuId;
  using context_type = lazy_context<Traits, CpuId>;
  using state_type = typename Traits::state_type;

  static inline constexpr CpuId invalid_cpu = context_type::invalid_cpu;

  // -------------------------------------------------------------------------
  // Thread Construction
  // -------------------------------------------------------------------------
  /**
   * @brief Construct resources on thread creation.
   * Does nothing for subsystems requiring deferred/lazy allocations (e.g. RISC-V RVV).
   */
  static reloco::result<void> on_thread_construct(context_type &ctx, reloco::allocator_ref alloc) noexcept {
    if constexpr (detail::traits_has_on_thread_construct<Traits, state_type>::value) {
      auto res = Traits::on_thread_construct(ctx.state(), alloc);
      if (!res) {
        return res;
      }
      ctx.set_constructed(true);
      return {};
    } else {
      (void)ctx;
      (void)alloc;
      return {};
    }
  }

  // -------------------------------------------------------------------------
  // Lazy Construction
  // -------------------------------------------------------------------------
  /**
   * @brief Allocate/construct resources on-demand before registers are restored.
   * Safe to call multiple times (idempotent).
   */
  static reloco::result<void> on_lazy_construct(context_type &ctx, reloco::allocator_ref alloc) noexcept {
    if (ctx.is_constructed()) {
      return {};
    }

    if constexpr (detail::traits_has_on_lazy_construct<Traits, state_type>::value) {
      auto res = Traits::on_lazy_construct(ctx.state(), alloc);
      if (!res) {
        return res;
      }
    } else if constexpr (detail::traits_has_try_construct<Traits, state_type>::value) {
      auto res = Traits::try_construct(ctx.state(), alloc);
      if (!res) {
        return res;
      }
    } else if constexpr (detail::state_has_try_construct<state_type>::value) {
      auto res = ctx.state().try_construct(alloc);
      if (!res) {
        return res;
      }
    }

    ctx.set_constructed(true);
    return {};
  }

  // -------------------------------------------------------------------------
  // Trap Handling (Lazy Restore & Cascading)
  // -------------------------------------------------------------------------
  /**
   * @brief Dispatches coprocessor/extension traps for lazy restoration.
   *
   * @tparam FaultContext CPU frame/syndrome type passed from architecture vector.
   * @param ctx         Context of the thread that triggered the trap.
   * @param fault_ctx   Architectural exception frame.
   * @param current_cpu CPU executing the trapped instruction.
   *
   * @return reloco::result<bool>:
   *         - Ok(true): Trap belonged to this hardware block and was handled.
   *         - Ok(false): Trap was NOT for this block (allows cascading handlers).
   *         - Err: Architectural or state error detected.
   */
  template <typename FaultContext>
  static reloco::result<bool> on_trap(context_type &ctx, const FaultContext &fault_ctx, CpuId current_cpu) noexcept {
    // Verify if this fault belongs to this co-processor/extension
    auto match_res = detail::trap_matcher<Traits, FaultContext>::match(fault_ctx);
    if (!match_res) {
      return reloco::unexpected(match_res.error());
    }
    if (!*match_res) {
      return false; // Not our block; cascade to next extension handler
    }

    // Precondition check: state buffer must be constructed
    if (!ctx.is_constructed()) {
      return false;
    }

    // Identity-only: never dereferenced, only compared against &ctx below.
    context_type *active_on_cpu = static_cast<context_type *>(Traits::get_active_context(current_cpu));
    const CpuId prev_cpu = ctx.last_cpu();

    // Fast Path: Context is already resident in this CPU's register file
    if (active_on_cpu == &ctx && prev_cpu == current_cpu) {
      if (!Traits::is_enabled()) {
        Traits::enable();
      }
      if (ctx.is_dirty()) {
        Traits::restore_context(ctx.state());
        ctx.set_dirty(false);
      }
      return true;
    }

    // Migration notification if context ran on another CPU previously
    if (prev_cpu != invalid_cpu && prev_cpu != current_cpu) {
      if constexpr (detail::traits_has_on_migrate<Traits, context_type, CpuId>::value) {
        Traits::on_migrate(ctx, prev_cpu, current_cpu);
      }
    }

    // Enable hardware access to allow register programming
    if (!Traits::is_enabled()) {
      Traits::enable();
    }

    // Restore or initialize hardware registers
    if (ctx.is_initialized()) {
      Traits::restore_context(ctx.state());
    } else {
      if constexpr (detail::traits_has_init_context<Traits, state_type>::value) {
        Traits::init_context(ctx.state());
      } else {
        Traits::restore_context(ctx.state());
      }
      ctx.set_initialized(true);
    }

    // Bind ownership to this core
    ctx.set_dirty(false);
    ctx.set_last_cpu(current_cpu);
    Traits::set_active_context(current_cpu, static_cast<void *>(&ctx));

    return true;
  }

  // -------------------------------------------------------------------------
  // Thread Leave (Scheduled Out)
  // -------------------------------------------------------------------------
  /**
   * @brief Flushes registers to memory if active on this CPU, updates indices,
   *        and disables hardware access.
   */
  static void on_thread_leave(context_type *ctx, CpuId current_cpu) noexcept {
    if (ctx == nullptr || !ctx->is_initialized()) {
      return;
    }

    // Identity-only: never dereferenced, only compared against ctx below.
    context_type *active_on_cpu = static_cast<context_type *>(Traits::get_active_context(current_cpu));

    // Save only if this context actually held the hardware registers on this CPU
    if (active_on_cpu == ctx && Traits::is_enabled()) {
      Traits::save_context(ctx->state());
      ctx->set_last_cpu(current_cpu);
      // Retain per-CPU pointer so on_thread_enter can verify residency
      Traits::set_active_context(current_cpu, static_cast<void *>(ctx));
    }

    if (Traits::is_enabled()) {
      Traits::disable();
    }
  }

  // -------------------------------------------------------------------------
  // Thread Enter (Scheduled In)
  // -------------------------------------------------------------------------
  /**
   * @brief Optionally re-enables the block if this CPU and context match last ownership.
   *
   * @return true if hardware was resident and re-enabled without trap overhead;
   *         false if left disabled for lazy trapping.
   */
  static bool on_thread_enter(context_type *ctx, CpuId current_cpu) noexcept {
    if (ctx == nullptr || !ctx->is_initialized() || ctx->is_dirty()) {
      if (Traits::is_enabled()) {
        Traits::disable();
      }
      return false;
    }

    // Re-enable without trap if and only if:
    // This context was last run on this CPU.
    // This CPU's last active user was this context.
    const bool was_last_on_cpu = (ctx->last_cpu() == current_cpu);
    // Identity-only comparison; the returned pointer is never dereferenced.
    const bool cpu_has_this_ctx = (static_cast<context_type *>(Traits::get_active_context(current_cpu)) == ctx);

    if (was_last_on_cpu && cpu_has_this_ctx) {
      if (!Traits::is_enabled()) {
        Traits::enable();
      }
      return true;
    }

    if (Traits::is_enabled()) {
      Traits::disable();
    }
    return false;
  }

  // -------------------------------------------------------------------------
  // Thread Exit (Termination)
  // -------------------------------------------------------------------------
  /**
   * @brief Discards hardware tracking, unbinds per-CPU pointers, and releases memory.
   */
  static void on_thread_exit(context_type *ctx, CpuId current_cpu) noexcept {
    if (ctx == nullptr) {
      return;
    }

    // Clear per-CPU pointer if this CPU recorded ctx as last resident
    // (identity-only comparison; the returned pointer is never dereferenced).
    if (static_cast<context_type *>(Traits::get_active_context(current_cpu)) == ctx) {
      Traits::clear_active_context(current_cpu);
      if (Traits::is_enabled()) {
        Traits::disable();
      }
    }

    // Clear previous CPU's pointer if different and accessible
    const CpuId last = ctx->last_cpu();
    if (last != invalid_cpu && last != current_cpu) {
      if (static_cast<context_type *>(Traits::get_active_context(last)) == ctx) {
        Traits::clear_active_context(last);
      }
    }

    if constexpr (detail::traits_has_destroy_plain<Traits, state_type>::value) {
      Traits::destroy_context(ctx->state());
    }

    ctx->reset();
  }

  static void on_thread_exit(context_type *ctx, CpuId current_cpu, reloco::allocator_ref alloc) noexcept {
    if (ctx == nullptr) {
      return;
    }

    // Identity-only comparison; the returned pointer is never dereferenced.
    if (static_cast<context_type *>(Traits::get_active_context(current_cpu)) == ctx) {
      Traits::clear_active_context(current_cpu);
      if (Traits::is_enabled()) {
        Traits::disable();
      }
    }

    const CpuId last = ctx->last_cpu();
    if (last != invalid_cpu && last != current_cpu) {
      if (static_cast<context_type *>(Traits::get_active_context(last)) == ctx) {
        Traits::clear_active_context(last);
      }
    }

    if constexpr (detail::traits_has_destroy_alloc<Traits, state_type>::value) {
      Traits::destroy_context(ctx->state(), alloc);
    } else if constexpr (detail::traits_has_destroy_plain<Traits, state_type>::value) {
      Traits::destroy_context(ctx->state());
    }

    ctx->reset();
  }

  // -------------------------------------------------------------------------
  // Thread Local Sync (Kernel Query / In-Thread Mutation)
  // -------------------------------------------------------------------------
  /**
   * @brief Synchronizes hardware registers into ctx.state() from current thread POV.
   *
   * Flushes active silicon registers to memory if resident, disables hardware access,
   * and severs CPU residency. Ensures memory is the sole source of truth for
   * subsequent kernel inspection or in-memory mutation (e.g. signal delivery).
   */
  static void on_thread_local_sync(context_type &ctx, CpuId current_cpu) noexcept {
    // Identity-only: never dereferenced, only compared against &ctx below.
    context_type *active = static_cast<context_type *>(Traits::get_active_context(current_cpu));

    if (active == &ctx && Traits::is_enabled()) {
      Traits::save_context(ctx.state());
      Traits::disable();
      Traits::clear_active_context(current_cpu);
    }

    // Unconditionally sever residency so any subsequent mutation to ctx.state()
    // will force a fresh restore_context() when the thread resumes execution.
    ctx.set_last_cpu(invalid_cpu);
  }

  // -------------------------------------------------------------------------
  // Thread Remote Sync (Debugger / ptrace Mutation)
  // -------------------------------------------------------------------------
  /**
   * @brief Marks state as modified remotely while thread is descheduled.
   * Invalidates CPU residency to ensure fresh reload on subsequent activation.
   */
  static void on_thread_remote_sync(context_type &ctx) noexcept {
    ctx.set_dirty(true);
    ctx.set_last_cpu(invalid_cpu);
  }
};

// -------------------------------------------------------------------------
// Static Per-CPU Storage Helper
// -------------------------------------------------------------------------
/**
 * @brief Bundled zero-allocation implementation of the three
 * `*_active_context()` residency hooks `lazy_context_switcher` requires
 * from `Traits`: a flat `Context*[MaxCpus]` array, one slot per CPU.
 * Inherit from this (as in the `toy_fpu_traits` example in the
 * file-level docs above) instead of reimplementing per-CPU residency
 * tracking in every `Traits` type.
 *
 * @tparam MaxCpus Number of CPU slots to reserve (indices `[0, MaxCpus)`;
 * out-of-range `cpu` values passed to any method are silently ignored).
 * @tparam Context Context pointee type tracked per CPU (typically `void`,
 * type-erasing the actual `lazy_context<Traits, CpuId>` behind a `void*`
 * that `lazy_context_switcher` `static_cast`s back as needed).
 * @tparam CpuId Type representing the CPU core index.
 */
template <std::size_t MaxCpus, typename Context = void, typename CpuId = std::size_t> struct static_per_cpu_storage {
  static inline reloco::array<Context *, MaxCpus> active_contexts{};

  /** @brief Returns the context currently resident on `cpu`, or `nullptr` if none/out-of-range. */
  static inline Context *get_active_context(CpuId cpu) noexcept {
    const auto idx = static_cast<std::size_t>(cpu);
    return (idx < MaxCpus) ? active_contexts[idx] : nullptr;
  }

  /** @brief Records `ctx` as resident on `cpu`; a no-op if `cpu` is out of range. */
  static inline void set_active_context(CpuId cpu, Context *ctx) noexcept {
    const auto idx = static_cast<std::size_t>(cpu);
    if (idx < MaxCpus) {
      active_contexts[idx] = ctx;
    }
  }

  /** @brief Clears `cpu`'s resident-context pointer (sets it back to `nullptr`); a no-op if `cpu` is out of range. */
  static inline void clear_active_context(CpuId cpu) noexcept {
    const auto idx = static_cast<std::size_t>(cpu);
    if (idx < MaxCpus) {
      active_contexts[idx] = nullptr;
    }
  }
};

} // namespace structo::arch