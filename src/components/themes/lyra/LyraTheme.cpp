#include "LyraTheme.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <BidiUtils.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/lucide_ui.h"
#include "fontIds.h"

// Internal constants
namespace {
constexpr int hPaddingInSelection = 10;
constexpr int cornerRadius = 12;
constexpr int topHintButtonY = 345;
constexpr int maxListValueWidth = 200;
constexpr int mainMenuIconSize = 24;
constexpr int listIconSize = 24;
constexpr int toggleWidth = 30;
constexpr int toggleHeight = 16;

const uint8_t* iconForName(UIIcon icon, int size) {
  if (size == 24) {
    switch (icon) {
      case UIIcon::Folder:
        return LucideFolder24;
      case UIIcon::Text:
      case UIIcon::File:
        return LucideFileText24;
      case UIIcon::Image:
        return LucideImage24;
      case UIIcon::Book:
        return LucideBookOpen24;
      case UIIcon::Recent:
      case UIIcon::Clock:
        return LucideClock24;
      case UIIcon::Settings:
        return LucideSettings24;
      case UIIcon::Transfer:
        return LucideSend24;
      case UIIcon::Library:
        return LucideLibrary24;
      case UIIcon::Wifi:
        return LucideWifi24;
      case UIIcon::Hotspot:
        return LucideHotspot24;
      case UIIcon::Bookmark:
        return LucideBookmark24;
      case UIIcon::Interface:
        return LucideInterface24;
      case UIIcon::Power:
        return LucidePower24;
      case UIIcon::Reading:
        return LucideReading24;
      case UIIcon::Controls:
        return LucideControls24;
      case UIIcon::Files:
        return LucideFiles24;
      case UIIcon::NetworkSync:
        return LucideNetwork24;
      case UIIcon::System:
        return LucideSystem24;
      default:
        return nullptr;
    }
  }
  return nullptr;
}

void drawHairline(const GfxRenderer& renderer, int x1, int x2, int y) {
  for (int x = x1; x <= x2; x += 2) renderer.drawPixel(x, y, true);
}

int accessoryWidth(const UIAccessory accessory) {
  return accessory == UIAccessory::ToggleOff || accessory == UIAccessory::ToggleOn ? toggleWidth : 16;
}

void drawAccessory(const GfxRenderer& renderer, const UIAccessory accessory, const int x, const int y, bool rtl) {
  switch (accessory) {
    case UIAccessory::Chevron:
      renderer.drawIcon(rtl ? LucideChevronLeft16 : LucideChevronRight16, x, y, 16, 16);
      break;
    case UIAccessory::Check:
      renderer.drawIcon(LucideCheck16, x, y, 16, 16);
      break;
    case UIAccessory::ToggleOff:
    case UIAccessory::ToggleOn: {
      renderer.drawRoundedRect(x, y, toggleWidth, toggleHeight, 1, toggleHeight / 2, true);
      const int knobSize = toggleHeight - 6;
      const int knobX = accessory == UIAccessory::ToggleOn ? x + toggleWidth - knobSize - 3 : x + 3;
      renderer.fillRoundedRect(knobX, y + 3, knobSize, knobSize, knobSize / 2, Color::Black);
      break;
    }
    case UIAccessory::None:
      break;
  }
}

}  // namespace

void LyraTheme::fillBatteryIcon(const GfxRenderer& renderer, Rect rect, uint16_t percentage) const {
  const bool charging = gpio.isUsbConnected();
  const int fillWidth =
      charging ? rect.width - 6
               : std::clamp(static_cast<int>(percentage) * (rect.width - 6) / 100, 0, rect.width - 6);
  if (fillWidth > 0) {
    renderer.fillRoundedRect(rect.x + 2, rect.y + 2, fillWidth, rect.height - 4, 1, Color::Black);
  }
  if (charging) drawBatteryLightningBolt(renderer, rect.x + 4, rect.y + 2);
}

void LyraTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title, const char* subtitle) const {
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  constexpr int titleTop = 11;
  const int contentLeft = rect.x + LyraMetrics::values.contentSidePadding;
  int contentRight = rect.x + rect.width - LyraMetrics::values.contentSidePadding;
  const bool primaryHeader = rect.y <= LyraMetrics::values.topPadding;
  if (primaryHeader && SETTINGS.showBatteryIndicator) {
    contentRight -= UITheme::getInstance().getSystemBatteryOverlayWidth(renderer) + hPaddingInSelection;
  }
  const bool rtl = title && BidiUtils::startsWithRtl(title);
  int secondaryWidth = 0;
  if (subtitle) {
    if (auto* cache = renderer.getFontCacheManager()) cache->warmGlyphCache(SMALL_FONT_ID, subtitle);
    const auto secondary =
        renderer.truncatedText(SMALL_FONT_ID, subtitle, std::max(0, (contentRight - contentLeft) / 2));
    secondaryWidth = renderer.getTextWidth(SMALL_FONT_ID, secondary.c_str());
    const int secondaryX = rtl ? contentLeft : contentRight - secondaryWidth;
    renderer.drawText(SMALL_FONT_ID, secondaryX, rect.y + titleTop + 5, secondary.c_str());
  }
  if (title) {
    if (auto* cache = renderer.getFontCacheManager()) cache->warmGlyphCache(HEADER_FONT_ID, title);
    const int maxWidth = std::max(0, contentRight - contentLeft - secondaryWidth - hPaddingInSelection);
    const auto heading = renderer.truncatedText(HEADER_FONT_ID, title, maxWidth);
    const int headingWidth = renderer.getTextWidth(HEADER_FONT_ID, heading.c_str());
    const int headingX = rtl ? contentRight - headingWidth : contentLeft;
    renderer.drawText(HEADER_FONT_ID, headingX, rect.y + titleTop, heading.c_str(), true, EpdFontFamily::BOLD);
  }
  drawHairline(renderer, contentLeft, rect.x + rect.width - LyraMetrics::values.contentSidePadding,
               rect.y + rect.height - 2);
}

void LyraTheme::drawSubHeader(const GfxRenderer& renderer, Rect rect, const char* label, const char* rightLabel) const {
  const bool rtl = label && BidiUtils::startsWithRtl(label);
  int secondarySpace = LyraMetrics::values.contentSidePadding;
  if (rightLabel) {
    auto secondary =
        renderer.truncatedText(SMALL_FONT_ID, rightLabel, maxListValueWidth, EpdFontFamily::REGULAR);
    const int secondaryWidth = renderer.getTextWidth(SMALL_FONT_ID, secondary.c_str());
    const int secondaryX = rtl ? rect.x + LyraMetrics::values.contentSidePadding
                               : rect.x + rect.width - LyraMetrics::values.contentSidePadding - secondaryWidth;
    renderer.drawText(SMALL_FONT_ID, secondaryX, rect.y + 9, secondary.c_str());
    secondarySpace += secondaryWidth + hPaddingInSelection;
  }

  auto heading = renderer.truncatedText(UI_10_FONT_ID, label,
                                        rect.width - LyraMetrics::values.contentSidePadding - secondarySpace,
                                        EpdFontFamily::BOLD);
  const int headingWidth = renderer.getTextWidth(UI_10_FONT_ID, heading.c_str());
  const int headingX = rtl ? rect.x + rect.width - LyraMetrics::values.contentSidePadding - headingWidth
                           : rect.x + LyraMetrics::values.contentSidePadding;
  renderer.drawText(UI_10_FONT_ID, headingX, rect.y + 8, heading.c_str(), true, EpdFontFamily::BOLD);

  drawHairline(renderer, rect.x + LyraMetrics::values.contentSidePadding,
               rect.x + rect.width - LyraMetrics::values.contentSidePadding, rect.y + rect.height - 1);
}

void LyraTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                           bool selected) const {
  int currentX = rect.x + LyraMetrics::values.contentSidePadding;

  for (const auto& tab : tabs) {
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, tab.label, EpdFontFamily::REGULAR);

    if (tab.selected) {
      if (selected) {
        drawSelection(renderer, Rect{currentX, rect.y + 3, textWidth + 2 * hPaddingInSelection, rect.height - 7});
      } else {
        renderer.drawLine(currentX, rect.y + rect.height - 3, currentX + textWidth + 2 * hPaddingInSelection,
                          rect.y + rect.height - 3, 2, true);
      }
    }

    renderer.drawText(UI_10_FONT_ID, currentX + hPaddingInSelection, rect.y + 8, tab.label, true,
                      tab.selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);

    currentX += textWidth + LyraMetrics::values.tabSpacing + 2 * hPaddingInSelection;
  }

  drawHairline(renderer, rect.x + LyraMetrics::values.contentSidePadding,
               rect.x + rect.width - LyraMetrics::values.contentSidePadding, rect.y + rect.height - 1);
}

