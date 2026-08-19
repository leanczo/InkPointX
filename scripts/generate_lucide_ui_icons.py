#!/usr/bin/env python3
"""Generate 1-bit firmware icons from a pinned Lucide release.

Sizes must be multiples of 8: EInkDisplay::drawImageTransparent derives the source
stride as width / 8 with integer division, so any other width desynchronises the
rows and renders noise.
"""

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
    ("LucideBookOpen32", "book-open", 32),
    ("LucideFolder32", "folder", 32),
    ("LucideImage32", "image", 32),
    ("LucideBookmark32", "bookmark", 32),
    ("LucideStar32", "star", 32),
    ("LucideWifi32", "wifi", 32),
    ("LucideLibrary32", "library-big", 32),
    ("LucideHotspot32", "radio-tower", 32),
    ("LucideInterface32", "sliders-horizontal", 32),
    ("LucidePower32", "battery-charging", 32),
    ("LucideReading32", "book-open-text", 32),
    ("LucideControls32", "toggle-left", 32),
    ("LucideFiles32", "folder-open", 32),
    ("LucideNetwork32", "refresh-cw", 32),
    ("LucideSystem32", "info", 32),
    ("LucideFileText32", "file-text", 32),
    ("LucideClock32", "clock", 32),
    ("LucideSend32", "send", 32),
    ("LucideSettings32", "settings", 32),
    ("LucidePage32", "file-digit", 32),
    ("LucideChapters32", "list-tree", 32),
    ("LucideDictionary32", "book-a", 32),
    ("LucideFootnotes32", "notebook-tabs", 32),
    ("LucideStats32", "chart-no-axes-column-increasing", 32),
    ("LucideRotate32", "rotate-cw", 32),
    ("LucideAutoTurn32", "timer", 32),
    ("LucideQr32", "qr-code", 32),
    ("LucideHome32", "house", 32),
    ("LucideTrash32", "trash-2", 32),
    ("LucideChevronLeft24", "chevron-left", 24),
    ("LucideChevronRight24", "chevron-right", 24),
    ("LucideCheck24", "check", 24),
    ("LucideStar24", "star", 24),
    ("LucideWorm32", "worm", 32),
    ("LucideCalculator32", "calculator", 32),
    ("LucideDices32", "dices", 32),
    ("LucideGoal32", "goal", 32),
    ("LucideCar32", "car", 32),
    ("LucideGrid3x332", "grid-3x3", 32),
    ("LucideGrid2x232", "grid-2x2", 32),
    ("LucideBomb32", "bomb", 32),
    ("LucideLightbulb32", "lightbulb", 32),
    ("LucideHash32", "hash", 32),
    ("LucideCalendarDays32", "calendar-days", 32),
    ("LucideBell32", "bell", 32),
    ("LucideCloudSun32", "cloud-sun", 32),
    ("LucideMapPin32", "map-pin", 32),
    ("LucideGlobe32", "globe", 32),
    ("LucideFlame32", "flame", 32),
    ("LucideRss32", "rss", 32),
    ("LucideSearch32", "search", 32),
    ("LucideHistory32", "history", 32),
    ("LucideCircleDot32", "circle-dot", 32),
    ("LucideActivity32", "activity", 32),
    ("LucideClapperboard32", "clapperboard", 32),
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
    render_svg_pure_python(svg, png, size)


def render_svg_pure_python(svg: Path, png: Path, size: int) -> None:
    """Fallback renderer for hosts without sips/rsvg-convert (e.g. Windows CI/dev
    machines). Needs `pip install svglib reportlab pymupdf` - none of which need a
    system-installed native library, unlike cairosvg's libcairo dependency."""
    try:
        from svglib.svglib import svg2rlg
        from reportlab.graphics import renderPDF
        import fitz  # PyMuPDF
    except ImportError as exc:
        raise RuntimeError(
            "Install librsvg (rsvg-convert), run the generator on macOS with sips, or "
            "`pip install svglib reportlab pymupdf` for the pure-Python fallback"
        ) from exc

    # Lucide SVGs set stroke="currentColor" at the root, relying on CSS context a
    # standalone SVG parser doesn't have; svglib silently renders it as an
    # invisible stroke instead of erroring. Pin it to black before parsing.
    fixed_svg = svg.with_name(svg.stem + "-fixed.svg")
    fixed_svg.write_text(svg.read_text(encoding="utf-8").replace("currentColor", "#000000"), encoding="utf-8")

    drawing = svg2rlg(str(fixed_svg))
    pdf_path = svg.with_suffix(".pdf")
    renderPDF.drawToFile(drawing, str(pdf_path))

    doc = fitz.open(str(pdf_path))
    page = doc[0]
    zoom = size / page.rect.width
    pix = page.get_pixmap(matrix=fitz.Matrix(zoom, zoom), alpha=True)
    pix.save(str(png))


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
            url = f"https://cdn.jsdelivr.net/npm/lucide-static@{LUCIDE_VERSION}/icons/{slug}.svg"
            # A stalled asset host must not leave local builds hanging forever.
            with urllib.request.urlopen(url, timeout=20) as response:
                svg.write_bytes(response.read())
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
