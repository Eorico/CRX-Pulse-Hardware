# Design: recalibrate pH voltages to the current probe

Date: 2026-11-26

## Intent

The probe now produces different voltages from the ones in the existing table. With the old anchors, a buffer that should read `9.18` reads `2.67`, and buffers whose voltage lands above `V_PH7` clamp to `0.00`. The person using the system wants the standard buffer values to show correctly on the dashboard.

The first two buffer labels from the measurement run were confirmed swapped, so the usable measurements are:

| Buffer | Measured reconstructed probe voltage |
|---|---:|
| 4.01 | `3.970 V` |
| 6.86 | `3.715 V` |
| 9.18 | `2.680 V` |

## Goals

- A probe in the 4.01 buffer displays `4.01`; in 6.86 displays `6.86`; in 9.18 displays `9.18`.
- Keep the two-segment interpolation shape and the pH constants `6.86 / 4.01 / 9.18`.
- Keep the jump guard, median-of-15, snap publish, read cadence and publish cadence unchanged.
- Keep the app's calibration reference in step with the firmware.
- Make the three anchors verifiable by an automated host test rather than by arithmetic done in chat.

## Non-goals

- No change to the probe, divider, wiring, or power. The electrical cause remains a separate problem.
- No forced/clamped fake value such as “any high voltage means 9.18”.
- No change to `PhReadingFilter`, `JumpGuard` threshold `0.5`, deadband `0.02`, or sample count `15`.
- No mobile dependency or new artwork.
- `crx_pulse/HARDWARE_WIRING.md` stays untouched under the earlier scope rule; its printed pH table will remain stale until a separate request updates it.
- Never open, read, print, search, stage, or commit `crx_pulse/secrets.h` or the credential block near the top of `crx_pulse.ino`.

## Constraints

- Firmware host tests compile with `g++ -std=c++11 -Icrx_pulse`; no Arduino CLI.
- Mobile verification is `flutter analyze` plus the targeted test set including `test/calibration_test.dart`.
- `PhCalibration` currently lives inside `crx_pulse.ino`, which a host test cannot include without pulling in the credential block. Moving it into an included helper header is therefore part of this change.
- The include must be added alongside the existing helper-header includes; the credential lines are not read or edited.

## Approaches considered

1. **Change only `V_PH9`.** Rejected: the measured voltages are above `V_PH7`, so the firmware takes the acid branch and never reads `V_PH9`.
2. **Add a voltage transform before calibration.** Rejected: it adds a tuning layer, still needs multiple measured points, and is less direct than correcting the anchors themselves.
3. **Replace the three anchor voltages and extract `PhCalibration` for testing (chosen).** Exact at the three measured buffers, keeps the interpolation shape, and gives a real regression test.

## Design

- Create `crx_pulse/ph_calibration.h` containing the existing `PhCalibration` class with:
  - `PH_NEUTRAL = 6.86`, `PH_ACID = 4.01`, `PH_BASE = 9.18`
  - `V_PH7 = 3.715`, `V_PH4 = 3.970`, `V_PH9 = 2.680`
  - `DIVIDER_RATIO = 2.0` and the unchanged two-segment `valueOf()` with `constrain(pH, 0.0, 14.0)`
  - `constrain()` stays as supplied by Arduino in the sketch; `tests/ph_calibration_test.cpp` defines a minimal shim before including the header so the real `valueOf()` can be compiled on the host
- In `crx_pulse.ino`, include `ph_calibration.h` with the other helper headers and remove the in-sketch `PhCalibration` definition. No other pH line changes.
- Add `tests/ph_calibration_test.cpp` with three assertions: `valueOf(3.715f) == 6.86f`, `valueOf(3.970f) == 4.01f`, `valueOf(2.680f) == 9.18f`, plus an ordering assertion `V_PH4 > V_PH7 > V_PH9`.
- In the mobile repo, update `FirmwareSpec.phVoltNeutral / phVoltAcid / phVoltBase` to `3.715 / 3.970 / 2.680` and update the matching expectations in `test/calibration_test.dart`. The pH buffer constants and ordering test stay valid.
- Push both repos only after host tests and targeted Flutter tests pass.

## What is left to the builder

Write the step-by-step plan, implement test-first, verify the exact anchor values, check firmware hunk headers for credential safety, run a fresh review, then commit and push.
