# AutoLoop=0 Refactor Plan v2 (Refined)

## 1. Recovery State Baseline

### Current File State
| Property | Value |
|----------|-------|
| File | `AuctionSensor_v1.cpp` |
| SHA-256 | `26F7D3D83FFD893674DF16279B091DB4DAA47175D639BACBE396AA1C572D4F20` |
| Timestamp | `2025-12-27 12:56:57` |
| Lines | 4094 |
| AutoLoop | `1` (Sierra-managed iteration) |
| NOOP_BODY_TEST | `0` (disabled) |

### Backup File
| Property | Value |
|----------|-------|
| File | `AuctionSensor_v1.cpp.backup_20251227_1240` |
| SHA-256 | `560DD26FE4880228598D8FF146A5936D5BE24F83B427BEAAB2914F63FA0666F7` |

**ROLLBACK SAFETY**: Do not proceed with refactor stages until backup exists and SHA-256 is verified.

---

## 2. NOOP Body Test: Proof of Near-Zero Work

### Exact Code Block (Lines 1595-1612)
```cpp
    // ========================================================================
    // NO-OP BODY TEST: Early return to measure pure dispatch overhead
    // Enable by setting NOOP_BODY_TEST to 1 at top of file
    // PLACEMENT: Immediately after state allocation, BEFORE any:
    //   - Logging / string formatting
    //   - Input validation
    //   - CollectObservableSnapshot()
    //   - GetStudyArrayUsingID / VbP calls
    //   - map/vector operations
    // ========================================================================
#if NOOP_BODY_TEST
    // Minimal work: write zeros to subgraphs and return immediately
    // This isolates Sierra's AutoLoop dispatch cost from function body work
    sc.Subgraph[0][sc.Index] = 0.0f;
    sc.Subgraph[1][sc.Index] = 0.0f;
    sc.Subgraph[2][sc.Index] = 0.0f;
    return;
#endif
```

### Placement Verification

**Code BEFORE NOOP (lines 1556-1593)**:
- `GetPersistentPointer(1)` - state retrieval (required)
- `LastCallToFunction` check - cleanup (required)
- `new StudyState()` - allocation if null (required)
- `needsStateInit` flag setting (cheap bool ops)

**Code AFTER NOOP (lines 1614+)**:
- `sc.AddMessageToLog()` - logging (SKIPPED by NOOP)
- Input validation checks (SKIPPED by NOOP)
- `CollectObservableSnapshot()` (SKIPPED by NOOP)
- VbP/GetStudyArrayUsingID calls (SKIPPED by NOOP)
- All map/vector operations (SKIPPED by NOOP)

**Conclusion**: NOOP path executes only: state pointer retrieval + 3 float writes + return. This is valid near-zero work.

---

## 3. sc.Index Reference Count (Reconciled)

### Grep Command
```
grep -c "sc\.Index" AuctionSensor_v1.cpp
```

### Result: **162 references**

Previous "200+" was approximate from visual scan. Previous "159" was before NOOP relocation added 3 more references.

### Breakdown by Function Scope

| Location | Line Range | Count | Notes |
|----------|------------|-------|-------|
| CollectObservableSnapshot | 723-1270 | ~42 | Has `int idx` param but uses `sc.Index` internally |
| Main function body | 1332-4630 | ~120 | Direct usage in main study function |
| NOOP test block | 1608-1610 | 3 | Intentional for subgraph writes |

### High-Frequency Patterns
- `sc.BaseDateTimeIn[sc.Index]` - 15 occurrences
- `sc.Index == sc.ArraySize - 1` (isLiveBar) - 12 occurrences
- `sc.Index == 0` (init check) - 8 occurrences
- `sc.Subgraph[N][sc.Index]` - 11 occurrences
- `sc.Volume/High/Low/Close[sc.Index]` - 22 occurrences

---

## 4. Measurement Protocol

### Experiment 1: Isolation Baseline

**Objective**: Establish true per-study cost with minimal interference.

**Chart Setup** (DETERMINISTIC):
| Parameter | Value |
|-----------|-------|
| Symbol | ES (E-mini S&P 500 futures) |
| Timeframe | 1-minute bars |
| Date Range | Fixed historical: 2024-12-20 to 2024-12-26 (5 trading days) |
| Expected Bar Count | ~4000-5000 bars (verify with sc.ArraySize) |
| Replay | OFF |
| Live Ticks | DISABLED (disconnect data feed or use historical-only mode) |

**Required Dependency Studies** (minimum viable):
| Study | Why Required |
|-------|--------------|
| Volume by Price (VbP) | Input 20 - provides POC/VAH/VAL |
| Numbers Bars | Inputs 70-75 - provides delta/volume rates |

**Optional Studies** (remove if possible):
- VWAP (Inputs 50-54) - can be disabled
- Daily OHLC (Inputs 40-43) - can be disabled

**Measurement Table** (capture for EACH study):
| Study Name | Per-Study ms | Bar Count | Recalc Trigger |
|------------|--------------|-----------|----------------|
| AuctionSensor_v1 | _____ | _____ | Full Recalc via Recalculate |
| Volume by Price | _____ | _____ | (same) |
| Numbers Bars | _____ | _____ | (same) |
| **Chart Total** | _____ | _____ | |

### Experiment 2: NOOP Body Test

**Objective**: Isolate dispatch overhead by measuring with near-zero function body work.

**Procedure**:
1. Edit line 17: change `#define NOOP_BODY_TEST 0` to `#define NOOP_BODY_TEST 1`
2. Save file
3. In Sierra Chart: Analysis > Studies > Recalculate (or reload study)
4. Record measurements using same table format

