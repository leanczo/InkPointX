#include "NotesActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "MappedInputManager.h"
#include "NotesChecklistActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* kNotesDir = "/notes";
constexpr size_t kNameBufferSize = 256;
// Notes are user-authored checklists/snippets, not a book library -- this is
// a generous cap against a stray huge folder, not an expected real count.
constexpr size_t kMaxListedFiles = 500;
}  // namespace

void NotesActivity::loadFiles() {
  files.clear();
  files.reserve(16);

  auto dir = Storage.open(kNotesDir);
  if (!dir || !dir.isDirectory()) return;

  dir.rewindDirectory();

  auto nameBuffer = makeUniqueNoThrow<char[]>(kNameBufferSize);
  if (!nameBuffer) {
    LOG_ERR("Notes", "nameBuffer allocation failed");
    dir.close();
    return;
  }

  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (files.size() >= kMaxListedFiles) {
      file.close();
      break;
    }
    file.getName(nameBuffer.get(), kNameBufferSize);
    const bool isDirectory = file.isDirectory();
    file.close();
    if (isDirectory || nameBuffer[0] == '.') continue;  // flat listing only, skip hidden files

    std::string_view filename{nameBuffer.get()};
    if (FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename) ||
        FsHelpers::hasEpubExtension(filename)) {
      files.emplace_back(filename);
    }
  }
  dir.close();
  FsHelpers::sortFileList(files);
}

void NotesActivity::openSelected() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(files.size())) return;
  const std::string fullPath = std::string(kNotesDir) + "/" + files[static_cast<size_t>(selectedIndex)];

  if (FsHelpers::hasEpubExtension(fullPath)) {
    onSelectBook(fullPath);
    return;
  }

  startActivityForResult(makeUniqueNoThrow<NotesChecklistActivity>(renderer, mappedInput, fullPath),
                         [this](const ActivityResult&) {
                           loadFiles();
                           if (selectedIndex >= static_cast<int>(files.size())) {
                             selectedIndex = std::max(0, static_cast<int>(files.size()) - 1);
                           }
                           requestUpdate();
                         });
}

void NotesActivity::onEnter() {
  Activity::onEnter();
  Storage.ensureDirectoryExists(kNotesDir);
  loadFiles();
  selectedIndex = 0;
  requestUpdate();
}

void NotesActivity::loop() {
  using Button = MappedInputManager::Button;

  if (mappedInput.wasReleased(Button::Back)) {
    onGoHome(HomeMenuItem::TOOLS_MENU);
    return;
  }

  if (mappedInput.wasPressed(Button::Confirm)) {
    openSelected();
    return;
  }

  const int itemCount = static_cast<int>(files.size());
  if (itemCount > 0) {
    buttonNavigator.onNext([this, itemCount] {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this, itemCount] {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount);
      requestUpdate();
    });
  }
}

void NotesActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_NOTES_TITLE));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  if (files.empty()) {
    renderer.drawCenteredText(UI_12_FONT_ID, contentTop + contentHeight / 2, tr(STR_NOTES_EMPTY), true);
  } else {
    GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(files.size()),
                selectedIndex, [this](int index) { return files[static_cast<size_t>(index)]; });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
