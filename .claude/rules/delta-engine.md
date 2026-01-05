# AMT Framework - Delta Engine

## SSOT Location

| SSOT Owner | Location |
|------------|----------|
| **`DeltaEngine`** | `AMT_DeltaEngine.h` (StudyState owns `deltaEngine` and `lastDeltaResult`) |

**PHILOSOPHY:** Delta is PARTICIPATION PRESSURE, not "bull/bear".
It measures WHO is more aggressive in fulfilling their order, not WHO is right.

> A strong negative delta at a low doesn't mean "sellers winning" - it means aggressive sellers are HITTING into passive buyers. The buyers who absorb without moving price are often the informed party.

---

## 5 Key Questions the Engine Answers

| Question | Output | Purpose |
|----------|--------|---------|
| **1. CHARACTER** | `DeltaCharacter` enum | Is aggression sustained or episodic? (trend vs burst) |
| **2. ALIGNMENT** | `DeltaAlignment` enum | Is delta aligned with price or diverging? (efficiency flag) |
| **3. NOISE FLOOR** | Baseline percentiles | What's the baseline-relative magnitude today? |
| **4. CONFIDENCE GATE** | `DeltaConfidence` enum | When should I downgrade confidence? |
| **5. CONSTRAINTS** | `DeltaTradingConstraints` | What trading restrictions apply? |

---

## 7 Delta Characters

| Character | Meaning | Trading Implication |
|-----------|---------|---------------------|
| `UNKNOWN` | Baseline not ready | Skip delta signals |
| `NEUTRAL` | Delta within noise band | Low signal content, avoid |
| `EPISODIC` | Single-bar spike | Burst, may fade, lower confidence |
| `SUSTAINED` | Multi-bar aligned | Conviction, trend-following appropriate |
| `BUILDING` | Increasing magnitude | Acceleration, momentum intensifying |
| `FADING` | Decreasing magnitude | Exhaustion, potential reversal setup |
| `REVERSAL` | Direction flipped | High signal, active trend change |

---

## 6 Delta Alignments

| Alignment | Definition | Signal |
|-----------|------------|--------|
| `UNKNOWN` | Baseline not ready | Skip |
| `NEUTRAL` | Low delta, low signal | Avoid trading |
| `CONVERGENT` | Delta matches price direction | Efficient, follow trend |
| `DIVERGENT` | Delta opposes price direction | Absorption, reversal warning |
| `ABSORPTION_BID` | Passive buyers absorbing at low | Bullish divergence |
| `ABSORPTION_ASK` | Passive sellers absorbing at high | Bearish divergence |

**Key Insight:** Price up + negative delta = price rising on selling = passive buyers absorbing = bullish.

---

## 4 Confidence Levels

| Level | Condition | Action |
|-------|-----------|--------|
| `FULL` | Normal conditions | Full weight to delta signals |
| `DEGRADED` | Some concern (low volume, some chop) | Proceed with caution |
| `LOW` | Significant concern (thin tape, extreme) | Require additional confirmation |
| `BLOCKED` | Critical conditions | Do not use delta for decisions |

### Confidence Degradation Triggers

| Flag | Condition | Impact |
|------|-----------|--------|
| `isThinTape` | Volume < P10 | Degraded/Low |
| `isHighChop` | 4+ reversals in lookback | Degraded |
| `isExhaustion` | Delta > P95 | Low (exhaustion risk) |
| `isGlobexSession` | GLOBEX hours | Degraded (lower liquidity) |

---

## Trading Constraints

```cpp
struct DeltaTradingConstraints {
    bool allowContinuation;       // Can take continuation signals
    bool allowBreakout;           // Can take breakout signals
    bool allowFade;               // Can fade (mean reversion)
    bool requireDeltaAlignment;   // Must have CONVERGENT delta
    bool requireSustained;        // Must have SUSTAINED character
    double positionSizeMultiplier; // Scale position (0.5-1.0)
    double confidenceWeight;      // Weight in composite score
};
```

