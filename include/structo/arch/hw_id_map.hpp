// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace structo::arch {

// -------------------------------------------------------------------------
// Multiplication-Free Hardware ID Mixer
// -------------------------------------------------------------------------
template <typename HwId> struct default_hw_id_hash {
  [[nodiscard]] constexpr std::size_t operator()(HwId id) const noexcept {
    if constexpr (sizeof(HwId) <= 4) {
      // ARM barrel-shifter friendly: 2 instructions, 0 multipliers
      uint32_t val = static_cast<uint32_t>(id);
      val ^= (val >> 8);
      val ^= (val >> 16);
      return static_cast<std::size_t>(val);
    } else {
      uint64_t val = static_cast<uint64_t>(id);
      val ^= (val >> 32);
      val ^= (val >> 16);
      val ^= (val >> 8);
      return static_cast<std::size_t>(static_cast<uint32_t>(val));
    }
  }
};

// -------------------------------------------------------------------------
// Two-Tier Hardware ID Lookup Table
// -------------------------------------------------------------------------
/**
 * @brief Two-tier lookup: 64-byte direct hash filter (L1) -> sorted array (L2).
 * @tparam HwId    Hardware ID type (uint32_t for ARM MPIDR).
 * @tparam MaxCpus Maximum logical cores supported (<= 254).
 * @tparam L1Size  Size of L1 filter (must be power of two, defaults to 64).
 */
template <typename HwId = uint32_t, std::size_t MaxCpus = 8, std::size_t L1Size = 64,
          typename Hash = default_hw_id_hash<HwId>>
class hw_id_lut {
  static_assert((L1Size & (L1Size - 1)) == 0, "L1Size must be a power of two");
  static_assert(L1Size <= 64, "L1Size > 64 requires wider collision bitmasks");
  static_assert(MaxCpus <= 254, "CPU indices 0-254 supported (0xFF is tombstone)");

public:
  using hw_id_type = HwId;
  using hasher = Hash;

  static inline constexpr std::size_t max_cpus = MaxCpus;
  static inline constexpr std::size_t l1_capacity = L1Size;
  static inline constexpr std::size_t l1_mask = L1Size - 1;
  static inline constexpr uint8_t tombstone = 0xFF;
  static inline constexpr std::size_t invalid_index = static_cast<std::size_t>(-1);

  struct l2_entry {
    hw_id_type hw_id{};
    uint8_t cpu_idx{tombstone};
  };

  constexpr hw_id_lut() noexcept { clear(); }

  /**
   * @brief Register a hardware ID mapping.
   * Inserts into sorted L2, then registers into L1 if no hash collision occurs.
   */
  constexpr bool insert(hw_id_type id, uint8_t cpu_idx) noexcept {
    if (cpu_idx >= max_cpus || m_l2_size >= max_cpus || cpu_idx == tombstone) {
      return false;
    }

    // Maintain L2 sorted by hw_id (Insertion Sort for tiny N)
    std::size_t insert_pos = 0;
    while (insert_pos < m_l2_size && m_l2[insert_pos].hw_id < id) {
      ++insert_pos;
    }

    // If entry already exists, update cpu_idx
    if (insert_pos < m_l2_size && m_l2[insert_pos].hw_id == id) {
      m_l2[insert_pos].cpu_idx = cpu_idx;
      m_hw_by_cpu[cpu_idx] = id;
      return true;
    }

    // Shift elements right
    for (std::size_t i = m_l2_size; i > insert_pos; --i) {
      m_l2[i] = m_l2[i - 1];
    }
    m_l2[insert_pos] = {id, cpu_idx};
    ++m_l2_size;

    // Direct inverse map for verification
    m_hw_by_cpu[cpu_idx] = id;

    // Populate L1 Table
    const std::size_t slot = hasher{}(id)&l1_mask;
    const uint64_t slot_bit = (1ULL << slot);

    if ((m_l1_collided & slot_bit) != 0) {
      // Already collided: remains a tombstone, must use L2
      m_l1[slot] = tombstone;
    } else if ((m_l1_claimed & slot_bit) != 0) {
      // Second key hashing to the same slot -> Demote to tombstone
      m_l1_collided |= slot_bit;
      m_l1[slot] = tombstone;
    } else {
      // First key at this slot -> Fast-path direct hit!
      m_l1_claimed |= slot_bit;
      m_l1[slot] = cpu_idx;
    }

    return true;
  }

  /**
   * @brief Resolve hardware ID to core index.
   * Path 1: Check L1 filter and verify against inverse map (O(1)).
   * Path 2: Fall back to L2 binary search (O(log N)).
   */
  [[nodiscard]] constexpr std::size_t find(hw_id_type id) const noexcept {
    // --- Level 1: Direct Mapped Filter ---
    const std::size_t slot = hasher{}(id)&l1_mask;
    const uint8_t candidate_idx = m_l1[slot];

    if (candidate_idx != tombstone) {
      // Verify candidate to eliminate false positives from unregistered IDs
      if (candidate_idx < max_cpus && m_hw_by_cpu[candidate_idx] == id) {
        return candidate_idx;
      }
    }

    // --- Level 2: Binary Search in Sorted L2 ---
    return binary_search_l2(id);
  }

  [[nodiscard]] constexpr bool lookup(hw_id_type id, std::size_t &out_idx) const noexcept {
    const std::size_t res = find(id);
    if (res != invalid_index) {
      out_idx = res;
      return true;
    }
    return false;
  }

  constexpr void clear() noexcept {
    for (std::size_t i = 0; i < l1_capacity; ++i) {
      m_l1[i] = tombstone;
    }
    for (std::size_t i = 0; i < max_cpus; ++i) {
      m_l2[i] = l2_entry{};
      m_hw_by_cpu[i] = hw_id_type{};
    }
    m_l1_claimed = 0;
    m_l1_collided = 0;
    m_l2_size = 0;
  }

  [[nodiscard]] constexpr std::size_t size() const noexcept { return m_l2_size; }

private:
  // Binary search in sorted m_l2 array
  [[nodiscard]] constexpr std::size_t binary_search_l2(hw_id_type id) const noexcept {
    std::size_t low = 0;
    std::size_t high = m_l2_size;

    while (low < high) {
      const std::size_t mid = low + (high - low) / 2;
      if (m_l2[mid].hw_id < id) {
        low = mid + 1;
      } else if (m_l2[mid].hw_id > id) {
        high = mid;
      } else {
        return m_l2[mid].cpu_idx;
      }
    }

    return invalid_index;
  }

  // --- Data Members (Aligned for L1 D-Cache) ---
  alignas(64) uint8_t m_l1[l1_capacity]; // Cache line 0: 64 bytes L1 filter
  uint64_t m_l1_claimed{0};              // Track first occupant
  uint64_t m_l1_collided{0};             // Track multi-occupant collision
  l2_entry m_l2[max_cpus]{};             // Sorted backup array
  hw_id_type m_hw_by_cpu[max_cpus]{};    // Quick inverse validation map
  std::size_t m_l2_size{0};
};

} // namespace structo::arch
