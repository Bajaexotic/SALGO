# AMT Framework - Imbalance Engine

## SSOT Location

| SSOT Owner | Location |
|------------|----------|
| **`ImbalanceEngine`** | `AMT_Imbalance.h` (StudyState owns `imbalanceEngine` and `lastImbalanceResult`) |

**PURPOSE:** The ImbalanceEngine answers 5 questions about auction displacement:

1. **Did price displace or rotate?** -> `displacementScore` [0-1]
2. **Is the move initiative vs responsive?** -> `ConvictionType` enum
3. **Valid liquidity/vol context?** -> `ContextGateResult` gates
4. **What is the trigger?** -> `ImbalanceType` + `ImbalanceDirection` + strength
5. **What invalidates it?** -> `ImbalanceErrorReason` taxonomy

---

## 10 Imbalance Types

| Type | Detection | Signal |
|------|-----------|--------|
| `STACKED_BUY/SELL` | 3+ diagonal imbalances (SG43/44) | Strong directional |
| `DELTA_DIVERGENCE` | Price/CVD divergence at swing | Reversal warning |
| `ABSORPTION_BID/ASK` | High volume + narrow range | Support/resistance |
| `TRAPPED_LONGS/SHORTS` | Imbalances opposite bar direction | Failed breakout |
| `VALUE_MIGRATION` | POC shift + VA expansion | Trend development |
| `RANGE_EXTENSION` | IB break + 1TF conviction | Breakout day |
| `EXCESS` | Single-print tail (long wick + rejection) | Auction end |

---

## 3 Conviction Types

| Type | Indicators |
|------|------------|
| `INITIATIVE` | 1TF pattern, stacked imbalances, delta confirms direction |
| `RESPONSIVE` | Absorption, divergence, trapped traders, excess |
| `LIQUIDATION` | LIQ_VOID + high friction (forced exit) |

---

## Context Gates (from LiquidityEngine + VolatilityEngine)

```cpp
struct ContextGateResult {
    bool liquidityOK;     // Not VOID (or THIN if configured)
    bool volatilityOK;    // Not EVENT regime
    bool chopOK;          // Not high-rotation overlapping profile
    bool allGatesPass;    // All three gates pass
};
```

---

## Hysteresis Pattern (prevents signal whipsaw)

```cpp
// State change requires minConfirmationBars (default: 2)
// Signal persists maxPersistenceBars (default: 10) without refresh

ImbalanceEngine engine;
engine.SetPhase(currentPhase);

ImbalanceResult result = engine.Compute(
    high, low, close, open, prevHigh, prevLow, prevClose, tickSize, barIndex,
    poc, vah, val, prevPOC, prevVAH, prevVAL,
    diagonalPosDelta, diagonalNegDelta,  // From NB SG43/44
    totalVolume, barDelta, cumulativeDelta,
    liqState, volRegime, executionFriction,
    ibHigh, ibLow, sessionHigh, sessionLow,
    rotationFactor, is1TF
);

if (result.IsReady() && result.HasConfirmedSignal()) {
    if (result.IsBullish() && result.IsInitiative()) {
        // Strong bullish displacement confirmed
    }
}
```

---

## Quality Check

```cpp
// IsHighQualitySignal() = confirmed + initiative + context gates pass + confidence >= 0.6
if (result.IsHighQualitySignal()) {
    // Actionable signal with full conviction
}
```

---

## Extreme Imbalance Detection (Jan 2025)

| Level | Threshold | Meaning |
|-------|-----------|---------|
| `EXTREME` | >= P95 diagonal percentile | Exceptional pressure, high conviction |
| `SHOCK` | >= P99 diagonal percentile | Capitulation or institutional sweep |

**Data source:** `diagonalPercentile` (magnitude of diagonal delta vs baseline).

```cpp
// Check for extreme conditions
if (result.IsShock()) {
    // P99+ diagonal delta - capitulation or institutional sweep
    // May indicate exhaustion or start of strong move
}
else if (result.IsExtreme()) {
    // P95+ diagonal delta - exceptional pressure
    // Increase conviction for continuation signals
}

// Access raw flags
if (result.isExtremeImbalance || result.isShockImbalance) {
    // Factor into downstream decisions
}
```

### Configuration

```cpp
double extremeImbalanceThreshold = 95.0; // >= P95 = extreme
double shockImbalanceThreshold = 99.0;   // >= P99 = shock
```

---

## NO-FALLBACK Contract

- `result.IsReady()` must be true before using type/direction/conviction
- `result.errorReason` distinguishes warmup from blocks from hard errors
- `IsImbalanceWarmup(reason)` / `IsImbalanceBlocked(reason)` helpers

---

## Log Output

```
IMB: TYPE=STACKED_BUY DIR=BULLISH CONV=INITIATIVE STR=0.72 CONF=0.68
IMB: DIAG=+1250 PCTILE=87 STACK=4 | DIV=NO | ABSORB=NO | TRAPPED=NO
IMB: GATES: LIQ=OK VOL=OK CHOP=OK | DISP=0.65 POC_SHIFT=+6t VA_OVL=32%
IMB: HYSTERESIS: confirmed=STACKED_BUY candidate=SAME bars=3
```