**Constraint Rules:**
- Block continuation on DIVERGENT alignment
- Require CONVERGENT for breakout signals
- Require SUSTAINED for continuation
- Scale position size on LOW confidence

---

## Hysteresis Pattern

```cpp
// Character change requires confirmBars (default: 2) consecutive bars
// Alignment change requires confirmBars (default: 2) consecutive bars

DeltaEngine engine;
engine.SetPhase(currentPhase);

DeltaResult result = engine.Compute(barDelta, barVolume, priceChange,
                                     sessionCumDelta, sessionVolume);

if (result.IsReady()) {
    if (result.character == DeltaCharacter::SUSTAINED &&
        result.alignment == DeltaAlignment::CONVERGENT) {
        // High confidence continuation signal
    }
}
```

---

## Phase-Aware Baselines

GLOBEX and RTH delta characteristics differ dramatically:

| Component | Baseline Location | Why Phase-Aware? |
|-----------|-------------------|------------------|
| **Bar Delta %** | `EffortBaselineStore.delta_pct[phase]` | IB delta differs from GLOBEX |
| **Session Delta %** | `SessionDeltaBaseline.sessionDeltaPct[phase]` | Session context varies |
| **Volume** | `EffortBaselineStore.vol_sec[phase]` | Confidence gating |

---

## Wiring Pattern (in StudyState)

```cpp
// In StudyState.resetAll():
deltaEngine.Reset();
deltaEngine.SetEffortStore(&effortStore);
deltaEngine.SetSessionDeltaBaseline(&sessionDeltaBaseline);

// Before each Compute():
deltaEngine.SetPhase(closedBarPhase);
lastDeltaResult = deltaEngine.Compute(
    barDelta, barVolume, priceChangeTicks,
    sessionCumDelta, sessionVolume
);
```

---

## Session Boundary Pattern

```cpp
// At session start:
deltaEngine.ResetForSession();
// Clears history tracker, preserves baseline priors
```

---

## NO-FALLBACK Contract

- `result.IsReady()` must be true before using character/alignment
- `result.IsWarmup()` distinguishes warmup from hard errors
- `result.errorReason` provides explicit failure taxonomy:
  - `WARMUP_BAR_BASELINE` - Bar delta baseline not ready
  - `WARMUP_SESSION_BASELINE` - Session delta baseline not ready
  - `ERR_ZERO_VOLUME` - Can't compute deltaPct
  - `ERR_NO_BASELINE_STORE` - Configuration error

---

## Error Taxonomy

| Category | Codes | Meaning |
|----------|-------|---------|
| **Warmup** | 10-13 | Expected during startup, not errors |
| **Input Errors** | 20-22 | Invalid data or configuration |
| **Warnings** | 30-33 | Confidence degradation reasons |
| **Session Events** | 40 | Session just reset |

---

## Events (Transition Bars Only)

| Event | When True |
|-------|-----------|
| `characterChanged` | Character classification changed (after hysteresis) |
| `alignmentChanged` | Alignment classification changed |
| `reversalDetected` | Delta direction reversed |
| `divergenceStarted` | Just entered divergence |
| `convergenceRestored` | Just exited divergence |

---

## Log Output

```
DELTA: CHAR=SUSTAINED ALIGN=CONVERGENT CONF=FULL | BAR=+0.35 P=72 | SESS=+0.12 P=58
DELTA: SUSTAINED_BARS=5 MAG_TREND=+0.08 | CONSTRAINTS: cont=OK break=OK size=1.00x
DELTA: EVENTS: charChg=NO alignChg=NO reversal=NO divStart=NO
```

---

## Configuration (DeltaConfig)

