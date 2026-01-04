# Baseline Framework Design Document

## Deliverable A: Baseline Inventory Report

### 1. Baseline Producers

#### Core Structures (AMT_Snapshots.h)

| Structure | Location | Purpose |
|-----------|----------|---------|
| `RollingDist` | AMT_Snapshots.h:264-385 | Rolling window distribution with robust statistics |
| `BaselineEngine` | AMT_Snapshots.h:391-469 | Container holding all metric baselines |

#### RollingDist API (Current)
```cpp
struct RollingDist {
    std::deque<double> values;
    int window = 300;

    void reset(int w);
    void push(double v);          // Adds sample
    double percentile(double val) const;     // FALLBACK: returns 50.0 if empty
    double mean() const;                      // FALLBACK: returns 1.0 if empty
    double median() const;                    // Returns 0.0 if empty
    double mad() const;                       // Returns 0.0 if size < 2
    bool isExtreme(double val, double k=2.5); // Returns false if size < 10
    double percentileRank(double val) const;  // FALLBACK: returns 50.0 if empty
    size_t size() const;
};
```

#### BaselineEngine Members (AMT_Snapshots.h:393-413)

| Member | Type/Units | Current Stats | Semantics |
|--------|-----------|---------------|-----------|
| `vol_sec` | Continuous, rate (vol/sec) | percentile, isExtreme | Volume intensity per second |
| `delta_sec` | Continuous signed, rate | push only | Delta intensity per second |
| `total_vol` | Continuous nonneg, magnitude | median, percentileRank | Total bar volume |
| `delta_pct` | Bounded [-1,1] | percentile, isExtreme | Bar delta as fraction of volume |
| `session_delta_pct` | Bounded [0,1] magnitude | percentile | Session cumulative delta magnitude |
| `max_delta` | Continuous nonneg | push only | Maximum single-price delta |
| `trades_sec` | Continuous nonneg, rate | isExtreme | Trade count per second |
| `depth_mass_core` | Continuous nonneg | median, mean, isExtreme | Core DOM depth mass |
| `depth_mass_halo` | Continuous nonneg | push only | Halo DOM depth mass |
| `stack_rate` | Continuous signed | isExtreme | Net stack rate |
| `pull_rate` | Continuous signed | isExtreme | Net pull rate |
| `escape_velocity` | Continuous nonneg | push only | Zone escape velocity |
| `time_in_zone` | Discrete count | push only | Bars spent in zone |
| `bar_range` | Continuous nonneg, ticks | percentileRank | Bar high-low range |

### 2. Baseline Push Sites (Writers)

| Location | Metrics Pushed | Condition |
|----------|---------------|-----------|
| AuctionSensor_v1.cpp:1238-1247 | vol_sec, total_vol, delta_pct, max_delta | Session-type routing |
| AuctionSensor_v1.cpp:1390-1402 | trades_sec, bar_range, delta_sec | Always |
| AuctionSensor_v1.cpp:1424 | session_delta_pct | Session volume > 0 |
| AuctionSensor_v1.cpp:1495-1523 | stack_rate, pull_rate, depth_mass_core, depth_mass_halo | DOM inputs valid |
| AuctionSensor_v1.cpp:1934-1935 | time_in_zone, escape_velocity | Engagement finalized |

### 3. Baseline Consumers

#### Critical Decision Points

| Location | Metric | Method | Used For |
|----------|--------|--------|----------|
| AMT_LoggingContext.h:296-298 | session_delta_pct | `.percentile(abs(val))` | Session delta extremeness |
| AMT_LoggingContext.h:314 | total_vol | `.median()` | Delta reliability weighting |
| AMT_LoggingContext.h:340-341 | total_vol, bar_range | `.percentileRank()` | Facilitation classification |
| AMT_LoggingContext.h:361 | depth_mass_core | `.median()` | Liquidity availability |
| AuctionSensor_v1.cpp:2581 | Multiple | `.checkExtremes()` | Extreme condition detection |
| AuctionSensor_v1.cpp:2619 | total_vol | `.median()` | Delta consistency reliability |
| AuctionSensor_v1.cpp:2655 | depth_mass_core | `.mean()` | Liquidity normalization |
| AuctionSensor_v1.cpp:2687-2688 | total_vol, bar_range | `.percentileRank()` | Facilitation percentiles |
| AuctionSensor_v1.cpp:4456, 4757 | session_delta_pct | `.percentile()` | Extreme delta session check |
| AuctionSensor_v1.cpp:5243-5247 | vol_sec, delta_pct | `.percentile()` | Probe scoring |

