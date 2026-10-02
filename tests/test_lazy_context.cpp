// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/arch/lazy_context.hpp>

#include <cstddef>

#include <reloco/allocator.hpp>

namespace {

// Minimal architectural fault frame carrying only what trap_matcher and
// cpu_index-style from_context() hooks would need in a real port.
struct trap_frame {
  std::size_t cpu = 0;
};

// "Eager" traits: no dynamic construction, no init_context hook, no
// migration hook -- exercises lazy_context_switcher's fallback paths
// (restore_context() doubling as the first-touch initializer,
// is_dynamically_constructed() == false so the context starts already
// "constructed", and the plain/no-alloc on_thread_exit() overload).
struct minimal_traits : structo::arch::static_per_cpu_storage<4, void, std::size_t> {
  struct state_type {
    int value = 0;
  };

  static inline constexpr std::size_t invalid_cpu = static_cast<std::size_t>(-1);

  static inline bool enabled = false;
  static inline bool should_match = true;
  static inline int enable_calls = 0;
  static inline int disable_calls = 0;
  static inline int save_calls = 0;
  static inline int restore_calls = 0;

  static void reset() noexcept {
    enabled = false;
    should_match = true;
    enable_calls = disable_calls = save_calls = restore_calls = 0;
    for (std::size_t i = 0; i < 4; ++i) {
      clear_active_context(i);
    }
  }

  static bool is_enabled() noexcept { return enabled; }
  static void enable() noexcept {
    enabled = true;
    ++enable_calls;
  }
  static void disable() noexcept {
    enabled = false;
    ++disable_calls;
  }

  static void save_context(state_type &) noexcept { ++save_calls; }
  static void restore_context(state_type &) noexcept { ++restore_calls; }

  static bool matches_trap(const trap_frame &) noexcept { return should_match; }
};

// "Full-featured" traits: dynamically constructed state (via
// on_lazy_construct/on_thread_construct), an explicit init_context hook
// distinct from restore_context, migration notification, and the
// allocator-aware destroy_context() overload -- exercises every optional
// SFINAE-detected hook lazy_context_switcher supports.
struct full_traits : structo::arch::static_per_cpu_storage<8, void, std::size_t> {
  struct state_type {
    int value = 0;
  };

  static inline constexpr std::size_t invalid_cpu = static_cast<std::size_t>(-1);

  static inline bool enabled = false;
  static inline int enable_calls = 0;
  static inline int disable_calls = 0;
  static inline int save_calls = 0;
  static inline int restore_calls = 0;
  static inline int init_calls = 0;
  static inline int thread_construct_calls = 0;
  static inline int lazy_construct_calls = 0;
  static inline int destroy_alloc_calls = 0;
  static inline int migrate_calls = 0;
  static inline std::size_t migrate_from = invalid_cpu;
  static inline std::size_t migrate_to = invalid_cpu;

  static void reset() noexcept {
    enabled = false;
    enable_calls = disable_calls = save_calls = restore_calls = init_calls = 0;
    thread_construct_calls = lazy_construct_calls = destroy_alloc_calls = migrate_calls = 0;
    migrate_from = migrate_to = invalid_cpu;
    for (std::size_t i = 0; i < 8; ++i) {
      clear_active_context(i);
    }
  }

  static bool is_enabled() noexcept { return enabled; }
  static void enable() noexcept {
    enabled = true;
    ++enable_calls;
  }
  static void disable() noexcept {
    enabled = false;
    ++disable_calls;
  }

  static void save_context(state_type &) noexcept { ++save_calls; }
  static void restore_context(state_type &) noexcept { ++restore_calls; }
  static void init_context(state_type &s) noexcept {
    ++init_calls;
    s.value = 100;
  }

  static reloco::result<void> on_thread_construct(state_type &, reloco::allocator_ref) noexcept {
    ++thread_construct_calls;
    return {};
  }

  static reloco::result<void> on_lazy_construct(state_type &, reloco::allocator_ref) noexcept {
    ++lazy_construct_calls;
    return {};
  }

  static void destroy_context(state_type &, reloco::allocator_ref) noexcept { ++destroy_alloc_calls; }

  static void on_migrate(structo::arch::lazy_context<full_traits, std::size_t> &, std::size_t from,
                         std::size_t to) noexcept {
    ++migrate_calls;
    migrate_from = from;
    migrate_to = to;
  }
};

} // namespace

