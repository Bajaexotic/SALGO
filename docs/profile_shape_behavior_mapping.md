# Profile Shape Behavior Mapping Specification v1.2

> **Document Status**: Ship-ready specification
> **SSOT**: `AMT_ProfileShape.h` (classifier), `amt_core.h` (ProfileShape enum)
> **Scope**: Maps frozen profile shape labels to behavioral hypotheses (Continuation vs Mean-Reversion)

---

## 1. Symbol Dictionary (Variable Hygiene)

All observable variables used in this specification with their SSOT sources:

### 1.1 Profile Metrics (Frozen at Classification Time)

| Symbol | Name | SSOT Source | Range | Definition |
|--------|------|-------------|-------|------------|
| `x` | POC Position | `ProfileFeatures.pocInRange` | [0, 1] | `(POC - P_lo) / R` where R = profile range |
| `w` | Breadth | `ProfileFeatures.breadth` | (0, 1] | `W_va / R` (VA width fraction) |
| `a` | Asymmetry | `ProfileFeatures.asymmetry` | [-0.5, 0.5] | `(POC - VA_mid) / W_va` |
| `k` | Peakiness | `ProfileFeatures.peakiness` | [1, ∞) | `POC_vol / VA_mean` |
| `S` | Shape Label | `ShapeClassificationResult.shape` | ProfileShape enum | Frozen classification output |

### 1.2 Structure References (Frozen at Classification Time)

| Symbol | Name | SSOT Source | Range | Definition |
|--------|------|-------------|-------|------------|
| `POC_0` | Session POC | `SessionVolumeProfile.session_poc` | Price | POC at freeze time `t_freeze` |
| `VAH_0` | Session VAH | `SessionVolumeProfile.session_vah` | Price | VAH at freeze time `t_freeze` |
| `VAL_0` | Session VAL | `SessionVolumeProfile.session_val` | Price | VAL at freeze time `t_freeze` |
| `VA_mid_0` | VA Midpoint | Derived | Price | `(VAH_0 + VAL_0) / 2` |
| `W_va` | VA Width | Derived | Ticks or Price | `VAH_0 - VAL_0` (frozen) |
| `R_0` | Profile Range | Derived | Ticks or Price | `profileHigh_0 - profileLow_0` (frozen) |

### 1.3 Live Price Observables

| Symbol | Name | SSOT Source | Definition |
|--------|------|-------------|------------|
| `P_t` | Current Price | `sc.Close[sc.Index]` | Live bar close price |
| `P_hi` | Bar High | `sc.High[sc.Index]` | Current bar high |
| `P_lo` | Bar Low | `sc.Low[sc.Index]` | Current bar low |

### 1.4 Classification Thresholds (Constants from AMT_ProfileShape.h)

| Symbol | Value | Purpose |
|--------|-------|---------|
| `C_MIN` | 0.35 | x < C_MIN → B territory |
| `C_MAX` | 0.65 | x > C_MAX → P territory |
| `W_THIN` | 0.40 | w ≤ W_THIN → THIN_VERTICAL |
| `W_BAL` | 0.50 | w ≥ W_BAL required for BALANCED |
| `K_MOD` | 1.5 | Moderate peakiness threshold |
| `K_SHARP` | 2.0 | Sharp peakiness threshold |
| `A_BAL` | 0.10 | \|a\| ≤ A_BAL → symmetric |
| `A_D` | 0.15 | \|a\| ≥ A_D → asymmetric (D_SHAPED) |

### 1.5 Observation Parameters

| Symbol | Name | Default | Definition |
|--------|------|---------|------------|
| `t_freeze` | Freeze Bar | Bar 30 of session | Bar index at which shape and references are frozen |
| `t_end` | Session End Bar | Last bar of session | Bar index at which observation window closes |
| `t_brk` | Trigger Bar | N/A (computed) | First bar after `t_freeze` where breakout condition triggers |
| `N` | Hold Bars | 3 | Minimum consecutive bars price must remain beyond boundary to confirm breakout |
| `tolerance` | Return Tolerance | `0.25 × W_va` | Distance from `VA_mid_0` considered "returned to center" |

---

## 2. Outcome Labels (Observable Definitions)

### 2.1 Primary Outcomes

