#include "UnitConverterActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

struct UnitDef {
  const char* label;  // Universal symbol (km, °F, ...) -- not translated.
  double factor;
  double offset;
};

struct UnitCategoryDef {
  StrId titleKey;
  const UnitDef* units;
  int count;
};

double toBase(double raw, const UnitDef& u) { return raw * u.factor + u.offset; }
double fromBase(double base, const UnitDef& u) { return (base - u.offset) / u.factor; }
double convert(double raw, const UnitDef& from, const UnitDef& to) { return fromBase(toBase(raw, from), to); }

constexpr UnitDef kLengthUnits[] = {
    {"m", 1.0, 0.0}, {"km", 1000.0, 0.0}, {"cm", 0.01, 0.0}, {"mm", 0.001, 0.0},
    {"mi", 1609.344, 0.0}, {"yd", 0.9144, 0.0}, {"ft", 0.3048, 0.0}, {"in", 0.0254, 0.0},
};
constexpr UnitDef kWeightUnits[] = {
    {"kg", 1.0, 0.0}, {"g", 0.001, 0.0}, {"mg", 0.000001, 0.0}, {"lb", 0.45359237, 0.0}, {"oz", 0.0283495231, 0.0},
};
constexpr UnitDef kVolumeUnits[] = {
    {"l", 1.0, 0.0}, {"ml", 0.001, 0.0}, {"gal", 3.78541, 0.0}, {"qt", 0.946353, 0.0}, {"cup", 0.24, 0.0},
};
// Base unit is Celsius. toBase(raw) = raw*factor + offset must yield Celsius;
// fromBase inverts that. Derived from the standard C<->F<->K formulas.
constexpr UnitDef kTemperatureUnits[] = {
    {"\xC2\xB0" "C", 1.0, 0.0},
    {"\xC2\xB0" "F", 5.0 / 9.0, -32.0 * 5.0 / 9.0},
    {"K", 1.0, -273.15},
};

const UnitCategoryDef kCategories[] = {
    {StrId::STR_CONVERTER_LENGTH, kLengthUnits, static_cast<int>(sizeof(kLengthUnits) / sizeof(kLengthUnits[0]))},
    {StrId::STR_CONVERTER_WEIGHT, kWeightUnits, static_cast<int>(sizeof(kWeightUnits) / sizeof(kWeightUnits[0]))},
    {StrId::STR_CONVERTER_TEMPERATURE, kTemperatureUnits,
     static_cast<int>(sizeof(kTemperatureUnits) / sizeof(kTemperatureUnits[0]))},
    {StrId::STR_CONVERTER_VOLUME, kVolumeUnits, static_cast<int>(sizeof(kVolumeUnits) / sizeof(kVolumeUnits[0]))},
};
constexpr int kCategoryCount = sizeof(kCategories) / sizeof(kCategories[0]);

// Same shape as CalculatorActivity's CALC_GRID: a 4x4 grid of labels with
// blank cells skipped by the do-while navigation below. Only digits/./- plus
// Del and OK are needed since this only ever builds a plain decimal number,
// not a full expression.
const char* const KEYPAD_GRID[4][4] = {
    {"7", "8", "9", "Del"},
    {"4", "5", "6", "-"},
    {"1", "2", "3", "."},
    {"", "0", "", "OK"},
};

std::string formatNumber(double value) {
  char buf[64];
  if (value == static_cast<long long>(value)) {
    snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
  } else {
    snprintf(buf, sizeof(buf), "%.4g", value);
  }
  return std::string(buf);
}

}  // namespace

void UnitConverterActivity::onEnter() {
  Activity::onEnter();
  state = ConverterState::CategoryPicker;
  selectedCategory = 0;
  selectedFromUnit = 0;
  selectedToUnit = 0;
  pickerIndex = 0;
  selRow = 0;
  selCol = 0;
  inputValue.clear();
  requestUpdate();
}

void UnitConverterActivity::computeResult() {
  const auto& category = kCategories[selectedCategory];
  const double raw = inputValue.empty() ? 0.0 : atof(inputValue.c_str());
  resultValue = convert(raw, category.units[selectedFromUnit], category.units[selectedToUnit]);
}