namespace {

/** @brief Fixture for `lazy_context`/`lazy_context_switcher<minimal_traits>` tests; resets `minimal_traits`'s mutable
 * static state before each test. */
class LazyContextMinimalTraitsTest : public ::testing::Test {
protected:
  void SetUp() override { minimal_traits::reset(); }
};

/** @brief Fixture for `lazy_context`/`lazy_context_switcher<full_traits>` tests; resets `full_traits`'s mutable static
 * state before each test. */
class LazyContextFullTraitsTest : public ::testing::Test {
protected:
  void SetUp() override { full_traits::reset(); }
};

/** @brief Fixture for `static_per_cpu_storage` tests; no shared mutable state to reset beyond each test's own local
 * instance. */
class StaticPerCpuStorageTest : public ::testing::Test {};

} // namespace

// ---------------------------------------------------------------------------
// minimal_traits: eager construction, restore_context()-as-initializer
// ---------------------------------------------------------------------------

TEST_F(LazyContextMinimalTraitsTest, DefaultConstructedContextStartsConstructedWhenNoDynamicConstructionNeeded) {
  structo::arch::lazy_context<minimal_traits> ctx;

  EXPECT_TRUE(ctx.is_constructed());
  EXPECT_FALSE(ctx.is_initialized());
  EXPECT_FALSE(ctx.is_dirty());
  EXPECT_EQ(ctx.last_cpu(), minimal_traits::invalid_cpu);
}

TEST_F(LazyContextMinimalTraitsTest, OnTrapInitializesHardwareLazilyOnFirstTouch) {
  using switcher = structo::arch::lazy_context_switcher<minimal_traits>;
  switcher::context_type ctx;

  auto res = switcher::on_trap(ctx, trap_frame{0}, std::size_t{0});

  ASSERT_TRUE(res);
  EXPECT_TRUE(*res);
  EXPECT_TRUE(ctx.is_initialized());
  EXPECT_EQ(ctx.last_cpu(), 0u);
  EXPECT_TRUE(minimal_traits::enabled);
  EXPECT_EQ(minimal_traits::enable_calls, 1);
  // No init_context hook: restore_context() doubles as the initializer.
  EXPECT_EQ(minimal_traits::restore_calls, 1);
}

TEST_F(LazyContextMinimalTraitsTest, OnTrapFastPathReusesResidentRegistersWithoutReload) {
  using switcher = structo::arch::lazy_context_switcher<minimal_traits>;
  switcher::context_type ctx;
  ASSERT_TRUE(switcher::on_trap(ctx, trap_frame{0}, std::size_t{0}));
  const int restores_before = minimal_traits::restore_calls;

  auto res = switcher::on_trap(ctx, trap_frame{0}, std::size_t{0});

  ASSERT_TRUE(res);
  EXPECT_TRUE(*res);
  EXPECT_EQ(minimal_traits::restore_calls, restores_before); // zero register reload
}

TEST_F(LazyContextMinimalTraitsTest, OnTrapCascadesWhenMatcherRejectsTheFaultWithoutTouchingHardware) {
  minimal_traits::should_match = false;
  using switcher = structo::arch::lazy_context_switcher<minimal_traits>;
  switcher::context_type ctx;

  auto res = switcher::on_trap(ctx, trap_frame{0}, std::size_t{0});

  ASSERT_TRUE(res);
  EXPECT_FALSE(*res); // not for this block: cascade to the next handler
  EXPECT_FALSE(ctx.is_initialized());
  EXPECT_EQ(minimal_traits::enable_calls, 0);
}

TEST_F(LazyContextMinimalTraitsTest, OnThreadLeaveSavesWhenResidentAndDisablesHardware) {
  using switcher = structo::arch::lazy_context_switcher<minimal_traits>;
  switcher::context_type ctx;
  ASSERT_TRUE(switcher::on_trap(ctx, trap_frame{0}, std::size_t{0}));

  switcher::on_thread_leave(&ctx, 0);

  EXPECT_EQ(minimal_traits::save_calls, 1);
  EXPECT_FALSE(minimal_traits::enabled);
}

TEST_F(LazyContextMinimalTraitsTest, OnThreadLeaveIsANoOpForAContextThatWasNeverInitialized) {
  using switcher = structo::arch::lazy_context_switcher<minimal_traits>;
  switcher::context_type ctx;

  switcher::on_thread_leave(&ctx, 0);
  switcher::on_thread_leave(nullptr, 0);

  EXPECT_EQ(minimal_traits::save_calls, 0);
  EXPECT_EQ(minimal_traits::disable_calls, 0);
}

