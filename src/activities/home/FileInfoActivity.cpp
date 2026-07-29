#include "FileInfoActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"

void FileInfoActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void FileInfoActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
  }
}

void FileInfoActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const std::array<std::string, 3> labels = {tr(STR_FILENAME), tr(STR_ITEM_TYPE), tr(STR_FILE_SIZE)};
  const size_t slash = path.find_last_of('/');
  const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  const std::array<std::string, 3> values = {
      name,
      directory ? std::string(tr(STR_FOLDER)) : std::string(tr(STR_FILE)),
      directory ? std::string("-") : std::to_string(size) + " " + tr(STR_BYTES),
  };

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_PROPERTIES));
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(labels.size()), -1,
      [&labels](int index) { return labels[index]; }, nullptr, nullptr, [&values](int index) { return values[index]; },
      false);
  const auto hints = mappedInput.mapLabels(tr(STR_BACK), tr(STR_BACK), "", "");
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
  renderer.displayBuffer();
}
