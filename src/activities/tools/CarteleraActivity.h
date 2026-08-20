#pragma once

#include <HalStorage.h>

#include <string>
#include <vector>

#include "activities/Activity.h"

struct CarteleraMovie {
  std::string slug;       // "spider-man-un-nuevo-dia" - used to build the pelicula/ URL
  std::string title;      // "Spider-man: Un Nuevo Día" - title-cased from the site's ALL-CAPS alt text
  std::string posterUrl;  // "https://cdn.cinemark.com.ar/content/posters/HO00012566.jpg"
  // Empty until the user opens "Sinopsis" for this movie - fetched on demand
  // from the movie's own page (see fetchSynopsis()), not from the homepage.
  std::string synopsis;
};

// Cinemark Hoyts Argentina has no public showtimes API: the real one
// (bff.cinemark.com.ar/cinema/showtimes) is not reachable from outside the
// site's own browser session - verified from a Salta residential IP, every
// request (with matching browser headers) came back HTTP 502 in under half a
// second, consistent with an edge-level allowlist rather than a slow/broken
// origin. What *is* publicly reachable is the plain server-rendered homepage,
// which lists the movies currently in cartelera as plain <a href="/pelicula/
// <slug>"><img alt="TITLE" ...> markup - no JS execution needed to read it.
//
// So this activity only shows the national "en cartelera" list, not times or
// per-cine availability (Salta has two Cinemark Hoyts locations and the movie
// mix isn't split between them here). Confirm on a title hands the movie's
// page to the user's phone as a QR - same "no on-device browser" pattern
// FootballActivity/SismosActivity already use - where the phone's own browser
// session can load real showtimes for whichever Salta cine the user picks.
//
// Poster thumbnails come from a separate, unauthenticated static asset CDN
// (cdn.cinemark.com.ar) - unlike bff.cinemark.com.ar this one is meant to be
// hit directly by <img> tags in every visitor's browser, and was reachable in
// testing. GUI.drawList has no per-row image slot (it only places baked-in
// UIIcon glyphs), so this activity hand-rolls a small paginated list instead
// of reusing it - see render() for the row layout.
class CarteleraActivity final : public Activity {
 private:
  bool loaded = false;
  bool refreshing = false;
  // Set when a manual refresh fails while `loaded` was already true, so the
  // old list stays on screen but the user still sees that it didn't update.
  bool refreshFailed = false;
  std::string errorMessage;
  std::vector<CarteleraMovie> movies;
  int selectedRow = 0;

  // Set once a fetch actually reaches HttpDownloader, so onExit() only pays
  // for a heap-defrag reboot when this session actually used WiFi.
  bool wifiWasUsed = false;

  // Whether the synopsis detail view (instead of the movie list) is on
  // screen. Back from here returns to the list, not to the home menu.
  bool showingSynopsis = false;
  bool synopsisLoading = false;
  bool synopsisFetchFailed = false;

  void startFetch();
  void doFetch();
  void fetchPosters();
  bool loadCacheFromSd();
  void parseAndStore(HalFile& file);
  static std::string cachePath();
  static std::string tmpPath();
  static std::string homeUrl();
  static std::string movieUrl(const std::string& slug);
  static std::string posterPath(const std::string& slug);
  void showTicketsForSelected();
  void startSynopsisFetch();
  void doFetchSynopsis();

 public:
  explicit CarteleraActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Cartelera", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
