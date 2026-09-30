<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `fdt_index<Container>`

`include/structo/fdt_index.hpp`

> Moved from `jplcz_reloco` into `jplcz_structo`'s own `include/structo/`
> and re-homed into the `structo` namespace (the header adds `using
> namespace reloco;` so unmoved reloco types remain reachable unqualified).
> Documented here as extracted from reloco's own reference, with some
> prose still to be reconciled with the new namespace split.

Random-access index over an `fdt_reader`'s blob: a template class over a
caller-supplied *class template* `Container` (e.g. `structo::external_vector`,
or an alias binding a second parameter, like
`template <typename T> using my_vec = structo::inline_vector<T, 32>;`),
mirroring `digraph`'s dense-index style but resolved once, up front, from
the struct block rather than grown incrementally. `try_build` walks the
blob a single time, iteratively (never recursively -- matching
`digraph.hpp`'s worklist convention, so stack footprint stays bounded
independent of tree depth), filling three caller-owned buffers: the node
table, an optional phandle index, and a transient depth-tracking scratch
stack that is discarded once the build finishes:

```cpp
using node_vec = structo::external_vector<structo::fdt::fdt_index_node>;
using phandle_vec = structo::external_vector<structo::fdt::fdt_index_phandle_entry>;
using frame_vec = structo::external_vector<structo::fdt::detail::fdt_index_build_frame>;

reloco::array<structo::fdt::fdt_index_node, 64> node_storage{};
reloco::array<structo::fdt::fdt_index_phandle_entry, 32> phandle_storage{};
reloco::array<structo::fdt::detail::fdt_index_build_frame, 16> stack_storage{};

auto made = structo::fdt::fdt_index<structo::external_vector>::try_build(
    reader, node_vec(structo::span(node_storage)), phandle_vec(structo::span(phandle_storage)),
    frame_vec(structo::span(stack_storage)));
if (!made)
  return made.error();
auto idx = std::move(made).value();

auto root = idx.root();
for (std::size_t child : idx.children(*root)) { // O(1) per hop, no descent
  const auto &node = idx.node(child);           // node.name, node.parent, ...
  for (auto prop : idx.properties(child)) {
    if (!prop)
      return prop.error();
    // prop->name, prop->try_as_u32()/try_as_string()
  }
}

if (auto found = idx.find_by_phandle(0x10))
  // *found is a node index
  ;
```

Every `Container<T>` instantiation is checked against the minimal
fallible-vector shape `fdt_index` requires (`size`/`empty`/`clear`/
`try_push_back`/`try_pop_back`/`operator[]`/`data`) -- matching every
reloco vector -- via a `void_t`-based detection trait, so a mismatched
`Container` fails with a short `static_assert` instead of a page of nested
template errors; on a C++20 compiler, the same check is also exposed as
`detail::fdt_index_compatible_container`, a `concept` alias over the same
trait (present only when C++20 or newer is active -- `fdt_index.hpp`
itself still compiles cleanly under C++17). `fdt_index_node` records each
node's struct-block offset, its body offset (right after its name, so
`properties()` never re-parses it), `parent`/`first_child`/`next_sibling`
links (`fdt_index_npos` for "none"), and its `phandle`/`linux,phandle`
value (`0` if absent -- phandle `0` is reserved per the DTB spec and is
never indexed). `children()` and `properties()` return Rust-style
`iterator_adaptor`-based views (see `include/structo/iterator.hpp`);
`children()` never descends into a subtree -- skipping one is just "don't
call `children()` on it," an O(1) operation with zero blob re-parsing.
`find_by_phandle()` is `O(log n)` via `span::binary_search_by` over the
phandle buffer, sorted once (via `span::sort_by`) at the end of
`try_build`. `all_nodes()` returns a flat, preorder `span<const
fdt_index_node>` view of every indexed node. `node()`, `parent_of()`,
`children()`, and `properties()` all assert (active even with `NDEBUG`) if
given an out-of-range node index; each has a fallible `try_`-prefixed
sibling (`try_node()`, `try_parent_of()`, `try_children()`,
`try_properties()`) that instead returns `error::out_of_bounds` through
`result<...>`, for callers walking an index by an untrusted/externally
supplied node index (e.g. one round-tripped through IPC) where an assert
would be an unacceptable abort surface. Node names are retrievable
directly while iterating either way: `children()`/`all_nodes()` yield
node indices, and `name_of(index)`/`try_name_of(index)` are `O(1)`
convenience wrappers over `node(index).name`/`try_node(index)->name` so a
walk never has to spell that out; property names need no such wrapper --
`properties()`/`try_properties()` already yield `fdt_property_view`
directly, whose `name` field is a plain public member. `find_child()` and
`find_property()` are always-fallible by-name lookups over a node's
*direct* children/properties only (never descending further), returning
`result<optional<...>>`: the outer `result` carries `error::out_of_bounds`
(bad node index) or a propagated struct-block decode error, while the
inner `optional` is simply empty (not an error) when no direct
child/property matches @p name -- e.g. `idx.find_child(cpus, "cpu@0")`,
`idx.find_property(node, "compatible")`. `find_by_path()` translates a
full, slash-separated path (e.g. `"/cpus/cpu@0"`, `libfdt`'s
`fdt_path_offset` convention) into a node index by repeatedly calling
`find_child()` one segment at a time from the root, failing with
`error::invalid_argument` if the path doesn't start with `'/'` or
`error::not_found` if any segment doesn't match. `try_build` fails with
`error::capacity_exceeded` if any caller-supplied buffer is too small (node
buffer, phandle buffer, or the scratch stack -- e.g. too shallow for the
blob's actual nesting depth) and with `error::invalid_argument`/
`error::out_of_bounds` on a truncated or corrupt struct block, never
trapping on malformed input.

`tests/test_fdt_real_world.cpp` decodes a real-world DTB captured from
QEMU's aarch64 `virt` machine (`qemu-system-aarch64 -M virt,dumpdtb=... -smp
4 -m 1G`, repacked with `dtc -p 0` to drop QEMU's padding), embedded as a
plain byte array in `tests/fixtures/qemu_virt_dtb.hpp`. This complements the
hand-built fixtures elsewhere with a sanity check against actual
firmware-generated output: 63 nodes, 8 phandles, and known paths/properties
are all round-tripped through `fdt_reader` and `fdt_index`.

`tests/perf_fdt_index.cpp` is a separate, non-gtest timing executable
(built as the `jplcz_reloco_fdt_index_perf` CMake target, only when system
`libfdt` -- Debian/Ubuntu's `libfdt-dev` -- is available) that benchmarks
property lookup, full-tree enumeration, and path translation against the
same embedded DTB, comparing `fdt_index`'s cached, pre-resolved lookups
against the reference `libfdt` C library's stateless, re-scanning
equivalents (`fdt_getprop`/`fdt_path_offset`/`fdt_next_node`). It reports
wall-clock nanoseconds per operation, not pass/fail assertions, and is
intentionally excluded from `ctest`.

See also: [`fdt_reader.md`](fdt_reader.md), [`fdt_memory.md`](fdt_memory.md).
