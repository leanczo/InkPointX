#include <Arduino.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <SPI.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <builtinFonts/all.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "FavoriteBooksStore.h"
#include "InterfaceFont.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "WifiCredentialStore.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/settings/OtaUpdateActivity.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/LoadingIcon.h"
#include "activities/reader/ProgressFile.h"
#include "util/BookCacheUtils.h"
#include "util/BootDiag.h"
#include "util/ButtonNavigator.h"
#include "util/ScreenshotUtil.h"

// Native PDF parsing has legitimate recursive dictionary/array paths and can
// enter newlib formatting while several parser frames are live.  The Arduino
// default (8 KiB) is too small for real-world PDFs on ESP32-C3.
SET_LOOP_TASK_STACK_SIZE(16384);

GfxRenderer renderer(display);
MappedInputManager mappedInputManager(gpio, renderer);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());
static unsigned long allowSleepAt = 0;

// Fonts
EpdFont notoserif14RegularFont(&notoserif_14_regular);
EpdFont notoserif14BoldFont(&notoserif_14_bold);
EpdFont notoserif14ItalicFont(&notoserif_14_italic);
EpdFont notoserif14BoldItalicFont(&notoserif_14_bolditalic);
EpdFontFamily notoserif14FontFamily(&notoserif14RegularFont, &notoserif14BoldFont, &notoserif14ItalicFont,
                                    &notoserif14BoldItalicFont);
#ifndef OMIT_FONTS
EpdFont notoserif12RegularFont(&notoserif_12_regular);
EpdFont notoserif12BoldFont(&notoserif_12_bold);
EpdFont notoserif12ItalicFont(&notoserif_12_italic);
EpdFont notoserif12BoldItalicFont(&notoserif_12_bolditalic);
EpdFontFamily notoserif12FontFamily(&notoserif12RegularFont, &notoserif12BoldFont, &notoserif12ItalicFont,
                                    &notoserif12BoldItalicFont);
EpdFont notoserif16RegularFont(&notoserif_16_regular);
EpdFont notoserif16BoldFont(&notoserif_16_bold);
EpdFont notoserif16ItalicFont(&notoserif_16_italic);
EpdFont notoserif16BoldItalicFont(&notoserif_16_bolditalic);
EpdFontFamily notoserif16FontFamily(&notoserif16RegularFont, &notoserif16BoldFont, &notoserif16ItalicFont,
                                    &notoserif16BoldItalicFont);
EpdFont notoserif18RegularFont(&notoserif_18_regular);
EpdFont notoserif18BoldFont(&notoserif_18_bold);
EpdFont notoserif18ItalicFont(&notoserif_18_italic);
EpdFont notoserif18BoldItalicFont(&notoserif_18_bolditalic);
EpdFontFamily notoserif18FontFamily(&notoserif18RegularFont, &notoserif18BoldFont, &notoserif18ItalicFont,
                                    &notoserif18BoldItalicFont);

EpdFont notosans12RegularFont(&notosans_12_regular);
EpdFont notosans12BoldFont(&notosans_12_bold);
EpdFont notosans12ItalicFont(&notosans_12_italic);
EpdFont notosans12BoldItalicFont(&notosans_12_bolditalic);
EpdFontFamily notosans12FontFamily(&notosans12RegularFont, &notosans12BoldFont, &notosans12ItalicFont,
                                   &notosans12BoldItalicFont);
EpdFont notosans14RegularFont(&notosans_14_regular);
EpdFont notosans14BoldFont(&notosans_14_bold);
EpdFont notosans14ItalicFont(&notosans_14_italic);
EpdFont notosans14BoldItalicFont(&notosans_14_bolditalic);
EpdFontFamily notosans14FontFamily(&notosans14RegularFont, &notosans14BoldFont, &notosans14ItalicFont,
                                   &notosans14BoldItalicFont);
EpdFont notosans16RegularFont(&notosans_16_regular);
EpdFont notosans16BoldFont(&notosans_16_bold);
EpdFont notosans16ItalicFont(&notosans_16_italic);
EpdFont notosans16BoldItalicFont(&notosans_16_bolditalic);
EpdFontFamily notosans16FontFamily(&notosans16RegularFont, &notosans16BoldFont, &notosans16ItalicFont,
                                   &notosans16BoldItalicFont);
