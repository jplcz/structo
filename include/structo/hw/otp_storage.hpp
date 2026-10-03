// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file otp_storage.hpp
 * @brief `structo::hw::otp_storage_ref`: a type-erased, non-owning handle
 * over hardware one-time-programmable (OTP) persistent storage -- an
 * eFuse/antifuse array (e.g. Allwinner/Rockchip SoC eFuse controllers,
 * NXP OCOTP, a RISC-V OpenTitan OTP controller) or a flash-sector-backed
 * OTP partition (e.g. STM32's OTP area, physically regular flash but
 * logically presented as write-once via a dedicated lock mechanism) --
 * plus the `otp_storage_traits<Backend>` customization point a concrete
 * backend specializes to be bindable through it.
 *
 * This header is deliberately abstraction-only, the write-once-storage
 * counterpart of `hw_rng.hpp`: there is no fuse-controller register
 * layout or programming-voltage sequencing here, nothing vendor- or
 * arch-specific. A concrete backend (a per-SoC eFuse-controller driver,
 * a flash-OTP-partition adapter, or a unit test's in-memory fake)
 * implements `otp_storage_traits<Backend>`.
 *
 * ## The one universal invariant: programming is OR-only and irreversible
 *
 * Every OTP/eFuse technology -- electrical fuses, antifuses, or a
 * flash-based OTP partition -- shares one physical contract regardless
 * of vendor: a "blank"/erased bit can be *programmed* ("burned"/"blown")
 * from its blank state to its programmed state exactly once, and never
 * back. There is no erase operation. Programming a byte therefore never
 * *overwrites* it -- it bitwise-**ORs** the requested bits into whatever
 * is already there (conventionally blank = `0`, programmed = `1`; a few
 * vendors invert this polarity, which a backend's own documentation
 * covers, not this header). Calling `try_program` a second time with
 * overlapping bits is not an error -- any bit already `1` simply stays
 * `1` -- but there is no way to ask for a bit to go back to `0`:
 * `try_program`'s `bits` parameter can only ever *add* programmed bits,
 * never remove them.
 *
 * This is also why, unlike `hw_rng_ref::try_generate64`, neither
 * `try_read` nor `try_program` here retries automatically on a
 * transient error: a spurious retry of an idempotent entropy draw is
 * harmless, but silently retrying a program pulse on storage that can
 * never be un-burned is not something this header will ever decide on
 * a caller's behalf. A caller that wants a retry loop (e.g. around a
 * `error::busy` from a programming-voltage pump still charging) is
 * expected to write it explicitly.
 *
 * ## Customization point: `otp_storage_traits<Backend>`
 *
 * `otp_storage_traits<Backend>` is left undefined for any `Backend`
 * that hasn't opted in (mirroring `hw_rng_traits`/`uart_traits`). A
 * specialization must supply:
 *
 * @code
 * template <> struct structo::hw::otp_storage_traits<my_backend> {
 *   static std::size_t size_bytes(my_backend &) noexcept;
 *   static reloco::result<void> try_read(my_backend &, std::size_t offset,
 *                                        reloco::span<std::byte> dst) noexcept;
 *   static reloco::result<void> try_program(my_backend &, std::size_t offset,
 *                                           reloco::span<const std::byte> bits) noexcept;
 * };
 * @endcode
 *
 * `size_bytes` reports the storage's total addressable size; `offset +
 * dst.size()`/`offset + bits.size()` beyond it is rejected by
 * `otp_storage_ref` itself with `error::out_of_range` before the
 * backend is ever called. `try_program` fails with
 * `error::permission_denied` if any byte in range falls in a
 * previously-locked region (see `try_lock` below), or whatever other
 * error the backend's hardware itself reports (e.g. `error::busy` while
 * a programming-voltage pump is still charging, `error::io_error` for a
 * programming-verify mismatch the backend itself detected).
 *
 * Optionally, a backend may also supply region locking, for hardware
 * that supports permanently write-protecting part of its OTP storage
 * independently of whether every bit in it has been programmed yet
 * (e.g. a dedicated "lock row" fuse bank):
 *
 * @code
 * static bool is_locked(my_backend &, std::size_t offset, std::size_t len) noexcept;
 * static reloco::result<void> try_lock(my_backend &, std::size_t offset, std::size_t len) noexcept;
 * @endcode
 *
 * and whether the storage is actually accessible right now (distinct
 * from a general compiled-in/architecture check -- e.g. OTP sense
 * amplifiers not yet powered, a controller still in reset):
 *
 * @code
 * static bool is_available(my_backend &) noexcept;
 * @endcode
 *
 * All three are detected via SFINAE (the same optional-member idiom
 * `hw_rng_traits::is_available`/`uart_traits::current_config` use). If
 * `is_locked`/`try_lock` are absent, `otp_storage_ref::is_locked()`
 * always reports `false` and `try_lock()` always fails with
 * `error::unsupported_operation` -- the backend has no concept of
 * sub-region locking, not that everything is always unlocked forever;
 * callers that need an actual permanent-lock guarantee must confirm the
 * concrete backend they bound actually implements it. If `is_available`
 * is absent, `otp_storage_ref::is_available()` reports `true` whenever
 * bound.
 */

#include <reloco/detail/assert.hpp>
#include <reloco/detail/compat.hpp>
#include <reloco/error.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/span.hpp>

#include <cstddef>
#include <cstdint>
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
 * program a concrete one-time-programmable storage backend, through
 * @ref otp_storage_ref.
 *
 * Intentionally left undefined for any `Backend` that hasn't been
 * adapted, mirroring `hw_rng_traits`/`uart_traits`. See the @file-level
 * docs above for the complete required/optional member list.
 */
template <typename Backend> struct otp_storage_traits;

namespace detail {

template <typename Backend, typename = void> struct has_otp_storage_traits : std::false_type {};

template <typename Backend>
struct has_otp_storage_traits<Backend, std::void_t<decltype(otp_storage_traits<Backend>::size_bytes),
                                                   decltype(otp_storage_traits<Backend>::try_read),
                                                   decltype(otp_storage_traits<Backend>::try_program)>>
    : std::true_type {};

// Detects the optional Traits::is_available probe.
template <typename Traits, typename = void> struct otp_storage_has_is_available : std::false_type {};
template <typename Traits>
struct otp_storage_has_is_available<Traits, std::void_t<decltype(Traits::is_available)>> : std::true_type {};

// Detects the optional Traits::is_locked probe.
template <typename Traits, typename = void> struct otp_storage_has_is_locked : std::false_type {};
template <typename Traits>
struct otp_storage_has_is_locked<Traits, std::void_t<decltype(Traits::is_locked)>> : std::true_type {};

// Detects the optional Traits::try_lock probe.
template <typename Traits, typename = void> struct otp_storage_has_try_lock : std::false_type {};
template <typename Traits>
struct otp_storage_has_try_lock<Traits, std::void_t<decltype(Traits::try_lock)>> : std::true_type {};

} // namespace detail

// ============================================================================
// Type-Erased OTP Storage Handle
// ============================================================================

/**
 * @brief Type-erased, non-owning handle over hardware one-time-
 * programmable (OTP) storage, for whatever concrete backend it is
 * bound to.
 *
 * Default-constructed (or copied from a default-constructed) refs are
 * *unbound*: every operation on one fails with
 * `error::unsupported_operation` rather than trapping, mirroring
 * `hw_rng_ref`/`io_space_ref`/`uart_ref`'s null-safety convention.
 *
 * Follows the single-`vtable`, resolved-once-per-`Backend` shape every
 * `structo` `*_ref` handle uses -- see
 * [`type-erased-base-containers.md`](https://github.com/jplcz/reloco/blob/master/docs/type-erased-base-containers.md)
 * and `docs/coding-guide.md`'s "Type-erase a `*_ref` handle's backend
 * behind one `vtable`" section.
 */
class RELOCO_POINTER otp_storage_ref {
public:
  /** @brief Fixed, per-bound-backend-type dispatch table. */
  struct vtable {
    std::size_t (*size_bytes)(void *ctx) noexcept;
    result<void> (*read)(void *ctx, std::size_t offset, span<std::byte> dst) noexcept;
    result<void> (*program)(void *ctx, std::size_t offset, span<const std::byte> bits) noexcept;
    bool (*is_locked)(void *ctx, std::size_t offset, std::size_t len) noexcept;
    result<void> (*try_lock)(void *ctx, std::size_t offset, std::size_t len) noexcept;
    bool (*is_available)(void *ctx) noexcept;
  };

  /** @brief Constructs an unbound ref. */
  constexpr otp_storage_ref() noexcept = default;

  /**
   * @brief Binds this ref to an existing, adapted backend.
   * @tparam Backend Concrete backend type, deduced. Must have an
   * @ref otp_storage_traits specialization.
   * @param b Backend to bind. Must outlive this handle and every copy of
   * it. Marked `explicit`: binding a backend is always a deliberate step,
   * never an implicit conversion.
   */
  template <typename Backend, std::enable_if_t<detail::has_otp_storage_traits<Backend>::value, int> = 0>
  constexpr explicit otp_storage_ref(Backend &b RELOCO_LIFETIMEBOUND RELOCO_LIFETIME_CAPTURE_BY_THIS) noexcept
      : ctx_(std::addressof(b)), vtbl_(&s_vtbl<Backend>) {}

  /** @brief Rejects rvalue/temporary backend bindings. */
  template <typename Backend, std::enable_if_t<!std::is_lvalue_reference_v<Backend>, int> = 0>
  otp_storage_ref(Backend &&) = delete;

  /** @brief Whether this ref is bound to a backend. */
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return vtbl_ != nullptr; }

  /** @brief Total addressable size of the bound storage, in bytes. `0` if unbound. */
  [[nodiscard]] std::size_t size_bytes() const noexcept { return vtbl_ ? vtbl_->size_bytes(ctx_) : 0; }

  /**
   * @brief Whether the bound backend's OTP storage is actually
   * accessible right now. `false` if unbound; if the bound backend does
   * not implement the optional `otp_storage_traits::is_available`,
   * assumed `true` whenever bound.
   */
  [[nodiscard]] bool is_available() const noexcept {
    if (!vtbl_)
      return false;
    return vtbl_->is_available(ctx_);
  }

  /**
   * @brief Whether `[offset, offset + len)` falls (even partially)
   * within a region the backend has permanently write-protected.
   * `false` if unbound or if the backend does not implement the
   * optional `otp_storage_traits::is_locked` -- the backend has no
   * concept of sub-region locking, not that this range is confirmed
   * unlocked.
   */
  [[nodiscard]] bool is_locked(std::size_t offset, std::size_t len) const noexcept {
    if (!vtbl_)
      return false;
    return vtbl_->is_locked(ctx_, offset, len);
  }

  // --------------------------------------------------------------------
  // Mandatory backend operations (directly forwarded, with bounds
  // checking against size_bytes() performed here rather than in every
  // backend).
  // --------------------------------------------------------------------

  /**
   * @brief Reads `dst.size()` bytes starting at `offset`.
   * Fails with `error::unsupported_operation` if this ref is unbound,
   * `error::out_of_range` if `[offset, offset + dst.size())` falls
   * outside `[0, size_bytes())`, or whatever other error the backend
   * itself reports.
   */
  [[nodiscard]] result<void> try_read(std::size_t offset, span<std::byte> dst) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    if (offset > size_bytes() || dst.size() > size_bytes() - offset)
      return unexpected(error::out_of_range);
    return vtbl_->read(ctx_, offset, dst);
  }

  /**
   * @brief Bitwise-**ORs** `bits` into the storage starting at `offset`
   * -- programs ("burns") every requested `1` bit, irreversibly; bits
   * already programmed are left as-is, and there is no operation that
   * clears a programmed bit back.
   * Fails with `error::unsupported_operation` if this ref is unbound,
   * `error::out_of_range` if `[offset, offset + bits.size())` falls
   * outside `[0, size_bytes())`, `error::permission_denied` if any byte
   * in range falls within a locked region, or whatever other error the
   * backend itself reports (e.g. `error::busy` while a
   * programming-voltage pump is still charging).
   */
  [[nodiscard]] result<void> try_program(std::size_t offset, span<const std::byte> bits) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    if (offset > size_bytes() || bits.size() > size_bytes() - offset)
      return unexpected(error::out_of_range);
    if (is_locked(offset, bits.size()))
      return unexpected(error::permission_denied);
    return vtbl_->program(ctx_, offset, bits);
  }

  /**
   * @brief Permanently write-protects `[offset, offset + len)`: every
   * subsequent `try_program` call overlapping this range fails with
   * `error::permission_denied`, regardless of whether every bit in it
   * has actually been programmed yet.
   * Fails with `error::unsupported_operation` if this ref is unbound or
   * the bound backend does not implement the optional
   * `otp_storage_traits::try_lock`, `error::out_of_range` if the range
   * falls outside `[0, size_bytes())`, or whatever other error the
   * backend itself reports. There is no corresponding unlock operation:
   * a lock set this way is as permanent as the fuses it protects.
   */
  [[nodiscard]] result<void> try_lock(std::size_t offset, std::size_t len) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    if (offset > size_bytes() || len > size_bytes() - offset)
      return unexpected(error::out_of_range);
    return vtbl_->try_lock(ctx_, offset, len);
  }

  // --------------------------------------------------------------------
  // Generic convenience, synthesized purely from try_program/try_read
  // above -- no further backend support is required.
  // --------------------------------------------------------------------

  /**
   * @brief Programs `bits` at `offset` (as `try_program`), then reads
   * the same range back and confirms every requested `1` bit actually
   * reads back as `1` -- the read-back-and-verify step most real
   * eFuse/OTP programming procedures mandate, since a programming pulse
   * can fail to fully burn a bit without the controller itself
   * reporting an error.
   *
   * The verify compares `(readback[i] & bits[i]) == bits[i]` per byte,
   * *not* `readback[i] == bits[i]`: other bits already programmed by an
   * earlier, unrelated `try_program` call in the same byte are expected
   * and not a verification failure, only a requested bit that failed to
   * take is.
   *
   * Fails with whatever `try_program`/`try_read` themselves would fail
   * with, or `error::io_error` if the verify comparison fails;
   * `scratch` must be at least `bits.size()` bytes and is used as the
   * read-back buffer (no further storage is allocated).
   */
  [[nodiscard]] result<void> try_program_verify(std::size_t offset, span<const std::byte> bits,
                                                span<std::byte> scratch) const noexcept {
    if (scratch.size() < bits.size())
      return unexpected(error::invalid_argument);
    auto programmed = try_program(offset, bits);
    if (!programmed)
      return programmed;
    span<std::byte> readback(scratch.data(), bits.size());
    auto read = try_read(offset, readback);
    if (!read)
      return read;
    for (std::size_t i = 0; i < bits.size(); ++i) {
      if ((readback[i] & bits[i]) != bits[i])
        return unexpected(error::io_error);
    }
    return {};
  }

private:
  template <typename Backend> static std::size_t size_bytes_entry(void *ctx) noexcept {
    return otp_storage_traits<Backend>::size_bytes(*static_cast<Backend *>(ctx));
  }

  template <typename Backend>
  static result<void> read_entry(void *ctx, std::size_t offset, span<std::byte> dst) noexcept {
    return otp_storage_traits<Backend>::try_read(*static_cast<Backend *>(ctx), offset, dst);
  }

  template <typename Backend>
  static result<void> program_entry(void *ctx, std::size_t offset, span<const std::byte> bits) noexcept {
    return otp_storage_traits<Backend>::try_program(*static_cast<Backend *>(ctx), offset, bits);
  }

  template <typename Backend>
  static bool is_locked_entry(void *ctx, std::size_t offset, std::size_t len) noexcept {
    using traits = otp_storage_traits<Backend>;
    if constexpr (detail::otp_storage_has_is_locked<traits>::value) {
      return traits::is_locked(*static_cast<Backend *>(ctx), offset, len);
    } else {
      (void)ctx;
      (void)offset;
      (void)len;
      return false;
    }
  }

  template <typename Backend>
  static result<void> try_lock_entry(void *ctx, std::size_t offset, std::size_t len) noexcept {
    using traits = otp_storage_traits<Backend>;
    if constexpr (detail::otp_storage_has_try_lock<traits>::value) {
      return traits::try_lock(*static_cast<Backend *>(ctx), offset, len);
    } else {
      (void)ctx;
      (void)offset;
      (void)len;
      return unexpected(error::unsupported_operation);
    }
  }

  template <typename Backend> static bool is_available_entry(void *ctx) noexcept {
    using traits = otp_storage_traits<Backend>;
    if constexpr (detail::otp_storage_has_is_available<traits>::value) {
      return traits::is_available(*static_cast<Backend *>(ctx));
    } else {
      (void)ctx;
      return true;
    }
  }

  template <typename Backend>
  static constexpr vtable s_vtbl{&size_bytes_entry<Backend>, &read_entry<Backend>,       &program_entry<Backend>,
                                 &is_locked_entry<Backend>,  &try_lock_entry<Backend>,   &is_available_entry<Backend>};

  void *ctx_ = nullptr;
  const vtable *vtbl_ = nullptr;
};

} // namespace hw
} // namespace structo
