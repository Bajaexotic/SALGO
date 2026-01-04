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
