# AMT Framework - Contract Changes (December 2024)

## A. Session-Scoped Delta as First-Class Decision Input

**Location:** `AMT_Session.h:417-446`, `AMT_Arbitration_Seam.h:31-50`

### Definitions

- `sessionCumDelta := NB_cumDelta_now - cumDeltaAtSessionStart`
- `sessionDeltaPct := sessionCumDelta / max(sessionTotalVolume, 1.0)`

### New Extreme Delta Definition (Persistence-Validated)

```cpp
isExtremeDeltaBar    := (deltaConsistency > 0.7)    // Per-bar: 70%+ one-sided
isExtremeDeltaSession := (sessionDeltaPctile >= 85) // Session: top 15% MAGNITUDE
isExtremeDelta       := isExtremeDeltaBar && isExtremeDeltaSession  // Combined
```

### Magnitude-Only Extremeness

- Distribution stores `|sessionDeltaPct|` (absolute value)
- Query uses `|sessionDeltaPct|` (absolute value)
- Threshold `>= 85` detects extreme magnitude in EITHER direction (buying or selling)
- Direction is handled separately by the coherence check

This eliminates false positives from single-bar delta spikes that lack session conviction.

### Directional Coherence (for Aggression Classification)

- INITIATIVE requires `isExtremeDelta && directionalCoherence`
- Coherence = session delta sign matches bar delta direction
- Incoherent extreme -> RESPONSIVE (absorption, not attack)

---

## B. PRIOR VBP Tri-State Contract

**Location:** `amt_core.h:74-87`, `AMT_Zones.h:1440-1458`, `AMT_Updates.h:728-806`

### Enum

```cpp
enum class PriorVBPState : int {
    PRIOR_VALID = 0,             // Prior exists and differs from current
    PRIOR_MISSING = 1,           // Insufficient history (not a bug)
    PRIOR_DUPLICATES_CURRENT = 2 // Prior exists but matches current (defect)
};
```

### Behavior

- `PRIOR_MISSING`: Degraded mode, skip prior zones, log once per session
- `PRIOR_VALID`: Create PRIOR zones normally
- `PRIOR_DUPLICATES_CURRENT`: Log as BUG with diagnostic context for reproduction

---

## C. Safety Invariants

**Location:** `AuctionSensor_v1.cpp:3360-3372`

Runtime assertions (gated diagnostics):
1. `sessionDeltaPctile` must be in [0, 100] - clamped with error log if violated
2. `sessionVol` source is always `sessionAccum.sessionTotalVolume`
3. `cumDeltaAtSessionStart = NB_cumDelta - SC_barDelta` at session boundaries (captures actual baseline)
