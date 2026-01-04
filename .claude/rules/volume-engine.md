# AMT Framework - Volume Acceptance Engine

## SSOT Location

| SSOT Owner | Location |
|------------|----------|
| **`VolumeAcceptanceEngine`** | `AMT_VolumeAcceptance.h` (StudyState owns `volumeEngine` and `lastVolumeResult`) |

**PURPOSE:** Volume answers "was this move accepted by the market?"

1. **Did volume support the move or reject it?** -> `AcceptanceState` enum
2. **Is value forming higher/lower or unchanged?** -> `ValueMigrationState` enum
3. **What is 'high' volume today?** -> `VolumeIntensity` (baseline-relative)
4. **What confirmation does it provide to triggers?** -> `ConfirmationMultiplier`

---

## 3 Acceptance States

| State | Condition | Trading Implication |
|-------|-----------|---------------------|
| `ACCEPTED` | High volume + sustained move + POC migration | Move is "real", continuation likely |
| `REJECTED` | Low volume breakout OR fast return OR wick rejection | Move is "fake", fade opportunity |
| `TESTING` | At value edge, mixed signals | Wait for confirmation |

---

## 5 Volume Intensity Levels

| Level | Percentile | Meaning |
|-------|------------|---------|
| `VERY_LOW` | < P10 | Extremely quiet, holiday-like |
| `LOW` | P10-P25 | Below normal, low conviction |
| `NORMAL` | P25-P75 | Typical activity |
| `HIGH` | P75-P90 | Elevated, institutional interest |
| `VERY_HIGH` | > P90 | Extreme, major player activity |

---

## 4 Value Migration States

| State | Detection | Meaning |
|-------|-----------|---------|
| `MIGRATING_HIGHER` | POC rate > 0.3 ticks/bar OR VAH expanding | Value accepting higher prices |
| `MIGRATING_LOWER` | POC rate < -0.3 ticks/bar OR VAL expanding | Value accepting lower prices |
| `ROTATING` | VA expanding both directions | Balance day, auction bracketing |
| `UNCHANGED` | POC stable, VA stable | No meaningful value shift |

---

## Acceptance Detection Mechanisms

```cpp
// Volume component (30% weight)
// High percentile = high acceptance
volumeComponent = (volumePercentile > 60) ? normalized : 0.0;

// Price action component (20% weight)
// Sustained bars outside VA = acceptance
priceActionComponent = min(1.0, barsOutsideVA / 10.0);

// Time component (15% weight)
// Time spent at level = acceptance
timeComponent = (barsAtLevel / sessionBars) * 2.0;

// Delta component (20% weight)
// Delta confirming direction = acceptance
deltaComponent = (delta aligns with price direction) ? deltaRatio : 0.0;

// POC migration component (15% weight)
// POC following price = acceptance
pocMigrationComponent = (POC moving toward price) ? 0.8 : 0.2;
```

---

## Rejection Detection Mechanisms

| Signal | Detection | Weight |
|--------|-----------|--------|
| `lowVolumeBreakout` | Price outside VA + volume < P30 | 0.30 |
| `fastReturn` | Was outside VA, returned within 3 bars | 0.35 |
| `wickRejection` | Wick > 40% of range at VA edge | 0.25 |
| `deltaRejection` | Delta opposite to price direction | 0.20 |

Multiple rejection signals get 1.2x bonus.

---

## Confirmation Multipliers

| State + Volume | Multiplier | Usage |
|----------------|------------|-------|
| ACCEPTED + HIGH | 1.5x | Boost signal confidence |
| ACCEPTED + NORMAL | 1.0x | Neutral |
| TESTING | 0.8x | Reduce confidence |
| REJECTED | 0.5x | Strong reduction |
| Any + LOW | 0.7x | Additional penalty |

---

## Data Sources (Sierra Chart)

| Data | Source | Usage |
|------|--------|-------|
| Volume | Numbers Bars SG13 | Total volume per bar |
| Volume Rate | Numbers Bars SG53/54 | Bid/Ask volume per second |
| POC | VbP Study SG2 | Value migration tracking |
| VAH/VAL | VbP Study SG3/SG4 | Acceptance boundary |
| Delta | Cumulative Delta Bars | Directional confirmation |

---

