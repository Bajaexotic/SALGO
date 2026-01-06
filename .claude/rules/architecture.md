# AMT Framework - Architecture Reference

## File Structure (36 files)

| File | Purpose |
|------|---------|
| `AuctionSensor_v1.cpp` | Main study entry point, orchestration, SC inputs/outputs |
| `amt_core.h` | Core enums: ZoneType, ZoneProximity, SessionPhase, AMTMarketState, etc. |
| `AMT_Analytics.h` | SessionStatistics, MarketStateBucket (hysteresis), performance metrics |
| `AMT_Arbitration_Seam.h` | M0 Arbitration Ladder, extreme delta thresholds (testable, no SC deps) |
| `AMT_BaselineGate.h` | **Decision Gate Pattern**: centralized baseline queries with validity gating |
| `AMT_BehaviorMapping.h` | Profile shape -> behavioral hypothesis mapping (O1-O5 outcomes) |
| `AMT_Bridge.h` | DeriveRoleFromType, DeriveMechanismFromType helpers |
| `AMT_config.h` | Configuration constants and defaults |
| `AMT_ContextBuilder.h` | AuctionContext builder (stateless, single authoritative population) |
| `AMT_Dalton.h` | **DaltonEngine**: 1TF/2TF, IB, day type, session bridge (overnight, gap, opening type) |
| `AMT_DayType.h` | Day structure classification (BALANCED/IMBALANCED), range extension |
| `AMT_DeltaEngine.h` | **DeltaEngine**: participation pressure (character, alignment, confidence, constraints) |
| `AMT_Display.h` | Chart drawing utilities |
| `AMT_DomEvents.h` | DOM event detection: DOMControlPattern, DOMEvent (no SC deps) |
| `AMT_DomPatterns.h` | Static DOM patterns: BalanceDOMPattern, ImbalanceDOMPattern |
| `AMT_Helpers.h` | Pure utility functions: TimeToSeconds, PriceToTicks, IsValidPrice |
| `AMT_Imbalance.h` | **ImbalanceEngine**: displacement detection, diagonal imbalance, delta divergence, absorption |
| `AMT_Invariants.h` | Runtime SSOT assertions and validation macros |
| `AMT_Levels.h` | Versioned profile levels (tick-based SSOT for POC/VAH/VAL) |
| `AMT_Liquidity.h` | **LiquidityEngine**: Kyle's 4-component model + DOM time-series patterns (SSOT) |
| `AMT_Logger.h` | LogManager, LogChannel, throttling |
| `AMT_LoggingContext.h` | Logging lifecycle contract, SSOT-compliant log value sourcing |
| `AMT_Modules.h` | AuctionContextModule, MiniVPModule, ZoneStore |
| `AMT_Patterns.h` | Pattern enums, ConfidenceAttribute, AuctionContext struct |
| `AMT_Phase.h` | PhaseTracker, phase detection logic |
| `AMT_Probes.h` | ProbeManager, ProbeRequest, ProbeResult, scenario matching |
| `AMT_ProfileShape.h` | Deterministic profile shape classification (HVN/LVN clusters) |
| `AMT_Session.h` | SessionContext (baselines), SessionManager (SSOT), SessionPhaseCoordinator |
| `AMT_Snapshots.h` | ObservableSnapshot, RollingDist, EffortBaselineStore, SessionDeltaBaseline |
| `AMT_TuningTelemetry.h` | Advisory friction/volatility tuning telemetry (observational only) |
| `AMT_Updates.h` | CreateZonesFromProfile, InitializeZoneSessionState, zone updates |
| `AMT_Volatility.h` | VolatilityEngine: regime classification (COMPRESSION/NORMAL/EXPANSION/EVENT) with hysteresis |
| `AMT_ValueLocation.h` | **ValueLocationEngine**: value-relative location, VA overlap, reference levels, strategy gating |
| `AMT_VolumeAcceptance.h` | **VolumeAcceptanceEngine**: acceptance/rejection detection, value migration, volume intensity |
| `AMT_VolumePatterns.h` | Deterministic volume profile pattern detection from structure |
| `AMT_VolumeProfile.h` | SessionVolumeProfile - VbP study integration |
| `AMT_Zones.h` | ZoneRuntime, ZoneManager, ZoneSessionState (cache), proximity, engagement |

