#include "CalendarActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cctype>
#include <ctime>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/settings/ClockSyncActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/HoldGestures.h"

namespace {
// Every d-pad button in the Grid state is already spoken for (day/month
// navigation, note, back), so Holidays is reached by holding the button that
// already means "act on this day" rather than a dedicated key.
constexpr unsigned long HOLIDAYS_HOLD_MS = HoldGestures::SHORT_MS;

// Month names are stored lowercase (correct Spanish grammar for mid-sentence
// use, e.g. OnThisDayActivity's "14 de agosto de 2026"), but every place this
// screen shows one it's a standalone label, not mid-sentence, so it reads
// better title-cased.
std::string capitalizeFirst(const std::string& s) {
  std::string out = s;
  if (!out.empty()) out[0] = static_cast<char>(toupper(static_cast<unsigned char>(out[0])));
  return out;
}

constexpr StrId kMonthKeys[12] = {
    StrId::STR_MONTH_JANUARY,   StrId::STR_MONTH_FEBRUARY, StrId::STR_MONTH_MARCH,    StrId::STR_MONTH_APRIL,
    StrId::STR_MONTH_MAY,       StrId::STR_MONTH_JUNE,     StrId::STR_MONTH_JULY,     StrId::STR_MONTH_AUGUST,
    StrId::STR_MONTH_SEPTEMBER, StrId::STR_MONTH_OCTOBER,  StrId::STR_MONTH_NOVEMBER, StrId::STR_MONTH_DECEMBER};

constexpr StrId kWeekdayKeys[7] = {StrId::STR_WEEKDAY_MON, StrId::STR_WEEKDAY_TUE, StrId::STR_WEEKDAY_WED,
                                    StrId::STR_WEEKDAY_THU, StrId::STR_WEEKDAY_FRI, StrId::STR_WEEKDAY_SAT,
                                    StrId::STR_WEEKDAY_SUN};
}  // namespace

int CalendarActivity::daysInMonth(int year, int month) {
  static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2) {
    const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    return leap ? 29 : 28;
  }
  return days[month - 1];
}

// Sakamoto's algorithm: day of week for a given date, 0=Sunday..6=Saturday.
int CalendarActivity::firstWeekdayOfMonth(int year, int month) {
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  int y = year;
  if (month < 3) y -= 1;
  const int day = 1;
  const int wday = (y + y / 4 - y / 100 + y / 400 + t[month - 1] + day) % 7;
  return (wday + 6) % 7;  // remap 0=Sunday..6=Saturday to 0=Monday..6=Sunday
}

void CalendarActivity::getToday(int& year, int& month, int& day) const {
  struct tm t {};
  if (halClock.hasValidTime() && halClock.getDateTime(t, SETTINGS.clockUtcOffsetQ)) {
    year = t.tm_year + 1900;
    month = t.tm_mon + 1;
    day = t.tm_mday;
  } else {
    year = 1970;
    month = 1;
    day = 1;
  }
}

std::string CalendarActivity::notesPath(int year, int month) const {
  char buf[40];
  snprintf(buf, sizeof(buf), "/apps/calendar/%04d-%02d.txt", year, month);
  return buf;
}