```cpp
struct DeltaConfig {
    // Noise thresholds
    double noiseFloorPctile = 25.0;     // Below P25 = noise
    double strongSignalPctile = 75.0;   // Above P75 = strong
    double extremePctile = 90.0;        // Above P90 = extreme

    // Character classification
    int sustainedMinBars = 3;           // Bars to confirm sustained
    double buildingMagnitudeThreshold = 0.1;  // Increase per bar

    // Alignment classification
    double alignmentDeltaThreshold = 0.15;    // Min |deltaPct| for signal
    double absorptionStrengthMin = 0.5;       // Min for absorption signal

    // Confidence thresholds
    double thinTapeVolumePctile = 10.0;       // Below P10 = thin tape
    double exhaustionDeltaPctile = 95.0;      // Above P95 = exhaustion
    int highChopReversalsThreshold = 4;       // 4+ reversals = chop

    // Hysteresis
    int characterConfirmBars = 2;
    int alignmentConfirmBars = 2;

    // Constraints
    bool blockContinuationOnDivergence = true;
    bool requireAlignmentForBreakout = true;
};
```

---

## Related Pattern Detectors

| Detector | File | Context |
|----------|------|---------|
| `BalanceDeltaPatternDetector` | AMT_DeltaPatterns.h | BALANCE regime (2TF) |
| `ImbalanceDeltaPatternDetector` | AMT_ImbalanceDeltaPatterns.h | IMBALANCE regime (1TF) |

### Balance Patterns (2TF)
- `ABSORPTION` - Volume absorbed without price movement
- `DELTA_DIVERGENCE_FADE` - Delta diverges, expect mean reversion
- `AGGRESSIVE_INITIATION` - Strong delta initiates new move

### Imbalance Patterns (1TF)
- `STRONG_CONVERGENCE` - Delta confirms direction (continuation)
- `WEAK_PULLBACK` - Price retraces but delta doesn't reverse (add-on)
- `EFFORT_NO_RESULT` - High delta, low price movement (absorption)
- `CLIMAX_EXHAUSTION` - Extreme delta at boundary (capitulation)

---

## AMT Value-Relative Awareness (Jan 2025)

**KEY AMT INSIGHT:** Delta is only meaningful relative to where the auction is.

| Location | Expected Delta Behavior | Interpretation |
|----------|------------------------|----------------|
| **At POC** | Lower delta expected | Rotation normal, don't overtrade signals |
| **At VAH/VAL edges** | Higher delta expected | Breakout/rejection attempts, watch for absorption |
| **Outside value** | Sustained delta expected | Testing acceptance/rejection of new prices |
| **In discovery** | High-conviction only | Far from value, need strong confirmation |

### What DeltaEngine MUST Be Aware Of

1. **Auction Location Relative to Value**
   - Where is price relative to POC, VAH, VAL?
   - Is price inside or outside the value area?
   - Distance from nearest profile level (in ticks)

2. **Progress vs Failure Detection**
   - Is the auction progressing (building/sustained)?
   - Is the auction failing (fading/diverging)?

3. **Time Persistence of Signals**
   - Sustained character implies strong signal
   - Episodic character implies potential trap

4. **Regime Quality Gates**
   - LIQ_VOID, EVENT regime should block/degrade signals
   - Thin tape requires extra confirmation

5. **Contextual Extremes**
   - Session high/low proximity
   - IB boundary proximity
   - Percentile relative to phase baseline

6. **Auction Outcome Implications**
   - Emit ACCEPTANCE_LIKELY / REJECTION_LIKELY / ROTATION_LIKELY
   - These are state descriptors, NOT trade signals

### What DeltaEngine MUST NOT Own

- DOM microstructure (depth, stack/pull) → Use LiquidityEngine
- Profile topology (HVN/LVN, shape) → Use ProfileShapeEngine

---

## Location Context (DeltaLocationContext)

DeltaEngine CONSUMES location context from caller, does NOT compute it.

```cpp
struct DeltaLocationContext {
    ValueZoneSimple zone;           // IN_VALUE, AT_VALUE_EDGE, OUTSIDE_VALUE, IN_DISCOVERY
    double distanceFromPOCTicks;
    double distanceFromVAHTicks;
    double distanceFromVALTicks;
    bool isInValue, isAtEdge, isOutsideValue, isInDiscovery;
    bool isMigratingTowardPrice;    // POC following price
    bool isMigratingAwayFromPrice;  // POC retreating
    bool isAboveSessionHigh, isBelowSessionLow;
    bool isAtIBExtreme;
    bool isValid;

    static DeltaLocationContext Build(price, poc, vah, val, tickSize, ...);
};
```

