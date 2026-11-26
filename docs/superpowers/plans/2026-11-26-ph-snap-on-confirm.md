# Snap pH to confirmed reading Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make published pH follow the calibrated reading in one 2 s pass for ordinary changes and on the confirming pass for a >0.5 jump, instead of easing toward it over ~20 s.

**Architecture:** Replace the slow `StableValue` EMA with `SensorFilters::SnapValue`, a deadband filter that publishes the sample unchanged once it differs from the published value by at least 0.02 pH. `JumpGuard` still holds a jump until a second agreeing pass, so a single 9–10 reading cannot start the change. The exact 3-point calibration and the median-of-15 sampler are untouched.

**Tech Stack:** ESP32 Arduino C++ sketch; host-side regression tests compiled with `g++ -std=c++11`; no new dependencies.

**Spec:** `docs/superpowers/specs/2026-11-26-ph-snap-on-confirm-design.md`

## Global Constraints

- Buffers and voltages stay exactly `6.86 / 1.950`, `4.01 / 2.447`, `9.18 / 1.425`; `PhCalibration::valueOf` is not edited.
- Median sample count stays `PhFilterConfig::SAMPLE_COUNT = 15`; publish cadence stays 3 s; read cadence stays 2 s.
- `JumpGuard::MAX_IMMEDIATE_CHANGE` stays `0.5f`; deadband stays `PhFilterConfig::MIN_CHANGE = 0.02`.
- Mobile app is not changed; its `PhReadingFilter` already snaps after confirmation.
- No new dependency, no new asset.
- Never open, print, edit, stage, or commit `crx_pulse/secrets.h` or the credential block near the top of `crx_pulse/crx_pulse.ino`.
- Before staging, verify no diff hunk of `crx_pulse.ino` touches those credential lines.
- Scope is the firmware repo only; the three files `Wiring Diagram.fzz`, `crx_pulse/HARDWARE_WIRING.md`, and `crx_pulse_test/crx_pulse_test.ino` are not part of this change.

## Review Focus

- A single bad pass `7.00 → 10.00 → 7.10` must never publish `10`; the confirming pass settles on the actual `7.10` — pinned in Task 1 by the bad-pass assertions.
- Two agreeing high passes `7.00 → 10.00 → 10.10` must publish `10.10` on the confirming pass, not a fraction of it — pinned in Task 1 by the `confirmed breach publishes immediately` assertion.
- Ordinary drift `7.00 → 7.30` must publish `7.30` in the same pass — pinned in Task 1 by the drift assertions.
- Alternating `7.01 / 6.99` ADC noise around `7.00` must keep publishing `7.00` — pinned in Task 1 by the sub-deadband noise loop.
- The first sample after `reset()` must be authoritative — pinned in Task 1 by the reset assertion.

---

### Task 1: Add the snap filter and rewrite its host tests

**Files:**
- Modify: `crx_pulse/sensor_filters.h`
- Test: `tests/sensor_filters_test.cpp`

**Interfaces:**
- Consumes: `SensorFilters::medianOf`, `SensorFilters::JumpGuard::MAX_IMMEDIATE_CHANGE` (both already in `sensor_filters.h`).
- Produces: `SensorFilters::SnapValue(float minChange)` with `float update(float sample)`, `float value() const`, and `void reset()`; first sample authoritative, then publish `sample` unchanged when `|sample - published| >= minChange`, otherwise hold the published value.

- [ ] **Step 1: Replace the EMA assertions in `tests/sensor_filters_test.cpp` with snap assertions**

Replace the `StableValue stable(...)` block and the two `StableValue` instances in the guard-integration assertions with these assertions (keep all `medianOf` and standalone `JumpGuard` blocks):

```cpp
  SensorFilters::SnapValue snap(0.02f);
  assert(snap.update(7.00f) == 7.00f);
  assert(snap.update(7.30f) == 7.30f);
  assert(snap.update(7.31f) == 7.31f);
  assert(snap.update(7.30f) == 7.30f);

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

  // bad pass never starts a published change
  SensorFilters::JumpGuard badPass;
  SensorFilters::SnapValue badPassSnap(0.02f);
  badPassSnap.update(badPass.update(7.00f));
  assert(badPassSnap.update(badPass.update(10.00f)) == 7.00f);
  assert(badPassSnap.update(badPass.update(7.10f)) == 7.00f);

  // confirmed breach reaches the published value on the confirming pass
  SensorFilters::JumpGuard realBreach;
  SensorFilters::SnapValue realBreachSnap(0.02f);
  realBreachSnap.update(realBreach.update(7.00f));
  realBreachSnap.update(realBreach.update(10.00f));
  assert(realBreachSnap.update(realBreach.update(10.10f)) == 10.10f);
```

