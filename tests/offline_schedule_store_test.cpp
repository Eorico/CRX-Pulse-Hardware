#include <cassert>
#include <cstring>

#include "offline_schedule_store.h"

int main() {
  char slot[64] = {};

  // A dated row round-trips with every field intact.
  encodeScheduleSlot("08:00", "2026-11-24", 15, 180, true,
                     slot, sizeof(slot));
  assert(std::strcmp(slot, "08:00|2026-11-24|15|180|1") == 0);

  char time[6] = {};
  char date[11] = {};
  int grams = 0;
  int pwm = 0;
  bool active = false;
  assert(decodeScheduleSlot(slot, time, sizeof(time), date, sizeof(date),
                            &grams, &pwm, &active));
  assert(std::strcmp(time, "08:00") == 0);
  assert(std::strcmp(date, "2026-11-24") == 0);
  assert(grams == 15);
  assert(pwm == 180);
  assert(active);

  // A legacy every-day row has an empty date, which must survive the split
  // even though strtok() would swallow it.
  encodeScheduleSlot("23:30", "", 10, 255, false, slot, sizeof(slot));
  assert(std::strcmp(slot, "23:30||10|255|0") == 0);
  assert(decodeScheduleSlot(slot, time, sizeof(time), date, sizeof(date),
                            &grams, &pwm, &active));
  assert(std::strcmp(time, "23:30") == 0);
  assert(date[0] == '\0');
  assert(grams == 10);
  assert(pwm == 255 && !active);

  // A truncated slot is rejected rather than decoded into a bogus feed.
  assert(!decodeScheduleSlot("08:00|2026-11-24|15", time, sizeof(time),
                             date, sizeof(date), &grams, &pwm, &active));

  return 0;
}
