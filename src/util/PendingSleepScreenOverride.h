#pragma once

#include <cstdint>

// Persists a one-shot override of SETTINGS.sleepScreen across a single deep
// sleep cycle. Deep sleep on this device is a full chip reset -- nothing in
// RAM survives it -- so "use this sleep screen just for the next sleep, then
// go back to normal" has to be written to disk and consumed again at the
// next boot; there's no in-memory way to remember it.
namespace PendingSleepScreenOverride {

// Saves the current SETTINGS.sleepScreen value to disk as the value to
// restore later, then switches it to `temporaryMode` and persists that too,
// so the very next deep sleep (however it ends up being triggered) uses
// `temporaryMode`.
void arm(uint8_t temporaryMode);

// Called once at boot, right after SETTINGS.loadFromFile(). If an override
// is pending, restores the saved sleepScreen value, persists it, and clears
// the pending marker. A no-op if nothing is pending.
void consumeIfPending();

}  // namespace PendingSleepScreenOverride
