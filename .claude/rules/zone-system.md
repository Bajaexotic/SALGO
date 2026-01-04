# AMT Framework - Zone System

## Zone Posture (VBP + PRIOR + STRUCTURE)

**Location:** `AMT_config.h` - `ZonePosture` struct

The system operates in a strict posture that controls which zone families are instantiated:

| Family | Enabled | Zone Types | Notes |
|--------|---------|------------|-------|
| **VBP** | Yes | VPB_POC, VPB_VAH, VPB_VAL | Current session profile zones |
| **PRIOR** | Yes | PRIOR_POC, PRIOR_VAH, PRIOR_VAL | Prior session reference zones |
| **TPO** | No | TPO_* | Disabled by design |
| **STRUCTURE** | Yes (track-only) | SESSION_HIGH/LOW, IB_HIGH/LOW | Tracked for logging, not created as zones |

**Posture enforcement:**
- `g_zonePosture.IsZoneTypeAllowed(type)` gates all zone creation
- TPO zones are never created (`enableTPO = false`)
- Structure levels are tracked in `StructureTracker` but NOT created as zones by default

**Log output at init:**
```
[ZONE-POSTURE] Posture: VBP=ON PRIOR=ON TPO=OFF STRUCT=ON(track-only)
```

---

## Structure Tracking (Session Extremes + IB)

**Location:** `AMT_Zones.h` - `StructureTracker` struct (owned by `ZoneManager.structure`)

Structure tracking provides non-ambiguous logging of session extremes and IB levels:

| Field | Description |
|-------|-------------|
| `sessionHigh/Low` | Current session extremes (dynamic, update on new high/low) |
| `ibHigh/Low` | Initial Balance levels (frozen after IB window ends) |
| `ibFrozen` | True after IB window closes (default: 60 minutes into RTH) |
| `sessionRangeTicks` | Session range in ticks (for range-adaptive sizing) |
| `adaptiveCoreTicks/Halo` | Range-derived proximity thresholds |

**Log output (every session stats block):**
```
State: ZONE=NONE | SESS=PRE_MKT | FACIL=EFFICIENT | DELTA=1.00 | LIQ=0.61
Struct: SESS_HI=6100.25 SESS_LO=6095.00 DIST_HI_T=0 DIST_LO_T=21 | IB_HI=6098.50 IB_LO=6095.00 DIST_IB_HI_T=7 DIST_IB_LO_T=0 IB=FROZEN | RANGE_T=21
```

This eliminates "ZONE=NONE at session high" confusion:
- `ZONE=` shows nearest VBP/PRIOR profile zone (profile-centric selection)
- `Struct:` always shows session/IB levels with tick distances

**Range-adaptive thresholds** (logged on change):
```
[PROX-RANGE] Bar 500 | sess=RTH_AM rangeTicks=48 core=4 halo=10
```

---

## SSOT: Zone Anchor Prices (POC/VAH/VAL)

| SSOT Owner | Location |
|------------|----------|
| **VbP Study** | `sessionVolumeProfile.session_poc/vah/val` |

**Data flow:**
```
VbP Study API -> PopulateFromVbPStudy() -> sessionVolumeProfile.session_*
    -> CreateZonesFromProfile() -> sessionCtx.rth_* -> Zone anchors
```

**Common violation:** Using calculated values from vapArray instead of VbP study values.

**Fix pattern:**
```cpp
// Pass VbP values to CreateZonesFromProfile (they override calculated)
AMT::CreateZonesFromProfile(zm, vapArray.data(), size, tickSize, time, bar,
    st->sessionVolumeProfile.session_poc,  // VbP SSOT
    st->sessionVolumeProfile.session_vah,  // VbP SSOT
    st->sessionVolumeProfile.session_val); // VbP SSOT
```

---

## SSOT: Peaks/Valleys (HVN/LVN)

| SSOT Owner | Location |
|------------|----------|
| **Sierra Chart VbP Study** | `sc.GetStudyPeakValleyLine()` API |

