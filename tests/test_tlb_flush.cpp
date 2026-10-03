// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/arch/tlb_flush.hpp>

#include <cstdint>

// This file exercises only the architecture-agnostic dispatch/fallback
// logic in `tlb_flusher<Arch>` against fake, in-process `tlb_flush_traits`
// specializations -- no real hardware TLB instruction is ever executed,
// matching the established convention for every other `arch/*` trait
// customization point in this library (`io_space_ref`'s fake backends,
// `page_table_entry_traits`'s documented-but-unconsumed extension point).
// The `static_assert`-guarded "no operation at all for this Space"/"no
// broadcast for this Space" failure paths are compile-time-only and
// therefore cannot be exercised as a passing/failing runtime test.

namespace {

using namespace structo::arch;

// --------------------------------------------------------------------
// A fake architecture exposing a different subset of operations per
// Space, so the same trait demonstrates both the "fully-featured" and
// the "legacy, untagged hardware" ends of the fallback chain:
//
// - `process_tlb_space`: every local op, plus a partial set of
//   broadcast ops (missing `flush_page_tag_broadcast` and
//   `flush_range_tag_broadcast`, to exercise their fallback chains).
// - `untagged_tlb_space`: only `flush_all` -- modeling hardware with no
//   ASID/tag concept and no by-address flush at all (e.g. legacy ARM),
//   so every call on this Space degrades to `flush_all`.
// --------------------------------------------------------------------
struct fake_arch {};

struct call_counts {
  int flush_all = 0;
  int flush_tag = 0;
  int flush_page = 0;
  int flush_page_tag = 0;
  int flush_range = 0;
  int flush_range_tag = 0;
  int flush_all_broadcast = 0;
  int flush_tag_broadcast = 0;
  int flush_page_broadcast = 0;
  int flush_range_broadcast = 0;
  int flush_range_tag_broadcast = 0;
};

call_counts &counts() {
  static call_counts c;
  return c;
}

struct last_args {
  std::uint64_t addr = 0;
  std::uint64_t tag = 0;
  std::uint64_t addr_begin = 0;
  std::uint64_t addr_end = 0;
};

last_args &args() {
  static last_args a;
  return a;
}

} // namespace

template <> struct structo::arch::tlb_flush_traits<fake_arch> {
  static constexpr bool supports_broadcast = true;

  template <typename Space> static void flush_all() noexcept { counts().flush_all++; }

  template <typename Space>
  static auto flush_tag(std::uint64_t tag) noexcept -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    counts().flush_tag++;
    args().tag = tag;
  }

  template <typename Space>
  static auto flush_page(std::uint64_t addr) noexcept -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    counts().flush_page++;
    args().addr = addr;
  }

  template <typename Space>
  static auto flush_page_tag(std::uint64_t addr, std::uint64_t tag) noexcept
      -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    counts().flush_page_tag++;
    args().addr = addr;
    args().tag = tag;
  }

  template <typename Space>
  static auto flush_range(std::uint64_t addr_begin, std::uint64_t addr_end) noexcept
      -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    counts().flush_range++;
    args().addr_begin = addr_begin;
    args().addr_end = addr_end;
  }

  // Deliberately no flush_range_tag for any Space: exercises the
  // flush_range_tag -> flush_tag fallback.

  template <typename Space>
  static auto flush_all_broadcast() noexcept -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    counts().flush_all_broadcast++;
  }

  template <typename Space>
  static auto flush_tag_broadcast(std::uint64_t tag) noexcept
      -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    counts().flush_tag_broadcast++;
    args().tag = tag;
  }

  template <typename Space>
  static auto flush_page_broadcast(std::uint64_t addr) noexcept
      -> std::enable_if_t<std::is_same_v<Space, process_tlb_space>> {
    counts().flush_page_broadcast++;
    args().addr = addr;
  }

  // Deliberately no flush_page_tag_broadcast/flush_range_broadcast/
  // flush_range_tag_broadcast for any Space: exercises their fallback
  // chains down to flush_all_broadcast/flush_tag_broadcast.
};

// A second fake architecture with no hardware broadcast at all -- every
// `_broadcast` call for it must fail to compile (not exercised here,
// per the file-level note above), and `supports_broadcast` must read
// `false` for callers to branch on.
namespace {
struct fake_local_only_arch {};
}

