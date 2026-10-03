// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file debug_symtab_resolver.hpp
 * @brief Adapts `structo::debug_symtab_view` to microfmt's
 * `symbol_resolver_traits<Tag>` customization point, so a `DSYM` blob can
 * back a `microfmt::remote_symbol_view`/`microfmt::make_remote_symbol`
 * call directly.
 *
 * Kept separate from `debug_symtab.hpp` (which only depends on `reloco`)
 * so a consumer that only needs the decoder -- e.g. a from-scratch crash
 * dump walker with its own output formatting -- never pays for pulling
 * in microfmt. Mirrors the split microfmt itself uses between
 * `inspector/symbol_resolver.hpp` (generic customization point) and
 * `inspector/dl_symbol_resolver.hpp` (one concrete backend).
 *
 * Usage:
 * @code
 * auto view = structo::debug_symtab_view::try_create(blob).value();
 * auto resolver = microfmt::symbol_resolver_ref::make<structo::debug_symtab_resolver_tag>(view);
 * char scratch[64];
 * microfmt::symbol_resolution_context ctx(scratch);
 * microfmt::println("fault at {}", microfmt::make_remote_symbol(fault_addr, resolver, ctx));
 * @endcode
 */

#include "debug_symtab.hpp"
#include <microfmt/inspector/symbol_resolver.hpp>
#include <reloco/value_ref.hpp>

namespace structo {

/**
 * @brief Tag selecting the `debug_symtab_view`-backed
 * `microfmt::symbol_resolver_traits` specialization.
 */
struct debug_symtab_resolver_tag {};

} // namespace structo

namespace microfmt {

/**
 * @brief Traits binding @ref structo::debug_symtab_resolver_tag to a
 * `structo::debug_symtab_view`.
 *
 * Stateful: resolution needs the specific blob view to search. The
 * returned `raw_resolved_symbol::symbol_name` aliases @p scratch (per
 * `debug_symtab_view::try_resolve`'s contract), so it stays valid exactly
 * as long as `scratch` does; `image_name`/`image_load_base` are left
 * empty/zero since a `DSYM` blob only ever describes a single image.
 */
template <> struct symbol_resolver_traits<structo::debug_symtab_resolver_tag> {
  using context_type = structo::debug_symtab_view;

  static bool resolve(reloco::value_ref<const context_type> view, uintptr_t addr, span<char> scratch,
                     raw_resolved_symbol &out_raw) noexcept {
    auto resolved = view.get()->try_resolve(static_cast<std::uint64_t>(addr), scratch);
    if (!resolved.has_value())
      return false;

    out_raw.symbol_name = resolved.value().name;
    out_raw.symbol_base = static_cast<uintptr_t>(resolved.value().symbol_base);
    out_raw.is_exact = resolved.value().is_exact;
    return true;
  }
};

} // namespace microfmt
