#include "IntervalSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "components/UITheme.h"
#include "fontIds.h"

int IntervalSelectionActivity::clampedValue(const int candidate) const {
  return std::clamp(candidate, minValue, maxValue);
}

void IntervalSelectionActivity::onEnter() {
  Activity::onEnter();
  value = clampedValue(value);
  requestUpdate();
}

void IntervalSelectionActivity::adjustValue(const int delta) {
  value = clampedValue(value + delta);
  requestUpdate();
}

void IntervalSelectionActivity::loop() {
  if (ignoreConfirmRelease) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      ignoreConfirmRelease = false;
      return;
    }
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      ignoreConfirmRelease = false;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    setResult(IntervalResult{static_cast<uint32_t>(value)});
    finish();
    return;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this] { adjustValue(-smallStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this] { adjustValue(smallStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [this] { adjustValue(largeStep); });
  buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [this] { adjustValue(-largeStep); });
}

void IntervalSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int screenWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, screenWidth, metrics.headerHeight}, I18N.get(titleId));

  char formattedValue[32];
  if (maxBoundaryLabelId != StrId::STR_NONE_OPT && value == maxValue) {
    snprintf(formattedValue, sizeof(formattedValue), "%s", I18N.get(maxBoundaryLabelId));
  } else if (valueFormatId != StrId::STR_NONE_OPT) {
    snprintf(formattedValue, sizeof(formattedValue), I18N.get(valueFormatId), static_cast<unsigned int>(value));
  } else {
    snprintf(formattedValue, sizeof(formattedValue), "%d", value);
  }
  const int valueY = metrics.topPadding + metrics.headerHeight + 54;
  renderer.drawCenteredText(UI_18_FONT_ID, valueY, formattedValue, true, EpdFontFamily::BOLD);

  const int barWidth = std::min(360, std::max(0, screenWidth - 40));
  constexpr int barHeight = 8;
  const int barX = std::max(0, (screenWidth - barWidth) / 2);
  const int barY = valueY + 74;

  renderer.fillRoundedRect(barX, barY, barWidth, barHeight, barHeight / 2, Color::LightGray);

  const int range = std::max(1, maxValue - minValue);
  const int fillWidth = barWidth * (value - minValue) / range;
  if (fillWidth > 0) {
    renderer.fillRoundedRect(barX, barY, fillWidth, barHeight, barHeight / 2, Color::Black);
  }

  constexpr int knobSize = 16;
  const int knobX = std::clamp(barX + fillWidth - knobSize / 2, barX, barX + barWidth - knobSize);
  renderer.fillRoundedRect(knobX, barY - (knobSize - barHeight) / 2, knobSize, knobSize, knobSize / 2, Color::Black);

  renderer.drawCenteredText(SMALL_FONT_ID, barY + 34, I18N.get(stepHintId), true);

  char decrement[12];
  char increment[12];
  snprintf(decrement, sizeof(decrement), "-%d", smallStep);
  snprintf(increment, sizeof(increment), "+%d", smallStep);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), decrement, increment);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  char largeIncrement[12];
  char largeDecrement[12];
  snprintf(largeIncrement, sizeof(largeIncrement), "+%d", largeStep);
  snprintf(largeDecrement, sizeof(largeDecrement), "-%d", largeStep);
  GUI.drawSideButtonHints(renderer, largeIncrement, largeDecrement);

  renderer.displayBuffer();
}
