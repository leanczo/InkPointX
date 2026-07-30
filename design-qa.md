# InkPoint X home screen — design QA

final result: passed

## Systematic defect pass — design and function — 2026-07-30

A four-part audit (interface layer, reader and settings, network and transfer,
rendering and document formats) drove this pass. Findings were verified against
the tree before being fixed; nothing was changed on suspicion alone.

### Interface

- Gallery's list reserved only 8 px above the button legend while it also drew
  the "n / m" footer counter, so with nine or more images the last row was drawn
  underneath it. Library, Settings and Interface font each reserved a different
  value (54, 42, 42). All four now derive the bottom edge from one shared
  `UITheme::getListContentBottom`.
- The Favorites marker was a U+2605 star appended to the format string. FiraGO
  has no glyph for that codepoint, so nothing was drawn and the two padding
  spaces left favourited rows with a ragged right edge. It is now a generated
  16 px Lucide star drawn through the shared accessory lane.
- The image viewer painted photographs with the differential waveform and its
  text error states with the clean one. Reversed: a halftoned photo is the one
  payload a differential update cannot reconcile, and browsing siblings stacked
  photo over photo with no clean in between.
- Two hardcoded English strings in the image viewer now use the existing
  localized key, so no screen falls back to English in the other 26 locales.
- Back did nothing on the confirmation dialog — the modal every destructive
  action routes through. Back now cancels, Confirm confirms, and both commit on
  release like every other screen.
- The file browser's path bar drew a 3 px edge-to-edge rule, the heaviest element
  on the screen; the folder picker drew none. Both now use one shared inset
  hairline, and the folder picker's action row is distinguishable by accessory
  rather than only by position.
- Dotfiles rendered as blank, selectable, deletable rows because the name was
  split on its leading dot.
- `truncatedText` dropped one more character than necessary from every truncated
  label (`>=` where the fit test used `<=`).
- Properties clamped the filename it exists to show to ~200 px and printed raw
  byte counts; it now uses two-line rows and KB/MB units.
- The reader Font row drew a chevron and labelled Confirm "Select" but cycled the
  value in place unless SD fonts were installed. It always opens the picker now.
- Home's first-boot state showed two dashes and an empty 0 % rule; those bands
  are now simply empty, and the rule follows the content margin instead of a
  hardcoded 52 px that assumed a 480 px panel.
- Removed: two theme engines that were never instantiated, 15 orphaned icon
  headers, the tab-bar drawing triple and the legacy home wordmark. `tabBarHeight`
  is renamed `subHeaderHeight` — it governs the sub-header on a third of the
  firmware's screens and the tab bar it was named for no longer exists.
- 25 dead photo-frame and chess keys were removed from all 27 locale files; the
  generated string table drops from 331,810 to 315,642 bytes.

### Reader and settings

- Opening the reader menu switched automatic page turning off. The menu returns
  its picker value on cancel as well, and that value was a fresh 0 on every
  visit; the reader now passes the running rate in, as it already did for
  orientation.
- Reading statistics applied the UTC offset twice, so every time-of-day bucket,
  day of week and streak day was shifted by double the configured offset.
- Automatic page turning did not inhibit deep sleep, so hands-free reading was
  interrupted by the inactivity timer.
- `serialization::readString` resized a `std::string` to an unvalidated 32-bit
  length read from storage. With exceptions disabled that aborts, and the call
  sites are book-open and boot — a reboot loop that survives a power cycle. Reads
  are now bounded and checked, and the metadata-cache readers reject truncated
  entries.
- The chapter list built its indent from `level - 1` on a `uint8_t`; a level of 0
  (an unloaded cache, an out-of-range index, an `<a>` outside any `<ol>`)
  underflowed and aborted the firmware.
- The footnote return stack was popped even when the matching push had been
  skipped, so Back after an unresolvable footnote landed on the wrong page.
- XTH page buffers were sized from `(w*h+7)/8` while the reader addresses them
  column-major, under-allocating whenever the height is not a multiple of 8; a
  short read also rendered uninitialised heap. The status-bar setting was
  silently ignored for XTH books, and a 384,000-iteration debug histogram ran in
  release builds.
