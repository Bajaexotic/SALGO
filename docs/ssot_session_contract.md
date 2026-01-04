
# SSOT Session Contract & Failure Playbook

## Purpose
This document defines the Single Source of Truth (SSOT) architecture for session management in the AMT Framework. It serves as the authoritative reference for:
- Session identity (RTH vs Globex, trading day)
- Session levels (POC, VAH, VAL)
- Session transitions and boundary detection

---

## 1. Authority Definitions

### Session Identity (SessionKey)
| Attribute | SSOT Owner | Location |
|-----------|------------|----------|
| `currentSession` | `SessionManager` | `AMT_Session.h:SessionManager` |
| `previousSession` | `SessionManager` | `AMT_Session.h:SessionManager` |
| `tradingDay` | Computed | `amt_core.h:ComputeSessionKey()` |
| `sessionType` | Computed | `amt_core.h:ComputeSessionKey()` |

**Key Constraint**: `ComputeSessionKey()` is the ONLY function that creates SessionKey values. It uses chart RTH boundaries (`rthStartSec`, `rthEndSec`) as inputs.

### Session Levels (POC/VAH/VAL)
| Attribute | SSOT Owner | Location |
|-----------|------------|----------|
| `sessionPOC` | `SessionManager` | `AMT_Session.h:SessionManager` |
| `sessionVAH` | `SessionManager` | `AMT_Session.h:SessionManager` |
| `sessionVAL` | `SessionManager` | `AMT_Session.h:SessionManager` |

**Key Constraint**: All mutations via `SessionManager.UpdateLevels()`. Reads via `GetPOC()`, `GetVAH()`, `GetVAL()`.

---

## 2. Invariants

### INV-1: Single Computation Point
```
SessionKey values MUST only be created by ComputeSessionKey().
No manual construction of SessionKey{tradingDay, type} outside this function.
```

### INV-2: Single Mutation Point
```
Session transition detection MUST use SessionManager.UpdateSession().
Returns true IFF session boundary crossed.
```

### INV-3: Derived State Sync
```
After UpdateSession() returns true:
  - sessionMgr.previousSession contains old session
  - sessionMgr.currentSession contains new session
  - sessionMgr.DidSessionChange() returns true for remainder of bar
```

### INV-4: Level Mutation Path
```
Session levels MUST only be modified via SessionManager.UpdateLevels().
No direct writes to sessionPOC/sessionVAH/sessionVAL.
```

### INV-5: Schedule Stability
```
rthStartSec and rthEndSec MUST be constant within a chart session.
These are read from SC inputs at initialization and never modified.
```

---

## 3. Prohibited Patterns

### PROHIBITED-1: Direct SessionKey Construction
```cpp
// WRONG - bypasses SSOT
SessionKey key;
key.tradingDay = 20241223;
key.sessionType = SessionType::RTH;

// CORRECT - use SSOT function
SessionKey key = ComputeSessionKey(dateYMD, timeSec, rthStartSec, rthEndSec);
```

### PROHIBITED-2: Legacy Transition Detection
```cpp
// WRONG - deprecated pattern
if (phaseCoordinator.DidSessionTypeChange()) { ... }

// CORRECT - SSOT pattern
if (sessionMgr.DidSessionChange()) { ... }
```

### PROHIBITED-3: Local Session Level Storage
```cpp
// WRONG - creates dual truth
double localPOC = vbpProfile.session_poc;
// ... later used instead of sessionMgr.GetPOC()

// CORRECT - always read from SSOT
double poc = sessionMgr.GetPOC();
```

### PROHIBITED-4: Hardcoded Session Boundaries
```cpp
// WRONG - hardcoded times
const int rthStart = 9 * 3600 + 30 * 60;  // 09:30

// CORRECT - use SC input values
const int rthStart = rthStartSec;  // From sc.Input[8].GetTime()
```

---

## 4. Data Flow Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                     Bar Processing Loop                         │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│  Extract bar timestamp: dateYMD, timeSec                        │
│  (from sc.BaseDateTimeIn[idx])                                  │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│  ComputeSessionKey(dateYMD, timeSec, rthStartSec, rthEndSec)    │
│  → Returns SessionKey{tradingDay, sessionType}                  │
│  Location: amt_core.h:138-168                                   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│  sessionMgr.UpdateSession(newSessionKey)                        │
│  → Compares with currentSession                                 │
│  → If different: previousSession = currentSession               │
│                  currentSession = newSessionKey                 │
│                  returns TRUE                                   │
│  Location: AMT_Session.h:SessionManager::UpdateSession()        │
└─────────────────────────────────────────────────────────────────┘
                              │
                    ┌─────────┴─────────┐
                    │                   │
              returns TRUE        returns FALSE
                    │                   │
                    ▼                   ▼
