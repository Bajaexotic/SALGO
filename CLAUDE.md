# AMT Framework - Auction Market Theory

> **START HERE**: Read `docs/STATUS.md` first. It's the 25-line control file for current work. Only read other files if STATUS references them.

## Working Directory

**All source files are located in:** `E:\SierraChart\ACS_Source\`

- Main study: `AuctionSensor_v1.cpp`
- Headers: `AMT_*.h` and `amt_core.h`
- Documentation: `docs/`

---

## Quick Navigation

Detailed documentation is modularized in `.claude/rules/`:

| File | Contents |
|------|----------|
| `architecture.md` | File structure (35 files), header deps, key structs |
| `zone-system.md` | Zone posture, anchors, recenter policy, structure tracking |
| `session-system.md` | Session phase SSOT, extremes, management contract |
| `baselines.md` | Decision gate, effort signals, confidence scores |
| `liquidity-model.md` | Kyle's 4-component model (Depth, Stress, Resilience, Spread) |
| `volatility-engine.md` | 4 regimes, tradability rules, hysteresis |
| `imbalance-engine.md` | 10 types, conviction, context gates |
| `volume-engine.md` | Acceptance/rejection, value migration, intensity |
| `market-state.md` | Balance/Imbalance (Dalton 1TF/2TF), CurrentPhase |
| `external-inputs.md` | VbP, Numbers Bars, DOM dependencies |
| `patterns.md` | DRY violations, common patterns, Windows macros |
| `contracts.md` | December 2024 contract changes |

All rules in `.claude/rules/` are automatically loaded by Claude Code.

---

## Core Concepts (Quick Reference)

**Market State (SSOT: DaltonEngine):**
- 1TF = IMBALANCE (trending, ~20%)
- 2TF = BALANCE (rotation, ~80%)

**Liquidity State (SSOT: LiquidityEngine):**
- VOID / THIN / NORMAL / THICK

**Volatility Regime (SSOT: VolatilityEngine):**
- COMPRESSION / NORMAL / EXPANSION / EVENT

**Volume Acceptance (SSOT: VolumeAcceptanceEngine):**
- ACCEPTED / REJECTED / TESTING

**Session Phases (7 tradeable):**
- GLOBEX, LONDON_OPEN, PRE_MARKET, INITIAL_BALANCE, MID_SESSION, CLOSING_SESSION, POST_CLOSE

---

## Critical Patterns

```cpp
// Session phase sync (DRY helper)
st->SyncSessionPhase(newPhase);

// Zone clearing (atomic)
zm.ClearZonesOnly(bar, time, reason);

// Windows macro protection
int val = (std::min)(a, b);

// Always check validity before using engine results
if (result.IsReady()) { ... }
```

---

## Test Compilation

**ALWAYS use `compile.bat`** - never use raw g++ commands or ad-hoc scripts.

```batch
# From any directory:
E:/SierraChart/ACS_Source/test/compile.bat <test_name> run     # compile and run
E:/SierraChart/ACS_Source/test/compile.bat <test_name> syntax  # syntax check only
E:/SierraChart/ACS_Source/test/compile.bat all                 # run all tests
```

The script handles TEMP/TMP environment issues automatically. Do NOT:
- Create new .bat files for compilation
- Use `C:/msys64/usr/bin/bash.exe` commands
- Use `powershell` for compilation
- Set environment variables manually
