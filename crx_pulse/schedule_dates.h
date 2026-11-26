#ifndef SCHEDULE_DATES_H
#define SCHEDULE_DATES_H

#include <stdio.h>
#include <string.h>
#include <time.h>

// The ESP32's getLocalTime() already applies the UTC+8 offset, so its tm
// fields are the calendar date the operator picked in the app.
inline void formatScheduleDate(const struct tm& t, char* out, size_t size) {
  snprintf(out, size, "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
}

// An empty date is a legacy every-day schedule.
inline bool scheduleMatchesDate(const char* feedDate, const char* today) {
  return feedDate[0] == '\0' || strcmp(feedDate, today) == 0;
}

inline void buildFeedKey(const char* today, int minuteOfDay, char* out, size_t size) {
  snprintf(out, size, "%s %02d:%02d", today, minuteOfDay / 60, minuteOfDay % 60);
}

// Feeds only fire when the stored string matches the firmware's own
// padded output. Rejecting malformed values at the AP boundary stops a
// typo from becoming a schedule that silently never runs.
inline bool scheduleIsDigit(char c) { return c >= '0' && c <= '9'; }

inline int scheduleTwoDigits(const char* value) {
  return (value[0] - '0') * 10 + (value[1] - '0');
}

inline bool isValidScheduleTime(const char* value) {
  if (strlen(value) != 5 || value[2] != ':') return false;
  for (int i = 0; i < 5; i++) {
    if (i == 2) continue;
    if (!scheduleIsDigit(value[i])) return false;
  }
  const int hour = scheduleTwoDigits(value);
  const int minute = scheduleTwoDigits(value + 3);
  return hour <= 23 && minute <= 59;
}

inline bool isValidScheduleDate(const char* value) {
  if (strlen(value) != 10 || value[4] != '-' || value[7] != '-') return false;
  for (int i = 0; i < 10; i++) {
    if (i == 4 || i == 7) continue;
    if (!scheduleIsDigit(value[i])) return false;
  }
  const int month = scheduleTwoDigits(value + 5);
  const int day = scheduleTwoDigits(value + 8);
  return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

#endif
