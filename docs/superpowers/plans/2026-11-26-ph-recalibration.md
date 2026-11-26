# pH Recalibration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the current probe display `4.01`, `6.86`, and `9.18` in its three buffers by replacing the three firmware voltage anchors with the measured values, and keep the app's reference copy in step.

**Architecture:** Move `PhCalibration` out of `crx_pulse.ino` into an included header so host tests can exercise the real interpolation without including the sketch's credential block. Keep the two-segment interpolation, median-of-15, `JumpGuard`, `SnapValue`, deadband, and all cadences unchanged. Update the mobile `FirmwareSpec` mirror and its pinned test.

**Tech Stack:** ESP32 Arduino C++ sketch; host C++11 tests with `g++`; Flutter/Dart reference test with `flutter test`.

**Spec:** `docs/superpowers/specs/2026-11-26-ph-recalibration-design.md`

## Global Constraints

- pH constants stay `PH_NEUTRAL = 6.86`, `PH_ACID = 4.01`, `PH_BASE = 9.18`.
- New voltage anchors are `V_PH7 = 3.715`, `V_PH4 = 3.970`, `V_PH9 = 2.680`; `DIVIDER_RATIO = 2.0` unchanged.
- Interpolation remains the existing two-segment form with `constrain(pH, 0.0, 14.0)`.
- `JumpGuard::MAX_IMMEDIATE_CHANGE = 0.5f`, `PhFilterConfig::SAMPLE_COUNT = 15`, `MIN_CHANGE = 0.02f`, 2 s read cadence and 3 s cloud publish cadence are unchanged.
- Mobile app gains no dependency and no artwork; its `PhReadingFilter` is unchanged.
- The two firmware files `Wiring Diagram.fzz` and `crx_pulse/HARDWARE_WIRING.md`, and the bench sketch `crx_pulse_test/crx_pulse_test.ino`, are out of scope.
- Never open, read, print, search, stage, or commit `crx_pulse/secrets.h` or the credential block near the top of `crx_pulse/crx_pulse.ino`.
- Before staging any `.ino` change, inspect `git diff -U0 -- crx_pulse/crx_pulse.ino | Select-String '^@@'` and confirm no hunk starts in the credential lines.
- Host tests compile with `g++ -std=c++11 -Icrx_pulse`; no Arduino CLI.
- Push both repos to `origin/main` only after review and verification.

## Review Focus

- `3.970 V` must display `4.01`, not clamp to `0.00` — pinned in Task 1 by the acid anchor assertion.
- `3.715 V` must display `6.86` — pinned in Task 1 by the neutral anchor assertion.
- `2.680 V` must display `9.18`, not `2.67` — pinned in Task 1 by the base anchor assertion.
- The anchors must stay ordered acid > neutral > base so the `probeVolts >= V_PH7` branch keeps working — pinned in Task 1 by the ordering assertions and in Task 3 by the mobile ordering test.
- The app's reference voltages must not drift from the firmware — pinned in Task 3 by `test/calibration_test.dart`.
- Out-of-range probes must still be clamped to `0..14` rather than emitting nonsense — pinned in Task 1 by the `6.000 V -> 0.0` and `0.000 V -> 14.0` assertions.

---

### Task 1: Extract `PhCalibration` and pin the new anchors

**Files:**
- Create: `crx_pulse/ph_calibration.h`
- Create: `tests/ph_calibration_test.cpp`
- Modify: `docs/superpowers/specs/2026-11-26-ph-recalibration-design.md`
- Modify: `docs/superpowers/plans/2026-11-26-ph-recalibration.md`

**Interfaces:**
- Consumes: the three buffer voltages confirmed by the user after the label swap.
- Produces: `PhCalibration::valueOf(float probeVolts)` from `crx_pulse/ph_calibration.h`, with `PH_NEUTRAL`, `PH_ACID`, `PH_BASE`, `V_PH7`, `V_PH4`, `V_PH9`, and `DIVIDER_RATIO` as `static constexpr float`.

