#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
#
# SPDX-License-Identifier: BSD-2-Clause
"""Converts a TTF/OTF font into a bitmap `Font` header compatible with
structo::hw::framebuffer_console's `Font` trait
(include/structo/hw/framebuffer_console.hpp).

The trait packs every glyph row into a single `std::uint8_t` (bit 7 =
leftmost pixel), so generated glyphs are at most 8 pixels wide; the
height is configurable (8, 16, and similar classic bitmap-font sizes
all work). This is a general-purpose developer tool: point it at any
font file you have the rights to use this way and it will produce a
ready-to-include header -- it is not specific to any one font shipped
with structo.

Requires Pillow (`pip install Pillow`); Pillow's FreeType-backed text
rendering is used to rasterize each glyph.

Example:
    ./scripts/ttf_to_font_header.py \\
        --font /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf \\
        --height 16 \\
        --struct-name dejavu_sans_mono_8x16 \\
        --namespace structo::examples::fonts \\
        --font-name "DejaVu Sans Mono" \\
        --font-license "Bitstream Vera License (permissive; see DejaVu Fonts License)" \\
        --font-source "https://dejavu-fonts.github.io/" \\
        --output examples/fonts/dejavu_sans_mono_8x16_font.hpp
"""

from __future__ import annotations

import argparse
import datetime
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:  # pragma: no cover - environment-dependent
    sys.exit("error: Pillow is required (pip install Pillow)")


def parse_codepoint(value: str) -> int:
    """Parses --first/--last: a single literal character or an int
    (decimal or 0x-prefixed hex)."""
    if len(value) == 1:
        return ord(value)
    return int(value, 0)


def render_glyph_rows(
    font: "ImageFont.FreeTypeFont",
    ch: str,
    width: int,
    height: int,
    threshold: int,
    x_offset: int,
    y_offset: int,
    advance_width: int,
) -> list[int]:
    """Rasterizes one character at the font's natural (monospace)
    advance width and `height`, then resamples down (or up) to the
    requested `width x height` cell -- so every glyph is condensed by
    the same ratio instead of being clipped on one side -- and packs
    each row into a byte with bit 7 as the leftmost pixel."""
    image = Image.new("L", (advance_width, height), 0)
    draw = ImageDraw.Draw(image)
    draw.text((x_offset, y_offset), ch, font=font, fill=255)
    if advance_width != width:
        image = image.resize((width, height), Image.LANCZOS)
    rows = []
    for y in range(height):
        byte = 0
        for x in range(width):
            if image.getpixel((x, y)) >= threshold:
                byte |= 0x80 >> x
        rows.append(byte)
    return rows


def glyph_art(rows: list[int], width: int) -> list[str]:
    """Renders a glyph's bytes as a '#'/'.' ASCII-art comment block."""
    art = []
    for byte in rows:
        line = "".join("#" if byte & (0x80 >> x) else "." for x in range(width))
        art.append(line)
    return art


def char_label(code: int) -> str:
    ch = chr(code)
    if ch == "\\":
        printable = "'\\\\'"
    elif ch == "'":
        printable = "'\\''"
    elif 0x20 < code < 0x7F:
        printable = f"'{ch}'"
    else:
        printable = "(non-printable)"
    return f"0x{code:02X} {printable}"