### 4. Fallback Inventory

#### Silent Defaults in RollingDist (VIOLATIONS)

| Method | Condition | Fallback Value | Risk Level |
|--------|-----------|---------------|------------|
| `percentile()` | `values.empty()` | **50.0** | **HIGH** - Fabricates neutral |
| `mean()` | `values.empty()` | **1.0** | **HIGH** - Fabricates non-zero |
| `median()` | `values.empty()` | 0.0 | Medium - At least detectable |
| `mad()` | `size() < 2` | 0.0 | Medium - Makes isExtreme safe |
| `isExtreme()` | `size() < 10` | false | Low - Conservative |
| `percentileRank()` | `values.empty()` | **50.0** | **HIGH** - Fabricates neutral |

#### Downstream Fallbacks

| Location | Condition | Fallback | Risk |
|----------|-----------|----------|------|
| AMT_LoggingContext.h:322 | baselineVolMedian <= 0 | reliability = 0.5 | Medium |
| AMT_LoggingContext.h:347 | baseline not ready | facilitation = EFFICIENT | **HIGH** |
| AMT_LoggingContext.h:367 | baselineDepth == 0 | liquidityAvailability = 0.5 | **HIGH** |

### 5. Existing Sample Gates (Good Patterns)

| Location | Gate | Threshold | Behavior |
|----------|------|-----------|----------|
| AMT_LoggingContext.h:296 | session_delta_pct.size() | >= 10 | Sets sessDeltaPctlValid=false |
| AMT_LoggingContext.h:311 | total_vol.size() | >= 20 | Sets deltaConfValid=false |
| AMT_LoggingContext.h:336-337 | total_vol + bar_range.size() | >= 20 | Sets facilitationValid=false |
| AMT_LoggingContext.h:357 | depth_mass_core.size() | >= 10 | Sets liquidityValid=false |
| AuctionSensor_v1.cpp:4755 | session_delta_pct.size() | >= 10 | Guards percentile call |

---

## Deliverable B: Baseline Contract + Mapping Spec

### 1. Unified Baseline Contract

```cpp
// ============================================================================
// BASELINE CONTRACT (Proposed)
// ============================================================================

enum class BaselineReadiness : int {
    READY = 0,           // Baseline has sufficient samples
    WARMUP = 1,          // Insufficient samples, not yet ready
    STALE = 2,           // Data too old or context changed
    UNAVAILABLE = 3      // Input source not configured
};

struct BaselineQuery {
    double value;                    // Query value
    BaselineReadiness readiness;     // Gate status

    // Location estimates (when READY)
    double median;
    double mean;

    // Dispersion estimates (when READY)
    double mad;                      // Median Absolute Deviation
    double iqr;                      // Interquartile range (p75 - p25)

    // Percentile output (when READY)
    double percentile;               // 0-100 rank
    double percentileRank;           // Robust z-score based rank

    // Extremeness (when READY)
    bool isExtreme;                  // Beyond k*MAD from median

    // Metadata
    size_t sampleCount;
    int contextId;                   // Session/bucket identifier
};

// Contract requirements:
// 1. When readiness != READY, all numeric outputs are UNDEFINED (not fabricated)
// 2. Consumers MUST check readiness before using numeric values
// 3. No fallback values like 50.0, 1.0, or 0.5 - explicit INVALID instead
```

### 2. Model Types

| Model | Applicable Metrics | Location Estimator | Dispersion | Rationale |
|-------|-------------------|-------------------|------------|-----------|
| **ROBUST_CONTINUOUS** | vol_sec, delta_sec, total_vol, max_delta, bar_range | median | MAD | Heavy-tailed, outlier-prone rates |
| **BOUNDED_RATIO** | delta_pct, session_delta_pct | median | IQR | Bounded [-1,1] or [0,1] |
| **POSITIVE_SKEW** | depth_mass_core, depth_mass_halo, stack_rate, pull_rate | median | MAD | Non-negative, often right-skewed |
| **COUNT_MODEL** | trades_sec, time_in_zone | quantiles only | none | Discrete counts, no parametric fit |

