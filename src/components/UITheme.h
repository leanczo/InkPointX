#pragma once

#include <EpdFontFamily.h>

#include <atomic>
#include <functional>
#include <memory>

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
  static int getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle, int extraReservedHeight = 0);
  static std::string getCoverThumbPath(std::string coverBmpPath, int coverHeight);
  static UIIcon getFileIcon(const std::string& filename);
  static int getStatusBarHeight();
  static int getProgressBarHeight();
  // System-wide top-right battery indicator. Reader pages keep their own
  // status-bar layout, while every other screen uses this shared overlay.
  int getSystemBatteryOverlayWidth(const GfxRenderer& renderer) const;
  void clearSystemBatteryOverlay(const GfxRenderer& renderer) const;
  void drawSystemBatteryOverlay(const GfxRenderer& renderer) const;
  // Sticky for the lifetime of the current activity. The main input loop uses
  // this to schedule one visual-only redraw after a front button is released,
  // without imposing a second refresh on reader pages that do not show hints.
  void markButtonHintsVisible() { buttonHintsVisible.store(true, std::memory_order_release); }
  void resetButtonHintsVisible() { buttonHintsVisible.store(false, std::memory_order_release); }
  bool hasVisibleButtonHints() const { return buttonHintsVisible.load(std::memory_order_acquire); }

 private:
  ThemeMetrics currentMetrics = BaseMetrics::values;
  std::unique_ptr<BaseTheme> currentTheme;
  std::atomic_bool buttonHintsVisible{false};
};

// Helper macro to access current theme
#define GUI UITheme::getInstance().getTheme()
