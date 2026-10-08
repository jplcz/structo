// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

// Shared helpers for the network tests. Tests must not use std:: containers, so
// byte buffers are reloco::vector and these helpers hide the fallible API.

#pragma once

#include <gtest/gtest.h>
#include <reloco/span.hpp>
#include <reloco/vector.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include <reloco/lifetime.hpp>

// Test fixtures index raw buffers freely; bounds are checked by the assertions.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

namespace net_test {

using bytes = reloco::vector<std::uint8_t>;
using byte_span = reloco::span<const std::uint8_t>;

namespace detail {

// Allocation failure in a test is a fatal setup problem.
inline void require(const reloco::result<void> &r) {
  if (!r) {
    ADD_FAILURE() << "net_test: allocation failed";
    std::abort();
  }
}

} // namespace detail

// vector::data() asserts on an empty vector, so empty buffers need an explicit span.
inline byte_span as_span(const bytes &b) { return b.empty() ? byte_span() : byte_span(b.data(), b.size()); }

inline void append(bytes &b, byte_span s) {
  detail::require(b.try_reserve(b.size() + s.size()));
  for (std::size_t i = 0; i < s.size(); ++i)
    detail::require(b.try_push_back(s[i]));
}

inline void push(bytes &b, std::uint8_t v) { detail::require(b.try_push_back(v)); }

inline bytes make_bytes(byte_span s) {
  bytes b;
  append(b, s);
  return b;
}

inline bytes make_bytes(std::initializer_list<std::uint8_t> il) {
  return make_bytes(byte_span(il.begin(), il.size()));
}

inline bytes make_bytes(std::size_t n, std::uint8_t fill) {
  bytes b;
  detail::require(b.try_resize(n, fill));
  return b;
}

inline ::testing::AssertionResult bytes_equal(byte_span a, byte_span b) {
  if (a.size() == b.size()) {
    bool same = true;
    for (std::size_t i = 0; i < a.size() && same; ++i)
      same = a[i] == b[i];
    if (same)
      return ::testing::AssertionSuccess();
  }
  auto fail = ::testing::AssertionFailure();
  fail << "byte sequences differ\n";
  const byte_span both[2] = {a, b};
  const char *names[2] = {"  lhs (", "  rhs ("};
  for (int k = 0; k < 2; ++k) {
    fail << names[k] << both[k].size() << " bytes):";
    for (std::size_t i = 0; i < both[k].size(); ++i) {
      char hex[4];
      std::snprintf(hex, sizeof hex, "%02x", static_cast<unsigned>(both[k][i]));
      fail << ' ' << hex;
    }
    fail << '\n';
  }
  return fail;
}

// Convenience for comparing against temporaries (a span cannot bind to an rvalue container).
inline ::testing::AssertionResult bytes_equal(const bytes &a, const bytes &b) {
  return bytes_equal(as_span(a), as_span(b));
}

} // namespace net_test

RELOCO_END_UNSAFE_BUFFER_USAGE
