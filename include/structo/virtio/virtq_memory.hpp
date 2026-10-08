// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file virtq_memory.hpp
 * @brief `structo::virtio::virtq_memory_traits<Mem, Space>`: the
 * customization point through which virtqueue code reaches ring and buffer
 * memory, plus `direct_virtq_memory<Space>`, a bounds-checked backend over
 * a flat host-mapped window.
 *
 * Virtqueue memory is not necessarily directly dereferenceable by the code
 * that owns the queue: a driver talks to memory it shares with a device
 * (possibly non-cacheable, possibly addressed differently by the device),
 * while a hypervisor/VMM device model reaches *guest* RAM through a
 * fallible guest-physical -> host mapping, and that memory can change under
 * it at any time (a hostile guest racing the device). So every access is
 * address-space-tagged and fallible, and the device side copies data out of
 * guest memory instead of holding references into it.
 *
 * `Space` is the address-space tag of the `phys_addr<void, Space>` handed to
 * the traits. Ring areas and descriptor buffers may live in different spaces
 * (`RingSpace` vs `BufSpace`), each with its own `Mem` binding; the same
 * traits template serves both.
 *
 * ## Customization point
 *
 * Left undefined for any `(Mem, Space)` that hasn't opted in. A specialization
 * supplies four functions:
 *
 * @code
 * struct my_guest_ram { ... };
 *
 * template <> struct structo::virtio::virtq_memory_traits<my_guest_ram, structo::guest_phys_space> {
 *   using addr = structo::phys_addr<void, structo::guest_phys_space>;
 *
 *   // Copy [a, a + dst.size()) out of the memory into dst. Fails (typically
 *   // error::out_of_range) if any byte is unmapped.
 *   static reloco::result<void> try_read(my_guest_ram &, addr a, reloco::span<std::byte> dst) noexcept;
 *
 *   // Copy src into [a, a + src.size()). Same failure behaviour.
 *   static reloco::result<void> try_write(my_guest_ram &, addr a, reloco::span<const std::byte> src) noexcept;
 *
 *   // One naturally-aligned, single-copy-atomic 16-bit access with no
 *   // ordering of its own (ring indices and flags). Fails on a
 *   // misaligned or unmapped address.
 *   static reloco::result<std::uint16_t> try_load16(my_guest_ram &, addr a) noexcept;
 *   static reloco::result<void> try_store16(my_guest_ram &, addr a, std::uint16_t v) noexcept;
 * };
 * @endcode
 *
 * Memory ordering between accesses is *not* the traits' job: ring code
 * issues explicit barriers around the loads/stores above.
 *
 * ## Security contract (hostile peer)
 *
 * The peer can rewrite this memory at any instant and chooses every address
 * and length, so the traits interface is shaped to make the classic bugs
 * structurally impossible:
 *
 * - **TOCTOU / double fetch.** There is deliberately no accessor that returns
 *   a pointer or reference into peer-writable memory. Data only ever leaves
 *   as a by-value copy (`try_read`, `try_load16`, `try_read_object`), and
 *   callers validate and use *that copy*, never re-reading the memory for the
 *   same decision. Each ring index/flag is fetched exactly once into a local.
 *   (Same rule as `compat_sg.hpp`'s "always copy before reading" and
 *   `RELOCO_IPC_LOAD_ACQUIRE_FAULT`.)
 * - **Speculation.** Every peer-controlled bound check must mask the value it
 *   guards with `reloco::nospec::sanitize` before branching, so a
 *   mispredicted check cannot speculatively access out-of-range memory
 *   (`direct_virtq_memory` does this; custom backends must too).
 *   `virtq_layout.hpp`'s `try_checked_index` does the same for descriptor
 *   and ring indices.
 */

#include "virtq_types.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <reloco/error.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/span.hpp>
#include <reloco/speculation_defense.hpp>
#include <structo/phys_addr.hpp>
#include <type_traits>

