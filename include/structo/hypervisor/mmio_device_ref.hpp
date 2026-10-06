// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file mmio_device_ref.hpp
 * @brief `structo::hypervisor::mmio_device_ref`: a type-erased, non-
 * owning handle over one *emulated* MMIO device -- the hypervisor side
 * of a guest MMIO access, not the host-side register access
 * `structo::io_space_ref`/`structo::mmio_space_backend` provide.
 *
 * ## The other direction from `io_space_ref`
 *
 * `io_space_ref` (see `io_space_ref.hpp`/`mmio_space.hpp`) is for code
 * that itself *is* a guest/kernel, touching a real (or passed-through)
 * hardware register window through ordinary `volatile` loads/stores.
 * `mmio_device_ref` is for the opposite role: the hypervisor *servicing*
 * a guest's trapped access to a memory range it does not actually back
 * with real hardware at all -- a virtio-mmio device, an emulated
 * PL011/16550 UART, a software-only doorbell/status register bank, or
 * any other address range the guest believes is a device but which only
 * exists as hypervisor-maintained state.
 *
 * The expected caller is a VM-exit MMIO handler (see
 * `hypervisor/vm_exit_dispatcher.hpp`): once the architecture's exit
 * qualification has been decoded into a faulting guest-physical address
 * and access width, and some address-space map (out of this header's
 * scope -- a sorted range table, an interval tree, ...) has resolved
 * that address down to "device D, offset O within D's own window", the
 * handler calls `try_read`/`try_write` on the `mmio_device_ref` for
 * device D with that offset. This header is deliberately silent about
 * how that resolution happens, exactly as `vm_exit_dispatcher` is
 * silent about how a raw exit reason maps to a handler function --
 * both only cover the last mile once the caller already knows which
 * concrete thing it is calling into.
 *
 * ## Customization point: `mmio_device_traits<Backend>`
 *
 * Left undefined for any `Backend` that hasn't opted in (mirroring
 * `block_device_traits`/`otp_storage_traits`). A specialization must
 * supply:
 *
 * @code
 * template <> struct structo::hypervisor::mmio_device_traits<my_device> {
 *   static std::size_t size(my_device &) noexcept;
 *   static reloco::result<void> try_read(my_device &, std::uint64_t offset,
 *                                        reloco::span<std::byte> dst) noexcept;
 * };
 * @endcode
 *
 * `size` reports the device's own MMIO window size in bytes (its BAR
 * size, its virtio-mmio region size, ...); `try_read` fills `dst` with
 * `dst.size()` raw bytes read starting at byte offset `offset` within
 * that window, in whatever order the backend's own register layout
 * naturally produces them (no endianness conversion happens here -- the
 * caller already decoded the guest access down to a plain byte count,
 * exactly like `mmio_space_backend`'s `volatile` dereferences do no
 * byte-swapping of their own). `offset`/`dst.size()` are only checked
 * against `size()` by `mmio_device_ref` itself (`error::out_of_range`);
 * no particular access width is required or enforced here -- real
 * accesses are overwhelmingly 1/2/4/8 bytes, but a backend that only
 * supports some subset of widths is free to reject the rest with
 * `error::invalid_argument` from inside `try_read` itself.
 *
 * Optionally, a backend may also support writes:
 *
 * @code
 * static reloco::result<void> try_write(my_device &, std::uint64_t offset,
 *                                       reloco::span<const std::byte> src) noexcept;
 * static bool is_read_only(my_device &) noexcept;
 * static bool is_available(my_device &) noexcept;
 * static reloco::result<void> try_reset(my_device &) noexcept;
 * @endcode
 *
 * All four are detected via SFINAE (the same optional-member idiom
 * `block_device_traits`'s `try_write_blocks`/`is_read_only`/
 * `is_available` probes use). If `try_write` is absent entirely, the
 * device is hard-wired read-only: `mmio_device_ref::is_read_only()`
 * always reports `true` and `try_write()` always fails with
 * `error::unsupported_operation`. If `try_write` *is* present but the
 * optional `is_read_only` probe is absent, the device is assumed
 * writable whenever bound; the probe exists for devices that can flip
 * read-only at runtime independently of write support existing at all
 * (e.g. a guest-visible "locked" flash region). If `is_available` is
 * absent, `is_available()` reports `true` whenever bound -- the probe
 * exists for devices that can be transiently unavailable (e.g. a
 * hot-unplugged virtio-mmio device whose address range is still mapped
 * but no longer backed). If `try_reset` is absent, `try_reset()` always
 * succeeds trivially -- the device has no internal state a VM reset
 * needs to clear.
 *
 * See `hypervisor/mmio_print_device.hpp` for a complete, ready-to-use backend (a one-byte "print port"
 * forwarding every guest write to a `console_ref` sink) if a worked example that actually compiles and is
 * directly reusable is more useful than the illustrative one immediately below.
 *
 * ## Worked example: a tiny doorbell/status register device
 *
 * @code
 * // Two 32-bit registers at offsets 0 and 4: writing any value to the doorbell at offset 0 increments a
 * // ring counter and the status register at offset 4 reports it. The status register is read-only.
 * struct doorbell_device {
 *   std::uint32_t rings = 0;
 * };
 *
 * template <> struct structo::hypervisor::mmio_device_traits<doorbell_device> {
 *   static std::size_t size(doorbell_device &) noexcept { return 8; }
 *
 *   static reloco::result<void> try_read(doorbell_device &d, std::uint64_t offset,
 *                                        reloco::span<std::byte> dst) noexcept {
 *     if (offset != 4 || dst.size() != 4)
 *       return reloco::unexpected(reloco::error::invalid_argument); // doorbell itself is write-only
 *     std::memcpy(dst.data(), &d.rings, 4);
 *     return {};
 *   }
 *
 *   static reloco::result<void> try_write(doorbell_device &d, std::uint64_t offset,
 *                                         reloco::span<const std::byte> src) noexcept {
 *     if (offset != 0 || src.size() != 4)
 *       return reloco::unexpected(reloco::error::invalid_argument); // status register is read-only
 *     ++d.rings; // the written value itself is ignored -- any write is a doorbell ring
 *     return {};
 *   }
 * };
 *
 * doorbell_device dev;
 * structo::hypervisor::mmio_device_ref ref(dev);
 *
 * // In the VM-exit MMIO handler, once (device, relative offset) has been resolved:
 * std::uint32_t ring_value = 0; // the actual bits are irrelevant to this device
 * (void)ref.try_write(0, reloco::span<const std::byte>(
 *                            reinterpret_cast<const std::byte *>(&ring_value), 4));
 * @endcode
 */

