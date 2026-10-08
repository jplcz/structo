// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/target_ptr.hpp>

#include <array>
#include <cstring>

#include <reloco/lifetime.hpp>

// Test fixtures index raw buffers freely; bounds are checked by the assertions.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

namespace {

using namespace structo;

// --------------------------------------------------------------------
// A small, in-process "current address space" simulation: try_read/
// try_write operate against a plain byte array instead of a real page
// table, and anything at or past `kFaultThreshold` additionally pretends
// to require resolving a page fault -- fine for the plain accessors, but
// rejected with `error::page_fault` by the `_nofault` ones.
// --------------------------------------------------------------------

struct test_user_space {};

constexpr std::uintptr_t kUserBase = 0x2000;
constexpr std::size_t kUserStoreSize = 256;
constexpr std::uintptr_t kFaultThreshold = kUserBase + 128;

std::array<std::byte, kUserStoreSize> &user_store() {
  static std::array<std::byte, kUserStoreSize> store{};
  return store;
}

} // namespace

template <> struct structo::target_ptr_space_traits<test_user_space> {
  using address_type = std::uintptr_t;

  static constexpr address_type min_value = kUserBase;
  static constexpr address_type max_value = kUserBase + kUserStoreSize - 1;

  static reloco::result<void> try_read(address_type addr, reloco::span<std::byte> dst) noexcept {
    std::memcpy(dst.data(), user_store().data() + (addr - kUserBase), dst.size());
    return {};
  }

  static reloco::result<void> try_write(address_type addr, reloco::span<const std::byte> src) noexcept {
    std::memcpy(user_store().data() + (addr - kUserBase), src.data(), src.size());
    return {};
  }

  static reloco::result<void> try_read_nofault(address_type addr, reloco::span<std::byte> dst) noexcept {
    if (addr + dst.size() > kFaultThreshold)
      return reloco::unexpected(reloco::error::page_fault);
    return try_read(addr, dst);
  }

  static reloco::result<void> try_write_nofault(address_type addr, reloco::span<const std::byte> src) noexcept {
    if (addr + src.size() > kFaultThreshold)
      return reloco::unexpected(reloco::error::page_fault);
    return try_write(addr, src);
  }
};

namespace {

// A second space tag whose traits deliberately omit min_value/max_value,
// exercising the "no declared range" path of is_in_range()/materialize_
// bytes()/store_bytes() -- every address is considered in-range.
struct test_kernel_space {};

constexpr std::uintptr_t kKernelBase = 0x5000;
constexpr std::size_t kKernelStoreSize = 64;

std::array<std::byte, kKernelStoreSize> &kernel_store() {
  static std::array<std::byte, kKernelStoreSize> store{};
  return store;
}

} // namespace

template <> struct structo::target_ptr_space_traits<test_kernel_space> {
  using address_type = std::uintptr_t;

  static reloco::result<void> try_read(address_type addr, reloco::span<std::byte> dst) noexcept {
    std::memcpy(dst.data(), kernel_store().data() + (addr - kKernelBase), dst.size());
    return {};
  }

  static reloco::result<void> try_write(address_type addr, reloco::span<const std::byte> src) noexcept {
    std::memcpy(kernel_store().data() + (addr - kKernelBase), src.data(), src.size());
    return {};
  }

  static reloco::result<void> try_read_nofault(address_type addr, reloco::span<std::byte> dst) noexcept {
    return try_read(addr, dst);
  }

  static reloco::result<void> try_write_nofault(address_type addr, reloco::span<const std::byte> src) noexcept {
    return try_write(addr, src);
  }
};

namespace {

// A tag that is never given a target_ptr_space_traits specialization at
// all -- fine as long as only the pure-arithmetic API (which never
// touches space_traits) is exercised against it.
struct arith_space {};

// --------------------------------------------------------------------
// SFINAE detection helpers used to assert, at compile time, that the
// materialize/store family is unavailable for target_ptr<void, ...>.
// --------------------------------------------------------------------

template <typename TP, typename = void> struct has_try_materialize : std::false_type {};
template <typename TP>
struct has_try_materialize<TP, std::void_t<decltype(std::declval<const TP &>().try_materialize())>> : std::true_type {};

struct small_payload {
  std::int32_t a;
  std::int32_t b;
};

// Larger than target_ptr's 32-byte stack_inline_threshold, so materialize/
// store/as_bytes(_mut) must take the heap-backed scratch-buffer path.
struct large_payload {
  std::uint8_t data[64];

