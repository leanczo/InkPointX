#pragma once

#include <cstdint>

// Post-mortem breadcrumbs for field failures.
//
// On battery the X4 has no console: the latch MOSFET removes power from the
// whole chip when deep sleep opens it, so RTC memory does not survive either.
// Everything we can learn about a device that "froze at a random moment" has
// to be on the SD card *before* it happens — which screen was up, how long
// the session had run, and above all whether the firmware asked for the
// power-down or the power simply vanished.
//
// The next boot reads that marker, pairs it with esp_reset_reason() and writes
// one line to /.crosspoint/diag.log (and to the serial log, so plugging USB in
// after a failure reports the previous session immediately). The pairing is
// what makes it a diagnosis rather than a hint:
//
//   clean + power-on  -> the firmware chose to sleep (button, idle timeout)
//   dirty + power-on  -> power vanished: brownout, flat pack, latch glitch
//   dirty + task-wdt  -> the main loop wedged and the watchdog rebooted us
//   dirty + panic     -> a crash, with the screen it happened on
namespace BootDiag {

enum class Shutdown : uint8_t {
  Unexpected = 0,   // never written; the absence of a reason *is* the finding
  PowerButton = 1,  // the user's sleep gesture
  IdleTimeout = 2,  // auto-sleep after inactivity
  Restart = 3,      // deliberate ESP.restart(): heap defrag, OTA, SD update
};

// Reports the previous session, then arms a fresh marker for this one.
// Call once, after storage is up.
void begin();

// Records the screen the user is on. Called from every activity install, so
// the write rate is bounded by how fast a person can navigate.
void noteScreen(const char* screen);

// The power-down that follows was asked for. Call immediately before deep
// sleep or a restart.
void markCleanShutdown(Shutdown reason);

}  // namespace BootDiag
