# InkPoint X

[![Support on Ko‑fi](https://img.shields.io/badge/Support-Ko--fi-F16061?logo=kofi&logoColor=white)](https://ko-fi.com/yokkivans)

InkPoint X is a custom firmware for Xteink X3/X4 devices, built as a fork of **CrossPoint Reader**.

Firmware version: **v1.0.2**

> Runs on ESP32-C3 (X4 and X3), with full web server support, Wi‑Fi features, OTA updates, and extended reading functionality.

## What this project is

InkPoint X inherits the core architecture and baseline features from CrossPoint and adds/improves:

- full **FB2** format support
- full **PDF** support
- improved reading and status flow (including reading statistics and photo frame enhancements already integrated)
- cleaner, stable versioning (`v1.0.2`) and dedicated release publishing in a separate repository

## Main differences vs CrossPoint and CrossInk

| Feature | CrossPoint | CrossInk | InkPoint X |
| --- | --- | --- | --- |
| EPUB / TXT / BMP | ✅ | ✅ | ✅ |
| FB2 | Partial/indirect | Partial (depends on build) | ✅ (complete local support for import, markup handling, attachments, and navigation) |
| PDF | ❌/Limited | ❌/Limited | ✅ (full page rendering, zoom/navigation, and caching) |
| OTA updates | ✅ | ✅ (implementation varies) | ✅ (via GitHub releases of `yokki-vans/inkpointx`) |
| Reading stats | limited | extended | ✅ (integrated and adapted from CrossInk) |
| Photo frame | missing / experimental | missing | ✅ (built-in app in the main menu) |
| Branding/logo | CrossPoint | CrossInk | InkPoint X |
| Default version format | `1.x-dev-*` for dev builds | varies | `v1.0.2` (stable) |

### FB2 support

- proper document parsing and chapter/section splitting
- metadata extraction and storage
- support for attachments and images
- reading position and bookmark navigation
- correct cache/progress recalculation on device

### PDF support

- native PDF document loading and rendering
- page-by-page navigation
- on-device scaling for comfortable reading
- caching for faster repeat opens

## OTA updates

The firmware already includes a full OTA client:

- checks for new releases on GitHub
- downloads `firmware.bin` from release assets
- shows install progress
- compares versions and prevents downgrade

The release source is:

- `https://api.github.com/repos/yokki-vans/inkpointx/releases/latest`

And published releases are here:

- https://github.com/yokki-vans/inkpointx/releases

## Installation and updating

### Quick install

1. Connect the device via USB‑C and wake it up.
2. Use your usual USB/web flasher workflow.
3. Download `firmware.bin` from `yokki-vans/inkpointx` releases and flash it.

### First-time flash for new users (no special flashing software)

If your device has not been flashed yet, this is the simplest safe method:

1. Download the latest `firmware.bin` from the release page (the same as before).
2. Insert a microSD card into your computer and format it as FAT32 (most memory cards are already). 
3. Copy only one file to the card root: `firmware.bin`.
4. Unmount/eject the card and insert it into the reader.
5. Turn off the reader, then hold the **left side button (UP)** while turning it on (or while pressing Power).
6. The screen will enter **Recovery Mode** and open **SD Card Firmware Update**.
7. Select `firmware.bin`, confirm **Update firmware?**, then wait until the process finishes.
8. Reboot happens automatically. Do not remove power during the process.

This method works even from a stock reader and does not require any PC flashing utility.

### OTA from device menu

In the device Settings menu, open update check:

- Wi‑Fi → Check for updates
- If a newer version is available, choose Update

### Command line (optional)

```bash
pip install esptool
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 firmware.bin
```

Replace `/dev/ttyACM0` with your device port.

## What else is included

- EPUB/XTC/TXT support and extended format handling
- device web interface and network operations
- WebDAV and network file access
- Wi‑Fi AP/STA modes
- OPDS and remote library control
- custom font loading from SD card
- InkPoint X logo/branding on the screen

## Build from source

```bash
git clone --recursive https://github.com/yokki-vans/inkpointx
cd inkpointx
pio run --target upload
```

For binary-only local build:

```bash
git submodule update --init --recursive
pio run
```

## Versioning and troubleshooting

In local/dev builds, the version shown in logs is now always `v1.0.2` (no `-dev-*` suffix), which is required for consistent OTA comparison.

If a new book is not detected on device:

- verify filename and supported extension
- re-run indexing in the file browser UI after SD card structure changes

## Project origin

InkPoint X is a fork of CrossPoint Reader. The core codebase, project structure, and rendering model come from CrossPoint, then were extended for:

- expanded format support (FB2 + PDF),
- improved reading UX flow,
- dedicated branding and versioning,
- independent OTA update pipeline for this fork.

## Licenses and attribution

This remains an open-source project and includes components from CrossPoint and the broader community under their respective licenses.

## Support InkPoint X

If you want to support the project, you can contribute via Ko‑fi:

- [https://ko-fi.com/yokkivans](https://ko-fi.com/yokkivans)

## Contributing

If you want to extend the project, add formats, or improve features, continue via fork, issues, or PRs in `yokki-vans/inkpointx`.