| Label | Code | Definition | Observation Window |
|-------|------|------------|-------------------|
| **O1: CONTINUATION_UP** | `+C` | See §2.2 for formal definition | Bars `(t_freeze, t_end]` |
| **O2: CONTINUATION_DN** | `-C` | See §2.2 for formal definition | Bars `(t_freeze, t_end]` |
| **O3: MEAN_REVERT_FROM_HIGH** | `MR_H` | Touched `VAH_0` then returned to `VA_mid_0 ± tolerance` | Bars `(t_freeze, t_end]` |
| **O4: MEAN_REVERT_FROM_LOW** | `MR_L` | Touched `VAL_0` then returned to `VA_mid_0 ± tolerance` | Bars `(t_freeze, t_end]` |
| **O5: RANGE_BOUND** | `RB` | No sustained breakout occurred (see §2.2) | Bars `(t_freeze, t_end]` |

### 2.2 Formal Outcome Definitions

**Price field convention**: All boundary triggers use bar extremes (`P_hi`, `P_lo`), not closes. This captures intrabar spikes and is consistent across all outcomes.

**O1 (CONTINUATION_UP) - Formal Definition**:
1. **Trigger bar** `t_brk`: The first bar after `t_freeze` where `P_hi[t_brk] >= VAH_0`.
2. **Hold condition**: For all bars in `{t_brk+1, t_brk+2, ..., t_brk+N}`, require `P_lo[i] >= VAH_0`.
3. **Completion**: O1 is "completed" at bar `t_brk+N` iff the hold condition is satisfied for all N bars.
4. **Failure**: If any bar `i` in the hold window has `P_lo[i] < VAH_0`, the breakout attempt fails. A new trigger bar may occur later.

**O2 (CONTINUATION_DN) - Formal Definition**:
1. **Trigger bar** `t_brk`: The first bar after `t_freeze` where `P_lo[t_brk] <= VAL_0`.
2. **Hold condition**: For all bars in `{t_brk+1, t_brk+2, ..., t_brk+N}`, require `P_hi[i] <= VAL_0`.
3. **Completion**: O2 is "completed" at bar `t_brk+N` iff the hold condition is satisfied for all N bars.
4. **Failure**: If any bar `i` in the hold window has `P_hi[i] > VAL_0`, the breakout attempt fails. A new trigger bar may occur later.

**O3 (MEAN_REVERT_FROM_HIGH) - Formal Definition**:
1. **Touch**: A bar exists where `P_hi >= VAH_0`.
2. **Return**: A subsequent bar exists where `|P_t - VA_mid_0| <= tolerance`.
3. **Completion**: O3 is "completed" when both touch and return occur, in that order.

**O4 (MEAN_REVERT_FROM_LOW) - Formal Definition**:
1. **Touch**: A bar exists where `P_lo <= VAL_0`.
2. **Return**: A subsequent bar exists where `|P_t - VA_mid_0| <= tolerance`.
3. **Completion**: O4 is "completed" when both touch and return occur, in that order.

**O5 (RANGE_BOUND) - Formal Definition**:
O5 is the residual outcome when no O1-O4 completes by session end. Formally:
- No O1 completed (no sustained UP breakout)
- No O2 completed (no sustained DN breakout)
- No O3 completed (no touch-and-return from high)
- No O4 completed (no touch-and-return from low)

**Note**: O5 permits boundary touches and even brief boundary breaches, provided no *sustained* breakout (O1/O2) or *completed* mean-reversion (O3/O4) occurs. This resolves the same-bar collision case: a bar spanning both VAH and VAL is a spike, not a sustained break, hence O5.

### 2.3 Edge Case Handling

| Condition | Resolution |
|-----------|------------|
| **Touch definition** | A "touch" of `VAH_0` occurs when `P_hi >= VAH_0`. A "touch" of `VAL_0` occurs when `P_lo <= VAL_0`. |
| **Same-bar collision** | If `P_hi >= VAH_0 AND P_lo <= VAL_0` in a single bar: this is a spike, not a sustained break. Neither O1 nor O2 can complete from this bar (hold condition would fail immediately). Classify as O5 if no other outcome completes. |
| **Tolerance collision** | Impossible if `W_va > 2 × tolerance`. If VA narrow (`W_va <= 2 × tolerance`), any touch of a boundary satisfies return condition → classify based on which boundary was touched first. |
| **Multiple breakout attempts** | Each failed breakout resets; next trigger bar starts a new attempt. First *completed* outcome wins. |
| **Session ends before hold completes** | If `t_brk` exists but `t_brk + N > t_end`, the breakout attempt is incomplete → UNRESOLVED. |
| **Session ends with no events** | If no touch of VAH_0 or VAL_0 occurred → O5 (trivially range-bound). |
| **Session ends after touch but before return** | Touched a boundary but never returned to VA_mid → UNRESOLVED. |
| **UNRESOLVED handling** | UNRESOLVED observations are excluded from hit-rate numerator AND denominator. Included in stability metrics only. |