#include <reloco/detail/compat.hpp>
#include <reloco/error.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/span.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace structo::hypervisor {

using namespace reloco;

// ============================================================================
// Customization Point
// ============================================================================

/**
 * @brief Opt-in customization point describing how to query, read, and
 * (optionally) write/reset a concrete emulated MMIO device backend,
 * through @ref mmio_device_ref.
 *
 * Intentionally left undefined for any `Backend` that hasn't been
 * adapted, mirroring `block_device_traits`/`otp_storage_traits`. See
 * the @file-level docs above for the complete required/optional member
 * list and a worked example.
 */
template <typename Backend> struct mmio_device_traits;

namespace detail {

template <typename Backend, typename = void> struct has_mmio_device_traits : std::false_type {};

template <typename Backend>
struct has_mmio_device_traits<Backend, std::void_t<decltype(mmio_device_traits<Backend>::size),
                                                   decltype(mmio_device_traits<Backend>::try_read)>>
    : std::true_type {};

// Detects the optional Traits::try_write probe.
template <typename Traits, typename = void> struct mmio_device_has_write : std::false_type {};
template <typename Traits>
struct mmio_device_has_write<Traits, std::void_t<decltype(Traits::try_write)>> : std::true_type {};

// Detects the optional Traits::is_read_only probe.
template <typename Traits, typename = void> struct mmio_device_has_is_read_only : std::false_type {};
template <typename Traits>
struct mmio_device_has_is_read_only<Traits, std::void_t<decltype(Traits::is_read_only)>> : std::true_type {};

// Detects the optional Traits::is_available probe.
template <typename Traits, typename = void> struct mmio_device_has_is_available : std::false_type {};
template <typename Traits>
struct mmio_device_has_is_available<Traits, std::void_t<decltype(Traits::is_available)>> : std::true_type {};

// Detects the optional Traits::try_reset probe.
template <typename Traits, typename = void> struct mmio_device_has_reset : std::false_type {};
template <typename Traits>
struct mmio_device_has_reset<Traits, std::void_t<decltype(Traits::try_reset)>> : std::true_type {};

} // namespace detail

