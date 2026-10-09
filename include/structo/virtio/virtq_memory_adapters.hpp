// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file virtq_memory_adapters.hpp
 * @brief Two composable `virtq_memory_traits` backends:
 *
 * - `virtq_memory_ref<Space>`: a type-erased, non-owning handle over *any*
 *   backend with a `virtq_memory_traits<Backend, Space>`, so ring/driver code
 *   can be written once, without a template parameter per memory type;
 * - `translating_virtq_memory<Translator, Inner>`: a backend that translates
 *   every access from one address space to another through a
 *   `phys_translator` (guest-physical -> host-physical, DMA bus -> CPU
 *   physical, ...) and forwards it to an inner backend. Translation is
 *   bounds-checked per access (with the access size), so a descriptor or ring
 *   address a hostile peer points outside the mapped window fails before any
 *   host memory is touched.
 *
 * They compose: a `translating_virtq_memory` bound into a `virtq_memory_ref`
 * lets one `split_virtq_device<..., virtq_memory_ref<guest_space>>` serve
 * guests whose RAM is mapped by different policies.
 *
 * ## Example
 *
 * @code
 * struct guest_ram_policy {
 *   using from_space = guest_phys_space;
 *   using to_space = host_window_space;
 *   std::uint64_t host_base, size;
 *   reloco::result<std::uint64_t> translate(std::uint64_t gpa, std::uint64_t len) const noexcept {
 *     if (gpa > size || len > size - gpa)
 *       return reloco::unexpected(reloco::error::out_of_range);
 *     return host_base + gpa;
 *   }
 * };
 *
 * direct_virtq_memory<host_window_space> window(host_ptr, size, base);
 * translating_virtq_memory<phys_translator<guest_ram_policy>, direct_virtq_memory<host_window_space>>
 *     guest_mem(phys_translator<guest_ram_policy>(guest_ram_policy{base, size}), window);
 * virtq_memory_ref<guest_phys_space> ref(guest_mem); // type-erased
 * @endcode
 *
 * The security contract of `virtq_memory.hpp` (copy-out only, single fetch,
 * speculation-masked bounds) is the inner backend's responsibility; the
 * adapters add no peer-visible pointers.
 */

#include "virtq_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <reloco/error.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/span.hpp>
#include <structo/phys_addr.hpp>
#include <structo/phys_translator.hpp>
#include <type_traits>

namespace structo::virtio {

// ============================================================================
// Type-erased memory handle
// ============================================================================

/**
 * @brief Type-erased, non-owning handle over a `virtq_memory_traits<Backend, Space>`
 * backend. Unbound refs fail every access with `error::unsupported_operation`.
 */
template <typename Space> class RELOCO_POINTER virtq_memory_ref {
public:
  using addr_type = phys_addr<void, Space>;

  /** @brief Fixed, per-bound-backend-type dispatch table. */
  struct vtable {
    reloco::result<void> (*read)(void *ctx, addr_type a, reloco::span<std::byte> dst) noexcept;
    reloco::result<void> (*write)(void *ctx, addr_type a, reloco::span<const std::byte> src) noexcept;
    reloco::result<std::uint16_t> (*load16)(void *ctx, addr_type a) noexcept;
    reloco::result<void> (*store16)(void *ctx, addr_type a, std::uint16_t v) noexcept;
  };

  constexpr virtq_memory_ref() noexcept = default;

  /**
   * @brief Binds to @p backend, which must outlive this handle and its copies.
   * `virtq_memory_traits<Backend, Space>` must be specialized.
   */
  template <typename Backend, std::enable_if_t<!std::is_same_v<std::remove_cv_t<Backend>, virtq_memory_ref>, int> = 0>
  constexpr explicit virtq_memory_ref(Backend &backend RELOCO_LIFETIMEBOUND RELOCO_LIFETIME_CAPTURE_BY_THIS) noexcept
      : ctx_(std::addressof(backend)), vtbl_(&s_vtbl<Backend>) {}

  template <typename Backend, std::enable_if_t<!std::is_lvalue_reference_v<Backend>, int> = 0>
  virtq_memory_ref(Backend &&) = delete;

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return vtbl_ != nullptr; }

