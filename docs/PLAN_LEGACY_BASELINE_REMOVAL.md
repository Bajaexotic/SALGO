# Plan: Remove Legacy BaselineEngine, Migrate to Bucket-Based System

## Overview

The legacy `BaselineEngine` (in `SessionContext.baselines`) is a single rolling-window that mixes all phases/times together. The new system uses:

| New Component | Purpose | Location |
|---------------|---------|----------|
| `EffortBaselineStore` | Per-bucket (OPEN/MID/POWER) effort distributions from prior RTH | `StudyState.effortBaselines` |
| `SessionDeltaBaseline` | Session-aggregate delta ratio baseline | `StudyState.sessionDeltaBaseline` |
| `DOMWarmup` | Live 15-min RTH warmup for DOM metrics | `StudyState.domWarmup` |

---

## PHASE 1: Identify All Legacy Usages

### Legacy BaselineEngine Fields (AMT_Snapshots.h:497-536)
```cpp
// Effort (bar-level)
vol_sec, delta_sec, total_vol, delta_pct, max_delta, trades_sec

// Session-aggregate
session_delta_pct

// Liquidity/DOM
depth_mass_core, depth_mass_halo, stack_rate, pull_rate

// Zone behavior
escape_velocity, time_in_zone

// Facilitation
bar_range
```

### Legacy Usages in AuctionSensor_v1.cpp (by line)
| Line | Usage | Migration Target |
|------|-------|------------------|
| 184 | `using AMT::BaselineEngine;` | REMOVE |
| 356 | Comment referencing BaselineEngine | UPDATE comment |
| 372 | `BaselineEngine stats;` in SessionContext | REMOVE field |
| 1263 | `BaselineEngine& baselines = ctx.baselines;` | REMOVE |
| 3126 | `const BaselineEngine& sessionBaselines = activeCtx.baselines;` | REMOVE |
| 3136 | `sessionBaselines.checkExtremes(...)` | Migrate to new Try* APIs |
| 3179-3200 | `sessionBaselines.total_vol.*` for volume baseline | Use `EffortBaselineStore` |
| 3242-3263 | `sessionBaselines.depth_mass_core.*` for DOM | Use `DOMWarmup` |
| 3530-3594 | `sessionBaselines.total_vol/bar_range.*` for volume/range | Use `EffortBaselineStore` |
| 5571-5572 | `sessionBaselines.session_delta_pct.*` | Use `SessionDeltaBaseline` |
| 5811-5828 | Legacy diagnostic logging | REMOVE entirely |
| 5902-5904 | `sessionBaselines.session_delta_pct.*` | Use `SessionDeltaBaseline` |
| 5939-5941 | `sessionBaselines.bar_range.*` | Use `EffortBaselineStore` |
| 5960-5962 | `sessionBaselines.depth_mass_core.*` | Use `DOMWarmup` |
| 5994-5995 | `sessionBaselines.total_vol.*` | Use `EffortBaselineStore` |
| 6002-6004 | `sessionBaselines.total_vol.*` | Use `EffortBaselineStore` |
| 6806 | `activeBaselines = ...baselines` | REMOVE |

---

## PHASE 2: Migration Mapping

### Effort Metrics (vol_sec, trades_sec, delta_pct, bar_range, total_vol)
**Old:**
```cpp
sessionBaselines.total_vol.percentileRank(curBarVol)
sessionBaselines.bar_range.percentileRank(curBarRangeTicks)
```

**New:**
```cpp
// Determine current bucket from time
AMT::EffortBucket bucket = AMT::GetEffortBucketFromTime(barTimeSec, rthStartSec, rthEndSec);

// Get bucket distribution
const AMT::EffortBucketDistribution& dist = st->effortBaselines.Get(bucket);

// Use Try* APIs (return PercentileResult with valid flag)
AMT::PercentileResult volResult = dist.vol_sec.TryPercentile(curVolSec);
if (volResult.valid) {
    // Use volResult.value
}
```

### Session Delta (session_delta_pct)
**Old:**
```cpp
sessionBaselines.session_delta_pct.percentile(std::abs(sessionDeltaRatio))
```

**New:**
```cpp
AMT::PercentileResult result = st->sessionDeltaBaseline.TryPercentile(std::abs(sessionDeltaRatio));
if (result.valid) {
    sessionDeltaPctile = result.value;
}
```

### DOM Metrics (depth_mass_core, stack_rate, pull_rate)
**Old:**
```cpp
sessionBaselines.depth_mass_core.percentileRank(curDepth)
sessionBaselines.stack_rate.percentileRank(curStack)
```

**New:**
```cpp
if (st->domWarmup.IsReady()) {
    AMT::PercentileResult depthResult = st->domWarmup.depthMassCore.TryPercentile(curDepth);
    if (depthResult.valid) {
        // Use depthResult.value
    }
}
```

### GBX Policy
- Effort baselines: Return `NOT_APPLICABLE` outside RTH (bucket == OUTSIDE_RTH)
- DOM warmup: Only valid during RTH after 15-min warmup
- Session delta: RTH sessions only

---

## PHASE 3: Execution Steps

### Step 1: Remove Legacy Logging (5811-5828)
Delete the entire diagnostic block that logs legacy baseline sizes/MAD.

### Step 2: Remove BaselineEngine from SessionContext
- Remove `BaselineEngine stats;` field from SessionContext
- Remove `using AMT::BaselineEngine;`
- Remove all `sessionBaselines` references

### Step 3: Migrate Volume/Range Consumers (3179-3200, 3530-3594, 5939-5941, 5994-6004)
Replace with bucket-aware lookups using `EffortBaselineStore.Get(bucket).vol_sec/bar_range`.

### Step 4: Migrate Session Delta Consumers (5571-5572, 5902-5904)
Replace with `SessionDeltaBaseline.TryPercentile()`.

### Step 5: Migrate DOM Consumers (3242-3263, 5960-5962)
Replace with `DOMWarmup` queries (already partially done).

### Step 6: Migrate checkExtremes() (3136)
Replace with individual Try* calls and explicit gating.

### Step 7: Remove Legacy Push Sites
Find and remove all places that push to `ctx.baselines.*`.

### Step 8: Remove BaselineEngine Struct
Once no usages remain, remove `struct BaselineEngine` from AMT_Snapshots.h.

---

## PHASE 4: Verification

1. Compile successfully
2. No references to `BaselineEngine`, `sessionBaselines`, or `ctx.baselines`
3. New bucket-based logs appear instead of legacy logs
4. GBX bars show `NOT_APPLICABLE` for effort baselines
5. RTH bars show bucket-specific (OPEN/MID/POWER) baseline queries

---

## Files to Modify

| File | Changes |
|------|---------|
| `AuctionSensor_v1.cpp` | Remove all legacy usages, migrate consumers |
| `AMT_Snapshots.h` | Remove `BaselineEngine` struct after migration |
| `AMT_Session.h` | Remove `baselines` field from `SessionContext` |

---

## Estimated Scope

- ~30 legacy usage sites in AuctionSensor_v1.cpp
- 1 struct removal (BaselineEngine)
- 1 field removal (SessionContext.baselines)
- Multiple consumer migrations to Try* APIs with explicit validity checks
