// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file fdt_index.hpp
 * @brief `fdt_index<Container>`: a random-access index over a Flattened
 * Device Tree (DTB) blob, built once from an `fdt_reader` in a single
 * forward pass, so repeated tree navigation afterwards never re-scans the
 * blob from the start.
 *
 * `fdt_reader` (see `fdt_reader.hpp`) is deliberately a single-pass,
 * forward-only stream: cheap to construct, but answering "what is this
 * node's parent", "what are this node's siblings", or "which node has
 * phandle N" all cost an `O(blob size)` re-scan. `fdt_index` trades a
 * one-time `O(blob size)` build pass for `O(1)` parent lookup, `O(1)`
 * child/sibling hops (via a first-child/next-sibling linked structure --
 * *without* ever having to descend into a subtree just to skip past it),
 * and an `O(log n)` phandle-to-node lookup, all served out of a handful
 * of flat index buffers instead of blob bytes.
 *
 * Every one of those buffers is supplied by the caller, not allocated by
 * this header: `fdt_index` is templated on `Container` -- any class
 * template with the fallible-vector shape common to every reloco vector
 * (`external_vector`, `inline_vector`, `outline_vector`, `sso_vector`,
 * `vector`, or a caller's own type): `size()`, `empty()`, `clear()`,
 * `try_push_back(T)`, `try_pop_back()`, `operator[](index)`, `data()`. The
 * common case is `reloco::external_vector`, binding every buffer directly
 * to caller-owned memory (e.g. a stack array sized from a known maximum
 * node/phandle/depth count) with no allocation at all:
 *
 * @code
 * auto reader_made = reloco::fdt::fdt_reader::try_create(blob);
 * if (!reader_made)
 *   return reader_made.error();
 *
 * std::array<reloco::fdt::fdt_index_node, 64> node_storage{};
 * std::array<reloco::fdt::fdt_index_phandle_entry, 16> phandle_storage{};
 * std::array<reloco::fdt::detail::fdt_index_build_frame, 16> stack_storage{};
 *
 * using index_type = reloco::fdt::fdt_index<reloco::external_vector>;
 * auto index_made = index_type::try_build(
 *     *reader_made,
 *     reloco::external_vector<reloco::fdt::fdt_index_node>(reloco::span(node_storage)),
 *     reloco::external_vector<reloco::fdt::fdt_index_phandle_entry>(reloco::span(phandle_storage)),
 *     reloco::external_vector<reloco::fdt::detail::fdt_index_build_frame>(reloco::span(stack_storage)));
 * if (!index_made)
 *   return index_made.error();
 * auto index = std::move(index_made).value();
 *
 * auto root = index.root().value();
 * for (std::size_t child : index.children(root)) {
 *   // one hop per sibling, no subtree descent, no blob re-scan
 * }
 * @endcode
 *
 * The third buffer (`fdt_index_build_frame`) is scratch space needed only
 * for the duration of `try_build` itself: one frame per currently-open
 * ancestor while walking the struct block, so the build is a fully
 * iterative, bounded-stack-footprint walk (never recursive, matching
 * `digraph.hpp`'s worklist convention) -- its capacity only needs to cover
 * the blob's actual maximum nesting depth, which is typically tiny (a
 * handful of levels), independent of the total node/property count.
 *
 * No floating point is used anywhere in this file.
 */

#include <reloco/detail/assert.hpp>
#include <reloco/detail/fdt_format.hpp>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include "fdt_reader.hpp"
#include <reloco/iterator.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/optional.hpp>
#include <reloco/span.hpp>
#include <reloco/string_view.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#if RELOCO_CXX20
#include <concepts>
#endif

namespace structo::fdt {

using namespace reloco;

RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

/** @brief Sentinel `fdt_index` node index meaning "no such node": returned
 * for the root's parent, a leaf's first child, and a last child's next
 * sibling. */
inline constexpr std::size_t fdt_index_npos = static_cast<std::size_t>(-1);

/** @brief One indexed node: struct-block position plus `O(1)` navigation
 * links, all resolved once by `fdt_index::try_build`. */
struct fdt_index_node {
  /** @brief Offset of this node's `FDT_BEGIN_NODE` token within the
   * blob's struct block. */
  std::size_t struct_offset{};
  /** @brief Offset within the struct block where this node's own
   * properties/children begin (right after its name), so
   * `fdt_index::properties()` never has to re-parse the name. */
  std::size_t body_offset{};
  /** @brief Index of the parent node, or `fdt_index_npos` for the root. */
  std::size_t parent{fdt_index_npos};
  /** @brief Index of the first child node, or `fdt_index_npos` if this
   * node has none. */
  std::size_t first_child{fdt_index_npos};
  /** @brief Index of the next sibling node, or `fdt_index_npos` if this
   * is the last child of its parent. */
  std::size_t next_sibling{fdt_index_npos};
  /** @brief This node's `phandle`/`linux,phandle` value, or `0` if it has
   * neither (`0` is reserved -- never a valid phandle). */
  uint32_t phandle{0};
  /** @brief Node name, borrowed from the blob (e.g. `"cpu@0"`). */
  string_view name;
};

/** @brief One `phandle -> node index` mapping, held sorted by `phandle` in
 * `fdt_index` for `O(log n)` lookup via `fdt_index::find_by_phandle`. */
struct fdt_index_phandle_entry {
  uint32_t phandle{};
  std::size_t node{};
};

namespace detail {

/** @brief Scratch stack frame used only during `fdt_index::try_build`:
 * one per currently-open ancestor node, tracking that node's index and
 * the index of its most-recently-appended child (to link `next_sibling`
 * in `O(1)` without walking the sibling chain). Never stored in the
 * finished `fdt_index`. */
struct fdt_index_build_frame {
  std::size_t node{fdt_index_npos};
  std::size_t last_child{fdt_index_npos};
};

/** @brief The minimal fallible-vector shape `fdt_index` requires from its
 * `Container` template parameter, matching every reloco vector
 * (`external_vector`, `inline_vector`, `outline_vector`, `sso_vector`,
 * `vector`): `size()`, `empty()`, `clear()`, `try_push_back(T)`,
 * `try_pop_back()`, `operator[](index)`, `data()`. Detected with the
 * classic `void_t` idiom so the check (and `fdt_index`'s `static_assert`s
 * using it) compile identically under C++17 and C++20; the `concept`
 * alias below is only *additionally* defined under C++20, purely so
 * callers on a C++20+ compiler may spell the constraint with `requires`
 * if they want to. */
template <typename C, typename T, typename = void> struct has_fdt_index_container_shape : std::false_type {};

template <typename C, typename T>
struct has_fdt_index_container_shape<
    C, T,
    std::void_t<decltype(std::declval<C &>().size()), decltype(std::declval<C &>().empty()),
                decltype(std::declval<C &>().clear()),
                decltype(std::declval<C &>().try_push_back(std::declval<T>())),
                decltype(std::declval<C &>().try_pop_back()), decltype(std::declval<C &>()[std::size_t{0}]),
                decltype(std::declval<C &>().data())>>
    : std::bool_constant<std::is_convertible_v<decltype(std::declval<C &>().size()), std::size_t> &&
                         std::is_convertible_v<decltype(std::declval<C &>().empty()), bool> &&
                         std::is_same_v<decltype(std::declval<C &>().try_push_back(std::declval<T>())), result<void>> &&
                         std::is_same_v<decltype(std::declval<C &>().try_pop_back()), result<void>> &&
                         std::is_same_v<decltype(std::declval<C &>()[std::size_t{0}]), T &> &&
                         std::is_same_v<decltype(std::declval<C &>().data()), T *>> {};

template <typename C, typename T>
inline constexpr bool fdt_index_compatible_container_v = has_fdt_index_container_shape<C, T>::value;

#if RELOCO_CXX20
template <typename C, typename T>
concept fdt_index_compatible_container = fdt_index_compatible_container_v<C, T>;
#endif // RELOCO_CXX20

} // namespace detail

/**
 * @brief Rust-style, `O(1)`-per-hop iterator over one node's direct
 * children (via the `first_child`/`next_sibling` links `fdt_index::
 * try_build` resolves), yielding child node indices. Never descends into
 * a child's own children -- exactly the "iterate without descending"
 * shape `fdt_index` exists to provide. Never fails: every hop is a plain
 * array read already validated at build time, so `item_type` is a bare
 * `std::size_t`, not a `result`.
 */
template <typename NodeContainer>
class RELOCO_POINTER fdt_index_child_iterator
    : public iterator_adaptor<fdt_index_child_iterator<NodeContainer>, std::size_t> {
public:
  using item_type = std::size_t;

  fdt_index_child_iterator(const NodeContainer *nodes, std::size_t first) noexcept : nodes_(nodes), current_(first) {}

  [[nodiscard]] optional<item_type> next_impl() noexcept {
    if (current_ == fdt_index_npos)
      return nullopt;
    const std::size_t yielded = current_;
    current_ = (*nodes_)[current_].next_sibling;
    return optional<item_type>(yielded);
  }

private:
  const NodeContainer *nodes_;
  std::size_t current_;
};

/**
 * @brief Rust-style, bounds-checked iterator over one node's *direct*
 * properties only (never its children's properties, never descending),
 * decoded on demand straight from the struct block starting at
 * `fdt_index_node::body_offset` -- see `fdt_index::properties()`. Mirrors
 * `fdt_reader`'s own token decoding (bounds-checked via `span::
 * try_subspan` at every step, fused: once it fails or naturally ends,
 * every subsequent poll returns empty), but is a plain, non-template
 * class since it only needs the two blob regions, not `Container`.
 */
class RELOCO_POINTER fdt_index_property_iterator
    : public iterator_adaptor<fdt_index_property_iterator, result<fdt_property_view>> {
public:
  using item_type = result<fdt_property_view>;

  fdt_index_property_iterator(span<const std::byte> struct_region, span<const std::byte> strings_region,
                               std::size_t start_offset) noexcept
      : struct_region_(struct_region), strings_region_(strings_region), cursor_(start_offset) {}

  [[nodiscard]] optional<item_type> next_impl() noexcept {
    if (done_)
      return nullopt;
    for (;;) {
      auto tok_r = fdt::detail::read_u32_at(struct_region_, cursor_);
      if (!tok_r)
        return fail(tok_r.error());
      const uint32_t tok = *tok_r;

      if (tok == fdt::detail::token_nop) {
        cursor_ += 4;
        continue;
      }
      if (tok != fdt::detail::token_prop) {
        // A begin_node/end_node/end token marks the end of this node's own
        // property list (properties always precede child nodes in a valid
        // DTB) -- not an error, just "no more direct properties".
        done_ = true;
        return nullopt;
      }

      auto len_r = fdt::detail::read_u32_at(struct_region_, cursor_ + 4);
      if (!len_r)
        return fail(len_r.error());
      auto nameoff_r = fdt::detail::read_u32_at(struct_region_, cursor_ + 8);
      if (!nameoff_r)
        return fail(nameoff_r.error());
      auto value_r = struct_region_.try_subspan(cursor_ + 12, static_cast<std::size_t>(*len_r));
      if (!value_r)
        return fail(error::out_of_bounds);
      auto name_r = fdt::detail::read_cstring(strings_region_, static_cast<std::size_t>(*nameoff_r));
      if (!name_r)
        return fail(name_r.error());
      // `value_r`'s success already proves `cursor_ + 12 + *len_r` fits
      // within `struct_region_`, so this addition cannot overflow.
      cursor_ = fdt::detail::align4(cursor_ + 12 + static_cast<std::size_t>(*len_r));
      fdt_property_view prop{};
      prop.name = name_r->first;
      prop.value = *value_r;
      return optional<item_type>(item_type(prop));
    }
  }

private:
  [[nodiscard]] optional<item_type> fail(error e) noexcept {
    done_ = true;
    return optional<item_type>(item_type(unexpected(e)));
  }

  span<const std::byte> struct_region_;
  span<const std::byte> strings_region_;
  std::size_t cursor_;
  bool done_{false};
};

/**
 * @brief Random-access index over an `fdt_reader`'s blob: `O(1)` parent
 * lookup, `O(1)` child/sibling hops without descending into a subtree,
 * and `O(log n)` phandle-to-node lookup, built once by `try_build` from
 * every caller-supplied `Container` buffer. See the file-level docs above
 * for the full rationale and a usage example.
 *
 * @tparam Container Class template supplying every backing buffer (node
 * index, phandle index, and the transient build-time stack); see
 * `detail::fdt_index_compatible_container` for the exact shape required.
 */
template <template <typename T> class Container>
class RELOCO_OWNER fdt_index {
public:
  using node_container = Container<fdt_index_node>;
  using phandle_container = Container<fdt_index_phandle_entry>;
  using frame_container = Container<detail::fdt_index_build_frame>;

  static_assert(detail::fdt_index_compatible_container_v<node_container, fdt_index_node>,
                "fdt_index<Container>: Container<fdt_index_node> must be a reloco-style fallible vector "
                "(size/empty/clear/try_push_back/try_pop_back/operator[]/data)");
  static_assert(detail::fdt_index_compatible_container_v<phandle_container, fdt_index_phandle_entry>,
                "fdt_index<Container>: Container<fdt_index_phandle_entry> must be a reloco-style fallible vector");
  static_assert(detail::fdt_index_compatible_container_v<frame_container, detail::fdt_index_build_frame>,
                "fdt_index<Container>: Container<fdt_index_build_frame> must be a reloco-style fallible vector");

  /**
   * @brief Builds the index in a single, iterative (never-recursive)
   * forward pass over `reader.struct_region()`, using `scratch` only for
   * the duration of this call (its final contents are unspecified and
   * never read back). Fails with:
   *   - `error::invalid_argument` if the struct block itself is malformed
   *     (unbalanced nodes, unknown token, unterminated name/string).
   *   - `error::out_of_bounds` if a token/name/property offset or length
   *     falls outside the blob.
   *   - `error::capacity_exceeded` if `nodes`, `phandles`, or `scratch`
   *     runs out of room -- size every buffer from a known bound on the
   *     blob's node count, phandle count, and maximum nesting depth,
   *     respectively.
   * `nodes`/`phandles`/`scratch` are all cleared up front, so passing in
   * a previously-used, non-empty buffer is fine.
   */
  [[nodiscard]] static result<fdt_index> try_build(const fdt_reader &reader, node_container nodes,
                                                    phandle_container phandles, frame_container scratch) noexcept {
    nodes.clear();
    phandles.clear();
    scratch.clear();

    const span<const std::byte> struct_region = reader.struct_region();
    const span<const std::byte> strings_region = reader.strings_region();

    std::size_t cursor = 0;
    for (;;) {
      auto tok_r = fdt::detail::read_u32_at(struct_region, cursor);
      if (!tok_r)
        return unexpected(tok_r.error());
      const uint32_t tok = *tok_r;

      if (tok == fdt::detail::token_nop) {
        cursor += 4;
        continue;
      }

      if (tok == fdt::detail::token_begin_node) {
        auto name_r = fdt::detail::read_cstring(struct_region, cursor + 4);
        if (!name_r)
          return unexpected(name_r.error());

        fdt_index_node entry{};
        entry.struct_offset = cursor;
        entry.body_offset = fdt::detail::align4(name_r->second);
        entry.parent = scratch.empty() ? fdt_index_npos : scratch[scratch.size() - 1].node;
        entry.name = name_r->first;
        const std::size_t new_index = nodes.size();
        if (auto push_res = nodes.try_push_back(std::move(entry)); !push_res)
          return unexpected(push_res.error());

        if (!scratch.empty()) {
          auto &frame = scratch[scratch.size() - 1];
          if (frame.last_child == fdt_index_npos)
            nodes[frame.node].first_child = new_index;
          else
            nodes[frame.last_child].next_sibling = new_index;
          frame.last_child = new_index;
        }

        if (auto push_frame = scratch.try_push_back(detail::fdt_index_build_frame{new_index, fdt_index_npos});
            !push_frame)
          return unexpected(push_frame.error());

        cursor = nodes[new_index].body_offset;
        continue;
      }

      if (tok == fdt::detail::token_end_node) {
        if (scratch.empty())
          return unexpected(error::invalid_argument); // FDT_END_NODE without a matching FDT_BEGIN_NODE.
        if (auto pop_res = scratch.try_pop_back(); !pop_res)
          return unexpected(pop_res.error());
        cursor += 4;
        continue;
      }

      if (tok == fdt::detail::token_prop) {
        auto len_r = fdt::detail::read_u32_at(struct_region, cursor + 4);
        if (!len_r)
          return unexpected(len_r.error());
        auto nameoff_r = fdt::detail::read_u32_at(struct_region, cursor + 8);
        if (!nameoff_r)
          return unexpected(nameoff_r.error());
        auto value_r = struct_region.try_subspan(cursor + 12, static_cast<std::size_t>(*len_r));
        if (!value_r)
          return unexpected(error::out_of_bounds);
        auto name_r = fdt::detail::read_cstring(strings_region, static_cast<std::size_t>(*nameoff_r));
        if (!name_r)
          return unexpected(name_r.error());

        if (!scratch.empty() && *len_r == 4 && (name_r->first == "phandle" || name_r->first == "linux,phandle")) {
          const std::size_t owner = scratch[scratch.size() - 1].node;
          if (nodes[owner].phandle == 0) {
            const uint32_t phandle_value = fdt::detail::load_be32(value_r->data());
            if (phandle_value != 0) { // 0 is reserved: "this node has no phandle".
              nodes[owner].phandle = phandle_value;
              if (auto push_ph = phandles.try_push_back(fdt_index_phandle_entry{phandle_value, owner}); !push_ph)
                return unexpected(push_ph.error());
            }
          }
        }

        // `value_r`'s success already proves `cursor + 12 + *len_r` fits
        // within `struct_region`, so this addition cannot overflow.
        cursor = fdt::detail::align4(cursor + 12 + static_cast<std::size_t>(*len_r));
        continue;
      }

      if (tok == fdt::detail::token_end) {
        if (!scratch.empty())
          return unexpected(error::invalid_argument); // FDT_END while nodes are still open.
        break;
      }

      return unexpected(error::invalid_argument); // Unknown token.
    }

    if (!phandles.empty()) {
      span<fdt_index_phandle_entry>(phandles.data(), phandles.size())
          .sort_by([](const fdt_index_phandle_entry &a, const fdt_index_phandle_entry &b) noexcept {
            return a.phandle < b.phandle;
          });
    }

    return fdt_index(std::move(nodes), std::move(phandles), struct_region, strings_region);
  }

  /** @brief Number of indexed nodes. */
  [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }

  /** @brief Index of the tree's single top-level (`"/"`) node, or
   * `error::not_found` if the index is empty. */
  [[nodiscard]] result<std::size_t> root() const noexcept {
    if (nodes_.empty())
      return unexpected(error::not_found);
    return std::size_t{0};
  }

  /** @brief Checked access to node `index`; asserts (active even with
   * `NDEBUG`, matching every other reloco container) if `index >=
   * node_count()`. Use `try_node` if `index` may be untrusted. */
  [[nodiscard]] const fdt_index_node &node(std::size_t index) const & noexcept RELOCO_LIFETIMEBOUND {
    RELOCO_ASSERT(index < nodes_.size(), "fdt_index: node index out of bounds");
    return nodes_[index];
  }

  /** @brief Fallible access to node `index`, failing with
   * `error::out_of_bounds` instead of asserting. */
  [[nodiscard]] result<const fdt_index_node *> try_node(std::size_t index) const & noexcept RELOCO_LIFETIMEBOUND {
    if (index >= nodes_.size())
      return unexpected(error::out_of_bounds);
    return &nodes_[index];
  }

  /** @brief `O(1)` parent lookup: the parent of `node(index)`, or
   * `fdt_index_npos` if `index` is the root. Asserts if `index` is
   * out of bounds; use `try_parent_of` if `index` may be untrusted. */
  [[nodiscard]] std::size_t parent_of(std::size_t index) const noexcept { return node(index).parent; }

  /** @brief Fallible parent lookup, failing with `error::out_of_bounds`
   * instead of asserting if `index >= node_count()`. */
  [[nodiscard]] result<std::size_t> try_parent_of(std::size_t index) const noexcept {
    auto n = try_node(index);
    if (!n)
      return unexpected(n.error());
    return (*n)->parent;
  }

  /** @brief `node(index)`'s name (e.g. `"cpu@0"`, or `""` for the root) --
   * a thin, `O(1)` convenience wrapper so a `children()`/`all_nodes()`
   * walk can fetch just the name without spelling out `node(index).name`.
   * Asserts if `index` is out of bounds; use `try_name_of` if `index` may
   * be untrusted. */
  [[nodiscard]] string_view name_of(std::size_t index) const noexcept RELOCO_LIFETIMEBOUND { return node(index).name; }

  /** @brief Fallible variant of `name_of`, failing with
   * `error::out_of_bounds` instead of asserting if `index >=
   * node_count()`. */
  [[nodiscard]] result<string_view> try_name_of(std::size_t index) const noexcept RELOCO_LIFETIMEBOUND {
    auto n = try_node(index);
    if (!n)
      return unexpected(n.error());
    return (*n)->name;
  }

  /** @brief An `O(1)`-per-hop iterator over `node(index)`'s direct
   * children only -- never descending into grandchildren. Asserts if
   * `index` is out of bounds; use `try_children` if `index` may be
   * untrusted. */
  [[nodiscard]] fdt_index_child_iterator<node_container> children(std::size_t index) const noexcept {
    return fdt_index_child_iterator<node_container>(&nodes_, node(index).first_child);
  }

  /** @brief Fallible variant of `children`, failing with
   * `error::out_of_bounds` instead of asserting if `index >=
   * node_count()`. */
  [[nodiscard]] result<fdt_index_child_iterator<node_container>> try_children(std::size_t index) const noexcept {
    auto n = try_node(index);
    if (!n)
      return unexpected(n.error());
    return fdt_index_child_iterator<node_container>(&nodes_, (*n)->first_child);
  }

  /** @brief An iterator over `node(index)`'s direct properties only --
   * never descending into children -- decoded on demand from the blob.
   * Asserts if `index` is out of bounds; use `try_properties` if `index`
   * may be untrusted. */
  [[nodiscard]] fdt_index_property_iterator properties(std::size_t index) const noexcept {
    return fdt_index_property_iterator(struct_region_, strings_region_, node(index).body_offset);
  }

  /** @brief Fallible variant of `properties`, failing with
   * `error::out_of_bounds` instead of asserting if `index >=
   * node_count()`. */
  [[nodiscard]] result<fdt_index_property_iterator> try_properties(std::size_t index) const noexcept {
    auto n = try_node(index);
    if (!n)
      return unexpected(n.error());
    return fdt_index_property_iterator(struct_region_, strings_region_, (*n)->body_offset);
  }

  /** @brief Looks up `node(index)`'s direct child named @p name (an exact
   * match against `fdt_index_node::name`, e.g. `"cpu@0"` -- never
   * descending into grandchildren). Fails with `error::out_of_bounds` if
   * `index` is out of range, otherwise returns an empty `optional` (not
   * an error) if no direct child has that name. */
  [[nodiscard]] result<optional<std::size_t>> find_child(std::size_t index, string_view name) const noexcept {
    auto kids = try_children(index);
    if (!kids)
      return unexpected(kids.error());
    for (std::size_t child : *kids) {
      if (node(child).name == name)
        return optional<std::size_t>(child);
    }
    return optional<std::size_t>(nullopt);
  }

  /** @brief Looks up `node(index)`'s direct property named @p name (an
   * exact match against `fdt_property_view::name` -- never descending
   * into children). Fails with `error::out_of_bounds` if `index` is out
   * of range, or whatever error the property iterator itself hit while
   * scanning (a malformed struct block); otherwise returns an empty
   * `optional` (not an error) if no direct property has that name. */
  [[nodiscard]] result<optional<fdt_property_view>> find_property(std::size_t index, string_view name) const noexcept {
    auto props = try_properties(index);
    if (!props)
      return unexpected(props.error());
    for (auto prop : *props) {
      if (!prop)
        return unexpected(prop.error());
      if (prop->name == name)
        return optional<fdt_property_view>(*prop);
    }
    return optional<fdt_property_view>(nullopt);
  }

  /** @brief Translates a full, slash-separated device-tree path (e.g.
   * `"/cpus/cpu@0"`, or `"/"` for the root) into a node index, walking
   * `find_child` one path segment at a time from the root -- `O(depth *
   * children-per-level)`, no blob re-scan from scratch. Fails with
   * `error::invalid_argument` if @p path doesn't start with `'/'`,
   * `error::not_found` if the index is empty or any path segment doesn't
   * match a direct child, or whatever error `find_child` propagates from
   * a malformed struct block. Repeated/trailing `'/'` characters are
   * tolerated (treated as empty segments and skipped), matching
   * `libfdt`'s `fdt_path_offset` behavior. */
  [[nodiscard]] result<std::size_t> find_by_path(string_view path) const noexcept {
    if (path.empty() || path[0] != '/')
      return unexpected(error::invalid_argument);
    auto current = root();
    if (!current)
      return unexpected(current.error());
    std::size_t pos = 1;
    while (pos < path.size()) {
      const std::size_t slash = path.find('/', pos);
      const string_view segment = (slash == string_view::npos) ? path.substr(pos) : path.substr(pos, slash - pos);
      if (!segment.empty()) {
        auto found = find_child(*current, segment);
        if (!found)
          return unexpected(found.error());
        if (!found->has_value())
          return unexpected(error::not_found);
        current = **found;
      }
      if (slash == string_view::npos)
        break;
      pos = slash + 1;
    }
    return *current;
  }

  /** @brief `O(log n)` lookup of the node whose `phandle`/`linux,phandle`
   * property equals @p phandle, or an empty `optional` if none does. */
  [[nodiscard]] optional<std::size_t> find_by_phandle(uint32_t phandle) const noexcept {
    const span<const fdt_index_phandle_entry> view(phandles_.data(), phandles_.size());
    const fdt_index_phandle_entry needle{phandle, 0};
    auto found = view.binary_search_by(
        needle, [](const fdt_index_phandle_entry &a, const fdt_index_phandle_entry &b) { return a.phandle < b.phandle; });
    if (!found)
      return nullopt;
    return optional<std::size_t>(view[*found].node);
  }

  /** @brief A flat, preorder view of every indexed node -- no
   * child/sibling hopping needed when the whole tree, in declaration
   * order, is what's wanted. */
  [[nodiscard]] span<const fdt_index_node> all_nodes() const noexcept RELOCO_LIFETIMEBOUND {
    return span<const fdt_index_node>(nodes_.data(), nodes_.size());
  }

private:
  fdt_index(node_container nodes, phandle_container phandles, span<const std::byte> struct_region,
            span<const std::byte> strings_region) noexcept
      : nodes_(std::move(nodes)), phandles_(std::move(phandles)), struct_region_(struct_region),
        strings_region_(strings_region) {}

  node_container nodes_;
  phandle_container phandles_;
  span<const std::byte> struct_region_;
  span<const std::byte> strings_region_;
};

RELOCO_END_UNSAFE_BUFFER_USAGE

} // namespace structo::fdt
