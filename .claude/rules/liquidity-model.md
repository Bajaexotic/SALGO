# AMT Framework - Liquidity Model (Kyle's 4-Component)

## SSOT Location

| SSOT Owner | Location |
|------------|----------|
| **`LiquidityEngine`** | `AMT_Liquidity.h` (StudyState owns `liquidityEngine` and `lastLiqSnap`) |

**Definition:** Liquidity = executable near-touch depth vs aggressive pressure, with refill capacity and execution cost (spread).

---

## Kyle's Framework (1985) - All 3 Dimensions

```cpp
// 1. DEPTH (DepthMass): Distance-weighted resting volume within Dmax ticks
DepthMass = sum[ V(d) / (1 + d) ]  for d in [0, Dmax]
// Higher = more buffer against incoming orders

// 2. RESILIENCY (Resilience): Refill rate after depletion
RefillRate = max(0, DepthMass(t) - DepthMass(t-1)) / barDuration
// Higher = faster book recovery

// 3. TIGHTNESS (Spread): Bid-ask spread in ticks
// Lower spread = lower execution cost = higher liquidity
// spreadRank = percentile of current spread vs baseline
```

### Plus Stress (Demand Pressure)

```cpp
// 4. Stress: Aggressive demand relative to depth
Stress = AggressiveTotal / (DepthMassTotal + epsilon)
// Higher = more pressure consuming available liquidity
```

---

## Composite Formula (with Spread Penalty)

```cpp
// Resilience is only informative when stress tests it
// During quiet periods, resilience=0 means "untested", not "bad"
resilienceContrib = stressRank * resilienceRank + (1 - stressRank) * 1.0

// Spread penalty: wider spread reduces liquidity (Kyle's Tightness)
// spreadWeight = 0.15 (15% max penalty for widest spreads)
spreadPenalty = 1.0 - (spreadWeight * spreadRank)  // clamped to [0.5, 1.0]

LIQ = DepthRank * (1 - StressRank) * resilienceContrib * spreadPenalty

// When stress=0: resilienceContrib=1.0, LIQ=depthRank * spreadPenalty
// When stress=1: resilienceContrib=resilienceRank (fully weighted)
// When spread=wide (rank=1.0): 15% penalty applied
```

---

## Order Flow Toxicity Proxy (VPIN-lite)

```cpp
// Detects asymmetric liquidity consumption indicating informed order flow
// Based on VPIN (Easley, Lopez de Prado, O'Hara 2012)
toxicityProxy = |consumedBid - consumedAsk| / (consumedBid + consumedAsk + epsilon)
// Range: [0, 1]. High = one side consuming disproportionately = adverse selection risk
```

---

## LIQSTATE Classification

| State | Condition |
|-------|-----------|
| `LIQ_NOT_READY` | Any baseline insufficient |
| `LIQ_VOID` | LIQ <= 0.10 OR DepthRank <= 0.10 |
| `LIQ_THIN` | 0.10 < LIQ <= 0.25 OR StressRank >= 0.90 |
| `LIQ_NORMAL` | 0.25 < LIQ < 0.75 |
| `LIQ_THICK` | LIQ >= 0.75 |

**Safety Overrides:**
- DepthRank <= 0.10 -> Force VOID (insufficient buffer)
- StressRank >= 0.90 -> Force THIN (extreme pressure)

---

## Extreme Liquidity Detection (Jan 2025)

| Level | Stress Threshold | Depth Threshold | Meaning |
|-------|------------------|-----------------|---------|
| `EXTREME` | >= P95 | <= P5 | Dangerous conditions, reduce position size |
| `SHOCK` | >= P99 | <= P1 | Flash crash risk, hard block recommended |

**Combined OR logic:** Either extreme stress OR extreme thin depth is dangerous.
- High stress = aggressive demand overwhelming supply (stop runs, institutional sweep)
- Thin depth = no buffer to absorb aggression (flash crash, slippage risk)

### Result Flags

```cpp
// In Liq3Result
bool isExtremeLiquidity = false;    // Stress >= P95 OR Depth <= P5
bool isLiquidityShock = false;      // Stress >= P99 OR Depth <= P1
bool extremeFromStress = false;     // Stress component triggered extreme
bool extremeFromDepth = false;      // Depth component triggered extreme

// Helpers (require liqValid)
bool IsExtremeLiquidity() const { return liqValid && isExtremeLiquidity; }
bool IsLiquidityShock() const { return liqValid && isLiquidityShock; }
```