**Data flow:**
```
VbP Study Settings (Draw Peaks/Valleys) -> GetStudyPeakValleyLine() API
    -> PopulatePeaksValleysFromVbP() -> session_hvn/session_lvn vectors
```

**API usage:**
```cpp
float pvPrice = 0.0f;
int pvType = 0;  // 0=NONE, 1=PEAK(HVN), 2=VALLEY(LVN)
int startIndex = 0, endIndex = 0;

const int result = sc.GetStudyPeakValleyLine(
    sc.ChartNumber, vbpStudyId, pvPrice, pvType,
    startIndex, endIndex, profileIndex, pvIndex);
```

**Legacy computed HVN/LVN removed (Dec 2024):**
- `FindHvnLvn()`, `RefreshWithHysteresis()`, `ApplyHysteresis()` - removed
- `NodeCandidate`, `hvnCandidates`, `lvnCandidates` - removed
- Sigma-based thresholds replaced by SC's native peaks/valleys

**Note:** `cachedThresholds` remains for volume density classification at any price
(separate concept from peaks/valleys which are specific price levels).

---

## SSOT: Zone IDs (pocId, vahId, valId)

| SSOT Owner | Location |
|------------|----------|
| **`ZoneManager.anchors`** | `AMT_Zones.h` SessionAnchors struct |

**Aliased as:**
```cpp
int& pocId = anchors.pocId;  // Reference alias in ZoneManager
int& vahId = anchors.vahId;
int& valId = anchors.valId;
```

**Access pattern:**
```cpp
ZoneRuntime* poc = zm.GetPOC();  // Uses pocId internally
```

**Common violation:** Clearing `activeZones` without resetting anchor IDs.

---

## SSOT: Zone Anchor Storage (Tick-Based)

| SSOT Owner | Location |
|------------|----------|
| **`anchorTicks`** | `ZoneRuntime` (long long, integer ticks) |

**`anchorPrice` is DERIVED:**
```cpp
anchorPrice = anchorTicks * tickSizeCache;  // Never write to this directly
```

**Recenter policy:**
- Small drift (1-7 ticks): Recenter via `RecenterEx()` (preserves stats)
- Large jump (>=8 ticks): Retire + recreate (ES-specific, 8 ticks = 2.00 points)
- During engagement: Action latched, applied after finalize

**Thresholds (AMT_Zones.h):**
```cpp
static constexpr int RECENTER_MIN_TICKS = 1;    // Minimum delta to recenter
static constexpr int LARGE_JUMP_TICKS = 8;      // Threshold for retire+create
```

---

## POC Migration & Zone Recenter Policy

### Hysteresis (AMT_VolumeProfile.h)
```cpp
static constexpr int RECENTER_STABILITY_BARS = 3;  // POC must be stable N bars
```

**CheckStability() returns true when:**
1. POC has moved >= 2 ticks from previous anchor
2. POC has been stable at new price for >= 3 bars

### Recenter Decision Table

| POC Drift | Stability | Zone Engaged | Action |
|-----------|-----------|--------------|--------|
| +/-1 tick | N/A | N/A | No action (noise suppression) |
| 2-7 ticks | < 3 bars | N/A | Wait for stability |
| 2-7 ticks | >= 3 bars | No | Recenter applied |
| 2-7 ticks | >= 3 bars | Yes | Latched, applied after finalize |
| >= 8 ticks | >= 3 bars | No | Retire zone, recreate fresh |
| >= 8 ticks | >= 3 bars | Yes | Latched as REPLACE, retire after finalize |

### Key Points
- **"Recenter, don't reset"**: Small POC drift preserves zone stats (touch counts, engagements)
- **Engagement latch**: Never interrupt ongoing engagement; latch action for later
- **Integer tick math**: All anchor calculations use `long long` ticks to avoid float drift
- **Replace flag**: `pendingReplaceNeeded` signals zone should be retired after finalize
