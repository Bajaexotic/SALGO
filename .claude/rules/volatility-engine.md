# AMT Framework - Volatility Engine

## SSOT Location

| SSOT Owner | Location |
|------------|----------|
| **`VolatilityEngine`** | `AMT_Volatility.h` (StudyState owns `volatilityEngine` and `lastVolResult`) |

**PURPOSE:** Volatility is a context gate - it tells you whether triggers are trustworthy.

---

## 5 Key Questions the Engine Answers

1. **What regime am I in?** -> `VolatilityRegime` (COMPRESSION/NORMAL/EXPANSION/EVENT)
2. **Is the regime stable or transitioning?** -> `stabilityBars`, `isTransitioning`
3. **What range expansion should I expect?** -> `expectedRangeMultiplier`, `normalizedRange`
4. **Do I block or tighten requirements?** -> `TradabilityRules` struct
5. **What invalidates the estimate?** -> `VolatilityErrorReason` enum

---

## 4 Volatility Regimes

| Regime | Percentile | Trading Implications |
|--------|------------|---------------------|
| `COMPRESSION` | < P25 | Tight ranges, unreliable breakouts, prefer mean reversion |
| `NORMAL` | P25-P75 | Standard trading rules apply |
| `EXPANSION` | P75-P95 | Wide ranges, trend continuation likely, use wider stops |
| `EVENT` | > P95 | Extreme spike, consider pausing new entries |

**Data Source:** Uses existing `bar_range` from `EffortBaselineStore` (no new data collection).
Phase-aware baselines ensure GLOBEX volatility is compared to GLOBEX history, not RTH.

---

## Hysteresis Pattern (prevents regime whipsaw)

```cpp
// Regime change requires minConfirmationBars (default: 3) consecutive bars
// matching the new regime before transition is confirmed

VolatilityEngine engine;
engine.SetEffortStore(&effortStore);
engine.SetPhase(currentPhase);

VolatilityResult result = engine.Compute(barRangeTicks, atrValue);

if (result.IsReady()) {
    if (result.regime == VolatilityRegime::COMPRESSION) {
        // Tighten entry requirements, expect false breakouts
        if (!result.tradability.blockBreakouts) {
            // Allowed but use caution
        }
    }

    if (result.isTransitioning) {
        // Regime change in progress: candidateRegime != confirmedRegime
        // confirmationProgress shows how close (0.0 to 1.0)
    }
}
```

---

## Tradability Rules (per regime)

| Field | COMPRESSION | NORMAL | EXPANSION | EVENT |
|-------|-------------|--------|-----------|-------|
| `allowNewEntries` | true | true | true | false |
| `blockBreakouts` | true | false | false | false |
| `preferMeanReversion` | true | false | false | false |
| `requireWideStop` | false | false | true | true |
| `positionSizeMultiplier` | 0.75 | 1.0 | 1.0 | 0.5 |

---

## Session Boundary Pattern

```cpp
// At session end - update priors with EWMA blend
volatilityEngine.FinalizeSession();

// At session start - reset evidence, preserve priors
volatilityEngine.ResetForSession();
```

---

## ATR Normalization (optional)

When ATR is provided, `normalizedRange = barRange / ATR` enables cross-symbol comparison.
Values > 1.0 = wider than average, < 1.0 = tighter than average.

---

## NO-FALLBACK Contract

- `result.IsReady()` must be true before using `regime`
- `result.errorReason` distinguishes warmup from hard errors
- `VolatilityErrorReason::WARMUP_BASELINE` = phase baseline not yet ready
- `VolatilityErrorReason::ERR_NO_EFFORT_STORE` = configuration error

---

## Log Output

```
VOL: REGIME=NORMAL PCTILE=52.3 STABLE=15 | TRADABILITY: entries=OK breakouts=OK pos=1.00x
VOL: REGIME=COMPRESSION PCTILE=18.7 TRANSITIONING | candidate=NORMAL confirm=2/3
```