// ============================================================================
// Type-Erased Emulated MMIO Device Handle
// ============================================================================

/**
 * @brief Type-erased, non-owning handle over one emulated MMIO device,
 * for whatever concrete backend it is bound to.
 *
 * Default-constructed (or copied from a default-constructed) refs are
 * *unbound*: every operation on one fails with
 * `error::unsupported_operation` rather than trapping, mirroring
 * `block_device_ref`/`otp_storage_ref`/`io_space_ref`'s null-safety
 * convention.
 *
 * Follows the single-`vtable`, resolved-once-per-`Backend` shape every
 * `structo` `*_ref` handle uses -- see
 * [`type-erased-base-containers.md`](https://github.com/jplcz/reloco/blob/master/docs/type-erased-base-containers.md)
 * and `docs/coding-guide.md`'s "Type-erase a `*_ref` handle's backend
 * behind one `vtable`" section.
 */
class RELOCO_POINTER mmio_device_ref {
public:
  /** @brief Fixed, per-bound-backend-type dispatch table. */
  struct vtable {
    std::size_t (*size)(void *ctx) noexcept;
    result<void> (*read)(void *ctx, std::uint64_t offset, span<std::byte> dst) noexcept;
    result<void> (*write)(void *ctx, std::uint64_t offset, span<const std::byte> src) noexcept;
    bool (*is_read_only)(void *ctx) noexcept;
    bool (*is_available)(void *ctx) noexcept;
    result<void> (*reset)(void *ctx) noexcept;
  };

  /** @brief Constructs an unbound ref. */
  constexpr mmio_device_ref() noexcept = default;

  /**
   * @brief Binds this ref to an existing, adapted backend.
   * @tparam Backend Concrete backend type, deduced. Must have an
   * @ref mmio_device_traits specialization.
   * @param b Backend to bind. Must outlive this handle and every copy
   * of it. Marked `explicit`: binding a backend is always a deliberate
   * step, never an implicit conversion.
   */
  template <typename Backend, std::enable_if_t<detail::has_mmio_device_traits<Backend>::value, int> = 0>
  constexpr explicit mmio_device_ref(Backend &b RELOCO_LIFETIMEBOUND RELOCO_LIFETIME_CAPTURE_BY_THIS) noexcept
      : ctx_(std::addressof(b)), vtbl_(&s_vtbl<Backend>) {}

  /** @brief Rejects rvalue/temporary backend bindings. */
  template <typename Backend, std::enable_if_t<!std::is_lvalue_reference_v<Backend>, int> = 0>
  mmio_device_ref(Backend &&) = delete;