- [x] **Step 1: Write the failing host test**

Create `tests/ph_calibration_test.cpp`:

```cpp
#include <cmath>

// Arduino supplies constrain() in the sketch; this shim lets the host test
// exercise the real interpolation without including the credential block.
inline float constrain(float value, float low, float high) {
  return value < low ? low : (value > high ? high : value);
}

#include "ph_calibration.h"

#define CHECK(condition) \
  do {                   \
    if (!(condition)) {  \
      return 1;          \
    }                    \
  } while (0)

int main() {
  CHECK(std::fabs(PhCalibration::valueOf(3.715f) - 6.86f) < 1e-5f);
  CHECK(std::fabs(PhCalibration::valueOf(3.970f) - 4.01f) < 1e-5f);
  CHECK(std::fabs(PhCalibration::valueOf(2.680f) - 9.18f) < 1e-5f);
  CHECK(PhCalibration::V_PH4 > PhCalibration::V_PH7);
  CHECK(PhCalibration::V_PH7 > PhCalibration::V_PH9);
  CHECK(PhCalibration::valueOf(6.000f) == 0.0f);
  CHECK(PhCalibration::valueOf(0.000f) == 14.0f);
  return 0;
}
```

- [x] **Step 2: Run the test to verify it fails**

Run: `g++ -std=c++11 -Icrx_pulse tests/ph_calibration_test.cpp -o "$env:TEMP\ph_calibration_test.exe"`

Expected: compile failure naming `ph_calibration.h` as missing.

- [x] **Step 3: Create `crx_pulse/ph_calibration.h`**

Move the existing `PhCalibration` class into this header with an include guard `PH_CALIBRATION_H`. Keep `valueOf()` byte-for-byte equivalent to the current sketch implementation, including `constrain(pH, 0.0, 14.0)`. Replace only the voltage constants and the leading comment table:

```text
buffer  true pH  probe volts
acid    4.01     3.970 V
neutral 6.86     3.715 V
base    9.18     2.680 V
```

Do not invent a new formula and do not add `<cmath>` to the header.

- [x] **Step 4: Run the test to verify it passes**

Run:
`g++ -std=c++11 -Icrx_pulse tests/ph_calibration_test.cpp -o "$env:TEMP\ph_calibration_test.exe"`
then `& "$env:TEMP\ph_calibration_test.exe"`

Expected: compile succeeds and the executable exits 0.

- [x] **Step 5: Commit**

```bash
git add crx_pulse/ph_calibration.h tests/ph_calibration_test.cpp docs/superpowers/specs/2026-11-26-ph-recalibration-design.md docs/superpowers/plans/2026-11-26-ph-recalibration.md
git commit -m "fix: calibrate pH to measured buffer voltages"
```

---

### Task 2: Make the sketch use the calibration header

**Files:**
- Modify: `crx_pulse/crx_pulse.ino:135-151` (helper-header includes)
- Modify: `crx_pulse/crx_pulse.ino:565-601` (remove the in-sketch `PhCalibration` definition)

**Interfaces:**
- Consumes: `crx_pulse/ph_calibration.h` from Task 1.
- Produces: the sketch still calls `PhCalibration::valueOf(phCh.sensorVolts)` at the existing publish line; no call-site change.

- [x] **Step 1: Add the include**

After `#include "turbidity_calibration.h"` at line 151, add:

```cpp
#include "ph_calibration.h"
```

Do not read or edit the credential lines near the top of the file.

- [x] **Step 2: Remove the in-sketch class**

Delete the whole existing `class PhCalibration { ... };` block at lines 565–601, including its old comment table. Do not edit the surrounding `PhFilterConfig`, `phFilter`, or `phJumpGuard` declarations.

- [x] **Step 3: Verify host tests and scope**

Run all host tests:

