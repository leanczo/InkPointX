#!/usr/bin/env python3
"""Generate the home statistics icons from LVGL's bundled Font Awesome font."""

import argparse
import tempfile
from pathlib import Path

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont


ICONS = (
    ("LvglHomeBookOpenIcon", 0xF518),
    ("LvglHomeClockIcon", 0xF017),
    ("LvglHomeBookmarkIcon", 0xF02E),
)
ICON_SIZE = 32
FONT_SIZE = 25


def render_icon(font: ImageFont.FreeTypeFont, codepoint: int) -> Image.Image:
    image = Image.new("L", (ICON_SIZE, ICON_SIZE), 255)
    draw = ImageDraw.Draw(image)
    glyph = chr(codepoint)
    left, top, right, bottom = draw.textbbox((0, 0), glyph, font=font)
    x = (ICON_SIZE - (right - left)) // 2 - left
    y = (ICON_SIZE - (bottom - top)) // 2 - top
    draw.text((x, y), glyph, font=font, fill=0)

    # GfxRenderer::drawIcon writes directly to the landscape-native panel.
    # Store the square icon counter-clockwise so it is upright in portrait.
    return image.rotate(90, expand=False)


def pack_icon(image: Image.Image) -> list[int]:
    packed = []
    for y in range(ICON_SIZE):
        for x in range(0, ICON_SIZE, 8):
            byte = 0
            for bit_index in range(8):
                source_x = x + bit_index
                if source_x < ICON_SIZE and image.getpixel((source_x, y)) >= 128:
                    byte |= 1 << (7 - bit_index)
            packed.append(byte)
    return packed


def format_array(name: str, values: list[int]) -> str:
    lines = []
    for offset in range(0, len(values), 14):
        chunk = ", ".join(f"0x{value:02X}" for value in values[offset : offset + 14])
        lines.append(f"    {chunk},")
    return f"inline constexpr uint8_t {name}[] = {{\n" + "\n".join(lines) + "\n};"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("font", type=Path, help="LVGL FontAwesome5-Solid+Brands+Regular.woff")
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "src/components/icons/lvgl_home_stats.h",
    )
    args = parser.parse_args()

    with tempfile.NamedTemporaryFile(suffix=".ttf") as temporary_ttf:
        source_font = TTFont(args.font)
        source_font.flavor = None
        source_font.save(temporary_ttf.name)
        font = ImageFont.truetype(temporary_ttf.name, FONT_SIZE)
        arrays = [format_array(name, pack_icon(render_icon(font, codepoint))) for name, codepoint in ICONS]

    output = """#pragma once
#include <cstdint>

// Generated from Font Awesome 5 Free as bundled with LVGL.
// Glyphs: book-open (F518), clock (F017), bookmark (F02E).
// See docs/third-party-notices.md for attribution and license information.
// Logical size: 28x28 pixels.

""" + "\n\n".join(arrays) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(output, encoding="utf-8")
    print(f"Wrote {args.output}")


if __name__ == "__main__":
    main()
