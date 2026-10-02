// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/irqc_ref.hpp>

namespace {

using namespace structo;
using namespace structo::hw;

// --------------------------------------------------------------------
// A fake, fully-featured PIC backend: a single fixed `irq_source`
// (composed in, not derived from, per the @file-level docs) plus a few
// flags a test can inspect to confirm the optional hooks actually ran.
// Implements every optional `irqc_traits` member so one fixture can
// exercise the whole surface; `partial_pic` below implements only the
// mandatory subset to exercise the unsupported-operation fallback.
// --------------------------------------------------------------------
struct fake_pic {
  irq_source line{7};
  bool pre_ithread_called = false;
  bool post_ithread_called = false;
  bool post_filter_called = false;
};

struct partial_pic {
  irq_source line{3};
};

} // namespace

template <> struct structo::hw::irqc_traits<fake_pic> {
  static result<void> enable_intr(fake_pic &, irq_source &src) noexcept {
    src.set_enabled(true);
    return {};
  }
  static result<void> disable_intr(fake_pic &, irq_source &src) noexcept {
    src.set_enabled(false);
    return {};
  }
  static result<void> setup_intr(fake_pic &, irq_source &src, irq_config cfg,
                                 function_ref<void(irq_source &)> handler) noexcept {
    src.set_config(cfg);
    src.set_handler(handler);
    src.set_enabled(true);
    return {};
  }
  static result<void> teardown_intr(fake_pic &, irq_source &src) noexcept {
    src.clear_handler();
    src.set_enabled(false);
    return {};
  }
  static result<std::reference_wrapper<irq_source>> map_intr(fake_pic &b, const irq_map_data &data) noexcept {
    if (data.kind == irq_map_kind::gsi && data.gsi == b.line.irq())
      return std::ref(b.line);
    if (data.kind == irq_map_kind::fdt && data.fdt_cell_count > 0 && data.fdt_cells[0] == b.line.irq())
      return std::ref(b.line);
    return unexpected(error::not_found);
  }
  static result<void> assign_cpu(fake_pic &, irq_source &src, std::size_t cpu) noexcept {
    src.set_target_cpu(cpu);
    return {};
  }
  static result<void> pre_ithread(fake_pic &b, irq_source &) noexcept {
    b.pre_ithread_called = true;
    return {};
  }
  static result<void> post_ithread(fake_pic &b, irq_source &) noexcept {
    b.post_ithread_called = true;
    return {};
  }
  static result<void> post_filter(fake_pic &b, irq_source &) noexcept {
    b.post_filter_called = true;
    return {};
  }
  static result<irqc_capabilities> capabilities(fake_pic &) noexcept {
    irqc_capabilities caps;
    caps.max_sources = 64;
    caps.supports_fdt_mapping = true;
    caps.supports_gsi_mapping = true;
    caps.supports_affinity = true;
    caps.supports_percpu_sources = true;
    return caps;
  }
};

template <> struct structo::hw::irqc_traits<partial_pic> {
  static result<void> enable_intr(partial_pic &, irq_source &src) noexcept {
    src.set_enabled(true);
    return {};
  }
  static result<void> disable_intr(partial_pic &, irq_source &src) noexcept {
    src.set_enabled(false);
    return {};
  }
  static result<void> setup_intr(partial_pic &, irq_source &src, irq_config cfg,
                                 function_ref<void(irq_source &)> handler) noexcept {
    src.set_config(cfg);
    src.set_handler(handler);
    src.set_enabled(true);
    return {};
  }
  static result<void> teardown_intr(partial_pic &, irq_source &src) noexcept {
    src.clear_handler();
    src.set_enabled(false);
    return {};
  }
  static result<std::reference_wrapper<irq_source>> map_intr(partial_pic &b, const irq_map_data &data) noexcept {
    if (data.kind == irq_map_kind::gsi && data.gsi == b.line.irq())
      return std::ref(b.line);
    return unexpected(error::not_found);
  }
};

namespace {

struct IrqcRefTest : ::testing::Test {
  fake_pic backend;
  irqc_ref ref{backend};
};

} // namespace