template <> struct structo::arch::tlb_flush_traits<fake_local_only_arch> {
  static constexpr bool supports_broadcast = false;

  template <typename Space> static void flush_all() noexcept {}
};

// A fake architecture that implements a native range flush for every
// operation (local and broadcast), but caps it at `max_range_bytes` --
// exercising the "oversized range" short-circuit to the coarser
// tag/all-space flush documented at file scope.
namespace {
struct fake_arch_range_limited {};
}

template <> struct structo::arch::tlb_flush_traits<fake_arch_range_limited> {
  static constexpr bool supports_broadcast = true;
  static constexpr std::uint64_t max_range_bytes = 0x1000;

  template <typename Space> static void flush_all() noexcept { counts().flush_all++; }

  template <typename Space> static void flush_tag(std::uint64_t tag) noexcept {
    counts().flush_tag++;
    args().tag = tag;
  }

  template <typename Space> static void flush_range(std::uint64_t addr_begin, std::uint64_t addr_end) noexcept {
    counts().flush_range++;
    args().addr_begin = addr_begin;
    args().addr_end = addr_end;
  }

  template <typename Space>
  static void flush_range_tag(std::uint64_t addr_begin, std::uint64_t addr_end, std::uint64_t tag) noexcept {
    counts().flush_range_tag++;
    args().addr_begin = addr_begin;
    args().addr_end = addr_end;
    args().tag = tag;
  }

  template <typename Space> static void flush_all_broadcast() noexcept { counts().flush_all_broadcast++; }

  template <typename Space> static void flush_tag_broadcast(std::uint64_t tag) noexcept {
    counts().flush_tag_broadcast++;
    args().tag = tag;
  }

  template <typename Space>
  static void flush_range_broadcast(std::uint64_t addr_begin, std::uint64_t addr_end) noexcept {
    counts().flush_range_broadcast++;
    args().addr_begin = addr_begin;
    args().addr_end = addr_end;
  }

  template <typename Space>
  static void flush_range_tag_broadcast(std::uint64_t addr_begin, std::uint64_t addr_end,
                                         std::uint64_t tag) noexcept {
    counts().flush_range_tag_broadcast++;
    args().addr_begin = addr_begin;
    args().addr_end = addr_end;
    args().tag = tag;
  }
};