void CalendarActivity::loadNotes() {
  notes.clear();
  String content = Storage.readFile(notesPath(viewYear, viewMonth).c_str());
  std::string data(content.c_str());

  size_t pos = 0;
  while (pos < data.length()) {
    size_t nl = data.find('\n', pos);
    std::string line = (nl == std::string::npos) ? data.substr(pos) : data.substr(pos, nl - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t sep = line.find('|');
    if (!line.empty() && sep != std::string::npos) {
      const int day = atoi(line.substr(0, sep).c_str());
      if (day >= 1 && day <= 31) {
        notes.push_back(CalendarNote{day, line.substr(sep + 1)});
      }
    }
    if (nl == std::string::npos) break;
    pos = nl + 1;
  }
}

void CalendarActivity::saveNotes() {
  String content;
  for (const auto& note : notes) {
    content += String(note.day) + "|" + String(note.text.c_str()) + "\n";
  }
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/calendar");
  Storage.writeFile(notesPath(viewYear, viewMonth).c_str(), content);
}

std::string CalendarActivity::getNote(int day) const {
  for (const auto& note : notes) {
    if (note.day == day) return note.text;
  }
  return "";
}

void CalendarActivity::setNote(int day, const std::string& text) {
  for (auto it = notes.begin(); it != notes.end(); ++it) {
    if (it->day == day) {
      if (text.empty()) {
        notes.erase(it);
      } else {
        it->text = text;
      }
      return;
    }
  }
  if (!text.empty()) {
    notes.push_back(CalendarNote{day, text});
  }
}

std::string CalendarActivity::holidaysCachePath(int year) const {
  char buf[40];
  snprintf(buf, sizeof(buf), "/apps/calendar/feriados_%04d.json", year);
  return buf;
}

std::string CalendarActivity::holidaysTmpPath(int year) const { return holidaysCachePath(year) + ".tmp"; }

std::string CalendarActivity::holidaysApiUrl(int year) const {
  return "https://api.argentinadatos.com/v1/feriados/" + std::to_string(year);
}

bool CalendarActivity::loadHolidaysFromSd(int year) {
  HalFile file;
  if (!Storage.openFileForRead("CALENDAR", holidaysCachePath(year).c_str(), file)) {
    return false;
  }
  parseAndStoreHolidays(file);
  holidaysYear = year;
  return holidaysLoaded;
}

void CalendarActivity::parseAndStoreHolidays(HalFile& file) {
  struct HalFileJsonReader {
    HalFile& f;
    int read() { return f.read(); }
    size_t readBytes(char* buffer, size_t length) {
      const int n = f.read(buffer, length);
      return n < 0 ? 0 : static_cast<size_t>(n);
    }
  } reader{file};

  JsonDocument filter;
  filter[0]["fecha"] = true;
  filter[0]["nombre"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, reader, DeserializationOption::Filter(filter));
  if (err) {
    LOG_ERR("CALENDAR", "Holidays JSON parse failed: %s", err.c_str());
    holidaysError = tr(STR_CALENDAR_HOLIDAYS_NO_DATA);
    return;
  }

  std::vector<Holiday> parsed;
  JsonArray items = doc.as<JsonArray>();
  parsed.reserve(items.size());
  for (JsonObject item : items) {
    std::string fecha = item["fecha"] | "";  // "YYYY-MM-DD"
    std::string nombre = item["nombre"] | "";
    if (fecha.length() != 10 || nombre.empty()) continue;
    Holiday h;
    h.month = atoi(fecha.substr(5, 2).c_str());
    h.day = atoi(fecha.substr(8, 2).c_str());
    h.name = std::move(nombre);
    parsed.push_back(std::move(h));
  }

  holidays = std::move(parsed);
  holidaysLoaded = !holidays.empty();
  if (holidaysLoaded) {
    holidaysError.clear();
  } else {
    holidaysError = tr(STR_CALENDAR_HOLIDAYS_NO_DATA);
  }
}

// promptWifi=false is used for the silent "cache is empty but WiFi already
// happens to be connected" auto-load; promptWifi=true is used only by the
// Holiday List screen's explicit Refresh row.
void CalendarActivity::startHolidaysFetch(int year, bool promptWifi) {
  fetchingYear = year;
  holidaysRefreshing = true;
  holidaysRefreshFailed = false;
  Storage.ensureDirectoryExists("/apps");
  Storage.ensureDirectoryExists("/apps/calendar");
  requestUpdate();

  if (WiFi.status() == WL_CONNECTED) {
    doHolidaysFetch(year);
    return;
  }

  if (!promptWifi) {
    holidaysRefreshing = false;
    return;
  }

  WiFi.mode(WIFI_STA);
  startActivityForResult(makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput),
                          [this, year](const ActivityResult& result) {
                            if (result.isCancelled) {
                              holidaysRefreshing = false;
                              loadHolidaysFromSd(year);
                              if (!holidaysLoaded) holidaysError = tr(STR_CALENDAR_HOLIDAYS_NO_DATA);
                              requestUpdate();
                            } else {
                              doHolidaysFetch(year);
                            }
                          });
}