### 2.4 Parameters

| Parameter | Default | Rationale |
|-----------|---------|-----------|
| `N` (hold bars) | 3 | Minimum consecutive bars beyond boundary to confirm sustained breakout |
| `tolerance` | 0.25 × `W_va` | Distance from `VA_mid_0` considered "returned to center" |
| `t_end` | Session close bar | Last bar of observation window |

---

## 3. Shape → Hypothesis Mapping

### 3.1 Decision Table

| Shape (S) | Primary Hypothesis | Target (Frozen Ref) | Invalidation (Frozen Ref) | Notes |
|-----------|-------------------|---------------------|---------------------------|-------|
| **NORMAL_DISTRIBUTION** | Mean-Reversion (O3/O4) | `VA_mid_0` | `P_t > VAH_0 + 0.5×W_va` OR `P_t < VAL_0 - 0.5×W_va` | — |
| **D_SHAPED** (a > 0) | Mean-Reversion from high (O3) | `VA_mid_0` | `P_t > VAH_0 + 0.5×W_va` | — |
| **D_SHAPED** (a < 0) | Mean-Reversion from low (O4) | `VA_mid_0` | `P_t < VAL_0 - 0.5×W_va` | — |
| **BALANCED** | Range-Bound (O5) | Stay within `[VAL_0, VAH_0]` | `P_t` outside VA for N bars | — |
| **P_SHAPED** | Continuation Up (O1) | Above `VAH_0` | `P_t < POC_0` | — |
| **B_SHAPED** | Continuation Down (O2) | Below `VAL_0` | `P_t > POC_0` | — |
| **THIN_VERTICAL** | Continuation (O1 or O2) | See §3.3 | See §3.3 | Requires trend direction input |
| **DOUBLE_DISTRIBUTION** | See §3.2 | See §3.2 | See §3.2 | Requires cluster analysis |
| **UNDEFINED** | No hypothesis | N/A | N/A | Skip - classifier abstained |

### 3.2 DOUBLE_DISTRIBUTION Sub-Cases

DOUBLE_DISTRIBUTION requires additional context from `ProfileFeatures.hvnClusters`.

**Cluster selection**: Use the two clusters with largest `totalVolume`. If fewer than 2 clusters exist, reclassify as UNDEFINED.

**Cluster membership**: `POC_0` is "in" cluster `C` iff `C.startTick <= POC_0_tick <= C.endTick`, where `POC_0_tick = PriceToTicks(POC_0, tickSize)`.

**Upper/Lower assignment**:
- Upper cluster `C_u`: the selected cluster with `centerTick > VA_mid_0_tick`
- Lower cluster `C_l`: the selected cluster with `centerTick < VA_mid_0_tick`
- If both clusters are on the same side of `VA_mid_0`, treat as ambiguous → UNDEFINED.

| Sub-Case | Condition | Hypothesis | Target | Invalidation |
|----------|-----------|------------|--------|--------------|
| **DD_UPPER** | `POC_0` in `C_u` | Mean-Revert to `C_l` | `C_l.centerTick × tickSize` | `P_hi > C_u.endTick × tickSize` |
| **DD_LOWER** | `POC_0` in `C_l` | Mean-Revert to `C_u` | `C_u.centerTick × tickSize` | `P_lo < C_l.startTick × tickSize` |
| **DD_BALANCED** | `POC_0` between clusters (not in either) | Range-Bound in valley | Stay between `C_l.endTick` and `C_u.startTick` | `P_hi > C_u.endTick × tickSize` OR `P_lo < C_l.startTick × tickSize` |

### 3.3 THIN_VERTICAL Dependency

THIN_VERTICAL classification indicates a trend day but does NOT encode trend direction. This spec does not define trend direction detection.

**Gated behavior**: Consumers must supply `trendDirection ∈ {UP, DOWN}` from an external source (e.g., first-hour range break, prior close comparison). Without this input, THIN_VERTICAL generates no hypothesis (treated as UNDEFINED for behavior mapping purposes).

When `trendDirection` is supplied:
- `trendDirection = UP` → Hypothesis: O1 (CONTINUATION_UP), Target: above `VAH_0`, Invalidation: `P_lo < POC_0`
- `trendDirection = DOWN` → Hypothesis: O2 (CONTINUATION_DN), Target: below `VAL_0`, Invalidation: `P_hi > POC_0`