## Hysteresis Pattern

```cpp
// State change requires minConfirmationBars (default: 2)
// State persists maxPersistenceBars (default: 15) without refresh

VolumeAcceptanceEngine engine;
engine.SetEffortStore(&effortStore);
engine.SetPhase(currentPhase);

VolumeAcceptanceResult result = engine.Compute(
    close, high, low, tickSize, barIndex,
    totalVolume,
    bidVolume, askVolume, delta,
    poc, vah, val,
    priorPOC, priorVAH, priorVAL,
    volumePerSecond
);

if (result.IsReady()) {
    if (result.IsAccepted() && result.IsHighVolume()) {
        // Strong volume-confirmed acceptance - trust the move
    }
    if (result.IsRejected()) {
        // Volume doesn't support move - expect reversal
    }

    // Apply multiplier to downstream signals
    signalConfidence *= result.confirmationMultiplier;
}
```

---

## Quality Checks

```cpp
// IsHighQualityAcceptance() = accepted + high volume + score >= 0.7 + delta confirms
if (result.IsHighQualityAcceptance()) {
    // Very strong acceptance, high confidence continuation
}

// IsHighQualityRejection() = rejected + score >= 0.7 + at least one rejection signal
if (result.IsHighQualityRejection()) {
    // Very strong rejection, high confidence reversal
}
```

---

## NO-FALLBACK Contract

- `result.IsReady()` must be true before using state/intensity/migration
- `result.errorReason` distinguishes warmup from hard errors
- `IsAcceptanceWarmup(reason)` helper for warmup detection
- Warmup states: `WARMUP_VOLUME_BASELINE`, `WARMUP_POC_HISTORY`, `WARMUP_VA_HISTORY`

---

## Log Output

```
VOL: STATE=ACCEPTED INT=HIGH CONF_MULT=1.50 | SCORE: ACC=0.72 REJ=0.15
VOL: PCT=78.3 RATIO=1.42 | DELTA=0.35 | MIGR=HIGHER POC_SHIFT=+4.2t
VOL: VA_OVL=0.45 VA_EXP=+8t BIAS=+1 | BARS: above=12 in=45 below=3
VOL: REJECTION: lowVol=NO fast=NO wick=NO delta=NO
```

---

## Wiring Pattern (in StudyState.resetAll)

```cpp
volumeEngine.Reset();
volumeEngine.SetEffortStore(&effortStore);

// Before each Compute() call:
volumeEngine.SetPhase(closedBarPhase);
lastVolumeResult = volumeEngine.Compute(...);
```

---

## Session Boundary Pattern

```cpp
// At session start
volumeEngine.ResetForSession();

// Set prior session levels for overlap calculation
volumeEngine.SetPriorSessionLevels(priorPOC, priorVAH, priorVAL);
```

---

## Integration with Other Engines

| Engine | Integration |
|--------|-------------|
| `ImbalanceEngine` | Volume acceptance gates imbalance confidence |
| `VolatilityEngine` | EVENT regime + low volume = extra caution |
| `LiquidityEngine` | LIQ_VOID + low volume = very high rejection risk |
| `DaltonEngine` | 1TF + high volume acceptance = strong trend |

---

## Configuration (AMT_VolumeAcceptance.h)

```cpp
struct VolumeAcceptanceConfig {
    // Volume intensity thresholds
    double veryLowThreshold = 10.0;     // < P10
    double lowThreshold = 25.0;         // P10-P25
    double highThreshold = 75.0;        // P75-P90
    double veryHighThreshold = 90.0;    // > P90

    // Acceptance thresholds
    double acceptanceScoreThreshold = 0.6;
    double rejectionScoreThreshold = 0.6;
    double volumeConfirmationPctile = 60.0;
    double lowVolumeBreakoutPctile = 30.0;

    // POC migration
    double pocMigrationMinTicks = 2.0;
    double pocMigrationRateThreshold = 0.3;  // ticks/bar

    // Hysteresis
    int minConfirmationBars = 2;
    int maxPersistenceBars = 15;

    // Component weights
    double weightVolume = 0.30;
    double weightPriceAction = 0.20;
    double weightTime = 0.15;
    double weightDelta = 0.20;
    double weightPOCMigration = 0.15;
};
```
