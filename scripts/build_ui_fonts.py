#!/usr/bin/env python3
"""Generate the compact UI font subsets from the complete translation corpus.

Inter supplies the interface type. It is a variable font, and fontconvert.py takes
the default instance of whatever it is handed, so each weight is instanced here
first with fontTools: the wght axis gives Medium and SemiBold, and the opsz axis is
set to the raster size so every size gets Inter's own optical adjustments rather
than one outline scaled up and down.

Inter has no Hebrew, Arabic, or Korean, which the firmware needs for its system
locales. fontconvert.py accepts a font stack ordered by descending priority, so
the matching Noto faces fill exactly the code points Inter is missing and
nothing else.

The firmware never embeds the upstream TTF files.  This pre-build step extracts
only glyphs reachable from lib/I18n/translations/*.yaml, plus the bounded Arabic
and Hebrew ranges needed by dynamic book/file/author names. Generated bitmaps
use the native 1-bit format, avoiding the 2-bit decompression buffer on ESP32-C3.
"""

from __future__ import annotations

import hashlib
import os
import re
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
FONT_CACHE_DIR = ROOT / "lib/EpdFont/scripts/downloaded_fonts/Inter"
HEBREW_SOURCE_DIR = ROOT / "lib/EpdFont/builtinFonts/source/NotoSansHebrew"
BUILD_DIR = ROOT / "build/ui-fonts"
CODEPOINTS_PATH = BUILD_DIR / "ui-codepoints.txt"
SCRIPT_SMALL_CODEPOINTS_PATH = BUILD_DIR / "ui-script-small-codepoints.txt"
NON_KOREAN_CODEPOINTS_PATH = BUILD_DIR / "ui-non-korean-codepoints.txt"
STAMP_PATH = BUILD_DIR / "ui-subset.sha256"
FONT_IDS_PATH = ROOT / "src/fontIds.h"

# Pinned google/fonts revision: both files are OFL 1.1 and may be embedded and
# redistributed, which is what this firmware does with the rasterized result.
GOOGLE_FONTS_REVISION = "7ff85c87f93ea6cca5f41c69f2e4edcb90240f26"

INTER_SOURCE = {
    "url": (
        "https://raw.githubusercontent.com/google/fonts/"
        f"{GOOGLE_FONTS_REVISION}/ofl/inter/Inter%5Bopsz%2Cwght%5D.ttf"
    ),
    "sha256": "29160a80ff49ddcab2c97711247e08b1fab27a484a329ce8b813d820dc559031",
    "filename": "Inter[opsz,wght].ttf",
}

# Handwritten face for the firmware's accent voice. Caveat is OFL, covers Latin plus
# the full Cyrillic range our locales need (including Kazakh and Ukrainian
# extensions), and at the 600 weight its strokes survive 1-bit rendering
# without anti-aliasing.
CAVEAT_SOURCE = {
    "url": (
        "https://raw.githubusercontent.com/google/fonts/"
        f"{GOOGLE_FONTS_REVISION}/ofl/caveat/Caveat%5Bwght%5D.ttf"
    ),
    "sha256": "0bdb6b660482d31531b3945849fba5916b3ef8695da7024a9e6b9ee3c4157988",
    "filename": "Caveat[wght].ttf",
}

ARABIC_FALLBACK_SOURCE = {
    "url": (
        "https://raw.githubusercontent.com/google/fonts/"
        f"{GOOGLE_FONTS_REVISION}/ofl/notonaskharabic/NotoNaskhArabic%5Bwght%5D.ttf"
    ),
    "sha256": "67b5a525a661b607971fbd3f96a81b89d3a768e74534fca84f18ac97e6fab72f",
    "filename": "NotoNaskhArabic[wght].ttf",
}

KOREAN_FALLBACK_SOURCE = {
    "url": (
        "https://raw.githubusercontent.com/google/fonts/"
        f"{GOOGLE_FONTS_REVISION}/ofl/notosanskr/NotoSansKR%5Bwght%5D.ttf"
    ),
    "sha256": "194018e6b2b293a7964f037b25c0249ce1418bc9ab3c971060a03aa57861e252",
    "filename": "NotoSansKR[wght].ttf",
}

