// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

// Micro-benchmark comparing two ways of answering the same three DTB
// queries against the real-world QEMU `virt` DTB embedded for
// test_fdt_real_world.cpp:
//
//   "libfdt" -- the reference `libfdt` C library (the same library dtc/
//               U-Boot/the Linux kernel use), called directly
//               (`fdt_path_offset`, `fdt_getprop`, `fdt_first_subnode`/
//               `fdt_next_subnode`/`fdt_first_property_offset`/
//               `fdt_next_property_offset`). It keeps no persistent
//               index: every call re-walks/re-decodes from the struct
//               block, so repeated queries are O(struct block size)
//               each.
//
//   "cached" -- the same query answered via a pre-built `structo::fdt::
//               fdt_index`, which resolves parent/child/sibling/phandle
//               links once up front (`try_build`) so most subsequent
//               queries become O(1) (node access), O(children of one
//               node) (by-name lookup), or O(log n) (phandle lookup)
//               instead of O(whole tree).
//
// This is deliberately a plain executable (no gtest, no assertions on
// timing) rather than a unit test: wall-clock numbers are informative,
// not a pass/fail contract, and don't belong in the correctness-only
// jplcz_reloco_tests binary/ctest run. Only built when system libfdt
// (libfdt-dev) is available -- see the JPLCZ_RELOCO_HAVE_LIBFDT guard in
// CMakeLists.txt.

#include "fixtures/qemu_virt_dtb.hpp"

#include <reloco/array.hpp>
#include <reloco/external_vector.hpp>
#include <structo/fdt_index.hpp>
#include <structo/fdt_reader.hpp>

extern "C" {
#include <libfdt.h>
}

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>

using structo::array;
using structo::external_vector;
using structo::span;
using structo::fdt::fdt_index;
using structo::fdt::fdt_index_node;
using structo::fdt::fdt_index_phandle_entry;
using structo::fdt::fdt_reader;
using structo::fdt::test::qemu_virt_dtb;
using structo::fdt::test::qemu_virt_dtb_size;
using build_frame = structo::fdt::detail::fdt_index_build_frame;

