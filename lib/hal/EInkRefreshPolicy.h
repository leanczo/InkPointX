#pragma once

#include <cstdint>

// Hardware-independent refresh scheduler shared by HalDisplay and host tests.
// UI screens use fast differential updates for responsiveness. Automatic
// cleanup is opt-in: injecting an absolute waveform into normal navigation
// makes the whole panel flash, while the X4 driver already keeps RED RAM
// synchronized as the previous-frame baseline after every fast update.
// Reader activities use their explicit, user-configurable page cleanup cadence.
class EInkRefreshPolicy {
 public:
  enum class Mode : uint8_t { Full, Clean, Fast };

  static constexpr uint8_t MAX_CONSECUTIVE_FAST_REFRESHES = 8;

  Mode consume(const Mode requested) {
    Mode effective = requested;
    if (fullRequested_) {
      effective = Mode::Full;
    } else if (requested == Mode::Fast &&
               (cleanRequested_ ||
                (automaticCleanupEnabled_ && consecutiveFastRefreshes_ >= MAX_CONSECUTIVE_FAST_REFRESHES))) {
      effective = Mode::Clean;
    }

    fullRequested_ = false;
    cleanRequested_ = false;
    if (effective == Mode::Fast) {
      if (consecutiveFastRefreshes_ < UINT8_MAX) ++consecutiveFastRefreshes_;
    } else {
      consecutiveFastRefreshes_ = 0;
    }
    return effective;
  }

  void requestClean() { cleanRequested_ = true; }

  void requestFull() {
    fullRequested_ = true;
    cleanRequested_ = false;
  }

  void setAutomaticCleanupEnabled(const bool enabled) {
    automaticCleanupEnabled_ = enabled;
    if (!enabled) consecutiveFastRefreshes_ = 0;
  }

  void reset() {
    consecutiveFastRefreshes_ = 0;
    cleanRequested_ = false;
    fullRequested_ = false;
  }

  uint8_t consecutiveFastRefreshes() const { return consecutiveFastRefreshes_; }

 private:
  uint8_t consecutiveFastRefreshes_ = 0;
  bool cleanRequested_ = false;
  bool fullRequested_ = false;
  bool automaticCleanupEnabled_ = false;
};