**Measurement Table**:
| Study Name | Per-Study ms (NOOP) | Bar Count | Recalc Trigger |
|------------|---------------------|-----------|----------------|
| AuctionSensor_v1 | _____ | _____ | Full Recalc |
| Volume by Price | _____ | _____ | (same) |
| Numbers Bars | _____ | _____ | (same) |

**After measurement**: Revert `NOOP_BODY_TEST` to `0`.

---

## 5. Timing Reconciliation (Evidence-Based Only)

### What We Can Measure
| Metric | Source | Description |
|--------|--------|-------------|
| Sierra per-study ms | Studies window | Total time Sierra attributes to this study |
| Inside-function ms | Our `PERF_TIMING` timers | Time measured within study function body |

### What We Cannot Claim (Until NOOP Proves It)
- "Dispatch overhead" - requires NOOP experiment to prove
- "Memory management" - speculation without evidence
- "State restoration" - speculation without evidence

### Evidence-Based Statement Template
After experiments, state ONLY:
```
Experiment 1 (normal):
  - Sierra per-study ms: X
  - Inside-function ms (PERF): Y
  - Unmeasured delta: X - Y = Z

Experiment 2 (NOOP):
  - Sierra per-study ms: A
  - Inside-function ms: ~0 (by design)
  - Unmeasured delta: A

Interpretation:
  - If A > 500ms: External overhead dominates (A ms per full recalc)
  - If A < 100ms: Function body work dominates (Z - A ms of actual work)
  - Function body cost: X - A ms
```

---

## 6. Decision Framework

### After Experiments

| NOOP per-study ms | Interpretation | Decision |
|-------------------|----------------|----------|
| > 500ms (for ~5K bars) | External overhead dominates | Proceed with staged refactor |
| < 100ms | Function body work dominates | Optimize hot paths instead |
| 100-500ms | Mixed cost model | Evaluate ROI carefully |

### Go/No-Go Criteria
**PROCEED with refactor if ALL true**:
- [ ] NOOP per-study ms > 50% of normal per-study ms
- [ ] Backup file exists with verified SHA-256
- [ ] CollectObservableSnapshot idx conversion is < 50 changes

**ABANDON refactor if ANY true**:
- [ ] NOOP per-study ms < 20% of normal per-study ms (body work dominates)
- [ ] Experiment shows dependent study (VbP/NB) is the actual bottleneck
- [ ] Stage A.1 reveals > 50 helper functions need idx changes

---

## 7. Staged Refactor Plan (If Justified)

### Stage A: Top-Level Loop + Bulk Replace

**Scope**:
1. Change `sc.AutoLoop = 1` to `sc.AutoLoop = 0`
2. Add for loop wrapper
3. Bulk replace `sc.Index` → `BarIndex` in main function body ONLY

**What Changes**:
- Line 1345: `sc.AutoLoop = 0`
- Lines ~1595: Add `for (int BarIndex = sc.UpdateStartIndex; BarIndex < sc.ArraySize; BarIndex++) {`
- ~120 occurrences in main function: `sc.Index` → `BarIndex`
- End of function: Add closing `}`

**What Could Break**:
- Helper functions that use `sc.Index` internally (CollectObservableSnapshot has ~42 refs)
- Session accumulator logic depends on bar ordering

**Validation**:
- Compiles successfully
- No runtime errors on 100-bar test
- Subgraph outputs differ (expected - helpers still wrong)

### Stage A.1: Fix CollectObservableSnapshot

**Scope**: Replace ~42 `sc.Index` refs with `idx` parameter in CollectObservableSnapshot.

**What Could Break**:
- Subtle bugs if any reference missed
- Session accumulator timing

**Validation**:
- Add runtime assertion: `if (idx != sc.Index && sc.AutoLoop == 1) { /* error */ }`
- Under AutoLoop=1, assertion should never fire
- Under AutoLoop=0, outputs should match AutoLoop=1 baseline

### Stage B: A/B Compile-Time Toggle

**Scope**: Add `#define USE_MANUAL_LOOP 0/1` to enable side-by-side comparison.

**Validation**:
- Build with USE_MANUAL_LOOP=0, record subgraph outputs for 500 bars
- Build with USE_MANUAL_LOOP=1, record subgraph outputs for 500 bars
- Diff outputs: must be identical within float tolerance

### Stage C: Expand to Remaining Helpers

**Scope**: Only after Stage B validates, convert remaining helper functions.

**Method**: One file at a time with A/B validation after each.

---

## 8. Validation Checkpoints

| Checkpoint | Criteria | Method |
|------------|----------|--------|
| V0 | Backup exists | Verify SHA-256 of backup file |
| V1 | Compiles | Reload study in SC, no DLL errors |
| V2 | No runtime errors | SC message log shows no [ERROR] |
| V3 | NOOP experiment complete | Measurements recorded in tables |
| V4 | Go/No-Go decision made | Based on NOOP results |
| V5 | Subgraph parity (if proceeding) | A/B outputs match |
| V6 | Performance gain > 30% | Sierra per-study ms reduced |

---

## 9. Next Actions

**User must execute in order**:

1. **Validate baseline** - Load study in Sierra Chart, confirm no CUMΔ-REWIND log spam
2. **Run Experiment 1** - Fill in measurement table with normal operation
3. **Run Experiment 2** - Set `NOOP_BODY_TEST=1`, fill in measurement table
4. **Report measurements** - Provide both tables for go/no-go decision

**Do NOT proceed to refactor stages until experiments are complete and decision is made.**
