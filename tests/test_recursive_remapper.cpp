// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>

#include <structo/arch/arm/recursive_format.hpp>
#include <structo/arch/arm64/recursive_format.hpp>
#include <structo/arch/mair.hpp>
#include <structo/arch/recursive_remapper.hpp>
#include <structo/arch/riscv/recursive_format.hpp>
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

// The "MMU": resolves the window address to the table memory, proving the remapper only touches the
// table through its own self-mapped VA.
template <typename Format> struct sim_window {
  using word = typename Format::word;
  std::vector<word> *mem{nullptr};
  std::uint64_t expected_va{0};
  int *bad_accesses{nullptr};

  reloco::span<word> operator()(std::uint64_t va, std::size_t count) const noexcept {
    if (va != expected_va || count != Format::entry_count) {
      ++*bad_accesses;
      return {};
    }
    return reloco::span<word>(mem->data(), mem->size());
  }
};

template <typename Format, std::uint64_t Base = 0x4000'0000ull> struct rig {
  using phys = typename Format::phys_type;
  using window_t = sim_window<Format>;
  using remap_t = recursive_remapper<Format, rec_tlb, window_t>;
  static constexpr std::size_t self = 3;
  static constexpr std::uint64_t page = Format::page_size;
  static constexpr std::uint64_t table_phys = 0x1000'0000ull;

  rig() : mem(Format::entry_count, static_cast<typename Format::word>(0xFFFF'FFFFu)) {
    rec_tlb::reset();
    window_t w{&mem, Base + self * page, &bad};
    auto r = remap_t::try_initialize(reloco::span<typename Format::word>(mem.data(), mem.size()), phys{table_phys},
                                     Base, self, w);
    EXPECT_TRUE(r.has_value());
    m = std::make_unique<remap_t>(*r);
  }
  std::uint64_t va(std::size_t slot) const { return Base + slot * page; }

  std::vector<typename Format::word> mem;
  int bad{0};
  std::unique_ptr<remap_t> m;
};