def format_header(args: argparse.Namespace, first: int, last: int, glyphs: list[list[int]]) -> str:
    width = args.width
    height = args.height
    count = last - first + 1
    now = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")

    lines: list[str] = []
    lines.append("// clang-format off")
    lines.append("//")
    lines.append(f"// GENERATED FILE -- produced by scripts/ttf_to_font_header.py on {now}.")
    lines.append("// Do not hand-edit; re-run the generator against the source font instead.")
    lines.append("//")
    if args.font_name:
        lines.append(f"//   Source font:  {args.font_name}")
    lines.append(f"//   Source file:  {Path(args.font).name}")
    if args.font_source:
        lines.append(f"//   Upstream:     {args.font_source}")
    if args.font_license:
        lines.append(f"//   License:      {args.font_license}")
        lines.append("//   The embedded glyph bitmaps below are a mechanically-rasterized,")
        lines.append("//   low-resolution derivative of that font's outlines; confirm this")
        lines.append("//   license permits this kind of redistribution/embedding before use.")
    lines.append(f"//   Glyph size:   {width}x{height} pixels, bit 7 (MSB) = leftmost pixel")
    lines.append(f"//   Char range:   0x{first:02X}-0x{last:02X}")
    lines.append("// clang-format on")
    lines.append("")
    lines.append("#pragma once")
    lines.append("")
    lines.append(f"/** @file {Path(args.output).name}")
    lines.append(f" * @brief Generated {width}x{height} bitmap `Font` (see")
    lines.append(" * `structo::hw::framebuffer_console`'s `Font` trait in")
    lines.append(" * `structo/hw/framebuffer_console.hpp`) for the font named above.")
    lines.append(" * Produced by `scripts/ttf_to_font_header.py`; see that script's")
    lines.append(" * `--help` to regenerate this file or convert a different font.")
    lines.append(" */")
    lines.append("")
    lines.append("#include <cstddef>")
    lines.append("#include <cstdint>")
    lines.append("")

    namespaces = [n for n in args.namespace.split("::") if n]
    for ns in namespaces:
        lines.append(f"namespace {ns} {{")
    if namespaces:
        lines.append("")

    lines.append(f"/** @brief {width}x{height} bitmap font generated from "
                  f"{args.font_name or Path(args.font).name}. */")
    lines.append(f"struct {args.struct_name} {{")
    lines.append(f"  static constexpr std::size_t glyph_width = {width};")
    lines.append(f"  static constexpr std::size_t glyph_height = {height};")
    lines.append("")
    lines.append("  [[nodiscard]] static const std::uint8_t *glyph_bitmap(char ch) noexcept {")
    blank_row = ", ".join("0" for _ in range(height))
    lines.append(f"    static constexpr std::uint8_t blank[{height}] = {{{blank_row}}};")
    lines.append(f"    static constexpr std::uint8_t glyphs[{count}][{height}] = {{")
    for code in range(first, last + 1):
        rows = glyphs[code - first]
        art = glyph_art(rows, width)
        row_values = ", ".join(f"0x{b:02X}" for b in rows)
        lines.append(f"        // {char_label(code)}")
        for art_line in art:
            lines.append(f"        //   |{art_line}|")
        lines.append(f"        {{{row_values}}},")
    lines.append("    };")
    lines.append(f"    auto code = static_cast<unsigned char>(ch);")
    lines.append(f"    if (code < 0x{first:02X} || code > 0x{last:02X}) {{")
    lines.append("      return blank;")
    lines.append("    }")
    lines.append(f"    return glyphs[code - 0x{first:02X}];")
    lines.append("  }")
    lines.append("};")

    if namespaces:
        lines.append("")
        for ns in reversed(namespaces):
            lines.append(f"}} // namespace {ns}")

    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Convert a TTF/OTF font into a structo-compatible 8xN bitmap Font header.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--font", required=True, help="Path to the source .ttf/.otf file")
    parser.add_argument("--output", required=True, help="Path to write the generated header to")
    parser.add_argument("--width", type=int, default=8,
                         help="Glyph width in pixels, 1-8 (the Font trait packs one row per "
                              "byte); default 8")
    parser.add_argument("--height", type=int, default=16,
                         help="Glyph height in pixels; default 16")
    parser.add_argument("--first", default="0x20",
                         help="First character to render (literal char or int); default 0x20")
    parser.add_argument("--last", default="0x7E",
                         help="Last character to render (literal char or int); default 0x7E")
    parser.add_argument("--threshold", type=int, default=128,
                         help="Coverage threshold (0-255) above which a pixel is considered set; "
                              "default 128")
    parser.add_argument("--x-offset", type=int, default=0,
                         help="Horizontal pixel offset applied before rasterizing each glyph")
    parser.add_argument("--y-offset", type=int, default=0,
                         help="Vertical pixel offset applied before rasterizing each glyph "
                              "(use to align baseline within the cell)")
    parser.add_argument("--struct-name", default="generated_font",
                         help="Name of the generated C++ struct; default generated_font")
    parser.add_argument("--namespace", default="",
                         help="'::'-separated namespace to wrap the struct in, e.g. "
                              "structo::examples::fonts; default none")
    parser.add_argument("--font-name", default=None, help="Human-readable font name for the "
                         "generated file's doc comment")
    parser.add_argument("--font-license", default=None, help="Short license description for the "
                         "generated file's doc comment, e.g. 'MIT' or 'SIL OFL 1.1'")
    parser.add_argument("--font-source", default=None, help="Upstream URL for the font, for "
                         "attribution in the generated file's doc comment")
    args = parser.parse_args()

    if not (1 <= args.width <= 8):
        parser.error("--width must be between 1 and 8 (one row = one uint8_t, MSB = leftmost "
                      "pixel)")
    if args.height < 1:
        parser.error("--height must be >= 1")
    if not (0 <= args.threshold <= 255):
        parser.error("--threshold must be between 0 and 255")

    first = parse_codepoint(args.first)
    last = parse_codepoint(args.last)
    if first > last:
        parser.error("--first must be <= --last")
    if first < 0 or last > 0xFF:
        parser.error("--first/--last must be within 0x00-0xFF (char is one byte)")

    pil_font = ImageFont.truetype(args.font, args.height)
    # Use the font's own (monospace) advance width as the natural glyph
    # cell, so every character is condensed/expanded to the requested
    # --width by the same ratio instead of being clipped on one side.
    advance_width = max(1, round(pil_font.getlength("M")))

    glyphs = []
    for code in range(first, last + 1):
        rows = render_glyph_rows(pil_font, chr(code), args.width, args.height, args.threshold,
                                  args.x_offset, args.y_offset, advance_width)
        glyphs.append(rows)

    header = format_header(args, first, last, glyphs)

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(header, encoding="utf-8")
    print(f"wrote {output_path} ({last - first + 1} glyphs, {args.width}x{args.height})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
