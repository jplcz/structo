// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#if !defined(_MSC_VER)
#include <gtest/gtest.h>
#include <structo/phys_translator.hpp>

namespace {

using namespace structo;

// Dummy hardware struct of known size (8 bytes)
struct hw_register {
  uint32_t control;
  uint32_t status;
};

// ============================================================================
// Policy 1: Stateful Linear IOMMU (Host -> DMA)
// ============================================================================
struct linear_iommu_policy {
  using from_space = host_phys_space;
  using to_space = dma_bus_space;

  uint64_t bus_offset;
  uint64_t window_size;

  result<uint64_t> translate(uint64_t addr, uint64_t size) const noexcept {
    // Prevent integer overflow during bounds check
    if (addr > ~uint64_t(0) - size) {
      return unexpected(error::invalid_argument);
    }

    // Check if the ENTIRE requested block fits within the IOMMU window
    if (addr + size > window_size) {
      return unexpected(error::out_of_range);
    }

    return addr + bus_offset;
  }
};

TEST(PhysCastTest, ExplicitSizeDynamicBuffer) {
  phys_addr<void, host_phys_space> host_buf(0x1000);
  linear_iommu_policy iommu{0xC0000000, 0x10000000};

  // We are mapping a 4KB dynamically sized void buffer
  auto dma_res = physical_cast(host_buf, 4096, iommu);

  ASSERT_TRUE(dma_res.has_value());
  EXPECT_EQ(dma_res.value().value, 0xC0001000);
}

TEST(PhysCastTest, ImplicitSizeDeduction) {
  phys_addr<hw_register, host_phys_space> host_reg(0x2000);
  linear_iommu_policy iommu{0xC0000000, 0x10000000};

  // No size passed! The compiler automatically deduces sizeof(hw_register) = 8 bytes.
  auto dma_res = physical_cast(host_reg, iommu);

  ASSERT_TRUE(dma_res.has_value());
  EXPECT_EQ(dma_res.value().value, 0xC0002000);
}

TEST(PhysCastTest, SizeExceedsBoundary) {
  linear_iommu_policy iommu{0xC0000000, 0x1000}; // Only a 4KB window mapped (0x1000 size)

  // The base address is mapped (0x0FFC is inside the 0x1000 window)
  phys_addr<hw_register, host_phys_space> host_reg(0x0FFC);

  // BUT: The struct is 8 bytes long. It spans from 0x0FFC to 0x1004.
  // The translation must fail because the tail of the struct is outside the IOMMU window.
  auto dma_res = physical_cast(host_reg, iommu);

  EXPECT_FALSE(dma_res.has_value());
  EXPECT_EQ(dma_res.error(), error::out_of_range);
}

// ============================================================================
// Policy 2: EPT Walker (Guest -> Host)
// ============================================================================
struct mock_ept_policy {
  using from_space = guest_phys_space;
  using to_space = host_phys_space;

  result<uint64_t> translate(uint64_t gpa, uint64_t size) const noexcept {
    // Mocking an EPT table where ONLY page 0x4000 (4KB) is mapped.
    uint64_t page_base = 0x4000;
    uint64_t page_size = 4096;

    if (gpa >= page_base && (gpa + size) <= (page_base + page_size)) {
      uint64_t offset_in_page = gpa - page_base;
      return 0x8A000 + offset_in_page; // Mapped to HPA 0x8A000
    }

    return unexpected(error::security_violation);
  }
};

TEST(PhysCastTest, EptContiguousCheck) {
  mock_ept_policy ept;

  // A struct perfectly fits at the end of the mapped page
  phys_addr<hw_register, guest_phys_space> gpa_ok(0x4FF8);
  EXPECT_TRUE(physical_cast(gpa_ok, ept).has_value());

  // A struct crosses the page boundary into unmapped territory
  phys_addr<hw_register, guest_phys_space> gpa_spill(0x4FFA);

  auto res = physical_cast(gpa_spill, ept);
  EXPECT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), error::security_violation);
}

} // namespace
#endif