### Usage Pattern

```cpp
if (lastLiqSnap.IsLiquidityShock()) {
    // P99+ stress OR P1- depth - flash crash conditions
    // Hard block all new entries, tighten stops
}
else if (lastLiqSnap.IsExtremeLiquidity()) {
    // P95+ stress OR P5- depth - dangerous but not critical
    // Reduce position size, require additional confirmation
    if (lastLiqSnap.extremeFromStress) {
        // Aggressive sweep in progress
    }
    if (lastLiqSnap.extremeFromDepth) {
        // Book is dangerously thin
    }
}
```

---

## Temporal Coherence (Option B - Dec 2024)

ALL components use CLOSED BAR data for full temporal alignment:
- **DepthMass**: CLOSED BAR DOM via `c_ACSILDepthBars` API (see CRITICAL note below)
- **Stress**: CLOSED BAR aggressive volumes (`sc.AskVolume[closedBarIdx]`, `sc.BidVolume[closedBarIdx]`)
- **Reference price**: CLOSED BAR close price (`sc.Close[closedBarIdx]`)
- **Resilience**: Bar-to-bar change in DepthMass (both from historical data)

---

## CRITICAL: Historical Depth API Pattern (c_ACSILDepthBars)

Historical depth stores BOTH bid AND ask quantities at every price level (from different
moments during the bar). Calling both `GetLastBidQuantity` and `GetLastAskQuantity` at
the same price creates artificial "crossed markets" where BestBid > BestAsk.

**REQUIRED PATTERN**: Always use `GetLastDominantSide()` to classify each price level:
```cpp
const BuySellEnum dominantSide = p_DepthBars->GetLastDominantSide(barIdx, priceTickIdx);
if (dominantSide == BSE_BUY) {
    // This is a bid level - only call GetLastBidQuantity
} else if (dominantSide == BSE_SELL) {
    // This is an ask level - only call GetLastAskQuantity
}
// BSE_UNDEFINED: skip (ambiguous - equal quantities on both sides)
```

This applies to ALL historical depth extraction - baseline warmup AND live liquidity.

---

## Requirements

- `sc.MaintainHistoricalMarketDepthData = 1` (set in SetDefaults)
- User must enable "Support Downloading Historical Market Depth Data" in Global Settings >> Sierra Chart Server Settings
- Historical depth data available for up to 180 days (CME)

---

## Peak Liquidity & Consumed (Jan 2025)

In addition to end-of-bar depth (GetLast*), we now capture:
- **Peak depth**: Maximum depth during bar via `GetMaxBidQuantity`/`GetMaxAskQuantity`
- **Consumed depth**: Peak - Ending = liquidity absorbed by aggressive orders
- High consumed values indicate significant orderflow absorption

---

## Direct Stack/Pull API (Jan 2025)

Native ACSIL functions replace study input dependency:
```cpp
sc.SetUseMarketDepthPullingStackingData(1);  // In SetDefaults
const double bidStackPull = sc.GetBidMarketDepthStackPullSum();
const double askStackPull = sc.GetAskMarketDepthStackPullSum();
```

---

## DOM Staleness Detection (Jan 2025)

**SSOT:** `DOMQualityTracker` (AMT_Snapshots.h) tracks DOM freshness at two granularities:

| Level | Threshold | Detection | Purpose |
|-------|-----------|-----------|---------|
| **Millisecond** | 2000ms | `isStaleByMs` | Execution decisions (sub-second matters) |
| **Bar** | 10 bars | `isStaleByBars` | Structure changes (adaptive cadence) |

**Combined staleness:** `isStale = isStaleByMs OR isStaleByBars`

### Integration with LiquidityEngine

```cpp
// DOMQualityTracker populates Liq3Result staleness fields:
lastLiqSnap.depthAgeMs = domQualityTracker.GetAgeMs();
lastLiqSnap.depthStale = domQualityTracker.isStale;

// Stale DOM triggers HARD_BLOCK action (safety override)
if (domQualityTracker.isStale && lastLiqSnap.liqValid) {
    lastLiqSnap.recommendedAction = LiquidityAction::HARD_BLOCK;
}
```

