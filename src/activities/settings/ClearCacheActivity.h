#pragma once

#include <functional>

#include "activities/Activity.h"

class ClearCacheActivity final : public Activity {
 public:
  explicit ClearCacheActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ClearCache", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  // Full loop speed only while actually clearing — the unconditional version
  // busy-spun at 160 MHz while the user merely read the warning screen.
  bool skipLoopDelay() override { return state == CLEARING; }
  void render(RenderLock&&) override;

 private:
  enum State { WARNING, CLEARING, SUCCESS, FAILED };

  State state = WARNING;

  void goBack() { finish(); }

  int clearedCount = 0;
  int failedCount = 0;
  void clearCache();
};
