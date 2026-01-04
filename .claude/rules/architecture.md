# AMT Framework - Architecture Reference

## File Structure (37 files)

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
| `AMT_DayType.h` | Day structure classification (BALANCED/IMBALANCED), range extension |
| `AMT_DeltaPatterns.h` | Balance delta patterns: absorption, divergence, aggressive initiation |
| `AMT_Display.h` | Chart drawing utilities |
| `AMT_DomEvents.h` | DOM event detection: DOMControlPattern, DOMEvent (no SC deps) |
| `AMT_DomPatterns.h` | Static DOM patterns: BalanceDOMPattern, ImbalanceDOMPattern |
| `AMT_Helpers.h` | Pure utility functions: TimeToSeconds, PriceToTicks, IsValidPrice |
| `AMT_Imbalance.h` | **ImbalanceEngine**: displacement detection, diagonal imbalance, delta divergence, absorption |
| `AMT_ImbalanceDeltaPatterns.h` | Imbalance delta patterns: convergence, pullback, exhaustion, climax |
| `AMT_Invariants.h` | Runtime SSOT assertions and validation macros |
| `AMT_Levels.h` | Versioned profile levels (tick-based SSOT for POC/VAH/VAL) |
| `AMT_Liquidity.h` | 3-Component Liquidity Model (Dec 2024): DepthMass, Stress, Resilience |
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
| `ValueLocationConfig` | AMT_ValueLocation.h (tolerance thresholds, overlap thresholds, hysteresis) |
