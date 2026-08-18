#pragma once

#include <HalStorage.h>

#include <string>
#include <vector>

#include "activities/Activity.h"

// INPRES's own "sismos.xml" color_link field: "f00" (felt in Argentina),
// "000" (seismologist-reviewed, not felt), "00f" (automatic, unreviewed -
// may still contain errors per INPRES's own legend). This device's display
// is monochrome, so the only thing carried over from that coding is which
// rows to dim (Auto) vs. show at full weight.
enum class SismoReviewState { Auto = 0, Reviewed = 1, Felt = 2 };

struct Sismo {
  std::string idSismo;   // "20260818221142" - also the id INPRES's own map page URL takes
  std::string date;      // "18/08"
  std::string time;      // "19:11"
  std::string province;  // Title-cased on parse, e.g. "San Juan"
  float magnitude = 0.0f;
  int depthKm = 0;
  SismoReviewState reviewState = SismoReviewState::Auto;
};

// Network fetch is synchronous (blocks the main loop behind a "Refreshing"
// state), same pattern as every other tools/ activity - see FootballActivity.
class SismosActivity final : public Activity {
 private:
  bool loaded = false;
  bool refreshing = false;
  // Set when a manual refresh fails while `loaded` was already true, so the
  // old list stays on screen but the user still sees that it didn't update.
  bool refreshFailed = false;
  std::string errorMessage;
  std::vector<Sismo> sismos;
  int selectedRow = 0;

  // Set once a fetch actually reaches HttpDownloader, so onExit() only pays
  // for a heap-defrag reboot when this session actually used WiFi.
  bool wifiWasUsed = false;

  void startFetch();
  void doFetch();
  bool loadCacheFromSd();
  void parseAndStore(HalFile& file);
  static std::string cachePath();
  static std::string tmpPath();
  static std::string apiUrl();
  // No on-device browser (see SCOPE.md) - hands the selected quake's INPRES
  // map page to the user's phone as a QR code, same pattern RSS/On This Day
  // already use for "open this link on a device that has a browser".
  void showMapForSelected();

 public:
  explicit SismosActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Sismos", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
