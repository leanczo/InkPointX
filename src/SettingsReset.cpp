#include "SettingsReset.h"

#include <HalStorage.h>

#include "SilentRestart.h"

void resetFirmwareConfiguration() {
  constexpr const char* configurationFiles[] = {
      "/.crosspoint/settings.json", "/.crosspoint/settings.bin",     "/.crosspoint/settings.bin.bak",
      "/.crosspoint/language.bin",  "/.crosspoint/language.bin.bak", "/.crosspoint/wifi.json",
      "/.crosspoint/wifi.bin",      "/.crosspoint/wifi.bin.bak",     "/.crosspoint/opds.json",
      "/.crosspoint/koreader.json", "/.crosspoint/koreader.bin",     "/.crosspoint/koreader.bin.bak",
  };
  for (const char* path : configurationFiles) Storage.remove(path);
  silentRestart();
}