EpdFont notosans18RegularFont(&notosans_18_regular);
EpdFont notosans18BoldFont(&notosans_18_bold);
EpdFont notosans18ItalicFont(&notosans_18_italic);
EpdFont notosans18BoldItalicFont(&notosans_18_bolditalic);
EpdFontFamily notosans18FontFamily(&notosans18RegularFont, &notosans18BoldFont, &notosans18ItalicFont,
                                   &notosans18BoldItalicFont);

#endif  // OMIT_FONTS

// Inter's Medium and SemiBold weights, instanced from its variable axes and
// rasterized to compact 1-bit translation subsets. Medium keeps body labels open on
// the X4 panel; SemiBold supplies headings and selected/emphasized states. Hebrew
// and Arabic code points come from Noto fallbacks in the same subsets -- see
// scripts/build_ui_fonts.py.
EpdFont ui8MediumFont(&ui_8_medium);
EpdFont ui8SemiBoldFont(&ui_8_semibold);
EpdFontFamily ui8FontFamily(&ui8MediumFont, &ui8SemiBoldFont);

EpdFont ui10MediumFont(&ui_10_medium);
EpdFont ui10SemiBoldFont(&ui_10_semibold);
EpdFontFamily ui10FontFamily(&ui10MediumFont, &ui10SemiBoldFont);
// Handwritten accent face (Caveat 600): the Home author line. Single face —
// the script IS the emphasis, it has no bold/italic variants.
EpdFont uiScriptFont(&ui_script_20);
EpdFontFamily uiScriptFontFamily(&uiScriptFont);

EpdFont ui12MediumFont(&ui_12_medium);
EpdFont ui12SemiBoldFont(&ui_12_semibold);
EpdFontFamily ui12FontFamily(&ui12MediumFont, &ui12SemiBoldFont);

EpdFont ui14MediumFont(&ui_14_medium);
EpdFont ui14SemiBoldFont(&ui_14_semibold);
EpdFontFamily ui14FontFamily(&ui14MediumFont, &ui14SemiBoldFont);

EpdFont ui16MediumFont(&ui_16_medium);
EpdFont ui16SemiBoldFont(&ui_16_semibold);
EpdFontFamily ui16FontFamily(&ui16MediumFont, &ui16SemiBoldFont);

// Screen headings sit at the top of the scale.
EpdFontFamily uiHeaderFontFamily(&ui16SemiBoldFont);


// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
enum class BootResume : uint8_t {
  Splash,       // cold boot, flash, panic, or plain reboot
  Silent,       // heap-defrag ESP.restart() (RTC flag; lost on power loss)
  QuickResume,  // wake from a quick-resume deep sleep (SD flag; survives power loss)
};

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

void drawSystemFrameOverlay(const GfxRenderer& target) {
  UITheme::getInstance().drawSystemBatteryOverlay(target);
}

void applyInterfaceFont() {
  renderer.removeFont(MICRO_FONT_ID);
  renderer.removeFont(SMALL_FONT_ID);
  renderer.removeFont(UI_10_FONT_ID);
  renderer.removeFont(UI_12_FONT_ID);
  renderer.removeFont(UI_14_FONT_ID);
  renderer.removeFont(UI_16_FONT_ID);
  renderer.removeFont(UI_18_FONT_ID);
  if (renderer.getFontCacheManager()) renderer.getFontCacheManager()->clearCache();

  // The UI_nn identifiers are slot names, not pixel sizes. The whole scale sits one
  // step higher than the slot names suggest, so the interface reads comfortably at
  // arm's length on a 480 x 800 panel rather than merely fitting on it. Row heights,
  // the header, the legend bar and the footer counter are sized from these line
  // heights in LyraMetrics, so the two move together.
  //
  // MICRO exists for one job: the keyboard's secondary key labels. That grid is
  // structurally dense (10 columns of single characters) and does not benefit from
  // larger type, so it keeps the size the rest of the interface has outgrown.
  // The whole scale sits one step below the previous build (user request):
  // captions 12->10, labels 14->12, row titles 16->14, headings and book
  // titles 18->16. MICRO keeps 8. The 18 pt family is no longer linked.
  renderer.insertFont(MICRO_FONT_ID, ui8FontFamily);   // 8 px  — keyboard only
  renderer.insertFont(SMALL_FONT_ID, ui10FontFamily);  // 10 px — legends, captions
  renderer.insertFont(UI_10_FONT_ID, ui12FontFamily);  // 12 px — labels, values
  renderer.insertFont(UI_12_FONT_ID, ui14FontFamily);  // 14 px — list row titles
  renderer.insertFont(UI_14_FONT_ID, ui16FontFamily);  // 16 px — book titles
  renderer.insertFont(UI_16_FONT_ID, ui16FontFamily);  // 16 px
  renderer.insertFont(UI_18_FONT_ID, ui16FontFamily);  // 16 px
}