TEST_F(LazyContextMinimalTraitsTest, OnThreadEnterReEnablesWithoutTrapWhenStillResidentOnSameCpu) {
  using switcher = structo::arch::lazy_context_switcher<minimal_traits>;
  switcher::context_type ctx;
  ASSERT_TRUE(switcher::on_trap(ctx, trap_frame{0}, std::size_t{0}));
  switcher::on_thread_leave(&ctx, 0);
  ASSERT_FALSE(minimal_traits::enabled);

  const bool resumed = switcher::on_thread_enter(&ctx, 0);

  EXPECT_TRUE(resumed);
  EXPECT_TRUE(minimal_traits::enabled);
}

TEST_F(LazyContextMinimalTraitsTest, OnThreadEnterLeavesDisabledWhenContextIsDirtyOrNotResident) {
  using switcher = structo::arch::lazy_context_switcher<minimal_traits>;
  switcher::context_type ctx;

  EXPECT_FALSE(switcher::on_thread_enter(&ctx, 0)); // never initialized yet

  ASSERT_TRUE(switcher::on_trap(ctx, trap_frame{0}, std::size_t{0}));
  switcher::on_thread_remote_sync(ctx); // marks dirty, severs residency

  EXPECT_FALSE(switcher::on_thread_enter(&ctx, 0));
  EXPECT_FALSE(minimal_traits::enabled);
}

TEST_F(LazyContextMinimalTraitsTest, OnThreadLocalSyncFlushesRegistersAndSeversResidency) {
  using switcher = structo::arch::lazy_context_switcher<minimal_traits>;
  switcher::context_type ctx;
  ASSERT_TRUE(switcher::on_trap(ctx, trap_frame{0}, std::size_t{0}));

  switcher::on_thread_local_sync(ctx, 0);

  EXPECT_EQ(minimal_traits::save_calls, 1);
  EXPECT_FALSE(minimal_traits::enabled);
  EXPECT_EQ(ctx.last_cpu(), minimal_traits::invalid_cpu);
}

TEST_F(LazyContextMinimalTraitsTest, OnThreadRemoteSyncMarksDirtyWithoutTouchingHardwareAndForcesFreshRestore) {
  using switcher = structo::arch::lazy_context_switcher<minimal_traits>;
  switcher::context_type ctx;
  ASSERT_TRUE(switcher::on_trap(ctx, trap_frame{0}, std::size_t{0}));
  const int saves_before = minimal_traits::save_calls;

  switcher::on_thread_remote_sync(ctx);

  EXPECT_TRUE(ctx.is_dirty());
  EXPECT_EQ(ctx.last_cpu(), minimal_traits::invalid_cpu);
  EXPECT_EQ(minimal_traits::save_calls, saves_before); // no hardware touched directly

  // Even though this CPU previously held the context, the dirty flag must
  // force a fresh restore rather than taking the fast path.
  const int restores_before = minimal_traits::restore_calls;
  auto res = switcher::on_trap(ctx, trap_frame{0}, std::size_t{0});
  ASSERT_TRUE(res);
  EXPECT_TRUE(*res);
  EXPECT_FALSE(ctx.is_dirty());
  EXPECT_EQ(minimal_traits::restore_calls, restores_before + 1);
}

TEST_F(LazyContextMinimalTraitsTest, OnThreadExitClearsActiveContextAndResetsState) {
  using switcher = structo::arch::lazy_context_switcher<minimal_traits>;
  switcher::context_type ctx;
  ASSERT_TRUE(switcher::on_trap(ctx, trap_frame{0}, std::size_t{0}));

  switcher::on_thread_exit(&ctx, 0);

  EXPECT_FALSE(ctx.is_initialized());
  EXPECT_EQ(ctx.last_cpu(), minimal_traits::invalid_cpu);
  EXPECT_EQ(minimal_traits::get_active_context(0), nullptr);
}

// ---------------------------------------------------------------------------
// full_traits: dynamic construction, init_context, migration, alloc-aware
// destroy_context()
// ---------------------------------------------------------------------------

TEST_F(LazyContextFullTraitsTest, DynamicallyConstructedContextStartsUnconstructed) {
  structo::arch::lazy_context<full_traits> ctx;
  EXPECT_FALSE(ctx.is_constructed());
}

TEST_F(LazyContextFullTraitsTest, OnThreadConstructInvokesHookAndMarksContextConstructed) {
  using switcher = structo::arch::lazy_context_switcher<full_traits>;
  switcher::context_type ctx;
  reloco::allocator_ref alloc;

  auto res = switcher::on_thread_construct(ctx, alloc);

  ASSERT_TRUE(res);
  EXPECT_EQ(full_traits::thread_construct_calls, 1);
  EXPECT_TRUE(ctx.is_constructed());
}

