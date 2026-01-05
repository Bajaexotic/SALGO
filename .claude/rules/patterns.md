# AMT Framework - Patterns & DRY Violations

## Common Patterns

### Zone Proximity Check
```cpp
ZoneRuntime* nearest = zm.GetStrongestZoneAtPrice(price, tickSize, toleranceTicks);
if (nearest && nearest->proximity == ZoneProximity::AT_ZONE) { ... }
```

### Windows Macro Protection
Always use parentheses around `std::min`/`std::max`:
```cpp
int val = (std::min)(a, b);  // Prevents Windows min/max macro interference
```

---

## DRY Violations to Avoid

### 1. String Temporaries with .c_str()
```cpp
// BAD - dangling pointer
const char* str = SomeFunction().c_str();  // std::string destroyed!

// GOOD - store string first
std::string storage = SomeFunction();
const char* str = storage.c_str();
```

### 2. Duplicate Session Phase Storage
Don't add new session phase storage. Use `phaseCoordinator.GetPhase()`.

### 3. Zone Creation Bypass
Always use `CreateZonesFromProfile()` - never create zones manually and forget to set anchor IDs.

### 4. Baseline Calculations
Use `BaselineEngine` members:
- `vol_sec`, `delta_pct`, `trades_sec` (effort)
- `depth_mass_core`, `depth_mass_halo`, `stack_rate`, `pull_rate` (liquidity)
- `escape_velocity`, `time_in_zone` (zone behavior)

