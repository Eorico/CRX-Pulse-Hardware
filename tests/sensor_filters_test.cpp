#include <cassert>

#include "sensor_filters.h"

int main() {
  int odd[] = {9, 1, 8, 2, 7, 3, 6, 4, 5};
  assert(SensorFilters::medianOf(odd, 9) == 5);

  int even[] = {9, 1, 8, 2, 7, 3};
  assert(SensorFilters::medianOf(even, 6) == 5);

  // First sample seeds the output directly: a filter that eased up from
  // 7.00 would show a wrong pH for the first few seconds after boot.
  SensorFilters::SnapValue snap(0.02f);
  assert(snap.update(7.00f) == 7.00f);

  // A real change is published in that same pass, not eased toward over
  // many passes.
  assert(snap.update(7.30f) == 7.30f);
  assert(snap.update(7.31f) == 7.30f);  // still inside the 0.02 deadband
  assert(snap.update(7.35f) == 7.35f);
  assert(snap.update(7.32f) == 7.32f);

  // One-count ADC jitter must not make the last displayed digit dance.
  SensorFilters::SnapValue quiet(0.02f);
  assert(quiet.update(7.00f) == 7.00f);
  for (int i = 0; i < 50; i++) {
    const float sample = (i % 2 == 0) ? 7.01f : 6.99f;
    assert(quiet.update(sample) == 7.00f);
  }

  SensorFilters::SnapValue resetSnap(0.02f);
  assert(resetSnap.update(7.00f) == 7.00f);
  assert(resetSnap.update(7.30f) == 7.30f);
  resetSnap.reset();
  assert(resetSnap.update(10.00f) == 10.00f);

  SensorFilters::SnapValue boundary(0.02f);
  assert(boundary.update(7.00f) == 7.00f);
  assert(boundary.update(7.015f) == 7.00f);
  assert(boundary.update(7.025f) == 7.025f);
  assert(boundary.value() == 7.025f);
  assert(boundary.update(7.03f) == 7.025f);
  assert(boundary.value() == 7.025f);

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

  // one bad pass must never publish the spike, even when the next reading
  // settles at a value the guard accepts outright
  SensorFilters::JumpGuard badPass;
  SensorFilters::SnapValue badPassSnap(0.02f);
  badPassSnap.update(badPass.update(7.00f));
  assert(badPassSnap.update(badPass.update(10.00f)) == 7.00f);
  assert(badPassSnap.update(badPass.update(7.10f)) == 7.10f);

  // a confirmed breach is published in full on the confirming pass
  SensorFilters::JumpGuard realBreach;
  SensorFilters::SnapValue realBreachSnap(0.02f);
  realBreachSnap.update(realBreach.update(7.00f));
  realBreachSnap.update(realBreach.update(10.00f));
  assert(realBreachSnap.update(realBreach.update(10.10f)) == 10.10f);

  return 0;
}