void CalendarActivity::doHolidaysFetch(int year) {
  requestUpdateAndWait();  // paint the "Loading..." state before the blocking call below
  wifiWasUsed = true;

  const auto result = HttpDownloader::downloadToFile(holidaysApiUrl(year), holidaysTmpPath(year));
  holidaysRefreshing = false;

  if (result == HttpDownloader::OK) {
    Storage.remove(holidaysCachePath(year).c_str());
    Storage.rename(holidaysTmpPath(year).c_str(), holidaysCachePath(year).c_str());
  }

  loadHolidaysFromSd(year);
  if (!holidaysLoaded && holidaysError.empty()) {
    holidaysError = tr(STR_CALENDAR_HOLIDAYS_NO_DATA);
  } else if (holidaysLoaded && result != HttpDownloader::OK) {
    holidaysRefreshFailed = true;
  }
  requestUpdate();
}

// Called whenever the viewed year might have changed (onEnter, goToMonth).
// Loads from SD cache if present; otherwise, per the conservative fetch
// policy, only auto-fetches silently when WiFi happens to already be
// connected -- never force-prompts, so the core grid stays fully offline-first.
void CalendarActivity::ensureHolidaysForYear(int year) {
  if (year == holidaysYear && (holidaysLoaded || !holidaysError.empty())) return;
  holidays.clear();
  holidaysLoaded = false;
  holidaysError.clear();
  holidaysYear = year;
  if (!loadHolidaysFromSd(year)) {
    startHolidaysFetch(year, /*promptWifi=*/false);
  }
}

bool CalendarActivity::isHoliday(int month, int day, std::string* outName) const {
  for (const auto& h : holidays) {
    if (h.month == month && h.day == day) {
      if (outName) *outName = h.name;
      return true;
    }
  }
  return false;
}

// Indices into `holidays`, ordered by (month, day), without mutating storage
// order. Shared by the Holiday List's row layout and by nextHolidayRow() so
// the two can't drift apart.
std::vector<int> CalendarActivity::sortedHolidayOrder() const {
  std::vector<int> order(holidays.size());
  for (size_t i = 0; i < order.size(); i++) order[i] = static_cast<int>(i);
  std::sort(order.begin(), order.end(), [this](int a, int b) {
    const auto& ha = holidays[a];
    const auto& hb = holidays[b];
    return ha.month != hb.month ? ha.month < hb.month : ha.day < hb.day;
  });
  return order;
}

// Row to land on when opening the Holiday List, so it opens already scrolled
// to the next upcoming holiday instead of always starting at the Refresh row
// -- that's the one thing this screen exists to answer, and the day grid
// only ever holds `holidays` for the year currently being browsed (viewYear).
// Falls back to the first holiday in the list (row 1, right after Refresh)
// when browsing a year other than today's, or once every holiday still in
// `viewYear` has already passed.
int CalendarActivity::nextHolidayRow() const {
  if (holidays.empty()) return 0;
  if (viewYear == todayYear) {
    const auto order = sortedHolidayOrder();
    for (size_t i = 0; i < order.size(); i++) {
      const auto& h = holidays[order[i]];
      if (h.month > todayMonth || (h.month == todayMonth && h.day >= todayDay)) {
        return static_cast<int>(i) + 1;  // +1: row 0 is the synthetic Refresh row
      }
    }
  }
  return 1;
}

void CalendarActivity::goToMonth(int year, int month) {
  viewYear = year;
  viewMonth = month;
  const int total = daysInMonth(viewYear, viewMonth);
  if (viewYear == todayYear && viewMonth == todayMonth) {
    selectedDay = todayDay;
  } else {
    selectedDay = std::min(selectedDay, total);
  }
  loadNotes();
  ensureHolidaysForYear(viewYear);
  requestUpdate();
}

// Combined holiday name + personal note for `day`, as shown in the note
// view popup. Empty when the day has neither.
std::string CalendarActivity::dayPreviewText(int day) const {
  std::string holidayName;
  const bool dayIsHoliday = isHoliday(viewMonth, day, &holidayName);
  const std::string note = getNote(day);

  if (dayIsHoliday && !note.empty()) return holidayName + " - " + note;
  if (dayIsHoliday) return holidayName;
  return note;
}

