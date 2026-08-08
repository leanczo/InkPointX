#pragma once

#include <string>
#include <vector>

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
  std::vector<std::string> releaseNoteLines;
  int releaseNotesPage = 0;
  int releaseNotesLinesPerPage = 1;
  int releaseNotesWrapWidth = 0;

  void onWifiSelectionComplete(bool success);
  void prepareReleaseNoteLines(int wrapWidth);

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
