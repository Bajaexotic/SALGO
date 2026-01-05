# AMT Framework - Market State & Current Phase

## Balance/Imbalance (Market State) - Regime SSOT

| SSOT Owner | Location |
|------------|----------|
| **`AMTMarketState`** | `DaltonEngine.phase` -> `StateEvidence.currentState` |

**Per Dalton: 1TF/2TF is the DETECTION MECHANISM for Balance/Imbalance.**
- **1TF (One-Time Framing)** = IMBALANCE (one side in control, trending, ~20%)
- **2TF (Two-Time Framing)** = BALANCE (both sides active, rotation, ~80%)

**Activity classification** (INITIATIVE/RESPONSIVE) determines WHO is in control, not WHAT the state is.

---

## REMOVED Regime Types (Migration Complete Dec 2024)

| Removed | Replacement |
|---------|-------------|
| `BarRegime` / `MarketState` | `AMTMarketState` (SSOT via Dalton 1TF/2TF) |
| `MapAMTStateToLegacy()` | Removed - all code uses `AMTMarketState` directly |
| `AuctionRegime.EXCESS` | `CurrentPhase::FAILED_AUCTION` (derived from `ExcessType`) |
| `AuctionRegime.REBALANCE` | PULLBACK phase within IMBALANCE state |
| `AuctionRegime.BALANCE/IMBALANCE` | `AMTMarketState.BALANCE/IMBALANCE` |

**Per AMT Framework:** The fundamental model is TWO states (Balance/Imbalance).
EXCESS and REBALANCE are phase conditions, not separate states.
All 500+ usages have been migrated to use `AMTMarketState` directly.

---

## Data Flow

```cpp
// STEP 1: Dalton computes state via 1TF/2TF (SSOT)
st->lastDaltonState = st->daltonEngine.ProcessBar(...);
AMT::AMTMarketState daltonState = st->lastDaltonState.phase;

// STEP 2: Signal engine receives authoritative state
evidence = st->amtSignalEngine.ProcessBar(..., daltonState);
// evidence.currentState = daltonState (not accumulator-computed)
```

**Strength accumulator is now a CONFIRMATION metric:**
- Tracks activity-based strength for diagnostics
- Does NOT determine state (Dalton does)
- Falls back to accumulator only when daltonState == UNKNOWN (warmup)

**Common violation:** Using accumulator-based state instead of Dalton's 1TF/2TF.

---

## CurrentPhase (UNIFIED SSOT - Dalton Only)

| SSOT Owner | Location |
|------------|----------|
| **`DaltonState.DeriveCurrentPhase()`** | `lastDaltonState.DeriveCurrentPhase()` |

**SSOT UNIFICATION (Dec 2024):** `daltonPhase` is now the SINGLE authoritative
source for CurrentPhase. The legacy `ComputeRawPhase()` function is DEPRECATED.
`BuildPhaseSnapshot()` now REQUIRES `daltonPhase` parameter - no fallback.

---

## Phase Derivation Priority

Phase derivation uses (in priority order):
1. **Failed Auction** - `failedAuctionAbove/Below` OR `excess != NONE`
2. **Testing Boundary** - At VAH/VAL (probing)
3. **Balance -> Rotation** - 2TF inside value
4. **Imbalance -> one of:**
   - **Pullback** - Responsive activity within imbalance
   - **Range Extension** - At extreme + initiative + IB broken
   - **Driving Up/Down** - Directional imbalance (1TF)

---

## Phase Enum Values

```cpp
ROTATION         // BALANCE state (2TF, inside value)
DRIVING_UP       // IMBALANCE (1TF bullish, buyers in control)
DRIVING_DOWN     // IMBALANCE (1TF bearish, sellers in control)
RANGE_EXTENSION  // IMBALANCE + at extreme + initiative
PULLBACK         // IMBALANCE + responsive (counter-move in trend)
TESTING_BOUNDARY // At VA edge (VAH/VAL)
FAILED_AUCTION   // Excess/rejection detected
ACCEPTING_VALUE  // Consolidating in new value area
```

---

## Data Flow

```cpp
// STEP 1: Dalton derives state and phase (SSOT)
daltonState = st->lastDaltonState.phase;
daltonPhase = st->lastDaltonState.DeriveCurrentPhase();

// STEP 2: BuildPhaseSnapshot receives daltonPhase (REQUIRED, no fallback)
amtSnapshot = BuildPhaseSnapshot(zm, currentPrice, closePrice, tickSize,
                                  currentBar, tracker, daltonState, daltonPhase);
// amtSnapshot.rawPhase = daltonPhase (from Dalton SSOT)
// amtSnapshot.phase = PhaseTracker.Update(daltonPhase) (hysteresis only)
```

**REMOVED:** `ComputeRawPhase()` is deprecated. Do NOT call it directly.
Legacy `BuildPhaseSnapshot` overloads without daltonPhase have been REMOVED.

**Logging format:** `PHASE: DALTON=%s CONF=%s` (was `RAW=`)