### ValueZoneSimple Enum

| Zone | Definition | Delta Interpretation |
|------|------------|---------------------|
| `IN_VALUE` | Between VAH and VAL | Rotation expected, signals less decisive |
| `AT_VALUE_EDGE` | At/near VAH or VAL (within tolerance) | Breakout/rejection point, absorption significant |
| `OUTSIDE_VALUE` | Beyond VAH or VAL | Testing acceptance, sustained delta confirms |
| `IN_DISCOVERY` | Far outside value (> threshold ticks) | High conviction required for action |

---

## Auction Outcome Implications (AuctionOutcome)

**These are state descriptors, NOT trade signals.**

| Outcome | Condition | Meaning |
|---------|-----------|---------|
| `ACCEPTANCE_LIKELY` | Sustained + convergent + outside value + holding | Market accepting new value |
| `REJECTION_LIKELY` | Absorption + at edge + exhaustion + divergent | Market rejecting price level |
| `ROTATION_LIKELY` | Episodic + chop + in value center | Market rotating in balance |

### Likelihood Computation

Likelihoods sum to 1.0 and are computed based on:
- Location zone (in value → rotation biased)
- Delta character (sustained → acceptance biased)
- Delta alignment (divergent → rejection biased)
- POC migration (toward price → acceptance boost)
- Session extreme proximity

```cpp
// Access outcome likelihoods
if (result.HasLocationContext()) {
    if (result.IsAcceptanceLikely() && result.GetDominantLikelihood() > 0.6) {
        // High confidence acceptance - market forming new value
    }
    if (result.IsRejectionLikely() && result.IsAtValueEdge()) {
        // Rejection at edge - classic absorption pattern
    }
}
```

### Location-Sensitive Adjustments

| Location | Adjustment |
|----------|------------|
| `AT_VALUE_EDGE` | Boost divergence significance (1.3x) for absorption detection |
| `OUTSIDE_VALUE` | Allow continuation on sustained+aligned |
| `IN_DISCOVERY` | Reduce position size without clear conviction (0.75x) |
| `IN_VALUE` | Require delta alignment for breakout signals |

---

## Location-Aware Compute Pattern

```cpp
// Build location context from value levels
auto locCtx = DeltaLocationContext::Build(
    currentPrice, poc, vah, val, tickSize,
    2.0,  // edgeToleranceTicks
    8.0,  // discoveryThresholdTicks
    sessionHigh, sessionLow, ibHigh, ibLow, priorPOC);

// Use location-aware overload
DeltaResult result = engine.Compute(
    barDelta, barVolume, priceChangeTicks,
    sessionCumDelta, sessionVolume, currentBar,
    locCtx);  // Pass location context

if (result.IsReady() && result.HasLocationContext()) {
    // Access AMT-aware outcome
    if (result.IsHighQualitySignalWithContext()) {
        // Strong delta + high conviction outcome
    }
}
```

---

## Log Output (with Location)

```
DELTA: CHAR=SUSTAINED ALIGN=CONVERGENT CONF=FULL | BAR=+0.35 P=72 | SESS=+0.12 P=58
DELTA: LOC=OUTSIDE_VALUE POC=+12t VAH=+8t | MIGR=TOWARD
DELTA: OUTCOME=ACCEPT_LIKELY A=0.65 R=0.20 ROT=0.15 | CONVICT=HIGH
```

---

## Integration with Other Engines

| Engine | Integration |
|--------|-------------|
| `VolatilityEngine` | EVENT regime blocks delta signals |
| `LiquidityEngine` | LIQ_VOID degrades delta confidence |
| `ImbalanceEngine` | Delta alignment confirms imbalance direction |
| `VolumeAcceptanceEngine` | Delta confirms acceptance/rejection |
| `DaltonEngine` | 1TF/2TF determines which pattern detector to use |
| `ValueLocationEngine` | Provides location context for delta interpretation |

