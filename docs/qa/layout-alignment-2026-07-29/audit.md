# Layout alignment audit — 2026-07-29

Target: the production InkPoint X interface on the XTEINK X4 at its native
480 × 800, one-bit portrait framebuffer. Evidence was captured from the
connected device before and after this pass.

## Result

The home cover now consumes the maximum safe vertical space while preserving
the title, author, progress, reading time, primary action, pagination and
hardware legend. Common list geometry was also corrected so SemiBold selected
text, trailing values and RTL content occupy explicit non-overlapping lanes.

## Checked flow

1. **Now Reading — healthy**
   - Before: `01-before-home.png`
   - After: `14-release-final-device.png`
   - Comparison: `comparison-home.png`
   - The portrait cover grows from about 190 × 288 to about 258 × 390 for the
     current book. Its exact slot height is calculated from the real title and
     metadata line heights, so a two-line title reduces the cover before it
     can collide with the lower content.
   - The Continue Reading row now lays out its leading icon, localized label
     and directional disclosure from one shared vertical center and symmetric
     edge padding. RTL mirrors the two icons and keeps the text in the lane
     between them.

2. **Library and Transfer — healthy**
   - Before: `02-before-library.png`
   - After: `09-after-library.png`
   - Primary rows and two-line Transfer rows retain aligned icons,
     disclosures, dividers and selection bounds.

3. **Books — healthy**
   - Before: `03-before-books.png`
   - After: `10-after-books.png`
   - Selected titles are now measured and truncated using the same SemiBold
     weight used to draw them. Title/author and format lanes remain separate.

4. **Files — healthy**
   - Before: `04-before-files.png`
   - After: `11-after-files.png`
   - Folder and file icons, names, extensions, scroll indicator, breadcrumb
     and hardware legend remain inside their allocated regions.

5. **Gallery — healthy**
   - Before: `05-before-gallery.png`
   - After: `12-after-gallery.png`
   - Long image names and paths remain in separate lines without crossing the
     selection or footer.

6. **Interface settings — healthy**
   - Before: `07-before-settings.png`
   - After: `13-after-settings.png`
   - Chevrons and switches use their own measured accessory height; values,
     labels and selection bounds remain aligned.

## Device and accessibility notes

- The layout uses actual FiraGO bitmap advance heights rather than guessed
  baselines.
- Dynamic content is truncated in its own lane before drawing, including the
  wider selected weight.
- All focus states remain visible through a one-pixel rounded outline plus a
  sparse texture, without a large black e-ink fill.
- Screenshot evidence cannot prove every possible SD filename or physical
  button combination. Host tests, multilingual validation and a final release
  capture supplement the visual pass.

The connected device runs the release image captured in
`14-release-final-device.png`. The flashed artifact is
`artifacts/firmware.bin`, SHA-256
`0867b9a4aff34d9bd1f14446d388c68235c9cede1eaf8730c329d0433a8b00e2`.
