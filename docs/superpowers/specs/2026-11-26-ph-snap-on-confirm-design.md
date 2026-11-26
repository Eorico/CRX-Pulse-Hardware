# Design: snap pH to the confirmed reading

Date: 2026-11-26

## Intent

The dashboard's pH value visibly creeps toward a new reading over about 20
seconds. The person using the tank wants the number to follow the probe as
soon as it can, while keeping the protection against a single bogus 9–10
reading. The calibration itself is already correct and must stay untouched.

## Goals

- Publish a pH change in the same 2 s sensor pass when it is small enough to
  be ordinary drift or noise; publish a jump larger than 0.5 pH on the first
  pass that confirms it (two passes, 4 s, from the first bad-looking sample).
- Keep the exact 3-point mapping from the system update: 6.86 at 1.950 V,
  4.01 at 2.447 V, 9.18 at 1.425 V.
- Keep rejecting a one-off spike: one 7 → 10 → 7 sequence must never publish 10.
- Keep the small deadband so ADC noise cannot move the last displayed digit.

## Non-goals

- No change to buffer values or the two-segment interpolation in
  `PhCalibration::valueOf`.
- No change to the mobile app. Its `PhReadingFilter` already snaps after a
  confirming reading and has no ramp.
- No new dependency, no new sensor artwork, no change to publish cadence.

## Constraints

- `readSensors()` runs every 2 s (`LocalSensorTiming::READ_INTERVAL_MS`);
  cloud publish is every 3 s. A "pass" below means one 2 s read.
- pH still takes a median of 15 ADC reads per pass.
- `JumpGuard::MAX_IMMEDIATE_CHANGE` stays 0.5, matching the phone filter.
- Host tests are compiled with `g++ -std=c++11 -Icrx_pulse`; no Arduino CLI.
- The credential block near the top of `crx_pulse.ino` and `secrets.h` are
  never read, edited, staged, or printed.

## Approaches considered

1. **Publish the calibrated median directly.** One pass for every change,
   but raw ADC noise can move the last digit. Rejected: the deadband exists
   for a reason.
2. **Snap on confirmation (chosen).** Keep median-of-15, JumpGuard, and the
   0.02 deadband; replace the slow EMA with a filter that publishes the new
   value in full as soon as it clears the deadband. Fast for real changes,
   quiet for noise.
3. **Fast EMA (smoothing ~0.6).** Still a visible ramp of a few passes and a
   needless tuning knob. Rejected: the request is for the fastest honest
   reading, not a shorter ramp.

## Design

- Add `SensorFilters::SnapValue(minChange)` in `crx_pulse/sensor_filters.h`.
  First sample is authoritative. Afterwards, publish `sample` unchanged when
  `|sample - published| >= minChange`; otherwise keep the published value.
  Provide `value()` and `reset()` like the existing filter.
- Remove `SensorFilters::StableValue` and `PhFilterConfig::SMOOTHING`: pH is
  their only consumer, and leaving them would be dead code.
- Change `PhFilterConfig` to keep `SAMPLE_COUNT = 15` and `MIN_CHANGE = 0.02`,
  and change the pH filter instance to `SnapValue phFilter(PhFilterConfig::MIN_CHANGE)`.
- Leave the call site shape unchanged:
  `pH = phFilter.update(phJumpGuard.update(PhCalibration::valueOf(phCh.sensorVolts)));`
  The guard still enforces two agreeing passes for a >0.5 jump; the snap
  filter then publishes that confirmed value immediately instead of easing
  toward it.
- Update `tests/sensor_filters_test.cpp` so the EMA assertions become snap
  assertions: drift publishes in one pass, sub-deadband noise is held,
  a bad single pass never publishes 10, and a confirmed breach publishes
  10.10 on the confirming pass.

## What is left to the builder

Write the step-by-step plan, then implement test-first with host tests and
verify the diff touches only the pH filter lines and the filter header/tests.