---

## Context Gates (Jan 2025)

DeltaEngine CONSUMES context from LiquidityEngine, VolatilityEngine, and optionally DaltonEngine
to gate and adjust its signals. This follows the pattern established by ImbalanceEngine.

**Dependency order:** Baselines → VolatilityEngine → LiquidityEngine → DeltaEngine → ImbalanceEngine

### Gate Behavior

| Engine | State | Effect |
|--------|-------|--------|
| `LiquidityEngine` | `LIQ_VOID` | **BLOCK** - Do not use delta signals |
| `LiquidityEngine` | `LIQ_THIN` | Configurable (default: degrade only) |
| `LiquidityEngine` | High stress (≥ 90%) | **DEGRADE** - Tighten requirements |
| `VolatilityEngine` | `EVENT` | **BLOCK** - Extreme volatility |
| `VolatilityEngine` | `COMPRESSION` | **DEGRADE** - Prefer fade, distrust breakouts |
| `DaltonEngine` | `BALANCE` (2TF) | Tighten continuation requirements |
| `DaltonEngine` | `IMBALANCE` (1TF) | Relax requirements for aligned signals |

### DeltaContextGateResult Struct

```cpp
struct DeltaContextGateResult {
    // Gate status
    bool liquidityOK = false;           // Not VOID (or THIN if configured)
    bool volatilityOK = false;          // Not EVENT regime
    bool compressionDegraded = false;   // In COMPRESSION (degrades, doesn't block)
    bool allGatesPass = false;          // All gates pass
    bool contextValid = false;          // Context was provided and evaluated

    // External engine states (captured)
    LiquidityState liqState = LiquidityState::LIQ_NOT_READY;
    VolatilityRegime volRegime = VolatilityRegime::UNKNOWN;
    double stressRank = 0.0;            // [0, 1] - liquidity stress
    bool highStress = false;            // stressRank >= threshold (0.90)

    // Optional Dalton context
    AMTMarketState daltonState = AMTMarketState::UNKNOWN;
    bool is1TF = false;                 // True if IMBALANCE (one-time framing)
    bool hasDaltonContext = false;      // True if Dalton context was provided

    // Block reason (if any)
    DeltaErrorReason blockReason = DeltaErrorReason::NONE;

    // Helpers
    bool IsBlocked() const;   // liquidityOK == false || volatilityOK == false
    bool IsDegraded() const;  // compressionDegraded || highStress
};
```

### Error Codes for Context Gates

| Code | Meaning |
|------|---------|
| `BLOCKED_LIQUIDITY_VOID` (50) | LiquidityState::LIQ_VOID |
| `BLOCKED_LIQUIDITY_THIN` (51) | LiquidityState::LIQ_THIN (if blockOnThin enabled) |
| `BLOCKED_VOLATILITY_EVENT` (52) | VolatilityRegime::EVENT |
| `DEGRADED_VOLATILITY_COMPRESSION` (53) | COMPRESSION regime (warning only) |
| `DEGRADED_HIGH_STRESS` (54) | High liquidity stress ≥ 90% (warning only) |

### Full Context-Aware Compute Pattern

