#pragma once

#include <functional>
#include <memory>
#include <string>

#include "activities/Activity.h"
#include "network/CrossPointWebServer.h"

enum class CalibreConnectState { WIFI_SELECTION, SERVER_STARTING, SERVER_RUNNING, ERROR };

/**
 * CalibreConnectActivity starts the file transfer server in STA mode,
 * but renders Calibre-specific instructions instead of the web transfer UI.
 */
class CalibreConnectActivity final : public Activity {
  CalibreConnectState state = CalibreConnectState::WIFI_SELECTION;

  std::unique_ptr<CrossPointWebServer> webServer;
  std::string connectedIP;
  std::string connectedSSID;
  unsigned long lastHandleClientTime = 0;
  size_t lastProgressReceived = 0;
  size_t lastProgressTotal = 0;
  std::string currentUploadName;
  std::string lastCompleteName;
  unsigned long lastCompleteAt = 0;
  unsigned long lastProcessedCompleteAt = 0;  // Track which server value we've already processed
  bool exitRequested = false;
  unsigned long lastWifiCheckAt = 0;
  unsigned long firstDisconnectAt = 0;
  unsigned long lastProgressRepaintAt = 0;

  // Matches CrossPointWebServerActivity: the driver retries on its own, so only
  // give up once the network has really been gone this long.
  static constexpr unsigned long WIFI_ABANDON_MS = 20000;
  // The server reports upload progress every 64 KB, so a 5 MB book would
  // otherwise trigger ~80 full-screen e-ink repaints during one transfer.
  static constexpr unsigned long PROGRESS_REPAINT_MIN_MS = 2000;

  void renderServerRunning() const;

  void onWifiSelectionComplete(bool connected);
  void startWebServer();
  void stopWebServer();

 public:
  explicit CalibreConnectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("CalibreConnect", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return webServer && webServer->isRunning(); }
  bool preventAutoSleep() override { return webServer && webServer->isRunning(); }
};
