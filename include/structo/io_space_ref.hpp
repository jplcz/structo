// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file io_space_ref.hpp
 * @brief `structo::io_space_ref<SpaceTag>`: a type-erased, non-owning
 * handle that performs the actual fixed-width loads/stores and
 * `rep insb`/`outsb`-style string I/O over an `io_address<T, SpaceTag,
 * IoInt>`, plus the `io_space_traits<Backend>` customization point a
 * concrete backend specializes to be bindable through it.
 *
 * `io_address.hpp` is deliberately pure address tagging and arithmetic --
 * no `inb`/`outb`, no volatile MMIO load/store, nothing. This header is
 * the layer promised there: it owns *access*, not addressing. An
 * `io_address` says *which* register a caller means; an `io_space_ref`
 * says *how* to actually read or write it, for one concrete backend
 * (a port-I/O driver issuing real `IN`/`OUT` instructions, a plain
 * volatile-pointer MMIO window, a hypervisor's trap-and-emulate stub, a
 * unit test's in-memory fake register file, ...).
 *
 * ## Why type-erased, unlike every other `structo` abstraction
 *
 * `phys_translator`/`phys_space_ref` and friends elsewhere in this
 * library are policy-based templates: the concrete translation/access
 * strategy is a compile-time type parameter, resolved and inlined at
 * each call site. `io_space_ref` deliberately breaks that pattern and
 * erases the backend behind a small, fixed vtable (the same shape
 * `container_ref.hpp`'s `mutable_container_ref`/`function_ref.hpp`'s
 * `function_ref` already use: a two-word handle -- an untyped context
 * pointer plus a `const vtable *` -- no virtual base class, no RTTI, no
 * allocation of its own) because the whole point of this type is to be
 * passed across boundaries where the concrete backend is *not* known at
 * compile time: a device driver written once against `io_space_ref<
 * device_io_space>` and handed a real MMIO backend in production but a
 * fault-injecting fake in tests, or a syscall/hypercall dispatcher
 * choosing at runtime (based on which domain trapped) which backend's
 * `io_space_ref` to hand a device-emulation routine.
 *
 * ## Customizing: `io_space_traits<Backend>`
 *
 * `io_space_traits<Backend>` is left undefined for any `Backend` that
 * hasn't opted in (mirroring `container_ref_traits`/`allocator_traits`).
 * A specialization must supply, at minimum, fixed-width single-access
 * loads/stores over a raw `std::uint64_t` address (the "wire" address
 * format `io_space_ref` uses regardless of the concrete `io_address`'s
 * own, possibly narrower, `IoInt`):
 *
 * @code
 * template <> struct structo::io_space_traits<my_mmio_backend> {
 *   static reloco::result<std::uint8_t>  read8 (my_mmio_backend &, std::uint64_t addr) noexcept;
 *   static reloco::result<std::uint16_t> read16(my_mmio_backend &, std::uint64_t addr) noexcept;
 *   static reloco::result<std::uint32_t> read32(my_mmio_backend &, std::uint64_t addr) noexcept;
 *   static reloco::result<std::uint64_t> read64(my_mmio_backend &, std::uint64_t addr) noexcept;
 *   static reloco::result<void> write8 (my_mmio_backend &, std::uint64_t addr, std::uint8_t  val) noexcept;
 *   static reloco::result<void> write16(my_mmio_backend &, std::uint64_t addr, std::uint16_t val) noexcept;
 *   static reloco::result<void> write32(my_mmio_backend &, std::uint64_t addr, std::uint32_t val) noexcept;
 *   static reloco::result<void> write64(my_mmio_backend &, std::uint64_t addr, std::uint64_t val) noexcept;
 * };
 * @endcode
 *
 * A backend that genuinely cannot do some width (e.g. a port space with
 * no 64-bit `IN`/`OUT`) must still provide that function, reporting its
 * own lack of support through `unexpected(error::unsupported_operation)`
 * -- there is no separate optional-capability flag scheme here, exactly
 * like `container_ref_traits`.
 *
 * Optionally, a backend may *also* supply "rep" string-I/O fast paths for
 * any subset of widths -- real hardware with a true `rep insb`/`outsb`
 * instruction (or an MMIO FIFO backend that can batch a burst read under
 * one lock) can service many same-address accesses in a single
 * trampoline call instead of `io_space_ref` looping the single-access
 * function one element at a time:
 *
 * @code
 * static reloco::result<void> read_rep8(my_backend &, std::uint64_t addr,
 *                                        std::uint8_t *dst, std::size_t count) noexcept;
 * static reloco::result<void> write_rep8(my_backend &, std::uint64_t addr,
 *                                         const std::uint8_t *src, std::size_t count) noexcept;
 * // ... and the 16/32/64-bit counterparts.
 * @endcode
 *
 * These are detected via SFINAE (the same optional-member idiom
 * `target_ptr_space_traits::min_value`/`max_value` use): if a given
 * width's `read_repN`/`write_repN` is absent, `io_space_ref` synthesizes
 * it generically on top of that width's mandatory single-access
 * `readN`/`writeN`, so a minimal backend only ever needs to implement the
 * 8 mandatory functions above.
 */

#include <reloco/detail/assert.hpp>
#include <reloco/detail/compat.hpp>
#include <reloco/error.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/span.hpp>
#include "io_address.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>

namespace structo {

using namespace reloco;

/**
 * @brief Opt-in customization point describing how to perform fixed-width
 * loads/stores (and, optionally, "rep" string I/O) on a concrete I/O-space
 * backend, through @ref io_space_ref.
 *
 * Intentionally left undefined for any `Backend` that hasn't been
 * adapted, mirroring `container_ref_traits`/`allocator_traits<Tag>`. See
 * the @file-level docs above for the complete required/optional member
 * list.
 */
template <typename Backend> struct io_space_traits;

namespace detail {

template <typename Backend, typename = void> struct has_io_space_traits : std::false_type {};

template <typename Backend>
struct has_io_space_traits<Backend, std::void_t<decltype(io_space_traits<Backend>::read8),
                                                decltype(io_space_traits<Backend>::read16),
                                                decltype(io_space_traits<Backend>::read32),
                                                decltype(io_space_traits<Backend>::read64),
                                                decltype(io_space_traits<Backend>::write8),
                                                decltype(io_space_traits<Backend>::write16),
                                                decltype(io_space_traits<Backend>::write32),
                                                decltype(io_space_traits<Backend>::write64)>> : std::true_type {};

// Detects an optional Traits::read_repN/write_repN "rep" string-I/O fast
// path; absent widths are synthesized generically from readN/writeN.
template <typename Traits, typename = void> struct io_space_has_read_rep8 : std::false_type {};
template <typename Traits>
struct io_space_has_read_rep8<Traits, std::void_t<decltype(Traits::read_rep8)>> : std::true_type {};

template <typename Traits, typename = void> struct io_space_has_read_rep16 : std::false_type {};
template <typename Traits>
struct io_space_has_read_rep16<Traits, std::void_t<decltype(Traits::read_rep16)>> : std::true_type {};

template <typename Traits, typename = void> struct io_space_has_read_rep32 : std::false_type {};
template <typename Traits>
struct io_space_has_read_rep32<Traits, std::void_t<decltype(Traits::read_rep32)>> : std::true_type {};

template <typename Traits, typename = void> struct io_space_has_read_rep64 : std::false_type {};
template <typename Traits>
struct io_space_has_read_rep64<Traits, std::void_t<decltype(Traits::read_rep64)>> : std::true_type {};

template <typename Traits, typename = void> struct io_space_has_write_rep8 : std::false_type {};
template <typename Traits>
struct io_space_has_write_rep8<Traits, std::void_t<decltype(Traits::write_rep8)>> : std::true_type {};

template <typename Traits, typename = void> struct io_space_has_write_rep16 : std::false_type {};
template <typename Traits>
struct io_space_has_write_rep16<Traits, std::void_t<decltype(Traits::write_rep16)>> : std::true_type {};

template <typename Traits, typename = void> struct io_space_has_write_rep32 : std::false_type {};
template <typename Traits>
struct io_space_has_write_rep32<Traits, std::void_t<decltype(Traits::write_rep32)>> : std::true_type {};

template <typename Traits, typename = void> struct io_space_has_write_rep64 : std::false_type {};
template <typename Traits>
struct io_space_has_write_rep64<Traits, std::void_t<decltype(Traits::write_rep64)>> : std::true_type {};

} // namespace detail

/**
 * @brief Type-erased, non-owning handle that performs fixed-width
 * loads/stores and "rep" string I/O over `io_address<T, SpaceTag, IoInt>`
 * addresses, for whatever concrete backend it is bound to.
 *
 * @tparam SpaceTag The I/O address-space domain this ref services (must
 * match the `SpaceTag` of every `io_address` passed to it -- a compile
 * error, not a runtime check, guards against e.g. handing a
 * `port_io_space` address to an `io_space_ref<device_io_space>`).
 *
 * Default-constructed (or copied from a default-constructed) refs are
 * *unbound*: every operation on one fails with
 * `error::unsupported_operation` rather than trapping, mirroring
 * `mutable_container_ref`'s null-safety convention.
 */
template <typename SpaceTag = default_io_space> class RELOCO_POINTER io_space_ref {
public:
  using space_tag = SpaceTag;

  /** @brief Fixed, per-bound-backend-type dispatch table. */
  struct vtable {
    result<std::uint8_t> (*read8)(void *ctx, std::uint64_t addr) noexcept;
    result<std::uint16_t> (*read16)(void *ctx, std::uint64_t addr) noexcept;
    result<std::uint32_t> (*read32)(void *ctx, std::uint64_t addr) noexcept;
    result<std::uint64_t> (*read64)(void *ctx, std::uint64_t addr) noexcept;
    result<void> (*write8)(void *ctx, std::uint64_t addr, std::uint8_t val) noexcept;
    result<void> (*write16)(void *ctx, std::uint64_t addr, std::uint16_t val) noexcept;
    result<void> (*write32)(void *ctx, std::uint64_t addr, std::uint32_t val) noexcept;
    result<void> (*write64)(void *ctx, std::uint64_t addr, std::uint64_t val) noexcept;
    result<void> (*read_rep8)(void *ctx, std::uint64_t addr, std::uint8_t *dst, std::size_t count) noexcept;
    result<void> (*read_rep16)(void *ctx, std::uint64_t addr, std::uint16_t *dst, std::size_t count) noexcept;
    result<void> (*read_rep32)(void *ctx, std::uint64_t addr, std::uint32_t *dst, std::size_t count) noexcept;
    result<void> (*read_rep64)(void *ctx, std::uint64_t addr, std::uint64_t *dst, std::size_t count) noexcept;
    result<void> (*write_rep8)(void *ctx, std::uint64_t addr, const std::uint8_t *src, std::size_t count) noexcept;
    result<void> (*write_rep16)(void *ctx, std::uint64_t addr, const std::uint16_t *src, std::size_t count) noexcept;
    result<void> (*write_rep32)(void *ctx, std::uint64_t addr, const std::uint32_t *src, std::size_t count) noexcept;
    result<void> (*write_rep64)(void *ctx, std::uint64_t addr, const std::uint64_t *src, std::size_t count) noexcept;
  };

  /** @brief Constructs an unbound ref. */
  constexpr io_space_ref() noexcept = default;

  /**
   * @brief Binds this ref to an existing, adapted backend.
   * @tparam Backend Concrete backend type, deduced. Must have an
   * @ref io_space_traits specialization.
   * @param b Backend to bind. Must outlive this handle and every copy of
   * it. Marked `explicit`: binding a backend is always a deliberate step,
   * never an implicit conversion.
   */
  template <typename Backend, std::enable_if_t<detail::has_io_space_traits<Backend>::value, int> = 0>
  constexpr explicit io_space_ref(Backend &b RELOCO_LIFETIMEBOUND RELOCO_LIFETIME_CAPTURE_BY_THIS) noexcept
      : ctx_(std::addressof(b)), vtbl_(&s_vtbl<Backend>) {}

  /** @brief Rejects rvalue/temporary backend bindings. */
  template <typename Backend, std::enable_if_t<!std::is_lvalue_reference_v<Backend>, int> = 0>
  io_space_ref(Backend &&) = delete;

  /** @brief Whether this ref is bound to a backend. */
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return vtbl_ != nullptr; }

  // --------------------------------------------------------------------
  // Fixed-width single access. Dispatches to one of the vtable's four
  // widths based on sizeof(T); T's raw bytes are memcpy'd to/from the
  // wire-format unsigned integer, exactly like target_ptr's
  // materialize/store (never reinterpret_cast, so no strict-aliasing UB
  // and no alignment requirement on T beyond its own).
  // --------------------------------------------------------------------

  /**
   * @brief Reads one `T` from `addr`, failing with
   * `error::unsupported_operation` if this ref is unbound, or whatever
   * the backend itself reports.
   */
  template <typename T, typename IoInt> [[nodiscard]] result<T> read(io_address<T, SpaceTag, IoInt> addr) const noexcept {
    static_assert(!std::is_void_v<T>,
                  "read<T>() requires a concrete register type -- cast_type<U>() a void io_address first");
    static_assert(std::is_trivially_copyable_v<T>, "read<T>() memcpy's T's raw bytes, so T must be trivially copyable");
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8,
                  "read<T>() only supports 1/2/4/8-byte register widths");
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    const auto raw_addr = static_cast<std::uint64_t>(addr.value);
    if constexpr (sizeof(T) == 1) {
      auto r = vtbl_->read8(ctx_, raw_addr);
      if (!r)
        return unexpected(r.error());
      return bit_copy<T>(r.value());
    } else if constexpr (sizeof(T) == 2) {
      auto r = vtbl_->read16(ctx_, raw_addr);
      if (!r)
        return unexpected(r.error());
      return bit_copy<T>(r.value());
    } else if constexpr (sizeof(T) == 4) {
      auto r = vtbl_->read32(ctx_, raw_addr);
      if (!r)
        return unexpected(r.error());
      return bit_copy<T>(r.value());
    } else {
      auto r = vtbl_->read64(ctx_, raw_addr);
      if (!r)
        return unexpected(r.error());
      return bit_copy<T>(r.value());
    }
  }

  /**
   * @brief Writes one `T` to `addr`, failing with
   * `error::unsupported_operation` if this ref is unbound, or whatever
   * the backend itself reports.
   */
  template <typename T, typename IoInt>
  [[nodiscard]] result<void> write(io_address<T, SpaceTag, IoInt> addr, const T &value) const noexcept {
    static_assert(!std::is_void_v<T>,
                  "write<T>() requires a concrete register type -- cast_type<U>() a void io_address first");
    static_assert(std::is_trivially_copyable_v<T>, "write<T>() memcpy's T's raw bytes, so T must be trivially copyable");
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8,
                  "write<T>() only supports 1/2/4/8-byte register widths");
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    const auto raw_addr = static_cast<std::uint64_t>(addr.value);
    if constexpr (sizeof(T) == 1) {
      return vtbl_->write8(ctx_, raw_addr, bit_copy<std::uint8_t>(value));
    } else if constexpr (sizeof(T) == 2) {
      return vtbl_->write16(ctx_, raw_addr, bit_copy<std::uint16_t>(value));
    } else if constexpr (sizeof(T) == 4) {
      return vtbl_->write32(ctx_, raw_addr, bit_copy<std::uint32_t>(value));
    } else {
      return vtbl_->write64(ctx_, raw_addr, bit_copy<std::uint64_t>(value));
    }
  }

  // --------------------------------------------------------------------
  // "rep" string I/O: `count` consecutive accesses of the *same* address,
  // stepping through a buffer -- the classic x86 `rep insb`/`insw`/`insl`
  // (and `rep outsb`/`outsw`/`outsl`) pattern used to drain/fill a
  // hardware FIFO register one port read/write at a time. The address
  // is *not* incremented between accesses (unlike a block memcpy); use
  // `io_math`/`checked_add` plus repeated single-element `read`/`write`
  // calls for an incrementing-address bulk transfer instead.
  // --------------------------------------------------------------------

  /** @brief Reads `dst.size()` consecutive `T`s from the fixed address `addr`. */
  template <typename T, typename IoInt>
  [[nodiscard]] result<void> read_rep(io_address<T, SpaceTag, IoInt> addr, span<T> dst) const noexcept {
    static_assert(!std::is_void_v<T>,
                  "read_rep<T>() requires a concrete register type -- cast_type<U>() a void io_address first");
    static_assert(std::is_trivially_copyable_v<T>,
                  "read_rep<T>() memcpy's T's raw bytes, so T must be trivially copyable");
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8,
                  "read_rep<T>() only supports 1/2/4/8-byte register widths");
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    const auto raw_addr = static_cast<std::uint64_t>(addr.value);
    if constexpr (sizeof(T) == 1) {
      return vtbl_->read_rep8(ctx_, raw_addr, reinterpret_cast<std::uint8_t *>(dst.data()), dst.size());
    } else if constexpr (sizeof(T) == 2) {
      return vtbl_->read_rep16(ctx_, raw_addr, reinterpret_cast<std::uint16_t *>(dst.data()), dst.size());
    } else if constexpr (sizeof(T) == 4) {
      return vtbl_->read_rep32(ctx_, raw_addr, reinterpret_cast<std::uint32_t *>(dst.data()), dst.size());
    } else {
      return vtbl_->read_rep64(ctx_, raw_addr, reinterpret_cast<std::uint64_t *>(dst.data()), dst.size());
    }
  }

  /** @brief Writes `src.size()` consecutive `T`s to the fixed address `addr`. */
  template <typename T, typename IoInt>
  [[nodiscard]] result<void> write_rep(io_address<T, SpaceTag, IoInt> addr, span<const T> src) const noexcept {
    static_assert(!std::is_void_v<T>,
                  "write_rep<T>() requires a concrete register type -- cast_type<U>() a void io_address first");
    static_assert(std::is_trivially_copyable_v<T>,
                  "write_rep<T>() memcpy's T's raw bytes, so T must be trivially copyable");
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8,
                  "write_rep<T>() only supports 1/2/4/8-byte register widths");
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    const auto raw_addr = static_cast<std::uint64_t>(addr.value);
    if constexpr (sizeof(T) == 1) {
      return vtbl_->write_rep8(ctx_, raw_addr, reinterpret_cast<const std::uint8_t *>(src.data()), src.size());
    } else if constexpr (sizeof(T) == 2) {
      return vtbl_->write_rep16(ctx_, raw_addr, reinterpret_cast<const std::uint16_t *>(src.data()), src.size());
    } else if constexpr (sizeof(T) == 4) {
      return vtbl_->write_rep32(ctx_, raw_addr, reinterpret_cast<const std::uint32_t *>(src.data()), src.size());
    } else {
      return vtbl_->write_rep64(ctx_, raw_addr, reinterpret_cast<const std::uint64_t *>(src.data()), src.size());
    }
  }

