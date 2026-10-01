#pragma once

#include <reloco/error.hpp>
#include "phys_translator.hpp"
#include "sg_list.hpp"
#include <algorithm>
#include <type_traits>

namespace structo {

using namespace reloco;

/**
 * @brief Translates Scatter-Gather lists between physical address spaces
 * (e.g., guest_phys_space -> host_phys_space) across page boundaries.
 */
class sg_translator {
public:
  /**
   * @brief Translates an input sequence into a strongly-typed output SGL.
   *
   * @tparam Policy A mapping policy compatible with `phys_translator`; see
   * `phys_translator.hpp`'s @file-level docs for a complete example `Policy`.
   * @tparam InIterable Any iterable container/span of `sg_entry`.
   * @tparam OutContainer The underlying vector backing the output `sg_list`.
   */
  template <typename Policy, typename InIterable, typename OutContainer>
  [[nodiscard]] static RELOCO_CONSTEXPR20 result<void> translate(const InIterable &input, sg_list<OutContainer> &output,
                                                                 size_t page_size,
                                                                 const Policy &policy = Policy{}) noexcept {
    using from_space = typename Policy::from_space;

    // Power-of-two check for fast bitwise masking
    if (page_size == 0 || (page_size & (page_size - 1)) != 0) {
      return unexpected(error::invalid_argument);
    }

    phys_translator<Policy> translator{policy};

    for (const auto &in_elem : input) {
      // Input must be in the 'from_space' expected by the Policy
      static_assert(std::is_same_v<typename std::decay_t<decltype(in_elem)>::space_tag, from_space>,
                    "Input SGL space_tag does not match translator's from_space");

      auto curr_paddr = in_elem.addr;
      auto remaining = in_elem.length;
      using phys_int = decltype(remaining);

      const phys_int page_mask = ~(static_cast<phys_int>(page_size) - 1);

      while (remaining > 0) {
        phys_int curr_val = curr_paddr.value;
        phys_int page_base = curr_val & page_mask;
        phys_int offset = curr_val - page_base;

        phys_int chunk_size = std::min(remaining, static_cast<phys_int>(page_size - offset));

        // Translate the chunk.
        // phys_translator automatically propagates the 'to_space' tag[cite: 6].
        auto translated_res = translator(curr_paddr, chunk_size);
        if (!translated_res.has_value()) {
          return unexpected(translated_res.error());
        }

        // Append and auto-coalesce contiguous physical segments.
        // sg_list prevents length overflow and merges adjacent blocks.
        auto push_res = output.try_push_back(translated_res.value(), chunk_size);
        if (!push_res.has_value()) {
          return unexpected(push_res.error());
        }

        curr_paddr.value += chunk_size;
        remaining -= chunk_size;
      }
    }

    return {};
  }
};

} // namespace structo