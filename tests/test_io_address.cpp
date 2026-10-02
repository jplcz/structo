// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/io_address.hpp>

namespace {

using namespace structo;

struct mmio_regs {
  std::uint32_t ctrl;
  std::uint32_t status;
};

TEST(IoAddressTest, NullAddressIsTildeZero) {
  io_address<void, device_io_space> p1;
  EXPECT_TRUE(p1.is_null());
  EXPECT_EQ(p1.value, ~std::uint64_t(0));
  EXPECT_FALSE(static_cast<bool>(p1));

  io_address<void, device_io_space> p2 = nullptr;
  EXPECT_TRUE(p2.is_null());

  io_address<void, device_io_space> p3(0x1000);
  EXPECT_FALSE(p3.is_null());
  EXPECT_TRUE(static_cast<bool>(p3));
  EXPECT_EQ(p3.value, 0x1000u);
}

TEST(IoAddressTest, CustomIntegerTypeForPortIoSpace) {
  io_address<void, port_io_space, std::uint16_t> port(0x3F8);
  EXPECT_EQ(port.value, 0x3F8u);
  EXPECT_FALSE(port.is_null());
  EXPECT_EQ(sizeof(port.value), sizeof(std::uint16_t));
}

TEST(IoAddressTest, TagAndTypeCasting) {
  io_address<void, device_io_space> void_addr(0x4000);

  auto reg_addr = void_addr.cast_type<mmio_regs>();
  EXPECT_EQ(reg_addr.value, 0x4000u);

  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  auto secure_addr = reg_addr.cast_space<secure_io_space>();
  EXPECT_EQ(secure_addr.value, 0x4000u);
  RELOCO_END_UNSAFE_BUFFER_USAGE
}

TEST(IoAddressTest, ComparisonOperators) {
  io_address<void, device_io_space> a(0x1000);
  io_address<void, device_io_space> b(0x1000);
  io_address<void, device_io_space> c(0x2000);

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

TEST(IoAddressTest, ZeroOverheadLayout) {
  struct device_regs {
    io_address<mmio_regs, device_io_space> next;
  };
  EXPECT_EQ(sizeof(device_regs), sizeof(std::uint64_t));
  EXPECT_TRUE(std::is_standard_layout_v<device_regs>);
}

TEST(IoAddressTest, CheckedAddAndSubStrideBySizeofT) {
  io_address<mmio_regs, device_io_space> base(0x1000);

  auto next = base.checked_add(1);
  ASSERT_TRUE(next.has_value());
  EXPECT_EQ(next.value().value, 0x1000 + sizeof(mmio_regs));

  auto back = next.value().checked_sub(1);
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(back.value().value, base.value);
}

TEST(IoAddressTest, CheckedAddFailsOnOverflow) {
  io_address<std::uint8_t, device_io_space, std::uint8_t> base(250);
  auto res = base.checked_add(10);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), error::integer_overflow);
}

TEST(IoAddressTest, CheckedSubFailsOnUnderflow) {
  io_address<std::uint8_t, device_io_space, std::uint8_t> base(2);
  auto res = base.checked_sub(10);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), error::integer_overflow);
}

TEST(IoAddressTest, CheckedOffsetFromComputesElementDistance) {
  io_address<std::uint32_t, device_io_space> origin(1000);
  io_address<std::uint32_t, device_io_space> ahead(1000 + 4 * 4);
  io_address<std::uint32_t, device_io_space> behind(1000 - 4 * 3);

  auto forward = ahead.checked_offset_from(origin);
  ASSERT_TRUE(forward.has_value());
  EXPECT_EQ(forward.value(), 4);

  auto backward = behind.checked_offset_from(origin);
  ASSERT_TRUE(backward.has_value());
  EXPECT_EQ(backward.value(), -3);

  io_address<std::uint32_t, device_io_space> misaligned(1000 + 2);
  auto bad = misaligned.checked_offset_from(origin);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error(), error::invalid_argument);
}

TEST(IoMathTest, OffsetAndAlignment) {
  using reg = reg32;
  io_address<void, device_io_space> aligned(0x2000);
  io_address<void, device_io_space> unaligned(0x2003);

  EXPECT_EQ(io_math::offset<reg>(aligned), 0u);
  EXPECT_EQ(io_math::offset<reg>(unaligned), 3u);
  EXPECT_TRUE(io_math::is_aligned<reg>(aligned));
  EXPECT_FALSE(io_math::is_aligned<reg>(unaligned));

  EXPECT_EQ(io_math::align_down<reg>(unaligned).value, 0x2000u);
  EXPECT_EQ(io_math::align_up<reg>(unaligned).value, 0x2004u);

  io_address<void, device_io_space> null_addr;
  EXPECT_EQ(io_math::offset<reg>(null_addr), 0u);
  EXPECT_FALSE(io_math::is_aligned<reg>(null_addr));
  EXPECT_TRUE(io_math::align_down<reg>(null_addr).is_null());
  EXPECT_TRUE(io_math::align_up<reg>(null_addr).is_null());
}

TEST(IoMathTest, AtRegComputesRegisterSlotAddress) {
  using gicd = reg_traits<4>;
  io_address<void, device_io_space> base(0x3000);

  auto reg5 = io_math::at_reg<gicd>(base, 5);
  ASSERT_TRUE(reg5.has_value());
  EXPECT_EQ(reg5.value().value, 0x3000u + 5u * 4u);

  auto reg_before = io_math::at_reg<gicd>(base, -2);
  ASSERT_TRUE(reg_before.has_value());
  EXPECT_EQ(reg_before.value().value, 0x3000u - 2u * 4u);
}

TEST(IoMathTest, AtRegFailsOnOverflow) {
  using gicd = reg_traits<4>;
  io_address<void, device_io_space, std::uint8_t> base(250);

  auto res = io_math::at_reg<gicd>(base, 10);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), error::integer_overflow);
}

TEST(IoMathTest, RegIndexIsInverseOfAtReg) {
  using gicd = reg_traits<4>;
  io_address<void, device_io_space> base(0x3000);

  auto reg5 = io_math::at_reg<gicd>(base, 5);
  ASSERT_TRUE(reg5.has_value());

  auto idx = io_math::reg_index<gicd>(reg5.value(), base);
  ASSERT_TRUE(idx.has_value());
  EXPECT_EQ(idx.value(), 5);

  auto reg_before = io_math::at_reg<gicd>(base, -3);
  ASSERT_TRUE(reg_before.has_value());
  auto idx_before = io_math::reg_index<gicd>(reg_before.value(), base);
  ASSERT_TRUE(idx_before.has_value());
  EXPECT_EQ(idx_before.value(), -3);
}

TEST(IoMathTest, RegIndexFailsOnMisalignment) {
  using gicd = reg_traits<4>;
  io_address<void, device_io_space> base(0x3000);
  io_address<void, device_io_space> misaligned(0x3000 + 6);

  auto idx = io_math::reg_index<gicd>(misaligned, base);
  ASSERT_FALSE(idx.has_value());
  EXPECT_EQ(idx.error(), error::invalid_argument);
}

} // namespace
