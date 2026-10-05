// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/time_source_ref.hpp>

using structo::hw::cycles;
using structo::hw::time_source_capabilities;
using structo::hw::time_source_ref;
using structo::hw::time_source_traits;
using reloco::error;
using reloco::result;
using reloco::unexpected;

namespace {

// --------------------------------------------------------------------
// A fake free-running counter backend: a settable raw value (and an
// optional scripted failure), plus settable capabilities.
// --------------------------------------------------------------------
struct fake_counter {
  result<cycles> next_value = cycles{0};
  time_source_capabilities caps{};
};

} // namespace

template <> struct structo::hw::time_source_traits<fake_counter> {
  static result<cycles> try_now(fake_counter &b) noexcept { return b.next_value; }
  static time_source_capabilities capabilities(fake_counter &b) noexcept { return b.caps; }
};

namespace {

class TimeSourceRefTest : public ::testing::Test {
protected:
  fake_counter dev_;
};

} // namespace

TEST(TimeSourceCapabilitiesTest, EqualityComparesEveryField) {
  time_source_capabilities a;
  a.clock_hz = 1'000;
  a.max_value = cycles{UINT64_MAX};
  a.is_monotonic = true;
  a.is_per_cpu = false;
  time_source_capabilities b = a;
  EXPECT_EQ(a, b);

  b.clock_hz = 2'000;
  EXPECT_NE(a, b);

  b = a;
  b.is_monotonic = false;
  EXPECT_NE(a, b);

  b = a;
  b.is_per_cpu = true;
  EXPECT_NE(a, b);

  b = a;
  b.max_value = cycles{1};
  EXPECT_NE(a, b);
}

TEST_F(TimeSourceRefTest, UnboundRefFailsEveryOperation) {
  time_source_ref unbound;
  EXPECT_FALSE(static_cast<bool>(unbound));

  auto now = unbound.try_now();
  ASSERT_FALSE(now.has_value());
  EXPECT_EQ(now.error(), error::unsupported_operation);

  auto caps = unbound.capabilities();
  ASSERT_FALSE(caps.has_value());
  EXPECT_EQ(caps.error(), error::unsupported_operation);
}

TEST_F(TimeSourceRefTest, BoundRefReportsTrue) {
  time_source_ref ref(dev_);
  EXPECT_TRUE(static_cast<bool>(ref));
}

TEST_F(TimeSourceRefTest, TryNowForwardsBackendValue) {
  dev_.next_value = cycles{123'456};
  time_source_ref ref(dev_);

  auto now = ref.try_now();
  ASSERT_TRUE(now.has_value());
  EXPECT_EQ(now.value().raw(), 123'456u);
}

TEST_F(TimeSourceRefTest, TryNowForwardsBackendError) {
  dev_.next_value = unexpected(error::unsupported_operation);
  time_source_ref ref(dev_);

  auto now = ref.try_now();
  ASSERT_FALSE(now.has_value());
  EXPECT_EQ(now.error(), error::unsupported_operation);
}

TEST_F(TimeSourceRefTest, CapabilitiesRoundTripThroughTheRef) {
  dev_.caps.clock_hz = 24'000'000ULL;
  dev_.caps.max_value = cycles{(1ull << 56) - 1};
  dev_.caps.is_monotonic = false;
  dev_.caps.is_per_cpu = true;
  time_source_ref ref(dev_);

  auto caps = ref.capabilities();
  ASSERT_TRUE(caps.has_value());
  EXPECT_EQ(caps.value(), dev_.caps);
}