namespace {

class TlbFlushTest : public ::testing::Test {
protected:
  void SetUp() override {
    counts() = call_counts{};
    args() = last_args{};
  }
};

using flusher = tlb_flusher<fake_arch>;

// --- Local, direct operations ------------------------------------------

TEST_F(TlbFlushTest, FlushAllCallsTraitDirectly) {
  flusher::flush_all<process_tlb_space>();
  EXPECT_EQ(counts().flush_all, 1);
}

TEST_F(TlbFlushTest, FlushTagCallsTraitDirectlyWhenSupported) {
  flusher::flush_tag<process_tlb_space>(0x42);
  EXPECT_EQ(counts().flush_tag, 1);
  EXPECT_EQ(counts().flush_all, 0);
  EXPECT_EQ(args().tag, 0x42u);
}

TEST_F(TlbFlushTest, FlushPageCallsTraitDirectlyWhenSupported) {
  flusher::flush_page<process_tlb_space>(0x1000);
  EXPECT_EQ(counts().flush_page, 1);
  EXPECT_EQ(counts().flush_all, 0);
  EXPECT_EQ(args().addr, 0x1000u);
}

TEST_F(TlbFlushTest, FlushPageTagCallsTraitDirectlyWhenSupported) {
  flusher::flush_page_tag<process_tlb_space>(0x2000, 7);
  EXPECT_EQ(counts().flush_page_tag, 1);
  EXPECT_EQ(counts().flush_tag, 0);
  EXPECT_EQ(counts().flush_page, 0);
  EXPECT_EQ(counts().flush_all, 0);
}

TEST_F(TlbFlushTest, FlushRangeCallsTraitDirectlyWhenSupported) {
  flusher::flush_range<process_tlb_space>(0x3000, 0x5000);
  EXPECT_EQ(counts().flush_range, 1);
  EXPECT_EQ(counts().flush_all, 0);
  EXPECT_EQ(args().addr_begin, 0x3000u);
  EXPECT_EQ(args().addr_end, 0x5000u);
}

// --- Precision-fallback chain --------------------------------------------

TEST_F(TlbFlushTest, FlushTagFallsBackToFlushAllForUntaggedSpace) {
  // untagged_tlb_space only has flush_all in this fake trait -- modeling
  // legacy, ASID-less hardware.
  flusher::flush_tag<untagged_tlb_space>(0x99);
  EXPECT_EQ(counts().flush_tag, 0);
  EXPECT_EQ(counts().flush_all, 1);
}

TEST_F(TlbFlushTest, FlushPageFallsBackToFlushAllForUntaggedSpace) {
  flusher::flush_page<untagged_tlb_space>(0x1000);
  EXPECT_EQ(counts().flush_page, 0);
  EXPECT_EQ(counts().flush_all, 1);
}

TEST_F(TlbFlushTest, FlushPageTagFallsBackToFlushAllForUntaggedSpace) {
  flusher::flush_page_tag<untagged_tlb_space>(0x1000, 1);
  EXPECT_EQ(counts().flush_page_tag, 0);
  EXPECT_EQ(counts().flush_tag, 0);
  EXPECT_EQ(counts().flush_page, 0);
  EXPECT_EQ(counts().flush_all, 1);
}

TEST_F(TlbFlushTest, FlushRangeFallsBackToFlushAllForUntaggedSpace) {
  flusher::flush_range<untagged_tlb_space>(0, 0x1000);
  EXPECT_EQ(counts().flush_range, 0);
  EXPECT_EQ(counts().flush_all, 1);
}

TEST_F(TlbFlushTest, FlushRangeTagFallsBackToFlushTagBeforeFlushAll) {
  // process_tlb_space has flush_tag but no flush_range_tag in this fake
  // trait, so range_tag should prefer the tag-precision fallback over
  // the fully-coarse flush_all.
  flusher::flush_range_tag<process_tlb_space>(0, 0x1000, 55);
  EXPECT_EQ(counts().flush_range_tag, 0);
  EXPECT_EQ(counts().flush_tag, 1);
  EXPECT_EQ(counts().flush_all, 0);
  EXPECT_EQ(args().tag, 55u);
}

TEST_F(TlbFlushTest, FlushRangeTagFallsBackToFlushAllForUntaggedSpace) {
  // untagged_tlb_space has neither flush_range_tag nor flush_tag, so it
  // must bottom out at flush_all.
  flusher::flush_range_tag<untagged_tlb_space>(0, 0x1000, 55);
  EXPECT_EQ(counts().flush_tag, 0);
  EXPECT_EQ(counts().flush_all, 1);
}

// --- Oversized ranges: short-circuit to the coarser op, bypassing the
// native range instruction even when one exists -------------------------

using limited_flusher = tlb_flusher<fake_arch_range_limited>;

TEST_F(TlbFlushTest, FlushRangeUsesNativeRangeOpWhenWithinLimit) {
  limited_flusher::flush_range<process_tlb_space>(0, 0x1000);
  EXPECT_EQ(counts().flush_range, 1);
  EXPECT_EQ(counts().flush_all, 0);
}

TEST_F(TlbFlushTest, FlushRangeFallsBackToFlushAllWhenOversized) {
  limited_flusher::flush_range<process_tlb_space>(0, 0x1001);
  EXPECT_EQ(counts().flush_range, 0);
  EXPECT_EQ(counts().flush_all, 1);
}

TEST_F(TlbFlushTest, FlushRangeTagUsesNativeRangeOpWhenWithinLimit) {
  limited_flusher::flush_range_tag<process_tlb_space>(0, 0x1000, 7);
  EXPECT_EQ(counts().flush_range_tag, 1);
  EXPECT_EQ(counts().flush_tag, 0);
  EXPECT_EQ(counts().flush_all, 0);
}

TEST_F(TlbFlushTest, FlushRangeTagFallsBackToFlushTagWhenOversized) {
  // Oversized falls back to the whole tag, not the whole Space -- the
  // tag precision is kept even though the range precision is dropped.
  limited_flusher::flush_range_tag<process_tlb_space>(0, 0x1001, 7);
  EXPECT_EQ(counts().flush_range_tag, 0);
  EXPECT_EQ(counts().flush_tag, 1);
  EXPECT_EQ(counts().flush_all, 0);
  EXPECT_EQ(args().tag, 7u);
}

TEST_F(TlbFlushTest, FlushRangeBroadcastFallsBackToFlushAllBroadcastWhenOversized) {
  limited_flusher::flush_range_broadcast<process_tlb_space>(0, 0x1001);
  EXPECT_EQ(counts().flush_range_broadcast, 0);
  EXPECT_EQ(counts().flush_all_broadcast, 1);
}

TEST_F(TlbFlushTest, FlushRangeTagBroadcastFallsBackToFlushTagBroadcastWhenOversized) {
  limited_flusher::flush_range_tag_broadcast<process_tlb_space>(0, 0x1001, 9);
  EXPECT_EQ(counts().flush_range_tag_broadcast, 0);
  EXPECT_EQ(counts().flush_tag_broadcast, 1);
  EXPECT_EQ(counts().flush_all_broadcast, 0);
  EXPECT_EQ(args().tag, 9u);
}

TEST_F(TlbFlushTest, UnboundedTraitWithoutMaxRangeBytesNeverTreatsRangeAsOversized) {
  // fake_arch (no max_range_bytes at all) must still use its native
  // flush_range for an arbitrarily large range -- the oversized check is
  // opt-in per architecture.
  flusher::flush_range<process_tlb_space>(0, ~std::uint64_t{0});
  EXPECT_EQ(counts().flush_range, 1);
  EXPECT_EQ(counts().flush_all, 0);
}

// --- Broadcast: direct, fallback among broadcast ops, never to local ----

TEST_F(TlbFlushTest, SupportsBroadcastReflectsTrait) {
  EXPECT_TRUE(tlb_flusher<fake_arch>::supports_broadcast);
  EXPECT_FALSE(tlb_flusher<fake_local_only_arch>::supports_broadcast);
}

TEST_F(TlbFlushTest, FlushAllBroadcastCallsTraitDirectly) {
  flusher::flush_all_broadcast<process_tlb_space>();
  EXPECT_EQ(counts().flush_all_broadcast, 1);
  EXPECT_EQ(counts().flush_all, 0);
}

TEST_F(TlbFlushTest, FlushTagBroadcastCallsTraitDirectlyWhenSupported) {
  flusher::flush_tag_broadcast<process_tlb_space>(0x11);
  EXPECT_EQ(counts().flush_tag_broadcast, 1);
  EXPECT_EQ(counts().flush_all_broadcast, 0);
}

TEST_F(TlbFlushTest, FlushPageTagBroadcastFallsBackToTagBroadcastNotLocal) {
  // process_tlb_space has flush_tag_broadcast but no
  // flush_page_tag_broadcast, so it must fall back to the broadcast tag
  // flush, never to a local-only operation.
  flusher::flush_page_tag_broadcast<process_tlb_space>(0x1000, 0x22);
  EXPECT_EQ(counts().flush_tag_broadcast, 1);
  EXPECT_EQ(counts().flush_page_tag, 0);
  EXPECT_EQ(counts().flush_all_broadcast, 0);
  EXPECT_EQ(args().tag, 0x22u);
}

TEST_F(TlbFlushTest, FlushRangeBroadcastFallsBackToAllBroadcastNotLocal) {
  // process_tlb_space has no flush_range_broadcast at all, so it must
  // fall back all the way to flush_all_broadcast, never to the local
  // flush_range or flush_all.
  flusher::flush_range_broadcast<process_tlb_space>(0, 0x1000);
  EXPECT_EQ(counts().flush_all_broadcast, 1);
  EXPECT_EQ(counts().flush_range, 0);
  EXPECT_EQ(counts().flush_all, 0);
}

TEST_F(TlbFlushTest, FlushRangeTagBroadcastFallsBackToTagBroadcastNotLocal) {
  flusher::flush_range_tag_broadcast<process_tlb_space>(0, 0x1000, 0x33);
  EXPECT_EQ(counts().flush_tag_broadcast, 1);
  EXPECT_EQ(counts().flush_range_tag, 0);
  EXPECT_EQ(counts().flush_all_broadcast, 0);
  EXPECT_EQ(counts().flush_all, 0);
}

} // namespace