int LyraTheme::getListPageItems(int contentHeight, bool hasSubtitle) const {
  int rowHeight = (hasSubtitle) ? LyraMetrics::values.listWithSubtitleRowHeight : LyraMetrics::values.listRowHeight;
  return contentHeight / rowHeight;
}

void LyraTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle,
                         const std::function<std::string(int index)>& rowSubtitle,
                         const std::function<UIIcon(int index)>& rowIcon,
                         const std::function<std::string(int index)>& rowValue, bool highlightValue,
                         const std::function<bool(int index)>& rowDimmed,
                         const std::function<UIAccessory(int index)>& rowAccessory) const {
  const int rowHeight =
      rowSubtitle ? LyraMetrics::values.listWithSubtitleRowHeight : LyraMetrics::values.listRowHeight;
  const int titleFontId = rowSubtitle ? UI_12_FONT_ID : UI_10_FONT_ID;
  const int subtitleFontId = UI_10_FONT_ID;
  const int pageItems = std::max(1, rect.height / rowHeight);
  const int pageStartIndex = selectedIndex >= 0 ? selectedIndex / pageItems * pageItems : 0;
  const int pageEndIndex = std::min(itemCount, pageStartIndex + pageItems);

  // Batch the visible labels before drawing. The compressed built-in fonts
  // are grouped by Unicode range; warming in group order avoids repeatedly
  // inflating the same 10-35 KB block as rows alternate regular/bold text.
  std::string regularText;
  std::string boldText;
  std::string subtitleGlyphs;
  for (int i = pageStartIndex; i < pageEndIndex; ++i) {
    const std::string title = rowTitle(i);
    regularText.append(title).push_back('\n');
    if (i == selectedIndex) boldText.append(title).push_back('\n');
    if (rowSubtitle) subtitleGlyphs.append(rowSubtitle(i)).push_back('\n');
    if (rowValue) {
      const std::string value = rowValue(i);
      regularText.append(value).push_back('\n');
      if (highlightValue && i == selectedIndex) boldText.append(value).push_back('\n');
    }
  }
  if (auto* cache = renderer.getFontCacheManager()) {
    cache->warmGlyphCache(titleFontId, regularText.c_str(), 1U << EpdFontFamily::REGULAR);
    cache->warmGlyphCache(titleFontId, boldText.c_str(), 1U << EpdFontFamily::BOLD);
    cache->warmGlyphCache(subtitleFontId, subtitleGlyphs.c_str(), 1U << EpdFontFamily::REGULAR);
  }

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    const int scrollAreaHeight = rect.height;
    const int scrollBarHeight = std::max(28, (scrollAreaHeight * pageItems) / itemCount);
    const int currentPage = selectedIndex / pageItems;
    const int scrollBarY = rect.y + ((scrollAreaHeight - scrollBarHeight) * currentPage) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - LyraMetrics::values.scrollBarRightOffset;
    renderer.fillRoundedRect(scrollBarX - LyraMetrics::values.scrollBarWidth, scrollBarY,
                             LyraMetrics::values.scrollBarWidth, scrollBarHeight,
                             LyraMetrics::values.scrollBarWidth / 2, Color::Black);
  }

  const int contentWidth =
      rect.width -
      (totalPages > 1 ? (LyraMetrics::values.scrollBarWidth + LyraMetrics::values.scrollBarRightOffset) : 1);
  if (selectedIndex >= 0) {
    const int selectedY = rect.y + selectedIndex % pageItems * rowHeight;
    drawSelection(renderer, Rect{rect.x + LyraMetrics::values.contentSidePadding, selectedY + 4,
                                 contentWidth - LyraMetrics::values.contentSidePadding * 2, rowHeight - 8});
  }

  const int rowLeft = rect.x + LyraMetrics::values.contentSidePadding + hPaddingInSelection;
  const int rowRight = rect.x + contentWidth - LyraMetrics::values.contentSidePadding - hPaddingInSelection;
  const int iconSize = rowIcon ? listIconSize : 0;
  const int titleLineHeight = renderer.getLineHeight(titleFontId);
  const int subtitleLineHeight = renderer.getLineHeight(subtitleFontId);

  for (int i = pageStartIndex; i < pageEndIndex; ++i) {
    const int itemY = rect.y + (i % pageItems) * rowHeight;
    const std::string itemName = rowTitle(i);
    const bool rowRtl = BidiUtils::startsWithRtl(itemName.c_str());
    const UIAccessory accessory = rowAccessory ? rowAccessory(i) : UIAccessory::None;
    const int accessoryW = accessory == UIAccessory::None ? 0 : accessoryWidth(accessory);
    const int accessoryH = accessory == UIAccessory::None ? 0 : 16;
    const int accessorySpace = accessory == UIAccessory::None ? 0 : accessoryW + hPaddingInSelection;

    const int iconX = rowRtl ? rowRight - iconSize : rowLeft;
    const int accessoryX = rowRtl ? rowLeft : rowRight - accessoryW;
    const int textLeft = rowLeft + (rowRtl ? accessorySpace : (iconSize > 0 ? iconSize + hPaddingInSelection : 0));
    const int textRight =
        rowRight - (rowRtl ? (iconSize > 0 ? iconSize + hPaddingInSelection : 0) : accessorySpace);

    std::string valueText;
    int valueWidth = 0;
    if (rowValue) {
      valueText = rowValue(i);
      const int valueMaxWidth = std::min(maxListValueWidth, std::max(0, (textRight - textLeft) / 2));
      const auto valueStyle =
          highlightValue && i == selectedIndex ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      valueText = renderer.truncatedText(UI_10_FONT_ID, valueText.c_str(), valueMaxWidth, valueStyle);
      valueWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str(), valueStyle);
    }

    const int valueGap = valueWidth > 0 ? hPaddingInSelection : 0;
    const int rowTextWidth = std::max(0, textRight - textLeft - valueWidth - valueGap);
    const int textLaneLeft = rowRtl ? textLeft + valueWidth + valueGap : textLeft;
    const int textLaneRight = rowRtl ? textRight : textRight - valueWidth - valueGap;
    const auto titleStyle = i == selectedIndex ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const auto item = renderer.truncatedText(titleFontId, itemName.c_str(), rowTextWidth, titleStyle);
    const int itemWidth = renderer.getTextWidth(titleFontId, item.c_str(), titleStyle);
    const int titleX = rowRtl ? textLaneRight - itemWidth : textLaneLeft;
    const int titleY =
        rowSubtitle ? itemY + 8 : itemY + std::max(0, (rowHeight - titleLineHeight) / 2);
    renderer.drawText(titleFontId, titleX, titleY, item.c_str(), true,
                      i == selectedIndex ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);

    if (rowDimmed && rowDimmed(i) && i != selectedIndex) {
      for (int py = titleY; py < titleY + titleLineHeight; ++py)
        for (int px = titleX; px < titleX + itemWidth; ++px)
          if ((px + py) % 2 == 0) renderer.drawPixel(px, py, false);
    }

    if (rowIcon) {
      const UIIcon icon = rowIcon(i);
      const uint8_t* iconBitmap = iconForName(icon, iconSize);
      if (iconBitmap) renderer.drawIcon(iconBitmap, iconX, itemY + (rowHeight - iconSize) / 2, iconSize, iconSize);
    }

    if (rowSubtitle) {
      const std::string subtitleText = rowSubtitle(i);
      const auto subtitle = renderer.truncatedText(subtitleFontId, subtitleText.c_str(), rowTextWidth);
      const int subtitleWidth = renderer.getTextWidth(subtitleFontId, subtitle.c_str());
      const int subtitleX =
          BidiUtils::startsWithRtl(subtitleText.c_str()) ? textLaneRight - subtitleWidth : textLaneLeft;
      renderer.drawText(subtitleFontId, subtitleX, itemY + rowHeight - subtitleLineHeight - 7, subtitle.c_str(), true);
    }

    if (!valueText.empty()) {
      const int valueX = rowRtl ? textLeft : textRight - valueWidth;
      const int valueY = itemY + std::max(0, (rowHeight - titleLineHeight) / 2);
      const auto valueStyle =
          highlightValue && i == selectedIndex ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
      renderer.drawText(UI_10_FONT_ID, valueX, valueY, valueText.c_str(), true, valueStyle);
    }

    if (accessory != UIAccessory::None) {
      drawAccessory(renderer, accessory, accessoryX, itemY + (rowHeight - accessoryH) / 2, rowRtl);
    }

    const bool rowIsSelected = i == selectedIndex;
    const bool nextRowIsSelected = i + 1 == selectedIndex;
    if (!rowIsSelected && !nextRowIsSelected && i + 1 < pageEndIndex) {
      drawHairline(renderer, textLeft, textRight, itemY + rowHeight - 1);
    }
  }
}

void LyraTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4) const {
  BaseTheme::drawButtonHints(renderer, btn1, btn2, btn3, btn4);
}

void LyraTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  if (!SETTINGS.showButtonHints) return;

  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = LyraMetrics::values.sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 78;                                       // Height on screen (width when rotated)
  constexpr int buttonMargin = 0;

  if (gpio.deviceIsX3()) {
    // X3 layout: Up on left side, Down on right side, positioned higher
    constexpr int x3ButtonY = 155;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawRoundedRect(buttonMargin, x3ButtonY, buttonWidth, buttonHeight, 1, cornerRadius, false, true, false,
                               true, true);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, topBtn);
      renderer.drawTextRotated90CW(SMALL_FONT_ID, buttonMargin, x3ButtonY + (buttonHeight + textWidth) / 2, topBtn);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      const int rightX = screenWidth - buttonWidth;
      renderer.drawRoundedRect(rightX, x3ButtonY, buttonWidth, buttonHeight, 1, cornerRadius, true, false, true, false,
                               true);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, bottomBtn);
      renderer.drawTextRotated90CW(SMALL_FONT_ID, rightX, x3ButtonY + (buttonHeight + textWidth) / 2, bottomBtn);
    }
  } else {
    // X4 layout: Both buttons stacked on right side
    const char* labels[] = {topBtn, bottomBtn};
    const int x = screenWidth - buttonWidth;

    if (topBtn != nullptr && topBtn[0] != '\0') {
      renderer.drawRoundedRect(x, topHintButtonY, buttonWidth, buttonHeight, 1, cornerRadius, true, false, true, false,
                               true);
    }

    if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
      renderer.drawRoundedRect(x, topHintButtonY + buttonHeight + 5, buttonWidth, buttonHeight, 1, cornerRadius, true,
                               false, true, false, true);
    }

    for (int i = 0; i < 2; i++) {
      if (labels[i] != nullptr && labels[i][0] != '\0') {
        const int y = topHintButtonY + (i * buttonHeight) + 5;
        const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
        renderer.drawTextRotated90CW(SMALL_FONT_ID, x, y + (buttonHeight + textWidth) / 2, labels[i]);
      }
    }
  }
}

void LyraTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  (void)selectorIndex;
  (void)coverRendered;
  (void)coverBufferStored;
  (void)bufferRestored;
  (void)storeCoverBuffer;

  const int cardX = rect.x + 58;
  const int cardY = rect.y + 6;
  const int cardWidth = rect.width - 116;
  const int cardHeight = std::min(208, rect.height - 12);
  renderer.drawRoundedRect(cardX, cardY, cardWidth, cardHeight, 1, 12, true, true, true, true, true);
  renderer.drawIcon(LucideBookOpen24, cardX + 28, cardY + 42, 24, 24);

  const int textX = cardX + 82;
  const int textWidth = cardWidth - 104;
  renderer.drawText(UI_10_FONT_ID, textX, cardY + 24,
                    recentBooks.empty() ? tr(STR_START_READING) : tr(STR_CONTINUE_READING), true, EpdFontFamily::BOLD);

  if (recentBooks.empty()) {
    renderer.drawText(UI_10_FONT_ID, textX, cardY + 72, tr(STR_NO_OPEN_BOOK));
    renderer.drawText(SMALL_FONT_ID, textX, cardY + 112, tr(STR_OPEN_LIBRARY_HINT));
    return;
  }

  const RecentBook& book = recentBooks.front();
  const auto title = renderer.truncatedText(UI_12_FONT_ID, book.title.c_str(), textWidth, EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, textX, cardY + 68, title.c_str(), true, EpdFontFamily::BOLD);
  if (!book.author.empty()) {
    const auto author = renderer.truncatedText(UI_10_FONT_ID, book.author.c_str(), textWidth);
    renderer.drawText(UI_10_FONT_ID, textX, cardY + 108, author.c_str());
  }
  renderer.drawLine(textX, cardY + cardHeight - 36, cardX + cardWidth - 76, cardY + cardHeight - 36, 4, true);
  renderer.drawText(SMALL_FONT_ID, cardX + cardWidth - 56, cardY + cardHeight - 46, tr(STR_OPEN));
}

