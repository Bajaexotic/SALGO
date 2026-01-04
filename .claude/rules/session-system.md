# AMT Framework - Session System

## Session Management SSOT Contract

**Full specification:** [`docs/ssot_session_contract.md`](../docs/ssot_session_contract.md)

Key points:
- **Session Identity**: `SessionManager` owns `currentSession`/`previousSession` (SessionKey)
- **Session Levels**: `SessionManager` owns `sessionPOC`/`sessionVAH`/`sessionVAL`
- **Computation**: All SessionKey values via `ComputeSessionKey()` only
- **Transition Detection**: Use `sessionMgr.DidSessionChange()`, not legacy `phaseCoordinator.DidSessionTypeChange()`

---

## SSOT: Session Phase

| SSOT Owner | Location |
|------------|----------|
| **`phaseCoordinator`** | `ZoneRuntime` struct in .cpp |

**DRY Helper (PREFERRED):**
```cpp
st->SyncSessionPhase(newPhase);  // Atomically syncs all 3 consumers
```

**Manual sync (if helper unavailable):**
```cpp
st->phaseCoordinator.UpdatePhase(newPhase);          // SSOT write
st->sessionMgr.activePhase = newPhase;               // Sync 1
st->amtContext.session = newPhase;                   // Sync 2
st->sessionVolumeProfile.session_phase = curPhase;   // Sync 3 (on reset only)
```

**Common violation:** Reading `amtContext.session` without syncing it from `phaseCoordinator`.

---

## SSOT: Session Extremes (High/Low)

| SSOT Owner | Location |
|------------|----------|
| **`StructureTracker`** | `ZoneManager.structure` (AMT_Zones.h) |

**Data source:** Bar High/Low (`sc.High[idx]`/`sc.Low[idx]`), NOT Close price.

**Update site (single writer):**
```cpp
structure.UpdateExtremes(probeHigh, probeLow, currentBar);  // AuctionSensor_v1.cpp:2579
```

**Accessor boundary:**
```cpp
zm.GetSessionHigh()              // Returns structure.sessionHigh
zm.GetSessionLow()               // Returns structure.sessionLow
zm.IsHighUpdatedRecently(bar)    // Checks structure.sessionHighBar
zm.IsExtremeUpdatedRecently(bar) // Combined check
```

**Common violation (now fixed):** Using Close-based extremes from SessionContext - these were semantically wrong and have been removed.

---

## SSOT: Session Start Bar

| SSOT Owner | Location |
|------------|----------|
| **`SessionManager.sessionStartBar`** | AMT_Session.h |

**Update site (single writer):**
```cpp
// AuctionSensor_v1.cpp - in session transition block
st->sessionMgr.sessionStartBar = sc.Index;
```

**Consumer:**
```cpp
// AMT_Updates.h - UpdateZoneComplete() receives as parameter
int sessionBars = bar - sessionStartBar + 1;  // For volume ratio calculation
```

**Common violation (now fixed):** ZoneSessionState had its own `sessionStartBar` that bypassed SessionManager. This has been removed - all consumers now read from SessionManager.

---

## Session Phase Detection

```cpp
// PREFERRED: Use DetermineSessionPhase wrapper (drift-proof, accepts INCLUSIVE end)
// Input[1] returns 58499 (16:14:59 INCLUSIVE) - pass directly, wrapper adds +1 internally
const AMT::SessionPhase newPhase = DetermineSessionPhase(tSec, rthStartSec, rthEndSec);
st->SyncSessionPhase(newPhase);  // Atomically syncs phaseCoordinator + all consumers

// DEPRECATED: DetermineExactPhase - requires manual +1 conversion (error-prone)
// const AMT::SessionPhase newPhase = DetermineExactPhase(tSec, rthStartSec, rthEndSec + 1, gbxStartSec);
// NOTE: 4th parameter (gbxStartSec) is unused - kept for signature compatibility only
```

---

## SessionPhase Buckets (7 tradeable)

- GLOBEX (18:00-03:00)
- LONDON_OPEN (03:00-08:30)
- PRE_MARKET (08:30-09:30)
- INITIAL_BALANCE (09:30-10:30)
- MID_SESSION (10:30-15:30)
- CLOSING_SESSION (15:30-16:15)
- POST_CLOSE (16:15-17:00)