```cpp
// Extract context from other engines
const AMT::LiquidityState liqState = st->lastLiqSnap.liqValid
    ? st->lastLiqSnap.liqState : AMT::LiquidityState::LIQ_NOT_READY;
const double stressRank = st->lastLiqSnap.stressRankValid
    ? (st->lastLiqSnap.stressRank / 100.0) : 0.0;
const AMT::VolatilityRegime volRegime = st->lastVolResult.IsReady()
    ? st->lastVolResult.regime : AMT::VolatilityRegime::UNKNOWN;
const AMT::AMTMarketState daltonState = st->lastDaltonState.phase;
const bool is1TF = (daltonState == AMT::AMTMarketState::IMBALANCE);

// Build location context
auto locCtx = DeltaLocationContext::Build(
    currentPrice, poc, vah, val, tickSize,
    2.0, 8.0,  // edge/discovery thresholds
    sessionHigh, sessionLow, ibHigh, ibLow, priorPOC);

// Full context-aware compute (location + context gates)
DeltaResult result = engine.Compute(
    barDelta, barVolume, priceChangeTicks,
    sessionCumDelta, sessionVolume, currentBar,
    locCtx,                          // Location context
    liqState, volRegime, stressRank, // Context gates
    daltonState, is1TF);             // Optional Dalton

if (result.IsReady()) {
    if (result.IsContextBlocked()) {
        // Delta signals blocked by external context
        // result.errorReason has specific block reason
    } else if (result.IsContextDegraded()) {
        // Delta signals degraded but usable
        // result.contextGate has degradation details
    } else if (result.contextGate.allGatesPass) {
        // Full confidence in delta signals
    }
}
```

### Context-Aware Constraint Adjustments

When context gates are evaluated, constraints are adjusted:

| Gate Condition | Constraint Adjustment |
|----------------|----------------------|
| `LIQ_VOID` or `EVENT` | Block all signals (continuation, breakout, fade) |
| `COMPRESSION` | Block breakouts, allow fade only |
| High stress (≥ 90%) | Require sustained character, reduce position size 0.75x |
| `IMBALANCE` (1TF) | Relax continuation requirements if delta aligned |
| `BALANCE` (2TF) | Tighten continuation requirements |

### Log Output (with Context Gates)

```
DELTA: CHAR=SUSTAINED ALIGN=CONVERGENT CONF=FULL | BAR=+0.35 P=72 | SESS=+0.12 P=58
DELTA: LOC=OUTSIDE_VALUE POC=+12t VAH=+8t | MIGR=TOWARD
DELTA: GATES: LIQ=NORMAL VOL=NORMAL STRESS=23% | DALTON=IMBALANCE 1TF
DELTA: OUTCOME=ACCEPT_LIKELY A=0.65 R=0.20 ROT=0.15 | CONVICT=HIGH
```

### Configuration (Context Gates)

```cpp
// Added to DeltaConfig
bool requireLiquidityGate = true;   // Evaluate liquidity context
bool requireVolatilityGate = true;  // Evaluate volatility context
bool blockOnVoid = true;            // LIQ_VOID blocks signals
bool blockOnThin = false;           // LIQ_THIN blocks (default: degrade only)
bool blockOnEvent = true;           // EVENT regime blocks signals
bool degradeOnCompression = true;   // COMPRESSION degrades signals
double highStressThreshold = 0.90;  // Stress rank threshold for degradation
bool useDaltonContext = false;      // Include Dalton 1TF/2TF context
```

---

## Asymmetric Hysteresis (Jan 2025)

**PRINCIPLE:** Danger signals should enter fast, calm signals should exit slow.

| Transition | Bars | Rationale |
|------------|------|-----------|
| Any → `REVERSAL` | 1 | Danger signal - react fast |
| Any → `BUILDING` | 1 | Acceleration is time-sensitive |
| `SUSTAINED` → other | 3 | Confirm trend really ending |
| Any → `DIVERGENT`/`ABSORPTION` | 1 | Absorption warning - react fast |
| `CONVERGENT` → other | 3 | Confirm alignment really lost |
| Other transitions | 2 | Default (unchanged) |

### Configuration

```cpp
// Asymmetric hysteresis - Character
int reversalEntryBars = 1;              // Any -> REVERSAL: react fast
int buildingEntryBars = 1;              // Any -> BUILDING: acceleration
int sustainedExitBars = 3;              // SUSTAINED -> other: confirm exit
int otherCharacterTransitionBars = 2;   // Default

// Asymmetric hysteresis - Alignment
int divergenceEntryBars = 1;            // Any -> DIVERGENT/ABSORPTION
int convergenceExitBars = 3;            // CONVERGENT -> other: confirm exit
int otherAlignmentTransitionBars = 2;   // Default
```

