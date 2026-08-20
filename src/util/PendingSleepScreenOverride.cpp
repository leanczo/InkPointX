#include "PendingSleepScreenOverride.h"

#include <HalStorage.h>

#include "CrossPointSettings.h"

namespace {
// Lives beside settings.json under the same hidden system directory --
// system-level boot state, not data belonging to any one tool.
constexpr const char* kMarkerPath = "/.crosspoint/pending_sleep_restore.txt";
}  // namespace

namespace PendingSleepScreenOverride {

void arm(uint8_t temporaryMode) {
  const char digit = static_cast<char>('0' + SETTINGS.sleepScreen);
  HalFile f;
  if (Storage.openFileForWrite("SleepOverride", kMarkerPath, f)) {
    f.write(&digit, 1);
    f.close();
  }
  SETTINGS.sleepScreen = temporaryMode;
  SETTINGS.saveToFile();
}

void consumeIfPending() {
  if (!Storage.exists(kMarkerPath)) return;

  HalFile f;
  if (Storage.openFileForRead("SleepOverride", kMarkerPath, f)) {
    const int c = f.available() > 0 ? f.read() : -1;
    f.close();
    if (c >= '0' && c <= '9') {
      const uint8_t restored = static_cast<uint8_t>(c - '0');
      if (restored < CrossPointSettings::SLEEP_SCREEN_MODE_COUNT) {
        SETTINGS.sleepScreen = restored;
        SETTINGS.saveToFile();
      }
    }
  }
  Storage.remove(kMarkerPath);
}

}  // namespace PendingSleepScreenOverride
