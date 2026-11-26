# pH Recalibration — Execution Ledger

Date: 2026-11-26

- [x] Read approved spec and plan.
- [x] Confirmed measured anchors: acid 3.970 V, neutral 3.715 V, base 2.680 V.
- [x] Task 1: extract `PhCalibration` and pin anchors.
- [x] Task 2: make the sketch use the calibration header.
- [x] Task 3: update the mobile calibration mirror.
- [x] Task 4: review and push both repos.
  - Fresh reviews found no Critical or Important issues.
  - Replaced `assert()` checks with unconditional `CHECK` returns so `-DNDEBUG` cannot skip them.
  - Host tests pass in normal and `-DNDEBUG` builds.
