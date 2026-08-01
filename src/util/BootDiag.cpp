#include "BootDiag.h"

#include <Arduino.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_system.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr char CACHE_DIR[] = "/.crosspoint";
constexpr char MARKER_PATH[] = "/.crosspoint/session.bin";
constexpr char LOG_PATH[] = "/.crosspoint/diag.log";
constexpr uint32_t MARKER_MAGIC = 0x31445043;  // "CPD1"
// The log is a ring of one buffer: past this it starts over rather than
// carrying a partial tail around. Field reports need the last few boots, not
// the device's whole life.
constexpr size_t LOG_MAX_BYTES = 1536;

struct Marker {
  uint32_t magic;
  uint32_t uptimeSec;
  uint8_t shutdown;  // BootDiag::Shutdown
  uint8_t battery;
  char screen[18];
};

Marker current{MARKER_MAGIC, 0, static_cast<uint8_t>(BootDiag::Shutdown::Unexpected), 0, "boot"};

const char* resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
      return "power-on";
    case ESP_RST_EXT:
      return "external-pin";
    case ESP_RST_SW:
      return "software";
    case ESP_RST_PANIC:
      return "PANIC";
    case ESP_RST_INT_WDT:
      return "INTERRUPT-WDT";
    case ESP_RST_TASK_WDT:
      return "TASK-WDT";
    case ESP_RST_WDT:
      return "OTHER-WDT";
    case ESP_RST_DEEPSLEEP:
      return "deep-sleep";
    case ESP_RST_BROWNOUT:
      return "BROWNOUT";
    case ESP_RST_SDIO:
      return "sdio";
    default:
      return "unknown";
  }
}

const char* shutdownName(uint8_t reason) {
  switch (static_cast<BootDiag::Shutdown>(reason)) {
    case BootDiag::Shutdown::PowerButton:
      return "power button";
    case BootDiag::Shutdown::IdleTimeout:
      return "idle timeout";
    case BootDiag::Shutdown::Restart:
      return "restart";
    default:
      return "unexpected";
  }
}

void writeMarker() {
  if (!Storage.ready()) return;
  HalFile file;
  if (!Storage.openFileForWrite("DIAG", MARKER_PATH, file)) return;
  file.write(&current, sizeof(current));
  file.flush();
  file.close();
}

void appendLogLine(const char* line) {
  if (!Storage.ready()) return;
  std::string content;
  if (Storage.exists(LOG_PATH)) {
    char buffer[LOG_MAX_BYTES + 1] = {};
    const size_t read = Storage.readFileToBuffer(LOG_PATH, buffer, sizeof(buffer));
    if (read > 0 && read < LOG_MAX_BYTES) content.assign(buffer, read);
  }
  content += line;
  content += '\n';
  Storage.writeFile(LOG_PATH, String(content.c_str()));
}

}  // namespace

void BootDiag::begin() {
  if (!Storage.ready()) return;
  Storage.ensureDirectoryExists(CACHE_DIR);

  Marker previous{};
  bool havePrevious = false;
  {
    HalFile file;
    if (Storage.openFileForRead("DIAG", MARKER_PATH, file)) {
      havePrevious = file.read(&previous, sizeof(previous)) == static_cast<int>(sizeof(previous)) &&
                     previous.magic == MARKER_MAGIC;
      file.close();
    }
  }
  previous.screen[sizeof(previous.screen) - 1] = '\0';

  const char* reset = resetReasonName();
  char line[192];
  if (!havePrevious) {
    snprintf(line, sizeof(line), "boot: reset=%s, no previous session marker", reset);
    LOG_INF("DIAG", "%s", line);
  } else if (previous.shutdown != static_cast<uint8_t>(Shutdown::Unexpected)) {
    snprintf(line, sizeof(line), "boot: reset=%s after a clean shutdown (%s) from %s at %us, battery %u%%", reset,
             shutdownName(previous.shutdown), previous.screen, (unsigned)previous.uptimeSec, previous.battery);
    LOG_INF("DIAG", "%s", line);
  } else {
    snprintf(line, sizeof(line), "boot: reset=%s AFTER AN UNEXPECTED SHUTDOWN, last seen on %s at %us, battery %u%%",
             reset, previous.screen, (unsigned)previous.uptimeSec, previous.battery);
    LOG_ERR("DIAG", "%s", line);
  }
  appendLogLine(line);

  current.magic = MARKER_MAGIC;
  current.uptimeSec = 0;
  current.shutdown = static_cast<uint8_t>(Shutdown::Unexpected);
  current.battery = static_cast<uint8_t>(powerManager.getBatteryPercentage());
  strncpy(current.screen, "boot", sizeof(current.screen) - 1);
  writeMarker();
}

void BootDiag::noteScreen(const char* screen) {
  if (!screen || !*screen) return;
  current.uptimeSec = millis() / 1000;
  current.battery = static_cast<uint8_t>(powerManager.getBatteryPercentage());
  current.shutdown = static_cast<uint8_t>(Shutdown::Unexpected);
  strncpy(current.screen, screen, sizeof(current.screen) - 1);
  current.screen[sizeof(current.screen) - 1] = '\0';
  writeMarker();
}

void BootDiag::markCleanShutdown(const Shutdown reason) {
  current.uptimeSec = millis() / 1000;
  current.battery = static_cast<uint8_t>(powerManager.getBatteryPercentage());
  current.shutdown = static_cast<uint8_t>(reason);
  writeMarker();
}