  bool operator==(const large_payload &other) const noexcept {
    return std::memcmp(data, other.data, sizeof(data)) == 0;
  }
};

static_assert(sizeof(large_payload) > 32,
              "large_payload must exceed the stack-inline threshold for this test to be meaningful");

static_assert(has_try_materialize<target_ptr<small_payload, test_user_space>>::value,
              "materialize must be available for a non-void T");
static_assert(!has_try_materialize<target_ptr<void, test_user_space>>::value,
              "materialize must be SFINAE'd away for T = void");

TEST(TargetPtrTest, NullAndBooleanState) {
  target_ptr<small_payload, test_user_space> p1;
  EXPECT_TRUE(p1.is_null());
  EXPECT_FALSE(static_cast<bool>(p1));
  EXPECT_EQ(p1.raw_address(), 0u);

  target_ptr<small_payload, test_user_space> p2 = nullptr;
  EXPECT_TRUE(p2.is_null());

  target_ptr<small_payload, test_user_space> p3(kUserBase);
  EXPECT_FALSE(p3.is_null());
  EXPECT_TRUE(static_cast<bool>(p3));
  EXPECT_EQ(p3.raw_address(), kUserBase);
}

TEST(TargetPtrTest, ComparisonOperators) {
  target_ptr<small_payload, test_user_space> a(kUserBase);
  target_ptr<small_payload, test_user_space> b(kUserBase);
  target_ptr<small_payload, test_user_space> c(kUserBase + 8);

  EXPECT_TRUE(a == b);
  EXPECT_FALSE(a != b);
  EXPECT_TRUE(a != c);
  EXPECT_TRUE(a < c);
  EXPECT_TRUE(a <= c);
  EXPECT_TRUE(a <= b);
  EXPECT_TRUE(c > a);
  EXPECT_TRUE(c >= a);
  EXPECT_TRUE(a >= b);
  EXPECT_FALSE(c < a);
}

TEST(TargetPtrTest, CastTypeStaysInSameSpace) {
  target_ptr<void, test_user_space> void_ptr(kUserBase);
  target_ptr<small_payload, test_user_space> typed_ptr = void_ptr.cast_type<small_payload>();
  EXPECT_EQ(typed_ptr.raw_address(), kUserBase);
}

TEST(TargetPtrTest, ZeroOverheadLayout) {
  EXPECT_EQ(sizeof(target_ptr<small_payload, test_user_space>), sizeof(std::uintptr_t));
  EXPECT_EQ(sizeof(target_ptr<large_payload, test_user_space>), sizeof(std::uintptr_t));
  EXPECT_TRUE((std::is_standard_layout_v<target_ptr<small_payload, test_user_space>>));
}

TEST(TargetPtrTest, MaterializeAndStoreRoundTripSmallType) {
  target_ptr<small_payload, test_user_space> p(kUserBase);
  small_payload val{42, -7};
  ASSERT_TRUE(p.try_store(val).has_value());

  auto round_trip = p.try_materialize();
  ASSERT_TRUE(round_trip.has_value());
  EXPECT_EQ(round_trip.value().a, 42);
  EXPECT_EQ(round_trip.value().b, -7);
}

TEST(TargetPtrTest, MaterializeAndStoreRoundTripLargeType) {
  // kUserBase is comfortably below kFaultThreshold, and large_payload's
  // 64 bytes stay within the "no fault" region used by other tests too.
  target_ptr<large_payload, test_user_space> p(kUserBase);
  large_payload val{};
  for (std::size_t i = 0; i < sizeof(val.data); ++i)
    val.data[i] = static_cast<std::uint8_t>(i);
  ASSERT_TRUE(p.try_store(val).has_value());

  auto round_trip = p.try_materialize();
  ASSERT_TRUE(round_trip.has_value());
  EXPECT_EQ(round_trip.value(), val);
}

TEST(TargetPtrTest, MaterializeToAndStoreSpans) {
  target_ptr<std::int32_t, test_user_space> p(kUserBase + 32);
  std::int32_t src[4] = {1, 2, 3, 4};
  ASSERT_TRUE(p.try_store(span<const std::int32_t>(src, 4)).has_value());

  std::int32_t dst[4] = {};
  ASSERT_TRUE(p.try_materialize_to(span<std::int32_t>(dst, 4)).has_value());
  EXPECT_EQ(std::memcmp(src, dst, sizeof(src)), 0);
}

TEST(TargetPtrTest, MaterializePtrAllocatesOnHeap) {
  target_ptr<small_payload, test_user_space> p(kUserBase);
  ASSERT_TRUE(p.try_store(small_payload{11, 22}).has_value());

  auto boxed = p.try_materialize_ptr();
  ASSERT_TRUE(boxed.has_value());
  EXPECT_EQ(boxed.value()->a, 11);
  EXPECT_EQ(boxed.value()->b, 22);
}

TEST(TargetPtrTest, AsBytesSmallAndLarge) {
  target_ptr<small_payload, test_user_space> small_ptr(kUserBase);
  ASSERT_TRUE(small_ptr.try_store(small_payload{5, 6}).has_value());
  auto small_bytes = small_ptr.try_as_bytes();
  ASSERT_TRUE(small_bytes.has_value());
  EXPECT_EQ(small_bytes.value().size(), sizeof(small_payload));

  target_ptr<large_payload, test_user_space> large_ptr(kUserBase);
  large_payload val{};
  val.data[0] = 0xAB;
  ASSERT_TRUE(large_ptr.try_store(val).has_value());
  auto large_bytes = large_ptr.try_as_bytes();
  ASSERT_TRUE(large_bytes.has_value());
  EXPECT_EQ(large_bytes.value().size(), sizeof(large_payload));
  EXPECT_EQ(static_cast<std::uint8_t>(large_bytes.value()[0]), 0xAB);
}

TEST(TargetPtrTest, AsBytesMutSmallAndLargeAreMutable) {
  target_ptr<small_payload, test_user_space> small_ptr(kUserBase);
  ASSERT_TRUE(small_ptr.try_store(small_payload{1, 2}).has_value());
  auto small_mut_res = small_ptr.try_as_bytes_mut();
  ASSERT_TRUE(small_mut_res.has_value());
  bytes_mut &small_mut = small_mut_res.value();
  auto small_span = small_mut.as_span();
  small_span[0] = std::byte{0xFF};
  EXPECT_EQ(static_cast<std::uint8_t>(small_span[0]), 0xFF);

  target_ptr<large_payload, test_user_space> large_ptr(kUserBase);
  large_payload val{};
  ASSERT_TRUE(large_ptr.try_store(val).has_value());
  auto large_mut_res = large_ptr.try_as_bytes_mut();
  ASSERT_TRUE(large_mut_res.has_value());
  bytes_mut &large_mut = large_mut_res.value();
  auto large_span = large_mut.as_span();
  EXPECT_EQ(large_span.size(), sizeof(large_payload));
  large_span[sizeof(large_payload) - 1] = std::byte{0x42};
  EXPECT_EQ(static_cast<std::uint8_t>(large_span[sizeof(large_payload) - 1]), 0x42);
}

TEST(TargetPtrTest, NofaultSucceedsOutsideFaultRegion) {
  target_ptr<small_payload, test_user_space> p(kUserBase);
  ASSERT_TRUE(p.try_store_nofault(small_payload{3, 4}).has_value());
  auto val = p.try_materialize_nofault();
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(val.value().a, 3);
  EXPECT_EQ(val.value().b, 4);
}

TEST(TargetPtrTest, NofaultFailsWithPageFaultInsideFaultRegion) {
  // kFaultThreshold = kUserBase + 128; place the pointer so the access
  // straddles/lands past the threshold.
  target_ptr<small_payload, test_user_space> p(kFaultThreshold);

  auto store_res = p.try_store_nofault(small_payload{1, 1});
  ASSERT_FALSE(store_res.has_value());
  EXPECT_EQ(store_res.error(), error::page_fault);

  auto materialize_res = p.try_materialize_nofault();
  ASSERT_FALSE(materialize_res.has_value());
  EXPECT_EQ(materialize_res.error(), error::page_fault);

  // The plain, fault-resolving accessor succeeds at the very same address.
  EXPECT_TRUE(p.try_store(small_payload{1, 1}).has_value());
  EXPECT_TRUE(p.try_materialize().has_value());
}

TEST(TargetPtrTest, OutOfRangeIsRejectedBeforeReachingSpaceTraits) {
  target_ptr<small_payload, test_user_space> below(kUserBase - 1);
  auto res = below.try_materialize();
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), error::out_of_range);
  EXPECT_FALSE(below.is_in_range());

  target_ptr<small_payload, test_user_space> above(kUserBase + kUserStoreSize);
  EXPECT_FALSE(above.is_in_range());

  target_ptr<small_payload, test_user_space> in_range(kUserBase);
  EXPECT_TRUE(in_range.is_in_range());
}

