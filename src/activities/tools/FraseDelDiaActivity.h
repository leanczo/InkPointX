#pragma once

#include <string>

#include "activities/Activity.h"

enum class FraseState { CategoryPicker, Loading, Result };

// Network fetch is synchronous, same pattern as every other tools/ activity
// -- see HoroscopoActivity, which this mirrors closely (a picker before the
// fetch, no persistence of the choice). The category is asked every time
// (deliberately not persisted) and the response is a small RSS/XML payload
// fetched straight into memory -- a stale quote has no value, so there's no
// SD cache/offline fallback.
class FraseDelDiaActivity final : public Activity {
 private:
  FraseState state = FraseState::CategoryPicker;
  int selectedCategoryIndex = 0;
  bool loaded = false;
  bool refreshing = false;
  bool refreshFailed = false;
  std::string errorMessage;
  std::string phraseText;

  bool wifiWasUsed = false;

  void startFetch();
  void doFetch();

 public:
  explicit FraseDelDiaActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("FraseDelDia", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