- The plain-text reader read an uninitialised offset and could persist a garbage
  page index; its cache accepted an unbounded page count.
- Web settings listed the clock position labels in reverse, so the web UI put the
  clock opposite the chosen side and disagreed with the device screen. The
  status-bar screen clamped the wrong field, leaving progress-bar thickness
  unvalidated. "Go to percent" bound +/- to raw buttons while its legend followed
  orientation.
- Bookmark files dropped the extension from their key, so `a.epub` and `a.txt`
  shared one file. Fixed with a one-shot migration that adopts the old file.
- Bookmarks is no longer offered when there are none (it rendered an empty
  screen), and the status bar's progress fraction now matches the reader menu.

### Network and transfer

- `/download`, `/delete`, `/api/files`, `/mkdir` and WebDAV `PROPFIND` tested only
  the last path component, so `/.crosspoint/wifi.json` — the saved Wi-Fi and sync
  credentials — was readable and deletable by any client on the network. All
  paths are now canonicalized and every segment checked.
- Upload destinations were unvalidated in both the multipart and WebSocket paths,
  so a crafted filename or path wrote anywhere on the card.
- `GET /api/settings` returned the KOReader sync password in plaintext over an
  unauthenticated endpoint — served over an open network in hotspot mode. Secrets
  now report only whether a value is stored.
- `WifiCredentialStore` was loaded only by the Wi-Fi picker, and it persists its
  whole in-memory list. Adding a network from the web UI in hotspot mode rewrote
  `wifi.json` from an empty list, discarding every saved network. It is now loaded
  at boot with the other stores.
- OTA disabled TLS hostname verification while writing straight to the OTA
  partition with no image signature to fall back on.
- A browser that died mid-upload left the WebSocket slot claimed forever, so
  every retry was refused. Added a heartbeat and a stall deadline.
- Hotspot mode ran a wildcard DNS server but answered every probe with a plain
  404, so no captive-portal sheet appeared and Android flagged the network as
  offline.
- The web settings Save button stayed disabled after a failed save, and password
  fields were detected by sniffing the *translated* label — plain text in every
  non-English locale.
- Calibre mode had no Wi-Fi health check (a dropped access point looked like a
  hang) and repainted the panel on every 64 KB of progress.

### Rendering and formats

- Upscaled PNGs left destination rows unwritten. On the direct pass that showed
  as white combing; once cached it became permanent black stripes, because the
  cache flushes unwritten rows from a zeroed band where 0 means black. The row
  span is now derived from the real height ratio.
- `DirectPixelWriter` clipped rows but not columns, so one pixel past the layout
  wrote past the grayscale band scratch buffer.
- `ZipFile` computed `uncompressedSize + 1` in 32 bits: a ZIP64 sentinel wrapped
  to 0, `malloc(0)` succeeded, and the read wrote gigabytes past it. Entry and
  chapter counts read from file headers no longer drive unbounded reserves.
- The bidi pass stopped building its buffer at U+FFFD, which is what the UTF-8
  decoder returns for any malformed byte — so one bad byte in a title or filename
  silently dropped the rest of the string.
- `drawRect` drew every border one pixel wider and taller than requested, so
  frames bled outside their fill and a rect flush with a screen edge logged an
  out-of-range error per pixel.
- The three ditherers allocated their error rows with throwing `new`; cover
  generation on a fragmented heap aborted instead of degrading to plain
  quantization.
- Footnote hrefs longer than the fixed buffer were silently truncated and
  resolved to nothing; they are now left as ordinary text with a log line.

### Verification

- Development and release builds pass. Release uses 90.9 % of the application
  partition (was 91.2 %) and 32.3 % of RAM.
- 107 of 107 host tests pass.
- Localization validation passes for all 27 locales (498 strings each, 533 UI
  codepoints, 10 font families checked).
- `pio check` reports 0 high and 0 medium findings.
- Not changed on purpose: automatic clean-refresh injection stays off for
  navigation. That is a deliberate, documented and test-covered decision — the
  driver keeps controller RAM synchronized as the differential baseline, and the
  reader has its own configurable cadence. The photographic case that genuinely
  needed a clean waveform is fixed above.