Remove the final noisy-EMA integration loop; the `quiet` loop above replaces it.

- [ ] **Step 2: Run the test to verify it fails**

Run: `g++ -std=c++11 -Icrx_pulse tests/sensor_filters_test.cpp -o "$env:TEMP\sensor_filters_test.exe"`
Expected: compile failure naming `'SnapValue' is not a member of 'SensorFilters'`.

- [ ] **Step 3: Implement `SnapValue` and remove the unused EMA**

In `crx_pulse/sensor_filters.h`, delete `class StableValue` (lines 32–70) and add `class SnapValue` in its place with the constructor, `update`, `value`, `reset`, and private fields `minChange_`, `published_`, `initialized_`. Use a manual absolute-difference comparison; do not include `<cmath>`. Update the file's opening comment so it no longer describes an EMA. Leave `JumpGuard` unchanged.

- [ ] **Step 4: Run the test to verify it passes**

Run:
`g++ -std=c++11 -Icrx_pulse tests/sensor_filters_test.cpp -o "$env:TEMP\sensor_filters_test.exe"`
then `& "$env:TEMP\sensor_filters_test.exe"`
Expected: compile succeeds and the executable exits 0.

- [ ] **Step 5: Commit**

```bash
git add crx_pulse/sensor_filters.h tests/sensor_filters_test.cpp docs/superpowers/specs/2026-11-26-ph-snap-on-confirm-design.md docs/superpowers/plans/2026-11-26-ph-snap-on-confirm.md
git commit -m "fix: snap pH to confirmed reading instead of ramping"
```

---

### Task 2: Wire the snap filter into the sketch

**Files:**
- Modify: `crx_pulse/crx_pulse.ino:605-613` (filter config and instances)
- Modify: `crx_pulse/crx_pulse.ino:1909-1914` (comment and publish line)

**Interfaces:**
- Consumes: `SensorFilters::SnapValue(float)` from Task 1.
- Produces: `phFilter` as a `SnapValue` instance; call shape `phFilter.update(phJumpGuard.update(...))` unchanged.

- [ ] **Step 1: Change the filter config and instance**

Replace:

```cpp
  static constexpr float SMOOTHING = 0.15f;
  static constexpr float MIN_CHANGE = 0.02f;
```

with only `static constexpr float MIN_CHANGE = 0.02f;`, and replace:

```cpp
SensorFilters::StableValue phFilter(PhFilterConfig::SMOOTHING,
                                     PhFilterConfig::MIN_CHANGE);
```

with `SensorFilters::SnapValue phFilter(PhFilterConfig::MIN_CHANGE);`. Edit only those lines; do not touch the calibration block at lines 565–601.

- [ ] **Step 2: Update the pH section comment and keep the call shape**

In the `// ---------------- pH (3-point) ----------------` comment, change the clause about the EMA so it describes the median, the jump guard, and the snap-to-confirmed publish. Leave the call at line 1914 exactly as:

```cpp
  pH = phFilter.update(phJumpGuard.update(PhCalibration::valueOf(phCh.sensorVolts)));
```

- [ ] **Step 3: Verify host tests and scope**

Run all host tests:

```powershell
Get-ChildItem tests/*.cpp | ForEach-Object { $name = $_.BaseName; $exe = "$env:TEMP\$name.exe"; g++ -std=c++11 -Icrx_pulse $_.FullName -o $exe; if ($LASTEXITCODE -ne 0) { "$name : COMPILE FAIL"; return }; & $exe; if ($LASTEXITCODE -eq 0) { "$name : PASS" } else { "$name : FAIL" } }
```

Expected: `offline_schedule_store`, `schedule_dates`, `sensor_filters`, `turbidity_calibration` all `PASS`.

Run `git diff -U0 -- crx_pulse/crx_pulse.ino | Select-String '^@@'` and confirm no hunk header starts inside the credential lines near the top of the file (17–19). Run `git diff --check` and confirm no new whitespace error.

- [ ] **Step 4: Commit**

```bash
git add crx_pulse/crx_pulse.ino
git commit -m "Updated: Publish pH as soon as the reading is confirmed"
```

---

### Task 3: Review and push

**Files:** none.

**Interfaces:** Consumes the commits from Tasks 1 and 2.

- [ ] **Step 1: Fresh whole-change review**

Have a fresh reviewer check the two commits against the spec and the Review Focus list. It must not open or print `crx_pulse/secrets.h` or the credential block. If it finds a Critical or Important issue, fix it before pushing.

- [ ] **Step 2: Push**

Run: `git status -sb` — confirm the branch is `main` and there are no staged files.
Run: `git push origin main`
Expected: the two new commits land on `origin/main`; report the resulting short hashes.
