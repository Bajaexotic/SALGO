# AMT Logging System Fix Plan

> **Root Cause**: Split-brain logging with 74+ direct `sc.AddMessageToLog()` calls bypassing the `LogManager` system that has throttling, deduplication, and level filtering.

## Issues Identified

### 1. Performance Issues
- **74 direct `sc.AddMessageToLog()` calls** in `AuctionSensor_v1.cpp` bypass LogManager's throttling
- **27 direct calls** in `AMT_VolumeProfile.h` with no efficiency gating
- No `sc.Index == sc.ArraySize - 1` gate on most calls (ACSIL best practice for efficiency)
- During full recalc, every bar triggers logs = massive spam

### 2. Missing/Duplicate Logs
- LogManager has deduplication (`loggedEventHashes_`) but direct calls bypass it
- `liveMode_` flag not checked consistently by all logging paths
- Session event logging duplicates when `lastLogBar_` throttling is bypassed

### 3. Format Problems
- Inconsistent prefixes: `[VBP]`, `[SESSION]`, `[AMT]` - some use LogCategory enum, others hardcode strings
- Mixed message formats (some quoted, some not)
- Potential `SCString` lifetime issues with `.GetChars()` patterns per ACSIL doc

### 4. File I/O Issues
- `ClearLogsForFullRecalc()` is commented out at line 2688 - causes old data to accumulate
- File open/write/close on every single log entry is inefficient
- No batching or buffering for high-frequency logging

---

## Fix Plan

### Phase 1: Centralize All Logging Through LogManager (HIGH PRIORITY)

**Goal**: Eliminate direct `sc.AddMessageToLog()` calls in favor of LogManager methods

#### 1.1 Add Convenience Methods to LogManager

Add to `AMT_Logger.h`:
```cpp
// Simple SC message log with automatic throttling and gating
void LogDebug(int bar, const char* msg, LogCategory cat = LogCategory::DEBUG) {
    if (!ShouldEmit(LogChannel::SC_MESSAGE, LogLevel::VERBOSE, bar)) return;
    LogToSC(cat, msg, false);
}

void LogInfo(int bar, const char* msg, LogCategory cat = LogCategory::SYSTEM) {
    if (!ShouldEmit(LogChannel::SC_MESSAGE, LogLevel::MODERATE, bar)) return;
    LogToSC(cat, msg, false);
}

void LogWarn(int bar, const char* msg, LogCategory cat = LogCategory::SYSTEM) {
    if (!ShouldEmit(LogChannel::SC_MESSAGE, LogLevel::MINIMAL, bar)) return;
    LogToSC(cat, msg, true);
}

void LogError(int bar, const char* msg, LogCategory cat = LogCategory::SYSTEM) {
    // Errors always log (no level check), but still respect window
    if (bar >= 0 && !InLogWindow(bar)) return;
    LogToSC(cat, msg, true);
}

// Rate-limited variant (e.g., max 5 per session)
void LogOnce(ThrottleKey key, int bar, const char* msg,
             LogCategory cat = LogCategory::SYSTEM, int maxPerSession = 1) {
    if (!ShouldLogRateLimited(key, maxPerSession)) return;
    if (!ShouldEmit(LogChannel::SC_MESSAGE, LogLevel::MODERATE, bar)) return;
    LogToSC(cat, msg, false);
}
```

#### 1.2 Replace Direct Calls Pattern

**Before:**
```cpp
SCString msg;
msg.Format("[VBP] POC moved to %.2f", newPOC);
sc.AddMessageToLog(msg, 0);
```

**After:**
```cpp
SCString msg;
msg.Format("POC moved to %.2f", newPOC);
st->logManager.LogInfo(curBarIdx, msg.GetChars(), LogCategory::VBP);
```

#### 1.3 Create Helper Macros for Common Patterns

Add to `AMT_Logger.h` or new `AMT_LogMacros.h`:
```cpp
// Efficiency gate macro (per ACSIL doc line 973-974)
#define AMT_LOG_GATE(bar, arrSize) ((bar) >= (arrSize) - 100)

// Checked log macro
#define AMT_LOG_INFO(logMgr, bar, cat, fmt, ...) do { \
    if (AMT_LOG_GATE(bar, logMgr.GetArraySize())) { \
        SCString _msg; _msg.Format(fmt, ##__VA_ARGS__); \
        logMgr.LogInfo(bar, _msg.GetChars(), cat); \
    } \
} while(0)
```

---

### Phase 2: Fix LogManager Internals (MEDIUM PRIORITY)

#### 2.1 Re-enable ClearLogsForFullRecalc

At line 2688 in `AuctionSensor_v1.cpp`, uncomment:
```cpp
if (isFullRecalc && curBarIdx == 0) {
    st->logManager.ClearLogsForFullRecalc();  // Was commented out
}
```

#### 2.2 Add Batched File Writing