**Common violation:** Calling `BuildPhaseSnapshot` without providing daltonPhase.

---

## Session Bridge (SSOT - DaltonEngine, Jan 2025)

| SSOT Owner | Location |
|------------|----------|
| **`DaltonEngine.bridge_`** | `AMT_Dalton.h` (SessionBridge struct) |

**DaltonEngine owns ALL session bridge information:**
- **Overnight Session** - GLOBEX extremes (ON_HI, ON_LO, ON_POC), mini-IB, 1TF/2TF pattern
- **Overnight Inventory** - Net position (NET_LONG/NET_SHORT/NEUTRAL), score [-1,+1]
- **Gap Context** - Gap type (TRUE_GAP/VALUE_GAP/NO_GAP), size, fill status
- **Opening Type** - Dalton's 4 types (Open-Drive, Open-Test-Drive, Open-Rejection-Reverse, Open-Auction)
- **Prior RTH Context** - Prior session's High/Low/Close/POC/VAH/VAL (for gap calculation)

### Data Flow

```cpp
// RTH → GLOBEX transition: Capture prior RTH
st->sessionMgr.CapturePriorRTH(rthHigh, rthLow, rthClose);  // Staging
st->daltonEngine.SetPriorRTHContext(...);                    // SSOT write

// GLOBEX → RTH transition: Capture overnight, classify gap
st->daltonEngine.CaptureOvernightSession(on);
st->daltonEngine.ClassifyGap(rthOpenPrice, tickSize);

// During RTH first 30 min: Classify opening type
st->daltonEngine.UpdateOpeningClassification(...);

// Read SSOT
const SessionBridge& bridge = st->daltonEngine.GetSessionBridge();
const OpeningType openType = st->lastDaltonState.openingType;
```

### SessionManager Role (Staging Only)

`SessionManager.CapturePriorRTH()` stages prior RTH levels at transition time.
These are immediately passed to `DaltonEngine.SetPriorRTHContext()` which becomes SSOT.
**Always read from `DaltonEngine.GetSessionBridge()`, not SessionManager.**

### Opening Type Classification Window

Opening type is classified in first 30 minutes of RTH:
- `OPEN_DRIVE_UP/DOWN` - Strong directional, no return to open
- `OPEN_TEST_DRIVE_UP/DOWN` - Test one side, reverse, then drive
- `OPEN_REJECTION_REVERSE_UP/DOWN` - Test extreme, reject, reverse
- `OPEN_AUCTION` - Rotational, probing both sides

**Common violation:** Reading prior RTH from SessionManager instead of DaltonEngine bridge.

---

## Activity Classification (SSOT - AMT_Signals.h, Jan 2025)

| SSOT Owner | Location |
|------------|----------|
| **`AMTActivityType`** | `AMT_Signals.h` -> `StateEvidence.activity.activityType` |

**Initiative vs Responsive is LOCATION-GATED per Dalton's Market Profile:**
- **INITIATIVE** = Away from value + Aggressive participation (OTF conviction)
- **RESPONSIVE** = Toward value OR Absorptive (reversion, defensive)
- **NEUTRAL** = At value with balanced participation

### Data Flow

```cpp
// STEP 1: AMTSignalEngine computes activity type (SSOT)
// This is LOCATION-GATED: intent (toward/away from value) + participation
evidence = st->amtSignalEngine.ProcessBar(...);
// evidence.activity.activityType = location-gated classification

// STEP 2: Map to legacy AggressionType for downstream consumers
AggressionType aggression = MapAMTActivityToLegacy(
    evidence.activity.activityType);
ctxInput.ssotAggression = aggression;
ctxInput.ssotAggressionValid = evidence.activity.valid;

// STEP 3: ContextBuilder CONSUMES SSOT (does NOT compute its own)
ctx.aggression = in.ssotAggression;
ctx.aggressionValid = in.ssotAggressionValid;
```

### REMOVED: Delta-Only Classification (Jan 2025)

| Removed | Replacement |
|---------|-------------|
| `ArbitrationResult.detectedAggression` | SSOT from `AMT_Signals.h` |
| `ArbitrationResult.directionalCoherence` | Removed (only used for dead aggression code) |
| ContextBuilder delta-only computation | Consumes SSOT from input |

The delta-only classification (`isExtremeDelta && directionalCoherence`) was WRONG because
it ignored WHERE price was relative to value area - only Dalton's location-gated definition
correctly distinguishes Initiative from Responsive activity.

### ArbitrationSeam Scope (Clarified)

The ArbitrationSeam provides **delta primitives** for zone arbitration:
- `isExtremeDeltaBar` - Per-bar extreme one-sided delta
- `isExtremeDeltaSession` - Session extreme magnitude percentile
- `isExtremeDelta` - Combined (bar && session)
- `rawState` - Balance/Imbalance (from Dalton or legacy fallback)

Activity classification (Initiative/Responsive) is **NOT** in scope for ArbitrationSeam.

**Common violation:** Computing activity type from delta alone without considering price location.
