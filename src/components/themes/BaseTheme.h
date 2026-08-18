#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "fontIds.h"

class GfxRenderer;
struct RecentBook;

struct Rect {
  int x;
  int y;
  int width;
  int height;

  explicit Rect(int x = 0, int y = 0, int width = 0, int height = 0) : x(x), y(y), width(width), height(height) {}
};

struct ThemeMetrics {
  int batteryWidth;
  int batteryHeight;

  int topPadding;
  int batteryBarHeight;
  int headerHeight;
  int verticalSpacing;

  int previewPadding;
  int previewHeightPercent;

  int contentSidePadding;
  int listRowHeight;
  int listWithSubtitleRowHeight;
  int menuRowHeight;
  int menuSpacing;

  int subHeaderHeight;

  int scrollBarWidth;
  int scrollBarRightOffset;

  int homeTopPadding;
  int homeCoverHeight;
  int homeCoverTileHeight;
  int homeRecentBooksCount;
  bool homeContinueReadingInMenu;
  int homeMenuTopOffset;

  int buttonHintsHeight;
  int sideButtonHintsWidth;

  int progressBarHeight;
  int progressBarMarginTop;
  int statusBarHorizontalMargin;
  int statusBarVerticalMargin;

  int keyboardKeyWidth;
  int keyboardKeyHeight;
  int keyboardKeySpacing;
  int keyboardBottomKeyHeight;
  int keyboardBottomKeySpacing;
  bool keyboardBottomAligned;
  bool keyboardCenteredText;
  int keyboardVerticalOffset;
  int keyboardTextFieldWidthPercent;
  int keyboardWidthPercent;
  int keyboardKeyCornerRadius;
  bool keyboardFillUnselected;
  bool keyboardOutlineAllUnselected;
  bool keyboardDrawSpecialOutlineWhenUnselected;
  int keyboardSecondaryLabelRightPadding;
  int keyboardSecondaryLabelTopPadding;
  int keyboardMinArrowHeadSize;

  float popupTopOffsetRatio;
  int popupMarginX;
  int popupMarginY;
  int popupFrameThickness;
  int popupCornerRadius;
  bool popupTextBold;
  bool popupTextInverted;
  int popupTextBaselineOffsetY;
  int popupProgressBarHeight;
  bool popupProgressDrawOutline;
  bool popupProgressClampPercent;
  bool popupProgressFillInverted;
  bool popupProgressOutlineInverted;

  int textFieldHorizontalPadding;
  int textFieldNormalThickness;
  int textFieldCursorThickness;
  int textFieldLineEndOffset;
};

enum UIIcon {
  None = 0,
  Folder,
  Text,
  Image,
  Book,
  BookNew,
  File,
  Recent,
  Settings,
  Transfer,
  Library,
  Wifi,
  Hotspot,
  Bookmark,
  Favorite,
  Interface,
  Power,
  Reading,
  Controls,
  Files,
  NetworkSync,
  System,
  Clock,
  ReaderPage,
  ReaderChapters,
  ReaderDictionary,
  ReaderFootnotes,
  ReaderStats,
  ReaderRotate,
  ReaderAutoTurn,
  ReaderQr,
  ReaderHome,
  ReaderTrash,
  Snake,
  Calculator,
  Dice,
  Football,
  F1,
  TicTacToe,
  Game2048,
  Minesweeper,
  Simon,
  Sudoku,
  Calendar,
  Reminders,
  Weather,
  WeatherCity,
  Rss,
  OnThisDay,
  Sismos,
};

enum class UIAccessory { None, Chevron, Check, ToggleOff, ToggleOn, Favorite };

enum class KeyboardKeyType { Normal, Shift, Mode, Space, Del, Ok, Disabled };

// Default theme implementation (Classic Theme)
// Additional themes can inherit from this and override methods as needed