- Not verified here: this pass was validated by build, host tests, static
  analysis and framebuffer reasoning. It has not been flashed to a physical X4,
  so outdoor contrast, panel retention across hundreds of differential refreshes,
  and the live Wi-Fi, Calibre, WebDAV and OTA sessions remain untested on
  hardware.

final result: passed

## Brand-only lock and unlock presentation — 2026-07-29

- Source reference:
  `/var/folders/41/dmt5d0mj489fv_f_gdgb3lym0000gn/T/codex-clipboard-f1669422-9b99-48cd-a00c-b2222083c238.png`
- Native 480 × 800 release framebuffer:
  `docs/qa/brand-lock-screen-2026-07-29/07-release-final.png`
- Same-input visual comparison:
  `docs/qa/brand-lock-screen-2026-07-29/06-final-reference-vs-device.png`
- The boot/unlock and default lock screens now share one brand component:
  a 160 × 160 source-derived logo and a dedicated 36 px FiraGO SemiBold
  wordmark subset.
- Logo orientation, wordmark proportion, horizontal centring and vertical
  balance match the supplied source at the X4's native viewport. The deliberate
  one-bit rendering is slightly heavier than the antialiased reference.
- Firmware version, loading label, sleeping label and the system battery
  overlay are absent. Quick-resume explicitly clears a previously retained
  battery group; cover, custom, blank and grayscale sleep paths suppress the
  global overlay.
- The former iPhone-style clock option is removed from the device and web
  settings. Its historic persisted enum value migrates to the light brand
  screen without shifting the remaining option values.
- Final production artifact: `artifacts/firmware.bin`, 5,966,880 bytes,
  SHA-256 `893e1a17f02a56a6efefcc48b93d88fec40e415a97a71d06fe5a557df07ae72d`.
- Verification: release and development builds pass; localization/font
  validation passes for all 27 locales; 107/107 host tests pass; cppcheck
  reports zero high or medium findings. The artifact was flashed to the
  connected XTEINK X4 and recaptured from its real framebuffer.

final result: passed

## Maximum home cover and alignment pass — 2026-07-29

- Native before/after evidence and the six-screen audit are in
  `docs/qa/layout-alignment-2026-07-29/`.
- The current portrait cover grows from approximately 190 × 288 to
  approximately 258 × 390. The maximum is computed from the remaining safe
  height rather than assumed from one sample title.
- Title lines use the font's real 35-pixel advance. Author, progress, reading
  time, action, pagination and the global hardware legend have explicit
  clearance and do not overlap for one- or two-line titles.
- Continue Reading now has one layout calculation for its 24-pixel icon,
  localized label and 16-pixel disclosure. All three share the same center;
  RTL mirrors the composition.
- Shared lists now truncate selected text using SemiBold metrics, reserve
  independent text/value/accessory lanes and center each accessory by its own
  height.
- Library, Books, Files, Gallery and Interface settings were recaptured from
  the connected 480 × 800 device after the shared correction. No actionable
  clipping or collision remains in those evidence states.
- Final release evidence: `14-release-final-device.png`. The connected device
  was flashed with `artifacts/firmware.bin`, SHA-256
  `0867b9a4aff34d9bd1f14446d388c68235c9cede1eaf8730c329d0433a8b00e2`.

final result: passed

## System-wide Apple-inspired redesign — 2026-07-29

### Evidence

- Current-run native audit folder:
  `docs/qa/apple-system-audit-2026-07-29/`
- Now Reading comparison:
  `docs/qa/apple-system-audit-2026-07-29/comparison-home.png`
- Library comparison:
  `docs/qa/apple-system-audit-2026-07-29/comparison-library.png`
- Settings comparison:
  `docs/qa/apple-system-audit-2026-07-29/comparison-settings.png`
- Real books list:
  `docs/qa/apple-system-audit-2026-07-29/09-redesign-books.png`
- Real file manager:
  `docs/qa/apple-system-audit-2026-07-29/10-redesign-files.png`
- Settings hub:
  `docs/qa/apple-system-audit-2026-07-29/11-redesign-settings-hub.png`
