#pragma once

#include "components/themes/BaseTheme.h"

class GfxRenderer;

// Lyra theme metrics (zero runtime cost)
namespace LyraMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 16,
                                 .batteryHeight = 12,
                                 .topPadding = 8,
                                 .batteryBarHeight = 0,
                                 .headerHeight = 60,
                                 .verticalSpacing = 8,
                                 .previewPadding = 12,
                                 .previewHeightPercent = 30,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 54,
                                 .listWithSubtitleRowHeight = 76,
                                 .menuRowHeight = 58,
                                 .menuSpacing = 4,
                                 .subHeaderHeight = 42,
                                 .scrollBarWidth = 3,
                                 .scrollBarRightOffset = 8,
                                 .homeTopPadding = 58,
                                 .homeCoverHeight = 218,
                                 .homeCoverTileHeight = 234,
                                 .homeRecentBooksCount = 1,
                                 .homeContinueReadingInMenu = false,
                                 .homeMenuTopOffset = 12,
                                 .buttonHintsHeight = 32,
                                 .sideButtonHintsWidth = 28,
                                 .progressBarHeight = 10,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyWidth = 31,
                                 .keyboardKeyHeight = 40,
                                 .keyboardKeySpacing = 0,
                                 .keyboardBottomKeyHeight = 35,
                                 .keyboardBottomKeySpacing = 5,
                                 .keyboardBottomAligned = true,
                                 .keyboardCenteredText = false,
                                 .keyboardVerticalOffset = -7,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 90,
                                 .keyboardKeyCornerRadius = 8,
                                 .keyboardFillUnselected = false,
                                 .keyboardOutlineAllUnselected = true,
                                 .keyboardDrawSpecialOutlineWhenUnselected = true,
                                 .keyboardSecondaryLabelRightPadding = 1,
                                 .keyboardSecondaryLabelTopPadding = 0,
                                 .keyboardMinArrowHeadSize = 0,
                                 .popupTopOffsetRatio = 0.18f,
                                 .popupMarginX = 20,
                                 .popupMarginY = 16,
                                 .popupFrameThickness = 1,
                                 .popupCornerRadius = 12,
                                 .popupTextBold = false,
                                 .popupTextInverted = false,
                                 .popupTextBaselineOffsetY = -2,
                                 .popupProgressBarHeight = 4,
                                 .popupProgressDrawOutline = true,
                                 .popupProgressClampPercent = true,
                                 .popupProgressFillInverted = true,
                                 .popupProgressOutlineInverted = true,
                                 .textFieldHorizontalPadding = 8,
                                 .textFieldNormalThickness = 1,
                                 .textFieldCursorThickness = 3,
                                 .textFieldLineEndOffset = 0};
}

class LyraTheme : public BaseTheme {
 public:
  // Component drawing methods
  void fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const override;
  void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const override;
  void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                     const char* rightLabel = nullptr) const override;
  int getListPageItems(int contentHeight, bool hasSubtitle) const override;
  void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                const std::function<std::string(int index)>& rowTitle,
                const std::function<std::string(int index)>& rowSubtitle,
                const std::function<UIIcon(int index)>& rowIcon, const std::function<std::string(int index)>& rowValue,
                bool highlightValue, const std::function<bool(int index)>& rowDimmed = nullptr,
                const std::function<UIAccessory(int index)>& rowAccessory = nullptr) const override;
  void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) const override;
  void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const override;
  void drawDivider(const GfxRenderer& renderer, int x1, int x2, int y) const override;
  void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                      const std::function<std::string(int index)>& buttonLabel,
                      const std::function<UIIcon(int index)>& rowIcon) const override;
  bool showsFileIcons() const override { return true; }
};
