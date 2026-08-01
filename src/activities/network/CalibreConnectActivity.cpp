#include "CalibreConnectActivity.h"

#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* HOSTNAME = "inkpoint";
}  // namespace

void CalibreConnectActivity::onEnter() {
  Activity::onEnter();

  requestUpdate();
  state = CalibreConnectState::WIFI_SELECTION;
  connectedIP.clear();
  connectedSSID.clear();
  lastHandleClientTime = 0;
  lastProgressReceived = 0;
  lastProgressTotal = 0;
  currentUploadName.clear();
  lastCompleteName.clear();
  lastCompleteAt = 0;
  lastProcessedCompleteAt = 0;
  exitRequested = false;

  if (WiFi.status() != WL_CONNECTED) {
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& wifi = std::get<WifiResult>(result.data);
                               connectedIP = wifi.ip;
                               connectedSSID = wifi.ssid;
                             }
                             onWifiSelectionComplete(!result.isCancelled);
                           });
  } else {
    connectedIP = WiFi.localIP().toString().c_str();
    connectedSSID = WiFi.SSID().c_str();
    startWebServer();
  }
}

void CalibreConnectActivity::onExit() {
  Activity::onExit();

  MDNS.end();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void CalibreConnectActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    finish();
    return;
  }

  startWebServer();
}

void CalibreConnectActivity::startWebServer() {
  state = CalibreConnectState::SERVER_STARTING;
  requestUpdate();

  MDNS.end();
  if (MDNS.begin(HOSTNAME)) {
    // mDNS is optional for the Calibre plugin but still helpful for users.
    LOG_DBG("CAL", "mDNS started: http://%s.local/", HOSTNAME);
  }

  webServer.reset(new CrossPointWebServer());
  webServer->begin();

  if (webServer->isRunning()) {
    state = CalibreConnectState::SERVER_RUNNING;
    requestUpdate();
  } else {
    state = CalibreConnectState::ERROR;
    requestUpdate();
  }
}

void CalibreConnectActivity::stopWebServer() {
  if (webServer) {
    webServer->stop();
    webServer.reset();
  }
}

void CalibreConnectActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    exitRequested = true;
  }

  // Without this the screen keeps showing setup instructions forever when the
  // access point drops, which is indistinguishable from a hang.
  if (webServer && webServer->isRunning() && millis() - lastWifiCheckAt > 2000) {
    lastWifiCheckAt = millis();
    if (WiFi.status() != WL_CONNECTED) {
      if (firstDisconnectAt == 0) {
        firstDisconnectAt = millis();
      } else if (millis() - firstDisconnectAt > WIFI_ABANDON_MS) {
        LOG_ERR("CAL", "WiFi unavailable for >%lu s; leaving Calibre mode", WIFI_ABANDON_MS / 1000UL);
        state = CalibreConnectState::ERROR;
        stopWebServer();
        requestUpdate();
        return;
      }
    } else {
      firstDisconnectAt = 0;
    }
  }

  if (webServer && webServer->isRunning()) {
    const unsigned long timeSinceLastHandleClient = millis() - lastHandleClientTime;
    if (lastHandleClientTime > 0 && timeSinceLastHandleClient > 100) {
      LOG_DBG("CAL", "WARNING: %lu ms gap since last handleClient", timeSinceLastHandleClient);
    }

    esp_task_wdt_reset();
    constexpr int MAX_ITERATIONS = 80;
    for (int i = 0; i < MAX_ITERATIONS && webServer->isRunning(); i++) {
      webServer->handleClient();
      if ((i & 0x07) == 0x07) {
        esp_task_wdt_reset();
      }
      if ((i & 0x0F) == 0x0F) {
        yield();
        if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
          exitRequested = true;
          break;
        }
      }
    }
    lastHandleClientTime = millis();

    const auto status = webServer->getWsUploadStatus();
    bool changed = false;
    if (status.inProgress) {
      // A new file, or the first frame of one, repaints immediately; ongoing
      // byte counts are rate limited so the panel is not driven once per 64 KB.
      const bool newUpload = status.filename != currentUploadName || lastProgressTotal != status.total;
      const bool dueForRepaint = millis() - lastProgressRepaintAt >= PROGRESS_REPAINT_MIN_MS;
      const bool finished = status.total > 0 && status.received >= status.total;
      if (newUpload || dueForRepaint || finished) {
        if (status.received != lastProgressReceived || status.total != lastProgressTotal ||
            status.filename != currentUploadName) {
          lastProgressReceived = status.received;
          lastProgressTotal = status.total;
          currentUploadName = status.filename;
          lastProgressRepaintAt = millis();
          changed = true;
        }
      }
    } else if (lastProgressReceived != 0 || lastProgressTotal != 0) {
      lastProgressReceived = 0;
      lastProgressTotal = 0;
      currentUploadName.clear();
      changed = true;
    }
    // Only update lastCompleteAt if the server has a NEW value (not one we already processed)
    // This prevents restoring an old value after the 6s timeout clears it
    if (status.lastCompleteAt != 0 && status.lastCompleteAt != lastProcessedCompleteAt) {
      lastCompleteAt = status.lastCompleteAt;
      lastCompleteName = status.lastCompleteName;
      lastProcessedCompleteAt = status.lastCompleteAt;  // Mark this value as processed
      changed = true;
    }
    if (lastCompleteAt > 0 && (millis() - lastCompleteAt) >= 6000) {
      lastCompleteAt = 0;
      lastCompleteName.clear();
      // Note: we DON'T reset lastProcessedCompleteAt here, so we won't re-process the old server value
      changed = true;
    }
    if (changed) {
      requestUpdate();
    }
  }

  if (exitRequested) {
    finish();
    return;
  }
}

void CalibreConnectActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CALIBRE_WIRELESS));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == CalibreConnectState::SERVER_STARTING) {
    renderer.drawCenteredText(UI_12_FONT_ID, top, tr(STR_CALIBRE_STARTING));
  } else if (state == CalibreConnectState::ERROR) {
    renderer.drawCenteredText(UI_12_FONT_ID, top, tr(STR_CONNECTION_FAILED), true, EpdFontFamily::BOLD);
    // Back has always worked here; the legend just never said so.
    const auto errorLabels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, errorLabels.btn1, errorLabels.btn2, errorLabels.btn3, errorLabels.btn4);
  } else if (state == CalibreConnectState::SERVER_RUNNING) {
    // Raw IP on the right: with the localized "IP Address:" prefix the value
    // exceeded the 200 px value lane and was cut mid-octet — the one fact this
    // screen exists to show. A bare dotted quad needs no label.
    GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.subHeaderHeight},
                      connectedSSID.c_str(), connectedIP.c_str());

    int y = metrics.topPadding + metrics.headerHeight + metrics.subHeaderHeight + metrics.verticalSpacing * 4;
    const auto headingHeight = renderer.getTextHeight(HEADER_FONT_ID);
    renderer.drawText(HEADER_FONT_ID, metrics.contentSidePadding, y, tr(STR_CALIBRE_SETUP));
    y += headingHeight + metrics.verticalSpacing * 2;

    // Wrapped, and advanced by the SMALL line height — the old fixed grid used
    // the UI_10 metric and clipped 22 locales at the right edge.
    const int instructionLh = renderer.getLineHeight(SMALL_FONT_ID);
    const int instructionMaxWidth = pageWidth - metrics.contentSidePadding * 2;
    for (const auto instruction : {StrId::STR_CALIBRE_INSTRUCTION_1, StrId::STR_CALIBRE_INSTRUCTION_2,
                                   StrId::STR_CALIBRE_INSTRUCTION_3, StrId::STR_CALIBRE_INSTRUCTION_4}) {
      for (const auto& line : renderer.wrappedText(SMALL_FONT_ID, I18N.get(instruction), instructionMaxWidth, 2)) {
        renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y, line.c_str());
        y += instructionLh;
      }
    }

    y += metrics.verticalSpacing * 4;
    renderer.drawText(HEADER_FONT_ID, metrics.contentSidePadding, y, tr(STR_CALIBRE_STATUS));
    y += headingHeight + metrics.verticalSpacing * 2;

    if (lastProgressTotal > 0 && lastProgressReceived <= lastProgressTotal) {
      std::string label = tr(STR_CALIBRE_RECEIVING);
      if (!currentUploadName.empty()) {
        label += ": " + currentUploadName;
        label = renderer.truncatedText(SMALL_FONT_ID, label.c_str(), pageWidth - metrics.contentSidePadding * 2,
                                       EpdFontFamily::REGULAR);
      }
      renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y, label.c_str());
      GUI.drawProgressBar(renderer,
                          Rect{metrics.contentSidePadding, y + height + metrics.verticalSpacing,
                               pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
                          lastProgressReceived, lastProgressTotal);
      // drawProgressBar draws its own centred percent caption a line below
      // the bar — reserve that line, or the next status line lands on it.
      y += height + metrics.verticalSpacing * 2 + metrics.progressBarHeight +
           renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing;
    }

    if (lastCompleteAt > 0 && (millis() - lastCompleteAt) < 6000) {
      std::string msg = std::string(tr(STR_CALIBRE_RECEIVED)) + lastCompleteName;
      msg = renderer.truncatedText(SMALL_FONT_ID, msg.c_str(), pageWidth - metrics.contentSidePadding * 2,
                                   EpdFontFamily::REGULAR);
      renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y, msg.c_str());
    }

    const auto labels = mappedInput.mapLabels(tr(STR_EXIT), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}
