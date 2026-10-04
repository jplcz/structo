// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file block_device_ref.hpp
 * @brief `structo::hw::block_device_ref`: a type-erased, non-owning
 * handle over a fixed-block-size random-access storage device -- an
 * SD/eMMC/NVMe/virtio-blk device, a RAM disk, or a bootloader's own
 * in-memory "disk image" fake -- plus the `block_device_traits<Backend>`
 * customization point a concrete backend specializes to be bindable
 * through it.
 *
 * This header is deliberately abstraction-only, the block-storage
 * counterpart of `otp_storage.hpp`: there is no SDHCI/NVMe-queue/
 * virtio-blk-virtqueue register poking here, nothing vendor- or
 * bus-specific. A concrete backend (an SDHCI driver, an NVMe
 * submission-queue adapter, a virtio-blk front-end, or a unit test's
 * in-memory fake) implements `block_device_traits<Backend>`.
 *
 * The intended caller is exactly what the name suggests: a bootloader
 * (or any other early, allocation-free code) that has already located
 * which logical blocks on disk hold the file it wants -- typically by
 * walking a filesystem's own metadata (a FAT/ext4/whatever directory
 * entry and extent list) -- and now just needs to read those blocks
 * into a destination buffer it already owns, without caring whether the
 * underlying media is an SD card, a virtio-blk device, or a RAM disk.
 * This header has no notion of files, paths, or filesystems itself;
 * that is layered on top, outside this header's scope.
 *
 * ## Customization point: `block_device_traits<Backend>`
 *
 * `block_device_traits<Backend>` is left undefined for any `Backend`
 * that hasn't opted in (mirroring `otp_storage_traits`/`uart_traits`). A
 * specialization must supply:
 *
 * @code
 * template <> struct structo::hw::block_device_traits<my_backend> {
 *   static std::size_t block_size(my_backend &) noexcept;
 *   static std::uint64_t block_count(my_backend &) noexcept;
 *   static reloco::result<void> try_read_blocks(my_backend &, std::uint64_t lba,
 *                                               reloco::span<std::byte> dst) noexcept;
 * };
 * @endcode
 *
 * `block_size` reports the device's fixed block ("sector") size in
 * bytes (e.g. `512`, `4096`); `block_count` reports the device's total
 * addressable size in blocks. `try_read_blocks` reads
 * `dst.size() / block_size()` *contiguous* blocks starting at logical
 * block address `lba` into `dst`; `dst.size()` is always an exact
 * multiple of `block_size()` and `[lba, lba + dst.size() /
 * block_size())` is always within `[0, block_count())` by the time the
 * backend is called -- `block_device_ref` itself rejects a misaligned
 * `dst.size()` with `error::invalid_argument` and an out-of-range `lba`
 * with `error::out_of_range` before the backend is ever reached.
 *
 * Optionally, a backend may also support writing:
 *
 * @code
 * static reloco::result<void> try_write_blocks(my_backend &, std::uint64_t lba,
 *                                              reloco::span<const std::byte> src) noexcept;
 * static reloco::result<void> try_flush(my_backend &) noexcept;
 * static bool is_read_only(my_backend &) noexcept;
 * @endcode
 *
 * and report whether it is actually accessible right now (distinct from
 * a general compiled-in/architecture check -- e.g. a card not yet
 * inserted, a controller still negotiating link width):
 *
 * @code
 * static bool is_available(my_backend &) noexcept;
 * @endcode
 *
 * All four are detected via SFINAE (the same optional-member idiom
 * `otp_storage_traits::is_locked`/`uart_traits::current_config` use).
 * If `try_write_blocks` is absent entirely, the device has no write
 * capability whatsoever: `block_device_ref::is_read_only()` always
 * reports `true` regardless of the optional `is_read_only` probe below,
 * and `try_write_blocks()` always fails with
 * `error::unsupported_operation`. If `try_write_blocks` *is* present but
 * the optional `is_read_only` probe is absent, the device is assumed
 * writable whenever bound (`is_read_only()` reports `false`) -- the
 * probe exists purely for devices that can flip read-only at runtime
 * independently of write support existing at all (e.g. an SD card's
 * physical write-protect tab, a virtio-blk device negotiated
 * read-only). If `try_flush` is absent, `try_flush()` always succeeds
 * trivially -- the backend has no write-back cache to flush. If
 * `is_available` is absent, `is_available()` reports `true` whenever
 * bound.
 */

#include <reloco/detail/assert.hpp>
#include <reloco/detail/compat.hpp>
#include <reloco/error.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/span.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>

