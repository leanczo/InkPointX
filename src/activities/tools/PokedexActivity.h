#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"

enum class PokedexState { Landing, Loading, Detail };

// Precomputed so the "recently viewed" landing list never needs a network
// round-trip just to render its rows.
struct PokedexRecentEntry {
  int id = 0;
  std::string displayName;  // e.g. "#025 Pikachu", already localized
};

struct PokedexEntry {
  int id = 0;
  std::string name;                    // localized (or English fallback) display name
  std::vector<std::string> types;      // English slugs (e.g. "grass") - PokeAPI has no cheap localized type name
  int heightDm = 0;                    // decimetres, as returned by the API
  int weightHg = 0;                    // hectograms, as returned by the API
  int stats[6] = {0, 0, 0, 0, 0, 0};   // HP, Attack, Defense, Sp.Atk, Sp.Def, Speed
  std::string flavorText;              // localized (or English fallback) Pokedex description
};

// Fetches Pokemon data from PokeAPI (pokeapi.co, public, no key) on demand --
// storing the ~1300-entry Pokedex offline isn't practical on this device.
// Network fetches are synchronous (block the main loop behind a "Loading"
// state, same as every other online tool in this codebase) instead of a
// background-FreeRTOS-task pattern.
//
// There is no prior art to port here: the sibling crosspoint-reader-apps
// project has no Pokemon/Pokedex activity at all. This is a from-scratch
// design, structurally modeled on FootballActivity (list/detail over an API)
// and OnThisDayActivity (Prev/Next navigation, per-language text fallback).
//
// The sprite is the one new piece of infrastructure: it downloads a PNG,
// converts it once with PngToBmpConverter (already used for EPUB cover art)
// into a small cached 1-bit .bmp, and draws it with GfxRenderer::drawBitmap1Bit
// -- no new image decoder needed.
class PokedexActivity final : public Activity {
 private:
  PokedexState state = PokedexState::Landing;
  std::vector<PokedexRecentEntry> recent;
  PokedexEntry current;
  bool hasCurrent = false;
  bool hasSprite = false;

  // Set by a failed doFetch() or a cancelled WiFi prompt; shown as a
  // transient popup on top of whichever screen the failure leaves the user
  // on (Landing for a first-ever failed search, Detail for a failed
  // Prev/Next past the edge of the Pokedex). Empty means no popup.
  std::string errorPopupMessage;
  // The name or id string currently being fetched (stashed across the async
  // WiFi-connect prompt, same as WikipediaActivity::articleToFetch).
  std::string pendingQuery;

  int selectedIndex = 0;
  bool wifiWasUsed = false;

  void loadRecent();
  void saveRecent();
  void rememberRecent(int id, const std::string& displayName);

  void promptSearch();
  void openByQuery(const std::string& query);  // search flow: always fetches
  void openById(int id);                       // nav/recent flow: cache-first
  void ensureWifiThenFetch();
  void doFetch();
  bool fetchAndParse(PokedexEntry& outEntry);
  bool fetchSprite(int id, const std::string& spriteUrl);
  bool loadCacheFromSd(int id);
  bool saveCacheToSd(const PokedexEntry& entry) const;
  std::string cachePath(int id) const;
  std::string spritePath(int id) const;

  void drawSprite(int x, int y, int boxSize) const;

 public:
  explicit PokedexActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Pokedex", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