void CalendarActivity::openNoteEditor() {
  char titleBuf[48];
  const std::string monthName = capitalizeFirst(I18N.get(kMonthKeys[viewMonth - 1]));
  snprintf(titleBuf, sizeof(titleBuf), tr(STR_CALENDAR_NOTE_TITLE_FORMAT), monthName.c_str(), selectedDay, viewYear);
  const std::string existing = getNote(selectedDay);
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, titleBuf, existing, 200);
  const int day = selectedDay;
  startActivityForResult(std::move(keyboard), [this, day](const ActivityResult& result) {
    if (!result.isCancelled) {
      auto keyboardResult = std::get_if<KeyboardResult>(&result.data);
      if (keyboardResult) {
        setNote(day, keyboardResult->text);
        saveNotes();
      }
    }
    requestUpdate();
  });
}

void CalendarActivity::onEnter() {
  Activity::onEnter();

  // Fresh boot with no clock sync yet has no valid time. Only fix this
  // silently (no connect prompt - this app should still work fully offline)
  // when WiFi is already associated; otherwise just flag it.
  if (!halClock.hasValidTime() && WiFi.status() == WL_CONNECTED) {
    halClock.syncFromNTP();
  }
  getToday(todayYear, todayMonth, todayDay);
  dateUnconfirmed = !halClock.hasValidTime();

  viewYear = todayYear;
  viewMonth = todayMonth;
  selectedDay = todayDay;
  state = CalendarState::Grid;
  loadNotes();
  ensureHolidaysForYear(viewYear);
  requestUpdate();
}

void CalendarActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  if (wifiWasUsed) {
    silentRestart();
  }
}

void CalendarActivity::loop() {
  using Button = MappedInputManager::Button;

  // After a hold-to-open-Holidays fires, swallow input globally (regardless of
  // which state that transition landed on, i.e. Grid -> HolidayList) until
  // Confirm is physically released. Otherwise the same release that ends the
  // hold would immediately fire whatever Confirm does in the new state (e.g.
  // HolidayList's Refresh row). Re-arms once the button is up.
  if (holidayHoldFired) {
    if (!mappedInput.isPressed(Button::Confirm)) {
      holidayHoldFired = false;
    }
    return;
  }

  if (state == CalendarState::NoteView) {
    if (mappedInput.wasReleased(Button::Back)) {
      state = CalendarState::Grid;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Confirm)) {
      state = CalendarState::Grid;
      openNoteEditor();
    }
    return;
  }

  if (state == CalendarState::HolidayList) {
    const int totalRows = 1 + static_cast<int>(holidays.size());  // row 0 = synthetic "Refresh"
    if (mappedInput.wasReleased(Button::Back)) {
      state = CalendarState::Grid;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Up)) {
      holidayListSelectedRow = (holidayListSelectedRow - 1 + totalRows) % totalRows;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Down)) {
      holidayListSelectedRow = (holidayListSelectedRow + 1) % totalRows;
      requestUpdate();
    } else if (mappedInput.wasReleased(Button::Confirm)) {
      if (holidayListSelectedRow == 0) {
        startHolidaysFetch(viewYear, /*promptWifi=*/true);
      }
    }
    return;
  }

  if (mappedInput.isPressed(Button::Confirm) && mappedInput.getHeldTime() >= HOLIDAYS_HOLD_MS) {
    holidayHoldFired = true;
    holidayListSelectedRow = nextHolidayRow();
    state = CalendarState::HolidayList;
    requestUpdate();
    return;
  }

  // Long-press Back: force an NTP resync via ClockSyncActivity, same gesture
  // and entry point as ClockActivity::syncClock(). Not gated on
  // dateUnconfirmed -- a present-but-wrong time (RTC drift, bad initial
  // sync) needs this just as much as a missing one, and hasValidTime() can't
  // tell the two apart.
  if (mappedInput.isPressed(Button::Back) && mappedInput.getHeldTime() >= HoldGestures::LONG_MS) {
    startActivityForResult(makeUniqueNoThrow<ClockSyncActivity>(renderer, mappedInput),
                            [this](const ActivityResult&) {
                              getToday(todayYear, todayMonth, todayDay);
                              dateUnconfirmed = !halClock.hasValidTime();
                              if (viewYear == todayYear && viewMonth == todayMonth) selectedDay = todayDay;
                              requestUpdate();
                            });
    return;
  }

  if (mappedInput.wasReleased(Button::Back)) {
    onGoHome(HomeMenuItem::TOOLS_MENU);
    return;
  }

  const int total = daysInMonth(viewYear, viewMonth);

  if (mappedInput.wasReleased(Button::Up)) {
    selectedDay = selectedDay > 1 ? selectedDay - 1 : total;
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Down)) {
    selectedDay = selectedDay < total ? selectedDay + 1 : 1;
    requestUpdate();
  } else if (mappedInput.wasReleased(Button::Left)) {
    int y = viewYear;
    int m = viewMonth - 1;
    if (m < 1) {
      m = 12;
      y--;
    }
    goToMonth(y, m);
  } else if (mappedInput.wasReleased(Button::Right)) {
    int y = viewYear;
    int m = viewMonth + 1;
    if (m > 12) {
      m = 1;
      y++;
    }
    goToMonth(y, m);
  } else if (mappedInput.wasReleased(Button::Confirm)) {
    if (!dayPreviewText(selectedDay).empty()) {
      state = CalendarState::NoteView;
      requestUpdate();
    } else {
      openNoteEditor();
    }
  }
}

void CalendarActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (state == CalendarState::HolidayList) {
    GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   tr(STR_CALENDAR_HOLIDAYS_TITLE));

    const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int listBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;

    if (holidaysRefreshing) {
      const int textY = listTop + (listBottom - listTop) / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
      renderer.drawCenteredText(UI_12_FONT_ID, textY, tr(STR_CALENDAR_HOLIDAYS_LOADING));
    } else {
      const int totalRows = 1 + static_cast<int>(holidays.size());
      const std::vector<int> order = sortedHolidayOrder();

      GUI.drawList(
          renderer, Rect{0, listTop, pageWidth, listBottom - listTop}, totalRows, holidayListSelectedRow,
          [this, &order](int i) -> std::string {
            if (i == 0) return tr(STR_CALENDAR_REFRESH);
            return holidays[order[i - 1]].name;
          },
          [this, &order](int i) -> std::string {
            if (i == 0) return "";
            const auto& h = holidays[order[i - 1]];
            char buf[24];
            snprintf(buf, sizeof(buf), "%d %s", h.day, capitalizeFirst(I18N.get(kMonthKeys[h.month - 1])).c_str());
            return buf;
          },
          nullptr, nullptr, false);

      if (holidaysRefreshFailed) {
        GUI.drawPopup(renderer, tr(STR_CALENDAR_HOLIDAYS_REFRESH_FAILED));
      }
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  GUI.drawScriptHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CALENDAR_TITLE));

  char subBuf[32];
  snprintf(subBuf, sizeof(subBuf), "%s %d", capitalizeFirst(I18N.get(kMonthKeys[viewMonth - 1])).c_str(), viewYear);
  const int subHeaderY = metrics.topPadding + metrics.headerHeight;
  GUI.drawSubHeader(renderer, Rect{0, subHeaderY, pageWidth, metrics.subHeaderHeight}, subBuf, nullptr);

  // Persistent "Feriados" chip, top-right of the subheader row. Always visible
  // -- unlike the old last-day-of-month row it replaces, which only appeared
  // once you navigated all the way past every day to reach it -- and opened
  // via a Confirm hold since every d-pad button here is already spoken for.
  {
    const char* holidaysLabel = tr(STR_CALENDAR_HOLIDAYS_TITLE);
    constexpr int CHIP_PAD_X = 8;
    constexpr int CHIP_H = 24;
    const int labelW = renderer.getTextWidth(SMALL_FONT_ID, holidaysLabel);
    const int chipW = labelW + CHIP_PAD_X * 2;
    const int chipX = pageWidth - metrics.contentSidePadding - chipW;
    const int chipY = subHeaderY + (metrics.subHeaderHeight - CHIP_H) / 2;
    renderer.drawRect(chipX, chipY, chipW, CHIP_H, 1, true);
    const int textY = chipY + (CHIP_H - renderer.getLineHeight(SMALL_FONT_ID)) / 2;
    renderer.drawText(SMALL_FONT_ID, chipX + CHIP_PAD_X, textY, holidaysLabel, true);
  }

  constexpr int GRID_MARGIN_X = 10;
  constexpr int WEEKDAY_HEADER_H = 22;
  constexpr int NOTE_AREA_H = 34;

  const int gridTop = subHeaderY + metrics.subHeaderHeight + metrics.verticalSpacing;
  const int gridBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - NOTE_AREA_H;

  const int gridW = pageWidth - 2 * GRID_MARGIN_X;
  const int cellW = gridW / 7;
  const int daysGridTop = gridTop + WEEKDAY_HEADER_H;
  const int cellH = (gridBottom - daysGridTop) / 6;

  for (int i = 0; i < 7; i++) {
    const int x = GRID_MARGIN_X + i * cellW;
    const char* label = I18N.get(kWeekdayKeys[i]);
    const int tw = renderer.getTextWidth(SMALL_FONT_ID, label);
    renderer.drawText(SMALL_FONT_ID, x + (cellW - tw) / 2, gridTop, label, true);
  }

  const int firstWd = firstWeekdayOfMonth(viewYear, viewMonth);
  const int total = daysInMonth(viewYear, viewMonth);

  for (int day = 1; day <= total; day++) {
    const int cellIndex = firstWd + (day - 1);
    const int row = cellIndex / 7;
    const int col = cellIndex % 7;
    const int cx = GRID_MARGIN_X + col * cellW;
    const int cy = daysGridTop + row * cellH;

    const bool isToday = (viewYear == todayYear && viewMonth == todayMonth && day == todayDay);
    const bool isSelected = (day == selectedDay);
    const bool hasNote = !getNote(day).empty();
    const bool dayIsHoliday = isHoliday(viewMonth, day, nullptr);

    if (isSelected) {
      renderer.fillRect(cx + 1, cy + 1, cellW - 2, cellH - 2, true);
    } else if (isToday) {
      renderer.drawRect(cx + 1, cy + 1, cellW - 2, cellH - 2, 2, true);
    }

    char dayBuf[3];
    snprintf(dayBuf, sizeof(dayBuf), "%d", day);
    const int tw = renderer.getTextWidth(UI_12_FONT_ID, dayBuf);
    const int th = renderer.getLineHeight(UI_12_FONT_ID);
    const int tx = cx + (cellW - tw) / 2;
    const int ty = cy + (cellH - th) / 2;
    renderer.drawText(UI_12_FONT_ID, tx, ty, dayBuf, !isSelected);

    // Two fixed, non-overlapping marker positions so a day that is both a
    // holiday and has a personal note shows both: public holiday at the top,
    // personal note at the bottom.
    if (dayIsHoliday) {
      renderer.fillRect(cx + cellW / 2 - 2, cy + 3, 4, 4, !isSelected);
    }
    if (hasNote) {
      const int dotY = cy + cellH - 7;
      renderer.fillRect(cx + cellW / 2 - 2, dotY, 4, 4, !isSelected);
    }
  }

  // Day/holiday text lives in the NoteView popup on demand (see below), not
  // as a passive strip here -- a truncated one-liner squeezed under the grid
  // read as clutter rather than content.
  const int noteY = gridBottom + 6;
  if (dateUnconfirmed) {
    renderer.drawText(SMALL_FONT_ID, GRID_MARGIN_X, noteY, tr(STR_CALENDAR_CLOCK_NOT_SYNCED), true);
  } else {
    renderer.drawText(SMALL_FONT_ID, GRID_MARGIN_X, noteY, tr(STR_CALENDAR_HOLIDAYS_HOLD_HINT), true);
  }

  if (state == CalendarState::NoteView) {
    GUI.drawPopup(renderer, dayPreviewText(selectedDay).c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CALENDAR_NOTE), nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    const auto labels =
        mappedInput.mapLabels(tr(STR_BACK), tr(STR_CALENDAR_NOTE), tr(STR_PREVIOUS_TAB), tr(STR_NEXT_TAB));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
