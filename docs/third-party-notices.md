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

## FiraGO

The interface uses FiraGO Medium for body text and FiraGO SemiBold for headings
and emphasis. The build downloads pinned upstream sources and embeds only
translation-derived, 1-bit bitmap subsets; complete TTF files are not shipped
inside the firmware.

- Upstream project: https://github.com/bBoxType/FiraGO
- License: SIL Open Font License 1.1
- Pinned revision: `5bbcb9d066ab563686ed1de1e6f62eec0148e82d`

## Noto Serif and Noto Naskh Arabic

The optional `NotoSerifArabic` microSD reader family uses Noto Serif for
Latin/Cyrillic/Vietnamese glyphs and Noto Naskh Arabic as the Arabic fallback.
Its `.cpfont` files are generated in native 1-bit format and are not embedded
in the ESP32-C3 firmware image.

- Noto Serif: https://github.com/notofonts/NotoSerif
- Noto Naskh Arabic: https://github.com/notofonts/arabic
- License: SIL Open Font License 1.1