### New Methods

```cpp
// Called internally to determine confirmation bars for transition
int GetCharacterConfirmationBars(DeltaCharacter from, DeltaCharacter to) const;
int GetAlignmentConfirmationBars(DeltaAlignment from, DeltaAlignment to) const;
```

### Diagnostic Fields

```cpp
// In DeltaResult
int characterConfirmationRequired = 0;   // Bars required for this transition
int alignmentConfirmationRequired = 0;
int barsInConfirmedCharacter = 0;        // Time in confirmed state
int barsInConfirmedAlignment = 0;
```

---

## Extended Baseline Metrics (Jan 2025)

DeltaEngine now uses 5 of 7 available metrics from `EffortBaselineStore`, up from 2.

| Metric | Baseline | Purpose |
|--------|----------|---------|
| `delta_pct` | EffortBaselineStore | Core: bar delta percentile |
| `vol_sec` | EffortBaselineStore | Core: volume percentile (thin tape) |
| `trades_sec` | EffortBaselineStore | NEW: thin tape classification |
| `bar_range` | EffortBaselineStore | NEW: range-adaptive thresholds |
| `avg_trade_size` | EffortBaselineStore | NEW: institutional detection |

---

## Thin Tape Classification (ThinTapeType)

Distinguishes different types of low activity:

| Type | Condition | Interpretation | Confidence Impact |
|------|-----------|----------------|-------------------|
| `NONE` | Normal activity | Standard processing | 0 |
| `TRUE_THIN` | Low vol + low trades | No real participation | -3 (major) |
| `HFT_FRAGMENTED` | Low vol + high trades | HFT noise, unreliable | -1 (minor) |
| `INSTITUTIONAL` | High vol + low trades | Block trades, informed | +1 (boost) |

### Detection Logic

```cpp
// Classification thresholds (configurable)
lowVolume = volumePctile < P10
highVolume = volumePctile > P75
lowTrades = tradesPctile < P25
highTrades = tradesPctile > P75

// Classification
TRUE_THIN:       lowVolume && lowTrades
HFT_FRAGMENTED:  lowVolume && highTrades
INSTITUTIONAL:   highVolume && lowTrades
```

### New Result Fields

```cpp
// In DeltaResult
double tradesPctile = 0.0;               // Trades/sec percentile
bool tradesBaselineReady = false;
ThinTapeType thinTapeType = ThinTapeType::NONE;
```

### Configuration

```cpp
double lowTradesPctile = 25.0;          // Below P25 = low trades
double highTradesPctile = 75.0;         // Above P75 = high trades
double lowVolumePctile = 10.0;          // Below P10 = low volume
double highVolumePctile = 75.0;         // Above P75 = high volume
int thinTapeConfidencePenalty = 3;      // TRUE_THIN: -3
int hftFragmentedConfidencePenalty = 1; // HFT_FRAGMENTED: -1
int institutionalConfidenceBoost = 1;   // INSTITUTIONAL: +1
```

---

## Range-Adaptive Thresholds (Jan 2025)

**PRINCIPLE:** In compression, smaller delta is meaningful. In expansion, require larger delta.

| Range Regime | Noise Floor | Strong Signal |
|--------------|-------------|---------------|
| Compression (< P25) | 25 × 0.7 = 17.5 | 75 × 0.7 = 52.5 |
| Normal (P25-P75) | 25.0 | 75.0 |
| Expansion (> P75) | 25 × 1.3 = 32.5 | 75 × 1.3 = 97.5 |

### New Result Fields

```cpp
// In DeltaResult
double rangePctile = 0.0;                // Bar range percentile
bool rangeBaselineReady = false;
double effectiveNoiseFloor = 25.0;       // Adjusted noise floor
double effectiveStrongSignal = 75.0;     // Adjusted strong signal
bool rangeAdaptiveApplied = false;       // Was adjustment applied?
```

