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
  // Share the popup radius so a confirmation and the "Loading…" popup that can
  // precede it are the same shape.
  const int radius = UITheme::getInstance().getMetrics().popupCornerRadius;
  renderer.fillRoundedRect(cardX, cardY, cardWidth, cardHeight, radius, Color::White);
  renderer.drawRoundedRect(cardX, cardY, cardWidth, cardHeight, 1, radius, true);

  int currentY = cardY + margin;
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

  // Draw UI Elements. Back and Confirm carry their global meaning here, so the
  // legend names all four slots instead of leaving the first two blank.
  const auto labels = mappedInput.mapLabels(I18N.get(StrId::STR_CANCEL), I18N.get(StrId::STR_CONFIRM),
                                            I18N.get(StrId::STR_CANCEL), I18N.get(StrId::STR_CONFIRM));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void ConfirmationActivity::loop() {
  // Commit on release, like every other screen: acting on the press edge leaks
  // the release into whatever activity is shown next.
  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    ActivityResult res;
    res.isCancelled = false;
    setResult(std::move(res));
    finish();
    return;
  }

  // Back is the universal cancel in this firmware; without it the only way out
  // of a destructive-action modal was discovering that Left means cancel.
  if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult res;
    res.isCancelled = true;
    setResult(std::move(res));
    finish();
    return;
  }
}
