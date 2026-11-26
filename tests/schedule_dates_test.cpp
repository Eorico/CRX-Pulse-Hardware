#include <cassert>
#include <cstring>
#include <ctime>

#include "schedule_dates.h"

int main() {
  struct tm today = {};
  today.tm_year = 126;  // 2026
  today.tm_mon = 10;    // November
  today.tm_mday = 24;

  char date[11] = {};
  formatScheduleDate(today, date, sizeof(date));
  assert(std::strcmp(date, "2026-11-24") == 0);

  assert(scheduleMatchesDate("", "2026-11-24"));
  assert(scheduleMatchesDate("2026-11-24", "2026-11-24"));
  assert(!scheduleMatchesDate("2026-11-25", "2026-11-24"));

  char key[17] = {};
  buildFeedKey("2026-11-24", 8 * 60, key, sizeof(key));
  assert(std::strcmp(key, "2026-11-24 08:00") == 0);

  char otherDayKey[17] = {};
  buildFeedKey("2026-11-25", 8 * 60, otherDayKey, sizeof(otherDayKey));
  assert(std::strcmp(key, otherDayKey) != 0);

  char midnightKey[17] = {};
  buildFeedKey("2026-11-25", 0, midnightKey, sizeof(midnightKey));
  assert(std::strcmp(midnightKey, "2026-11-25 00:00") == 0);

  char endOfDayKey[17] = {};
  buildFeedKey("2026-11-24", 23 * 60 + 59, endOfDayKey, sizeof(endOfDayKey));
  assert(std::strcmp(endOfDayKey, "2026-11-24 23:59") == 0);

  // Values written through the AP must be parseable by the feeding check.
  assert(isValidScheduleTime("00:00"));
  assert(isValidScheduleTime("08:05"));
  assert(isValidScheduleTime("23:59"));
  assert(!isValidScheduleTime("8:00"));
  assert(!isValidScheduleTime("24:00"));
  assert(!isValidScheduleTime("12:60"));
  assert(!isValidScheduleTime("12-00"));
  assert(!isValidScheduleTime("1200"));
  assert(!isValidScheduleTime(""));

  assert(isValidScheduleDate("2026-11-24"));
  assert(isValidScheduleDate("2026-01-01"));
  assert(!isValidScheduleDate("2026-1-24"));
  assert(!isValidScheduleDate("2026-13-24"));
  assert(!isValidScheduleDate("2026-11-00"));
  assert(!isValidScheduleDate("2026/11/24"));
  assert(!isValidScheduleDate(""));

  return 0;
}