namespace {

using index_type = fdt_index<external_vector>;
using clock_type = std::chrono::steady_clock;

// Buffers large enough for the embedded fixture (63 nodes, 8 phandles);
// kept as file-scope storage so try_build()'s caller-provided-memory
// contract is exercised the same way real embedded callers would use it
// (fixed-size static/BSS buffers, no heap).
array<fdt_index_node, 128> g_nodes{};
array<fdt_index_phandle_entry, 32> g_phandles{};
array<build_frame, 16> g_stack{};

// A volatile accumulator that every benchmarked lambda below folds its
// result into. Without this, an optimizing compiler can (and, checked
// with -O2, does) prove these pure lookups' results are never observed
// and delete the calls entirely, timing an empty loop instead of the
// intended work.
volatile std::size_t g_sink = 0;

[[nodiscard]] index_type build_index(const fdt_reader &reader) {
  auto made = index_type::try_build(
      reader, external_vector<fdt_index_node>(span<fdt_index_node>(g_nodes.data(), g_nodes.size())),
      external_vector<fdt_index_phandle_entry>(span<fdt_index_phandle_entry>(g_phandles.data(), g_phandles.size())),
      external_vector<build_frame>(span<build_frame>(g_stack.data(), g_stack.size())));
  if (!made) {
    std::cerr << "fdt_index::try_build failed unexpectedly\n";
    std::exit(1);
  }
  return std::move(made).value();
}

template <typename Fn> double time_ns_per_iteration(std::size_t iterations, Fn &&fn) {
  const auto start = clock_type::now();
  for (std::size_t i = 0; i < iterations; ++i)
    fn();
  const auto end = clock_type::now();
  const auto elapsed_ns = std::chrono::duration_cast<std::chrono::duration<double, std::nano>>(end - start).count();
  return elapsed_ns / static_cast<double>(iterations);
}

void report(const char *label, double ns_per_op) {
  std::cout << "  " << std::left << std::setw(28) << label << std::right << std::fixed << std::setprecision(1)
            << std::setw(10) << ns_per_op << " ns/op\n";
}

// libfdt full-tree enumeration: fdt_next_node() over every node (matching
// depth-first order), and for each node, fdt_first_property_offset()/
// fdt_next_property_offset() over its properties -- the same shape as
// `fdt_for_each_subnode`/`fdt_for_each_property_offset` recursion, with
// no persistent structure built along the way.
std::size_t libfdt_enumerate_all(const void *fdt) {
  std::size_t count = 0;
  int depth = 0;
  for (int node = fdt_next_node(fdt, -1, &depth); node >= 0; node = fdt_next_node(fdt, node, &depth)) {
    ++count;
    int prop;
    fdt_for_each_property_offset(prop, fdt, node) {
      int len = 0;
      const auto *property = fdt_get_property_by_offset(fdt, prop, &len);
      if (property != nullptr)
        ++count;
    }
  }
  return count;
}

constexpr const char *k_property_name = "compatible";
constexpr const char *k_paths[] = {
    "/", "/cpus", "/cpus/cpu@0", "/cpus/cpu@3", "/memory@40000000", "/chosen", "/intc@8000000/v2m@8020000",
    "/pl011@9000000", "/no/such/node",
};
constexpr std::size_t k_path_count = sizeof(k_paths) / sizeof(k_paths[0]);

void run_property_lookup_benchmark(const void *fdt, const index_type &idx, std::size_t repeats) {
  std::cout << "property lookup: find_property(\"compatible\") on each of the fixed paths above\n";

  const double libfdt_ns = time_ns_per_iteration(repeats, [&] {
    for (const char *path : k_paths) {
      const int node = fdt_path_offset(fdt, path);
      if (node < 0)
        continue;
      int len = 0;
      const void *value = fdt_getprop(fdt, node, k_property_name, &len);
      g_sink += static_cast<std::size_t>(len) + (value != nullptr ? 1 : 0);
    }
  });
  report("libfdt (re-scan per call)", libfdt_ns / static_cast<double>(k_path_count));

  const double cached_ns = time_ns_per_iteration(repeats, [&] {
    for (const char *path : k_paths) {
      auto node = idx.find_by_path(path);
      if (!node)
        continue;
      auto found = idx.find_property(*node, k_property_name);
      if (found && found->has_value())
        g_sink += (*found)->value.size() + 1;
    }
  });
  report("cached (fdt_index)", cached_ns / static_cast<double>(k_path_count));
}

void run_enumeration_benchmark(const void *fdt, const index_type &idx, std::size_t repeats) {
  std::cout << "enumeration: one full preorder pass over every node and property\n";

  const double libfdt_ns = time_ns_per_iteration(repeats, [&] { g_sink += libfdt_enumerate_all(fdt); });
  report("libfdt (fdt_next_node etc.)", libfdt_ns);

  const double cached_ns = time_ns_per_iteration(repeats, [&] {
    std::size_t count = 0;
    for (const auto &node : idx.all_nodes()) {
      (void)node;
      ++count;
    }
    for (std::size_t i = 0; i < idx.node_count(); ++i) {
      auto props = idx.try_properties(i);
      if (!props)
        continue;
      for (auto prop : *props) {
        if (prop)
          ++count;
      }
    }
    g_sink += count;
  });
  report("cached (fdt_index)", cached_ns);
}

void run_path_translation_benchmark(const void *fdt, const index_type &idx, std::size_t repeats) {
  std::cout << "path translation: resolving each of the fixed paths above to a node\n";

  const double libfdt_ns = time_ns_per_iteration(repeats, [&] {
    for (const char *path : k_paths) {
      const int node = fdt_path_offset(fdt, path);
      g_sink += static_cast<std::size_t>(node + 1); // +1 so even -FDT_ERR_* results are non-negative
    }
  });
  report("libfdt (fdt_path_offset)", libfdt_ns / static_cast<double>(k_path_count));

  const double cached_ns = time_ns_per_iteration(repeats, [&] {
    for (const char *path : k_paths) {
      auto found = idx.find_by_path(path);
      g_sink += found ? *found + 1 : 0;
    }
  });
  report("cached (fdt_index)", cached_ns / static_cast<double>(k_path_count));
}

} // namespace

int main() {
  // Both engines decode the exact same bytes: libfdt operates on the
  // blob directly, fdt_index via structo::fdt_reader over the same span.
  const void *fdt = qemu_virt_dtb;
  if (fdt_check_full(fdt, qemu_virt_dtb_size) != 0) {
    std::cerr << "fdt_check_full rejected the embedded blob unexpectedly\n";
    return 1;
  }

  auto made_reader = fdt_reader::try_create(span<const std::byte>(qemu_virt_dtb, qemu_virt_dtb_size));
  if (!made_reader) {
    std::cerr << "fdt_reader::try_create failed unexpectedly\n";
    return 1;
  }
  auto reader = std::move(made_reader).value();
  auto idx = build_index(reader);

  std::cout << "fdt_index micro-benchmark: " << idx.node_count()
            << " nodes decoded from the embedded QEMU virt DTB (libfdt vs. structo::fdt_index)\n\n";

  constexpr std::size_t repeats = 20000;
  run_property_lookup_benchmark(fdt, idx, repeats);
  run_enumeration_benchmark(fdt, idx, repeats);
  run_path_translation_benchmark(fdt, idx, repeats);

  std::cout << "\n(sink: " << g_sink << ")\n";
  return 0;
}