# Interface weights, as Inter wght axis values. Medium carries body text; SemiBold
# is reserved for headings, selection and emphasis.
WEIGHTS = {"medium": 500, "semibold": 600}

# Hebrew fallback ships in-tree. Noto Sans Hebrew has no variable axis, so the two
# static weights are paired with the two Inter weights.
HEBREW_FALLBACKS = {"medium": "NotoSansHebrew-Regular.ttf", "semibold": "NotoSansHebrew-Bold.ttf"}

SIZES = (8, 10, 12, 14, 16)

# Invisible characters the renderer and bidi layer emit at runtime; they never
# appear in a translation file but must still have a glyph slot.
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

# Translation YAML is authoritative for every static string. Dynamic text is
# unbounded, so retain the complete core Hebrew and Arabic alphabets a book, author
# or file name can contain.
DYNAMIC_TEXT_RANGES = (
    (0x0590, 0x05FF),  # Hebrew
    (0x0600, 0x06FF),  # Arabic + Persian core
    (0x0750, 0x077F),  # Arabic Supplement
)

# The Arabic presentation forms are NOT requested as blocks. ArabicShaper can only
# ever emit the forms named in its own substitution table -- 106 of them -- while
# FB50-FDFF alone holds over two thousand decorative ligatures. Asking for the whole
# block was harmless while the UI font simply had no glyph for most of it, but a
# Naskh fallback resolves them all, which more than doubled every subset. Parse the
# shaper instead, so this stays exact and cannot drift from the C++ table.
ARABIC_SHAPER_SOURCE = ROOT / "lib/MiniBidi/ArabicShaper.cpp"


def arabic_presentation_forms() -> set[int]:
    text = ARABIC_SHAPER_SOURCE.read_text(encoding="utf-8")
    forms = {int(value, 16) for value in re.findall(r"0x(F[BE][0-9A-Fa-f]{2})", text)}
    if not forms:
        raise RuntimeError(f"No Arabic presentation forms found in {ARABIC_SHAPER_SOURCE}")
    return forms


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
    result.update(arabic_presentation_forms())
    return result


def is_rtl_script_codepoint(codepoint: int) -> bool:
    """Whether Home should use the compact structural fallback for this glyph.

    Caveat has no Hebrew or Arabic outlines. Embedding a second copy of the
    large Noto RTL fallback just to make the author line two points smaller
    costs almost half a megabyte of the fixed OTA slot. The regular 16 pt UI
    face already carries those glyphs, so the built-in small accent subset can
    stay focused on the Latin/Cyrillic scripts Caveat actually draws.
    """
    return (
        0x0590 <= codepoint <= 0x08FF
        or 0xFB1D <= codepoint <= 0xFDFF
        or 0xFE70 <= codepoint <= 0xFEFF
    )


def is_korean_codepoint(codepoint: int) -> bool:
    return (
        0x1100 <= codepoint <= 0x11FF
        or 0x3130 <= codepoint <= 0x318F
        or 0xA960 <= codepoint <= 0xA97F
        or 0xAC00 <= codepoint <= 0xD7AF
        or 0xD7B0 <= codepoint <= 0xD7FF
    )


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
    digest.update(b"inter-ui-subsets-v3\0")
    digest.update(b"sizes=8,10,12,14,16;script=16ltr-no-ko,20full-no-ko-w600;semibold=no-ko;"
                b"wordmark=none;mono=1;threshold=6;autohint=1;compressed=0;"
                b"wght=500/600;opsz=size;fallback=hebrew+arabic+korean\0")
    for codepoint in sorted(codepoints):
        digest.update(codepoint.to_bytes(4, "little"))
    for path in (SCRIPT_PATH, FONT_SCRIPT, FONT_ID_SCRIPT, ARABIC_SHAPER_SOURCE):
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


