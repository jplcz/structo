// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file ram_disk.hpp
 * @brief `structo::hw::ram_disk`/`structo::hw::read_only_ram_disk`:
 * minimal `block_device_traits`-conforming backends over an in-memory
 * buffer, bindable through `block_device_ref` -- commonly a
 * bootloader's already-loaded initrd image (see `fdt_initrd.hpp`), a
 * build-time embedded filesystem blob, or a unit test's fake disk:
 * something already present in memory with no real storage controller
 * behind it at all.
 *
 * Neither class owns the memory it wraps: both are thin,
 * `reloco::span`-based views, exactly as non-owning as every other
 * `structo` `*_ref` backend adapter is expected to be -- the caller is
 * responsible for keeping the backing buffer alive for as long as the
 * `ram_disk`/`read_only_ram_disk` (and any `block_device_ref` bound to
 * it) is used.
 *
 * `ram_disk` wraps a mutable `span<std::byte>` and supports both
 * reading and writing. `read_only_ram_disk` wraps an immutable
 * `span<const std::byte>` and has no write support at all -- mirroring
 * `otp_storage.hpp`'s "the optional trait is simply absent" convention
 * for a backend that can never support an operation, rather than a
 * runtime flag -- the natural choice for e.g. a kernel-supplied initrd
 * image a bootloader only ever needs to read from, never write back to.
 *
 * `block_size` is a caller-chosen constructor parameter, not a fixed
 * property of the memory itself -- there's no real storage controller
 * here imposing a sector size. Passing `1` turns the device into a
 * plain byte-addressable view when alignment doesn't matter (e.g.
 * handing a whole in-memory blob straight to
 * `block_device_ref::try_read_blocks`/`try_read_bytes` in one call).
 * If `storage.size()` isn't an exact multiple of `block_size`, the
 * short trailing partial block is simply excluded from `block_count()`
 * (floor-rounded), never rejected.
 */

#include "block_device_ref.hpp"

#include <reloco/detail/assert.hpp>

#include <cstring>

namespace structo {

using namespace reloco;

namespace hw {

/** @brief Read/write `block_device_traits`-conforming backend over a
 * caller-owned, mutable in-memory buffer. See the @file docs above. */
class ram_disk {
public:
  constexpr ram_disk() noexcept = default;

  /**
   * @brief Wraps `storage` as a read/write block device with the given
   * `block_size` (default `512`).
   * @param storage Backing buffer. Must outlive this `ram_disk` and
   * every `block_device_ref` bound to it.
   * @param block_size Must be non-zero (asserted).
   */
  constexpr explicit ram_disk(span<std::byte> storage RELOCO_LIFETIMEBOUND, std::size_t block_size = 512) noexcept
      : storage_(storage), block_size_(block_size) {
    RELOCO_ASSERT(block_size_ != 0, "ram_disk: block_size must be non-zero");
  }

  /** @brief The block size this device was constructed with. */
  [[nodiscard]] std::size_t block_size() const noexcept { return block_size_; }

  /** @brief `storage().size() / block_size()`, floor-rounded. */
  [[nodiscard]] std::uint64_t block_count() const noexcept {
    return block_size_ == 0 ? 0 : static_cast<std::uint64_t>(storage_.size() / block_size_);
  }

  /** @brief The backing buffer this device was constructed with. */
  [[nodiscard]] span<std::byte> storage() const noexcept RELOCO_LIFETIMEBOUND { return storage_; }

private:
  span<std::byte> storage_;
  std::size_t block_size_ = 0;
};

template <> struct block_device_traits<ram_disk> {
  static std::size_t block_size(ram_disk &b) noexcept { return b.block_size(); }
  static std::uint64_t block_count(ram_disk &b) noexcept { return b.block_count(); }

  static result<void> try_read_blocks(ram_disk &b, std::uint64_t lba, span<std::byte> dst) noexcept {
    const span<std::byte> storage = b.storage();
    std::memcpy(dst.data(), storage.data() + lba * b.block_size(), dst.size());
    return {};
  }

  static result<void> try_write_blocks(ram_disk &b, std::uint64_t lba, span<const std::byte> src) noexcept {
    const span<std::byte> storage = b.storage();
    std::memcpy(storage.data() + lba * b.block_size(), src.data(), src.size());
    return {};
  }
};

/** @brief Read-only `block_device_traits`-conforming backend over a
 * caller-owned, immutable in-memory buffer -- has no write support at
 * all. See the @file docs above. */
class read_only_ram_disk {
public:
  constexpr read_only_ram_disk() noexcept = default;

  /**
   * @brief Wraps `storage` as a read-only block device with the given
   * `block_size` (default `512`).
   * @param storage Backing buffer. Must outlive this `read_only_ram_disk`
   * and every `block_device_ref` bound to it.
   * @param block_size Must be non-zero (asserted).
   */
  constexpr explicit read_only_ram_disk(span<const std::byte> storage RELOCO_LIFETIMEBOUND,
                                        std::size_t block_size = 512) noexcept
      : storage_(storage), block_size_(block_size) {
    RELOCO_ASSERT(block_size_ != 0, "read_only_ram_disk: block_size must be non-zero");
  }

  /** @brief The block size this device was constructed with. */
  [[nodiscard]] std::size_t block_size() const noexcept { return block_size_; }

  /** @brief `storage().size() / block_size()`, floor-rounded. */
  [[nodiscard]] std::uint64_t block_count() const noexcept {
    return block_size_ == 0 ? 0 : static_cast<std::uint64_t>(storage_.size() / block_size_);
  }

  /** @brief The backing buffer this device was constructed with. */
  [[nodiscard]] span<const std::byte> storage() const noexcept RELOCO_LIFETIMEBOUND { return storage_; }

private:
  span<const std::byte> storage_;
  std::size_t block_size_ = 0;
};

template <> struct block_device_traits<read_only_ram_disk> {
  static std::size_t block_size(read_only_ram_disk &b) noexcept { return b.block_size(); }
  static std::uint64_t block_count(read_only_ram_disk &b) noexcept { return b.block_count(); }

  static result<void> try_read_blocks(read_only_ram_disk &b, std::uint64_t lba, span<std::byte> dst) noexcept {
    const span<const std::byte> storage = b.storage();
    std::memcpy(dst.data(), storage.data() + lba * b.block_size(), dst.size());
    return {};
  }

  // No `try_write_blocks`: `block_device_ref::is_read_only()` always
  // reports `true` for this backend, and `try_write_blocks()` always
  // fails with `error::unsupported_operation` -- see block_device_ref.hpp.
};

} // namespace hw
} // namespace structo
