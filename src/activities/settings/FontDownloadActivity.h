#pragma once

#include <string>
#include <vector>

#include "FontInstaller.h"
#include "SdCardFont.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// JSON schema version of the fonts.json manifest. The canonical version for
// the build tooling lives in lib/EpdFont/scripts/cpfont_version.py. This
// firmware-side copy must be bumped manually when the firmware is updated to
// support a new manifest schema.
#define FONTS_MANIFEST_VERSION 1

#ifndef FONT_MANIFEST_URL
// Manifest + .cpfont assets are published by .github/workflows/release-fonts.yml
// to the crosspoint-fonts repo under the "sd-fonts-m<META>-b<BIN>" tag. The tag
// pattern must stay in sync with the workflow; it derives its version numbers
// from lib/EpdFont/scripts/cpfont_version.py.
#define FONT_MANIFEST_URL_STRINGIFY_INNER(x) #x
#define FONT_MANIFEST_URL_STRINGIFY(x) FONT_MANIFEST_URL_STRINGIFY_INNER(x)
#define FONT_MANIFEST_URL                                                                                           \
  "https://github.com/crosspoint-reader/crosspoint-fonts/releases/download/sd-fonts-m" FONT_MANIFEST_URL_STRINGIFY( \
      FONTS_MANIFEST_VERSION) "-b" FONT_MANIFEST_URL_STRINGIFY(CPFONT_VERSION) "/fonts.json"
#endif

class FontDownloadActivity : public Activity {
 public:
  explicit FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state_ == LOADING_MANIFEST || state_ == DOWNLOADING ||
           // The download is synchronous and blocks the main loop until it
           // completes, so activityManager.preventAutoSleep() is never polled
           // during downloading.
           state_ == COMPLETE || state_ == ERROR;
  }
  // Only the transfer states need the fast loop; browsing the family list is
  // an ordinary menu and was busy-spinning at full clock.
  bool skipLoopDelay() override { return state_ == LOADING_MANIFEST || state_ == DOWNLOADING; }

 private:
  enum State {
    WIFI_SELECTION,
    LOADING_MANIFEST,
    FAMILY_LIST,
    DOWNLOADING,
    COMPLETE,
    ERROR,
  };

  struct ManifestFile {
    std::string name;
    size_t size = 0;
    uint32_t crc32 = 0;
  };

  struct ManifestFamily {
    std::string name;
    std::string description;
    // The catalog keeps only display/status metadata. File names and CRCs are
    // loaded for one family at a time from the manifest cached on SD, so TLS
    // never has to compete with all 84 file records on the C3 heap.
    std::vector<ManifestFile> files;
    size_t fileCount = 0;
    size_t totalSize = 0;
    bool installed = false;
    bool hasUpdate = false;
  };

  State state_ = WIFI_SELECTION;
  FontInstaller fontInstaller_;
  ButtonNavigator buttonNavigator_;

  // Manifest data
  std::string baseUrl_;
  std::vector<ManifestFamily> families_;
  int selectedIndex_ = 0;

  // Download progress
  size_t currentFileIndex_ = 0;
  size_t currentFileTotal_ = 0;
  size_t fileProgress_ = 0;
  size_t fileTotal_ = 0;
  int lastNotifiedPercent_ = -1;
  unsigned long lastProgressRenderMs_ = 0;
  int downloadingFamilyIndex_ = -1;
  std::string downloadingFamilyName_;
  // What the ERROR screen's Retry should re-run.
  enum class FailedOp { None, Manifest, Download, Delete };
  FailedOp lastFailedOp_ = FailedOp::None;
  std::string errorMessage_;
  bool cancelRequested_ = false;

  void onWifiSelectionComplete(bool success);
  bool fetchAndParseManifest();
  bool loadFamilyFiles(int familyIndex, ManifestFamily& outFamily);
  void downloadFamily(int familyIndex, bool finalize = true);
  void downloadAll();
  void updateAll();
  bool showDownloadAllRow() const;
  bool showUpdateAllRow() const;
  int specialRowCount() const;
  bool isDownloadAllRow(int index) const;
  bool isUpdateAllRow(int index) const;
  bool isSelectedFamilyDeletable() const;
  void promptDeleteSelectedFamily();
  void onDeleteConfirmationResult(const ActivityResult& result);
  int familyIndexFromList(int listIndex) const { return listIndex - specialRowCount(); }
  int listItemCount() const;
  size_t totalDownloadSize() const;
  size_t totalUpdateSize() const;
  static std::string formatSize(size_t bytes);
};