---

## 4. Timing Constraints (No Lookahead)

### 4.1 Freeze Points

| Event | What Freezes | When |
|-------|--------------|------|
| Shape Classification | `S`, `x`, `w`, `a`, `k` | At `t_freeze` (configurable, default: bar 30 of session) |
| Reference Levels | `POC_0`, `VAH_0`, `VAL_0`, `W_va`, `R_0` | Same as classification freeze |
| Outcome Observation | N/A (live) | Begins after freeze, ends at session close |

### 4.2 Timing Invariants

1. **No future data**: Hypothesis generated at `t_freeze` uses only data from bars `[0, t_freeze]`
2. **No reference drift**: `POC_0`, `VAH_0`, `VAL_0`, `W_va`, `R_0` are snapshots; live profile changes do NOT update targets
3. **Outcome observation**: Begins strictly after `t_freeze`; bars before freeze are training data only

### 4.3 Session Boundary Handling

| Boundary | Action |
|----------|--------|
| RTH → Globex | Force-finalize any open observation as UNRESOLVED. Algorithm: if no outcome (O1-O5) has completed, set `outcome = UNRESOLVED`. |
| Globex → RTH | New session begins. Reset freeze cycle: new `t_freeze`, new frozen references. |
| Chart recalc | Recompute from historical bars. Freeze points are deterministic (same bar index yields same frozen values). |

**UNRESOLVED disposition**: UNRESOLVED is not an outcome label (O1-O5). It indicates the observation window closed before any outcome could be determined. UNRESOLVED observations are:
- Excluded from hit-rate numerator AND denominator
- Included in stability metrics (shape classification still occurred)
- Logged separately for diagnostic purposes

---

## 5. Separation of Concerns

### 5.1 Structural Signal (Shape Classifier Output)

The shape classifier provides a **structural signal only**:
- Input: Histogram + VA levels
- Output: `ProfileShape` enum + confidence
- Invariant: Same inputs → same output (deterministic)

The classifier does NOT consider:
- Current price relative to levels
- Time of day
- Volume/delta flow
- Prior session context

### 5.2 Context Filters (Applied by Consumer)

Consumers of the shape signal may apply context filters:

| Filter | Purpose | Owned By |
|--------|---------|----------|
| Session phase gate | Different behavior in IB vs Mid-Session | `SessionPhaseCoordinator` |
| Flow confirmation | Delta/volume confirming shape direction | `EffortSnapshot` |
| Prior session bias | Prior day's shape influences today | `PriorSessionNode` |
| Volatility adjustment | Widen/tighten targets based on ATR | Consumer logic |

### 5.3 Boundary: What This Spec Covers

| In Scope | Out of Scope |
|----------|--------------|
| Shape → Hypothesis mapping | Entry/exit signal generation |
| Frozen reference definitions | Position sizing |
| Outcome label definitions | Risk management |
| Falsification criteria | Order execution |

---

## 6. Validation Plan

### 6.1 Hypothesis Testing Framework

For each shape class, we test:
```
H₀: Shape S has no predictive value for outcome O
H₁: Shape S predicts outcome O with accuracy > random baseline
```

### 6.2 Metrics

| Metric | Definition | Target |
|--------|------------|--------|
| Hit Rate | `count(predicted_outcome == actual_outcome) / count(resolved)` | > baseline (see §6.3) |
| Shape Stability | `count(shape_unchanged_after_freeze) / total` | See §6.3 for threshold |
| Edge Ratio | `mean(winner_magnitude) / mean(loser_magnitude)` | > 1.0 |

### 6.3 Falsification Criteria

A shape-hypothesis mapping is **falsified** if any of the following hold over 100+ resolved (non-UNRESOLVED) observations:

1. **No lift over random**: Hit rate ≤ `1/K` where K = number of possible outcomes for that shape's hypothesis. For mean-reversion hypotheses (O3/O4), K=2. For continuation (O1/O2), K=2. For range-bound (O5), K=1 (always "correct" if triggered, so use edge ratio instead).

2. **Unstable freeze**: Shape classification changes after `t_freeze` in more than `2σ` of sessions, where `σ` is the standard error of the stability rate. Equivalently: if 95% confidence interval for stability rate includes values < 0.85.

3. **Negative edge**: `invalidation_count > target_count` (ratio < 1.0). This is a relative criterion, not absolute.