TEST_F(LazyContextFullTraitsTest, OnLazyConstructIsIdempotentAfterTheFirstSuccess) {
  using switcher = structo::arch::lazy_context_switcher<full_traits>;
  switcher::context_type ctx;
  reloco::allocator_ref alloc;

  ASSERT_TRUE(switcher::on_lazy_construct(ctx, alloc));
  EXPECT_EQ(full_traits::lazy_construct_calls, 1);
  EXPECT_TRUE(ctx.is_constructed());

  ASSERT_TRUE(switcher::on_lazy_construct(ctx, alloc));
  EXPECT_EQ(full_traits::lazy_construct_calls, 1); // already constructed: no-op
}

TEST_F(LazyContextFullTraitsTest, OnTrapCascadesUntilLazyConstructHasRunForDynamicallyConstructedState) {
  using switcher = structo::arch::lazy_context_switcher<full_traits>;
  switcher::context_type ctx;
  ASSERT_FALSE(ctx.is_constructed());

  auto before = switcher::on_trap(ctx, trap_frame{0}, std::size_t{0});
  ASSERT_TRUE(before);
  EXPECT_FALSE(*before); // state buffer not constructed yet: cascades away
  EXPECT_EQ(full_traits::enable_calls, 0);

  reloco::allocator_ref alloc;
  ASSERT_TRUE(switcher::on_lazy_construct(ctx, alloc));

  auto after = switcher::on_trap(ctx, trap_frame{0}, std::size_t{0});
  ASSERT_TRUE(after);
  EXPECT_TRUE(*after);
}

TEST_F(LazyContextFullTraitsTest, OnTrapUsesInitContextHookInsteadOfRestoreContextOnFirstTouch) {
  using switcher = structo::arch::lazy_context_switcher<full_traits>;
  switcher::context_type ctx;
  reloco::allocator_ref alloc;
  ASSERT_TRUE(switcher::on_thread_construct(ctx, alloc));

  auto res = switcher::on_trap(ctx, trap_frame{0}, std::size_t{0});

  ASSERT_TRUE(res);
  EXPECT_TRUE(*res);
  EXPECT_EQ(full_traits::init_calls, 1);
  EXPECT_EQ(full_traits::restore_calls, 0);
  EXPECT_EQ(ctx.state().value, 100);
}

TEST_F(LazyContextFullTraitsTest, OnTrapNotifiesMigrationWhenContextMovesToADifferentCpu) {
  using switcher = structo::arch::lazy_context_switcher<full_traits>;
  switcher::context_type ctx;
  reloco::allocator_ref alloc;
  ASSERT_TRUE(switcher::on_thread_construct(ctx, alloc));
  ASSERT_TRUE(switcher::on_trap(ctx, trap_frame{0}, std::size_t{0}));
  switcher::on_thread_leave(&ctx, 0);

  auto res = switcher::on_trap(ctx, trap_frame{1}, std::size_t{1});

  ASSERT_TRUE(res);
  EXPECT_TRUE(*res);
  EXPECT_EQ(full_traits::migrate_calls, 1);
  EXPECT_EQ(full_traits::migrate_from, 0u);
  EXPECT_EQ(full_traits::migrate_to, 1u);
}

TEST_F(LazyContextFullTraitsTest, OnThreadExitWithAllocatorPrefersTheAllocAwareDestroyHook) {
  using switcher = structo::arch::lazy_context_switcher<full_traits>;
  switcher::context_type ctx;
  reloco::allocator_ref alloc;
  ASSERT_TRUE(switcher::on_thread_construct(ctx, alloc));
  ASSERT_TRUE(switcher::on_trap(ctx, trap_frame{0}, std::size_t{0}));

  switcher::on_thread_exit(&ctx, 0, alloc);

  EXPECT_EQ(full_traits::destroy_alloc_calls, 1);
  EXPECT_FALSE(ctx.is_initialized());
  EXPECT_EQ(full_traits::get_active_context(0), nullptr);
}

// ---------------------------------------------------------------------------
// static_per_cpu_storage
// ---------------------------------------------------------------------------

TEST_F(StaticPerCpuStorageTest, OutOfRangeCpuIndicesAreIgnoredRatherThanCorrupingMemory) {
  structo::arch::static_per_cpu_storage<2, int> storage;
  int value = 7;

  storage.set_active_context(5, &value); // out of range: silently ignored
  EXPECT_EQ(storage.get_active_context(5), nullptr);

  storage.set_active_context(1, &value);
  EXPECT_EQ(storage.get_active_context(1), &value);

  storage.clear_active_context(5); // out of range: silently ignored
  EXPECT_EQ(storage.get_active_context(1), &value);

  storage.clear_active_context(1);
  EXPECT_EQ(storage.get_active_context(1), nullptr);
}