void UnitConverterActivity::handleKeypadConfirm() {
  const char* label = KEYPAD_GRID[selRow][selCol];

  if (strcmp(label, "Del") == 0) {
    if (!inputValue.empty()) inputValue.pop_back();
    return;
  }
  if (strcmp(label, "OK") == 0) {
    if (inputValue.empty() || inputValue == "-" || inputValue == ".") return;
    computeResult();
    state = ConverterState::Result;
    return;
  }
  if (strcmp(label, "-") == 0) {
    if (inputValue.empty()) inputValue += label;  // leading sign only
    return;
  }
  if (strcmp(label, ".") == 0) {
    if (inputValue.find('.') != std::string::npos) return;  // one decimal point only
    if (inputValue.empty() || inputValue == "-") inputValue += "0";
    inputValue += label;
    return;
  }
  // Plain digit.
  if (inputValue.length() < 16) inputValue += label;
}

void UnitConverterActivity::loop() {
  using Button = MappedInputManager::Button;

  if (mappedInput.wasReleased(Button::Back)) {
    switch (state) {
      case ConverterState::CategoryPicker:
        onGoHome(HomeMenuItem::TOOLS_MENU);
        return;
      case ConverterState::FromUnitPicker:
        state = ConverterState::CategoryPicker;
        pickerIndex = selectedCategory;
        break;
      case ConverterState::ToUnitPicker:
        state = ConverterState::FromUnitPicker;
        pickerIndex = selectedFromUnit;
        break;
      case ConverterState::ValueEntry:
        state = ConverterState::ToUnitPicker;
        pickerIndex = selectedToUnit;
        break;
      case ConverterState::Result:
        state = ConverterState::ValueEntry;
        inputValue.clear();
        break;
    }
    requestUpdate();
    return;
  }

  if (state == ConverterState::CategoryPicker || state == ConverterState::FromUnitPicker ||
      state == ConverterState::ToUnitPicker) {
    const int count = (state == ConverterState::CategoryPicker) ? kCategoryCount
                                                                 : kCategories[selectedCategory].count;
    if (mappedInput.wasReleased(Button::Up)) {
      pickerIndex = (pickerIndex - 1 + count) % count;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Down)) {
      pickerIndex = (pickerIndex + 1) % count;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Confirm)) {
      if (state == ConverterState::CategoryPicker) {
        selectedCategory = pickerIndex;
        selectedFromUnit = 0;
        state = ConverterState::FromUnitPicker;
      } else if (state == ConverterState::FromUnitPicker) {
        selectedFromUnit = pickerIndex;
        selectedToUnit = 0;
        state = ConverterState::ToUnitPicker;
      } else {
        selectedToUnit = pickerIndex;
        inputValue.clear();
        selRow = 0;
        selCol = 0;
        state = ConverterState::ValueEntry;
      }
      pickerIndex = 0;
      requestUpdate();
    }
    return;
  }

  if (state == ConverterState::ValueEntry) {
    if (mappedInput.wasReleased(Button::Left)) {
      do {
        selCol = (selCol - 1 + 4) % 4;
      } while (KEYPAD_GRID[selRow][selCol][0] == '\0');
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Right)) {
      do {
        selCol = (selCol + 1) % 4;
      } while (KEYPAD_GRID[selRow][selCol][0] == '\0');
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Up)) {
      do {
        selRow = (selRow - 1 + 4) % 4;
      } while (KEYPAD_GRID[selRow][selCol][0] == '\0');
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Down)) {
      do {
        selRow = (selRow + 1) % 4;
      } while (KEYPAD_GRID[selRow][selCol][0] == '\0');
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Confirm)) {
      handleKeypadConfirm();
      requestUpdate();
    }
    return;
  }

  if (state == ConverterState::Result) {
    if (mappedInput.wasReleased(Button::Confirm) || mappedInput.wasReleased(Button::Right)) {
      // Convert another value between the same two units.
      state = ConverterState::ValueEntry;
      inputValue.clear();
      requestUpdate();
    }
  }
}

void UnitConverterActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CONVERTER_TITLE));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const Rect listRect{0, contentTop, pageWidth, contentBottom - contentTop};

  if (state == ConverterState::CategoryPicker) {
    GUI.drawList(
        renderer, listRect, kCategoryCount, pickerIndex,
        [](int i) { return std::string(I18N.get(kCategories[i].titleKey)); }, nullptr, nullptr, nullptr, false);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == ConverterState::FromUnitPicker || state == ConverterState::ToUnitPicker) {
    const auto& category = kCategories[selectedCategory];
    const bool isFrom = state == ConverterState::FromUnitPicker;
    const char* fromToLabel = isFrom ? tr(STR_CONVERTER_FROM) : tr(STR_CONVERTER_TO);
    std::string title = std::string(I18N.get(category.titleKey)) + " - " + fromToLabel;
    GUI.drawSubHeader(renderer, Rect{0, contentTop, pageWidth, metrics.subHeaderHeight}, title.c_str());
    const Rect pickerRect{0, contentTop + metrics.subHeaderHeight + metrics.verticalSpacing, pageWidth,
                          contentBottom - contentTop - metrics.subHeaderHeight - metrics.verticalSpacing};
    GUI.drawList(
        renderer, pickerRect, category.count, pickerIndex, [&category](int i) { return std::string(category.units[i].label); },
        nullptr, nullptr, nullptr, false);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == ConverterState::ValueEntry) {
    const auto& category = kCategories[selectedCategory];
    char subtitle[64];
    snprintf(subtitle, sizeof(subtitle), "%s -> %s", category.units[selectedFromUnit].label,
             category.units[selectedToUnit].label);
    GUI.drawSubHeader(renderer, Rect{0, contentTop, pageWidth, metrics.subHeaderHeight}, subtitle);

    const int displayTop = contentTop + metrics.subHeaderHeight + metrics.verticalSpacing;
    const int displayHeight = 60;
    const int displayX = metrics.contentSidePadding;
    const int displayWidth = pageWidth - 2 * metrics.contentSidePadding;
    renderer.drawRoundedRect(displayX, displayTop, displayWidth, displayHeight, 1, 8, true);
    const std::string displayText = inputValue.empty() ? "0" : inputValue;
    const int textW = renderer.getTextWidth(UI_12_FONT_ID, displayText.c_str());
    const int textH = renderer.getLineHeight(UI_12_FONT_ID);
    renderer.drawText(UI_12_FONT_ID, displayX + displayWidth - 12 - textW, displayTop + (displayHeight - textH) / 2,
                      displayText.c_str(), true, EpdFontFamily::BOLD);

    const int gridTop = displayTop + displayHeight + metrics.verticalSpacing;
    const int gridHeight = contentBottom - gridTop;
    const int rowStep = gridHeight / 4;
    const int colStep = displayWidth / 4;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        const char* label = KEYPAD_GRID[r][c];
        if (label[0] == '\0') continue;
        const int keyX = displayX + c * colStep + 4;
        const int keyY = gridTop + r * rowStep + 4;
        const int keyW = colStep - 8;
        const int keyH = rowStep - 8;
        const bool isSelected = (selRow == r && selCol == c);
        if (isSelected) {
          renderer.fillRoundedRect(keyX, keyY, keyW, keyH, 6, Color::Black);
        } else {
          renderer.drawRoundedRect(keyX, keyY, keyW, keyH, 1, 6, true);
        }
        const int lblW = renderer.getTextWidth(UI_12_FONT_ID, label);
        const int lblH = renderer.getLineHeight(UI_12_FONT_ID);
        renderer.drawText(UI_12_FONT_ID, keyX + (keyW - lblW) / 2, keyY + (keyH - lblH) / 2, label, !isSelected,
                          EpdFontFamily::BOLD);
      }
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {  // Result
    const auto& category = kCategories[selectedCategory];
    char resultLine[96];
    snprintf(resultLine, sizeof(resultLine), "%s %s = %s %s", inputValue.c_str(),
             category.units[selectedFromUnit].label, formatNumber(resultValue).c_str(),
             category.units[selectedToUnit].label);

    const int textY = contentTop + (contentBottom - contentTop) / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, textY, resultLine, true, EpdFontFamily::BOLD);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONVERTER_NEW_VALUE), nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