### Execution Safety

- `liqValid` remains true (computed values are still mathematically valid)
- `recommendedAction = HARD_BLOCK` (don't execute with stale data)
- Downstream consumers check `IsDataFresh()` before acting

---

## Log Output

```
State: ZONE=... | SESS=... | ... | LIQ=0.42 NORMAL [D=65 S=23 R=78 T=35]
DOM: Depth bidMass=1234 askMass=987 IMB=0.11 | Stress=0.45 | RefillRate=12.3/s
DOM: Peak bidMass=1500 askMass=1200 total=2700 | Consumed bid=266 ask=213 total=479 | TOX=0.12
DOM: StackPull bid=50 ask=-30 net=20 (direct API)
DOM: Freshness ageMs=150 staleByMs=no staleByBars=no combined=fresh
NB: DiagDelta pos=1200 neg=800 net=400 | AvgTrade bid=2.5 ask=3.1 ratio=1.24
```
Where: D=Depth, S=Stress, R=Resilience, T=Tightness(spread), TOX=Toxicity

**Warning (if historical depth unavailable):**
```
[LIQ-HIST] Bar 100 | Historical depth data unavailable at closedBar 99 | Enable 'Support Downloading Historical Market Depth Data' in Server Settings
```

---

## Configuration (AMT_Liquidity.h)

```cpp
struct LiquidityConfig {
    int dmaxTicks = 4;              // ES: 4 ticks = 1 point from reference
    int maxDomLevels = 10;          // Max DOM levels per side
    size_t baselineMinSamples = 10; // Samples before baseline ready
    int baselineWindow = 300;       // Rolling window (bars)
    double epsilon = 1.0;           // Stress denominator safety

    // Kyle's Tightness component (spread impact on composite LIQ)
    double spreadWeight = 0.15;     // Weight of spread penalty (0.15 = 15% max penalty)
    double spreadMaxTicks = 4.0;    // Spread above this is "wide" (ES: 4 ticks = 1 point)

    // Extreme Liquidity Detection (Jan 2025)
    double extremeStressThreshold = 95.0;   // Stress >= P95 = extreme demand pressure
    double extremeDepthThreshold = 5.0;     // Depth <= P5 = extreme thin (inverted)
    double shockStressThreshold = 99.0;     // Stress >= P99 = shock demand pressure
    double shockDepthThreshold = 1.0;       // Depth <= P1 = shock thin (inverted)
};
```

---

## Phase-Aware Baselines (Jan 2025)

GLOBEX and RTH are "two totally different beasts" - liquidity profiles vary dramatically
by time of day. Phase-aware baselines compare current values against same-phase history.

| Component | Baseline Location | Why Phase-Aware? |
|-----------|------------------|------------------|
| **DepthMass** | `DOMWarmup.depthMassCore[phase]` | IB depth differs from GLOBEX |
| **Spread** | `DOMWarmup.spreadTicks[phase]` | Spreads widen in GLOBEX/CLOSING |
| **Stress** | `LiquidityEngine.stressBaseline` (local) | Unique to liquidity engine |
| **Resilience** | `LiquidityEngine.resilienceBaseline` (local) | Unique to liquidity engine |

---

## Wiring Pattern (in StudyState.resetAll)

```cpp
liquidityEngine.Reset();
liquidityEngine.SetDOMWarmup(&domWarmup);  // SSOT for depth/spread baselines

// Before each Compute() call:
liquidityEngine.SetPhase(closedBarPhase);  // Must set before Compute()
lastLiqSnap = liquidityEngine.Compute(...);
```

---

## Pre-Warm Pattern (phase-aware)

```cpp
// In PreWarmLiquidityBaselines - determine phase for each historical bar
SCDateTime barDT = sc.BaseDateTimeIn[bar];
const AMT::SessionPhase barPhase = AMT::DetermineSessionPhase(barTimeSec, rthStartSec, rthEndSec);
liquidityEngine.PreWarmFromBar(depthMass, askVol, bidVol, prevDepth, duration, barPhase);
```

---

## Spatial Liquidity Profile (Jan 2025)

**SSOT:** `st->lastSpatialProfile` (full) and summary fields in `Liq3Result`

Spatial profile analyzes WHERE liquidity is around price to detect walls and voids,
estimate execution risk, and gate trades accordingly.

### Detection Thresholds

| Feature | Detection | Purpose |
|---------|-----------|---------|
| **Walls** | Depth > 2.5σ above mean | Support/resistance barriers that block price |
| **Voids** | Depth < 10% of mean | Acceleration zones where price moves fast |
| **OBI** | (bidDepth - askDepth) / total | Directional bias [-1, +1] |
| **POLR** | Side with less resistance | Path-of-least-resistance: UP/DOWN/BAL |
| **Kyle's λ** | 1 / cumulative depth | Slippage estimation coefficient |

### Wall Classification

| Sigma Score | Classification | Trade Impact |
|-------------|----------------|--------------|
| 2.5-3.0σ | Significant | Increased risk multiplier |
| 3.0-4.0σ | Strong | May block trade in that direction |
| > 4.0σ | Extreme | Hard block for trades into wall |

### Trade Gating (SpatialTradeGating)

```cpp
// Block if strong wall within 3 ticks of reference price
if (nearestAskWall <= 3 && wall.IsStrong()) {
    gating.longBlocked = true;    // Don't go long into resistance
}
if (nearestBidWall <= 3 && wall.IsStrong()) {
    gating.shortBlocked = true;   // Don't go short into support
}

// Note acceleration risk from voids
gating.acceleratedByAskVoid = riskUp.hasVoidAcceleration;
gating.acceleratedByBidVoid = riskDown.hasVoidAcceleration;
```

### Log Output

```
SPATIAL: OBI=+0.25 POLR=UP | WALLS: bid=1 ask=0 [+3t,-1] | VOIDS: bid=0 ask=1
SPATIAL: GATE: long=OK short=BLOCK | RISK: up=0.8t down=1.2t | mean=1500 sigma=320
```

### Configuration (SpatialConfig)

```cpp
struct SpatialConfig {
    int analysisRangeTicks = 10;      // Range to analyze from reference
    int riskTargetTicks = 4;          // Distance for slippage estimation (ES: 1 point)
    double wallSigmaThreshold = 2.5;  // Sigma score for wall detection
    double voidDepthRatio = 0.10;     // Depth < 10% of mean = void
    size_t minLevelsForStats = 3;     // Min levels for mean/stddev
    double polrBiasThreshold = 0.15;  // OBI threshold for directional bias
    double wallBlockDistance = 3;     // Wall within N ticks blocks trade
};
```

---

## SSOT for Liquidity Metrics

| Metric | SSOT Location | Staging (DRY) |
|--------|---------------|---------------|
| DepthMass, Stress, Resilience | `lastLiqSnap.*` | N/A (computed directly) |
| Spread/Tightness | `lastLiqSnap.spreadRank` | N/A (computed directly) |
| Toxicity Proxy | `lastLiqSnap.toxicityProxy` | N/A (computed directly) |
| Peak/Consumed Depth | `lastLiqSnap.peakBidMass` etc | N/A (computed directly) |
| Stack/Pull | `lastLiqSnap.directBidStackPull` | `snap.liquidity.bidStackPull` |
| Diagonal Delta (SG43/44) | `lastLiqSnap.diagonalPosDeltaSum` | `snap.effort.diagonalPosDeltaSum` |
| Avg Trade Size (SG51/52) | `lastLiqSnap.avgBidTradeSize` | `snap.effort.avgBidTradeSize` |
| **Spatial Profile** | `lastSpatialProfile.*` | Summary in `lastLiqSnap.spatialGating` |
| Walls/Voids | `lastSpatialProfile.walls/voids` | `lastLiqSnap.nearestBidWallTicks` etc |
| OBI/POLR | `lastSpatialProfile.direction.*` | `lastLiqSnap.orderBookImbalance` |

**SSOT Contract:**
- `Liq3Result` (via `st->lastLiqSnap`) is the authoritative source for ALL liquidity metrics
- `LiquiditySnapshot` and `EffortSnapshot` are staging only (values read at bar start)
- Consumers MUST read from `lastLiqSnap`, never from staging locations
- API calls (`GetBidMarketDepthStackPullSum()` etc) happen ONCE per bar (DRY)