def instance_variable_font(source_path: Path, weight_value: int, size: int, destination: Path) -> None:
    """Freeze a variable font into a static face for one weight and size.

    fontconvert.py hands the file straight to FreeType, which renders a variable
    font's default instance. Without this both weights would come out Regular and
    the Medium/SemiBold distinction the interface relies on would silently
    disappear. When available, opsz is set to the raster size (clamped to the
    axis range) so each size gets the font's intended optical treatment.
    """
    from fontTools.ttLib import TTFont
    from fontTools.varLib import instancer

    font = TTFont(str(source_path))
    axes = {axis.axisTag: axis for axis in font["fvar"].axes}
    coordinates = {"wght": weight_value}
    if "opsz" in axes:
        optical = axes["opsz"]
        coordinates["opsz"] = max(optical.minValue, min(optical.maxValue, float(size)))
    instancer.instantiateVariableFont(font, coordinates, inplace=True, updateFontNames=False)
    destination.parent.mkdir(parents=True, exist_ok=True)
    font.save(str(destination))


def generate_font(
    python: str,
    weight: str,
    size: int,
    fontstack: list[Path],
    *,
    font_name: str | None = None,
    codepoints_path: Path = CODEPOINTS_PATH,
) -> None:
    font_name = font_name or f"ui_{size}_{weight}"
    output_path = FONT_OUTPUT_DIR / f"{font_name}.h"
    command = [
        python,
        str(FONT_SCRIPT),
        font_name,
        str(size),
        # Ordered by descending priority: Inter first, then the scripts it lacks.
        *(str(path) for path in fontstack),
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
            f"UI font {weight} {size} generation failed:\n"
            f"{result.stderr.decode('utf-8', errors='replace')}"
        )
    # fontconvert records absolute source and temporary paths in its banner.
    # Canonicalize that line so generated headers, their IDs, and release
    # binaries are reproducible in local and GitHub Actions workspaces.
    generated = re.sub(
        rb"^ \* Command used:.*$",
        b" * Generated by scripts/build_ui_fonts.py",
        result.stdout,
        flags=re.MULTILINE,
    )
    write_if_changed(output_path, generated)


def locate_bash() -> str:
    """Resolve a real bash.exe on Windows instead of trusting name lookup.

    Windows machines with the "Windows Subsystem for Linux" optional feature
    enabled but no distro installed have a WSL launcher stub at
    C:\\Windows\\System32\\bash.exe that fails with "execvpe(/bin/bash) failed:
    No such file or directory" when invoked. That directory is always on
    PATH, so a plain shutil.which("bash") happily returns that stub as a
    "valid" match on a plain PowerShell/cmd session, since Git for Windows'
    standard installer only adds Git\\cmd (for git.exe) to PATH, not Git\\bin
    or Git\\usr\\bin, where the real bash.exe actually lives. Locate the Git
    install directory from git.exe instead, since that *is* reliably on
    PATH, and look for bash.exe relative to it before ever trying a bare
    PATH search.
    """
    git_exe = shutil.which("git")
    if git_exe:
        git_root = Path(git_exe).resolve().parent.parent  # .../Git/cmd/git.exe -> .../Git
        for candidate in (git_root / "bin" / "bash.exe", git_root / "usr" / "bin" / "bash.exe"):
            if candidate.exists():
                return str(candidate)
    on_path = shutil.which("bash")
    if on_path and Path(on_path).parent.name.lower() != "system32":
        return on_path
    return "bash"