Replace open-write-close pattern with batching:
```cpp
class LogManager {
    // Add buffer for batch writes
    std::vector<SCString> csvBuffer_;
    static constexpr int BUFFER_FLUSH_SIZE = 50;  // Flush every 50 entries

    void AppendToBuffer(const SCString& line) {
        csvBuffer_.push_back(line);
        if (csvBuffer_.size() >= BUFFER_FLUSH_SIZE) {
            FlushBuffer();
        }
    }

    void FlushBuffer() {
        if (csvBuffer_.empty() || !sc_) return;

        int fileHandle = 0;
        unsigned int bytesWritten = 0;
        sc_->OpenFile(eventsPath_, n_ACSIL::FILE_MODE_OPEN_TO_APPEND, fileHandle);
        if (fileHandle == 0) return;

        for (const auto& line : csvBuffer_) {
            sc_->WriteFile(fileHandle, line.GetChars(), line.GetLength(), &bytesWritten);
        }

        sc_->CloseFile(fileHandle);
        csvBuffer_.clear();
    }
};
```

#### 2.3 Add Log Window Size Accessor

```cpp
// Add to LogManager public section
int GetArraySize() const { return arraySize_; }
bool InLogWindowPublic(int bar) const { return InLogWindow(bar); }
```

---

### Phase 3: Apply Efficiency Gating (HIGH PRIORITY)

Per ACSIL doc line 973-974: *"This is for efficiency so the logging occurs only on the most recent chart bar"*

#### 3.1 Gate All Diagnostic Logging

**Pattern to apply everywhere:**
```cpp
// Only log on recent bars (last N bars)
const bool inLogWindow = curBarIdx >= sc.ArraySize - logWindowBars;
if (inLogWindow) {
    // ... logging code ...
}
```

This is already partially done but needs consistent application.

#### 3.2 Special Case: Full Recalc Suppression

```cpp
// Skip verbose logging during full recalc (per ACSIL doc line 775-783)
if (sc.IsFullRecalculation && curBarIdx < sc.ArraySize - 1) {
    // Skip non-critical logging
    return;
}
```

---

### Phase 4: Fix Format Consistency (LOW PRIORITY)

#### 4.1 Standardize Category Prefixes

All logs should use `LogCategory` enum for consistent `[CATEGORY]` prefixes:
- `LogCategory::VBP` -> `[VBP]`
- `LogCategory::SESSION` -> `[SESSION]`
- `LogCategory::ZONE` -> `[ZONE]`
- etc.

#### 4.2 Fix SCString Lifetime Issues

Per ACSIL doc "Directly Accessing a SCString" section:

**Before (potential dangling):**
```cpp
const char* str = SomeFunc().c_str();  // Temporary destroyed!
```

**After:**
```cpp
std::string storage = SomeFunc();      // Store first
const char* str = storage.c_str();     // Then access
```

Or with SCString:
```cpp
SCString storage = SomeFunc();
const char* str = storage.GetChars();
```

---

### Phase 5: Add Diagnostic Logging Controls (MEDIUM PRIORITY)

#### 5.1 Add Module-Level Enables

```cpp
// Add to LogManager or via study inputs
bool enableVBPDiag = false;      // Input 120
bool enableSessionDiag = false;  // Input 121
bool enableZoneDiag = false;     // Input 122
bool enableDeltaDiag = false;    // Input 123
```

#### 5.2 Add Runtime Verbosity Toggle

Allow changing `LogLevel` via study input without recompile.

---

## Implementation Priority

| Phase | Priority | Effort | Impact |
|-------|----------|--------|--------|
| Phase 1 | HIGH | Large | Fixes all 4 issue categories |
| Phase 2 | MEDIUM | Medium | File I/O + dedup |
| Phase 3 | HIGH | Small | Performance |
| Phase 4 | LOW | Medium | Code quality |
| Phase 5 | MEDIUM | Small | Usability |

---

## Files to Modify

1. **AMT_Logger.h** - Add convenience methods, batching, public accessors
2. **AuctionSensor_v1.cpp** - Replace 74 direct calls, uncomment ClearLogsForFullRecalc
3. **AMT_VolumeProfile.h** - Replace 27 direct calls, add efficiency gating
4. **AMT_Zones.h** - Replace 4 direct calls
5. **AMT_Session.h** - Replace 4 direct calls
6. **AMT_Snapshots.h** - Replace 3 direct calls

---

## Estimated Changes

- **~100 direct `sc.AddMessageToLog()` calls** to convert to LogManager methods
- **~20 new lines** in LogManager for convenience methods
- **~50 lines** for batched file writing
- **Uncomment 1 line** for ClearLogsForFullRecalc

---

## Testing Checklist

- [ ] Full recalc produces minimal log spam
- [ ] Live bar logging works correctly
- [ ] CSV files are populated without duplicates
- [ ] Session stats block appears once per session
- [ ] Probe events logged to CSV correctly
- [ ] No performance degradation vs current system
