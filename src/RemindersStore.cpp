#include "RemindersStore.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

namespace {
constexpr char REMINDERS_FILE_JSON[] = "/apps/reminders/reminders.json";

const char* repeatToString(DateMath::RepeatRule repeat) {
  switch (repeat) {
    case DateMath::RepeatRule::Weekly:
      return "weekly";
    case DateMath::RepeatRule::Monthly:
      return "monthly";
    case DateMath::RepeatRule::Yearly:
      return "yearly";
    case DateMath::RepeatRule::Never:
    default:
      return "never";
  }
}

DateMath::RepeatRule repeatFromString(const char* s) {
  if (strcmp(s, "weekly") == 0) return DateMath::RepeatRule::Weekly;
  if (strcmp(s, "monthly") == 0) return DateMath::RepeatRule::Monthly;
  if (strcmp(s, "yearly") == 0) return DateMath::RepeatRule::Yearly;
  return DateMath::RepeatRule::Never;
}
}  // namespace

RemindersStore RemindersStore::instance;

bool RemindersStore::saveToFile() const {
  Storage.mkdir("/apps/reminders");

  JsonDocument doc;
  JsonArray arr = doc["reminders"].to<JsonArray>();
  for (const Reminder& r : reminders) {
    JsonObject entry = arr.add<JsonObject>();
    entry["year"] = r.year;
    entry["month"] = r.month;
    entry["day"] = r.day;
    entry["repeat"] = repeatToString(r.repeat);
    entry["description"] = r.description;
  }

  String json;
  serializeJson(doc, json);
  return Storage.writeFile(REMINDERS_FILE_JSON, json);
}

bool RemindersStore::loadFromFile() {
  reminders.clear();
  Storage.recoverInterruptedWrite(REMINDERS_FILE_JSON);
  if (!Storage.exists(REMINDERS_FILE_JSON)) return false;

  const String json = Storage.readFile(REMINDERS_FILE_JSON);
  if (json.isEmpty()) return false;

  JsonDocument doc;
  const auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("RMS", "JSON parse error: %s", error.c_str());
    return false;
  }

  JsonArrayConst arr = doc["reminders"];
  for (JsonObjectConst entry : arr) {
    if (reminders.size() >= MAX_REMINDERS) break;
    Reminder r;
    r.year = entry["year"] | 1970;
    r.month = entry["month"] | 1;
    r.day = entry["day"] | 1;
    r.repeat = repeatFromString(entry["repeat"] | "never");
    r.description = entry["description"] | "";
    reminders.push_back(std::move(r));
  }
  return true;
}

bool RemindersStore::addReminder(const Reminder& reminder) {
  if (reminders.size() >= MAX_REMINDERS) {
    LOG_DBG("RMS", "Cannot add more reminders, limit of %zu reached", MAX_REMINDERS);
    return false;
  }

  reminders.push_back(reminder);
  if (saveToFile()) return true;
  reminders.pop_back();
  return false;
}

bool RemindersStore::updateReminder(size_t index, const Reminder& reminder) {
  if (index >= reminders.size()) return false;

  const Reminder previous = reminders[index];
  reminders[index] = reminder;
  if (saveToFile()) return true;
  reminders[index] = previous;
  return false;
}

bool RemindersStore::removeReminder(size_t index) {
  if (index >= reminders.size()) return false;

  const Reminder removed = reminders[index];
  reminders.erase(reminders.begin() + static_cast<ptrdiff_t>(index));
  if (saveToFile()) return true;
  reminders.insert(reminders.begin() + static_cast<ptrdiff_t>(index), removed);
  return false;
}

const Reminder* RemindersStore::getReminder(size_t index) const {
  if (index >= reminders.size()) return nullptr;
  return &reminders[index];
}