  [[nodiscard]] reloco::result<void> try_read(addr_type a, reloco::span<std::byte> dst) const noexcept {
    if (!vtbl_)
      return reloco::unexpected(reloco::error::unsupported_operation);
    return vtbl_->read(ctx_, a, dst);
  }
  [[nodiscard]] reloco::result<void> try_write(addr_type a, reloco::span<const std::byte> src) const noexcept {
    if (!vtbl_)
      return reloco::unexpected(reloco::error::unsupported_operation);
    return vtbl_->write(ctx_, a, src);
  }
  [[nodiscard]] reloco::result<std::uint16_t> try_load16(addr_type a) const noexcept {
    if (!vtbl_)
      return reloco::unexpected(reloco::error::unsupported_operation);
    return vtbl_->load16(ctx_, a);
  }
  [[nodiscard]] reloco::result<void> try_store16(addr_type a, std::uint16_t v) const noexcept {
    if (!vtbl_)
      return reloco::unexpected(reloco::error::unsupported_operation);
    return vtbl_->store16(ctx_, a, v);
  }

private:
  template <typename Backend>
  static constexpr vtable s_vtbl = {
      [](void *c, addr_type a, reloco::span<std::byte> dst) noexcept -> reloco::result<void> {
        return virtq_memory_traits<Backend, Space>::try_read(*static_cast<Backend *>(c), a, dst);
      },
      [](void *c, addr_type a, reloco::span<const std::byte> src) noexcept -> reloco::result<void> {
        return virtq_memory_traits<Backend, Space>::try_write(*static_cast<Backend *>(c), a, src);
      },
      [](void *c, addr_type a) noexcept -> reloco::result<std::uint16_t> {
        return virtq_memory_traits<Backend, Space>::try_load16(*static_cast<Backend *>(c), a);
      },
      [](void *c, addr_type a, std::uint16_t v) noexcept -> reloco::result<void> {
        return virtq_memory_traits<Backend, Space>::try_store16(*static_cast<Backend *>(c), a, v);
      },
  };

  void *ctx_ = nullptr;
  const vtable *vtbl_ = nullptr;
};

/** @brief `virtq_memory_traits` for the type-erased handle (forwards to the bound backend). */
template <typename Space> struct virtq_memory_traits<virtq_memory_ref<Space>, Space> {
  using mem_type = virtq_memory_ref<Space>;
  using addr_type = phys_addr<void, Space>;

  [[nodiscard]] static reloco::result<void> try_read(mem_type &m, addr_type a, reloco::span<std::byte> dst) noexcept {
    return m.try_read(a, dst);
  }
  [[nodiscard]] static reloco::result<void> try_write(mem_type &m, addr_type a,
                                                      reloco::span<const std::byte> src) noexcept {
    return m.try_write(a, src);
  }
  [[nodiscard]] static reloco::result<std::uint16_t> try_load16(mem_type &m, addr_type a) noexcept {
    return m.try_load16(a);
  }
  [[nodiscard]] static reloco::result<void> try_store16(mem_type &m, addr_type a, std::uint16_t v) noexcept {
    return m.try_store16(a, v);
  }
};

// ============================================================================
// phys_translator adapter
// ============================================================================

/**
 * @brief Memory backend that translates each access through @p Translator
 * (a `phys_translator<Policy>`: `from_space` -> `to_space`) and forwards it to
 * @p Inner (`virtq_memory_traits<Inner, to_space>`).
 *
 * The translation is asked for exactly the bytes being accessed (2 for the
 * 16-bit accessors), so the policy's bounds check covers the whole access.
 * Translation errors are returned unchanged and nothing reaches @p Inner.
 */
template <typename Translator, typename Inner> class translating_virtq_memory {
public:
  using from_space = typename Translator::from_space;
  using to_space = typename Translator::to_space;
  using addr_type = phys_addr<void, from_space>;

  translating_virtq_memory(Translator translator, Inner &inner RELOCO_LIFETIMEBOUND) noexcept
      : translator_(translator), inner_(&inner) {}

  /** @brief Translates the @p len bytes at @p a; the translated address, or the policy's error. */
  [[nodiscard]] reloco::result<phys_addr<void, to_space>> translate(addr_type a, std::size_t len) const noexcept {
    return translator_(a, len);
  }
  [[nodiscard]] Inner &inner() const noexcept { return *inner_; }

private:
  Translator translator_;
  Inner *inner_;
};

template <typename Translator, typename Inner>
struct virtq_memory_traits<translating_virtq_memory<Translator, Inner>, typename Translator::from_space> {
  using mem_type = translating_virtq_memory<Translator, Inner>;
  using from_space = typename Translator::from_space;
  using to_space = typename Translator::to_space;
  using addr_type = phys_addr<void, from_space>;
  using inner_traits = virtq_memory_traits<Inner, to_space>;

  [[nodiscard]] static reloco::result<void> try_read(mem_type &m, addr_type a, reloco::span<std::byte> dst) noexcept {
    auto t = m.translate(a, dst.size());
    if (!t)
      return reloco::unexpected(t.error());
    return inner_traits::try_read(m.inner(), *t, dst);
  }
  [[nodiscard]] static reloco::result<void> try_write(mem_type &m, addr_type a,
                                                      reloco::span<const std::byte> src) noexcept {
    auto t = m.translate(a, src.size());
    if (!t)
      return reloco::unexpected(t.error());
    return inner_traits::try_write(m.inner(), *t, src);
  }
  [[nodiscard]] static reloco::result<std::uint16_t> try_load16(mem_type &m, addr_type a) noexcept {
    auto t = m.translate(a, sizeof(std::uint16_t));
    if (!t)
      return reloco::unexpected(t.error());
    return inner_traits::try_load16(m.inner(), *t);
  }
  [[nodiscard]] static reloco::result<void> try_store16(mem_type &m, addr_type a, std::uint16_t v) noexcept {
    auto t = m.translate(a, sizeof(std::uint16_t));
    if (!t)
      return reloco::unexpected(t.error());
    return inner_traits::try_store16(m.inner(), *t, v);
  }
};

} // namespace structo::virtio