void LyraTheme::drawEmptyRecents(const GfxRenderer& renderer, const Rect rect) const {
  constexpr int padding = 48;
  renderer.drawText(UI_12_FONT_ID, rect.x + padding,
                    rect.y + rect.height / 2 - renderer.getLineHeight(UI_12_FONT_ID) - 2, tr(STR_NO_OPEN_BOOK), true,
                    EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, rect.x + padding, rect.y + rect.height / 2 + 2, tr(STR_START_READING), true);
}

void LyraTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<UIIcon(int index)>& rowIcon) const {
  std::string labels;
  std::string selectedLabel;
  for (int i = 0; i < buttonCount; ++i) {
    const std::string label = buttonLabel(i);
    labels.append(label).push_back('\n');
    if (i == selectedIndex) selectedLabel = label;
  }
  if (auto* cache = renderer.getFontCacheManager()) {
    cache->warmGlyphCache(UI_12_FONT_ID, labels.c_str(), 1U << EpdFontFamily::REGULAR);
    cache->warmGlyphCache(UI_12_FONT_ID, selectedLabel.c_str(), 1U << EpdFontFamily::BOLD);
  }

  for (int i = 0; i < buttonCount; ++i) {
    const int tileWidth = rect.width - LyraMetrics::values.contentSidePadding * 2;
    const Rect tileRect =
        Rect{rect.x + LyraMetrics::values.contentSidePadding,
             rect.y + i * (LyraMetrics::values.menuRowHeight + LyraMetrics::values.menuSpacing), tileWidth,
             LyraMetrics::values.menuRowHeight};

    const bool selected = selectedIndex == i;

    if (selected) {
      drawSelection(renderer, Rect{tileRect.x, tileRect.y + 3, tileRect.width, tileRect.height - 6});
    }

    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();
    const bool rtl = BidiUtils::startsWithRtl(label);
    const int iconX = rtl ? tileRect.x + tileRect.width - 16 - mainMenuIconSize : tileRect.x + 16;
    const int accessoryX = rtl ? tileRect.x + 12 : tileRect.x + tileRect.width - 28;
    const int textLeft = tileRect.x + 16 + (rtl ? 22 : mainMenuIconSize + hPaddingInSelection);
    const int textRight =
        tileRect.x + tileRect.width - 16 - (rtl ? mainMenuIconSize + hPaddingInSelection : 22);
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int textY = tileRect.y + (LyraMetrics::values.menuRowHeight - lineHeight) / 2;

    if (rowIcon) {
      const UIIcon icon = rowIcon(i);
      const uint8_t* iconBitmap = iconForName(icon, mainMenuIconSize);
      if (iconBitmap)
        renderer.drawIcon(iconBitmap, iconX, tileRect.y + (tileRect.height - mainMenuIconSize) / 2, mainMenuIconSize,
                          mainMenuIconSize);
    }

    const auto truncated =
        renderer.truncatedText(UI_12_FONT_ID, label, std::max(0, textRight - textLeft),
                               selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    const int labelWidth = renderer.getTextWidth(UI_12_FONT_ID, truncated.c_str(),
                                                 selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    const int textX = rtl ? textRight - labelWidth : textLeft;
    renderer.drawText(UI_12_FONT_ID, textX, textY, truncated.c_str(), true,
                      selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    drawAccessory(renderer, UIAccessory::Chevron, accessoryX, tileRect.y + (tileRect.height - 16) / 2, rtl);

    const bool nextSelected = i + 1 == selectedIndex;
    if (!selected && !nextSelected && i + 1 < buttonCount) {
      drawHairline(renderer, textLeft, textRight, tileRect.y + tileRect.height + LyraMetrics::values.menuSpacing / 2);
    }
  }
}