- Final release framebuffer:
  `docs/qa/apple-system-audit-2026-07-29/13-release-final-device.png`

### Findings

- Home now uses the same header, FiraGO SemiBold title, battery reservation,
  margins, and sparse hairline as every internal activity.
- The 20 px content grid, 54 px standard rows, 76 px title/subtitle rows, and
  58 px home-menu rows preserve a minimum 44 px interaction target.
- The shared selection primitive is a one-pixel 12 px-radius outline over a
  deterministic 1/16-density surface. It remains visible without creating a
  high-charge block that can linger after a fast e-ink refresh.
- FiraGO Medium carries body labels and values. SemiBold is limited to screen
  titles, selected labels, and primary book metadata.
- Two-line lists use 14 px primary copy and 12 px secondary copy. The first
  device pass exposed a collision in Transfer; increasing the row to 76 px
  resolved it before release.
- Boolean settings use language-independent outlined switches with positional
  state. Enum values and actions retain text values and mirrored disclosures.
- Lucide assets keep a consistent 24 px optical box and are mirrored where the
  shared RTL layout requires it.
- The hardware legend remains one system component with two long,
  two-section rockers. Its pressed feedback now uses the same sparse surface
  instead of a large black inversion.
- No visible functionality was removed: Books, Files, Gallery, Favorites,
  transfer actions, seven settings submenus, file operations, reader menus,
  network flows, dialogs, button remapping, battery visibility, and hint
  visibility remain reachable.

### Accessibility and e-ink limits

- Focus does not depend on gray alone: the rounded outline remains visible.
- No new shadows, transparency, large black navigation surfaces, or
  multi-frame animations were introduced.
- Screenshot evidence cannot prove outdoor contrast, motor accessibility, or
  panel retention over hundreds of differential refreshes. These are covered
  by the physical-device interaction and refresh-policy test pass.
- The final release image is 5,992,928 bytes, uses 91.2% of the application
  partition and 32.3% of RAM, and was flashed to the connected XTEINK X4.

Target: XTEINK X4 portrait display, 480 × 800, monochrome e-ink framebuffer.

## Evidence

- Reference: user-provided InkPoint X home-screen mockup.
- Device capture: `docs/qa/inkpoint-home-device.png`
- Side-by-side comparison: `docs/qa/inkpoint-home-comparison.png`
- Selection reference: user-provided photo of the Library screen.
- Updated selection capture: `docs/qa/inkpoint-selection-library-device.png`
- Selection comparison: `docs/qa/inkpoint-selection-comparison.png`
- File-transfer section capture: `docs/qa/inkpoint-file-transfer-library-device.png`
- File-transfer comparison: `docs/qa/inkpoint-file-transfer-comparison.png`
- Transfer subsections capture: `docs/qa/inkpoint-transfer-subsections-device.png`
- Transfer subsections comparison: `docs/qa/inkpoint-transfer-subsections-comparison.png`
- Final normal-boot capture: `docs/qa/inkpoint-transfer-final-home-device.png`
- Settings hub capture: `docs/qa/inkpoint-settings-hub-device.png`
- Interface settings submenu capture: `docs/qa/inkpoint-settings-interface-device.png`
- Final post-settings normal-boot capture: `docs/qa/inkpoint-settings-final-home-device.png`
- Ink Sans device capture: `docs/qa/inkpoint-inksans-library-device.png`
- Ink Sans home capture: `docs/qa/inkpoint-inksans-home-device.png`
- System-font comparison: `docs/qa/inkpoint-font-comparison.png`
- Complete-localization device capture:
  `docs/qa/inkpoint-all-locales-device.png`
- Ura Bum Bum SP heading capture:
  `docs/qa/inkpoint-urabumbumsp-headings-device.png`

The device capture was read from the firmware's real framebuffer over USB after
flashing the connected X4. It is not a browser or hand-built mock.

## Checks

- The firmware name has been removed from the home screen as requested.
- “Now reading” uses the same shared header component, typography, padding, and
  underline as the neighboring Library screen.
- The freed vertical space is assigned to a taller cover and a more compact,
  evenly spaced metadata/statistics composition.
