# AMT Migration Plan: Old System → New AMT Signals

## Executive Summary

This plan covers migration from the old phase/state system to the new AMT-based signal system in three phases:
1. **Shadow Mode** - Run both systems, compare outputs, validate new system
2. **Replace Logic** - Use new AMT signals for decisions
3. **Remove Expired** - Clean up old code

---

## Current State Analysis

### OLD SYSTEM (to be replaced)
| Component | Location | Purpose |
|-----------|----------|---------|
| `MarketStateTracker` | AMT_Analytics.h:367 | Phase-bucketed hysteresis for BALANCE/IMBALANCE |
| `MarketState` (BarRegime) | amt_core.h | BALANCE/IMBALANCE enum |
| `AggressionType` | AMT_Patterns.h | INITIATIVE/RESPONSIVE/NEUTRAL |
| `amtContext.state` | AuctionSensor_v1.cpp:7597 | Confirmed market state |
| `amtContext.aggression` | AuctionSensor_v1.cpp:7591 | Direction-based aggression |
| `amtSnapshot.regime` | From PhaseTracker | 4-state regime (BALANCE/IMBALANCE/EXCESS/REBALANCE) |

### NEW SYSTEM (implemented)
| Component | Location | Purpose |
|-----------|----------|---------|
| `AMTSignalEngine` | AMT_Signals.h | Unified signal processor |
| `AMTMarketState` | amt_core.h:1159 | BALANCE/IMBALANCE (leaky accumulator) |
| `AMTActivityType` | amt_core.h:1217 | INITIATIVE/RESPONSIVE/NEUTRAL (value-relative) |
| `ValueIntent` | amt_core.h:1181 | TOWARD_VALUE/AWAY_FROM_VALUE/AT_VALUE |
| `ParticipationMode` | amt_core.h:1199 | AGGRESSIVE/ABSORPTIVE/BALANCED |
| `ExcessType` | amt_core.h:1251 | NONE/POOR_HIGH/POOR_LOW/EXCESS_HIGH/EXCESS_LOW |
| `StateEvidence` | amt_core.h:1377 | Full evidence ledger |

### Key Differences
| Aspect | OLD | NEW |
|--------|-----|-----|
| State tracking | Phase-bucketed hysteresis (5 consecutive bars) | Leaky accumulator (continuous strength) |
| Activity classification | Direction-based (up/down) | Value-relative (toward/away from POC) |
| Excess detection | Phase-based (FAILED_AUCTION) | Tail + confirmation (EXCESS_HIGH/LOW) |
| Single prints | None | Profile-structural detection |
| State strength | Binary (confirmed/not) | Continuous [0,1] |

---

## PHASE 1: Shadow Mode (Validation)

### Objective
Run both systems in parallel, log comparisons, identify disagreements, validate new system behavior.

### Implementation Steps

#### Step 1.1: Add Shadow Comparison Block
**Location:** AuctionSensor_v1.cpp, after AMT signal processing (~line 5473)

```cpp
// ================================================================
// SHADOW MODE: Compare old vs new AMT state
// ================================================================
if (diagLevel >= 1) {
    // Old system values
    const AMT::MarketState oldState = st->amtContext.state;
    const AMT::AggressionType oldAggression = st->amtContext.aggression;

    // New system values
    const AMT::AMTMarketState newState = st->lastStateEvidence.currentState;
    const AMT::AMTActivityType newActivity = st->lastStateEvidence.activity.activityType;

    // Compare states (map AMTMarketState to MarketState for comparison)
    bool stateMatch = (oldState == AMT::MarketState::BALANCE && newState == AMT::AMTMarketState::BALANCE) ||
                      (oldState == AMT::MarketState::IMBALANCE && newState == AMT::AMTMarketState::IMBALANCE);

    // Compare activity (INITIATIVE/RESPONSIVE)
    bool activityMatch = (oldAggression == AMT::AggressionType::INITIATIVE && newActivity == AMT::AMTActivityType::INITIATIVE) ||
                         (oldAggression == AMT::AggressionType::RESPONSIVE && newActivity == AMT::AMTActivityType::RESPONSIVE) ||
                         (oldAggression == AMT::AggressionType::NEUTRAL && newActivity == AMT::AMTActivityType::NEUTRAL);

    // Track statistics
    st->sessionAccum.shadowStateMatches += stateMatch ? 1 : 0;
    st->sessionAccum.shadowActivityMatches += activityMatch ? 1 : 0;
    st->sessionAccum.shadowComparisons++;

    // Log disagreements
    if (!stateMatch || !activityMatch) {
        SCString shadowMsg;
        shadowMsg.Format("[SHADOW] Bar %d | OLD: state=%s aggr=%s | NEW: state=%s act=%s | MATCH: state=%d act=%d",
            currentBar,
            AMT::to_string(oldState), AMT::to_string(oldAggression),
            AMT::AMTMarketStateToString(newState), AMT::AMTActivityTypeToString(newActivity),
            stateMatch ? 1 : 0, activityMatch ? 1 : 0);
        st->logManager.LogInfo(currentBar, shadowMsg.GetChars(), LogCategory::AMT);
    }
}
```

