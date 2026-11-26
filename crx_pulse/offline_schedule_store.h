#ifndef OFFLINE_SCHEDULE_STORE_H
#define OFFLINE_SCHEDULE_STORE_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// One cached schedule as "HH:MM|YYYY-MM-DD|grams|pwm|active". An empty date
// is a legacy every-day row, so the empty field is deliberate and must not
// be collapsed by strtok().
inline void encodeScheduleSlot(const char* time, const char* date,
                               int grams, int pwm, bool active,
                               char* out, size_t size) {
  snprintf(out, size, "%s|%s|%d|%d|%d",
           time, date == nullptr ? "" : date, grams, pwm, active ? 1 : 0);
}

inline bool nextSlotField(const char** cursor, char* out, size_t outSize,
                          bool allowEmpty) {
  const char* start = *cursor;
  const char* sep = strchr(start, '|');
  const size_t len = sep ? (size_t)(sep - start) : strlen(start);
  if (len == 0 && !allowEmpty) return false;
  if (len >= outSize) return false;
  memcpy(out, start, len);
  out[len] = '\0';
  *cursor = sep ? sep + 1 : start + len;
  return true;
}

inline bool decodeScheduleSlot(const char* in,
                               char* time, size_t timeSize,
                               char* date, size_t dateSize,
                               int* grams, int* pwm, bool* active) {
  if (in == nullptr) return false;
  const char* cursor = in;
  char number[16] = {};

  if (!nextSlotField(&cursor, time, timeSize, false)) return false;
  if (!nextSlotField(&cursor, date, dateSize, true)) return false;
  if (!nextSlotField(&cursor, number, sizeof(number), false)) return false;
  *grams = atoi(number);
  if (!nextSlotField(&cursor, number, sizeof(number), false)) return false;
  *pwm = atoi(number);
  if (!nextSlotField(&cursor, number, sizeof(number), false)) return false;
  *active = atoi(number) != 0;
  return true;
}

#endif
