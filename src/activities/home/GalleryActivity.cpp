#include "GalleryActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <string_view>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/util/BmpViewerActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void GalleryActivity::onEnter() {
  Activity::onEnter();
  fileNameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  selectedIndex = 0;
  scanImages();
  requestUpdate();
}

void GalleryActivity::onExit() {
  Activity::onExit();
  images.clear();
  fileNameBuffer.reset();
}

void GalleryActivity::scanImages() {
  images.clear();
  if (!fileNameBuffer) return;

  std::vector<std::string> directories;
  directories.reserve(16);
  directories.emplace_back("/");

  while (!directories.empty() && images.size() < MAX_IMAGES) {
    std::string directory = std::move(directories.back());
    directories.pop_back();

    auto root = Storage.open(directory.c_str());
    if (!root || !root.isDirectory()) continue;
    root.rewindDirectory();

    for (auto entry = root.openNextFile(); entry && images.size() < MAX_IMAGES; entry = root.openNextFile()) {
      entry.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
      const char* name = fileNameBuffer.get();
      if (name[0] == '\0' || strcmp(name, "System Volume Information") == 0 || strcmp(name, ".crosspoint") == 0 ||
          (!SETTINGS.showHiddenFiles && name[0] == '.')) {
        entry.close();
        continue;
      }

      std::string fullPath = directory;
      if (fullPath.back() != '/') fullPath += '/';
      fullPath += name;
      if (entry.isDirectory()) {
        entry.close();
        directories.push_back(std::move(fullPath));
        continue;
      }
      entry.close();

      const std::string_view imageName{name};
      if (FsHelpers::hasBmpExtension(imageName) || FsHelpers::hasJpgExtension(imageName) ||
          FsHelpers::hasPngExtension(imageName)) {
        images.push_back(std::move(fullPath));
      }
    }
    root.close();
  }

  std::sort(images.begin(), images.end());
}

std::string GalleryActivity::displayName(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  const size_t start = slash == std::string::npos ? 0 : slash + 1;
  return path.substr(start);
}

std::string GalleryActivity::displayFolder(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos || slash == 0) return "/";
  return path.substr(0, slash);
}

void GalleryActivity::loop() {
  const int count = static_cast<int>(images.size());
  if (count > 0) {
    buttonNavigator.onNext([this, count] {
      selectedIndex = ButtonNavigator::nextIndex(static_cast<int>(selectedIndex), count);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this, count] {
      selectedIndex = ButtonNavigator::previousIndex(static_cast<int>(selectedIndex), count);
      requestUpdate();
    });
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && !images.empty()) {
    startActivityForResult(
        std::make_unique<BmpViewerActivity>(renderer, mappedInput, images[selectedIndex], true, false),
        [this](const ActivityResult&) { requestUpdate(true); });
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::GALLERY);
  }
}

void GalleryActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_GALLERY));
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (images.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, tr(STR_NO_IMAGES));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(images.size()),
        static_cast<int>(selectedIndex), [this](int index) { return displayName(images[index]); },
        [this](int index) { return displayFolder(images[index]); }, [](int) { return UIIcon::Image; }, nullptr, false);
    GUI.drawFooterCounter(renderer, static_cast<int>(selectedIndex), static_cast<int>(images.size()));
  }

  const auto labels =
      mappedInput.mapLabels(tr(STR_HOME), images.empty() ? "" : tr(STR_OPEN), images.empty() ? "" : tr(STR_DIR_UP),
                            images.empty() ? "" : tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
