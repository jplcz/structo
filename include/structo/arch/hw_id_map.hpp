// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file hw_id_map.hpp
 * @brief `structo::arch::hw_id_lut<HwId, MaxCpus, L1Size, Hash>`: a
 * caller-owned, allocation-free, two-tier lookup table mapping an
 * architectural hardware ID (e.g. ARM's MPIDR_EL1) to a logical CPU
 * index.
 *
 * Built for the SMP boot path: firmware/devicetree enumerates CPUs by
 * raw hardware ID, but the rest of the kernel wants a dense `[0,
 * max_cpus)` logical index. Lookup is a fixed-size hash filter (L1, one
 * byte per slot, capacity a power of two up to 64) verified against a
 * direct inverse map to eliminate false positives in O(1); any hash
 * collision demotes that slot to a tombstone and falls back to a
 * sorted, duplicate-free array (L2) searched in O(log `MaxCpus`).
 * `default_hw_id_hash` is the bundled multiplication-free mixer tuned
 * for small (<=4-byte) and wide (8-byte) hardware IDs alike.
 */

#include <cstddef>
#include <cstdint>
#include <reloco/lifetime.hpp>
#include <type_traits>

namespace structo::arch {

// -------------------------------------------------------------------------
// Multiplication-Free Hardware ID Mixer
// -------------------------------------------------------------------------
/**
 * @brief Default `hw_id_lut` hash functor: a multiplication-free bit mixer
 * (XOR-shift cascade) chosen for cheap bare-metal codegen (no hardware
 * multiplier required).
 * @tparam HwId Hardware ID integer type being hashed.
 */
template <typename HwId> struct default_hw_id_hash {
  /**
   * @brief Mixes `id` into a `std::size_t` hash value.
   * @param id Hardware ID to hash.
   * @return Mixed hash value (only the low `log2(L1Size)` bits are
   * actually consumed by `hw_id_lut`, via `& l1_mask`).
   */
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
// Every index is bounded by MaxCpus/L1Size/m_l2_size: L1 slots are masked with L1Size-1 and
// cpu indices are checked against MaxCpus before use.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

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
   * If `id` is already registered, updates its `cpu_idx` in place instead of
   * growing the table (keeping L1, L2, and the inverse map all consistent).
   * @param id      Hardware ID to register (e.g. an MPIDR value).
   * @param cpu_idx Logical CPU index to associate with `id`.
   * @return `true` on success; `false` if `cpu_idx` is out of range (or the
   * reserved tombstone value) or the table is already full.
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
      const uint8_t old_cpu_idx = m_l2[insert_pos].cpu_idx;
      m_l2[insert_pos].cpu_idx = cpu_idx;
      if (old_cpu_idx != cpu_idx) {
        // Drop the stale inverse-map entry for the vacated cpu_idx so a
        // later insert() reusing a different hw_id at that index cannot be
        // confused with this one.
        m_hw_by_cpu[old_cpu_idx] = hw_id_type{};
      }
      m_hw_by_cpu[cpu_idx] = id;

      // Repoint the L1 fast-path slot too, if this id owns one uncollided:
      // leaving it at old_cpu_idx would make find() return the superseded
      // index via the L1 path even though L2 (and m_hw_by_cpu) already
      // moved on.
      const std::size_t slot = hasher{}(id)&l1_mask;
      const uint64_t slot_bit = (1ULL << slot);
      if ((m_l1_collided & slot_bit) == 0) {
        m_l1_claimed |= slot_bit;
        m_l1[slot] = cpu_idx;
      }
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
   * @param id Hardware ID to resolve.
   * @return The registered logical CPU index, or `invalid_index` if `id`
   * was never registered via `insert()`.
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

  /**
   * @brief `find()` variant reporting success/failure instead of a sentinel.
   * @param id      Hardware ID to resolve.
   * @param out_idx Set to the resolved logical CPU index on success; left
   * untouched on failure.
   * @return `true` if `id` was found; `false` otherwise.
   */
  [[nodiscard]] constexpr bool lookup(hw_id_type id, std::size_t &out_idx) const noexcept {
    const std::size_t res = find(id);
    if (res != invalid_index) {
      out_idx = res;
      return true;
    }
    return false;
  }

  /**
   * @brief Resets the table to empty (no registered hardware IDs).
   */
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

  /**
   * @brief Number of hardware IDs currently registered.
   * @return Current L2 occupancy (always `<= max_cpus`).
   */
  [[nodiscard]] constexpr std::size_t size() const noexcept { return m_l2_size; }

private:
  /**
   * @brief Binary search for `id` in the sorted, duplicate-free L2 array.
   * @param id Hardware ID to resolve.
   * @return The registered logical CPU index, or `invalid_index` if not found.
   */
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

RELOCO_END_UNSAFE_BUFFER_USAGE

} // namespace structo::arch
