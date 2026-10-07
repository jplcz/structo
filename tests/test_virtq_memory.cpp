// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <reloco/array.hpp>
#include <structo/virtio/virtq_memory.hpp>

namespace {

using namespace structo;
using namespace structo::virtio;

struct guest_space {};
struct other_space {};

using guest_addr = phys_addr<void, guest_space>;
using guest_mem = direct_virtq_memory<guest_space>;
using traits = virtq_memory_traits<guest_mem, guest_space>;

constexpr std::uint64_t kBase = 0x8000'0000;
constexpr std::size_t kSize = 256;

class VirtqMemoryTest : public ::testing::Test {
protected:
  alignas(16) reloco::array<std::byte, kSize> backing_{};
  guest_mem mem_{backing_.data(), kSize, guest_addr{kBase}};
};

TEST_F(VirtqMemoryTest, UnboundFails) {
  guest_mem unbound;
  EXPECT_FALSE(unbound.is_bound());
  auto r = traits::try_load16(unbound, guest_addr{kBase});
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::not_initialized);
}

TEST_F(VirtqMemoryTest, ObjectRoundTrip) {
  const virtq_desc d{0x1122334455667788ull, 4096, desc_f_next | desc_f_write, 5};
  ASSERT_TRUE(try_write_object(mem_, guest_addr{kBase + 16}, d).has_value());

  auto back = try_read_object<virtq_desc>(mem_, guest_addr{kBase + 16});
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(back->addr, d.addr);
  EXPECT_EQ(back->len, d.len);
  EXPECT_EQ(back->flags, d.flags);
  EXPECT_EQ(back->next, d.next);
}

TEST_F(VirtqMemoryTest, ReadIsASnapshot) {
  ASSERT_TRUE(try_write_object(mem_, guest_addr{kBase}, std::uint32_t{0xAABBCCDDu}).has_value());
  auto snap = try_read_object<std::uint32_t>(mem_, guest_addr{kBase});
  ASSERT_TRUE(snap.has_value());

  // The peer rewrites the memory after the snapshot; the copy is unaffected.
  ASSERT_TRUE(try_write_object(mem_, guest_addr{kBase}, std::uint32_t{0}).has_value());
  EXPECT_EQ(*snap, 0xAABBCCDDu);
}

TEST_F(VirtqMemoryTest, Load16Store16) {
  ASSERT_TRUE(traits::try_store16(mem_, guest_addr{kBase + 2}, 0xBEEF).has_value());
  auto v = traits::try_load16(mem_, guest_addr{kBase + 2});
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(*v, 0xBEEF);
}

TEST_F(VirtqMemoryTest, Load16RejectsMisaligned) {
  auto v = traits::try_load16(mem_, guest_addr{kBase + 1});
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error(), reloco::error::invalid_argument);
  EXPECT_FALSE(traits::try_store16(mem_, guest_addr{kBase + 3}, 1).has_value());
}

TEST_F(VirtqMemoryTest, BoundsAreChecked) {
  // Last valid 16-bit slot.
  EXPECT_TRUE(traits::try_load16(mem_, guest_addr{kBase + kSize - 2}).has_value());
  // One past the end.
  auto past = traits::try_load16(mem_, guest_addr{kBase + kSize});
  ASSERT_FALSE(past.has_value());
  EXPECT_EQ(past.error(), reloco::error::out_of_range);
  // Straddling the end.
  EXPECT_FALSE(try_read_object<std::uint64_t>(mem_, guest_addr{kBase + kSize - 4}).has_value());
  // Below the window.
  EXPECT_FALSE(traits::try_load16(mem_, guest_addr{kBase - 2}).has_value());
  EXPECT_FALSE(traits::try_load16(mem_, guest_addr{0x10}).has_value());
}

TEST_F(VirtqMemoryTest, HostileLengthsCannotOverflowBounds) {
  reloco::array<std::byte, 8> buf{};
  // addr + len would wrap if computed naively; len is larger than the window.
  auto r = traits::try_read(mem_, guest_addr{kBase + 8}, reloco::span<std::byte>(buf.data(), buf.size()));
  EXPECT_TRUE(r.has_value());
  EXPECT_FALSE(traits::try_read(mem_, guest_addr{0xFFFFFFFFFFFFFFF8ull}, reloco::span<std::byte>(buf.data(), 8))
                   .has_value());
}

TEST_F(VirtqMemoryTest, ZeroLengthAccessInRange) {
  EXPECT_TRUE(traits::try_read(mem_, guest_addr{kBase + kSize}, reloco::span<std::byte>()).has_value());
}

TEST_F(VirtqMemoryTest, SeparateSpacesAreDistinctTypes) {
  // A different Space tag selects a different traits specialization;
  // a guest_addr cannot be passed where an other_space address is required.
  static_assert(!std::is_convertible_v<guest_addr, phys_addr<void, other_space>>);
  static_assert(!std::is_same_v<direct_virtq_memory<guest_space>, direct_virtq_memory<other_space>>);
  SUCCEED();
}

} // namespace