```powershell
Get-ChildItem tests/*.cpp | ForEach-Object { $name = $_.BaseName; $exe = "$env:TEMP\$name.exe"; g++ -std=c++11 -Icrx_pulse $_.FullName -o $exe; if ($LASTEXITCODE -ne 0) { "$name : COMPILE FAIL"; return }; & $exe; if ($LASTEXITCODE -eq 0) { "$name : PASS" } else { "$name : FAIL ($LASTEXITCODE)" } }
```

Expected: `offline_schedule_store_test`, `ph_calibration_test`, `schedule_dates_test`, `sensor_filters_test`, `turbidity_calibration_test` all `PASS`.

Run `git diff -U0 -- crx_pulse/crx_pulse.ino | Select-String '^@@'` and confirm no hunk header starts inside the credential lines. Run `git diff --check`.

- [x] **Step 4: Commit**

```bash
git add crx_pulse/crx_pulse.ino
git commit -m "Updated: Use measured pH calibration header"
```

---

### Task 3: Update the mobile calibration mirror

**Files:**
- Modify: `test/calibration_test.dart:134-143`
- Modify: `lib/core/firmware_spec.dart:285-296`

**Interfaces:**
- Consumes: the same three anchors from Task 1.
- Produces: `FirmwareSpec.phVoltNeutral = 3.715`, `phVoltAcid = 3.970`, `phVoltBase = 2.680`.

- [x] **Step 1: Change the test expectations first**

In `test/calibration_test.dart`, replace the three anchor expectations in `anchor voltages are the specified buffer voltages` with:

```dart
      expect(FirmwareSpec.phVoltNeutral, closeTo(3.715, 1e-9));
      expect(FirmwareSpec.phVoltAcid, closeTo(3.970, 1e-9));
      expect(FirmwareSpec.phVoltBase, closeTo(2.680, 1e-9));
```

Keep the `voltage falls as pH rises` ordering test unchanged; the new values still satisfy it.

- [x] **Step 2: Run the test to verify it fails**

Run: `flutter test --reporter compact test/calibration_test.dart`

Expected: three failures showing the old values `1.950`, `2.447`, `1.425`.

- [x] **Step 3: Update `FirmwareSpec`**

Replace only:

```dart
  static const double phVoltNeutral = 1.950;
  static const double phVoltAcid = 2.447;
  static const double phVoltBase = 1.425;
```

with:

```dart
  static const double phVoltNeutral = 3.715;
  static const double phVoltAcid = 3.970;
  static const double phVoltBase = 2.680;
```

Do not change the pH buffer constants, alert ranges, or any other field.

- [x] **Step 4: Run the mobile verification**

Run: `flutter test --reporter compact test/calibration_test.dart`
Expected: PASS.

Run: `flutter analyze`
Expected: `No issues found!`

Run the targeted suite:

```powershell
flutter test --reporter compact test/ph_reading_filter_test.dart test/notification_detail_screen_test.dart test/notifications_screen_detail_test.dart test/calibration_test.dart test/local_esp32_service_firmware_tag_test.dart test/notification_card_test.dart test/notification_format_test.dart
```

Expected: all listed tests pass.

Check `git status --short`. If Flutter regenerated plugin registrant files, restore them before staging. Stage only the two intended files.

- [x] **Step 5: Commit**

```bash
git add lib/core/firmware_spec.dart test/calibration_test.dart
git commit -m "fix: mirror measured pH calibration in app"
```

---

### Task 4: Review and push both repos

**Files:** none.

**Interfaces:** Consumes the firmware commits from Tasks 1–2 and the mobile commit from Task 3.

- [ ] **Step 1: Fresh whole-change review**

Run one reviewer on each repo over the new commits. The reviewer must not open or print `crx_pulse/secrets.h` or the credential block, and must check the Review Focus list. Fix any Critical or Important issue before pushing.

- [ ] **Step 2: Push**

In the hardware repo run `git status -sb`, then `git push origin main`.
In the mobile repo run `git status -sb`, then `git push origin HEAD:main`.
Fetch both and confirm `origin/main` points at the new commits and both working trees are clean.