namespace structo {

using namespace reloco;

namespace hw {

// ============================================================================
// Customization Point
// ============================================================================

/**
 * @brief Opt-in customization point describing how to query, read, and
 * (optionally) write/flush a concrete fixed-block-size storage backend,
 * through @ref block_device_ref.
 *
 * Intentionally left undefined for any `Backend` that hasn't been
 * adapted, mirroring `otp_storage_traits`/`uart_traits`. See the
 * @file-level docs above for the complete required/optional member
 * list.
 */
template <typename Backend> struct block_device_traits;

namespace detail {

template <typename Backend, typename = void> struct has_block_device_traits : std::false_type {};

template <typename Backend>
struct has_block_device_traits<Backend, std::void_t<decltype(block_device_traits<Backend>::block_size),
                                                    decltype(block_device_traits<Backend>::block_count),
                                                    decltype(block_device_traits<Backend>::try_read_blocks)>>
    : std::true_type {};

// Detects the optional Traits::try_write_blocks probe.
template <typename Traits, typename = void> struct block_device_has_write : std::false_type {};
template <typename Traits>
struct block_device_has_write<Traits, std::void_t<decltype(Traits::try_write_blocks)>> : std::true_type {};

// Detects the optional Traits::try_flush probe.
template <typename Traits, typename = void> struct block_device_has_flush : std::false_type {};
template <typename Traits>
struct block_device_has_flush<Traits, std::void_t<decltype(Traits::try_flush)>> : std::true_type {};

// Detects the optional Traits::is_read_only probe.
template <typename Traits, typename = void> struct block_device_has_is_read_only : std::false_type {};
template <typename Traits>
struct block_device_has_is_read_only<Traits, std::void_t<decltype(Traits::is_read_only)>> : std::true_type {};

// Detects the optional Traits::is_available probe.
template <typename Traits, typename = void> struct block_device_has_is_available : std::false_type {};
template <typename Traits>
struct block_device_has_is_available<Traits, std::void_t<decltype(Traits::is_available)>> : std::true_type {};

} // namespace detail

// ============================================================================
// Type-Erased Block Device Handle
// ============================================================================

/**
 * @brief Type-erased, non-owning handle over a fixed-block-size
 * random-access storage device, for whatever concrete backend it is
 * bound to.
 *
 * Default-constructed (or copied from a default-constructed) refs are
 * *unbound*: every operation on one fails with
 * `error::unsupported_operation` rather than trapping, mirroring
 * `otp_storage_ref`/`hw_rng_ref`/`io_space_ref`/`uart_ref`'s null-safety
 * convention.
 *
 * Follows the single-`vtable`, resolved-once-per-`Backend` shape every
 * `structo` `*_ref` handle uses -- see
 * [`type-erased-base-containers.md`](https://github.com/jplcz/reloco/blob/master/docs/type-erased-base-containers.md)
 * and `docs/coding-guide.md`'s "Type-erase a `*_ref` handle's backend
 * behind one `vtable`" section.
 */
class RELOCO_POINTER block_device_ref {
public:
  /** @brief Fixed, per-bound-backend-type dispatch table. */
  struct vtable {
    std::size_t (*block_size)(void *ctx) noexcept;
    std::uint64_t (*block_count)(void *ctx) noexcept;
    result<void> (*read_blocks)(void *ctx, std::uint64_t lba, span<std::byte> dst) noexcept;
    result<void> (*write_blocks)(void *ctx, std::uint64_t lba, span<const std::byte> src) noexcept;
    result<void> (*flush)(void *ctx) noexcept;
    bool (*is_read_only)(void *ctx) noexcept;
    bool (*is_available)(void *ctx) noexcept;
  };

  /** @brief Constructs an unbound ref. */
  constexpr block_device_ref() noexcept = default;

  /**
   * @brief Binds this ref to an existing, adapted backend.
   * @tparam Backend Concrete backend type, deduced. Must have a
   * @ref block_device_traits specialization.
   * @param b Backend to bind. Must outlive this handle and every copy of
   * it. Marked `explicit`: binding a backend is always a deliberate step,
   * never an implicit conversion.
   */
  template <typename Backend, std::enable_if_t<detail::has_block_device_traits<Backend>::value, int> = 0>
  constexpr explicit block_device_ref(Backend &b RELOCO_LIFETIMEBOUND RELOCO_LIFETIME_CAPTURE_BY_THIS) noexcept
      : ctx_(std::addressof(b)), vtbl_(&s_vtbl<Backend>) {}

  /** @brief Rejects rvalue/temporary backend bindings. */
  template <typename Backend, std::enable_if_t<!std::is_lvalue_reference_v<Backend>, int> = 0>
  block_device_ref(Backend &&) = delete;

  /** @brief Whether this ref is bound to a backend. */
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return vtbl_ != nullptr; }

