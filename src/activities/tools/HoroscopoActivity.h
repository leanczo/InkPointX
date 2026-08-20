#pragma once

#include <string>

#include "activities/Activity.h"

enum class HoroscopeState { SignSelect, Loading, Result };

// Network fetch is synchronous, same pattern as every other tools/ activity
// -- see PersonalTrackerActivity. The zodiac sign is asked every time
// (deliberately not persisted) and the response is a single small JSON
// object fetched straight into memory, same reasoning as FraseDelDiaActivity
// (a stale horoscope has no value, so no SD cache/offline fallback).
class HoroscopoActivity final : public Activity {
 private:
  HoroscopeState state = HoroscopeState::SignSelect;
  int selectedSignIndex = 0;
  bool loaded = false;
  bool refreshing = false;
  bool refreshFailed = false;
  std::string errorMessage;
  std::string horoscopeText;

  bool wifiWasUsed = false;

  void startFetch();
  void doFetch();

 public:
  explicit HoroscopoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Horoscopo", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
