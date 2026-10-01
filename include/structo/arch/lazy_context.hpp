// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include <reloco/allocator.hpp>
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
namespace detail {

template <typename T, typename CpuId, typename = void> struct get_invalid_cpu {
  static constexpr CpuId value = static_cast<CpuId>(-1);
};

template <typename T, typename CpuId> struct get_invalid_cpu<T, CpuId, std::void_t<decltype(T::invalid_cpu)>> {
  static constexpr CpuId value = static_cast<CpuId>(T::invalid_cpu);
};

template <typename S, typename Alloc = reloco::allocator_ref, typename = void>
struct state_has_try_construct : std::false_type {};

template <typename S, typename Alloc>
struct state_has_try_construct<S, Alloc,
                               std::void_t<decltype(std::declval<S &>().try_construct(std::declval<Alloc &>()))>>
    : std::true_type {};

template <typename T, typename S, typename Alloc = reloco::allocator_ref, typename = void>
struct traits_has_try_construct : std::false_type {};

template <typename T, typename S, typename Alloc>
struct traits_has_try_construct<T, S, Alloc,
                                std::void_t<decltype(T::try_construct(std::declval<S &>(), std::declval<Alloc &>()))>>
    : std::true_type {};

template <typename T, typename S, typename Alloc = reloco::allocator_ref, typename = void>
struct traits_has_on_thread_construct : std::false_type {};

template <typename T, typename S, typename Alloc>
struct traits_has_on_thread_construct<
    T, S, Alloc, std::void_t<decltype(T::on_thread_construct(std::declval<S &>(), std::declval<Alloc &>()))>>
    : std::true_type {};

template <typename T, typename S, typename Alloc = reloco::allocator_ref, typename = void>
struct traits_has_on_lazy_construct : std::false_type {};

template <typename T, typename S, typename Alloc>
struct traits_has_on_lazy_construct<
    T, S, Alloc, std::void_t<decltype(T::on_lazy_construct(std::declval<S &>(), std::declval<Alloc &>()))>>
    : std::true_type {};

template <typename T, typename S, typename Alloc = reloco::allocator_ref, typename = void>
struct traits_has_destroy_alloc : std::false_type {};

template <typename T, typename S, typename Alloc>
struct traits_has_destroy_alloc<T, S, Alloc,
                                std::void_t<decltype(T::destroy_context(std::declval<S &>(), std::declval<Alloc &>()))>>
    : std::true_type {};

template <typename T, typename S, typename = void> struct traits_has_destroy_plain : std::false_type {};

template <typename T, typename S>
struct traits_has_destroy_plain<T, S, std::void_t<decltype(T::destroy_context(std::declval<S &>()))>> : std::true_type {
};

template <typename T, typename S, typename = void> struct traits_has_init_context : std::false_type {};

template <typename T, typename S>
struct traits_has_init_context<T, S, std::void_t<decltype(T::init_context(std::declval<S &>()))>> : std::true_type {};

template <typename T, typename Ctx, typename Cpu, typename = void> struct traits_has_on_migrate : std::false_type {};

template <typename T, typename Ctx, typename Cpu>
struct traits_has_on_migrate<
    T, Ctx, Cpu, std::void_t<decltype(T::on_migrate(std::declval<Ctx &>(), std::declval<Cpu>(), std::declval<Cpu>()))>>
    : std::true_type {};

template <typename Traits, typename State> struct is_dynamically_constructed {
  static constexpr bool value = traits_has_try_construct<Traits, State>::value ||
                                traits_has_on_lazy_construct<Traits, State>::value ||
                                state_has_try_construct<State>::value;
};

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

  [[nodiscard]] constexpr CpuId last_cpu() const noexcept { return m_last_cpu; }
  constexpr void set_last_cpu(CpuId cpu) noexcept { m_last_cpu = cpu; }

  [[nodiscard]] constexpr bool is_initialized() const noexcept { return m_initialized; }
  constexpr void set_initialized(bool init = true) noexcept { m_initialized = init; }

  [[nodiscard]] constexpr bool is_constructed() const noexcept { return m_constructed; }
  constexpr void set_constructed(bool constructed = true) noexcept { m_constructed = constructed; }

  [[nodiscard]] constexpr bool is_dirty() const noexcept { return m_dirty; }
  constexpr void set_dirty(bool dirty = true) noexcept { m_dirty = dirty; }

  [[nodiscard]] constexpr state_type &state() noexcept RELOCO_LIFETIMEBOUND { return m_state; }
  [[nodiscard]] constexpr const state_type &state() const noexcept RELOCO_LIFETIMEBOUND { return m_state; }

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
template <std::size_t MaxCpus, typename Context = void, typename CpuId = std::size_t> struct static_per_cpu_storage {
  static inline Context *active_contexts[MaxCpus]{nullptr};

  static inline Context *get_active_context(CpuId cpu) noexcept {
    const auto idx = static_cast<std::size_t>(cpu);
    return (idx < MaxCpus) ? active_contexts[idx] : nullptr;
  }

  static inline void set_active_context(CpuId cpu, Context *ctx) noexcept {
    const auto idx = static_cast<std::size_t>(cpu);
    if (idx < MaxCpus) {
      active_contexts[idx] = ctx;
    }
  }

  static inline void clear_active_context(CpuId cpu) noexcept {
    const auto idx = static_cast<std::size_t>(cpu);
    if (idx < MaxCpus) {
      active_contexts[idx] = nullptr;
    }
  }
};

} // namespace structo::arch