TEST(TargetPtrTest, NullPointerIsRejectedBeforeReachingSpaceTraits) {
  target_ptr<small_payload, test_user_space> null_ptr;
  auto res = null_ptr.try_materialize();
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), error::invalid_argument);
}

TEST(TargetPtrTest, SpaceWithoutDeclaredRangeAcceptsEveryAddress) {
  target_ptr<small_payload, test_kernel_space> p(kKernelBase);
  EXPECT_TRUE(p.is_in_range());
  ASSERT_TRUE(p.try_store(small_payload{9, 10}).has_value());
  auto res = p.try_materialize();
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(res.value().a, 9);
  EXPECT_EQ(res.value().b, 10);
}

TEST(TargetPtrTest, CheckedArithmeticSucceedsAndFails) {
  using small_ptr_type = target_ptr<std::uint8_t, arith_space, std::uint8_t>;
  small_ptr_type p(200);

  auto ok = p.checked_add(50);
  ASSERT_TRUE(ok.has_value());
  EXPECT_EQ(ok.value().raw_address(), 250);

  auto overflow = p.checked_add(100);
  ASSERT_FALSE(overflow.has_value());
  EXPECT_EQ(overflow.error(), error::integer_overflow);

  small_ptr_type q(10);
  auto underflow = q.checked_sub(20);
  ASSERT_FALSE(underflow.has_value());
  EXPECT_EQ(underflow.error(), error::integer_overflow);

  auto sub_ok = p.checked_sub(50);
  ASSERT_TRUE(sub_ok.has_value());
  EXPECT_EQ(sub_ok.value().raw_address(), 150);
}