void silentRestart() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=home)");
  BootDiag::markCleanShutdown(BootDiag::Shutdown::Restart);
  // E-ink retains the previous frame until Home's first paint lands (~2-3s).
  // Without an overlay, users don't see the reboot and fire input through to
  // Home. Select on the default selectorIndex=0 then opens the most-recent
  // book, looking like a trampoline back to the reader they just exited.
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestartToReader() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=reader)");
  BootDiag::markCleanShutdown(BootDiag::Shutdown::Restart);
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void waitForPowerRelease() {
  gpio.update();
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
}

constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";

static void saveSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_FILE, file)) return;
  file.write(renderer.getFrameBuffer(), renderer.getBufferSize());
  file.close();
}

static bool loadSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  if (bytesRead != bufferSize) {
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }
  Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

// Enter deep sleep mode
void enterDeepSleep(bool fromTimeout = false) {
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();

  const bool isQuickResumeSleep =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);
  APP_STATE.showBootScreen = !isQuickResumeSleep;

  APP_STATE.saveToFile();

  // Commit to sleeping before goToSleep() runs the outgoing activity's onExit():
  // a WiFi activity would otherwise silentRestart() here and reboot instead.
  deepSleepInProgress = true;
  activityManager.goToSleep(fromTimeout);

  // X4 loses its internal clock when the battery latch removes power. Keep the
  // best known epoch on SD so the next boot still has a useful visual fallback.
  halClock.saveCurrentTime();

  if (isQuickResumeSleep) {
    saveSleepFrameBuffer();
  }

  // Tear down WiFi so the modem power domain isn't held alive across deep sleep.
  // Wake from deep sleep is effectively a chip reset, so no state needs to survive.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  halTiltSensor.deepSleep();
  display.deepSleep();
  BootDiag::markCleanShutdown(fromTimeout ? BootDiag::Shutdown::IdleTimeout : BootDiag::Shutdown::PowerButton);
  LOG_DBG("MAIN", "Entering deep sleep");

  powerManager.startDeepSleep(gpio);
}

void setupDisplayAndFonts(bool seamless = false) {
  display.begin(seamless);
  renderer.begin();
  renderer.setFrameOverlayHook(drawSystemFrameOverlay);
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(NOTOSERIF_14_FONT_ID, notoserif14FontFamily);
#ifndef OMIT_FONTS
  renderer.insertFont(NOTOSERIF_12_FONT_ID, notoserif12FontFamily);
  renderer.insertFont(NOTOSERIF_16_FONT_ID, notoserif16FontFamily);
  renderer.insertFont(NOTOSERIF_18_FONT_ID, notoserif18FontFamily);

  renderer.insertFont(NOTOSANS_12_FONT_ID, notosans12FontFamily);
  renderer.insertFont(NOTOSANS_14_FONT_ID, notosans14FontFamily);
  renderer.insertFont(NOTOSANS_16_FONT_ID, notosans16FontFamily);
  renderer.insertFont(NOTOSANS_18_FONT_ID, notosans18FontFamily);
#endif  // OMIT_FONTS
  applyInterfaceFont();
  renderer.insertFont(HEADER_FONT_ID, uiHeaderFontFamily);  // 16 px semibold
  renderer.insertFont(SCRIPT_FONT_ID, uiScriptFontFamily);  // 20 px handwritten — Home author line

  // Discover and load SD card fonts
  sdFontSystem.begin(renderer);

  LOG_DBG("MAIN", "Fonts setup");
}

