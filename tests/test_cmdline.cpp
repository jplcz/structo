// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/bootldr/cmdline.hpp>

using namespace structo::bootldr;

namespace {

struct split_result {
  reloco::result<std::size_t> n{std::size_t{0}};
  char buf[128];
  char *argv[8];
};

void split(split_result &s, const char *text, std::size_t max_args = 8) {
  std::size_t len = 0;
  while (text[len] != '\0') {
    s.buf[len] = text[len];
    ++len;
  }
  s.buf[len] = '\0';
  s.n = split_command_line(s.buf, len, reloco::span<char *>(s.argv, max_args));
}

std::uint64_t num(reloco::string_view text) {
  auto r = parse_number(text);
  EXPECT_TRUE(r.has_value());
  return r ? *r : 0;
}

bool is(const char *a, const char *b) {
  while (*a && *a == *b) {
    ++a;
    ++b;
  }
  return *a == *b;
}

} // namespace

TEST(Cmdline, SplitsOnBlanks) {
  split_result s;
  split(s, "  load \t kernel   0x80000 ");
  ASSERT_TRUE(s.n.has_value());
  ASSERT_EQ(*s.n, 3u);
  EXPECT_TRUE(is(s.argv[0], "load"));
  EXPECT_TRUE(is(s.argv[1], "kernel"));
  EXPECT_TRUE(is(s.argv[2], "0x80000"));
}

TEST(Cmdline, EmptyAndCommentLines) {
  split_result s;
  split(s, "   ");
  ASSERT_TRUE(s.n.has_value());
  EXPECT_EQ(*s.n, 0u);
  split(s, "boot # not this");
  ASSERT_TRUE(s.n.has_value());
  EXPECT_EQ(*s.n, 1u);
  split(s, "# only a comment");
  ASSERT_TRUE(s.n.has_value());
  EXPECT_EQ(*s.n, 0u);
}

TEST(Cmdline, QuotesAndEscapes) {
  split_result s;
  split(s, "a \"b c\" 'd \\e' f\\ g \"q\\\"r\" \"\" x\"y z\"w");
  ASSERT_TRUE(s.n.has_value());
  ASSERT_EQ(*s.n, 7u);
  EXPECT_TRUE(is(s.argv[0], "a"));
  EXPECT_TRUE(is(s.argv[1], "b c"));
  EXPECT_TRUE(is(s.argv[2], "d \\e")); // single quotes are literal
  EXPECT_TRUE(is(s.argv[3], "f g"));
  EXPECT_TRUE(is(s.argv[4], "q\"r"));
  EXPECT_TRUE(is(s.argv[5], ""));
  EXPECT_TRUE(is(s.argv[6], "xy zw"));
}

TEST(Cmdline, Errors) {
  split_result s;
  split(s, "a \"unterminated");
  ASSERT_FALSE(s.n.has_value());
  EXPECT_EQ(s.n.error(), reloco::error::invalid_argument);
  split(s, "a b\\");
  ASSERT_FALSE(s.n.has_value());
  EXPECT_EQ(s.n.error(), reloco::error::invalid_argument);
  split(s, "a b c", 2);
  ASSERT_FALSE(s.n.has_value());
  EXPECT_EQ(s.n.error(), reloco::error::out_of_range);
}

TEST(Cmdline, ParseNumber) {
  EXPECT_EQ(num("0"), 0u);
  EXPECT_EQ(num("1234"), 1234u);
  EXPECT_EQ(num("0x80000"), 0x80000u);
  EXPECT_EQ(num("0XfF"), 255u);
  EXPECT_EQ(num("0b101"), 5u);
  EXPECT_EQ(num("017"), 15u);
  EXPECT_EQ(num("0xffffffffffffffff"), UINT64_MAX);
  EXPECT_EQ(parse_number("").error(), reloco::error::invalid_argument);
  EXPECT_EQ(parse_number("0x").error(), reloco::error::invalid_argument);
  EXPECT_EQ(parse_number("12a").error(), reloco::error::invalid_argument);
  EXPECT_EQ(parse_number("0b12").error(), reloco::error::invalid_argument);
  EXPECT_EQ(parse_number("089").error(), reloco::error::invalid_argument);
  EXPECT_EQ(parse_number("0x10000000000000000").error(), reloco::error::out_of_range);
}