  /** @brief Fixed block ("sector") size, in bytes. `0` if unbound. */
  [[nodiscard]] std::size_t block_size() const noexcept { return vtbl_ ? vtbl_->block_size(ctx_) : 0; }

  /** @brief Total addressable size of the bound device, in blocks. `0` if unbound. */
  [[nodiscard]] std::uint64_t block_count() const noexcept { return vtbl_ ? vtbl_->block_count(ctx_) : 0; }

  /**
   * @brief Total addressable size of the bound device, in bytes
   * (`block_count() * block_size()`). `0` if unbound. May overflow for a
   * pathologically large device; use `block_count()`/`block_size()`
   * directly if that matters.
   */
  [[nodiscard]] std::uint64_t size_bytes() const noexcept {
    return static_cast<std::uint64_t>(block_count()) * static_cast<std::uint64_t>(block_size());
  }

  /**
   * @brief Whether the bound backend's storage is actually accessible
   * right now. `false` if unbound; if the bound backend does not
   * implement the optional `block_device_traits::is_available`, assumed
   * `true` whenever bound.
   */
  [[nodiscard]] bool is_available() const noexcept {
    if (!vtbl_)
      return false;
    return vtbl_->is_available(ctx_);
  }

  /**
   * @brief Whether this device cannot be written to right now. `true`
   * if unbound, or if the backend has no write support at all (no
   * `block_device_traits::try_write_blocks`). If the backend does
   * support writing but does not implement the optional
   * `is_read_only` probe, assumed `false` (writable) whenever bound.
   */
  [[nodiscard]] bool is_read_only() const noexcept {
    if (!vtbl_)
      return true;
    return vtbl_->is_read_only(ctx_);
  }

  // --------------------------------------------------------------------
  // Mandatory backend operations (directly forwarded, with bounds
  // checking against block_count()/block_size() performed here rather
  // than in every backend).
  // --------------------------------------------------------------------

  /**
   * @brief Reads `dst.size() / block_size()` contiguous blocks starting
   * at logical block address `lba` into `dst`.
   * Fails with `error::unsupported_operation` if this ref is unbound,
   * `error::invalid_argument` if `dst.size()` is not an exact multiple
   * of `block_size()`, `error::out_of_range` if `[lba, lba +
   * dst.size() / block_size())` falls outside `[0, block_count())`, or
   * whatever other error the backend itself reports.
   */
  [[nodiscard]] result<void> try_read_blocks(std::uint64_t lba, span<std::byte> dst) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    const std::size_t bs = block_size();
    if (bs == 0 || dst.size() % bs != 0)
      return unexpected(error::invalid_argument);
    const std::uint64_t nblocks = dst.size() / bs;
    if (lba > block_count() || nblocks > block_count() - lba)
      return unexpected(error::out_of_range);
    return vtbl_->read_blocks(ctx_, lba, dst);
  }

  /**
   * @brief Writes `src.size() / block_size()` contiguous blocks
   * starting at logical block address `lba` from `src`.
   * Fails with `error::unsupported_operation` if this ref is unbound or
   * the bound backend has no write support at all,
   * `error::permission_denied` if `is_read_only()`,
   * `error::invalid_argument` if `src.size()` is not an exact multiple
   * of `block_size()`, `error::out_of_range` if `[lba, lba +
   * src.size() / block_size())` falls outside `[0, block_count())`, or
   * whatever other error the backend itself reports.
   */
  [[nodiscard]] result<void> try_write_blocks(std::uint64_t lba, span<const std::byte> src) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    const std::size_t bs = block_size();
    if (bs == 0 || src.size() % bs != 0)
      return unexpected(error::invalid_argument);
    const std::uint64_t nblocks = src.size() / bs;
    if (lba > block_count() || nblocks > block_count() - lba)
      return unexpected(error::out_of_range);
    return vtbl_->write_blocks(ctx_, lba, src);
  }

  /**
   * @brief Flushes any pending writes to the underlying media.
   * Fails with `error::unsupported_operation` if this ref is unbound;
   * if the bound backend does not implement the optional
   * `block_device_traits::try_flush`, succeeds trivially (no write-back
   * cache to flush).
   */
  [[nodiscard]] result<void> try_flush() const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->flush(ctx_);
  }

  // --------------------------------------------------------------------
  // Generic convenience, synthesized purely from try_read_blocks above
  // -- no further backend support is required.
  // --------------------------------------------------------------------

  /**
   * @brief Reads `dst.size()` bytes starting at arbitrary byte offset
   * `byte_offset` -- unlike `try_read_blocks`, neither `byte_offset` nor
   * `dst.size()` need be block-aligned, the typical case when loading a
   * file's raw content once its owning blocks have been located by a
   * filesystem layer above this header. Whole blocks that fall entirely
   * within `[byte_offset, byte_offset + dst.size())` are read directly
   * into `dst`; a partial leading or trailing block is instead read
   * into `scratch` and the relevant slice copied out, so no block is
   * ever over-read into `dst` itself.
   *
   * Fails with whatever `try_read_blocks` itself would fail with (in
   * particular `error::out_of_range` if the requested byte range falls
   * outside `[0, size_bytes())`), or `error::invalid_argument` if
   * `scratch` is smaller than `block_size()`.
   */
  [[nodiscard]] result<void> try_read_bytes(std::uint64_t byte_offset, span<std::byte> dst,
                                            span<std::byte> scratch) const noexcept {
    const std::size_t bs = block_size();
    if (bs == 0)
      return unexpected(error::unsupported_operation);
    if (scratch.size() < bs)
      return unexpected(error::invalid_argument);

    std::size_t done = 0;
    while (done < dst.size()) {
      const std::uint64_t offset = byte_offset + done;
      const std::uint64_t lba = offset / bs;
      const std::size_t in_block_off = static_cast<std::size_t>(offset % bs);
      const std::size_t remaining = dst.size() - done;

      if (in_block_off == 0 && remaining >= bs) {
        const std::size_t whole_blocks_bytes = remaining - remaining % bs;
        auto read = try_read_blocks(lba, span<std::byte>(dst.data() + done, whole_blocks_bytes));
        if (!read)
          return read;
        done += whole_blocks_bytes;
      } else {
        auto read = try_read_blocks(lba, span<std::byte>(scratch.data(), bs));
        if (!read)
          return read;
        const std::size_t take = std::min(bs - in_block_off, remaining);
        std::memcpy(dst.data() + done, scratch.data() + in_block_off, take);
        done += take;
      }
    }
    return {};
  }

