#include "CrossPointState.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>

namespace {
constexpr uint8_t STATE_FILE_VERSION = 4;
constexpr char STATE_FILE_BIN[] = "/.crosspoint/state.bin";
constexpr char STATE_FILE_JSON[] = "/.crosspoint/state.json";
constexpr char STATE_FILE_BAK[] = "/.crosspoint/state.bin.bak";
}  // namespace

CrossPointState CrossPointState::instance;

bool CrossPointState::isRecentSleep(uint16_t idx, uint8_t checkCount) const {
  const uint8_t effectiveCount = std::min(checkCount, recentSleepFill);
  for (uint8_t i = 0; i < effectiveCount; i++) {
    const uint8_t slot = (recentSleepPos + SLEEP_RECENT_COUNT - 1 - i) % SLEEP_RECENT_COUNT;
    if (recentSleepImages[slot] == idx) return true;
  }
  return false;
}

void CrossPointState::pushRecentSleep(uint16_t idx) {
  recentSleepImages[recentSleepPos] = idx;
  recentSleepPos = (recentSleepPos + 1) % SLEEP_RECENT_COUNT;
  if (recentSleepFill < SLEEP_RECENT_COUNT) recentSleepFill++;
}

bool CrossPointState::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveState(*this, STATE_FILE_JSON);
}

bool CrossPointState::loadFromFile() {
  // Try JSON first
  // An atomic write interrupted between its remove and its rename leaves the
  // payload under "<path>.tmp" and no canonical file, which every exists()
  // gate below would read as "the user has no saved data". Claim it first.
  Storage.recoverInterruptedWrite(STATE_FILE_JSON);
  if (Storage.exists(STATE_FILE_JSON)) {
    String json = Storage.readFile(STATE_FILE_JSON);
    if (!json.isEmpty()) {
      return JsonSettingsIO::loadState(*this, json.c_str());
    }
  }

  // Fall back to binary migration
  if (Storage.exists(STATE_FILE_BIN)) {
    if (loadFromBinaryFile()) {
      if (saveToFile()) {
        Storage.rename(STATE_FILE_BIN, STATE_FILE_BAK);
        LOG_DBG("CPS", "Migrated state.bin to state.json");
        return true;
      } else {
        LOG_ERR("CPS", "Failed to save state during migration");
        return false;
      }
    }
  }

  return false;
}

bool CrossPointState::loadFromBinaryFile() {
  HalFile inputFile;
  if (!Storage.openFileForRead("CPS", STATE_FILE_BIN, inputFile)) {
    return false;
  }

  uint8_t version = 0;
  if (!serialization::readPod(inputFile, version) || version == 0 || version > STATE_FILE_VERSION) {
    LOG_ERR("CPS", "Deserialization failed: Unknown version %u", version);
    return false;
  }

  std::string loadedPath;
  if (!serialization::readString(inputFile, loadedPath)) return false;
  if (version >= 2) {
    uint8_t legacyLastSleep = UINT8_MAX;
    if (!serialization::readPod(inputFile, legacyLastSleep)) return false;
    if (legacyLastSleep != UINT8_MAX) {
      pushRecentSleep(static_cast<uint16_t>(legacyLastSleep));
    }
  }

  if (version >= 3) {
    if (!serialization::readPod(inputFile, readerActivityLoadCount)) return false;
  }

  if (version >= 4) {
    if (!serialization::readPod(inputFile, lastSleepFromReader)) return false;
  } else {
    lastSleepFromReader = false;
  }

  openEpubPath = std::move(loadedPath);
  return true;
}