### 3. Metric → Model Mapping

| Metric | Model Type | Min Samples | Units |
|--------|-----------|-------------|-------|
| vol_sec | ROBUST_CONTINUOUS | 20 | vol/sec |
| delta_sec | ROBUST_CONTINUOUS | 20 | delta/sec |
| total_vol | ROBUST_CONTINUOUS | 20 | volume |
| delta_pct | BOUNDED_RATIO | 20 | ratio [-1,1] |
| session_delta_pct | BOUNDED_RATIO | 10 | ratio [0,1] |
| max_delta | ROBUST_CONTINUOUS | 20 | volume |
| trades_sec | COUNT_MODEL | 20 | trades/sec |
| depth_mass_core | POSITIVE_SKEW | 10 | depth units |
| depth_mass_halo | POSITIVE_SKEW | 10 | depth units |
| stack_rate | POSITIVE_SKEW | 10 | rate |
| pull_rate | POSITIVE_SKEW | 10 | rate |
| escape_velocity | ROBUST_CONTINUOUS | 10 | ticks/bar |
| time_in_zone | COUNT_MODEL | 10 | bars |
| bar_range | ROBUST_CONTINUOUS | 20 | ticks |

### 4. Readiness Rules

```cpp
// Minimum samples per model type
constexpr size_t MIN_SAMPLES_ROBUST_CONTINUOUS = 20;
constexpr size_t MIN_SAMPLES_BOUNDED_RATIO = 10;
constexpr size_t MIN_SAMPLES_POSITIVE_SKEW = 10;
constexpr size_t MIN_SAMPLES_COUNT_MODEL = 10;

// Readiness check
BaselineReadiness CheckReadiness(const RollingDist& dist, BaselineModelType model) {
    const size_t minSamples = GetMinSamples(model);
    if (dist.size() >= minSamples) return BaselineReadiness::READY;
    if (dist.size() > 0) return BaselineReadiness::WARMUP;
    return BaselineReadiness::UNAVAILABLE;
}
```

### 5. Downstream Behavior When INVALID

| Consumer | Current Behavior | Required Behavior |
|----------|-----------------|-------------------|
| deltaConsistency | Uses fallback 50.0 | Set deltaConfValid=false, skip metric |
| facilitation | Defaults to EFFICIENT | Set facilitationValid=false, log once |
| liquidityAvailability | Uses fallback 0.5 | Set liquidityValid=false, exclude from score |
| sessionDeltaPctile | Uses fallback 50.0 | Set sessDeltaPctlValid=false, skip extreme check |
| checkExtremes() | Returns false | Return ExtremeCheck with all false + notReady flag |
| percentileRank | Returns 50.0 | Return -1 or NaN + valid=false flag |

---

## Deliverable C: Staged Implementation Plan

### Stage 1: Contract + Wrapper + Readiness (LOW RISK)

**Goal:** Add readiness checking without changing behavior when ready.

**Changes:**
1. Add `BaselineReadiness` enum to amt_core.h
2. Add `IsReady(size_t minSamples)` method to RollingDist
3. Add model-type min-samples constants to AMT_config.h
4. Create `BaselineWrapper` struct that wraps RollingDist with readiness

**Evidence Required:**
- [ ] Compilation passes
- [ ] Existing tests pass unchanged
- [ ] New tests for IsReady() at boundaries (0, 9, 10, 11 samples)
- [ ] Log shows "baseline ready" when samples >= threshold

**Files Modified:**
- amt_core.h (add enum)
- AMT_Snapshots.h (add IsReady method)
- AMT_config.h (add constants)

**Blast Radius:** Minimal - new code only, no behavior changes.

---

### Stage 2: Remove Fallbacks in RollingDist (MEDIUM RISK)

**Goal:** Make RollingDist methods return explicit INVALID indicators.

**Changes:**
1. Change `percentile()` to return -1.0 when empty (not 50.0)
2. Change `mean()` to return NaN when empty (not 1.0)
3. Change `percentileRank()` to return -1.0 when empty (not 50.0)
4. Add `percentileSafe()` that returns Optional<double>
5. Update all consumers to check readiness first

