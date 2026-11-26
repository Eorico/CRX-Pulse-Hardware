#include <cassert>

#include "sensor_filters.h"

int main() {
  int odd[] = {9, 1, 8, 2, 7, 3, 6, 4, 5};
  assert(SensorFilters::medianOf(odd, 9) == 5);

  int even[] = {9, 1, 8, 2, 7, 3};
  assert(SensorFilters::medianOf(even, 6) == 5);

  // First sample seeds the output directly: a filter that eased up from
  // 7.00 would show a wrong pH for the first few seconds after boot.
  SensorFilters::StableValue stable(0.15f, 0.02f);
  assert(stable.update(7.00f) == 7.00f);

  // One-count ADC jitter must not make the last displayed digit dance.
  for (int i = 0; i < 50; i++) {
    const float sample = (i % 2 == 0) ? 7.01f : 6.99f;
    assert(stable.update(sample) == 7.00f);
  }

  // A real change still gets through, but only after it is sustained.
  for (int i = 0; i < 200; i++) stable.update(7.20f);
  assert(stable.value() > 7.18f);
  assert(stable.value() <= 7.20f);

  stable.reset();
  assert(stable.update(6.86f) == 6.86f);

  SensorFilters::JumpGuard guard;
  assert(guard.update(7.00f) == 7.00f);
  assert(guard.update(10.00f) == 7.00f);
  assert(guard.update(7.10f) == 7.10f);

  SensorFilters::JumpGuard drifting;
  assert(drifting.update(7.00f) == 7.00f);
  assert(drifting.update(7.30f) == 7.30f);

  SensorFilters::JumpGuard resetGuard;
  assert(resetGuard.update(7.00f) == 7.00f);
  assert(resetGuard.update(10.00f) == 7.00f);
  resetGuard.reset();
  assert(resetGuard.update(10.00f) == 10.00f);

  SensorFilters::JumpGuard confirmed;
  assert(confirmed.update(7.00f) == 7.00f);
  assert(confirmed.update(10.00f) == 7.00f);
  assert(confirmed.update(10.10f) == 10.10f);

  // bad pass never starts the ramp
  SensorFilters::JumpGuard badPass;
  SensorFilters::StableValue badPassEma(0.15f, 0.02f);
  badPassEma.update(badPass.update(7.00f));
  assert(badPassEma.update(badPass.update(10.00f)) == 7.00f);
  assert(badPassEma.update(badPass.update(7.10f)) == 7.00f);

  // confirmed breach reaches the EMA
  SensorFilters::JumpGuard realBreach;
  SensorFilters::StableValue realBreachEma(0.15f, 0.02f);
  realBreachEma.update(realBreach.update(7.00f));
  realBreachEma.update(realBreach.update(10.00f));
  assert(realBreachEma.update(realBreach.update(10.10f)) > 7.00f);

  // noise test still passes through the guard
  SensorFilters::JumpGuard noisyGuard;
  SensorFilters::StableValue noisyEma(0.15f, 0.02f);
  noisyEma.update(noisyGuard.update(7.00f));
  for (int i = 0; i < 50; i++) {
    const float sample = (i % 2 == 0) ? 7.01f : 6.99f;
    assert(noisyEma.update(noisyGuard.update(sample)) == 7.00f);
  }

  return 0;
}