namespace BaseMetrics {
constexpr ThemeMetrics values = {.batteryWidth = 17,
                                 .batteryHeight = 12,
                                 .topPadding = 5,
                                 .batteryBarHeight = 20,
                                 .headerHeight = 45,
                                 .verticalSpacing = 10,
                                 .previewPadding = 12,
                                 .previewHeightPercent = 30,
                                 .contentSidePadding = 20,
                                 .listRowHeight = 30,
                                 .listWithSubtitleRowHeight = 50,
                                 .menuRowHeight = 45,
                                 .menuSpacing = 8,
                                 .subHeaderHeight = 50,
                                 .scrollBarWidth = 4,
                                 .scrollBarRightOffset = 5,
                                 .homeTopPadding = 40,
                                 .homeCoverHeight = 400,
                                 .homeCoverTileHeight = 400,
                                 .homeRecentBooksCount = 1,
                                 .homeContinueReadingInMenu = false,
                                 .homeMenuTopOffset = 10,
                                 .buttonHintsHeight = 39,
                                 .sideButtonHintsWidth = 30,
                                 .progressBarHeight = 16,
                                 .progressBarMarginTop = 1,
                                 .statusBarHorizontalMargin = 5,
                                 .statusBarVerticalMargin = 19,
                                 .keyboardKeyWidth = 22,
                                 .keyboardKeyHeight = 40,
                                 .keyboardKeySpacing = 0,
                                 .keyboardBottomKeyHeight = 35,
                                 .keyboardBottomKeySpacing = 5,
                                 .keyboardBottomAligned = true,
                                 .keyboardCenteredText = false,
                                 .keyboardVerticalOffset = -13,
                                 .keyboardTextFieldWidthPercent = 85,
                                 .keyboardWidthPercent = 90,
                                 .keyboardKeyCornerRadius = 0,
                                 .keyboardFillUnselected = false,
                                 .keyboardOutlineAllUnselected = false,
                                 .keyboardDrawSpecialOutlineWhenUnselected = true,
                                 .keyboardSecondaryLabelRightPadding = 1,
                                 .keyboardSecondaryLabelTopPadding = 0,
                                 .keyboardMinArrowHeadSize = 0,
                                 .popupTopOffsetRatio = 0.075f,
                                 .popupMarginX = 15,
                                 .popupMarginY = 15,
                                 .popupFrameThickness = 2,
                                 .popupCornerRadius = 0,
                                 .popupTextBold = true,
                                 .popupTextInverted = true,
                                 .popupTextBaselineOffsetY = -2,
                                 .popupProgressBarHeight = 4,
                                 .popupProgressDrawOutline = false,
                                 .popupProgressClampPercent = false,
                                 .popupProgressFillInverted = true,
                                 .popupProgressOutlineInverted = true,
                                 .textFieldHorizontalPadding = 6,
                                 .textFieldNormalThickness = 1,
                                 .textFieldCursorThickness = 3,
                                 .textFieldLineEndOffset = 0};
}

class BaseTheme {
 public:
  virtual ~BaseTheme() = default;

  // Distance from the top of the button legend up to the "n / m" footer
  // counter's first row. Screens that draw the counter must keep list content
  // above this band — see UITheme::getListContentBottom.
  static constexpr int footerCounterTopOffset = 36;