namespace structo::virtio {

/** @brief Customization point; see the file comment. Undefined unless specialized. */
template <typename Mem, typename Space> struct virtq_memory_traits;

/**
 * @brief Reads a trivially-copyable @p T out of @p mem at @p addr (a snapshot:
 * the returned value is a private copy the peer can no longer change).
 */
template <typename T, typename Mem, typename Space>
[[nodiscard]] reloco::result<T> try_read_object(Mem &mem, phys_addr<void, Space> addr) noexcept {
  static_assert(std::is_trivially_copyable_v<T>, "try_read_object requires a trivially copyable type");
  T value{}; // zeroed so the snapshot is defined even if the backend fills it only partially
  auto r = virtq_memory_traits<Mem, Space>::try_read(
      mem, addr, reloco::span<std::byte>(reinterpret_cast<std::byte *>(&value), sizeof(T)));
  if (!r)
    return reloco::unexpected(r.error());
  return value;
}

/** @brief Writes a trivially-copyable @p value into @p mem at @p addr. */
template <typename T, typename Mem, typename Space>
[[nodiscard]] reloco::result<void> try_write_object(Mem &mem, phys_addr<void, Space> addr, const T &value) noexcept {
  static_assert(std::is_trivially_copyable_v<T>, "try_write_object requires a trivially copyable type");
  return virtq_memory_traits<Mem, Space>::try_write(
      mem, addr, reloco::span<const std::byte>(reinterpret_cast<const std::byte *>(&value), sizeof(T)));
}

/* The bounds checks in `at()` are what make the raw pointer arithmetic and
 * memcpy calls below safe. */
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

/**
 * @brief A `virtq_memory_traits` backend over one flat, caller-owned,
 * host-mapped byte window that represents the address range
 * `[base, base + size)` of address space @p Space.
 *
 * Bounds-checked (`error::out_of_range`) and alignment-checked
 * (`error::invalid_argument` for a misaligned 16-bit access); suitable for a
 * driver's own coherent DMA allocation, identity/offset-mapped guest RAM, and
 * unit-test fakes. The 16-bit accessors go through `volatile` so the compiler
 * cannot elide, merge, or reorder them against the peer.
 */
template <typename Space> class direct_virtq_memory {
public:
  using addr_type = phys_addr<void, Space>;

  constexpr direct_virtq_memory() noexcept = default;

  /**
   * @param host Host pointer to the first byte of the window.
   * @param size Window size in bytes.
   * @param base Address (in @p Space) that @p host corresponds to.
   */
  constexpr direct_virtq_memory(std::byte *host, std::size_t size, addr_type base) noexcept
      : host_(host), size_(size), base_(base) {}

  [[nodiscard]] constexpr bool is_bound() const noexcept { return host_ != nullptr; }
  [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
  [[nodiscard]] constexpr addr_type base() const noexcept { return base_; }

private:
  /**
   * @brief Resolves `[addr, addr + len)` to a host pointer, or fails.
   *
   * @p addr and @p len are guest-controlled. The bounds decision is folded
   * into one branch-free boolean and the offset is masked with
   * `nospec::sanitize` *before* the branch, so a mispredicted check cannot
   * speculatively form a pointer outside the window.
   */
  [[nodiscard]] reloco::result<std::byte *> at(addr_type addr, std::size_t len) const noexcept {
    if (!host_)
      return reloco::unexpected(reloco::error::not_initialized);
    const std::uint64_t offset = addr.value - base_.value;
    const bool in_range = (addr.value >= base_.value) & (offset <= size_) & (size_ - offset >= len);
    const std::uint64_t safe_offset = reloco::nospec::sanitize(offset, in_range, std::uint64_t{0});
    if (!in_range)
      return reloco::unexpected(reloco::error::out_of_range);
    return host_ + static_cast<std::size_t>(safe_offset);
  }

  std::byte *host_ = nullptr;
  std::size_t size_ = 0;
  addr_type base_{};

  friend struct virtq_memory_traits<direct_virtq_memory, Space>;
};

template <typename Space> struct virtq_memory_traits<direct_virtq_memory<Space>, Space> {
  using mem_type = direct_virtq_memory<Space>;
  using addr_type = phys_addr<void, Space>;

  [[nodiscard]] static reloco::result<void> try_read(mem_type &m, addr_type a, reloco::span<std::byte> dst) noexcept {
    auto p = m.at(a, dst.size());
    if (!p)
      return reloco::unexpected(p.error());
    if (!dst.empty())
      std::memcpy(dst.data(), *p, dst.size());
    return {};
  }

  [[nodiscard]] static reloco::result<void> try_write(mem_type &m, addr_type a,
                                                      reloco::span<const std::byte> src) noexcept {
    auto p = m.at(a, src.size());
    if (!p)
      return reloco::unexpected(p.error());
    if (!src.empty())
      std::memcpy(*p, src.data(), src.size());
    return {};
  }

  [[nodiscard]] static reloco::result<std::uint16_t> try_load16(mem_type &m, addr_type a) noexcept {
    auto p = m.at(a, sizeof(std::uint16_t));
    if (!p)
      return reloco::unexpected(p.error());
    if (!is_aligned16(*p))
      return reloco::unexpected(reloco::error::invalid_argument);
    return *reinterpret_cast<volatile std::uint16_t *>(*p);
  }

  [[nodiscard]] static reloco::result<void> try_store16(mem_type &m, addr_type a, std::uint16_t v) noexcept {
    auto p = m.at(a, sizeof(std::uint16_t));
    if (!p)
      return reloco::unexpected(p.error());
    if (!is_aligned16(*p))
      return reloco::unexpected(reloco::error::invalid_argument);
    *reinterpret_cast<volatile std::uint16_t *>(*p) = v;
    return {};
  }

private:
  [[nodiscard]] static bool is_aligned16(const std::byte *p) noexcept {
    return (reinterpret_cast<std::uintptr_t>(p) % alignof(std::uint16_t)) == 0;
  }
};

RELOCO_END_UNSAFE_BUFFER_USAGE

} // namespace structo::virtio
