#include "OtaUpdateActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/OtaUpdater.h"
#include "util/BootDiag.h"

const char* OtaUpdateActivity::failureText(const int result) {
  // The user could not previously tell "no network" from "bad image" — the
  // reason was logged and thrown away.
  switch (result) {
    case OtaUpdater::HTTP_ERROR:
      return tr(STR_SYNC_ERR_NETWORK);
    case OtaUpdater::OOM_ERROR:
      return tr(STR_MEMORY_ERROR);
    case OtaUpdater::JSON_PARSE_ERROR:
    case OtaUpdater::UPDATE_OLDER_ERROR:
    case OtaUpdater::INTERNAL_UPDATE_ERROR:
    default:
      return tr(STR_ERROR_GENERAL_FAILURE);
  }
}

void OtaUpdateActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_ERR("OTA", "WiFi connection failed, exiting");
    finish();
    return;
  }

  LOG_DBG("OTA", "WiFi connected, checking for update");

  {
    RenderLock lock(*this);
    state = CHECKING_FOR_UPDATE;
  }
  requestUpdateAndWait();

  const auto res = updater.checkForUpdate();
  if (res != OtaUpdater::OK) {
    LOG_DBG("OTA", "Update check failed: %d", res);
    {
      RenderLock lock(*this);
      state = FAILED;
      failureReason = failureText(res);
    }
    return;
  }

  if (!updater.isUpdateNewer()) {
    LOG_DBG("OTA", "No new update available");
    {
      RenderLock lock(*this);
      state = NO_UPDATE;
    }
    return;
  }

  {
    RenderLock lock(*this);
    state = WAITING_CONFIRMATION;
  }
}

void OtaUpdateActivity::onEnter() {
  Activity::onEnter();

  // Turn on WiFi immediately
  LOG_DBG("OTA", "Turning on WiFi...");
  WiFi.mode(WIFI_STA);

  // Launch WiFi selection subactivity
  LOG_DBG("OTA", "Launching WifiSelectionActivity...");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void OtaUpdateActivity::onExit() {
  Activity::onExit();

  // Success path reboots via the SHUTTING_DOWN state's plain ESP.restart()
  // (loop() above) so the new firmware boots normally. Back-out paths land
  // here with wifi still active; silent-restart to free the LWIP/mbedTLS
  // fragmentation, same as the other wifi activities.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void OtaUpdateActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_UPDATE));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  float updaterProgress = 0;
  if (state == UPDATE_IN_PROGRESS) {
    LOG_DBG("OTA", "Update progress: %d / %d", updater.getProcessedSize(), updater.getTotalSize());
    const size_t totalSize = updater.getTotalSize();
    if (totalSize > 0) {
      updaterProgress = static_cast<float>(updater.getProcessedSize()) / static_cast<float>(totalSize);
    }
    const int updatePercent = static_cast<int>(updaterProgress * 100);
    if (updatePercent == lastUpdaterPercentage) {
      return;
    }
    lastUpdaterPercentage = updatePercent;
  }

  if (state == CHECKING_FOR_UPDATE) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_CHECKING_UPDATE));
  } else if (state == WAITING_CONFIRMATION) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_NEW_UPDATE), true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, top + height + metrics.verticalSpacing,
                      (std::string(tr(STR_CURRENT_VERSION)) + CROSSPOINT_VERSION).c_str());
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, top + height * 2 + metrics.verticalSpacing * 2,
                      (std::string(tr(STR_NEW_VERSION)) + updater.getLatestVersion()).c_str());

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_UPDATE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == UPDATE_IN_PROGRESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATING));

    int y = top + height + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(updaterProgress * 100), 100);

    y += metrics.progressBarHeight + metrics.verticalSpacing;
    // Percent label is drawn by BaseTheme::drawProgressBar; this slot is left intentionally empty
    // so the bytes line below stays at the same Y it was at when the activity drew its own percent.
    y += height + metrics.verticalSpacing;
    renderer.drawCenteredText(
        UI_10_FONT_ID, y,
        (std::to_string(updater.getProcessedSize()) + " / " + std::to_string(updater.getTotalSize())).c_str());
    // Same warning the SD flasher shows: an interrupted OTA is a brick risk.
    y += height + metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_FIRMWARE_UPDATE_DO_NOT_POWER_OFF), true, EpdFontFamily::BOLD);
  } else if (state == NO_UPDATE) {
    GUI.drawEmptyState(renderer,
                       Rect{0, top, pageWidth,
                            pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - top},
                       tr(STR_NO_UPDATE), nullptr, /*script=*/true);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FAILED) {
    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    GUI.drawEmptyState(renderer,
                       Rect{0, contentTop, pageWidth,
                            pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - contentTop},
                       tr(STR_UPDATE_FAILED), failureReason);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FINISHED) {
    GUI.drawEmptyState(renderer,
                       Rect{0, top, pageWidth,
                            pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - top},
                       tr(STR_UPDATE_COMPLETE), tr(STR_POWER_ON_HINT), /*script=*/true);
  }

  renderer.displayBuffer();
}

void OtaUpdateActivity::loop() {
  if (state == WAITING_CONFIRMATION) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      LOG_DBG("OTA", "New update available, starting download...");
      {
        RenderLock lock(*this);
        state = UPDATE_IN_PROGRESS;
      }
      requestUpdateAndWait();
      const auto res = updater.installUpdate(
          [](void* ctx) {
            // immediate=true notifies the render task directly. The default deferred path only
            // sets a flag consumed at the end of ActivityManager::loop(), which never runs while
            // installUpdate() blocks this task.
            static_cast<OtaUpdateActivity*>(ctx)->requestUpdate(true);
          },
          this);

      if (res != OtaUpdater::OK) {
        LOG_DBG("OTA", "Update failed: %d", res);
        {
          RenderLock lock(*this);
          state = FAILED;
          failureReason = failureText(res);
        }
        requestUpdate();
        return;
      }

      {
        RenderLock lock(*this);
        state = FINISHED;
      }
      requestUpdateAndWait();
      // Hold the completion screen briefly so the user sees it, then restart.
      delay(3000);
      {
        RenderLock lock(*this);
        state = SHUTTING_DOWN;
      }
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }

    return;
  }

  if (state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      // Re-run the whole check: covers both a failed check and a failed
      // install without special-casing.
      if (WiFi.status() == WL_CONNECTED) {
        onWifiSelectionComplete(true);
        requestUpdate();
      } else {
        startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                               [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
      }
    }
    return;
  }

  if (state == NO_UPDATE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  if (state == SHUTTING_DOWN) {
    BootDiag::markCleanShutdown(BootDiag::Shutdown::Restart);
    ESP.restart();
  }
}