**Rationale**: These criteria avoid smuggling arbitrary absolute thresholds. Criterion 1 uses the mathematically correct random baseline. Criterion 2 uses statistical significance. Criterion 3 is purely relative.

### 6.4 Test Cases (Per Shape)

| Shape | Test Scenario | Expected Outcome | Pass Criteria |
|-------|---------------|------------------|---------------|
| NORMAL | Price at VAH at freeze | O3 (MR_H) | Returns to VA_mid within session |
| NORMAL | Price at VAL at freeze | O4 (MR_L) | Returns to VA_mid within session |
| P_SHAPED | Price above POC | O1 (+C) | Breaks and holds above VAH |
| B_SHAPED | Price below POC | O2 (-C) | Breaks and holds below VAL |
| BALANCED | Price mid-range | O5 (RB) | Stays within VA all session |
| D_SHAPED (a>0) | Price near VAH | O3 (MR_H) | Rejected from high, returns to center |
| THIN_VERTICAL | Strong trend day + trendDirection supplied | O1 or O2 | Continuation in trend direction |

---

## 7. Change Log

| Version | Date | Changes |
|---------|------|---------|
| v1.0 | 2024-12-29 | Initial spec. |
| v1.1 | 2024-12-29 | Variable hygiene: added `W_va`, `R_0`, `t_freeze` to dictionary. Edge cases: defined "touch", added same-bar collision rule, reconciled first-vs-last, specified UNRESOLVED handling. Decision table: removed undefined "+0.1 modifier", gated THIN_VERTICAL behind trend direction dependency. DOUBLE_DISTRIBUTION: added computable cluster membership and boundary definitions. Falsification: replaced absolute thresholds with relative lift criteria. |
| v1.2 | 2024-12-29 | **Outcome determinism**: Added §2.2 Formal Outcome Definitions with explicit trigger/hold logic for O1/O2. O1 trigger: `P_hi[t_brk] >= VAH_0`; hold: `P_lo[i] >= VAH_0` for N consecutive bars. O2 trigger: `P_lo[t_brk] <= VAL_0`; hold: `P_hi[i] <= VAL_0` for N consecutive bars. **O5 consistency**: Redefined O5 as residual (no O1-O4 completed), resolving conflict with same-bar collision. Same-bar collision now correctly implies O5 since hold condition fails immediately. **Symbol dictionary**: Added `t_end`, `t_brk` to §1.5. **Edge cases**: Added rules for session-ends-before-hold and session-ends-after-touch-before-return. |

---

## Appendix A: ProfileShape Enum Reference

From `amt_core.h`:

```cpp
enum class ProfileShape : int {
    UNDEFINED = 0,

    // Balance patterns
    NORMAL_DISTRIBUTION = 1,  // Sharp symmetric peak (k >= K_SHARP, |a| <= A_BAL)
    D_SHAPED = 2,             // Moderate peak with asymmetry (K_MOD <= k < K_SHARP, |a| >= A_D)
    BALANCED = 3,             // Wide acceptance, no dominant POC (k < K_MOD, w >= W_BAL, |a| <= A_BAL)

    // Imbalance patterns
    P_SHAPED = 4,             // POC high in range (x > C_MAX) - fat top, thin bottom
    B_SHAPED = 5,             // POC low in range (x < C_MIN) - fat bottom, thin top
    THIN_VERTICAL = 6,        // Narrow acceptance (w <= W_THIN) - trend day
    DOUBLE_DISTRIBUTION = 7   // Bimodal - two HVN clusters with LVN valley
};
```

## Appendix B: Classification Priority

From `AMT_ProfileShape.h:ClassifyProfileShape()`:

1. **THIN_VERTICAL** (w ≤ W_THIN) - Structural, fires first
2. **DOUBLE_DISTRIBUTION** (2+ HVN clusters with valley) - Bimodal detection
3. **P_SHAPED** (x > C_MAX) - POC high in range
4. **B_SHAPED** (x < C_MIN) - POC low in range
5. **NORMAL_DISTRIBUTION** (k ≥ K_SHARP ∧ |a| ≤ A_BAL) - Sharp symmetric
6. **D_SHAPED** (K_MOD ≤ k < K_SHARP ∧ |a| ≥ A_D) - Moderate asymmetric
7. **BALANCED** (k < K_MOD ∧ w ≥ W_BAL ∧ |a| ≤ A_BAL) - Flat wide
8. **UNDEFINED** - Falls in intentional gap regions
