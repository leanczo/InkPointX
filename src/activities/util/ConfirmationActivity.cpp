#include "ConfirmationActivity.h"

#include <I18n.h>

#include <algorithm>

#include "HalDisplay.h"
#include "components/UITheme.h"

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body)
    : Activity("Confirmation", renderer, mappedInput), heading(heading), body(body) {}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();

  headingLineHeight = renderer.getLineHeight(headingFontId);
  bodyLineHeight = renderer.getLineHeight(bodyFontId);
  const int maxCardWidth = renderer.getScreenWidth() - (margin * 2);
  const int maxTextWidth = maxCardWidth - margin * 2;

  if (!heading.empty()) {
    safeHeading = renderer.truncatedText(headingFontId, heading.c_str(), maxTextWidth, EpdFontFamily::BOLD);
  }
  if (!body.empty()) {
    safeBodyLines = renderer.wrappedText(bodyFontId, body.c_str(), maxTextWidth, 5);
  }

  int widestText = safeHeading.empty() ? 0 : renderer.getTextWidth(headingFontId, safeHeading.c_str(),
                                                                   EpdFontFamily::BOLD);
  for (const auto& line : safeBodyLines) {
    widestText = std::max(widestText, renderer.getTextWidth(bodyFontId, line.c_str()));
  }
  cardWidth = std::min(maxCardWidth, std::max(300, widestText + margin * 2));
  cardHeight = margin * 2;
  if (!safeHeading.empty()) cardHeight += headingLineHeight;
  if (!safeHeading.empty() && !safeBodyLines.empty()) cardHeight += spacing;
  cardHeight += static_cast<int>(safeBodyLines.size()) * bodyLineHeight;

  cardX = (renderer.getScreenWidth() - cardWidth) / 2;
  cardY = (renderer.getScreenHeight() - cardHeight) / 2;

  requestUpdate(true);
}

void ConfirmationActivity::render(RenderLock&& lock) {
  renderer.clearScreen();
  renderer.fillRoundedRect(cardX, cardY, cardWidth, cardHeight, 14, Color::White);
  renderer.drawRoundedRect(cardX, cardY, cardWidth, cardHeight, 1, 14, true);

  int currentY = cardY + margin;
  LOG_DBG("CONF", "currentY: %d", currentY);
  if (!safeHeading.empty()) {
    const int headingWidth =
        renderer.getTextWidth(headingFontId, safeHeading.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(headingFontId, cardX + (cardWidth - headingWidth) / 2, currentY, safeHeading.c_str(), true,
                      EpdFontFamily::BOLD);
    currentY += headingLineHeight + spacing;
  }

  for (const auto& line : safeBodyLines) {
    const int lineWidth = renderer.getTextWidth(bodyFontId, line.c_str());
    renderer.drawText(bodyFontId, cardX + (cardWidth - lineWidth) / 2, currentY, line.c_str());
    currentY += bodyLineHeight;
  }

  // Draw UI Elements
  const auto labels = mappedInput.mapLabels("", "", I18N.get(StrId::STR_CANCEL), I18N.get(StrId::STR_CONFIRM));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void ConfirmationActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    ActivityResult res;
    res.isCancelled = false;
    setResult(std::move(res));
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    ActivityResult res;
    res.isCancelled = true;
    setResult(std::move(res));
    finish();
    return;
  }
}
