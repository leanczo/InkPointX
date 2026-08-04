#include "DictionaryPickerActivity.h"

#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace {
constexpr char DICTIONARIES_ROOT[] = "/dictionaries";

bool hasExtension(const std::string& name, const char* extension) {
  const size_t n = strlen(extension);
  if (name.size() < n) return false;
  for (size_t i = 0; i < n; ++i) {
    if (std::tolower(static_cast<unsigned char>(name[name.size() - n + i])) !=
        std::tolower(static_cast<unsigned char>(extension[i]))) return false;
  }
  return true;
}

bool folderLooksLikeDictionary(const std::string& folder) {
  bool hasIfo = false;
  bool hasIdx = false;
  bool hasDict = false;
  for (const String& entry : Storage.listFiles(folder.c_str())) {
    const std::string name = entry.c_str();
    if (name.empty() || name.front() == '.') continue;
    hasIfo = hasIfo || hasExtension(name, ".ifo");
    hasIdx = hasIdx || hasExtension(name, ".idx");
    hasDict = hasDict || hasExtension(name, ".dict");
  }
  return hasIfo && hasIdx && hasDict;
}
}  // namespace

void DictionaryPickerActivity::onEnter() {
  Activity::onEnter();
  scan();
  requestUpdate();
}

void DictionaryPickerActivity::scan() {
  folders_.clear();
  Storage.mkdir(DICTIONARIES_ROOT);
  HalFile root = Storage.open(DICTIONARIES_ROOT);
  if (root && root.isDirectory()) {
    for (HalFile entry = root.openNextFile(); entry; entry = root.openNextFile()) {
      if (!entry.isDirectory()) {
        entry.close();
        continue;
      }
      char nameBuffer[128]{};
      entry.getName(nameBuffer, sizeof(nameBuffer));
      entry.close();
      const std::string name = nameBuffer;
      if (name.empty() || name.front() == '.') continue;
      const std::string full = std::string(DICTIONARIES_ROOT) + "/" + name;
      if (folderLooksLikeDictionary(full)) folders_.push_back(name);
    }
  }
  root.close();
  std::sort(folders_.begin(), folders_.end());
  const auto selected = std::find(folders_.begin(), folders_.end(), std::string(SETTINGS.dictionaryFolder));
  selectedIndex_ = selected == folders_.end() ? 0 : static_cast<int>(std::distance(folders_.begin(), selected));
}

void DictionaryPickerActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (folders_.empty()) return;
  navigator_.onNextPress([this] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, static_cast<int>(folders_.size()));
    requestUpdate();
  });
  navigator_.onPreviousPress([this] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, static_cast<int>(folders_.size()));
    requestUpdate();
  });
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    strncpy(SETTINGS.dictionaryFolder, folders_[selectedIndex_].c_str(), sizeof(SETTINGS.dictionaryFolder) - 1);
    SETTINGS.dictionaryFolder[sizeof(SETTINGS.dictionaryFolder) - 1] = '\0';
    SETTINGS.saveToFile();
    finish();
  }
}

void DictionaryPickerActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_CHOOSE_DICTIONARY));
  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = UITheme::getListContentBottom(renderer, true);
  if (folders_.empty()) {
    GUI.drawEmptyState(renderer, Rect{screen.x, contentTop, screen.width, contentBottom - contentTop},
                       tr(STR_NO_DICTIONARIES), tr(STR_DICTIONARY_INSTALL_HINT));
  } else {
    GUI.drawList(renderer, Rect{screen.x, contentTop, screen.width, contentBottom - contentTop}, folders_.size(),
                 selectedIndex_, [this](int index) { return folders_[index].c_str(); }, nullptr, nullptr, nullptr,
                 false, nullptr, [this](int index) {
                   return folders_[index] == SETTINGS.dictionaryFolder ? UIAccessory::Check : UIAccessory::None;
                 });
    GUI.drawFooterCounter(renderer, selectedIndex_, static_cast<int>(folders_.size()));
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), folders_.empty() ? "" : tr(STR_SELECT), tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
