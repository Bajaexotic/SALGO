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