┌──────────────────────────┐  ┌──────────────────────────┐
│  SESSION TRANSITION      │  │  SAME SESSION            │
│  - Reset accumulators    │  │  - Continue normal       │
│  - Clear zones           │  │    processing            │
│  - Log [SESSION-KEY]     │  │                          │
└──────────────────────────┘  └──────────────────────────┘
```

---

## 5. Failure Playbook

### F1: Zones not resetting on session change

| Symptom | Yesterday's zones persist into today's RTH |
|---------|-------------------------------------------|
| Root Cause | Session change not detected |
| Check | `[SESSION-KEY]` log line appears at expected time? |
| File/Line | `AuctionSensor_v1.cpp:1515-1527` |
| Fix | Verify `ComputeSessionKey()` receives correct `rthStartSec`/`rthEndSec` |

### F2: Session detected at wrong time

| Symptom | RTH starts at 09:00 instead of 09:30 |
|---------|-------------------------------------|
| Root Cause | Input misconfiguration |
| Check | `[SESSION-SCHEDULE]` log shows correct boundaries? |
| File/Line | `AuctionSensor_v1.cpp` (init block, near line 1404) |
| Fix | Verify SC Input[8] and Input[9] values |

### F3: Morning Globex tagged as new session

| Symptom | Session changes at midnight instead of RTH open |
|---------|------------------------------------------------|
| Root Cause | `DecrementDate()` not applied for pre-RTH bars |
| Check | Morning Globex SessionKey has previous day's tradingDay? |
| File/Line | `amt_core.h:150-156` (DecrementDate logic) |
| Fix | Verify `timeSec < rthStartSec` triggers decrement |

### F4: Trading day roll not detected

| Symptom | Globex → RTH transition shows no `[SESSION-KEY]` log |
|---------|-----------------------------------------------------|
| Root Cause | SessionKey comparison failing |
| Check | Both tradingDay AND sessionType differ between keys? |
| File/Line | `AMT_Session.h:UpdateSession()` |
| Fix | Verify `SessionKey::operator!=` checks both fields |

### F5: Dual truth for session levels

| Symptom | POC/VAH/VAL values inconsistent between components |
|---------|---------------------------------------------------|
| Root Cause | Local storage bypassing SessionManager |
| Check | `grep -n "sessionPOC\|sessionVAH\|sessionVAL"` shows only AMT_Session.h? |
| File/Line | Grep output will identify violating file |
| Fix | Replace local storage with `sessionMgr.GetPOC()` etc. |

### F6: Session phase out of sync

| Symptom | `amtContext.session` differs from `phaseCoordinator.GetPhase()` |
|---------|----------------------------------------------------------------|
| Root Cause | Sync line missing after phase update |
| Check | Line `st->amtContext.session = newPhase;` present after phase update? |
| File/Line | `AuctionSensor_v1.cpp:1487` |
| Fix | Ensure sync line follows every `phaseCoordinator.UpdatePhase()` call |

### F7: VbP session boundary vs SessionKey mismatch

| Symptom | VbP says RTH started but SessionKey says Globex |
|---------|------------------------------------------------|
| Root Cause | Different time sources or rounding |
| Check | VbP profile start time matches RTH boundary? |
| File/Line | `AuctionSensor_v1.cpp:1459-1478` |
| Fix | Ensure both use same `rthStartSec` constant |

### F8: Test failures after boundary modification

| Symptom | `test_session_key` fails after code changes |
|---------|---------------------------------------------|
| Root Cause | Test constants don't match production |
| Check | Test uses same `RTH_START_SEC`/`RTH_END_SEC` as production? |
| File/Line | `test/test_session_key.cpp:37-38` |
| Fix | Sync test constants with `AMT_config.h` defaults |

---

## 6. Evidence Gates

Run these commands to verify SSOT compliance:

### Gate 1: Session Level Storage
```bash
grep -rn "sessionPOC\|sessionVAH\|sessionVAL" --include="*.cpp" --include="*.h" | grep -v AMT_Session.h | grep -v "//"
```
**Expected**: Empty output (all storage in AMT_Session.h only)

### Gate 2: SessionKey Construction
```bash
grep -rn "SessionKey{" --include="*.cpp" --include="*.h" | grep -v ComputeSessionKey | grep -v "= .*SessionKey{}"
```
**Expected**: Empty output (no manual construction outside ComputeSessionKey)

### Gate 3: Legacy Transition Detection
```bash
grep -rn "DidSessionTypeChange" --include="*.cpp" --include="*.h" | grep -v "//"
```
**Expected**: Empty or only comments (deprecated pattern removed)

### Gate 4: Unit Tests
```bash
./test_session_key.exe
```
**Expected**: `8 passed, 0 failed`

---

## 7. Guardrails

### G1: Compile-Guard Comment
Location: `amt_core.h` above `ComputeSessionKey()`
```cpp
// ============================================================================
// SSOT: This is the ONLY function that creates SessionKey values.
// All session identity must flow through this function.
// DO NOT construct SessionKey manually elsewhere.
// Contract: docs/ssot_session_contract.md
// ============================================================================
```

### G2: Debug Assertion
Location: `AMT_Session.h` in `SessionManager::UpdateSession()`
```cpp
// Debug assertion: SessionKey must have valid trading day
assert(newKey.tradingDay >= 20200101 && "SessionKey must come from ComputeSessionKey()");
```

### G3: Init Schedule Logging
Location: `AuctionSensor_v1.cpp` initialization block
```cpp
// Log session schedule at init (verifies correct boundary configuration)
if (idx == 0) {
    SCString msg;
    msg.Format("[SESSION-SCHEDULE] RTH=%02d:%02d-%02d:%02d ET | Compute via ComputeSessionKey()",
        rthStartSec / 3600, (rthStartSec % 3600) / 60,
        rthEndSec / 3600, (rthEndSec % 3600) / 60);
    sc.AddMessageToLog(msg, 0);
}
```

---

## 8. Maintenance Checklist

When modifying session-related code:

- [ ] Does change touch `ComputeSessionKey()`? Update tests.
- [ ] Does change add session storage? Route through `SessionManager`.
- [ ] Does change read session identity? Use `sessionMgr.currentSession`.
- [ ] Does change detect transitions? Use `sessionMgr.DidSessionChange()`.
- [ ] Run `test_session_key.exe` after changes.
- [ ] Verify evidence gates pass.

---

## 9. Version History

| Date | Change | Author |
|------|--------|--------|
| 2024-12-24 | Initial SSOT contract created | Claude Code |
| | SessionKey computation wired to main loop | |
| | Legacy DidSessionTypeChange() deprecated | |
| | 8 unit tests added for session transitions | |

---

## 10. Evidence Gates — Canonical Commands

Run these commands from the AMT project root to verify SSOT compliance. Each must meet its acceptance condition for the contract to hold.

### Gate 1: Session Levels Isolation
```bash
grep -rn "sessionPOC\|sessionVAH\|sessionVAL" --include="*.cpp" --include="*.h" | grep -v AMT_Session.h | grep -v "//"
```
**Acceptance**: 0 matches. All session level storage must be in `AMT_Session.h:SessionManager`.

### Gate 2: No Manual SessionKey Construction
```bash
grep -rn "SessionKey{" --include="*.cpp" --include="*.h" | grep -v ComputeSessionKey | grep -v "= .*SessionKey{}"
```
**Acceptance**: 0 matches outside empty initialization. All SessionKey values must originate from `ComputeSessionKey()`.

### Gate 3: Legacy Transition Detection Removed
```bash
grep -rn "DidSessionTypeChange" --include="*.cpp" --include="*.h" | grep -v "//"
```
**Acceptance**: 0 matches in active code. Method may exist but must not be called (deprecated).

### Gate 4: SessionKey Ownership Verification
```bash
grep -n "ComputeSessionKey\|sessionMgr\.UpdateSession" AuctionSensor_v1.cpp
```
**Acceptance**: Both patterns present. `ComputeSessionKey` feeds `sessionMgr.UpdateSession` in the bar loop.

### Gate 5: Unit Tests — Session Transitions
```bash
cd test && ./test_session_key.exe
```
**Acceptance**: `8 passed, 0 failed`

### Gate 6: Unit Tests — Phase Detection
```bash
cd test && ./test_phase_detection.exe
```
**Acceptance**: `7 passed, 0 failed`

---

## Quick Verification Script

```bash
#!/bin/bash
# ssot_verify.sh - Run from AMT project root