---

## Header Dependency Order

```
amt_core.h
    |-- AMT_config.h
    |-- AMT_Patterns.h
    |-- AMT_Helpers.h
    |       +-- AMT_Snapshots.h
    |               |-- AMT_VolumeProfile.h
    |               +-- AMT_Session.h
    |                       |-- AMT_Logger.h
    |                       |-- AMT_Probes.h
    |                       +-- AMT_Modules.h
    |-- AMT_Zones.h
    |-- AMT_Updates.h (requires AMT_Zones.h)
    |-- AMT_Phase.h
    |-- AMT_Analytics.h
    +-- AMT_Bridge.h
```

---

## Key Structs Location Reference

| Struct | File |
|--------|------|
| `ZoneRuntime` | AMT_Zones.h |
| `ZoneManager` | AMT_Zones.h |
| `ZoneSessionState` | AMT_Zones.h (cache of session state for zone operations) |
| `SessionContext` | AMT_Session.h (baselines + phase context) |
| `SessionManager` | AMT_Session.h (SSOT for SessionKey + POC/VAH/VAL) |
| `SessionKey` | amt_core.h (deterministic session identity) |
| `SessionPhaseCoordinator` | AMT_Session.h |
| `SessionVolumeProfile` | AMT_VolumeProfile.h |
| `ObservableSnapshot` | AMT_Snapshots.h |
| `EffortBaselineStore` | AMT_Snapshots.h (phase-bucketed bar metrics: vol, trades, delta, range) |
| `SessionDeltaBaseline` | AMT_Snapshots.h (phase-bucketed session delta ratios) |
| `DOMWarmup` | AMT_Snapshots.h (phase-bucketed DOM metrics: stack, pull, depth) |
| `BaselineDecisionGate` | AMT_BaselineGate.h (centralized baseline query with validity gating) |
| `ExtremeDeltaInput` | AMT_BaselineGate.h (decision input for delta extremeness) |
| `MarketCompositionInput` | AMT_BaselineGate.h (decision input for activity level) |
| `RangeClassificationInput` | AMT_BaselineGate.h (decision input for range regime) |
| `AuctionContext` | AMT_Patterns.h |
| `PhaseTracker` | AMT_Phase.h |
| `ProbeManager` | AMT_Probes.h |
| `LogManager` | AMT_Logger.h |
| `LiquidityEngine` | AMT_Liquidity.h (Kyle's 4-component: Depth, Stress, Resilience, Spread; phase-aware via DOMWarmup) |
| `Liq3Result` | AMT_Liquidity.h (per-bar output with validity flags) |
| `LiquidityState` | amt_core.h (enum: LIQ_NOT_READY, LIQ_VOID, LIQ_THIN, LIQ_NORMAL, LIQ_THICK) |
| `VolatilityEngine` | AMT_Volatility.h (regime classification with hysteresis, phase-aware via EffortBaselineStore) |
| `VolatilityResult` | AMT_Volatility.h (per-bar output: regime, tradability, validity) |
| `VolatilityRegime` | AMT_Volatility.h (enum: UNKNOWN, COMPRESSION, NORMAL, EXPANSION, EVENT) |
| `TradabilityRules` | AMT_Volatility.h (per-regime trading constraints) |
| `ImbalanceEngine` | AMT_Imbalance.h (displacement detection: diagonal imbalance, divergence, absorption, trapped traders) |
| `ImbalanceResult` | AMT_Imbalance.h (per-bar output: type, direction, conviction, context gates) |
| `ImbalanceType` | AMT_Imbalance.h (enum: NONE, STACKED_BUY/SELL, DELTA_DIVERGENCE, ABSORPTION_*, TRAPPED_*, etc.) |
| `ConvictionType` | AMT_Imbalance.h (enum: UNKNOWN, INITIATIVE, RESPONSIVE, LIQUIDATION) |
| `ImbalanceDirection` | AMT_Imbalance.h (enum: NEUTRAL, BULLISH, BEARISH) |
| `ContextGateResult` | AMT_Imbalance.h (liquidity + volatility + chop gate status) |
| `VolumeAcceptanceEngine` | AMT_VolumeAcceptance.h (acceptance/rejection detection, phase-aware via EffortBaselineStore) |
| `VolumeAcceptanceResult` | AMT_VolumeAcceptance.h (per-bar output: state, migration, intensity, confirmation) |
| `AcceptanceState` | AMT_VolumeAcceptance.h (enum: UNKNOWN, TESTING, ACCEPTED, REJECTED) |
| `ValueMigrationState` | AMT_VolumeAcceptance.h (enum: UNKNOWN, UNCHANGED, MIGRATING_HIGHER/LOWER, ROTATING) |
| `VolumeIntensity` | AMT_VolumeAcceptance.h (enum: UNKNOWN, VERY_LOW, LOW, NORMAL, HIGH, VERY_HIGH) |
| `ConfirmationRequirement` | AMT_VolumeAcceptance.h (confirmation multiplier and requirements for downstream) |
| `POCMigrationTracker` | AMT_VolumeAcceptance.h (tracks POC position for value migration detection) |
| `ValueAreaTracker` | AMT_VolumeAcceptance.h (tracks VA expansion/contraction/overlap) |
| `ValueLocationEngine` | AMT_ValueLocation.h (value-relative location classification with hysteresis) |
| `ValueLocationResult` | AMT_ValueLocation.h (per-bar output: zone, structure, references, gating) |
| `ValueZone` | AMT_ValueLocation.h (enum: FAR_ABOVE/NEAR_ABOVE/AT_VAH/UPPER/AT_POC/LOWER/AT_VAL/NEAR_BELOW/FAR_BELOW) |
| `VAOverlapState` | AMT_ValueLocation.h (enum: SEPARATED_ABOVE/BELOW, OVERLAPPING, CONTAINED, EXPANDING) |
| `ReferenceLevelProximity` | AMT_ValueLocation.h (distance to reference levels with proximity flags) |
| `StrategyGating` | AMT_ValueLocation.h (actionable recommendations: fade/breakout/trend multipliers) |
| `DaltonEngine` | AMT_Dalton.h (SSOT: 1TF/2TF, IB, day type, session bridge) |
| `DaltonState` | AMT_Dalton.h (per-bar output: phase, timeframe, IB, extension, opening type) |
| `SessionBridge` | AMT_Dalton.h (SSOT: overnight context, gap, prior RTH for GLOBEX→RTH transition) |
| `OvernightSession` | AMT_Dalton.h (GLOBEX extremes, mini-IB, overnight pattern) |
| `OvernightInventory` | AMT_Dalton.h (net position from overnight: NET_LONG/NET_SHORT/NEUTRAL, score) |
| `GapContext` | AMT_Dalton.h (gap type, size, fill status at RTH open) |
| `OpeningType` | AMT_Dalton.h (enum: OPEN_DRIVE, OPEN_TEST_DRIVE, OPEN_REJECTION_REVERSE, OPEN_AUCTION) |
| `GlobexMiniIBTracker` | AMT_Dalton.h (tracks first 30 min of GLOBEX as mini-IB) |
| `OpeningTypeClassifier` | AMT_Dalton.h (classifies Dalton's 4 opening types in first 30 min RTH) |
| `ValueLocationConfig` | AMT_ValueLocation.h (tolerance thresholds, overlap thresholds, hysteresis) |
| `DeltaEngine` | AMT_DeltaEngine.h (delta participation pressure: character, alignment, confidence; phase-aware) |
| `DeltaResult` | AMT_DeltaEngine.h (per-bar output: character, alignment, constraints, events, validity) |
| `DeltaCharacter` | AMT_DeltaEngine.h (enum: UNKNOWN, NEUTRAL, EPISODIC, SUSTAINED, BUILDING, FADING, REVERSAL) |
| `DeltaAlignment` | AMT_DeltaEngine.h (enum: UNKNOWN, NEUTRAL, CONVERGENT, DIVERGENT, ABSORPTION_BID/ASK) |
| `DeltaConfidence` | AMT_DeltaEngine.h (enum: UNKNOWN, BLOCKED, LOW, DEGRADED, FULL) |
| `DeltaTradingConstraints` | AMT_DeltaEngine.h (position sizing, entry restrictions based on delta state) |
| `DeltaHistoryTracker` | AMT_DeltaEngine.h (rolling window for character/alignment hysteresis) |
| `DomHistoryBuffer` | AMT_DomEvents.h (session-scoped circular buffer for DOM time-series, owned by LiquidityEngine) |
| `DomDetectionResult` | AMT_DomEvents.h (DOM patterns detected from time-series: control patterns + events) |

---

## Value Location SSOT Hierarchy

**`ValueLocationEngine`** is the Single Source of Truth for all value-relative location classification.

```
ValueLocationEngine (SSOT)
         │
         ▼
    ValueZone (9 states) ─────────────────────────────────────────┐
         │                                                         │
         ├──► ValueLocation (6 states) ← ZoneToLocation() mapping  │
         │         └──► DaltonState.location                       │
         │         └──► ActivityClassification.location            │
         │                                                         │
         ├──► ValueZoneSimple (5 states) ← MapValueZoneToSimple()  │
         │         └──► DeltaLocationContext.zone                  │
         │                                                         │
         └──► ValueZone (direct) ─────────────────────────────────┘
                   └──► DomPatternContext.valueZone
                   └──► Liq3Result.spatialValueZone
```

### Enum Granularities

| Enum | States | Purpose |
|------|--------|---------|
| `ValueZone` | 9 | Fine-grained SSOT (FAR_ABOVE, NEAR_ABOVE, AT_VAH, UPPER, AT_POC, LOWER, AT_VAL, NEAR_BELOW, FAR_BELOW) |
| `ValueLocation` | 6 | Coarse classification for high-level decisions (INSIDE, ABOVE, BELOW, AT_POC, AT_VAH, AT_VAL) |
| `ValueZoneSimple` | 5 | Delta-specific simplification (IN_VALUE, AT_VALUE_EDGE, OUTSIDE_VALUE, IN_DISCOVERY) |

### SSOT Consumer Pattern

All consumers MUST use `BuildFromValueLocation()` or equivalent SSOT-consuming methods:

```cpp
// CORRECT: Consume from SSOT
auto locCtx = DeltaLocationContext::BuildFromValueLocation(st->lastValueLocationResult);
auto domCtx = DomPatternContext::BuildFromValueLocation(st->lastValueLocationResult, ...);
auto activity = classifier.ClassifyFromValueLocation(st->lastValueLocationResult, ...);

// DEPRECATED: Computes own location (bypasses SSOT)
auto locCtx = DeltaLocationContext::Build(price, poc, vah, val, ...);  // [[deprecated]]
auto domCtx = DomPatternContext::Build(price, poc, vah, val, ...);     // [[deprecated]]
auto activity = classifier.Classify(price, prevPrice, poc, vah, val, ...);  // [[deprecated]]
```

### Orchestration Order

The orchestrator (AuctionSensor) MUST call `ValueLocationEngine` first, then pass `ValueLocationResult` to all downstream engines:

1. `ValueLocationEngine.Compute()` → `lastValueLocationResult` (SSOT)
2. `DeltaEngine.Compute()` with `BuildFromValueLocation()`
3. `ActivityClassifier.ClassifyFromValueLocation()`
4. `LiquidityEngine` spatial patterns with `BuildPatternContextFromValueLocation()`
5. Other engines consume location from step 1