private:
  template <typename Backend> static std::size_t block_size_entry(void *ctx) noexcept {
    return block_device_traits<Backend>::block_size(*static_cast<Backend *>(ctx));
  }

  template <typename Backend> static std::uint64_t block_count_entry(void *ctx) noexcept {
    return block_device_traits<Backend>::block_count(*static_cast<Backend *>(ctx));
  }

  template <typename Backend>
  static result<void> read_blocks_entry(void *ctx, std::uint64_t lba, span<std::byte> dst) noexcept {
    return block_device_traits<Backend>::try_read_blocks(*static_cast<Backend *>(ctx), lba, dst);
  }

  template <typename Backend>
  static result<void> write_blocks_entry(void *ctx, std::uint64_t lba, span<const std::byte> src) noexcept {
    using traits = block_device_traits<Backend>;
    if constexpr (!detail::block_device_has_write<traits>::value) {
      (void)ctx;
      (void)lba;
      (void)src;
      return unexpected(error::unsupported_operation);
    } else {
      auto &backend = *static_cast<Backend *>(ctx);
      if constexpr (detail::block_device_has_is_read_only<traits>::value) {
        if (traits::is_read_only(backend))
          return unexpected(error::permission_denied);
      }
      return traits::try_write_blocks(backend, lba, src);
    }
  }

  template <typename Backend> static result<void> flush_entry(void *ctx) noexcept {
    using traits = block_device_traits<Backend>;
    if constexpr (detail::block_device_has_flush<traits>::value) {
      return traits::try_flush(*static_cast<Backend *>(ctx));
    } else {
      (void)ctx;
      return {};
    }
  }

  template <typename Backend> static bool is_read_only_entry(void *ctx) noexcept {
    using traits = block_device_traits<Backend>;
    if constexpr (!detail::block_device_has_write<traits>::value) {
      (void)ctx;
      return true;
    } else if constexpr (detail::block_device_has_is_read_only<traits>::value) {
      return traits::is_read_only(*static_cast<Backend *>(ctx));
    } else {
      (void)ctx;
      return false;
    }
  }

  template <typename Backend> static bool is_available_entry(void *ctx) noexcept {
    using traits = block_device_traits<Backend>;
    if constexpr (detail::block_device_has_is_available<traits>::value) {
      return traits::is_available(*static_cast<Backend *>(ctx));
    } else {
      (void)ctx;
      return true;
    }
  }

  template <typename Backend>
  static constexpr vtable s_vtbl{&block_size_entry<Backend>,  &block_count_entry<Backend>,
                                 &read_blocks_entry<Backend>, &write_blocks_entry<Backend>,
                                 &flush_entry<Backend>,       &is_read_only_entry<Backend>,
                                 &is_available_entry<Backend>};

  void *ctx_ = nullptr;
  const vtable *vtbl_ = nullptr;
};

} // namespace hw
} // namespace structo