echo "=== SSOT Evidence Gates ==="

echo -n "Gate 1 (Session Levels): "
COUNT=$(grep -rn "sessionPOC\|sessionVAH\|sessionVAL" --include="*.cpp" --include="*.h" | grep -v AMT_Session.h | grep -v "//" | wc -l)
[ "$COUNT" -eq 0 ] && echo "PASS" || echo "FAIL ($COUNT violations)"

echo -n "Gate 2 (SessionKey Construction): "
COUNT=$(grep -rn "SessionKey{" --include="*.cpp" --include="*.h" | grep -v ComputeSessionKey | grep -v "= {}" | wc -l)
[ "$COUNT" -eq 0 ] && echo "PASS" || echo "FAIL ($COUNT violations)"

echo -n "Gate 3 (Legacy Transition): "
COUNT=$(grep -rn "DidSessionTypeChange" --include="*.cpp" --include="*.h" | grep -v "//" | wc -l)
[ "$COUNT" -eq 0 ] && echo "PASS" || echo "FAIL ($COUNT violations)"

echo -n "Gate 4 (SSOT Wiring): "
grep -q "ComputeSessionKey" AuctionSensor_v1.cpp && grep -q "sessionMgr\.UpdateSession" AuctionSensor_v1.cpp && echo "PASS" || echo "FAIL"

echo -n "Gate 5 (SessionKey Tests): "
cd test && ./test_session_key.exe 2>/dev/null | grep -q "8 passed, 0 failed" && echo "PASS" || echo "FAIL"

echo -n "Gate 6 (Phase Tests): "
./test_phase_detection.exe 2>/dev/null | grep -q "7 passed, 0 failed" && echo "PASS" || echo "FAIL"
```