def regenerate_font_ids() -> None:
    result = subprocess.run(
        [locate_bash(), str(FONT_ID_SCRIPT)],
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
    inter_source = FONT_CACHE_DIR / INTER_SOURCE["filename"]
    download_verified(INTER_SOURCE["url"], inter_source, INTER_SOURCE["sha256"])
    arabic_source = FONT_CACHE_DIR / ARABIC_FALLBACK_SOURCE["filename"]
    download_verified(ARABIC_FALLBACK_SOURCE["url"], arabic_source, ARABIC_FALLBACK_SOURCE["sha256"])
    korean_source = FONT_CACHE_DIR / KOREAN_FALLBACK_SOURCE["filename"]
    download_verified(KOREAN_FALLBACK_SOURCE["url"], korean_source, KOREAN_FALLBACK_SOURCE["sha256"])
    caveat_source = FONT_CACHE_DIR / CAVEAT_SOURCE["filename"]
    download_verified(CAVEAT_SOURCE["url"], caveat_source, CAVEAT_SOURCE["sha256"])

    hebrew_sources = {weight: HEBREW_SOURCE_DIR / name for weight, name in HEBREW_FALLBACKS.items()}
    for path in hebrew_sources.values():
        if not path.exists():
            raise RuntimeError(f"Missing in-tree Hebrew fallback: {path}")

    # Every input participates in the stamp so a changed source or axis rebuilds.
    font_paths = {
        "inter": inter_source,
        "caveat": caveat_source,
        "arabic": arabic_source,
        "korean": korean_source,
        **hebrew_sources,
    }

    codepoints = collect_codepoints()
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    manifest = "".join(f"U+{codepoint:04X}\n" for codepoint in sorted(codepoints))
    write_if_changed(CODEPOINTS_PATH, manifest.encode("utf-8"))
    non_korean_codepoints = {codepoint for codepoint in codepoints if not is_korean_codepoint(codepoint)}
    non_korean_manifest = "".join(f"U+{codepoint:04X}\n" for codepoint in sorted(non_korean_codepoints))
    write_if_changed(NON_KOREAN_CODEPOINTS_PATH, non_korean_manifest.encode("utf-8"))
    script_small_codepoints = {
        codepoint
        for codepoint in non_korean_codepoints
        if not is_rtl_script_codepoint(codepoint)
    }
    script_small_manifest = "".join(f"U+{codepoint:04X}\n" for codepoint in sorted(script_small_codepoints))
    write_if_changed(SCRIPT_SMALL_CODEPOINTS_PATH, script_small_manifest.encode("utf-8"))

    signature = build_signature(codepoints, font_paths)
    expected_outputs = [
        FONT_OUTPUT_DIR / f"ui_{size}_{weight}.h"
        for size in SIZES
        for weight in WEIGHTS
    ]
    expected_outputs.extend(FONT_OUTPUT_DIR / f"ui_script_{size}.h" for size in (16, 20))
    current_stamp = STAMP_PATH.read_text(encoding="ascii").strip() if STAMP_PATH.exists() else ""
    if current_stamp != signature or not all(path.exists() for path in expected_outputs):
        python = select_converter_python()
        print(
            f"UI fonts: generating {len(expected_outputs)} 1-bit Inter subsets "
            f"from {len(codepoints)} requested codepoints"
        )
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)

            stacks: dict[tuple[str, int], list[Path]] = {}

            def stack_for(weight: str, size: int) -> list[Path]:
                key = (weight, size)
                if key in stacks:
                    return stacks[key]
                instanced = temporary / f"Inter-{weight}-{size}.ttf"
                instance_variable_font(inter_source, WEIGHTS[weight], size, instanced)
                korean = temporary / f"NotoSansKR-{weight}-{size}.ttf"
                instance_variable_font(korean_source, WEIGHTS[weight], size, korean)
                stacks[key] = [instanced, hebrew_sources[weight], arabic_source, korean]
                return stacks[key]

            for size in SIZES:
                for weight in WEIGHTS:
                    generate_font(
                        python,
                        weight,
                        size,
                        stack_for(weight, size),
                        codepoints_path=(CODEPOINTS_PATH if weight == "medium" else NON_KOREAN_CODEPOINTS_PATH),
                    )

            # The normal accent voice is 20 pt and carries the Hebrew/Arabic
            # fallback stack. The author-only 16 pt cut keeps Caveat's
            # Latin/Cyrillic coverage compact. Korean routes both accent slots
            # to Noto Sans KR Medium at runtime, so neither cut duplicates Hangul.
            caveat_small = temporary / "Caveat-600-16.ttf"
            instance_variable_font(caveat_source, 600, 16, caveat_small)
            accent_stack = stack_for("medium", 16)
            generate_font(
                python,
                "script",
                16,
                [caveat_small, accent_stack[0], accent_stack[-1]],
                font_name="ui_script_16",
                codepoints_path=SCRIPT_SMALL_CODEPOINTS_PATH,
            )

            caveat_normal = temporary / "Caveat-600-20.ttf"
            instance_variable_font(caveat_source, 600, 20, caveat_normal)
            generate_font(
                python,
                "script",
                20,
                [
                    caveat_normal,
                    accent_stack[0],
                    hebrew_sources["medium"],
                    arabic_source,
                    accent_stack[-1],
                ],
                font_name="ui_script_20",
                codepoints_path=NON_KOREAN_CODEPOINTS_PATH,
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
    except NameError:
        pass
    else:
        main()
