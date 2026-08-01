# Brand lock/unlock QA — 2026-07-29

Target: XTEINK X4, native 480 × 800 monochrome framebuffer.

## Evidence

- `01-boot.png` — first implementation pass using the shared UI font.
- `02-boot-wordmark.png` — dedicated FiraGO SemiBold wordmark pass.
- `03-settings.png` — device UI sanity capture after settings migration.
- `04-reference-vs-device.png` — intermediate same-input comparison.
- `05-final-boot.png` — corrected logo orientation in the development build.
- `06-final-reference-vs-device.png` — final source/device visual comparison.
- `07-release-final.png` — production-profile framebuffer after flashing.

## Result

No actionable clipping, overlap, stale overlay or alignment issue remains.
The brand block is the only content on boot/unlock and on the default lock
screen. Battery removal also covers quick resume, custom/cover imagery,
grayscale imagery and blank sleep.

final result: passed
