# pH jump guard in firmware — design (2026-11-26)

## Intent
Stop the published pH from drifting to 9–10 and back while the probe is reading. Fix it at the source so the device, phone app, alerts and cloud rows all see the same stable value.

## Root cause found
`readSensors()` converts the median-of-15 ADC result to pH, then feeds it to `SensorFilters::StableValue` (EMA 0.15, deadband 0.02). That EMA publishes 15% of any jump on the very next 2 s pass. Host probe of the existing class: one converted `10.0` sample moves `7.00 → 7.45`; sustained high samples reach `9.41` after 10 passes (20 s) and `9.88` after 20 passes (40 s). Every step is below 0.5, so the phone-side `PhReadingFilter` cannot reject the drift.

## Chosen approach: jump guard before the EMA
Add a small stateful `SensorFilters::JumpGuard` to `sensor_filters.h` using the phone's rule: changes up to 0.5 pass immediately; a larger jump is held until the next reading agrees (within 0.5 of the candidate). A return toward the accepted value discards the candidate. The first reading, and the first after `reset()`, is authoritative.

In `crx_pulse.ino`, feed `PhCalibration::valueOf(phCh.sensorVolts)` through the guard and then into the existing `phFilter.update(...)`. The median still removes ADC spikes; the EMA still quiets the last digit; a bad high pass can no longer start the ramp. A genuine breach is published on the second agreeing pass, so pH alerts still work.

## Alternatives considered
- Guard after the EMA: useless here — each EMA step is already under 0.5.
- Replace the EMA with the guard alone: lets ADC noise move the last digit again.
- Clamp pH to 6.5–8.5: hides a real unsafe pH. Rejected.

## Constraints
- No new files in the sketch folder beyond the header change; no new dependencies.
- Do not open or edit `crx_pulse/secrets.h` or the credential block near the top of `crx_pulse.ino`.
- Leave the existing uncommitted work (`Wiring Diagram.fzz`, bench sketch, docs, untracked headers/tests) untouched.
- Do not stage or commit.
- Host tests cover: one spike held, confirming reading accepted, ordinary drift immediate, reset, null-equivalent/invalid handling is not needed here because the firmware always supplies a finite float.