#### Step 1.2: Add Shadow Statistics to SessionAccumulators
**Location:** AMT_Session.h, SessionAccumulators struct

```cpp
// Shadow mode comparison statistics
int shadowComparisons = 0;
int shadowStateMatches = 0;
int shadowActivityMatches = 0;
```

#### Step 1.3: Log Shadow Summary in Stats Block
**Location:** AuctionSensor_v1.cpp, stats block section

```cpp
// Shadow mode summary
if (st->sessionAccum.shadowComparisons > 0) {
    const double stateMatchRate = 100.0 * st->sessionAccum.shadowStateMatches / st->sessionAccum.shadowComparisons;
    const double activityMatchRate = 100.0 * st->sessionAccum.shadowActivityMatches / st->sessionAccum.shadowComparisons;
    msg.Format("SHADOW: n=%d stateMatch=%.1f%% actMatch=%.1f%%",
        st->sessionAccum.shadowComparisons, stateMatchRate, activityMatchRate);
    st->logManager.LogToSC(LogCategory::AMT, msg, false);
}
```

#### Step 1.4: Validation Criteria
- **State agreement rate > 80%** - Systems should largely agree on BALANCE/IMBALANCE
- **Activity agreement rate > 70%** - Some divergence expected due to different classification logic
- **Transition alignment** - Both systems should detect transitions at similar times
- **Review disagreement patterns** - Identify systematic differences

### Duration
Run shadow mode for 2-3 trading sessions, review logs, tune if needed.

---

## PHASE 2: Replace Old Logic

### Objective
Replace old state/aggression usage with new AMT signals for actual decisions.

### Dependency Analysis

#### 2.1 State Usage Sites (AuctionSensor_v1.cpp)

| Line | Usage | Action |
|------|-------|--------|
| 6236 | Stats logging `amtContext.state` | Replace with `lastStateEvidence.currentState` |
| 6834 | ContextBuilder `confirmedState` | Replace source |
| 7576-7598 | MarketStateTracker update | **REMOVE** (new system handles) |

#### 2.2 Aggression Usage Sites

| Line | Usage | Action |
|------|-------|--------|
| 6238 | Stats logging `amtContext.aggression` | Replace with `lastStateEvidence.activity.activityType` |
| 7591 | Read from context | Replace source |
| 7648 | SessionEvent logging | Update to use new activity type |

#### 2.3 Subgraph Output (Decision Point)

| Line | Usage | Action |
|------|-------|--------|
| 5786 | `Subgraph[13]` = old state | Replace with new AMTMarketState |
| 5808 | Same for current bar | Replace with new AMTMarketState |

### Implementation Steps

#### Step 2.1: Add Mapping Functions
**Location:** amt_core.h (after new enums)

```cpp
// Mapping helpers for gradual migration
inline MarketState MapAMTStateToLegacy(AMTMarketState s) {
    switch (s) {
        case AMTMarketState::BALANCE: return MarketState::BALANCE;
        case AMTMarketState::IMBALANCE: return MarketState::IMBALANCE;
        default: return MarketState::UNDEFINED;
    }
}

inline AggressionType MapAMTActivityToLegacy(AMTActivityType a) {
    switch (a) {
        case AMTActivityType::INITIATIVE: return AggressionType::INITIATIVE;
        case AMTActivityType::RESPONSIVE: return AggressionType::RESPONSIVE;
        default: return AggressionType::NEUTRAL;
    }
}
```

#### Step 2.2: Replace State Source
**Location:** AuctionSensor_v1.cpp

**Before (line ~7597):**
```cpp
st->amtContext.state = confirmedState;
st->amtContext.stateValid = (confirmedState != AMT::MarketState::UNDEFINED);
```

**After:**
```cpp
// NEW: Use AMT signal engine state as SSOT
st->amtContext.state = AMT::MapAMTStateToLegacy(st->lastStateEvidence.currentState);
st->amtContext.stateValid = (st->lastStateEvidence.currentState != AMT::AMTMarketState::UNKNOWN);
```