TEST(TargetPtrTest, WrappingArithmeticNeverFails) {
  using small_ptr_type = target_ptr<std::uint8_t, arith_space, std::uint8_t>;
  small_ptr_type p(200);
  small_ptr_type q(10);

  EXPECT_EQ(p.wrapping_add(100).raw_address(), static_cast<std::uint8_t>(300));
  // difference_type for an address_type of uint8_t is int8_t (signed char),
  // so the subtrahend must itself fit [-128, 127]; use a base address (q)
  // close enough to zero that a sub-128 offset still wraps past it.
  EXPECT_EQ(q.wrapping_sub(100).raw_address(), static_cast<std::uint8_t>(10 - 100));
}

TEST(TargetPtrTest, SaturatingArithmeticClampsInsteadOfFailing) {
  using small_ptr_type = target_ptr<std::uint8_t, arith_space, std::uint8_t>;
  small_ptr_type p(200);

  EXPECT_EQ(p.saturating_add(100).raw_address(), 255);

  small_ptr_type q(10);
  EXPECT_EQ(q.saturating_sub(100).raw_address(), 0);
}

TEST(TargetPtrTest, CheckedOffsetFromComputesElementDistance) {
  using ptr_type = target_ptr<std::uint32_t, arith_space, std::uintptr_t>;
  ptr_type origin(1000);
  ptr_type ahead(1000 + 4 * 4); // 4 elements of 4 bytes each
  ptr_type behind(1000 - 4 * 3);

  auto forward = ahead.checked_offset_from(origin);
  ASSERT_TRUE(forward.has_value());
  EXPECT_EQ(forward.value(), 4);

  auto backward = behind.checked_offset_from(origin);
  ASSERT_TRUE(backward.has_value());
  EXPECT_EQ(backward.value(), -3);

  ptr_type misaligned(1000 + 2);
  auto bad = misaligned.checked_offset_from(origin);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error(), error::invalid_argument);
}

} // namespace

RELOCO_END_UNSAFE_BUFFER_USAGE