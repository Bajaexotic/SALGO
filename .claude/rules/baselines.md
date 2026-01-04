# AMT Framework - Baseline Systems

## Baseline Decision Gate (Dec 2024)

| SSOT Owner | Location |
|------------|----------|
| **`BaselineDecisionGate`** | `AMT_BaselineGate.h` (centralized baseline queries) |

**PURPOSE:** Single access point for all baseline-derived decision inputs.
All consumers query through the gate, not directly from baseline stores.

### 4 Baseline Systems Unified

| System | Location | Data Type |
|--------|----------|-----------|
| `EffortBaselineStore` | AMT_Snapshots.h | Phase-bucketed bar metrics (vol_sec, trades_sec, delta_pct, bar_range, avg_trade_size, abs_close_change) |
| `SessionDeltaBaseline` | AMT_Snapshots.h | Phase-bucketed session delta ratios |
| `LiquidityEngine` | AMT_Liquidity.h | Kyle's 4-component model (Stress, Resilience local; Depth, Spread via DOMWarmup) |
| `DOMWarmup` | AMT_Snapshots.h | Phase-bucketed DOM metrics (stack, pull, depth, halo, spread) - SSOT for LiquidityEngine depth/spread |

### 6 Decision Domains

| Decision | Method | Output |
|----------|--------|--------|
| Extreme Delta | `QueryExtremeDelta()` | `ExtremeDeltaInput` (bar + session percentiles) |
| Market Composition | `QueryMarketComposition()` | `MarketCompositionInput` (vol + trades percentiles) |
| Range Classification | `QueryRangeClassification()` | `RangeClassificationInput` (COMPRESSED/NORMAL/EXPANDED) |
| Directional Travel | `QueryDirectionalTravel()` | `DirectionalTravelInput` (abs change percentile) |
| Liquidity State | `WrapLiquidityResult()` | `LiquidityStateInput` (VOID/THIN/NORMAL/THICK) |
| Depth Percentile | `QueryDepthPercentile()` | `DepthPercentileInput` (core/halo/spread) |

### Usage Pattern

```cpp
// Initialize gate with baseline references
BaselineDecisionGate gate(&effortStore, &sessionDeltaBaseline,
                          &liquidityEngine, &domWarmup);
gate.SetPhase(currentPhase);

// Query with validity checking (NO-FALLBACK contract)
auto deltaInput = gate.QueryExtremeDelta(barDeltaPct, sessionDeltaPct);
if (deltaInput.IsReady()) {
    if (deltaInput.IsExtreme(85.0)) {
        // Both bar AND session delta exceed 85th percentile
        isExtremeDeltaConfirmed = true;
    }
}

auto rangeInput = gate.QueryRangeClassification(barRangeTicks);
if (rangeInput.IsReady()) {
    switch (rangeInput.GetRegime()) {
        case RangeRegime::COMPRESSED: /* tight range */ break;
        case RangeRegime::EXPANDED:   /* wide range */  break;
        default: break;
    }
}
```

### NO-FALLBACK Contract

- Every output includes explicit `IsReady()` validity check
- Missing baselines produce "not ready" state, NOT fake values
- Consumers MUST check validity before using percentile values

**Common violation:** Calling percentile methods without checking `IsReady()` first.

---

## SSOT: Effort Signals (Rates vs Totals)

| SSOT Owner | Location |
|------------|----------|
| **`EffortSnapshot`** | `AMT_Snapshots.h` (full contract documented there) |

### Rate Signals (per-second intensity, from Numbers Bars Inputs 70-71)

```cpp
bidVolSec, askVolSec  // Volume at bid/ask PER SECOND (Input 70-71: NB SG53-54)
tradesSec             // Trades PER SECOND (derived)
deltaSec              // Delta PER SECOND (derived)
```

### Total Signals (per-bar aggregates)

```cpp
totalVolume           // Volume FOR THE BAR (Input 72: NB SG13)
delta                 // Net delta FOR THE BAR (derived)
maxDelta, cumDelta    // Max/cumulative delta (Inputs 74-75)
```

### Baseline Matching (MUST use correct units)

```cpp
vol_sec.percentile(bidVolSec + askVolSec)   // CORRECT: rates vs rates
total_vol.percentile(totalVolume)            // CORRECT: totals vs totals
total_vol.percentile(bidVolSec + askVolSec)  // WRONG: rates vs totals
```

### Conversion (when needed)

```cpp
estimatedTotal = rate * sc.SecondsPerBar;    // rate -> total
estimatedRate = total / sc.SecondsPerBar;    // total -> rate
```

**Common violation:** Comparing rates against total baselines or vice versa.

---

## SSOT: Confidence Scores

| SSOT Owner | Location |
|------------|----------|
| **Computed from snapshot** | `currentSnapshot.effort.*` and `currentSnapshot.liquidity.*` |

**Must sync to:**
```cpp
st->amtContext.confidence.deltaConsistency = <computed from closedBarDeltaPct>;  // CLOSED bar policy (Dec 2024)
st->amtContext.confidence.liquidityAvailability = <computed from closedBarDepth>;  // CLOSED bar policy (Dec 2024)
st->amtContext.confidence.liquidityAvailabilityValid = <true if baseline sufficient>;
```

### CLOSED BAR POLICY (Dec 2024)

- Both `deltaConsistency` and `liquidityAvailability` use the **closed bar** (curBarIdx - 1)
- Rationale: `sc.AskVolume`/`sc.BidVolume` and DOM arrays may lag for the forming bar
- Closed bars have complete, reliable data

### liquidityAvailability No-Fallback Policy (Dec 2024)

- Baseline requires `depth_mass_core.size() >= liquidityBaselineMinSamples` (default: 10)
- When baseline insufficient: `liquidityAvailabilityValid = false`, rate-limited error logged
- When DOM inputs not configured: `liquidityAvailabilityValid = false` (no error, just unavailable)
- `calculate_score()` excludes invalid metrics and renormalizes remaining weights
- Session stats log shows `LIQ=N/A` when invalid, `LIQ=0.XX` when valid

**Common violation:** Reading `amtContext.confidence.*` without computing from snapshot.
