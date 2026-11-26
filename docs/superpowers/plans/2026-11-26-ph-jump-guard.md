# pH Jump Guard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop the device's published pH from ramping to 9–10 by rejecting unconfirmed jumps before the EMA.

**Architecture:** A small stateful `SensorFilters::JumpGuard` in the existing `sensor_filters.h` holds the last accepted pH and one candidate. It runs between the 15-sample median conversion and the existing `StableValue` EMA, so one bad pass cannot start a slow ramp while ADC noise is still smoothed. No new dependencies.

**Tech Stack:** Arduino C++ (ESP32), host-side `g++` tests.

**Spec:** `docs/superpowers/specs/2026-11-26-ph-jump-guard-design.md`

## Global Constraints

- Guard threshold is exactly `0.5f`; constant name is exactly `MAX_IMMEDIATE_CHANGE`.
- Do not change `SAMPLE_COUNT = 15`, `SMOOTHING = 0.15f`, or `MIN_CHANGE = 0.02f`.
- Never open, print, edit, stage or commit `crx_pulse/secrets.h` or the credential block near the top of `crx_pulse/crx_pulse.ino`.
- Do not stage or commit anything. This overrides the usual final commit step in every task.
- Leave the existing uncommitted work untouched: `Wiring Diagram.fzz`, `crx_pulse/HARDWARE_WIRING.md`, `crx_pulse_test/crx_pulse_test.ino`, and all untracked files except `crx_pulse/sensor_filters.h` and `tests/sensor_filters_test.cpp` as listed below.
- No new files inside `crx_pulse/`; no new dependencies.

## Review Focus

- One bad high pass `7 → 10 → 7` must keep the published value at `7.00` on all three passes — integration test in Task 1, `bad pass never starts the ramp`.
- Two agreeing high passes `7 → 10 → 10.1` must let the EMA start moving on the third pass, so a real breach is not hidden — integration test in Task 1, `confirmed breach reaches the EMA`.
- Ordinary drift `7 → 7.3` must pass on the first sample — assertion in Task 1, `small drift passes immediately`.
- The first sample after boot or `reset()` must be authoritative even if it is far from the previous value — assertion in Task 1, `reset makes the next sample authoritative`.
- Existing ADC-noise behavior must survive the extra guard — integration test in Task 1, `noise test still passes through the guard`.

---

### Task 1: Jump guard helper and host tests

**Files:**
- Modify: `crx_pulse/sensor_filters.h` (add the class before the namespace close at line 72)
- Test: `tests/sensor_filters_test.cpp`

**Interfaces:**
- Consumes: `SensorFilters::StableValue` (existing, `float update(float)`, `float value() const`).
- Produces: `SensorFilters::JumpGuard` with `static constexpr float MAX_IMMEDIATE_CHANGE = 0.5f`, `float update(float sample)`, and `void reset()`.

- [ ] **Step 1: Write the failing tests**

Append these assertions to `tests/sensor_filters_test.cpp` immediately before `return 0;`:

```cpp
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
```

- [ ] **Step 2: Run the tests to verify they fail**

Run from `crx_pulse_dev`:

```powershell
g++ -std=c++11 -Icrx_pulse tests/sensor_filters_test.cpp -o "$env:TEMP\sensor_filters_test.exe"
& "$env:TEMP\sensor_filters_test.exe"
```

Expected: compile failure naming `JumpGuard` as not a member of `SensorFilters`.

- [ ] **Step 3: Implement `SensorFilters::JumpGuard`**

In `crx_pulse/sensor_filters.h`, add the class after `StableValue` and before `namespace SensorFilters` closes. It holds a last accepted value, an optional candidate, and an initialized flag. Rules:

- First `update`, or any call after `reset()`: accept and return the sample.
- Change from the accepted value within `MAX_IMMEDIATE_CHANGE` (inclusive): accept, clear any candidate, return the sample.
- If a candidate exists and the new sample is within `MAX_IMMEDIATE_CHANGE` of it: accept the new sample, clear the candidate, return it.
- Otherwise: store the sample as the candidate and return the accepted value.
- `reset()` clears the accepted value, candidate and initialized flag.

- [ ] **Step 4: Run the tests to verify they pass**

Run the same two commands as Step 2. Expected: no compiler output and no assertion failure (exit code 0).

- [ ] **Step 5: Do not commit**

No `git add`, no `git commit`. Leave the change in the working tree.

### Task 2: Apply the guard to the device's pH reading

**Files:**
- Modify: `crx_pulse/crx_pulse.ino:605-612` (instance beside `phFilter`), `:1913` (single call site)

**Interfaces:**
- Consumes: `SensorFilters::JumpGuard` from Task 1.
- Produces: no new public API; global `pH` now comes from guard → EMA.

- [ ] **Step 1: Add the guard instance**

Immediately after the `phFilter` declaration at lines 611–612, add:

```cpp
SensorFilters::JumpGuard phJumpGuard;
```

Use a narrow edit so no line in the credential block near the top of the file is touched.

- [ ] **Step 2: Put the guard before the EMA**

Replace line 1913 with:

```cpp
  pH = phFilter.update(phJumpGuard.update(PhCalibration::valueOf(phCh.sensorVolts)));
```

Do not change the median sampling at line 1877 or any calibration constant.

- [ ] **Step 3: Verify**

Run the host tests again (same two commands as Task 1, Step 2). Expected: exit code 0.

No Arduino CLI is installed, so the sketch itself cannot be compiled here. Review the exact changed hunks with:

```powershell
git diff -- crx_pulse/sensor_filters.h crx_pulse/crx_pulse.ino tests/sensor_filters_test.cpp
git diff --check
git status --short
```

Expected: only the `sensor_filters.h`, `crx_pulse.ino` pH hunks and `tests/sensor_filters_test.cpp` changed by this plan; `Wiring Diagram.fzz` and the other pre-existing changes are untouched; no whitespace errors; `secrets.h` does not appear.

- [ ] **Step 4: Do not commit**

No `git add`, no `git commit`. Leave all changes in the working tree.
