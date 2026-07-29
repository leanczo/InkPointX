#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class FileBrowserActivity final : public Activity {
 public:
  // Picker modes filter files and return the selected path via ActivityResult.
  enum class Mode { Manager, PickBook, PickFirmware, PickSleepImage };

 private:
  // File operations
  bool removeDirFile(const std::string& fullPath);
  bool copyDirFile(const std::string& sourcePath, const std::string& destinationDirectory);
  bool copyFile(const std::string& sourcePath, const std::string& destinationPath);
  bool moveDirFile(const std::string& sourcePath, const std::string& destinationDirectory);
  bool renameDirFile(const std::string& sourcePath, const std::string& destinationPath);
  void migrateBookState(const std::string& oldPath, const std::string& newPath);
  void openEntry(const std::string& fullPath, bool isDirectory);
  void showActions(const std::string& entry);
  void createFolder();
  void renameEntry(const std::string& fullPath);
  void deleteEntry(const std::string& fullPath, const std::string& entry);
  void chooseDestination(const std::string& fullPath, bool move);
  void showOperationMessage(const char* message);
  static bool isValidName(const std::string& name);
  static std::string joinPath(const std::string& directory, const std::string& name);
  static std::string baseName(const std::string& path);

  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;

  bool lockLongPressBack = false;
  // True when this activity was entered while Confirm was already held; we must swallow the next
  // release so we don't immediately auto-open the first entry.
  bool lockNextConfirmRelease = false;

  Mode mode = Mode::Manager;

  // Files state
  std::string basepath = "/";
  std::vector<std::string> files;
  std::unique_ptr<char[]> fileNameBuffer;

  // Data loading
  void loadFiles();
  size_t findEntry(const std::string& name) const;

 public:
  explicit FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/",
                               Mode mode = Mode::Manager)
      : Activity("FileBrowser", renderer, mappedInput),
        mode(mode),
        basepath(initialPath.empty() ? "/" : std::move(initialPath)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