- Dynamic book metadata wraps without colliding with the author or statistics.
- The cover keeps its source aspect ratio, is centered, and has rounded corners.
- Statistics are populated from persisted reader progress and reading-time data.
- Book, clock, and bookmark use Font Awesome glyphs from LVGL's bundled symbol
  font and are aligned to the X4 driver's 8-pixel bitmap stride.
- The requested compact button-assignment footer fits below the page indicator.
- No clipped text, out-of-bounds drawing, overlapping blocks, or broken rounded
  corners are visible in the final 480 × 800 capture.
- The former solid-black selection has been replaced with one shared 25% black
  light-gray dither and a 12 px corner radius.
- Selected text and icons remain black, preserve their detail, and pass visual
  contrast checks on the physical e-ink representation.
- The shared selection primitive is used by menus, lists, tabs, keyboard focus,
  chapter/footnote choices, sync actions, clock fields, and statistics fields.
- Transfer remains inside the second home section and is visually separated
  from Books, Files, Gallery, and Favorites by a second full-size header.
- Join Network, Calibre Wireless, and Create Hotspot are directly selectable
  below that header; the redundant File Transfer wrapper screen is skipped.
- The third home section is now the Settings hub itself. It exposes seven
  purpose-built submenus instead of linking to a second settings screen.
- The former Advanced settings entry and category tab bar are absent; every
  setting remains reachable through Interface, Screen & Power, Reading,
  Controls, Library & Files, Network & Sync, or System.
- Configured OPDS catalogues remain both editable and browseable from Network &
  Sync, so removing the former More page does not remove catalogue access.
- The new group and its selected state fit above the page dots and compact
  button-assignment footer at 480 × 800 without clipping.
- Ink Sans replaces the default system UI typography at small, list, header,
  metadata, and home-title sizes while leaving reader typography independent.
- Ura Bum Bum SP replaces Ink Sans only for system headings. Body copy, list rows,
  values, metadata, control hints, and reader text retain their existing fonts.
- Medium and SemiBold weights preserve counters and stroke continuity after
  e-ink quantisation; no label, footer, or Cyrillic text is clipped.
- The final device capture confirms that the Cyrillic title and author, large
  home metadata, statistics, and compact control footer fit the 480 × 800 panel
  without overlap or truncation.
- Ten generated Ink Sans variants pass bitmap decompression verification with 1050
  glyphs each, proportional figures, kerning, and Hebrew fallback coverage.
- Legacy devices using the former default Noto Sans migrate once to Ink Sans;
  explicit Ubuntu selection remains intact, and both legacy choices remain
  available in settings.

## Intentional differences

- The original brand lockup and centered “Now reading” ornament are superseded
  by the requested Library-style section header.
- Text language follows the device setting; the captured device was set to
  English while the reference example is Russian.
- The book cover, title, author, progress, reading time, and page estimate are
  real user data, not the mockup's Tolkien example.
- LVGL's bundled Font Awesome glyphs are solid, while the reference uses outline
  variants.
- The bottom control hint is an explicit product requirement added after the
  visual reference.

## Transfer subsection QA — 2026-07-25

### Comparison target

- Source visual truth:
  `/tmp/codex-remote-attachments/019f98bf-011f-70f1-af6d-f97211e3826c/4A9E13D9-F2D2-457F-9F1D-8268C0EA3794/2-Фото-2.jpg`
- Supporting close-up:
  `/tmp/codex-remote-attachments/019f98bf-011f-70f1-af6d-f97211e3826c/4A9E13D9-F2D2-457F-9F1D-8268C0EA3794/1-Фото-1.jpg`
- Implementation: `docs/qa/inkpoint-transfer-subsections-device.png`
- Full-view comparison: `docs/qa/inkpoint-transfer-subsections-comparison.png`
- Source pixels: 2880 × 3840 perspective device photo.
- Implementation pixels and viewport: native 480 × 800 framebuffer, one bit per
  pixel; no browser CSS scaling or device-frame simulation.
- Comparison normalization: both images were aspect-fit into equal 720 × 920
  cells. Perspective and bezel differences were excluded from layout findings.
