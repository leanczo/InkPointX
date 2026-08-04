#include "InterfaceFontSelectActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>

#include "CrossPointSettings.h"
#include "InterfaceFont.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"

void InterfaceFontSelectActivity::rebuildOptions() {
  // A font uploaded over the web UI a moment ago belongs in this list.
  sdFontSystem.refreshIfDirty();

  options.clear();
  options.emplace_back();  // the built-in face, stored as an empty name
  const auto& families = sdFontSystem.registry().getFamilies();
  std::transform(families.cbegin(), families.cend(), std::back_inserter(options),
                 [](const SdCardFontFamilyInfo& family) { return family.name; });

  const char* current = targetSetting();
  selectedIndex = 0;
  if (current && current[0] != '\0') {
    for (size_t i = 1; i < options.size(); ++i) {
      if (options[i] == current) {
        selectedIndex = static_cast<int>(i);
        break;
      }
    }
  }
}

char* InterfaceFontSelectActivity::targetSetting() const {
  return target == Target::Accent ? SETTINGS.scriptSdFontFamilyName : SETTINGS.uiSdFontFamilyName;
}

StrId InterfaceFontSelectActivity::builtInNameId() const {
  return target == Target::Accent ? StrId::STR_UI_ACCENT_FONT_NAME : StrId::STR_UI_FONT_NAME;
}

void InterfaceFontSelectActivity::onEnter() {
  Activity::onEnter();
  rebuildOptions();
  requestUpdate();
}

void InterfaceFontSelectActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(options.size())) {
      char* setting = targetSetting();
      strncpy(setting, options[selectedIndex].c_str(), CrossPointSettings::SD_FONT_NAME_MAX - 1);
      setting[CrossPointSettings::SD_FONT_NAME_MAX - 1] = '\0';
      // Rebinds every slot: the card's face where it has a size to offer, the
      // built-in face everywhere else. Takes the render lock itself.
      applyInterfaceFont();
      SETTINGS.saveToFile();
    }
    finish();
    return;
  }

  buttonNavigator.onNextPress([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(options.size()));
    requestUpdate();
  });
  buttonNavigator.onPreviousPress([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(options.size()));
    requestUpdate();
  });
}

void InterfaceFontSelectActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight},
                 target == Target::Accent ? tr(STR_ACCENT_FONT) : tr(STR_INTERFACE_FONT));
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = std::max(0, UITheme::getListContentBottom(renderer, true) - contentTop);

  const char* current = targetSetting();
  const bool usingBuiltIn = !current || current[0] == '\0';
  GUI.drawList(
      renderer, Rect{0, contentTop, width, contentHeight}, static_cast<int>(options.size()), selectedIndex,
      [this](const int index) { return index == 0 ? std::string(I18N.get(builtInNameId())) : options[index]; }, nullptr,
      nullptr, nullptr, false, nullptr,
      [this, current, usingBuiltIn](const int index) {
        const bool isCurrent = index == 0 ? usingBuiltIn : (!usingBuiltIn && options[index] == current);
        return isCurrent ? UIAccessory::Check : UIAccessory::None;
      });
  GUI.drawFooterCounter(renderer, selectedIndex, static_cast<int>(options.size()));

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