  /** @brief Whether this ref is bound to a backend. */
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return vtbl_ != nullptr; }

  /** @brief This device's own MMIO window size, in bytes. `0` if unbound. */
  [[nodiscard]] std::size_t size() const noexcept { return vtbl_ ? vtbl_->size(ctx_) : 0; }

  /**
   * @brief Whether the bound backend is actually accessible right now.
   * `false` if unbound; if the bound backend does not implement the
   * optional `mmio_device_traits::is_available`, assumed `true`
   * whenever bound.
   */
  [[nodiscard]] bool is_available() const noexcept { return vtbl_ && vtbl_->is_available(ctx_); }

  /**
   * @brief Whether this device cannot be written to right now. `true`
   * if unbound, or if the backend has no write support at all (no
   * `mmio_device_traits::try_write`). If the backend does support
   * writing but does not implement the optional `is_read_only` probe,
   * assumed `false` (writable) whenever bound.
   */
  [[nodiscard]] bool is_read_only() const noexcept { return !vtbl_ || vtbl_->is_read_only(ctx_); }

  /**
   * @brief Reads `dst.size()` raw bytes starting at byte offset
   * `offset` within this device's own window.
   * Fails with `error::unsupported_operation` if this ref is unbound,
   * `error::invalid_argument` if `dst` is empty, `error::out_of_range`
   * if `[offset, offset + dst.size())` falls outside `[0, size())`, or
   * whatever other error the backend itself reports.
   */
  [[nodiscard]] result<void> try_read(std::uint64_t offset, span<std::byte> dst) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    if (dst.empty())
      return unexpected(error::invalid_argument);
    const std::size_t window = size();
    if (offset > window || dst.size() > window - offset)
      return unexpected(error::out_of_range);
    return vtbl_->read(ctx_, offset, dst);
  }

  /**
   * @brief Writes `src.size()` raw bytes starting at byte offset
   * `offset` within this device's own window.
   * Fails with `error::unsupported_operation` if this ref is unbound or
   * the bound backend has no write support at all,
   * `error::permission_denied` if `is_read_only()`,
   * `error::invalid_argument` if `src` is empty, `error::out_of_range`
   * if `[offset, offset + src.size())` falls outside `[0, size())`, or
   * whatever other error the backend itself reports.
   */
  [[nodiscard]] result<void> try_write(std::uint64_t offset, span<const std::byte> src) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    if (src.empty())
      return unexpected(error::invalid_argument);
    const std::size_t window = size();
    if (offset > window || src.size() > window - offset)
      return unexpected(error::out_of_range);
    return vtbl_->write(ctx_, offset, src);
  }

  /**
   * @brief Resets this device to whatever state it starts VM boot in
   * (e.g. in response to a whole-VM/guest reset).
   * Fails with `error::unsupported_operation` if this ref is unbound;
   * if the bound backend does not implement the optional
   * `mmio_device_traits::try_reset`, succeeds trivially (no internal
   * state to clear).
   */
  [[nodiscard]] result<void> try_reset() const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->reset(ctx_);
  }

private:
  template <typename Backend> static std::size_t size_entry(void *ctx) noexcept {
    return mmio_device_traits<Backend>::size(*static_cast<Backend *>(ctx));
  }

  template <typename Backend>
  static result<void> read_entry(void *ctx, std::uint64_t offset, span<std::byte> dst) noexcept {
    return mmio_device_traits<Backend>::try_read(*static_cast<Backend *>(ctx), offset, dst);
  }

  template <typename Backend>
  static result<void> write_entry(void *ctx, std::uint64_t offset, span<const std::byte> src) noexcept {
    using traits = mmio_device_traits<Backend>;
    if constexpr (!detail::mmio_device_has_write<traits>::value) {
      (void)ctx;
      (void)offset;
      (void)src;
      return unexpected(error::unsupported_operation);
    } else {
      auto &backend = *static_cast<Backend *>(ctx);
      if constexpr (detail::mmio_device_has_is_read_only<traits>::value) {
        if (traits::is_read_only(backend))
          return unexpected(error::permission_denied);
      }
      return traits::try_write(backend, offset, src);
    }
  }

  template <typename Backend> static bool is_read_only_entry(void *ctx) noexcept {
    using traits = mmio_device_traits<Backend>;
    if constexpr (!detail::mmio_device_has_write<traits>::value) {
      (void)ctx;
      return true;
    } else if constexpr (detail::mmio_device_has_is_read_only<traits>::value) {
      return traits::is_read_only(*static_cast<Backend *>(ctx));
    } else {
      (void)ctx;
      return false;
    }
  }

  template <typename Backend> static bool is_available_entry(void *ctx) noexcept {
    using traits = mmio_device_traits<Backend>;
    if constexpr (detail::mmio_device_has_is_available<traits>::value) {
      return traits::is_available(*static_cast<Backend *>(ctx));
    } else {
      (void)ctx;
      return true;
    }
  }

  template <typename Backend> static result<void> reset_entry(void *ctx) noexcept {
    using traits = mmio_device_traits<Backend>;
    if constexpr (detail::mmio_device_has_reset<traits>::value) {
      return traits::try_reset(*static_cast<Backend *>(ctx));
    } else {
      (void)ctx;
      return {};
    }
  }

  template <typename Backend>
  static constexpr vtable s_vtbl{&size_entry<Backend>,         &read_entry<Backend>,
                                 &write_entry<Backend>,        &is_read_only_entry<Backend>,
                                 &is_available_entry<Backend>, &reset_entry<Backend>};

  void *ctx_ = nullptr;
  const vtable *vtbl_ = nullptr;
};

} // namespace structo::hypervisor
