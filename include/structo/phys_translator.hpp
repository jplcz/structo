// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <reloco/error.hpp>
#include "phys_addr.hpp"
#include <type_traits>
#include <utility>

namespace structo {

using namespace reloco;

/**
 * @brief Functor for fallibly translating physical addresses.
 *
 * The Policy must provide:
 * - `using from_space`
 * - `using to_space`
 * - `result<PhysInt> translate(PhysInt addr, PhysInt size) const noexcept`
 */
/**
 * @brief Policy-based virtual/physical address translator.
 * @tparam Policy Translation policy.
 */
template <typename Policy> class phys_translator {
public:
  using from_space = typename Policy::from_space;
  using to_space = typename Policy::to_space;

  constexpr phys_translator() noexcept = default;
  constexpr explicit phys_translator(Policy p) noexcept : policy_(std::move(p)) {}

  /** @brief Translates with an explicit size (mandatory for void pointers or dynamic arrays) */
  template <typename T, typename PhysInt, typename SizeType>
  [[nodiscard]] result<phys_addr<T, to_space, PhysInt>> operator()(phys_addr<T, from_space, PhysInt> paddr,
                                                                   SizeType size) const noexcept {
    if (paddr.is_null()) {
      return phys_addr<T, to_space, PhysInt>{nullptr};
    }

    auto res = policy_.translate(paddr.value, static_cast<PhysInt>(size));
    if (!res.has_value()) {
      return unexpected(res.error());
    }

    return phys_addr<T, to_space, PhysInt>{res.value()};
  }

  // Auto-deduces size using sizeof(T). Disabled if T is void.
  template <typename T, typename PhysInt, typename U = T, std::enable_if_t<!std::is_void_v<U>, int> = 0>
  [[nodiscard]] result<phys_addr<T, to_space, PhysInt>>
  operator()(phys_addr<T, from_space, PhysInt> paddr) const noexcept {
    return (*this)(paddr, static_cast<PhysInt>(sizeof(T)));
  }

private:
  Policy policy_;
};

/** @brief physical_cast with an explicit size (for void/dynamic buffers) */
template <typename Policy, typename T, typename PhysInt, typename SizeType>
[[nodiscard]] constexpr result<phys_addr<T, typename Policy::to_space, PhysInt>>
physical_cast(phys_addr<T, typename Policy::from_space, PhysInt> paddr, SizeType size,
              const Policy &policy = Policy{}) noexcept {
  return phys_translator<Policy>{policy}(paddr, size);
}

// physical_cast with implicit sizeof(T) deduction
template <typename Policy, typename T, typename PhysInt, typename U = T, std::enable_if_t<!std::is_void_v<U>, int> = 0>
[[nodiscard]] constexpr result<phys_addr<T, typename Policy::to_space, PhysInt>>
physical_cast(phys_addr<T, typename Policy::from_space, PhysInt> paddr, const Policy &policy = Policy{}) noexcept {
  return phys_translator<Policy>{policy}(paddr);
}

} // namespace structo