NOT `depth` (doesn't exist).

### 5. Historical Depth API (c_ACSILDepthBars)
```cpp
// BAD - creates crossed markets (BestBid > BestAsk)
const int bidQty = p_DepthBars->GetLastBidQuantity(bar, tick);
const int askQty = p_DepthBars->GetLastAskQuantity(bar, tick);
// Both can be non-zero at same price from different moments!

// GOOD - use GetLastDominantSide to classify first
const BuySellEnum side = p_DepthBars->GetLastDominantSide(bar, tick);
if (side == BSE_BUY) {
    bidQty = p_DepthBars->GetLastBidQuantity(bar, tick);
} else if (side == BSE_SELL) {
    askQty = p_DepthBars->GetLastAskQuantity(bar, tick);
}
```

### 6. Zone Clearing (DRY Helper Available)
```cpp
// BAD - manual 3-line pattern (can forget anchor reset)
zm.ForceFinalizePendingEngagements(bar, time, reason);
zm.activeZones.clear();
zm.anchors.Reset();  // Easy to forget!

// GOOD - use atomic helper (AMT_Zones.h)
zm.ClearZonesOnly(bar, time, reason);  // Finalizes + clears + resets anchors atomically
```

### 7. Session Phase Sync (DRY Helper Available)
```cpp
// BAD - manual 3-line sync (can miss a consumer)
st->phaseCoordinator.UpdatePhase(newPhase);
st->sessionMgr.activePhase = newPhase;
st->amtContext.session = newPhase;

// GOOD - use atomic helper (StudyState method)
st->SyncSessionPhase(newPhase);  // Updates all 3 SSOT consumers atomically
```

### 8. Redundant Loop Variable Declarations
Don't redeclare variables that are already in scope from the loop header.

```cpp
// BAD - redeclaring isLiveBar inside nested blocks
const bool isLiveBar = (curBarIdx == sc.ArraySize - 1);  // Line 3296 (loop header)
// ... later in same loop body ...
{
    const bool isLiveBar = (curBarIdx == sc.ArraySize - 1);  // REDUNDANT - shadows outer
    if (isLiveBar && condition) { ... }
}

// GOOD - use the loop-scoped variable directly
const bool isLiveBar = (curBarIdx == sc.ArraySize - 1);  // Line 3296 (loop header)
// ... later in same loop body ...
{
    if (isLiveBar && condition) { ... }  // Uses outer scope variable
}
```

**Key variables defined once at loop start:**
- `curBarIdx` - Current bar index (works for both AutoLoop modes)
- `isLiveBar` - True when processing the forming bar (`curBarIdx == sc.ArraySize - 1`)
- `inLogWindow` - True when bar is in logging window

**Exception:** Helper functions (e.g., `UpdateSessionBaselines()`) have their own scope
and require their own declarations if they don't receive these as parameters.

---

## AutoLoop Patterns

### AutoLoop Mode Support
The study supports both `AutoLoop=1` (Sierra handles iteration) and `AutoLoop=0` (manual loop):

```cpp
#define USE_MANUAL_LOOP 0  // Toggle at compile time

#if USE_MANUAL_LOOP
    sc.AutoLoop = 0;
#else
    sc.AutoLoop = 1;
#endif
```

### Bar Index Abstraction
Always use `curBarIdx` instead of `sc.Index` directly to support both modes:

```cpp
#if USE_MANUAL_LOOP
    const int curBarIdx = BarIndex;                           // Manual loop variable
    const bool isLiveBar = (curBarIdx == sc.ArraySize - 1);   // Redefine for this iteration
#else
    const int curBarIdx = sc.Index;                           // AutoLoop provides this
    // isLiveBar already defined before loop
#endif
```

### Key AutoLoop Variables

| Variable | Definition | Purpose |
|----------|------------|---------|
| `sc.Index` | Current bar (AutoLoop=1 only) | Bar being processed |
| `sc.ArraySize` | Total bars in chart | For bounds checking |
| `sc.UpdateStartIndex` | First bar needing update | Incremental update start |
| `sc.IsFullRecalculation` | True during full recalc | Gate initialization code |

### Incremental Update Detection
```cpp
// Full recalculation: sc.UpdateStartIndex == 0
if (sc.UpdateStartIndex == 0) {
    // One-time initialization code here
}

// Manual loop bounds
for (int BarIndex = sc.UpdateStartIndex; BarIndex < sc.ArraySize; BarIndex++)
```

### Live Bar Detection
Always use `sc.ArraySize - 1` comparison, never `sc.Index` alone:

```cpp
// CORRECT - works in both modes
const bool isLiveBar = (curBarIdx == sc.ArraySize - 1);

// WRONG - only works in AutoLoop=1
const bool isLiveBar = (sc.Index == sc.ArraySize - 1);  // Fails in manual loop
```

---

## ACSIL Multi-Instance Safety

### 9. Static Local Variables (CRITICAL)

**Per ACSIL Programming Concepts:** Static local variables are shared across ALL study instances
in the DLL. This causes cross-chart interference when multiple charts use the same study.

```cpp
// BAD - shared across all instances (causes cross-chart pollution)
static int lastLoggedBar = -1;
if (curBarIdx - lastLoggedBar > 100) {
    lastLoggedBar = curBarIdx;  // Affects ALL charts!
}

// GOOD - use StudyState struct member (per-instance)
// In StudyState struct:
int diagLastLoggedBar = -1;

// In study function:
if (curBarIdx - st->diagLastLoggedBar > 100) {
    st->diagLastLoggedBar = curBarIdx;  // Only affects THIS chart
}
```

**Alternatives for per-instance storage:**
- `StudyState*` via `GetPersistentPointer()` (recommended)
- `sc.GetPersistentInt()` / `sc.GetPersistentFloat()` / `sc.GetPersistentDouble()`
- `sc.GetPersistentSCString()`

**Rate-limiting variables in StudyState:**
```cpp
struct StudyState {
    // Per-instance rate-limiting (NOT static)
    int diagLastValidationBar = -1;
    int diagLastBaselineLogBar = -1;
    int diagLastViolationBar = -1;
    int diagLastDepthDiagBar = -1;
    // ... etc
};
```

---

## ACSIL Study Lifecycle Patterns

### 10. Historical Data Download Check

Skip heavy processing while Sierra Chart is downloading historical data:

```cpp
// After sc.LastCallToFunction check, before main processing
if (sc.DownloadingHistoricalData)
{
    return;  // Skip processing during download
}
```

**Why?** Heavy calculations during download can cause performance issues.
The study will be called again with a full recalculation after download completes.

### 11. Last Call Cleanup

Always handle `sc.LastCallToFunction` for proper cleanup:

```cpp
if (sc.LastCallToFunction)
{
    StudyState* st = static_cast<StudyState*>(sc.GetPersistentPointer(1));
    if (st != nullptr)
    {
        // Cleanup: close files, flush buffers, etc.
        st->logManager.Shutdown();

        delete st;
        sc.SetPersistentPointer(1, nullptr);
    }
    return;
}
```

### 12. Persistent Pointer Pattern

Use `GetPersistentPointer` for per-instance dynamic memory:

```cpp
// Allocate on first call
StudyState* st = static_cast<StudyState*>(sc.GetPersistentPointer(1));
if (st == nullptr)
{
    st = new StudyState();
    sc.SetPersistentPointer(1, st);
}

// Reset on full recalculation
if (sc.IsFullRecalculation && sc.UpdateStartIndex == 0)
{
    st->resetAll(baselineWindow);
}
```

### 13. SCString Memory Safety

Always store SCString before calling `.GetChars()`:

```cpp
// BAD - temporary destroyed before use
const char* msg = someFunction().GetChars();  // Dangling pointer!

// GOOD - store first
SCString msg = someFunction();
sc.AddMessageToLog(msg.GetChars(), 0);
```

Same pattern applies to `std::string::c_str()` (see #1 above).