  // Component drawing methods
  void drawProgressBar(const GfxRenderer& renderer, Rect rect, size_t current, size_t total) const;
  // Left aligned (reader mode). fontId sizes the percent digits and the
  // icon's vertical centring — the reader's status bar passes MICRO.
  void drawBatteryLeft(const GfxRenderer& renderer, Rect rect, bool showPercentage = true,
                       int fontId = SMALL_FONT_ID) const;
  void drawBatteryRight(const GfxRenderer& renderer, Rect rect,
                        bool showPercentage = true) const;  // Right aligned (UI headers)
  virtual void fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const;
  virtual void drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                               const char* btn4) const;
  virtual void drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const;
  virtual int getListPageItems(int contentHeight, bool hasSubtitle) const;
  virtual void drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                        const std::function<std::string(int index)>& rowTitle,
                        const std::function<std::string(int index)>& rowSubtitle = nullptr,
                        const std::function<UIIcon(int index)>& rowIcon = nullptr,
                        const std::function<std::string(int index)>& rowValue = nullptr, bool highlightValue = false,
                        const std::function<bool(int index)>& rowDimmed = nullptr,
                        const std::function<UIAccessory(int index)>& rowAccessory = nullptr,
                        const std::function<bool(int index)>& rowSection = nullptr) const;
  virtual void drawHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                          const char* subtitle = nullptr) const;
  // Same battery/rule chrome as drawHeader, but the title is set in the
  // handwritten accent face used by the Home hub pages (Herramientas, Apps,
  // Juegos) instead of the structural header font.
  void drawScriptHeader(const GfxRenderer& renderer, Rect rect, const char* title,
                        const char* subtitle = nullptr) const;
  virtual void drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label,
                             const char* rightLabel = nullptr) const;
  virtual void drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                              const std::function<std::string(int index)>& buttonLabel,
                              const std::function<UIIcon(int index)>& rowIcon) const;
  void drawSelection(const GfxRenderer& renderer, Rect rect) const;
  virtual void drawPageDots(const GfxRenderer& renderer, int selectedPage, int pageCount) const;
  virtual void drawFooterCounter(GfxRenderer& renderer, int selectedIndex, int itemCount,
                                 const char* status = nullptr) const;
  virtual Rect drawPopup(const GfxRenderer& renderer, const char* message) const;
  virtual void fillPopupProgress(const GfxRenderer& renderer, const Rect& layout, const int progress) const;
  void drawStatusBar(GfxRenderer& renderer, const float bookProgress, const int currentPage, const int pageCount,
                     std::string title, const int paddingBottom = 0, const int textYOffset = 0,
                     const bool fillMargin = true, const bool isPageBookmarked = false) const;
  void drawHelpText(const GfxRenderer& renderer, Rect rect, const char* label) const;
  virtual void drawTextField(const GfxRenderer& renderer, Rect rect, const int textWidth, bool cursorMode = false,
                             int contentStartX = 0, int contentWidth = 0) const;
  virtual void drawKeyboardKey(const GfxRenderer& renderer, Rect rect, const char* label, const bool isSelected,
                               const char* secondaryLabel = nullptr, KeyboardKeyType keyType = KeyboardKeyType::Normal,
                               bool inactiveSelection = false) const;
  virtual bool showsFileIcons() const { return false; }
  // The shared divider treatment, inset to the content margin. Screens that
  // need a rule outside a list (e.g. the file browser's path bar) must use this
  // instead of drawing their own line, so weight and inset stay consistent.
  virtual void drawDivider(const GfxRenderer& renderer, int x1, int x2, int y) const;
  // The shared "nothing here" treatment for an empty list: centred in the content
  // rect and direction-aware, rather than each screen hard-left-aligning its own
  // message at an unexplained offset.
  // "Nothing here" and "something went wrong" are the same shape: a centred line,
  // optionally with an explanation under it. One primitive rather than each screen
  // centring on pageHeight / 2 with its own hand-picked offsets.
  // script=true sets the message in the handwritten accent face for human
  // moments (empty collections and successful completion), never for errors.
  virtual void drawEmptyState(const GfxRenderer& renderer, Rect content, const char* message,
                              const char* detail = nullptr, bool script = false) const;
  // A single line the reader has to notice: end of book, empty chapter, page load
  // failure. Centred in the current viewport rather than at a fixed Y, which was
  // above centre in portrait and near the bottom edge in landscape.
  void drawReaderMessage(const GfxRenderer& renderer, const char* message, bool script = false) const;

  // Shared constants and helpers for battery drawing (used by all themes)
  static constexpr int batteryPercentSpacing = 4;
  // The charge reading is a caption, not a label: one step below the 10 px the
  // header used, so the digits sit beside the icon instead of towering over it.
  // Shared, because the overlay's width and its clear rect must agree with what
  // drawBatteryRight actually draws or the group smears on partial redraws.
  static constexpr int batteryPercentFontId = MICRO_FONT_ID;
  static constexpr int batteryTerminalWidth = 2;
  static Rect batteryBodyRect(Rect box, int centerY);
  static void drawBatteryOutline(const GfxRenderer& renderer, Rect body);
  static int batteryDigitsCenterY(const GfxRenderer& renderer, Rect rect, int fontId, const char* percentageText);
  static void drawBatteryLightningBolt(const GfxRenderer& renderer, int boltX, int boltY);
};
