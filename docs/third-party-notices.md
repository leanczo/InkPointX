# Third-party notices

## Lucide

System navigation, library, transfer, settings and status icons are derived
from Lucide 1.27.0. The SVG sources are rasterized to compact 1-bit 24 × 24 and
16 × 16 firmware assets by `scripts/generate_lucide_ui_icons.py`.

- Project: https://lucide.dev
- Package: `lucide-static@1.27.0`
- License: ISC (some inherited Feather icons are MIT)
- License text: `docs/licenses/lucide-ISC.txt`

## Font Awesome Free

The home-screen book, clock, and bookmark glyphs are derived from Font Awesome
Free as distributed in LVGL's built-in symbol font. Font Awesome Free icons are
licensed under CC BY 4.0.

- Project: https://fontawesome.com
- License: https://fontawesome.com/license/free
- LVGL font source: https://github.com/lvgl/lvgl/tree/master/scripts/built_in_font

## Inter

The interface uses Inter for all of its type: the Medium weight for body text and
SemiBold for headings, selection and emphasis. Inter ships as a variable font, and
the build instances its `wght` and `opsz` axes per weight and per raster size before
subsetting. The build downloads pinned upstream sources and embeds only
translation-derived, 1-bit bitmap subsets; complete TTF files are not shipped inside
the firmware.

- Upstream project: https://github.com/rsms/inter
- License: SIL Open Font License 1.1 — `LICENSES/Inter-OFL-1.1.txt`
- Fetched from: `google/fonts`, revision
  `7ff85c87f93ea6cca5f41c69f2e4edcb90240f26`, `ofl/inter`

## Interface Hebrew, Arabic, and Korean fallbacks

Inter covers no Hebrew, Arabic, or Korean. The interface subsets are therefore
generated from a font stack: Inter first, then Noto Sans Hebrew, Noto Naskh Arabic,
and Noto Sans KR supply exactly the code points Inter is missing.

- Noto Sans Hebrew: SIL Open Font License 1.1 —
  `LICENSES/NotoSansHebrew-OFL-1.1.txt`, sources in
  `lib/EpdFont/builtinFonts/source/NotoSansHebrew`
- Noto Naskh Arabic: SIL Open Font License 1.1 —
  `LICENSES/NotoNaskhArabic-OFL-1.1.txt`, fetched from the pinned `google/fonts`
  revision above
- Noto Sans KR: SIL Open Font License 1.1 —
  `LICENSES/NotoSansKR-OFL-1.1.txt`, fetched from the pinned `google/fonts`
  revision above

The Arabic presentation forms requested from the fallback are exactly those
`lib/MiniBidi/ArabicShaper.cpp` can emit, parsed from that file at build time
rather than requested as Unicode blocks.

## Noto Serif and Noto Naskh Arabic

The optional `NotoSerifArabic` microSD reader family uses Noto Serif for
Latin/Cyrillic/Vietnamese glyphs and Noto Naskh Arabic as the Arabic fallback.
Its `.cpfont` files are generated in native 1-bit format and are not embedded
in the ESP32-C3 firmware image.

- Noto Serif: https://github.com/notofonts/NotoSerif
- Noto Naskh Arabic: https://github.com/notofonts/arabic
- License: SIL Open Font License 1.1