- State: second home section, first Transfer action selected.
- Focused comparison: the supporting source close-up and native framebuffer
  make both header treatments, labels, descriptions, icons, and selection
  boundary readable; a separate crop was therefore unnecessary.

### Findings

- No actionable P0, P1, or P2 differences remain.
- Typography: Library and Transfer use the same UI font ID, bold weight,
  56-pixel header component, 20-pixel inset, and underline treatment.
- Spacing: four 72-pixel Library rows, the second header, and three 72-pixel
  Transfer rows fit above the page dots and control footer without clipping.
- Color and state tokens: the selected transfer row uses the shared light-gray
  e-ink dither, black foreground, and 12-pixel rounded selection geometry.
- Asset quality: Wi-Fi, Calibre/library, and hotspot use the existing
  production bitmap icon library; no placeholder or improvised glyph was added.
- Copy: the device's Ukrainian titles and descriptions remain intact. Every
  declared locale now owns every firmware string, so this screen no longer
  depends on English fallback text.

### Comparison history

- Earlier P1 information-architecture mismatch: an intermediate implementation
  proposed a fourth carousel section. It was removed before handoff.
- Fix: retained three page dots and split the second section into Library and
  Transfer blocks. Each transfer action now launches its existing workflow
  directly.
- Post-fix evidence: `docs/qa/inkpoint-transfer-subsections-device.png`.

### Residual test gap

- The framebuffer proves the selected state and complete layout. Wi-Fi,
  Calibre, and hotspot network sessions were not run during visual QA because
  they require external network endpoints; their existing activities and the
  new direct routing compile successfully.

## Settings hub QA — 2026-07-25

- The native 480 × 800 capture shows all seven categories without clipping:
  Interface, Screen & Power, Reading, Controls, Library & Files, Network &
  Sync, and System.
- The shared light-gray, 12-pixel-radius selection treatment renders correctly
  over the category icon and label.
- The Interface capture proves the submenu layout uses a single list with a
  page counter and button legend; there is no tab bar or Advanced settings
  wrapper.
- All legacy setting sources were accounted for: the shared settings registry,
  language and interface-font pickers, status-bar controls, button remapping,
  Wi-Fi, KOReader, OPDS, cache maintenance, firmware updates, device
  information, and reset.
- Names for the new categories are included in all 26 supported languages.
- The final production image boots to Now reading, confirming that both
  temporary QA entry points were removed before flashing.

## Complete localization QA — 2026-07-25

- All declared languages contain all 523 source strings in canonical order,
  with zero empty values and zero English
  fallback entries.
- The validator checks duplicate, missing, and extra keys; `printf`
  placeholders; edge spacing used by concatenated labels; leaked translation
  tokens; control characters; and embedded-font glyph coverage.
- All 369 Unicode code points used by locale names and translated UI strings
  exist in every selectable interface-font family: Ink Sans, Noto Sans, and
  Ubuntu. Hebrew punctuation was added to the generated bitmap subsets.
- Critical file operations, reset warnings, network actions, and SD firmware
  update warnings received an additional terminology review after the
  completeness pass.
- The release build uses 96.4% of the application partition and 31.0% of RAM.
  All 94 host tests pass when run sequentially; static analysis reports no high
  or medium findings.
- The connected XTEINK X4 was flashed successfully. The native framebuffer
  capture shows the Russian Library and Transfer subsections rendered with the
  final translations and font data; boot logs show a stable Home activity with
  roughly 90 KB free heap.

## Ura Bum Bum SP heading QA — 2026-07-25

- One dedicated 16-pixel compressed bitmap contains 1,052 glyphs. Ura Bum Bum SP
  supplies its Latin and Cyrillic display forms; Ink Sans and Noto Sans Hebrew
  fill only missing code points.
- Shared screen headers and manually drawn activity headings use the dedicated
  heading font ID. Menu rows, descriptions, values, dialogs, reader content,
  book metadata, and button hints remain on their existing readable fonts.
- All 26 locales pass complete glyph coverage through the generated heading
  stack; no locale string falls back to a missing-character box.
