#pragma once

#include <EpdFontFamily.h>

#include <functional>

#include "CrossPointSettings.h"
#include "components/themes/BaseTheme.h"

class UITheme {
  // Static instance
  static UITheme instance;

 public:
  UITheme();
  static UITheme& getInstance() { return instance; }

  const ThemeMetrics& getMetrics() const { return currentMetrics; }
  const BaseTheme& getTheme() const { return *currentTheme; }
  Rect getScreenSafeArea(const GfxRenderer& renderer, bool hasFrontButtonHints = false,
                         bool hasSideButtonHints = false);
  static void drawCenteredText(const GfxRenderer& renderer, Rect screen, int fontId, int y, const char* text,
                               bool black = true, EpdFontFamily::Style style = EpdFontFamily::REGULAR);
  void reload();
  void setTheme(CrossPointSettings::UI_THEME type);
  static int getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasSubHeader,
                                     bool hasButtonHints, bool hasSubtitle, int extraReservedHeight = 0);
  // Bottom Y available to list content above the button legend. Screens that
  // show the "n / m" footer counter must pass true so the last row is not drawn
  // underneath it; each list screen previously reserved its own value (8, 42 or
  // 54 px), and the shortest of those overlapped.
  static int getListContentBottom(const GfxRenderer& renderer, bool hasFooterCounter);
  static std::string getCoverThumbPath(std::string coverBmpPath, int coverHeight);
  static UIIcon getFileIcon(const std::string& filename);
  static int getStatusBarHeight();
  static int getProgressBarHeight();
  // System-wide top-right battery indicator. Reader pages keep their own
  // status-bar layout, while every other screen uses this shared overlay.
  int getSystemBatteryOverlayWidth(const GfxRenderer& renderer) const;
  void clearSystemBatteryOverlay(const GfxRenderer& renderer) const;
  void drawSystemBatteryOverlay(const GfxRenderer& renderer) const;
 private:
  ThemeMetrics currentMetrics = BaseMetrics::values;
  BaseTheme* currentTheme = nullptr;
};

// Helper macro to access current theme
#define GUI UITheme::getInstance().getTheme()
