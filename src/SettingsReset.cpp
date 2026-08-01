#include "SettingsReset.h"

#include <HalStorage.h>

#include "KOReaderCredentialStore.h"
#include "SilentRestart.h"
#include "WifiCredentialStore.h"

void resetFirmwareConfiguration() {
  constexpr const char* configurationFiles[] = {
      "/.crosspoint/settings.json", "/.crosspoint/settings.bin",     "/.crosspoint/settings.bin.bak",
      "/.crosspoint/language.bin",  "/.crosspoint/language.bin.bak", "/.crosspoint/wifi.json",
      "/.crosspoint/wifi.bin",      "/.crosspoint/wifi.bin.bak",     "/.crosspoint/opds.json",
      "/.crosspoint/koreader.json", "/.crosspoint/koreader.bin",     "/.crosspoint/koreader.bin.bak",
  };
  for (const char* path : configurationFiles) Storage.remove(path);

  // Forget the credentials still held in RAM. Deleting the files alone left them
  // live for the rest of the session, and any store that saved afterwards would
  // write them straight back over the reset — silentRestart() below early-returns
  // when the device is already committed to sleeping, so the reboot that would
  // otherwise clear memory is not guaranteed.
  WIFI_STORE.clearAll();
  KOREADER_STORE.clearCredentials();

  silentRestart();
}
