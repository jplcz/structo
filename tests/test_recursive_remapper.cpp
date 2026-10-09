// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>

#include <structo/arch/arm64/recursive_format.hpp>
#include <structo/arch/page_table_memory.hpp>
#include <structo/arch/recursive_remapper.hpp>
#include <structo/arch/x86/recursive_format.hpp>

#include <vector>

RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

namespace {

using namespace structo::arch;
using reloco::error;

struct rec_tlb {
  static inline int flushes = 0;
  static inline int completes = 0;
  static void flush(std::uint64_t, std::uint64_t) noexcept { ++flushes; }
  static void complete() noexcept { ++completes; }
  static void reset() noexcept { flushes = completes = 0; }
};

// Simulated MMU: resolves a window address by really walking the tables with the format,
// so the tests prove that the recursive address formula reaches the intended table.
template <typename Format, typename Arena> struct sim_window {
  struct state {
    Arena *arena{nullptr};
    typename Format::phys_type root{};
  };
  state *st{nullptr};

  reloco::span<std::uint64_t> operator()(std::uint64_t va, std::size_t count) const noexcept {
    using levels = typename Format::levels;
    constexpr std::uint64_t mask = (std::uint64_t{1} << levels::va_bits) - 1;
    const std::uint64_t low = va & mask;
    auto phys = st->root;
    reloco::span<std::uint64_t> tbl = st->arena->table(phys, 512);
    for (std::size_t j = 0; j < levels::level_count; ++j) {
      const std::size_t shift = 12 + 9 * (levels::level_count - 1 - j);
      const std::size_t idx = static_cast<std::size_t>((low >> shift) & 511);
      const std::uint64_t raw = tbl[idx];
      if (!Format::is_present(raw)) {
        return {};
      }
      if (j + 1 == levels::level_count) {
        return st->arena->table(Format::frame_addr(raw, j), count);
      }
      if (Format::is_leaf(raw, j)) {
        return {};
      }
      tbl = st->arena->table(Format::table_addr(raw), 512);
    }
    return {};
  }
};

template <typename Format> struct rig {
  using phys = typename Format::phys_type;
  using arena_t = table_arena<phys>;
  using window_t = sim_window<Format, arena_t>;
  using remap_t = recursive_remapper<Format, arena_t, rec_tlb, window_t>;

  explicit rig(std::size_t tables = 64) : pool(512 * tables, 0) {
    arena = arena_t(reloco::span<std::uint64_t>(pool.data(), pool.size()), phys{0x1000'0000});
    state.arena = &arena;
    rec_tlb::reset();
  }

  remap_t make(std::size_t self = 510) {
    auto r = remap_t::try_create(arena, self, window_t{&state});
    EXPECT_TRUE(r.has_value());
    state.root = r->root();
    return *r;
  }

  std::vector<std::uint64_t> pool;
  arena_t arena;
  typename window_t::state state;
};

using x86_fmt = x86::recursive_pte_format<>;
using arm_fmt = arm64::recursive_stage1_format<>;
using arm_hi_wxn = arm64::recursive_stage1_format<arm64::levels_4k_48bit, arm64::stage1_ns_tag<>, true, false,
                                                  mmu_policy<true>>;

constexpr std::uint64_t k_kva = 0xFFFF'8000'0000'0000ull;

class X86Remapper : public ::testing::Test {
protected:
  rig<x86_fmt> r;
};
class ArmRemapper : public ::testing::Test {
protected:
  rig<arm_fmt> r;
};
class ArmHighRemapper : public ::testing::Test {
protected:
  rig<arm_hi_wxn> r;
};

// ---------------------------------------------------------------- x86

TEST_F(X86Remapper, CreateInstallsSelfEntry) {
  auto m = r.make();
  const std::uint64_t self = r.pool[(m.root().value - 0x1000'0000) / 8 + 510];
  EXPECT_TRUE(x86_fmt::is_present(self));
  EXPECT_EQ(x86_fmt::table_addr(self).value, m.root().value);
  EXPECT_NE(self & (1ull << 63), 0u);  // XD
  EXPECT_EQ(self & (1ull << 2), 0u);   // supervisor only
}

TEST_F(X86Remapper, WindowAddressFormula) {
  auto m = r.make(510);
  EXPECT_EQ(m.table_window_va(0, 0), 0xFFFF'FF7F'BFDF'E000ull);
  EXPECT_EQ(m.table_window_va(3, k_kva), 0xFFFF'0000'0000'0000ull | (510ull << 39) | (256ull << 30));
}

TEST_F(X86Remapper, MapPageAndQuery) {
  auto m = r.make();
  ASSERT_TRUE(m.try_map(k_kva + 0x1000, x86_fmt::phys_type{0x20'0000}, 0x1000, protection::kernel_data()));
  auto q = m.query(k_kva + 0x1234);
  ASSERT_TRUE(q.has_value());
  const auto info = *q;
  EXPECT_EQ(info.level, 3u);
  EXPECT_EQ(info.size, 0x1000u);
  EXPECT_EQ(info.physical.value, 0x20'0234u);
  EXPECT_EQ(info.virt_base, k_kva + 0x1000);
  EXPECT_TRUE(info.prot.kernel_write());
  EXPECT_FALSE(info.prot.kernel_exec());
  EXPECT_FALSE(info.prot.is_user_visible());
  EXPECT_EQ(m.query(k_kva).error(), error::not_found);
}

TEST_F(X86Remapper, LargeBlocksAreChosen) {
  auto m = r.make();
  ASSERT_TRUE(m.try_map(k_kva, x86_fmt::phys_type{0x4000'0000}, 4 << 20, protection::kernel_text()));
  auto q = m.query(k_kva + 0x20'1000);
  ASSERT_TRUE(q.has_value());
  EXPECT_EQ((*q).level, 2u);
  EXPECT_EQ((*q).size, 2u << 20);
  EXPECT_TRUE((*q).prot.kernel_exec());

  ASSERT_TRUE(m.try_map(k_kva + (1ull << 30), x86_fmt::phys_type{1ull << 30}, 1ull << 30, protection::kernel_data(),
                        map_flags::no_huge));
  {
    auto lq = m.query(k_kva + (1ull << 30));
    EXPECT_EQ((*lq).level, 2u);
  }
  ASSERT_TRUE(m.try_map(k_kva + (2ull << 30), x86_fmt::phys_type{2ull << 30}, 1ull << 30, protection::kernel_data()));
  {
    auto lq = m.query(k_kva + (2ull << 30));
    EXPECT_EQ((*lq).level, 1u);
  }
  ASSERT_TRUE(m.try_map(k_kva + (3ull << 30), x86_fmt::phys_type{3ull << 30}, 2u << 20, protection::kernel_data(),
                        map_flags::no_large));
  {
    auto lq = m.query(k_kva + (3ull << 30));
    EXPECT_EQ((*lq).level, 3u);
  }
}

TEST_F(X86Remapper, OverlapAndReplace) {
  auto m = r.make();
  const x86_fmt::phys_type pa{0x20'0000};
  ASSERT_TRUE(m.try_map(k_kva, pa, 0x2000, protection::kernel_data()));
  EXPECT_EQ(m.try_map(k_kva + 0x1000, pa, 0x1000, protection::kernel_data()).error(), error::already_exists);
  ASSERT_TRUE(m.try_map(k_kva + 0x1000, x86_fmt::phys_type{0x30'0000}, 0x1000, protection::kernel_rodata(),
                        map_flags::replace));
  auto tr = m.translate(k_kva + 0x1000);
  EXPECT_EQ((*tr).value, 0x30'0000u);
  auto qq = m.query(k_kva + 0x1000);
  EXPECT_FALSE((*qq).prot.kernel_write());
  EXPECT_GE(rec_tlb::flushes, 1);
  // A block cannot be mapped over existing 4K pages.
  EXPECT_EQ(m.try_map(k_kva, x86_fmt::phys_type{0x20'0000}, 2u << 20, protection::kernel_data(), map_flags::replace)
                .error(),
            error::already_exists);
}

TEST_F(X86Remapper, UnmapAndPartialBlock) {
  auto m = r.make();
  ASSERT_TRUE(m.try_map(k_kva, x86_fmt::phys_type{0x4000'0000}, 2u << 20, protection::kernel_data()));
  EXPECT_EQ(m.try_unmap(k_kva, 0x1000).error(), error::invalid_argument);
  EXPECT_TRUE(m.query(k_kva).has_value());
  ASSERT_TRUE(m.try_unmap(k_kva, 4u << 20)); // holes are fine
  EXPECT_EQ(m.query(k_kva).error(), error::not_found);
}

TEST_F(X86Remapper, ProtectKeepsFrame) {
  auto m = r.make();
  ASSERT_TRUE(m.try_map(k_kva, x86_fmt::phys_type{0x20'0000}, 0x3000, protection::kernel_data()));
  ASSERT_TRUE(m.try_protect(k_kva, 0x3000, protection::kernel_text()));
  auto q = m.query(k_kva + 0x2000);
  ASSERT_TRUE(q.has_value());
  EXPECT_EQ((*q).physical.value, 0x20'2000u);
  EXPECT_TRUE((*q).prot.kernel_exec());
  EXPECT_FALSE((*q).prot.kernel_write());
  EXPECT_EQ(m.try_protect(k_kva, 0x4000, protection::kernel_data()).error(), error::not_found);
}

TEST_F(X86Remapper, RejectsBadRanges) {
  auto m = r.make();
  const x86_fmt::phys_type pa{0x20'0000};
  EXPECT_EQ(m.try_map(k_kva + 1, pa, 0x1000, protection::kernel_data()).error(), error::invalid_argument);
  EXPECT_EQ(m.try_map(0x0000'8000'0000'0000ull, pa, 0x1000, protection::kernel_data()).error(),
            error::invalid_argument); // non-canonical
  EXPECT_EQ(m.try_map(0x0000'7FFF'FFFF'F000ull, pa, 0x2000, protection::kernel_data()).error(),
            error::invalid_argument); // crosses the canonical hole
  EXPECT_EQ(m.try_map(m.table_window_va(0, 0), pa, 0x1000, protection::kernel_data()).error(),
            error::invalid_argument); // the window itself
  EXPECT_EQ(m.try_map(k_kva, pa, 0x1000, protection{}).error(), error::invalid_argument);
  EXPECT_EQ(m.try_map(k_kva, pa, 0x1000, protection::kernel_data().with_cache(cache_mode::write_combining)).error(),
            error::unsupported_operation);
}

TEST_F(X86Remapper, FailedMapRollsBack) {
  rig<x86_fmt> small(5); // root + 3 tables + 1 spare: not enough for two distant pages
  auto m = small.make();
  ASSERT_TRUE(m.try_map(k_kva, x86_fmt::phys_type{0x20'0000}, 0x1000, protection::kernel_data()));
  EXPECT_EQ(m.try_map(k_kva + (1ull << 39), x86_fmt::phys_type{0x30'0000}, 0x1000, protection::kernel_data()).error(),
            error::allocation_failed);
  EXPECT_EQ(m.query(k_kva + (1ull << 39)).error(), error::not_found);
  EXPECT_TRUE(m.query(k_kva).has_value());
}

TEST_F(X86Remapper, InstallSelfRefusesOccupiedSlot) {
  auto root = r.arena.try_allocate_table(512);
  ASSERT_TRUE(root.has_value());
  auto view = r.arena.table(*root, 512);
  using remap_t = rig<x86_fmt>::remap_t;
  EXPECT_TRUE(remap_t::install_self(view, *root, 5));
  EXPECT_EQ(remap_t::install_self(view, *root, 5).error(), error::already_exists);
}

TEST_F(X86Remapper, LegalizesUserAndKernelPermissions) {
  using bits = x86::detail::pte_bits;
  const x86_fmt::phys_type pa{0x1000};
  auto leaf = [&](protection p) {
    auto v = x86_fmt::make_leaf(pa, p, 3);
    return *v;
  };
  // kernel RW + user RO -> read-only user page.
  EXPECT_FALSE(bits::rw::test(leaf(protection::kernel_rw_user_ro())));
  EXPECT_TRUE(bits::us::test(leaf(protection::kernel_rw_user_ro())));
  // user exec only follows uexec; the kernel's exec request alone does not make a user page executable.
  const protection p = (kprot::read_exec | uprot::read);
  EXPECT_TRUE(bits::xd::test(leaf(p)));
  EXPECT_FALSE(bits::xd::test(leaf(protection::user_text())));
  EXPECT_TRUE(bits::global::test(leaf(protection::kernel_text())));
  // Device memory = UC.
  const auto dev = leaf(protection::device_mmio());
  EXPECT_TRUE(bits::pcd::test(dev) && bits::pwt::test(dev));
  auto huge = x86_fmt::make_leaf(pa, protection::kernel_data(), 2);
  EXPECT_TRUE(bits::ps_or_pat::test(*huge));
}

// ---------------------------------------------------------------- arm64

TEST_F(ArmRemapper, TableDescriptorIsAlsoAValidPage) {
  using bits = structo::arch::detail::vmsa::stage1_bits;
  const std::uint64_t d = arm_fmt::make_table(arm_fmt::phys_type{0x4000'0000});
  EXPECT_TRUE(bits::valid::test(d));
  EXPECT_TRUE(bits::table_or_page::test(d));
  EXPECT_TRUE(bits::af::test(d));
  EXPECT_EQ(bits::ap::get(d), 0u);
  EXPECT_TRUE(bits::pxn::test(d) && bits::uxn::test(d));
  EXPECT_FALSE(bits::ng::test(d));
}

TEST_F(ArmRemapper, MapQueryUnmap) {
  auto m = r.make();
  ASSERT_TRUE(m.try_map(0x4000'0000, arm_fmt::phys_type{0x8000'0000}, 4 << 20, protection::kernel_text()));
  auto q = m.query(0x4020'0000 + 0x10);
  ASSERT_TRUE(q.has_value());
  EXPECT_EQ((*q).level, 2u);
  EXPECT_EQ((*q).physical.value, 0x8020'0010u);
  EXPECT_TRUE((*q).prot.kernel_exec());
  EXPECT_FALSE((*q).prot.kernel_write());
  EXPECT_TRUE((*q).prot.is_global());
  ASSERT_TRUE(m.try_unmap(0x4000'0000, 4 << 20));
  EXPECT_EQ(m.query(0x4000'0000).error(), error::not_found);
}

TEST_F(ArmRemapper, BreakBeforeMakeOnlyWhenNeeded) {
  auto m = r.make();
  ASSERT_TRUE(m.try_map(0x4000'0000, arm_fmt::phys_type{0x8000'0000}, 0x1000, protection::kernel_data()));
  rec_tlb::reset();
  // Permission-only change: a single TLB invalidation after the store.
  ASSERT_TRUE(m.try_protect(0x4000'0000, 0x1000, protection::kernel_rodata()));
  EXPECT_EQ(rec_tlb::flushes, 1);
  const int completes = rec_tlb::completes;
  // Memory type change: break (flush + barrier) before make.
  ASSERT_TRUE(m.try_protect(0x4000'0000, 0x1000, protection::kernel_rodata().with_cache(cache_mode::device)));
  EXPECT_EQ(rec_tlb::flushes, 2);
  EXPECT_GE(rec_tlb::completes, completes + 2);
  auto cq = m.query(0x4000'0000);
  EXPECT_EQ((*cq).prot.cache(), cache_mode::device);
}

TEST_F(ArmRemapper, LegalizesUserAccess) {
  using bits = structo::arch::detail::vmsa::stage1_bits;
  const arm_fmt::phys_type pa{0x1000};
  auto leaf = [&](protection p) {
    auto v = arm_fmt::make_leaf(pa, p, 3);
    return *v;
  };
  EXPECT_EQ(bits::ap::get(leaf(protection::kernel_data())), 0b00u);
  EXPECT_EQ(bits::ap::get(leaf(protection::kernel_rodata())), 0b10u);
  EXPECT_EQ(bits::ap::get(leaf(protection::user_data())), 0b01u);
  EXPECT_EQ(bits::ap::get(leaf(protection::user_rodata())), 0b11u);
  EXPECT_EQ(bits::ap::get(leaf(protection::kernel_rw_user_ro())), 0b11u); // downgraded to RO/RO
  // user write with kernel read-only is downgraded to user read-only.
  EXPECT_EQ(bits::ap::get(leaf(kprot::read | uprot::write)), 0b11u);
  EXPECT_TRUE(bits::pxn::test(leaf(protection::user_text())));
  EXPECT_FALSE(bits::uxn::test(leaf(protection::user_text())));
  EXPECT_TRUE(bits::ng::test(leaf(protection::user_data().with_scope(scope::global))));
  EXPECT_EQ(bits::sh::get(leaf(protection::device_mmio())), 0b10u);
  EXPECT_EQ(arm_fmt::make_leaf(pa, protection::kernel_data().with_security(security_state::secure), 3).error(),
            error::unsupported_operation);
}

TEST_F(ArmRemapper, FlatRegimeHasSingleXnAndNoUser) {
  using flat = arm64::recursive_stage1_format<arm64::levels_4k_48bit, arm64::stage1_ns_tag<>, false, true>;
  using bits = structo::arch::detail::vmsa::stage1_bits;
  auto dr = flat::make_leaf(flat::phys_type{0x1000}, protection::kernel_text(), 3);
  const std::uint64_t d = *dr;
  EXPECT_EQ(bits::ap::get(d), 0b11u);
  EXPECT_FALSE(bits::uxn::test(d)); // XN clear = executable
  EXPECT_FALSE(bits::pxn::test(d)); // RES0
  EXPECT_EQ(flat::make_leaf(flat::phys_type{0x1000}, protection::user_data(), 3).error(), error::invalid_argument);
}

TEST_F(ArmHighRemapper, HighHalfAndWxn) {
  auto m = r.make(511);
  EXPECT_EQ(arm_hi_wxn::canonicalize(0x1000), 0xFFFF'0000'0000'1000ull);
  const std::uint64_t va = 0xFFFF'0000'0020'0000ull;
  // RWX under WXN: exec is dropped, as the hardware would.
  ASSERT_TRUE(m.try_map(va, arm_hi_wxn::phys_type{0x8000'0000}, 0x1000,
                        protection::kernel_data().with_kernel(kprot::write_exec)));
  auto q = m.query(va);
  ASSERT_TRUE(q.has_value());
  EXPECT_TRUE((*q).prot.kernel_write());
  EXPECT_FALSE((*q).prot.kernel_exec());
  EXPECT_EQ(m.try_map(0x0000'0000'0020'0000ull, arm_hi_wxn::phys_type{0x8000'0000}, 0x1000,
                      protection::kernel_data()).error(),
            error::invalid_argument); // TTBR0 address on a TTBR1 remapper
}

} // namespace

RELOCO_END_UNSAFE_BUFFER_USAGE
