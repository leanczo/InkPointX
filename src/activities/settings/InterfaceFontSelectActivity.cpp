#include "InterfaceFontSelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <array>
#include <string>

#include "CrossPointSettings.h"
#include "InterfaceFont.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

void InterfaceFontSelectActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = SETTINGS.uiFontFamily < CrossPointSettings::UI_FONT_FAMILY_COUNT ? SETTINGS.uiFontFamily : 0;
  requestUpdate();
}

void InterfaceFontSelectActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    SETTINGS.uiFontFamily = static_cast<uint8_t>(selectedIndex);
    applyInterfaceFont();
    SETTINGS.saveToFile();
    finish();
    return;
  }

  buttonNavigator.onNextPress([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, CrossPointSettings::UI_FONT_FAMILY_COUNT);
    requestUpdate();
  });
  buttonNavigator.onPreviousPress([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, CrossPointSettings::UI_FONT_FAMILY_COUNT);
    requestUpdate();
  });
}

void InterfaceFontSelectActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  constexpr std::array<StrId, CrossPointSettings::UI_FONT_FAMILY_COUNT> fontNames = {
      StrId::STR_UI_FONT_NAME};

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_INTERFACE_FONT));
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = std::max(0, UITheme::getListContentBottom(renderer, true) - contentTop);
  GUI.drawList(
      renderer, Rect{0, contentTop, width, contentHeight}, static_cast<int>(fontNames.size()), selectedIndex,
      [&fontNames](const int index) { return std::string(I18N.get(fontNames[index])); }, nullptr, nullptr, nullptr,
      false, nullptr, [](const int index) {
        return index == SETTINGS.uiFontFamily ? UIAccessory::Check : UIAccessory::None;
      });
  GUI.drawFooterCounter(renderer, selectedIndex, static_cast<int>(fontNames.size()));

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
