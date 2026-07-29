#include "BookInfoActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>

#include "components/UITheme.h"

void BookInfoActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void BookInfoActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    finish();
  }
}

void BookInfoActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_BOOK_INFO));

  std::string format;
  const auto dot = path.find_last_of('.');
  if (dot != std::string::npos) {
    format = path.substr(dot + 1);
    std::transform(format.begin(), format.end(), format.begin(),
                   [](const unsigned char c) { return static_cast<char>(std::toupper(c)); });
  }

  char pageValue[32];
  snprintf(pageValue, sizeof(pageValue), "%d / %d", currentPage, totalPages);
  char progressValue[16];
  snprintf(progressValue, sizeof(progressValue), "%d%%", progressPercent);
  const std::array<const char*, 7> labels = {tr(STR_TITLE), tr(STR_AUTHOR),   tr(STR_FORMAT),   tr(STR_LANGUAGE),
                                             tr(STR_PAGE),  tr(STR_PROGRESS), tr(STR_FILE_PATH)};
  const std::array<std::string, 7> values = {title,     author.empty() ? tr(STR_NOT_SET) : author,
                                             format,    language.empty() ? tr(STR_NOT_SET) : language,
                                             pageValue, progressValue,
                                             path};

  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  GUI.drawList(
      renderer,
      Rect{0, top, width, height - top - metrics.buttonHintsHeight - metrics.verticalSpacing},
      static_cast<int>(labels.size()), -1,
      [&labels](const int index) { return std::string(labels[index]); }, nullptr, nullptr,
      [&values](const int index) { return values[index]; }, false);

  const auto buttonLabels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_BACK), "", "");
  GUI.drawButtonHints(renderer, buttonLabels.btn1, buttonLabels.btn2, buttonLabels.btn3, buttonLabels.btn4);
  renderer.displayBuffer();
}