**Evidence Required:**
- [ ] Test: Empty RollingDist.percentile() returns -1.0
- [ ] Test: Empty RollingDist.mean() returns NaN
- [ ] All consumer sites check IsReady() before calling
- [ ] Logs show "BASELINE_NOT_READY" diagnostic when unready
- [ ] No fabricated 50.0 values in confidence attributes

**Files Modified:**
- AMT_Snapshots.h (change return values)
- AMT_LoggingContext.h (add readiness gates)
- AuctionSensor_v1.cpp (add readiness gates)

**Blast Radius:** Medium - consumers must be updated together.

---

### Stage 3: Validity Flags in ConfidenceAttribute (MEDIUM RISK)

**Goal:** Add explicit validity flags for all confidence metrics.

**Changes:**
1. Extend `ConfidenceAttribute` with validity flags:
   - `bool deltaConfValid = false`
   - `bool liquidityValid = false`
   - `bool facilitationValid = false`
   - `bool volumeProfileClarityValid = false`
   - `bool domStrengthValid = false`
2. Update `calculate_score()` to exclude invalid metrics and renormalize
3. Update all sites that write confidence values to set validity flags

**Evidence Required:**
- [ ] Test: Invalid metric excluded from score calculation
- [ ] Test: Renormalization when 1 of 5 metrics invalid
- [ ] Test: All metrics invalid returns 0.0 (not divide by zero)
- [ ] Logs show "LIQ=N/A" when liquidityValid=false

**Files Modified:**
- AMT_Patterns.h (extend struct)
- AMT_LoggingContext.h (set flags)
- AuctionSensor_v1.cpp (set flags)

**Blast Radius:** Medium - struct change requires recompile of all consumers.

---

### Stage 4: Add Robust Stats (LOW RISK)

**Goal:** Add IQR and improve MAD handling without changing decisions.

**Changes:**
1. Add `iqr()` method to RollingDist
2. Add `p25()` and `p75()` percentile methods
3. Add model-type-aware dispersion accessor
4. Update diagnostics to log robust stats

**Evidence Required:**
- [ ] Test: IQR of [1,2,3,4,5,6,7,8,9,10] = 5.0 (p75=7.5, p25=2.5)
- [ ] Test: MAD scaling factor 1.4826 applied correctly
- [ ] Diagnostic logs show median/MAD/IQR alongside mean

**Files Modified:**
- AMT_Snapshots.h (add methods)
- AuctionSensor_v1.cpp (update diagnostics)

**Blast Radius:** Low - additive only.

---

### Stage 5: ToD Bucketing Verification (LOW RISK)

**Goal:** Verify session-type baselines are correctly separated.

**Changes:**
1. Add diagnostic logging of baseline bucket routing
2. Verify RTH bars → RTH baseline, GBX bars → GBX baseline
3. Add cross-contamination detection

**Evidence Required:**
- [ ] Log shows "BASELINE_ROUTE: RTH -> ctx_rth" for RTH bars
- [ ] Log shows "BASELINE_ROUTE: GBX -> ctx_globex" for GBX bars
- [ ] No RTH data in GBX baseline during mixed session chart

**Files Modified:**
- AuctionSensor_v1.cpp (add routing diagnostics)

**Blast Radius:** Minimal - logging only.

---

## Appendix: Evidence Templates

### Stage 1 Test Output (Expected)
```
[TEST] RollingDist.IsReady(10)
  samples=0:  IsReady=false  PASS
  samples=9:  IsReady=false  PASS
  samples=10: IsReady=true   PASS
  samples=11: IsReady=true   PASS
```

### Stage 2 Runtime Log (Expected)
```
[BASELINE] Bar 50 | depth_mass_core samples=8/10 | NOT_READY - skipping liquidityAvailability
[BASELINE] Bar 100 | depth_mass_core samples=15/10 | READY | median=512.3 MAD=45.2
```

### Stage 3 Score Calculation Log (Expected)
```
[SCORE] Bar 150 | dom=0.8(V) delta=0.6(V) profile=0.7(V) tpo=0.5(V) liquidity=N/A(I)
[SCORE] Renormalized: sum=2.35 / weight=0.90 = 0.6889
```
