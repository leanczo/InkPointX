#include "ClearCacheActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_task_wdt.h>

#include "MappedInputManager.h"
#include "activities/home/HomeActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"

void ClearCacheActivity::onEnter() {
  Activity::onEnter();

  state = WARNING;
  requestUpdate();
}

void ClearCacheActivity::onExit() { Activity::onExit(); }

void ClearCacheActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CLEAR_READING_CACHE));

  // Laid out from the real line height. The previous fixed offsets of -60/-30/+10/+30
  // were picked when this font's line box was 30 px tall; at 35 px the third and
  // fourth lines overlapped by 8 px.
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = UITheme::getListContentBottom(renderer, false);
  const auto centredBlockTop = [&](const int lineCount) {
    return contentTop + std::max(0, (contentBottom - contentTop - lineCount * lineHeight) * 2 / 5);
  };

  if (state == WARNING) {
    int y = centredBlockTop(4);
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_CLEAR_CACHE_WARNING_1), true);
    y += lineHeight;
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_CLEAR_CACHE_WARNING_2), true, EpdFontFamily::BOLD);
    y += lineHeight;
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_CLEAR_CACHE_WARNING_3), true);
    y += lineHeight;
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_CLEAR_CACHE_WARNING_4), true);

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CLEAR_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == CLEARING) {
    renderer.drawCenteredText(UI_10_FONT_ID, centredBlockTop(1), tr(STR_CLEARING_CACHE));
    renderer.displayBuffer();
    return;
  }

  if (state == SUCCESS) {
    int y = centredBlockTop(2);
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_CACHE_CLEARED), true, EpdFontFamily::BOLD);
    y += lineHeight;
    std::string resultText = std::to_string(clearedCount) + " " + std::string(tr(STR_ITEMS_REMOVED));
    if (failedCount > 0) {
      resultText += ", " + std::to_string(failedCount) + " " + std::string(tr(STR_FAILED_LOWER));
    }
    renderer.drawCenteredText(UI_10_FONT_ID, y, resultText.c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FAILED) {
    int y = centredBlockTop(2);
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_CLEAR_CACHE_FAILED), true, EpdFontFamily::BOLD);
    y += lineHeight;
    // "Check serial output" was useless advice on a device with no exposed
    // serial port; the only failure path here is an unreadable cache dir.
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_ERROR_GENERAL_FAILURE));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}

void ClearCacheActivity::clearCache() {
  LOG_DBG("CLEAR_CACHE", "Clearing cache...");

  // Open .crosspoint directory
  auto root = Storage.open("/.crosspoint");
  if (!root || !root.isDirectory()) {
    LOG_DBG("CLEAR_CACHE", "Failed to open cache directory");
    if (root) root.close();
    state = FAILED;
    requestUpdate();
    return;
  }

  clearedCount = 0;
  failedCount = 0;
  char name[128];

  // Iterate through all entries in the directory
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    esp_task_wdt_reset();  // one book cache can hold hundreds of files; feed per entry
    file.getName(name, sizeof(name));
    String itemName(name);

    // Only delete directories matching known book cache names.
    if (file.isDirectory() && isBookCacheDirectoryName(itemName.c_str())) {
      String fullPath = "/.crosspoint/" + itemName;
      LOG_DBG("CLEAR_CACHE", "Removing cache: %s", fullPath.c_str());

      file.close();  // Close before attempting to delete

      if (Storage.removeDir(fullPath.c_str())) {
        clearedCount++;
      } else {
        LOG_ERR("CLEAR_CACHE", "Failed to remove: %s", fullPath.c_str());
        failedCount++;
      }
    } else {
      file.close();
    }
  }
  root.close();

  LOG_DBG("CLEAR_CACHE", "Cache cleared: %d removed, %d failed", clearedCount, failedCount);

  HomeActivity::invalidateDetailsCache();

  state = SUCCESS;
  requestUpdate();
}

void ClearCacheActivity::loop() {
  if (state == WARNING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      LOG_DBG("CLEAR_CACHE", "User confirmed, starting cache clear");
      {
        RenderLock lock(*this);
        state = CLEARING;
      }
      requestUpdateAndWait();

      clearCache();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      LOG_DBG("CLEAR_CACHE", "User cancelled");
      goBack();
    }
    return;
  }

  if (state == SUCCESS || state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }
}
