#!/usr/bin/env python3
"""Generate compact FiraGO UI fonts from the complete translation corpus.

The firmware never embeds the upstream TTF files.  This pre-build step extracts
only glyphs reachable from lib/I18n/translations/*.yaml, plus the bounded Arabic
and Hebrew ranges needed by dynamic book/file/author names. Generated bitmaps
use the native 1-bit format, avoiding the 2-bit decompression buffer on ESP32-C3.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path


SCRIPT_PATH = Path(globals().get("__file__", "scripts/build_ui_fonts.py")).resolve()
ROOT = SCRIPT_PATH.parent.parent
TRANSLATIONS_DIR = ROOT / "lib/I18n/translations"
FONT_SCRIPT = ROOT / "lib/EpdFont/scripts/fontconvert.py"
FONT_ID_SCRIPT = ROOT / "lib/EpdFont/scripts/build-font-ids.sh"
FONT_OUTPUT_DIR = ROOT / "lib/EpdFont/builtinFonts"
FONT_CACHE_DIR = ROOT / "lib/EpdFont/scripts/downloaded_fonts/FiraGO"
BUILD_DIR = ROOT / "build/ui-fonts"
CODEPOINTS_PATH = BUILD_DIR / "firago-ui-codepoints.txt"
WORDMARK_CODEPOINTS_PATH = BUILD_DIR / "firago-wordmark-codepoints.txt"
STAMP_PATH = BUILD_DIR / "firago-ui-subset.sha256"
FONT_IDS_PATH = ROOT / "src/fontIds.h"

FIRAGO_REVISION = "5bbcb9d066ab563686ed1de1e6f62eec0148e82d"
FONTS = {
    "medium": {
        "url": (
            "https://raw.githubusercontent.com/bBoxType/FiraGO/"
            f"{FIRAGO_REVISION}/Fonts/FiraGO_TTF_1001/Roman/FiraGO-Medium.ttf"
        ),
        "sha256": "5f753a48c7dff5b7af294e76624febb28c41071a5a65c0fd8a024ea9d1491e8a",
        "filename": "FiraGO-Medium.ttf",
    },
    "semibold": {
        "url": (
            "https://raw.githubusercontent.com/bBoxType/FiraGO/"
            f"{FIRAGO_REVISION}/Fonts/FiraGO_TTF_1001/Roman/FiraGO-SemiBold.ttf"
        ),
        "sha256": "b47f1eaf02deaf16051a897f84f275326476306eb198f1cbceb5b1f5882021b1",
        "filename": "FiraGO-SemiBold.ttf",
    },
}
SIZES = (8, 12, 14, 16, 18)
WORDMARK_SIZE = 36
WORDMARK_TEXT = "InkPoint X"

# Translation YAML is authoritative for every static string.  Dynamic text is
# unbounded, so retain complete core Hebrew/Arabic alphabets and the contextual
# presentation forms emitted by ArabicShaper.  These are still a small subset
# of FiraGO's full multiscript character set.
DYNAMIC_TEXT_RANGES = (
    (0x0590, 0x05FF),  # Hebrew
    (0x0600, 0x06FF),  # Arabic + Persian core
    (0x0750, 0x077F),  # Arabic Supplement
    (0x0870, 0x089F),  # Arabic Extended-B
    (0x08A0, 0x08FF),  # Arabic Extended-A
    (0xFB1D, 0xFB4F),  # Hebrew presentation forms
    (0xFB50, 0xFDFF),  # Arabic presentation forms A
    (0xFE70, 0xFEFC),  # Arabic contextual forms B
)

RUNTIME_CODEPOINTS = {
    0x00A0,  # NBSP
    0x200C,  # ZWNJ
    0x200D,  # ZWJ
    0x200E,  # LRM
    0x200F,  # RLM
    0x2026,  # ellipsis
    0x202A,
    0x202B,
    0x202C,
    0x202D,
    0x202E,
    0x2066,
    0x2067,
    0x2068,
    0x2069,
    0xFFFD,
}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download_verified(url: str, destination: Path, expected_sha256: str) -> None:
    if destination.exists() and sha256_file(destination) == expected_sha256:
        return

    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as temporary:
        temporary_path = Path(temporary.name)
    try:
        print(f"UI fonts: downloading {destination.name}")
        urllib.request.urlretrieve(url, temporary_path)
        actual = sha256_file(temporary_path)
        if actual != expected_sha256:
            raise RuntimeError(
                f"{destination.name}: SHA-256 {actual} != expected {expected_sha256}"
            )
        temporary_path.replace(destination)
    finally:
        temporary_path.unlink(missing_ok=True)


def collect_codepoints() -> set[int]:
    sys.path.insert(0, str(ROOT / "scripts"))
    from gen_i18n import parse_yaml_file

    result = set(range(0x20, 0x7F))
    result.update(RUNTIME_CODEPOINTS)
    for path in sorted(TRANSLATIONS_DIR.glob("*.yaml")):
        locale = parse_yaml_file(str(path))
        for value in locale.values():
            result.update(ord(character) for character in value if character not in "\r\n")
    for start, end in DYNAMIC_TEXT_RANGES:
        result.update(range(start, end + 1))
    return result


def select_converter_python() -> str:
    candidates = [
        ROOT / ".venv/bin/python",
        Path(sys.executable),
        Path(shutil.which("python3") or ""),
    ]
    tried = []
    for candidate in candidates:
        if not candidate or not candidate.exists():
            continue
        candidate_string = str(candidate)
        if candidate_string in tried:
            continue
        tried.append(candidate_string)
        result = subprocess.run(
            [candidate_string, "-c", "import freetype, fontTools"],
            capture_output=True,
            text=True,
        )
        if result.returncode == 0:
            return candidate_string
    raise RuntimeError(
        "UI font generation requires freetype-py and fonttools. "
        "Install lib/EpdFont/scripts/requirements.txt in .venv."
    )


def build_signature(codepoints: set[int], font_paths: dict[str, Path]) -> str:
    digest = hashlib.sha256()
    digest.update(b"firago-ui-subsets-v3\0")
    digest.update(b"sizes=8,12,14,16,18;wordmark=36;mono=1;threshold=6;autohint=1;compressed=0\0")
    for codepoint in sorted(codepoints):
        digest.update(codepoint.to_bytes(4, "little"))
    for path in (SCRIPT_PATH, FONT_SCRIPT, FONT_ID_SCRIPT):
        digest.update(path.read_bytes())
    for weight in sorted(font_paths):
        digest.update(weight.encode("ascii"))
        digest.update(bytes.fromhex(sha256_file(font_paths[weight])))
    return digest.hexdigest()


def write_if_changed(path: Path, content: bytes) -> None:
    if path.exists() and path.read_bytes() == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as temporary:
        temporary.write(content)
        temporary_path = Path(temporary.name)
    temporary_path.replace(path)


def generate_font(
    python: str,
    weight: str,
    size: int,
    source_path: Path,
    *,
    font_name: str | None = None,
    codepoints_path: Path = CODEPOINTS_PATH,
) -> None:
    font_name = font_name or f"firago_{size}_{weight}"
    output_path = FONT_OUTPUT_DIR / f"{font_name}.h"
    command = [
        python,
        str(FONT_SCRIPT),
        font_name,
        str(size),
        str(source_path),
        "--codepoints-file",
        str(codepoints_path),
        "--force-autohint",
        "--pnum",
        "--mono-threshold",
        "6",
    ]
    result = subprocess.run(command, capture_output=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"FiraGO {weight} {size} generation failed:\n"
            f"{result.stderr.decode('utf-8', errors='replace')}"
        )
    write_if_changed(output_path, result.stdout)


def regenerate_font_ids() -> None:
    result = subprocess.run(
        ["bash", str(FONT_ID_SCRIPT)],
        cwd=ROOT,
        capture_output=True,
    )
    if result.returncode != 0:
        raise RuntimeError(
            "Font ID generation failed:\n"
            + result.stderr.decode("utf-8", errors="replace")
        )
    write_if_changed(FONT_IDS_PATH, result.stdout)


def main() -> None:
    font_paths: dict[str, Path] = {}
    for weight, spec in FONTS.items():
        destination = FONT_CACHE_DIR / spec["filename"]
        download_verified(spec["url"], destination, spec["sha256"])
        font_paths[weight] = destination

    codepoints = collect_codepoints()
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    manifest = "".join(f"U+{codepoint:04X}\n" for codepoint in sorted(codepoints))
    write_if_changed(CODEPOINTS_PATH, manifest.encode("utf-8"))
    wordmark_manifest = "".join(f"U+{codepoint:04X}\n" for codepoint in sorted(set(map(ord, WORDMARK_TEXT))))
    write_if_changed(WORDMARK_CODEPOINTS_PATH, wordmark_manifest.encode("utf-8"))

    signature = build_signature(codepoints, font_paths)
    expected_outputs = [
        FONT_OUTPUT_DIR / f"firago_{size}_{weight}.h"
        for size in SIZES
        for weight in FONTS
    ]
    expected_outputs.append(FONT_OUTPUT_DIR / "firago_wordmark_36_semibold.h")
    current_stamp = STAMP_PATH.read_text(encoding="ascii").strip() if STAMP_PATH.exists() else ""
    if current_stamp != signature or not all(path.exists() for path in expected_outputs):
        python = select_converter_python()
        print(
            f"UI fonts: generating {len(expected_outputs)} 1-bit FiraGO subsets "
            f"from {len(codepoints)} requested codepoints"
        )
        for size in SIZES:
            for weight, source_path in font_paths.items():
                generate_font(python, weight, size, source_path)
        generate_font(
            python,
            "semibold",
            WORDMARK_SIZE,
            font_paths["semibold"],
            font_name="firago_wordmark_36_semibold",
            codepoints_path=WORDMARK_CODEPOINTS_PATH,
        )
        write_if_changed(STAMP_PATH, (signature + "\n").encode("ascii"))
    else:
        print("UI fonts: translation subsets are current")

    regenerate_font_ids()


if __name__ == "__main__":
    main()
else:
    try:
        Import("env")
        main()
    except NameError:
        pass
