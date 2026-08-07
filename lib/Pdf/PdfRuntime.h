#pragma once

// PDF conversion is intentionally synchronous: it borrows the display buffer
// and writes a durable EPUB-compatible cache before the reader opens it. Large
// books can therefore keep loopTask busy for several minutes. Service the task
// watchdog from bounded parser/rasterizer loops without making the host PDF
// harness depend on ESP-IDF.
#if defined(ARDUINO_ARCH_ESP32)
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace pdf_runtime {
class ScopedWatchdogPause {
  bool wasSubscribed = false;

 public:
  ScopedWatchdogPause() {
    wasSubscribed = esp_task_wdt_status(nullptr) == ESP_OK;
    if (wasSubscribed) esp_task_wdt_delete(nullptr);
  }

  ~ScopedWatchdogPause() {
    if (wasSubscribed) esp_task_wdt_add(nullptr);
  }

  ScopedWatchdogPause(const ScopedWatchdogPause&) = delete;
  ScopedWatchdogPause& operator=(const ScopedWatchdogPause&) = delete;
};

inline void serviceWatchdog() {
  static TickType_t lastService = 0;
  const TickType_t now = xTaskGetTickCount();
  if ((lastService == 0 || now - lastService >= pdMS_TO_TICKS(1000)) && esp_task_wdt_status(nullptr) == ESP_OK) {
    esp_task_wdt_reset();
    lastService = now;
  }
}
}  // namespace pdf_runtime
#else
namespace pdf_runtime {
class ScopedWatchdogPause {
 public:
  ScopedWatchdogPause() = default;
  ScopedWatchdogPause(const ScopedWatchdogPause&) = delete;
  ScopedWatchdogPause& operator=(const ScopedWatchdogPause&) = delete;
};

inline void serviceWatchdog() {}
}  // namespace pdf_runtime
#endif
