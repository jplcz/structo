// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file all_fonts.hpp
 * @brief Registry of the bitmap fonts shipped with the demos, with runtime selection by name.
 *
 * `framebuffer_console` takes its font as a compile-time `Font` type, so "choosing a font at run time" means
 * instantiating the demo once per font and dispatching on the name. `select_font` does that dispatch and also
 * handles the command line, so every demo supports the same options:
 *
 * ```
 * demo --font spleen        # or --font=spleen, or the STRUCTO_FONT environment variable
 * demo --list-fonts
 * ```
 *
 * Usage in a demo:
 *
 * ```cpp
 * template <class Font> int run() { ... framebuffer_console<rgba8888, Font>::try_create(...) ... }
 *
 * int main(int argc, char **argv) {
 *   // The visitor is called once with a `font_tag<Font>`; its return value becomes main()'s.
 *   return structo::examples::fonts::select_font(argc, argv, [](auto tag) {
 *     return run<typename decltype(tag)::type>();
 *   });
 * }
 * ```
 */

#include "dejavu_sans_mono_8x16_font.hpp"
#include "spleen_8x16_font.hpp"
#include "terminus_8x14_font.hpp"
#include "terminus_8x16_font.hpp"
#include "terminus_bold_8x16_font.hpp"

#include <reloco/array.hpp>
#include <reloco/lifetime.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace structo {
namespace examples {
namespace fonts {

/** @brief Carries a font type through a generic visitor. */
template <typename Font> struct font_tag {
  using type = Font;
};

/** @brief One selectable font: command-line name and a one-line description (including its license). */
struct font_entry {
  const char *name;
  const char *description;
};

inline constexpr reloco::array<font_entry, 5> font_list{{
    {"terminus", "Terminus 8x16 (SIL OFL 1.1) - crisp classic console font, the default"},
    {"terminus-bold", "Terminus Bold 8x16 (SIL OFL 1.1)"},
    {"terminus-14", "Terminus 8x14 (SIL OFL 1.1) - more rows per screen"},
    {"spleen", "Spleen 8x16 (BSD-2-Clause)"},
    {"dejavu", "DejaVu Sans Mono 8x16 (Bitstream Vera License) - rasterized from outlines, softer"},
}};

inline constexpr const char *default_font_name = "terminus";

// Printing and argv parsing sit on the libc/process boundary, so unsafe-buffer diagnostics are suppressed here.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

/** @brief Prints the available fonts to stdout. */
inline void print_font_list() {
  std::printf("Available fonts (--font NAME or STRUCTO_FONT=NAME):\n");
  for (const font_entry &e : font_list)
    std::printf("  %-14s %s\n", e.name, e.description);
}

/** @brief Calls `visitor(font_tag<Font>{})` for the font called @p name; returns false if there is none. */
template <typename Visitor> bool visit_font(std::string_view name, Visitor &&visitor, int &result) {
  if (name == "terminus") {
    result = visitor(font_tag<terminus_8x16>{});
  } else if (name == "terminus-bold") {
    result = visitor(font_tag<terminus_bold_8x16>{});
  } else if (name == "terminus-14") {
    result = visitor(font_tag<terminus_8x14>{});
  } else if (name == "spleen") {
    result = visitor(font_tag<spleen_8x16>{});
  } else if (name == "dejavu") {
    result = visitor(font_tag<dejavu_sans_mono_8x16>{});
  } else {
    return false;
  }
  return true;
}

/**
 * @brief Parses `--font`, `--list-fonts`, `--help` and `STRUCTO_FONT`, then runs @p visitor with the chosen font.
 * @return the visitor's result, or a non-zero exit code for bad arguments (`0` for `--list-fonts`/`--help`).
 */
template <typename Visitor> int select_font(int argc, char **argv, Visitor &&visitor) {
  const char *chosen = std::getenv("STRUCTO_FONT");
  if (chosen == nullptr || *chosen == '\0')
    chosen = default_font_name;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--list-fonts" || arg == "--help" || arg == "-h") {
      print_font_list();
      return 0;
    }
    if (arg == "--font" && i + 1 < argc) {
      chosen = argv[++i];
    } else if (arg.substr(0, 7) == "--font=") {
      chosen = argv[i] + 7;
    } else {
      std::fprintf(stderr, "unknown argument '%s' (try --list-fonts)\n", argv[i]);
      return 2;
    }
  }
  int result = 0;
  if (!visit_font(chosen, visitor, result)) {
    std::fprintf(stderr, "unknown font '%s'\n", chosen);
    print_font_list();
    return 2;
  }
  return result;
}

RELOCO_END_UNSAFE_BUFFER_USAGE

} // namespace fonts
} // namespace examples
} // namespace structo
