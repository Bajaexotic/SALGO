# Verification Queue
<!-- MAX 50 LINES. When full: archive PASS items to artifacts/verified_log.md -->

## V1: Session-Start Freshness Invariant — PASS
Gate exists at `AMT_Analytics.h:206-211`; reset at `:278`.

## V2: No Double-Lag — PASS
Single `Update()` method at `AMT_Analytics.h:192-266`; no external smoothing.

## V3: Prior Boundedness — PASS
Formula `priorMass / (sessionBars + priorMass)` at `:217-218` confirms decay.

---

## V5: Compile — ACTIVE
**How to verify**: Compile via Sierra Chart DLL system.
**Evidence**: `artifacts/build_log.txt`
**PASS**: No errors, DLL produced.
**FAIL**: Compile errors (capture to evidence file).

---

## V4: Runtime Logs — BLOCKED (until V5 PASS)
**How to verify**: Run study on test chart, capture log output.
**Evidence**: `artifacts/runtime_log.txt`
**PASS**: Log shows:
- `[MKTSTATE-PRIOR]` messages at session roll
- `Market State: UNDEFINED (sess=N, need 20)` for early bars
- `Market State: BALANCE/IMBALANCE (sess=N, bal=X%)` after sufficiency
**FAIL**: Missing or malformed log entries.

---

## Next Actions
1. Complete V5 (ACTIVE)
2. If V5 PASS → unblock V4
3. If V4 PASS → update STATUS to COMPLETE
