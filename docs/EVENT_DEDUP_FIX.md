# Event Deduplication Fix Plan

## Problem
Events are being logged 3-4 times in the `_events.csv` file:
```
1    GLOBEX    12/26/2025 16:15    9432    PHASE_SNAPSHOT  (x4)
1    GLOBEX    12/26/2025 16:15    9432    MODE_LOCK       (x4)
1    GLOBEX    12/26/2025 16:19    9436    ENGAGEMENT_FINAL (x4)
```

## Root Cause Analysis

The deduplication logic in `AMT_Logger.h` has a flaw:

**Current Flow (BROKEN):**
1. Full recalc starts → `ClearLogsForFullRecalc()` clears files AND `loggedEventHashes_`
2. Study processes bars, events queued
3. `SetLiveMode(true)` → `FlushEventQueue()` writes to file
4. Partial recalc/chart update → `SetLiveMode(false)` clears `loggedEventHashes_` BUT NOT files
5. Study reprocesses bars, events pass dedup check (hashes cleared!)
6. `SetLiveMode(true)` → appends to file = **DUPLICATES**

**The Bug (line 402-404):**
```cpp
void SetLiveMode(bool live) {
    if (liveMode_ && !live) {
        loggedEventHashes_.clear();  // <-- BUG: Clears hashes without clearing file
        eventQueue_.clear();
    }
    ...
}
```

## Fix

**Principle:** Only clear `loggedEventHashes_` when the file is also cleared.

### Change 1: Remove hash clearing from SetLiveMode()

**Before:**
```cpp
void SetLiveMode(bool live) {
    if (liveMode_ && !live) {
        loggedEventHashes_.clear();
        eventQueue_.clear();
    }
    if (!liveMode_ && live) {
        FlushEventQueue();
    }
    liveMode_ = live;
}
```

**After:**
```cpp
void SetLiveMode(bool live) {
    // When entering recalc, clear the queue (events will be re-generated)
    // but DON'T clear hashes - they prevent duplicates in the file
    if (liveMode_ && !live) {
        eventQueue_.clear();
        // loggedEventHashes_ deliberately NOT cleared here
        // Only ClearLogsForFullRecalc() should clear both file AND hashes
    }
    // When exiting recalc (not live -> live), flush queued events
    if (!liveMode_ && live) {
        FlushEventQueue();
    }
    liveMode_ = live;
}
```

### Change 2: Ensure ClearLogsForFullRecalc clears hashes (already does)

The existing code is correct:
```cpp
void ClearLogsForFullRecalc() {
    // ... truncate files ...

    // This is correct - when files are cleared, hashes must also be cleared
    // (already in place, just noting for completeness)
}
```

But we should add explicit hash clearing there for clarity:
```cpp
void ClearLogsForFullRecalc() {
    // ... existing file truncation code ...

    // Clear deduplication hashes - file is now empty, so all events are "new"
    loggedEventHashes_.clear();

    // ... existing buffer clearing code ...
}
```

## Verification

After fix:
1. Full recalc → files cleared, hashes cleared → events logged fresh
2. Partial recalc → files NOT cleared, hashes NOT cleared → events deduplicated
3. Live mode transitions → queue managed, hashes preserved → no duplicates

## Files to Modify

- `AMT_Logger.h`: Lines 400-411 (SetLiveMode function)
