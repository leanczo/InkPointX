# InkPoint X interface

InkPoint X is the default interface for the Xteink X4. It uses high-contrast,
single-column screens designed for the X4's 480 × 800 monochrome display and
four front buttons.

## Visual system

The interface follows Apple-inspired principles of clarity, restraint,
predictable hierarchy, visible selection feedback and accessibility, adapted
to a non-touch 1-bit e-ink device rather than copied from iOS.

- 20 px outer margins and an 8 px spacing rhythm keep screens calm and
  consistent.
- FiraGO Medium is used for body text; SemiBold is reserved for titles,
  selection and values that need emphasis.
- Focus uses a 12 px rounded outline over a deterministic 1/16-density surface.
  It never depends on color or an inverted black bar, and the sparse pixel
  charge is friendly to differential e-ink refreshes.
- Hairline dividers use a sparse deterministic pixel pattern, reducing visual
  weight and avoiding unstable antialiasing.
- Lucide line icons are rasterized at 24 × 24; disclosure and check
  accessories use 16 × 16 assets. RTL screens use mirrored disclosure icons.
- Bottom button legends reproduce the X4's two long, two-section rockers,
  omit unavailable actions and mirror the user's physical-button mapping.
- Boolean settings use a compact outlined switch with a position-changing
  black thumb instead of translated ON/OFF text.
- Popups and confirmations are rounded white cards with wrapped text. Progress
  controls use a light dithered track and solid black value.

No shadows, translucency, animation frames or continuous scrolling are used.
The only pressed feedback is composed into the next requested framebuffer, so
it does not add a second e-ink refresh or duplicate an input action.

## Home

The home screen has three pages:

1. Continue reading
2. Library and Transfer subsections
3. Settings

Use the front Left and Right buttons to change pages, the side buttons to move
through a list, Confirm to open an item, and Back to return.

Now Reading prioritizes the cover, then title and author, then three compact
statistics. The cover uses the maximum safe slot calculated from the current
title line count, real font advance heights, footer visibility and pagination
clearance; a long title therefore reduces the cover before content can
overlap. The primary action treats its icon, localized label and disclosure
as one measured row and mirrors them for RTL. Library, Transfer and Settings
use the same row, icon, selection, scroll indicator and disclosure language as
the rest of the firmware.

## Typography

FiraGO is the system typeface: Medium is used for normal UI text and SemiBold
for headings, selections and emphasis. The 1-bit scale is deliberately small:
8 px for hardware legends, 12 px for labels and metadata, 14 px for primary
two-line rows and book titles, and 16–18 px for screen hierarchy.

The pre-build pipeline reads every value in `lib/I18n/translations/*.yaml`,
adds bounded dynamic Hebrew/Arabic ranges, and generates native 1-bit subsets.
Complete TTF files are downloaded only as build inputs and are not embedded in
the firmware. Reader fonts are configured independently.

## Languages

The firmware ships complete interface text for all 27 declared languages:
English, Arabic, Belarusian, Catalan, Valencian, Czech, Danish, Dutch, Finnish,
French, German, Hebrew, Hungarian, Italian, Kazakh, Lithuanian, Polish, Portuguese
(Brazil), Romanian, Russian, Slovak, Slovenian, Spanish, Swedish, Turkish,
Ukrainian, and Vietnamese.

Each locale owns all 523 source strings. The generated release therefore never
falls back to English for a missing menu label, warning, file operation,
reader action, network workflow, or settings value. Format placeholders and
the spacing of concatenated labels are validated before building.

FiraGO includes every Unicode character used by these translations. Hebrew and
Arabic use the firmware's bidirectional text path; Arabic also receives
contextual shaping for static UI text and dynamic book, author and file names.
Vietnamese precomposed characters and the extended Cyrillic characters used by
Belarusian, Kazakh, Russian, and Ukrainian are embedded in the subsets.

## Library and favorites

Books recursively scans the SD card for supported book formats. Left and Right
change the sort order between title, author, format, and recently opened.

Hold Confirm on a book to add it to or remove it from Favorites. Favorites are
stored in `/.crosspoint/favorites.json`; removing a favorite never removes the
book itself.

The lower Transfer subsection uses the same full-size heading style as Library.
Join Network, Calibre Wireless, and Create Hotspot are available directly
without an intermediate File Transfer menu.

## Reader

Confirm opens the reader menu for EPUB, FB2, and PDF books. The menu includes
page and chapter navigation, bookmarks, reading settings, book information,
opening another file, favorites, reading statistics, orientation, automatic
page turning, screenshots, QR display, sync, and cache maintenance.

Reading settings include the font family and size, line spacing, margins,
alignment, hyphenation, and black/white inversion. Inversion uses the existing
one-bit framebuffer so it does not require a second full-screen allocation.

## Settings and reset

The third home page is the settings hub. It has seven submenus:

1. Interface
2. Screen & Power
3. Reading
4. Controls
5. Library & Files
6. Network & Sync
7. System

There is no separate Advanced settings screen and no category tab bar. The
submenus retain every existing setting and action, including language,
interface and reader fonts, sleep behavior, status-bar controls, button
mapping, library behavior, Wi-Fi, KOReader Sync, OPDS configuration and
browsing, cache maintenance, firmware updates, device information, and reset.

A compact legend at the bottom of menu screens shows the current assignment of
all four front buttons. The labels follow the user's configured button remap.

Reset restores firmware configuration and removes saved Wi-Fi, OPDS, and
KOReader Sync credentials. It preserves books, reading progress, bookmarks,
statistics, recent books, and favorites.