#### Step 2.3: Replace Aggression Source
**Before:**
```cpp
AMT::AggressionType detectedAggression = st->amtContext.aggression;
```

**After:**
```cpp
// NEW: Use AMT activity classification
AMT::AggressionType detectedAggression = AMT::MapAMTActivityToLegacy(st->lastStateEvidence.activity.activityType);
st->amtContext.aggression = detectedAggression;
st->amtContext.aggressionValid = st->lastStateEvidence.activity.valid;
```

#### Step 2.4: Update Subgraph Output
```cpp
// Use new state for subgraph
const int stateInt = static_cast<int>(st->lastStateEvidence.currentState);
sc.Subgraph[13][curBarIdx] = static_cast<float>(stateInt);
```

#### Step 2.5: Update ContextBuilder Input
**Location:** AMT_ContextBuilder.h

Update `ContextBuilderInput` to accept new state type or use mapped value.

### Verification
- Run all existing tests
- Compare outputs before/after
- Verify no behavior regression

---

## PHASE 3: Remove Expired Metrics

### Objective
Clean up old code that's no longer needed after migration.

### Components to Remove

#### 3.1 MarketStateTracker (AMT_Analytics.h)
- `MarketStateBucket` struct (lines ~290-360)
- `MarketStateTracker` struct (lines 367-437)
- Remove from StudyState

**BUT KEEP:** Phase-based statistics if still useful for historical analysis.

#### 3.2 Old State Computation (AuctionSensor_v1.cpp)
- Lines 7575-7582: MarketStateTracker.Update() calls
- Lines 4262-4265: FinalizeAllPhases/ResetForSession
- Lines 2119: SetPriorFromHistory

#### 3.3 Old Aggression Computation
- Direction-based aggression logic in ContextBuilder (if any)
- Coherence-based aggression signals that are now replaced

#### 3.4 Unused Enums (amt_core.h)
**DO NOT REMOVE YET:**
- `MarketState` enum - May still be used by external code
- `AggressionType` enum - May still be used for logging

**Consider deprecation warnings instead:**
```cpp
// [[deprecated("Use AMTMarketState instead")]]
enum class MarketState : int { ... };
```

### Implementation Steps

#### Step 3.1: Remove MarketStateTracker from StudyState
```cpp
// REMOVED: AMT::MarketStateTracker marketStateTracker;
```

#### Step 3.2: Remove MarketStateTracker Calls
Search and remove all `st->marketStateTracker.` calls.

#### Step 3.3: Remove Unused Session Boundary Logic
```cpp
// REMOVED: st->marketStateTracker.FinalizeAllPhases();
// REMOVED: st->marketStateTracker.ResetForSession();
```

#### Step 3.4: Clean Up Includes
Remove any includes that are no longer needed.

### Verification
- Full compilation check
- Run all tests
- Run in live environment for 1 session

---

## Timeline & Dependencies

```
PHASE 1 (Shadow Mode)     PHASE 2 (Replace)        PHASE 3 (Remove)
├─ Step 1.1: Compare      ├─ Step 2.1: Mappers     ├─ Step 3.1: Remove tracker
├─ Step 1.2: Stats        ├─ Step 2.2: State src   ├─ Step 3.2: Remove calls
├─ Step 1.3: Summary      ├─ Step 2.3: Aggr src    ├─ Step 3.3: Remove boundary
└─ Step 1.4: Validate     ├─ Step 2.4: Subgraph    └─ Step 3.4: Clean includes
                          └─ Step 2.5: Builder
        │                         │                        │
        ▼                         ▼                        ▼
   [2-3 sessions]           [After validation]       [After stable]
```

---

## Risk Mitigation

1. **Shadow mode first** - Never replace without validation
2. **Mapping functions** - Allow gradual migration, not big-bang
3. **Keep old enums** - Don't break external dependencies
4. **Feature flag** - Consider adding `USE_NEW_AMT_STATE` flag for rollback
5. **Test coverage** - Run all existing tests at each phase

---

## Success Criteria

### Phase 1
- [ ] Shadow comparison running
- [ ] State match rate > 80%
- [ ] Activity match rate > 70%
- [ ] No crashes or errors

### Phase 2
- [ ] All state references use new source
- [ ] All aggression references use new source
- [ ] Existing tests pass
- [ ] No behavior regression in logs

### Phase 3
- [ ] Old tracker removed
- [ ] Clean compilation
- [ ] All tests pass
- [ ] Live session stable
