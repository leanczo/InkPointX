#include "FolderPickerActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

void FolderPickerActivity::onEnter() {
  Activity::onEnter();
  fileNameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (basepath.empty() || basepath.front() != '/' || basepath.compare(0, strlen("/.crosspoint"), "/.crosspoint") == 0) {
    basepath = "/";
  }
  auto dir = Storage.open(basepath.c_str());
  if (!dir || !dir.isDirectory()) basepath = "/";
  loadDirectories();
  selectedIndex = 0;
  requestUpdate();
}

void FolderPickerActivity::onExit() {
  Activity::onExit();
  directories.clear();
  fileNameBuffer.reset();
}

void FolderPickerActivity::loadDirectories() {
  directories.clear();
  if (!fileNameBuffer) return;

  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) return;
  root.rewindDirectory();
  for (auto entry = root.openNextFile(); entry; entry = root.openNextFile()) {
    entry.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
    const char* name = fileNameBuffer.get();
    const bool hidden = name[0] == '.';
    if (!entry.isDirectory() || name[0] == '\0' || strcmp(name, ".crosspoint") == 0 ||
        strcmp(name, "System Volume Information") == 0 || (!SETTINGS.showHiddenFiles && hidden)) {
      entry.close();
      continue;
    }
    directories.emplace_back(std::string(name) + "/");
    entry.close();
  }
  root.close();
  FsHelpers::sortFileList(directories);
}

void FolderPickerActivity::goUp() {
  if (basepath == "/") return;
  const size_t slash = basepath.find_last_of('/');
  basepath = slash == 0 ? "/" : basepath.substr(0, slash);
  loadDirectories();
  selectedIndex = 0;
  requestUpdate();
}

void FolderPickerActivity::loop() {
  const int itemCount = static_cast<int>(directories.size()) + 1;
  buttonNavigator.onNext([this, itemCount] {
    selectedIndex = ButtonNavigator::nextIndex(static_cast<int>(selectedIndex), itemCount);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, itemCount] {
    selectedIndex = ButtonNavigator::previousIndex(static_cast<int>(selectedIndex), itemCount);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedIndex == 0) {
      setResult(FilePathResult{basepath});
      finish();
      return;
    }
    if (basepath.back() != '/') basepath += '/';
    const std::string& entry = directories[selectedIndex - 1];
    basepath += entry.substr(0, entry.size() - 1);
    loadDirectories();
    selectedIndex = 0;
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (basepath != "/") {
      goUp();
    } else {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    }
  }
}

void FolderPickerActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_SELECT_THIS_FOLDER));
  const int pathHeight = renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing;
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing - pathHeight;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(directories.size()) + 1,
      static_cast<int>(selectedIndex),
      [this](int index) {
        return index == 0 ? std::string(tr(STR_SELECT_THIS_FOLDER))
                          : directories[index - 1].substr(0, directories[index - 1].size() - 1);
      },
      nullptr, [](int) { return UIIcon::Folder; }, nullptr, true);

  const int pathY = pageHeight - metrics.buttonHintsHeight - pathHeight;
  renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, pathY, basepath.c_str());
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