// Behaviour every format must share.
template <typename Format> void exercise_common() {
  rig<Format> r;
  using phys = typename Format::phys_type;
  const std::uint64_t pg = Format::page_size;
  auto &m = *r.m;

  // Initialization cleared the table and installed only the self entry, which maps the table itself.
  for (std::size_t i = 0; i < Format::entry_count; ++i) {
    EXPECT_EQ(Format::is_present(r.mem[i]), i == r.self);
  }
  EXPECT_EQ(Format::frame_addr(r.mem[r.self]).value, r.table_phys);
  EXPECT_EQ(m.window_va(), r.va(r.self));

  // Map 4 pages, query inside the second one.
  ASSERT_TRUE(m.try_map(r.va(8), phys{0x2000'0000}, 4 * pg, protection::kernel_data()));
  auto q = m.query(r.va(9) + 0x10);
  ASSERT_TRUE(q.has_value());
  const auto info = *q;
  EXPECT_EQ(info.size, pg);
  EXPECT_EQ(info.virt_base, r.va(9));
  EXPECT_EQ(info.physical.value, 0x2000'0000u + pg + 0x10);
  EXPECT_TRUE(info.prot.kernel_write());
  EXPECT_FALSE(info.prot.is_user_visible());
  EXPECT_EQ(m.query(r.va(7)).error(), error::not_found);

  // Overlap is rejected unless `replace`.
  EXPECT_EQ(m.try_map(r.va(10), phys{0x3000'0000}, pg, protection::kernel_data()).error(), error::already_exists);
  ASSERT_TRUE(m.try_map(r.va(10), phys{0x3000'0000}, pg, protection::kernel_rodata(), map_flags::replace));
  auto t = m.translate(r.va(10));
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ((*t).value, 0x3000'0000u);

  // Protect keeps frames.
  ASSERT_TRUE(m.try_protect(r.va(8), 4 * pg, protection::kernel_text()));
  auto p = m.query(r.va(11));
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ((*p).physical.value, 0x2000'0000u + 3 * pg);
  EXPECT_TRUE((*p).prot.kernel_exec());
  EXPECT_FALSE((*p).prot.kernel_write());
  EXPECT_EQ(m.try_protect(r.va(7), 2 * pg, protection::kernel_data()).error(), error::not_found);

  // Unmap ignores holes.
  ASSERT_TRUE(m.try_unmap(r.va(6), 10 * pg));
  EXPECT_EQ(m.query(r.va(8)).error(), error::not_found);

  // Range validation: reserved window slot, outside the span, unaligned, empty.
  const phys pa{0x2000'0000};
  EXPECT_EQ(m.try_map(m.window_va(), pa, pg, protection::kernel_data()).error(), error::invalid_argument);
  EXPECT_EQ(m.try_map(r.va(1) - pg * 0, pa, 3 * pg, protection::kernel_data()).error(), error::invalid_argument);
  EXPECT_EQ(m.try_map(r.va(Format::entry_count - 1), pa, 2 * pg, protection::kernel_data()).error(),
            error::invalid_argument);
  EXPECT_EQ(m.try_map(r.va(0) - pg, pa, pg, protection::kernel_data()).error(), error::invalid_argument);
  EXPECT_EQ(m.try_map(r.va(8) + 1, pa, pg, protection::kernel_data()).error(), error::invalid_argument);
  EXPECT_EQ(m.try_map(r.va(8), pa, 0, protection::kernel_data()).error(), error::invalid_argument);
  EXPECT_EQ(m.try_map(r.va(8), pa, pg, protection{}).error(), error::invalid_argument);
  EXPECT_EQ(r.bad, 0);
}

class RecursiveRemapper : public ::testing::Test {};

using vmsa4k = arm64::recursive_stage1_format<>;
using vmsa16k = arm64::recursive_stage1_format<structo::page_16k>;
using vmsa64k = arm64::recursive_stage1_format<structo::page_64k>;
using lpae_fmt = arm::lpae::recursive_stage1_format<>;
using short_fmt = arm::short_descriptor::recursive_small_page_format<>;
using rv_fmt = riscv::recursive_pte_format<true>;

TEST_F(RecursiveRemapper, CommonX86Pae) { exercise_common<x86::recursive_pte_format>(); }
TEST_F(RecursiveRemapper, CommonX86_32) { exercise_common<x86::recursive_pte32_format>(); }
TEST_F(RecursiveRemapper, CommonArm64_4K) { exercise_common<vmsa4k>(); }
TEST_F(RecursiveRemapper, CommonArm64_16K) { exercise_common<vmsa16k>(); }
TEST_F(RecursiveRemapper, CommonArm64_64K) { exercise_common<vmsa64k>(); }
TEST_F(RecursiveRemapper, CommonArmLpae) { exercise_common<lpae_fmt>(); }
TEST_F(RecursiveRemapper, CommonArmShort) { exercise_common<short_fmt>(); }
TEST_F(RecursiveRemapper, CommonRiscv) { exercise_common<rv_fmt>(); }

TEST_F(RecursiveRemapper, TableSizes) {
  EXPECT_EQ(vmsa4k::entry_count, 512u);
  EXPECT_EQ(vmsa16k::entry_count, 2048u);
  EXPECT_EQ(vmsa64k::entry_count, 8192u);
  EXPECT_EQ(vmsa64k::page_size, 65536u);
  EXPECT_EQ(short_fmt::entry_count, 256u);
  using r = recursive_remapper<vmsa16k>;
  EXPECT_EQ(r::span_bytes, 32ull << 20);
}

TEST_F(RecursiveRemapper, InitializeValidatesArguments) {
  using fmt = x86::recursive_pte_format;
  using R = recursive_remapper<fmt, no_tlb>;
  std::vector<std::uint64_t> mem(512);
  reloco::span<std::uint64_t> view(mem.data(), mem.size());
  const fmt::phys_type pa{0x1000};
  EXPECT_EQ(R::try_initialize(view, fmt::phys_type{0x1001}, 0x20'0000, 0).error(), error::invalid_argument);
  EXPECT_EQ(R::try_initialize(view, pa, 0x1000, 0).error(), error::invalid_argument);  // base not span aligned
  EXPECT_EQ(R::try_initialize(view, pa, 0x20'0000, 512).error(), error::invalid_argument);
  EXPECT_EQ(R::try_initialize(view.first(10), pa, 0x20'0000, 0).error(), error::invalid_argument);
}

// ---------------------------------------------------------------- x86

TEST_F(RecursiveRemapper, X86Legalization) {
  using bits = x86::detail::pte_bits;
  using fmt = x86::recursive_pte_format;
  const fmt::phys_type pa{0x1000};
  auto leaf = [&](protection p) {
    auto v = fmt::make_leaf(pa, p);
    return *v;
  };
  // kernel RW + user RO -> read-only user page.
  EXPECT_FALSE(bits::rw::test(leaf(protection::kernel_rw_user_ro())));
  EXPECT_TRUE(bits::us::test(leaf(protection::kernel_rw_user_ro())));
  // On a user page only the user's execute bit counts.
  EXPECT_TRUE(bits::xd::test(leaf(kprot::read_exec | uprot::read)));
  EXPECT_FALSE(bits::xd::test(leaf(protection::user_text())));
  EXPECT_TRUE(bits::global::test(leaf(protection::kernel_text())));
  const auto dev = leaf(protection::device_mmio());
  EXPECT_TRUE(bits::pcd::test(dev) && bits::pwt::test(dev));
  EXPECT_EQ(fmt::make_leaf(pa, protection::framebuffer()).error(), error::unsupported_operation);
  EXPECT_EQ(fmt::make_leaf(pa, protection::kernel_data().with_security(security_state::secure)).error(),
            error::unsupported_operation);
  const protection round = fmt::attrs(leaf(protection::user_data()));
  EXPECT_TRUE(round.user_write());
  EXPECT_FALSE(round.user_exec());
}

TEST_F(RecursiveRemapper, X86_32HasNoNxAndLimitsAddresses) {
  using fmt = x86::recursive_pte32_format;
  auto v = fmt::make_leaf(fmt::phys_type{0x2000}, protection::kernel_data());
  ASSERT_TRUE(v.has_value());
  EXPECT_TRUE(fmt::attrs(*v).kernel_exec()); // documented exception: no XD on non-PAE
  EXPECT_EQ(fmt::make_leaf(fmt::phys_type{1ull << 32}, protection::kernel_data()).error(), error::out_of_range);
  EXPECT_EQ(fmt::frame_addr(*v).value, 0x2000u);
}

// ---------------------------------------------------------------- AArch64 / LPAE

TEST_F(RecursiveRemapper, Arm64Legalization) {
  using bits = structo::arch::detail::vmsa::stage1_bits;
  const vmsa4k::phys_type pa{0x1000};
  auto leaf = [&](protection p) {
    auto v = vmsa4k::make_leaf(pa, p);
    return *v;
  };
  EXPECT_EQ(bits::ap::get(leaf(protection::kernel_data())), 0b00u);
  EXPECT_EQ(bits::ap::get(leaf(protection::kernel_rodata())), 0b10u);
  EXPECT_EQ(bits::ap::get(leaf(protection::user_data())), 0b01u);
  EXPECT_EQ(bits::ap::get(leaf(protection::user_rodata())), 0b11u);
  EXPECT_EQ(bits::ap::get(leaf(protection::kernel_rw_user_ro())), 0b11u);
  EXPECT_EQ(bits::ap::get(leaf(kprot::read | uprot::write)), 0b11u);
  EXPECT_TRUE(bits::pxn::test(leaf(protection::user_text())));
  EXPECT_FALSE(bits::uxn::test(leaf(protection::user_text())));
  EXPECT_TRUE(bits::ng::test(leaf(protection::user_data().with_scope(scope::global))));
  EXPECT_EQ(bits::sh::get(leaf(protection::device_mmio())), 0b10u);
  EXPECT_TRUE(bits::af::test(leaf(protection::kernel_data())));
  // Writable pages are born dirty (AP[2]=0) and DBM (bit 51) stays clear: no hardware A/D update is needed.
  EXPECT_EQ(bits::ap::get(leaf(protection::kernel_data())) & 0b10, 0u);
  EXPECT_EQ(leaf(protection::kernel_data()) >> 51 & 1, 0u);
  EXPECT_EQ(vmsa4k::make_leaf(pa, protection::kernel_data().with_security(security_state::secure)).error(),
            error::unsupported_operation);
}

TEST_F(RecursiveRemapper, Arm64GranuleOutputAddress) {
  const vmsa64k::phys_type pa{0x1234'0000};
  auto v = vmsa64k::make_leaf(pa, protection::kernel_data());
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(vmsa64k::frame_addr(*v).value, 0x1234'0000u);
}

TEST_F(RecursiveRemapper, Arm64FlatRegime) {
  using flat = arm64::recursive_stage1_format<structo::page_4k, arm64::stage1_ns_tag<>, vmsa_regime::flat>;
  using bits = structo::arch::detail::vmsa::stage1_bits;
  auto dr = flat::make_leaf(flat::phys_type{0x1000}, protection::kernel_text());
  ASSERT_TRUE(dr.has_value());
  EXPECT_EQ(bits::ap::get(*dr), 0b11u);
  EXPECT_FALSE(bits::uxn::test(*dr)); // XN clear = executable
  EXPECT_EQ(flat::make_leaf(flat::phys_type{0x1000}, protection::user_data()).error(), error::invalid_argument);
}

TEST_F(RecursiveRemapper, Arm64WxnDropsExecute) {
  using wxn = arm64::recursive_stage1_format<structo::page_4k, arm64::stage1_ns_tag<>, vmsa_regime::el1_el0,
                                             mmu_policy<true>>;
  auto v = wxn::make_leaf(wxn::phys_type{0x1000}, protection::kernel_data().with_kernel(kprot::write_exec));
  ASSERT_TRUE(v.has_value());
  const protection p = wxn::attrs(*v);
  EXPECT_TRUE(p.kernel_write());
  EXPECT_FALSE(p.kernel_exec());
}

TEST_F(RecursiveRemapper, Arm64SecureTagUsesNsBit) {
  using sec = arm64::recursive_stage1_format<structo::page_4k, arm64::stage1_secure_tag<>>;
  using bits = structo::arch::detail::vmsa::stage1_bits;
  auto ns = sec::make_leaf(sec::phys_type{0x1000}, protection::kernel_data().with_security(security_state::non_secure));
  auto s = sec::make_leaf(sec::phys_type{0x1000}, protection::kernel_data().with_security(security_state::secure));
  ASSERT_TRUE(ns.has_value() && s.has_value());
  EXPECT_TRUE(bits::ns::test(*ns));
  EXPECT_FALSE(bits::ns::test(*s));
}

TEST_F(RecursiveRemapper, Arm64BreakBeforeMakeOnlyWhenNeeded) {
  rig<vmsa4k> r;
  auto &m = *r.m;
  ASSERT_TRUE(m.try_map(r.va(8), vmsa4k::phys_type{0x8000'0000}, 4096, protection::kernel_data()));
  rec_tlb::reset();
  // Permission-only change: one invalidation, no barrier between store and flush.
  ASSERT_TRUE(m.try_protect(r.va(8), 4096, protection::kernel_rodata()));
  EXPECT_EQ(rec_tlb::flushes, 1);
  const int completes = rec_tlb::completes;
  // Memory-type change: break (flush + barrier) before make.
  ASSERT_TRUE(m.try_protect(r.va(8), 4096, protection::kernel_rodata().with_cache(cache_mode::device)));
  EXPECT_EQ(rec_tlb::flushes, 2);
  EXPECT_GE(rec_tlb::completes, completes + 2);
  auto q = m.query(r.va(8));
  EXPECT_EQ((*q).prot.cache(), cache_mode::device);
}

TEST_F(RecursiveRemapper, LpaeSingleXnBlocksBothPrivileges) {
  using bits = structo::arch::detail::vmsa::stage1_bits;
  const lpae_fmt::phys_type pa{0x1000};
  // Kernel-only execute on a user page cannot be expressed with one XN: dropped.
  auto v = lpae_fmt::make_leaf(pa, kprot::read_exec | uprot::read);
  ASSERT_TRUE(v.has_value());
  EXPECT_TRUE(bits::uxn::test(*v));
  EXPECT_FALSE(lpae_fmt::attrs(*v).kernel_exec());
  auto k = lpae_fmt::make_leaf(pa, protection::kernel_text());
  ASSERT_TRUE(k.has_value());
  EXPECT_TRUE(lpae_fmt::attrs(*k).kernel_exec());
  auto u = lpae_fmt::make_leaf(pa, protection::user_text());
  ASSERT_TRUE(u.has_value());
  EXPECT_TRUE(lpae_fmt::attrs(*u).user_exec());
}

// ---------------------------------------------------------------- ARM short descriptor

TEST_F(RecursiveRemapper, ShortDescriptorEncodings) {
  using bits = arm::short_descriptor::detail::small_page_bits;
  const short_fmt::phys_type pa{0x4000};
  auto leaf = [&](protection p) {
    auto v = short_fmt::make_leaf(pa, p);
    return *v;
  };
  const auto kdata = leaf(protection::kernel_data());
  EXPECT_EQ(bits::ap01::get(kdata), 0b01u);
  EXPECT_FALSE(bits::apx::test(kdata));
  EXPECT_TRUE(bits::xn::test(kdata));
  EXPECT_TRUE(bits::apx::test(leaf(protection::kernel_rodata())));
  const auto ud = leaf(protection::user_data());
  EXPECT_EQ(bits::ap01::get(ud), 0b11u);
  const auto kru = leaf(protection::kernel_rw_user_ro()); // expressible here: APX=0, AP=10
  EXPECT_EQ(bits::ap01::get(kru), 0b10u);
  EXPECT_FALSE(bits::apx::test(kru));
  EXPECT_FALSE(bits::xn::test(leaf(protection::user_text())));
  const auto dev = leaf(protection::device_mmio());
  EXPECT_TRUE(bits::b::test(dev) && !bits::c::test(dev) && bits::tex::get(dev) == 0);
  EXPECT_EQ(short_fmt::frame_addr(ud).value, 0x4000u);
  EXPECT_EQ(short_fmt::attrs(kru).user(), uprot::read);
  EXPECT_TRUE(short_fmt::attrs(kru).kernel_write());
  EXPECT_EQ(short_fmt::attrs(leaf(protection::kernel_data().with_cache(cache_mode::write_through))).cache(),
            cache_mode::write_through);
  EXPECT_EQ(short_fmt::make_leaf(short_fmt::phys_type{1ull << 32}, protection::kernel_data()).error(),
            error::out_of_range);
}

// ---------------------------------------------------------------- RISC-V

TEST_F(RecursiveRemapper, RiscvEncodings) {
  using bits = riscv::detail::pte_bits;
  const rv_fmt::phys_type pa{0x8000'0000};
  auto leaf = [&](protection p) {
    auto v = rv_fmt::make_leaf(pa, p);
    return *v;
  };
  const auto text = leaf(protection::kernel_text());
  EXPECT_TRUE(bits::read::test(text) && bits::exec::test(text) && !bits::write::test(text));
  EXPECT_FALSE(bits::user::test(text));
  EXPECT_TRUE(bits::accessed::test(text) && bits::dirty::test(text) && bits::global::test(text));
  const auto ud = leaf(protection::user_data());
  EXPECT_TRUE(bits::user::test(ud) && bits::write::test(ud) && !bits::exec::test(ud));
  const auto ro = leaf(protection::kernel_rw_user_ro());
  EXPECT_TRUE(bits::user::test(ro) && !bits::write::test(ro)); // downgraded
  EXPECT_EQ(rv_fmt::frame_addr(text).value, 0x8000'0000u);
  const auto dev = leaf(protection::device_mmio());
  EXPECT_EQ((dev >> 61) & 3, 2u);
  EXPECT_EQ(rv_fmt::attrs(dev).cache(), cache_mode::device);
  using plain = riscv::recursive_pte_format<false>;
  EXPECT_EQ(plain::make_leaf(plain::phys_type{0x1000}, protection::device_mmio()).error(),
            error::unsupported_operation);
  EXPECT_EQ(plain::make_leaf(plain::phys_type{0x1000}, protection::kernel_data().with_cache(cache_mode::write_through))
                .error(),
            error::unsupported_operation);
}

TEST_F(RecursiveRemapper, RiscvFlushesAfterValidating) {
  rig<rv_fmt> r;
  rec_tlb::reset();
  ASSERT_TRUE(r.m->try_map(r.va(8), rv_fmt::phys_type{0x8000'0000}, 4096, protection::kernel_data()));
  EXPECT_EQ(rec_tlb::flushes, 1);
}

TEST_F(RecursiveRemapper, MairBuilders) {
  EXPECT_EQ(mair_attr::device_nGnRnE().raw, 0x00);
  EXPECT_EQ(mair_attr::device_nGnRE().raw, 0x04);
  EXPECT_EQ(mair_attr::device_nGRE().raw, 0x08);
  EXPECT_EQ(mair_attr::device_GRE().raw, 0x0C);
  EXPECT_EQ(mair_attr::normal(mair_cache::non_cacheable).raw, 0x44);
  EXPECT_EQ(mair_attr::normal(mair_cache::write_through, mair_alloc::read_write).raw, 0xBB);
  EXPECT_EQ(mair_attr::normal(mair_cache::write_back, mair_alloc::read_write).raw, 0xFF);
  EXPECT_EQ(mair_attr::normal(mair_cache::write_back, mair_alloc::read, mair_cache::non_cacheable, mair_alloc::none).raw,
            0xE4);
  EXPECT_EQ(mair_attr::normal_tagged().raw, 0xF0);
  EXPECT_TRUE(mair_attr::device_GRE().is_device());
  EXPECT_FALSE(mair_attr::normal_tagged().is_device());

  // The default layout is bit-identical to what earlier versions programmed into MAIR.
  EXPECT_EQ(default_mair::value, 0xFF'BB44'0400ull);
  EXPECT_EQ(default_mair_value.lo(), 0xBB44'0400u);
  EXPECT_EQ(default_mair_value.hi(), 0xFFu);
  EXPECT_EQ(default_mair_value.find(mair_attr::normal(mair_cache::write_back, mair_alloc::read_write)), 4);
  EXPECT_EQ(default_mair_value.find(mair_attr::device_GRE()), -1);
  EXPECT_EQ(default_mair_value.get(9).raw, 0);

  // Custom layout: write-back in slot 0, device in slot 7, round trip through index_of/cache_of.
  constexpr mair_value v = mair_value{}
                               .set<0>(mair_attr::normal(mair_cache::write_back, mair_alloc::read_write))
                               .set<7>(mair_attr::device_nGnRE());
  using layout = mair_layout<v.raw, 0, 0, 0, 0, 7, 7>;
  EXPECT_EQ(layout::index_of(cache_mode::device), 7u);
  EXPECT_EQ(layout::cache_of(7), cache_mode::device_ordered);
  EXPECT_EQ(layout::cache_of(0), cache_mode::uncached);
  EXPECT_EQ(v.raw >> 56, 0x04u);
}

} // namespace

RELOCO_END_UNSAFE_BUFFER_USAGE
