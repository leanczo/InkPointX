#!/usr/bin/env python3
"""Generate 1-bit firmware icons from a pinned Lucide release."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import tempfile
import urllib.request
from pathlib import Path

from PIL import Image


LUCIDE_VERSION = "1.27.0"
ICONS = (
    ("LucideBookOpen24", "book-open", 24),
    ("LucideFolder24", "folder", 24),
    ("LucideImage24", "image", 24),
    ("LucideBookmark24", "bookmark", 24),
    ("LucideStar24", "star", 24),
    ("LucideWifi24", "wifi", 24),
    ("LucideLibrary24", "library-big", 24),
    ("LucideHotspot24", "radio-tower", 24),
    ("LucideInterface24", "sliders-horizontal", 24),
    ("LucidePower24", "battery-charging", 24),
    ("LucideReading24", "book-open-text", 24),
    ("LucideControls24", "toggle-left", 24),
    ("LucideFiles24", "folder-open", 24),
    ("LucideNetwork24", "refresh-cw", 24),
    ("LucideSystem24", "info", 24),
    ("LucideFileText24", "file-text", 24),
    ("LucideClock24", "clock", 24),
    ("LucideSend24", "send", 24),
    ("LucideSettings24", "settings", 24),
    ("LucideChevronLeft16", "chevron-left", 16),
    ("LucideChevronRight16", "chevron-right", 16),
    ("LucideCheck16", "check", 16),
    ("LucideStar16", "star", 16),
)


def render_svg(svg: Path, png: Path, size: int) -> None:
    sips = shutil.which("sips")
    rsvg_convert = shutil.which("rsvg-convert")
    if sips:
        subprocess.run(
            [sips, "-z", str(size), str(size), "-s", "format", "png", str(svg), "--out", str(png)],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        return
    if rsvg_convert:
        subprocess.run(
            [rsvg_convert, "-w", str(size), "-h", str(size), "-o", str(png), str(svg)],
            check=True,
        )
        return
    raise RuntimeError("Install librsvg (rsvg-convert) or run the generator on macOS with sips")


def packed_bitmap(image: Image.Image, size: int) -> list[int]:
    rgba = image.convert("RGBA")
    mono = Image.new("1", (size, size), 1)
    source = rgba.load()
    target = mono.load()
    for y in range(size):
        for x in range(size):
            red, green, blue, alpha = source[x, y]
            target[x, y] = 0 if alpha >= 96 and min(red, green, blue) < 160 else 1

    # GfxRenderer::drawIcon writes to the native landscape framebuffer.
    mono = mono.rotate(90, expand=False)
    values: list[int] = []
    for y in range(size):
        for x in range(0, size, 8):
            value = 0
            for bit in range(8):
                if x + bit < size and mono.getpixel((x + bit, y)):
                    value |= 1 << (7 - bit)
            values.append(value)
    return values


def format_array(name: str, values: list[int]) -> str:
    rows = []
    for start in range(0, len(values), 14):
        rows.append("    " + ", ".join(f"0x{value:02X}" for value in values[start : start + 14]) + ",")
    return f"inline constexpr uint8_t {name}[] = {{\n" + "\n".join(rows) + "\n};"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "src/components/icons/lucide_ui.h",
    )
    args = parser.parse_args()

    arrays = []
    with tempfile.TemporaryDirectory() as temporary_directory:
        temp = Path(temporary_directory)
        for name, slug, size in ICONS:
            svg = temp / f"{slug}.svg"
            png = temp / f"{slug}-{size}.png"
            url = f"https://unpkg.com/lucide-static@{LUCIDE_VERSION}/icons/{slug}.svg"
            urllib.request.urlretrieve(url, svg)
            render_svg(svg, png, size)
            with Image.open(png) as image:
                arrays.append(format_array(name, packed_bitmap(image, size)))

    content = f"""#pragma once
#include <cstdint>

// Generated from Lucide {LUCIDE_VERSION} (ISC) by scripts/generate_lucide_ui_icons.py.
// The 2 px source strokes are rasterized and thresholded for the X4's 1-bit panel.

""" + "\n\n".join(arrays) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(content, encoding="utf-8")
    print(f"Wrote {args.output}")


if __name__ == "__main__":
    main()