private:
  // memcpy's between a wire-format unsigned integer and T, same
  // never-reinterpret_cast rationale as target_ptr's materialize/store.
  template <typename To, typename From> static To bit_copy(const From &from) noexcept {
    static_assert(sizeof(To) == sizeof(From), "bit_copy requires equally-sized To/From");
    To out;
    std::memcpy(&out, &from, sizeof(To));
    return out;
  }

  template <typename Backend> static result<std::uint8_t> read8_entry(void *ctx, std::uint64_t addr) noexcept {
    return io_space_traits<Backend>::read8(*static_cast<Backend *>(ctx), addr);
  }
  template <typename Backend> static result<std::uint16_t> read16_entry(void *ctx, std::uint64_t addr) noexcept {
    return io_space_traits<Backend>::read16(*static_cast<Backend *>(ctx), addr);
  }
  template <typename Backend> static result<std::uint32_t> read32_entry(void *ctx, std::uint64_t addr) noexcept {
    return io_space_traits<Backend>::read32(*static_cast<Backend *>(ctx), addr);
  }
  template <typename Backend> static result<std::uint64_t> read64_entry(void *ctx, std::uint64_t addr) noexcept {
    return io_space_traits<Backend>::read64(*static_cast<Backend *>(ctx), addr);
  }
  template <typename Backend> static result<void> write8_entry(void *ctx, std::uint64_t addr, std::uint8_t val) noexcept {
    return io_space_traits<Backend>::write8(*static_cast<Backend *>(ctx), addr, val);
  }
  template <typename Backend>
  static result<void> write16_entry(void *ctx, std::uint64_t addr, std::uint16_t val) noexcept {
    return io_space_traits<Backend>::write16(*static_cast<Backend *>(ctx), addr, val);
  }
  template <typename Backend>
  static result<void> write32_entry(void *ctx, std::uint64_t addr, std::uint32_t val) noexcept {
    return io_space_traits<Backend>::write32(*static_cast<Backend *>(ctx), addr, val);
  }
  template <typename Backend>
  static result<void> write64_entry(void *ctx, std::uint64_t addr, std::uint64_t val) noexcept {
    return io_space_traits<Backend>::write64(*static_cast<Backend *>(ctx), addr, val);
  }

  template <typename Backend>
  static result<void> read_rep8_entry(void *ctx, std::uint64_t addr, std::uint8_t *dst, std::size_t count) noexcept {
    using traits = io_space_traits<Backend>;
    auto &backend = *static_cast<Backend *>(ctx);
    if constexpr (detail::io_space_has_read_rep8<traits>::value) {
      return traits::read_rep8(backend, addr, dst, count);
    } else {
      for (std::size_t i = 0; i < count; ++i) {
        auto r = traits::read8(backend, addr);
        if (!r)
          return unexpected(r.error());
        dst[i] = r.value();
      }
      return {};
    }
  }
  template <typename Backend>
  static result<void> read_rep16_entry(void *ctx, std::uint64_t addr, std::uint16_t *dst, std::size_t count) noexcept {
    using traits = io_space_traits<Backend>;
    auto &backend = *static_cast<Backend *>(ctx);
    if constexpr (detail::io_space_has_read_rep16<traits>::value) {
      return traits::read_rep16(backend, addr, dst, count);
    } else {
      for (std::size_t i = 0; i < count; ++i) {
        auto r = traits::read16(backend, addr);
        if (!r)
          return unexpected(r.error());
        dst[i] = r.value();
      }
      return {};
    }
  }
  template <typename Backend>
  static result<void> read_rep32_entry(void *ctx, std::uint64_t addr, std::uint32_t *dst, std::size_t count) noexcept {
    using traits = io_space_traits<Backend>;
    auto &backend = *static_cast<Backend *>(ctx);
    if constexpr (detail::io_space_has_read_rep32<traits>::value) {
      return traits::read_rep32(backend, addr, dst, count);
    } else {
      for (std::size_t i = 0; i < count; ++i) {
        auto r = traits::read32(backend, addr);
        if (!r)
          return unexpected(r.error());
        dst[i] = r.value();
      }
      return {};
    }
  }
  template <typename Backend>
  static result<void> read_rep64_entry(void *ctx, std::uint64_t addr, std::uint64_t *dst, std::size_t count) noexcept {
    using traits = io_space_traits<Backend>;
    auto &backend = *static_cast<Backend *>(ctx);
    if constexpr (detail::io_space_has_read_rep64<traits>::value) {
      return traits::read_rep64(backend, addr, dst, count);
    } else {
      for (std::size_t i = 0; i < count; ++i) {
        auto r = traits::read64(backend, addr);
        if (!r)
          return unexpected(r.error());
        dst[i] = r.value();
      }
      return {};
    }
  }

  template <typename Backend>
  static result<void> write_rep8_entry(void *ctx, std::uint64_t addr, const std::uint8_t *src,
                                       std::size_t count) noexcept {
    using traits = io_space_traits<Backend>;
    auto &backend = *static_cast<Backend *>(ctx);
    if constexpr (detail::io_space_has_write_rep8<traits>::value) {
      return traits::write_rep8(backend, addr, src, count);
    } else {
      for (std::size_t i = 0; i < count; ++i) {
        auto r = traits::write8(backend, addr, src[i]);
        if (!r)
          return unexpected(r.error());
      }
      return {};
    }
  }
  template <typename Backend>
  static result<void> write_rep16_entry(void *ctx, std::uint64_t addr, const std::uint16_t *src,
                                        std::size_t count) noexcept {
    using traits = io_space_traits<Backend>;
    auto &backend = *static_cast<Backend *>(ctx);
    if constexpr (detail::io_space_has_write_rep16<traits>::value) {
      return traits::write_rep16(backend, addr, src, count);
    } else {
      for (std::size_t i = 0; i < count; ++i) {
        auto r = traits::write16(backend, addr, src[i]);
        if (!r)
          return unexpected(r.error());
      }
      return {};
    }
  }
  template <typename Backend>
  static result<void> write_rep32_entry(void *ctx, std::uint64_t addr, const std::uint32_t *src,
                                        std::size_t count) noexcept {
    using traits = io_space_traits<Backend>;
    auto &backend = *static_cast<Backend *>(ctx);
    if constexpr (detail::io_space_has_write_rep32<traits>::value) {
      return traits::write_rep32(backend, addr, src, count);
    } else {
      for (std::size_t i = 0; i < count; ++i) {
        auto r = traits::write32(backend, addr, src[i]);
        if (!r)
          return unexpected(r.error());
      }
      return {};
    }
  }
  template <typename Backend>
  static result<void> write_rep64_entry(void *ctx, std::uint64_t addr, const std::uint64_t *src,
                                        std::size_t count) noexcept {
    using traits = io_space_traits<Backend>;
    auto &backend = *static_cast<Backend *>(ctx);
    if constexpr (detail::io_space_has_write_rep64<traits>::value) {
      return traits::write_rep64(backend, addr, src, count);
    } else {
      for (std::size_t i = 0; i < count; ++i) {
        auto r = traits::write64(backend, addr, src[i]);
        if (!r)
          return unexpected(r.error());
      }
      return {};
    }
  }

  template <typename Backend>
  static constexpr vtable s_vtbl{
      &read8_entry<Backend>,      &read16_entry<Backend>,      &read32_entry<Backend>,      &read64_entry<Backend>,
      &write8_entry<Backend>,     &write16_entry<Backend>,     &write32_entry<Backend>,     &write64_entry<Backend>,
      &read_rep8_entry<Backend>,  &read_rep16_entry<Backend>,  &read_rep32_entry<Backend>,  &read_rep64_entry<Backend>,
      &write_rep8_entry<Backend>, &write_rep16_entry<Backend>, &write_rep32_entry<Backend>, &write_rep64_entry<Backend>};

  void *ctx_ = nullptr;
  const vtable *vtbl_ = nullptr;
};

} // namespace structo