### Configuration

```cpp
bool useRangeAdaptiveThresholds = true;
double compressionRangePctile = 25.0;    // Below P25 = compression
double expansionRangePctile = 75.0;      // Above P75 = expansion
double compressionNoiseMultiplier = 0.7; // Compression: 70% of normal
double expansionNoiseMultiplier = 1.3;   // Expansion: 130% of normal
```

---

## Institutional Activity Detection (Jan 2025)

Large average trade size indicates institutional (informed) order flow.

### Detection

```cpp
isInstitutionalActivity = avgTradeSizePctile >= P80
isRetailActivity = avgTradeSizePctile <= P20
```

### New Result Fields

```cpp
// In DeltaResult
double avgTradeSizePctile = 0.0;
bool avgTradeBaselineReady = false;
bool isInstitutionalActivity = false;    // Above P80
bool isRetailActivity = false;           // Below P20
```

### Configuration

```cpp
bool useAvgTradeSizeContext = true;
double institutionalAvgTradePctile = 80.0;
double retailAvgTradePctile = 20.0;
```

---

## DeltaInput Struct (Jan 2025)

Clean interface for passing inputs, including extended metrics:

```cpp
struct DeltaInput {
    // Core inputs (required)
    double barDelta = 0.0;
    double barVolume = 0.0;
    double priceChangeTicks = 0.0;
    double sessionCumDelta = 0.0;
    double sessionVolume = 0.0;
    int currentBar = -1;

    // Extended inputs (optional)
    double barRangeTicks = 0.0;         // For range-adaptive thresholds
    double numTrades = 0.0;             // For thin tape classification
    double tradesPerSec = 0.0;
    double avgBidTradeSize = 0.0;       // For institutional detection
    double avgAskTradeSize = 0.0;
    bool hasExtendedInputs = false;     // True when extended fields populated

    // Builder pattern
    DeltaInput& WithCore(...);          // Set core inputs
    DeltaInput& WithExtended(...);      // Set extended inputs + flag
};
```

### New Compute Overloads

```cpp
// Basic with extended inputs
DeltaResult Compute(const DeltaInput& input);

// With location context
DeltaResult Compute(const DeltaInput& input, const DeltaLocationContext& locationCtx);

// Full context (location + gates)
DeltaResult Compute(const DeltaInput& input, const DeltaLocationContext& locationCtx,
                    LiquidityState liqState, VolatilityRegime volRegime,
                    double stressRank, AMTMarketState daltonState, bool is1TF);
```

### Usage Pattern

```cpp
// Build input with extended metrics
DeltaInput input;
input.WithCore(barDelta, barVolume, priceChangeTicks,
               sessionCumDelta, sessionVolume, currentBar)
     .WithExtended(barRangeTicks, numTrades, tradesPerSec,
                   avgBidTradeSize, avgAskTradeSize);

// Compute with extended metrics
DeltaResult result = engine.Compute(input, locCtx, liqState, volRegime, stress);

if (result.hasExtendedInputs && result.tradesBaselineReady) {
    // Use thin tape classification
    if (result.thinTapeType == ThinTapeType::INSTITUTIONAL) {
        // High confidence - institutional activity
    }
}

if (result.rangeAdaptiveApplied) {
    // Thresholds were adjusted for compression/expansion
}
```

---

## Log Output (Extended Metrics)

```
DELTA: CHAR=SUSTAINED ALIGN=CONVERGENT CONF=FULL | BAR=+0.35 P=72 | SESS=+0.12 P=58
DELTA: EXT: TAPE=NONE TRADES_P=52 RANGE_P=45 AVG_P=67 | NOISE=25.0 STRONG=75.0
DELTA: HYSTER: CHAR_REQ=2 ALIGN_REQ=1 | BARS_CHAR=8 BARS_ALIGN=3
DELTA: GATES: LIQ=NORMAL VOL=NORMAL STRESS=23% | DALTON=IMBALANCE 1TF
```