TEST_F(IrqcRefTest, UnboundRefFailsEveryOperation) {
  irqc_ref unbound;
  irq_source src{1};
  EXPECT_FALSE(unbound.enable_intr(src));
  EXPECT_FALSE(unbound.disable_intr(src));
  EXPECT_FALSE(unbound.map_intr(irq_map_data::from_gsi(1)));
  EXPECT_FALSE(unbound.capabilities());
  EXPECT_FALSE(static_cast<bool>(unbound));
  EXPECT_TRUE(static_cast<bool>(ref));
}

TEST_F(IrqcRefTest, SetupIntrArmsConfigAndHandlerThenDispatchInvokesIt) {
  int fired = 0;
  irq_config cfg{irq_trigger::edge, irq_polarity::active_high};
  // Named local, not a temporary: `irq_source::set_handler`'s own docs
  // (and `function_ref`'s general contract) require the referenced
  // callable to outlive every `dispatch()` call, not merely the
  // `setup_intr` statement itself.
  auto handler = [&fired](irq_source &) { ++fired; };
  auto r = ref.setup_intr(backend.line, cfg, handler);
  ASSERT_TRUE(r);
  EXPECT_TRUE(backend.line.is_enabled());
  EXPECT_EQ(backend.line.config(), cfg);
  EXPECT_TRUE(backend.line.has_handler());

  backend.line.dispatch();
  backend.line.dispatch();
  EXPECT_EQ(fired, 2);
}

TEST_F(IrqcRefTest, DispatchWithoutHandlerIsASpuriousNoOp) {
  EXPECT_FALSE(backend.line.has_handler());
  backend.line.dispatch(); // must not trap
}

TEST_F(IrqcRefTest, TeardownIntrClearsHandlerAndDisables) {
  int fired = 0;
  auto handler = [&fired](irq_source &) { ++fired; };
  ASSERT_TRUE(ref.setup_intr(backend.line, irq_config{}, handler));
  ASSERT_TRUE(ref.teardown_intr(backend.line));
  EXPECT_FALSE(backend.line.is_enabled());
  EXPECT_FALSE(backend.line.has_handler());
  backend.line.dispatch();
  EXPECT_EQ(fired, 0);
}

TEST_F(IrqcRefTest, EnableDisableIntrToggleEnabledState) {
  ASSERT_TRUE(ref.disable_intr(backend.line));
  EXPECT_FALSE(backend.line.is_enabled());
  ASSERT_TRUE(ref.enable_intr(backend.line));
  EXPECT_TRUE(backend.line.is_enabled());
}

TEST_F(IrqcRefTest, MapIntrFromGsiResolvesTheRegisteredSource) {
  auto r = ref.map_intr(irq_map_data::from_gsi(7));
  ASSERT_TRUE(r);
  EXPECT_EQ(&r->get(), &backend.line);
}

TEST_F(IrqcRefTest, MapIntrFromFdtCellsResolvesTheRegisteredSource) {
  const std::uint32_t cells[] = {7, 0, 0};
  auto data = irq_map_data::try_from_fdt(reloco::span<const std::uint32_t>(cells, 3));
  ASSERT_TRUE(data);
  auto r = ref.map_intr(*data);
  ASSERT_TRUE(r);
  EXPECT_EQ(&r->get(), &backend.line);
}

TEST_F(IrqcRefTest, MapIntrFromFdtTooManyCellsFailsOutOfRange) {
  const std::uint32_t cells[] = {0, 0, 0, 0, 0};
  auto data = irq_map_data::try_from_fdt(reloco::span<const std::uint32_t>(cells, 5));
  ASSERT_FALSE(data);
  EXPECT_EQ(data.error(), error::out_of_range);
}

TEST_F(IrqcRefTest, MapIntrForUnknownSpecifierFailsNotFound) {
  auto r = ref.map_intr(irq_map_data::from_gsi(999));
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), error::not_found);
}

TEST_F(IrqcRefTest, AssignCpuRecordsAffinityOnTheSource) {
  ASSERT_TRUE(ref.assign_cpu(backend.line, 2));
  EXPECT_EQ(backend.line.target_cpu(), 2u);
}