void setup() {
#ifdef ENABLE_SERIAL_LOG
  // Earliest possible Serial setup. The 250 ms stall before begin() lets the
  // USB Serial/JTAG peripheral finish power-on and lets the host complete USB
  // enumeration before we touch the CDC state — otherwise cold boot races
  // and the host has to be physically replugged for logs to flow. Warm reboot
  // worked without the delay because USB was already enumerated.
  delay(250);
  Serial.begin(115200);
  logSerial.setTxTimeoutMs(1);  // This is a load-bearing 1. Do not modify.
#endif

  HalSystem::begin();

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Bound the target range too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const uint32_t snapshotTarget =
      (isSilentReboot && silentRebootTarget <= SILENT_REBOOT_TARGET_READER) ? silentRebootTarget : 0;
  silentRebootMagic = 0;
  silentRebootTarget = 0;

  gpio.begin();
  powerManager.begin();
  halTiltSensor.begin();
  halClock.begin();

  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts(isSilentReboot);
    activityManager.goToFullScreenMessage(tr(STR_SD_CARD_ERROR), EpdFontFamily::BOLD);
    return;
  }

  HalSystem::checkPanic();

  // Before anything can reroute the boot (a wake that goes straight back to
  // sleep, a silent reboot): report how the previous session ended.
  BootDiag::begin();

  SETTINGS.loadFromFile();
  halClock.restoreFromStorage();
  APP_STATE.loadFromFile();
  RECENT_BOOKS.loadFromFile();
  FAVORITE_BOOKS.loadFromFile();
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  KOREADER_STORE.loadFromFile();
  OPDS_STORE.loadFromFile();
  // Load here, not lazily in the Wi-Fi picker: the store persists its whole
  // in-memory vector, so any path that saves before a load (the web server's
  // Wi-Fi API in hotspot mode) would rewrite wifi.json from an empty list and
  // discard every saved network.
  WIFI_STORE.loadFromFile();
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);

  const auto wakeupReason = gpio.getWakeupReason();
  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      LOG_DBG("MAIN", "Verifying power button press duration");
      gpio.verifyPowerButtonWakeup(SETTINGS.getPowerButtonDuration(),
                                   SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // If USB power caused a cold boot, go back to sleep
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
      powerManager.startDeepSleep(gpio);
      break;
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  // Recovery firmware mode: hold left side button (BTN_UP) together with the power button at
  // boot to skip directly to the SD-card firmware update screen. Useful on devices where USB
  // flashing has been locked down (e.g. recent X3 firmware).
  bool recoveryFirmwareMode = false;
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton) {
    // Refresh the cached button state a few times — isPressed() needs ~half a second to settle
    // after boot per the HalGPIO contract. Use a millis-based deadline so we always wait the full
    // settle window even if the loop body takes longer than expected on slow boots.
    const unsigned long settleStart = millis();
    while (millis() - settleStart < 500) {
      gpio.update();
      delay(10);
    }
    if (gpio.isPressed(HalGPIO::BTN_UP)) {
      recoveryFirmwareMode = true;
      LOG_INF("MAIN", "Recovery firmware mode (UP + POWER held at boot)");
    }
  }

  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  LOG_DBG("MAIN", "Starting InkPoint X version " CROSSPOINT_VERSION);

  // Resolve the single boot-presentation decision. Seamless paths skip the
  // splash but still run one controller-safe fast-full update: begin() resets
  // controller RAM while the physical e-ink panel retains its old frame.
  const BootResume resume = isSilentReboot              ? BootResume::Silent
                            : !APP_STATE.showBootScreen ? BootResume::QuickResume
                                                        : BootResume::Splash;

  setupDisplayAndFonts(resume != BootResume::Splash);

  switch (resume) {
    case BootResume::Silent:
      // Splash skipped: the routing block below picks the target activity; the
      // panel keeps showing the pre-reboot popup until that first paint lands.
      break;
    case BootResume::QuickResume:
      // One-shot flag: re-arm the splash for the next non-quick-resume boot. Save
      // before any painting so a hang in the blocking paint path can't strand
      // us in a quick-resume-with-no-frame loop on the next boot.
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
      if (loadSleepFrameBuffer()) {
        // Frame restored: swap the sleep moon for the loading icon.
        const auto pageHeight = renderer.getScreenHeight();
        renderer.drawImage(LoadingIcon, 0, pageHeight - LOADINGICON_HEIGHT, LOADINGICON_WIDTH, LOADINGICON_HEIGHT);
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      } else {
        activityManager.goToBoot();  // frame file missing, fall back to the splash
      }
      break;
    case BootResume::Splash:
      activityManager.goToBoot();
      break;
  }

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (HalSystem::isRebootFromPanic()) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome();
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    activityManager.goHome();
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    activityManager.goToReader(path);
  }

  if (resume == BootResume::Silent) {
    // Block until the first paint physically completes. refreshDisplay()
    // waits on the panel BUSY pin so when this returns the user can see the
    // new activity. Without the wait, an edge captured by gpio.update()
    // during boot dispatches against an invisible Home and the default
    // selectorIndex=0 opens the most-recent book.
    activityManager.requestUpdateAndWait();
    // Absorb any button held at this point into currentState as a non-edge:
    // two gpio.update() calls separated by > InputManager's 20ms debounce
    // transition the held bit through lastDebounceTime into currentState
    // without setting pressedEvents, so the first loop()'s own gpio.update()
    // sees state == currentState and emits nothing.
    gpio.update();
    delay(25);
    gpio.update();
  }

  // Watchdog: the config arms the TWDT but nothing ever subscribed, so the 25
  // esp_task_wdt_reset() calls scattered through the network code were no-ops
  // and an application-level hang required the user to force-power the device.
  // The render task is deliberately not subscribed - it parks forever in
  // xTaskNotifyWait when idle, and a wedged render task starves the main loop
  // off the RenderLock anyway, which this watchdog then catches on the next
  // lock acquisition. 300 s, not less: a button press during a long chapter
  // index legitimately blocks the main loop on the RenderLock for however
  // long the index takes, and that must not read as a hang.
  {
    esp_task_wdt_config_t wdtConfig = {};
    wdtConfig.timeout_ms = 300000;
    wdtConfig.idle_core_mask = 0;
    wdtConfig.trigger_panic = true;
    esp_task_wdt_reconfigure(&wdtConfig);
    enableLoopWDT();
  }

  // Ensure we're not still holding the power button before leaving setup
  waitForPowerRelease();
  allowSleepAt = millis() + 2000;
}

