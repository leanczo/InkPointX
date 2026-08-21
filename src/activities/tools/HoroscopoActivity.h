#pragma once

#include <string>

#include "activities/Activity.h"

enum class HoroscopeState { SignSelect, Loading, Result };

// Network fetch is synchronous, same pattern as every other tools/ activity
// -- see PersonalTrackerActivity. The response (an RSS <item> picked out of
// horoscopo-del-dia.com's daily feed) is parsed straight into memory, same
// reasoning as FraseDelDiaActivity (a stale horoscope has no value, so no SD
// cache/offline fallback). The chosen sign itself IS persisted (in
// APP_STATE.lastHoroscopeSignIndex, unlike FraseDelDiaActivity's category)
// so reopening the activity starts back on the last-consulted sign.
class HoroscopoActivity final : public Activity {
 private:
  HoroscopeState state = HoroscopeState::SignSelect;
  int selectedSignIndex = 0;
  bool loaded = false;
  bool refreshing = false;
  bool refreshFailed = false;
  std::string errorMessage;
  std::string horoscopeText;

  int detailScrollOffset = 0;
  int detailMaxLines = 1;

  bool wifiWasUsed = false;

  // Locally generated (not fetched -- no source publishes this over RSS), from
  // a deterministic hash of the wall-clock date and sign so it's stable across
  // re-renders and only changes once a day. -1 while the device has no valid
  // clock to seed it (see updateLuckyLine()).
  int luckyNumber = -1;
  std::string luckyColorText;

  void startFetch();
  void doFetch();
  void updateLuckyLine();

 public:
  explicit HoroscopoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Horoscopo", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
