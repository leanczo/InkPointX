#pragma once

#include "activities/Activity.h"
#include "network/OtaUpdater.h"

class OtaUpdateActivity : public Activity {
  enum State {
    WIFI_SELECTION,
    CHECKING_FOR_UPDATE,
    WAITING_CONFIRMATION,
    UPDATE_IN_PROGRESS,
    NO_UPDATE,
    FAILED,
    FINISHED,
    SHUTTING_DOWN
  };

  // Can't initialize this to 0 or the first render doesn't happen
  static constexpr unsigned int UNINITIALIZED_PERCENTAGE = 111;

  State state = WIFI_SELECTION;
  const char* failureReason = nullptr;
  static const char* failureText(int result);
  int lastUpdaterPercentage = UNINITIALIZED_PERCENTAGE;
  OtaUpdater::Phase lastUpdaterPhase = OtaUpdater::Phase::IDLE;
  OtaUpdater updater;

  void onWifiSelectionComplete(bool success);

 public:
  explicit OtaUpdateActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("OtaUpdate", renderer, mappedInput), updater() {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state == CHECKING_FOR_UPDATE || state == WAITING_CONFIRMATION || state == UPDATE_IN_PROGRESS;
  }
  bool skipLoopDelay() override {
    return state == CHECKING_FOR_UPDATE || state == UPDATE_IN_PROGRESS || state == SHUTTING_DOWN;
  }
};