// Arduino's default marks a pending OTA image valid before setup() even runs,
// which makes bootloader rollback useless: a firmware that boots and panics
// immediately can never roll back. Returning true here defers the decision to
// markOtaValidOnceHealthy() below.
extern "C" bool verifyRollbackLater() { return true; }

namespace {
// 10 s of uptime without a panic is the health criterion: every crash loop we
// have seen fires well inside that. Until this runs, a reset returns the
// device to the previous slot.
void markOtaValidOnceHealthy() {
  static bool done = false;
  if (done || millis() < 10000) return;
  done = true;
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
    LOG_INF("OTA", "Image marked valid after healthy boot");
  }
}
}  // namespace

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.update();
  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.orientation, activityManager.isReaderActivity());
  markOtaValidOnceHealthy();

  renderer.setFadingFix(SETTINGS.fadingFix);

  if (Serial && millis() - lastMemPrint >= 10000) {
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes, MaxAlloc: %d bytes", ESP.getFreeHeap(),
            ESP.getHeapSize(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
    lastMemPrint = millis();
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      if (cmd == "SCREENSHOT") {
        // Snapshot the framebuffer under the same lock used by the render task.
        // Without this, an activity redraw can replace the buffer while the
        // relatively slow USB transfer is in progress, producing a torn image
        // containing parts of two different screens.
        RenderLock renderLock;
        const uint32_t bufferSize = display.getBufferSize();
        logSerial.setTxTimeoutMs(1000);
        logSerial.printf("SCREENSHOT_START:%u\n", (unsigned)bufferSize);
        uint8_t* buf = display.getFrameBuffer();
        uint32_t bytesSent = 0;
        while (bytesSent < bufferSize) {
          const size_t chunkSize = std::min<uint32_t>(64, bufferSize - bytesSent);
          const size_t written = logSerial.write(buf + bytesSent, chunkSize);
          if (written == 0) {
            delay(1);
            continue;
          }
          bytesSent += written;
          delay(1);
        }
        logSerial.flush();
        logSerial.printf("SCREENSHOT_END\n");
        logSerial.flush();
        logSerial.setTxTimeoutMs(1);
#if LOG_LEVEL >= 2
      } else if (cmd == "PROFILE_SLEEP") {
        // Drives the power button's sleep path end to end without a finger on
        // the button: the teardown, the sleep frame, the panel shutdown and
        // esp_deep_sleep_start(). A panic anywhere in there would look exactly
        // like "the power button rebooted the device instead of sleeping it".
        LOG_INF("MAIN", "Profile route: deep sleep (power-button path)");
        enterDeepSleep(false);
      } else if (cmd == "PROFILE_OTA") {
        // Verification route for the over-the-air update path: reaching it
        // through the UI needs several button presses this console cannot make.
        activityManager.replaceActivity(std::make_unique<OtaUpdateActivity>(renderer, mappedInputManager));
        LOG_DBG("MAIN", "Profile route: OTA update");
      } else if (cmd == "PROFILE_REINDEX") {
        // Wipes the most recent book's cache and reopens it — forces the full
        // chapter re-index path, which is where the light-sleep RenderLock
        // regression wedged the main loop.
        if (!RECENT_BOOKS.getBooks().empty()) {
          const auto path = RECENT_BOOKS.getBooks().front().path;
          // Force the position to a mid-book chapter after the wipe: reopening
          // at the one-page cover chapter indexes in seconds and never
          // exercises the long re-index path this route exists for.
          const std::string cacheDir = getBookCachePath(path);
          clearBookCache(path);
          Storage.ensureDirectoryExists(cacheDir.c_str());
          const uint8_t forced[6] = {24, 0, 1, 0, 0, 0};  // spine=24, page=1
          ProgressFile::writeAtomic(cacheDir, forced, sizeof(forced));
          activityManager.goToReader(path);
          LOG_DBG("MAIN", "Profile route: reindex %s", path.c_str());
        }
      } else if (cmd == "PROFILE_READER") {
        // Opens the most recent book — the only way to reach a reading page
        // from the console for framebuffer verification.
        if (!RECENT_BOOKS.getBooks().empty()) {
          activityManager.goToReader(RECENT_BOOKS.getBooks().front().path);
          LOG_DBG("MAIN", "Profile route: Reader");
        } else {
          LOG_DBG("MAIN", "Profile route: Reader - no recent books");
        }
      } else if (cmd == "PROFILE_REDRAW") {
        // Development-only latency probe. It exercises the exact active
        // activity render path without changing UI state.
        activityManager.requestUpdate();
        LOG_DBG("MAIN", "Profile redraw requested");
      } else if (cmd == "PROFILE_LIBRARY") {
        activityManager.goHome(HomeMenuItem::LIBRARY);
        LOG_DBG("MAIN", "Profile route: Library");
      } else if (cmd == "PROFILE_BOOKS") {
        activityManager.goToLibrary();
        LOG_DBG("MAIN", "Profile route: Books");
      } else if (cmd == "PROFILE_FILES") {
        activityManager.goToFileBrowser();
        LOG_DBG("MAIN", "Profile route: Files");
      } else if (cmd == "PROFILE_GALLERY") {
        activityManager.goToGallery();
        LOG_DBG("MAIN", "Profile route: Gallery");
      } else if (cmd == "PROFILE_HOME_SETTINGS") {
        activityManager.goHome(HomeMenuItem::SETTINGS_MENU);
        LOG_DBG("MAIN", "Profile route: Settings hub");
      } else if (cmd == "PROFILE_HOME") {
        activityManager.goHome();
        LOG_DBG("MAIN", "Profile route: Home");
      } else if (cmd == "PROFILE_SETTINGS") {
        activityManager.goToSettings();
        LOG_DBG("MAIN", "Profile route: Settings");
#endif
      }
    }
  }

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || halTiltSensor.hadActivity() ||
      activityManager.preventAutoSleep()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  static bool screenshotButtonsReleased = true;
  static bool screenshotComboActive = false;
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    screenshotComboActive = true;
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      {
        RenderLock lock;
        ScreenshotUtil::takeScreenshot(renderer);
      }
    }
    return;
  }
  if (screenshotComboActive) {
    if (gpio.isPressed(HalGPIO::BTN_POWER)) return;
    if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
      screenshotButtonsReleased = true;
      screenshotComboActive = false;
      return;
    }
    screenshotButtonsReleased = true;
    screenshotComboActive = false;
  }

  // Removed in 2.0.2: a critical-battery guard that force-slept the device
  // below 2%. Its premise was sound (a brownout mid-write is how settings
  // files used to vanish) but its input is not: on the X4 the reading is an
  // ADC divider smoothed in software, and it sags under a panel refresh, at
  // 10 MHz in power-saving mode, and after a sleep wake. Acting on it powers
  // a working device off, which on e-ink is indistinguishable from a freeze —
  // the panel keeps the last frame and the next press looks like a reboot.
  // A guard that turns the device off must be at least as trustworthy as the
  // failure it prevents. This one wasn't, and the failure is rarer.

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (sleepTimeoutMs > 0 && millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  if (millis() >= allowSleepAt && gpio.isPressed(HalGPIO::BTN_POWER) &&
      gpio.getPowerButtonHeldTime() > SETTINGS.getPowerButtonDuration()) {
    // If the screenshot combination is potentially being pressed, don't sleep
    if (gpio.isPressed(HalGPIO::BTN_DOWN)) {
      return;
    }
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // Refresh screen when power button is short-pressed with FORCE_REFRESH setting.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH &&
      mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    LOG_DBG("MAIN", "Manual screen refresh triggered");
    RenderLock lock;
    // The X4's single-pass D7 clean removes accumulated differential residue
    // without the conspicuous multi-phase black flash of FULL (F7).
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  if (gpio.wasUsbStateChanged()) {
    activityManager.requestUpdate();
  }

  // A button can still be physically held while the action-triggered frame is
  // painted, so its on-screen section is rendered black. The release edge does
  // not always change activity state and therefore used to leave that frame
  // latched on e-ink indefinitely. Queue one visual-only redraw after release.
  // It is deliberately deferred and coalesced with the activity's own update:
  // no extra gpio.update(), no replayed input event, and no duplicate action.
  const bool frontButtonReleased =
      gpio.wasReleased(HalGPIO::BTN_BACK) || gpio.wasReleased(HalGPIO::BTN_CONFIRM) ||
      gpio.wasReleased(HalGPIO::BTN_LEFT) || gpio.wasReleased(HalGPIO::BTN_RIGHT);
  // Only when the last rendered frame actually shows a pressed pill: the
  // unconditional version queued a second full-panel refresh on every list
  // step (action paints on the press edge, this fired on the release edge),
  // doubling both the visible flashing and the panel energy per keypress.
  if (frontButtonReleased && SETTINGS.showButtonHints && UITheme::getInstance().hasVisibleButtonHints() &&
      UITheme::getInstance().hasPressedButtonHints()) {
    activityManager.requestUpdate();
  }

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (activityManager.skipLoopDelay()) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      // Removed in 2.0.2: timer-wakeup esp_light_sleep_start() in place of this
      // delay. It saved real idle current, and it cost the firmware its
      // reliability on battery — the only configuration it ran in, and the one
      // configuration a USB-tethered bench cannot observe. Halting the clocks
      // this deep touches the ADC ladder every button rides on, the panel's
      // SPI state and the power rails, and each round of hardening produced a
      // new field failure mode instead of a quiet device. A plain delay is
      // 50 ms of WFI at 10 MHz: unglamorous, and correct on hardware I cannot
      // instrument. It can come back the day it can be measured on battery
      // with a current probe rather than reasoned about.
      delay(50);
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