TEST_F(IrqcRefTest, PreIthreadPostIthreadPostFilterForwardToBackend) {
  ASSERT_TRUE(ref.pre_ithread(backend.line));
  ASSERT_TRUE(ref.post_ithread(backend.line));
  ASSERT_TRUE(ref.post_filter(backend.line));
  EXPECT_TRUE(backend.pre_ithread_called);
  EXPECT_TRUE(backend.post_ithread_called);
  EXPECT_TRUE(backend.post_filter_called);
}

TEST_F(IrqcRefTest, CapabilitiesReportsBackendMetadata) {
  auto caps = ref.capabilities();
  ASSERT_TRUE(caps);
  EXPECT_EQ(caps->max_sources, 64u);
  EXPECT_TRUE(caps->supports_fdt_mapping);
  EXPECT_TRUE(caps->supports_gsi_mapping);
  EXPECT_FALSE(caps->supports_msi_mapping);
  EXPECT_TRUE(caps->supports_affinity);
  EXPECT_TRUE(caps->supports_percpu_sources);
}

TEST_F(IrqcRefTest, ScopedMaskDisablesThenRestoresPriorEnabledState) {
  ASSERT_TRUE(ref.enable_intr(backend.line));
  {
    auto guard = ref.mask_scoped(backend.line);
    EXPECT_FALSE(backend.line.is_enabled());
  }
  EXPECT_TRUE(backend.line.is_enabled());
}

TEST_F(IrqcRefTest, ScopedMaskLeavesAnAlreadyDisabledSourceDisabledAfterward) {
  ASSERT_TRUE(ref.disable_intr(backend.line));
  {
    auto guard = ref.mask_scoped(backend.line);
    EXPECT_FALSE(backend.line.is_enabled());
  }
  EXPECT_FALSE(backend.line.is_enabled());
}

TEST_F(IrqcRefTest, ScopedMaskUnlockRestoresEarlyAndIsIdempotent) {
  ASSERT_TRUE(ref.enable_intr(backend.line));
  auto guard = ref.mask_scoped(backend.line);
  EXPECT_FALSE(backend.line.is_enabled());
  guard.unlock();
  EXPECT_TRUE(backend.line.is_enabled());
  guard.unlock(); // idempotent, must not re-toggle anything
  EXPECT_TRUE(backend.line.is_enabled());
}

TEST(IrqcRefPartialBackendTest, OptionalOperationsFailUnsupportedWhenNotImplemented) {
  partial_pic backend;
  irqc_ref ref(backend);

  EXPECT_FALSE(ref.assign_cpu(backend.line, 1));
  EXPECT_FALSE(ref.pre_ithread(backend.line));
  EXPECT_FALSE(ref.post_ithread(backend.line));
  EXPECT_FALSE(ref.post_filter(backend.line));
  EXPECT_FALSE(ref.capabilities());

  // Mandatory operations still work on a backend that only implements those.
  // Named local: see the lifetime caveat noted in the earlier
  // `SetupIntrArmsConfigAndHandlerThenDispatchInvokesIt` test.
  int fired = 0;
  auto handler = [&fired](irq_source &) { ++fired; };
  ASSERT_TRUE(ref.setup_intr(backend.line, irq_config{}, handler));
  backend.line.dispatch();
  EXPECT_EQ(fired, 1);
}

TEST(IrqMapDataTest, FromMsiAndFromGsiPopulateTheExpectedKindAndField) {
  auto msi = irq_map_data::from_msi(42);
  EXPECT_EQ(msi.kind, irq_map_kind::msi);
  EXPECT_EQ(msi.msi_vector, 42u);

  auto gsi = irq_map_data::from_gsi(9);
  EXPECT_EQ(gsi.kind, irq_map_kind::gsi);
  EXPECT_EQ(gsi.gsi, 9u);
}

TEST(IrqSourceTest, DefaultsToDisabledWithNoHandlerAndZeroAffinity) {
  irq_source src{11};
  EXPECT_EQ(src.irq(), 11u);
  EXPECT_FALSE(src.is_enabled());
  EXPECT_FALSE(src.has_handler());
  EXPECT_EQ(src.target_cpu(), 0u);
  EXPECT_EQ(src.config(), irq_config{});
}
