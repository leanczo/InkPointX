#pragma once

#include <DateMath.h>

#include <string>
#include <vector>

struct Reminder {
  int16_t year = 1970;
  uint8_t month = 1;  // 1-12
  uint8_t day = 1;    // 1-31
  DateMath::RepeatRule repeat = DateMath::RepeatRule::Never;
  std::string description;
};

/**
 * Singleton class for storing user reminders on the SD card as JSON.
 */
class RemindersStore {
  static RemindersStore instance;
  std::vector<Reminder> reminders;

  static constexpr size_t MAX_REMINDERS = 200;

  RemindersStore() = default;

 public:
  RemindersStore(const RemindersStore&) = delete;
  RemindersStore& operator=(const RemindersStore&) = delete;

  static RemindersStore& getInstance() { return instance; }

  bool saveToFile() const;
  bool loadFromFile();

  bool addReminder(const Reminder& reminder);
  bool updateReminder(size_t index, const Reminder& reminder);
  bool removeReminder(size_t index);

  const std::vector<Reminder>& getReminders() const { return reminders; }
  const Reminder* getReminder(size_t index) const;
  size_t getCount() const { return reminders.size(); }
};

#define REMINDERS_STORE RemindersStore::getInstance()