- The release image uses 97.3% of the application partition and 31.0% of RAM.
  All 94 host tests pass sequentially, font decompression passes for all 45
  compressed bitmaps, and static analysis reports no high or medium findings.
- The connected XTEINK X4 was flashed with the final image. The native
  framebuffer capture confirms that Cyrillic headings fit the 480 × 800
  display without clipping while the surrounding body typography is unchanged.

## Home option 3 QA — 2026-07-29

### Comparison target

- Source visual truth: `docs/qa/home-option3-source-480x800.png`
- Native final-release framebuffer: `docs/qa/home-option3-final-device.png`
- Full final-release comparison: `docs/qa/home-option3-final-comparison.png`
- Source and implementation pixels: 480 × 800.
- Viewport: the native XTEINK X4 portrait framebuffer at one bit per pixel;
  neither side uses browser scaling or a simulated device frame.
- State: Russian locale, Now Reading page, Continue Reading action selected,
  battery and control hints visible.
- Focused regions: unnecessary because the equal-size full-view comparison
  keeps header, cover, metadata, action, pagination, and footer legible at
  native resolution.

### Findings

- No actionable P0, P1, or P2 differences remain.
- Typography: the hierarchy is preserved with a quiet medium-weight header,
  semibold title and action, and medium metadata. All text uses the production
  FiraGO subset and remains legible after one-bit quantisation.
- Layout: the real cover is aspect-fit to the centered 206 × 322 maximum
  region; title, author, progress, reading time, action, pagination, and footer
  occupy the same vertical bands as option 3 without overlap or clipping.
- Color and state tokens: the selected action uses a sparse one-bit dither,
  black foreground, a one-pixel outline, and an 11-pixel corner radius. It
  remains visibly selected without becoming a heavy black card.
- Image quality: the firmware renders the cached book cover directly from the
  microSD thumbnail and masks only the rounded corners. There are no temporary
  or stretched assets.
- Copy: all labels come from the active locale. Dynamic title and author data
  use the shared bidi/shaping renderer and are width constrained.
- Header adaptation: per the user's instruction, option 3's header was replaced
  with the production status header. It keeps the page name left aligned and
  the optional battery indicator right aligned without a decorative divider.
- Footer adaptation: the reference footer is rendered by the same shared
  `GUI.drawButtonHints` component used throughout the firmware. Labels retain
  their physical-button slots, empty mappings remain empty, and the footer
  obeys both button remapping and the existing Show button hints setting.

### Comparison history

- Pass 1 device evidence: `docs/qa/home-option3-implementation-pass1.png`.
- Pass 1 P2 findings: the header was visually heavy, the selected action's
  dither was too dense, and its text crowded the chevron.
- Fixes: reduced header emphasis, changed the selection to a sparse rounded
  dither, and reduced action-label emphasis.
- Post-fix debug evidence: `docs/qa/home-option3-implementation-pass2.png`.
- Final flashed release evidence: `docs/qa/home-option3-final-device.png`.

### Shared system footer correction — 2026-07-29

- The home-specific footer renderer was removed after user review.
- Home now calls the same `GUI.drawButtonHints` API as Library, Files, Gallery,
  Settings, network screens, and utility dialogs.
- Native home evidence: `docs/qa/home-unified-system-footer-debug.png`.
- Native settings evidence: `docs/qa/settings-unified-system-footer-debug.png`.
- Final flashed release evidence:
  `docs/qa/home-unified-system-footer-final.png`.
- Equal-scale focused comparison:
  `docs/qa/unified-system-footer-comparison.png`.
- The comparison confirms identical 18-pixel side margins, four physical
  button slots, five-pixel gaps, 20-pixel height, eight-pixel corner radius,
  one-pixel outline, bold small-label typography, and baseline alignment.
- Different words are intentional because each screen exposes different
  actions; geometry and visual treatment are shared.
- No actionable P0, P1, or P2 differences remain.

### Intentional differences

- The framebuffer contains current device battery data and persisted reading
  statistics rather than static values.
- The full localized phrase “Продолжить чтение” is retained instead of
  shortening the action to “Продолжить”, preserving meaning and translation
  consistency across every supported locale.

final result: passed
