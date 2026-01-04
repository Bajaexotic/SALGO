// ============================================================================
// AMT_Logger.h
// Unified logging system with persistent file handles
// Extracted from AuctionSensor_v1.cpp for modularization
// ============================================================================

#ifndef AMT_LOGGER_H
#define AMT_LOGGER_H

#include "sierrachart.h"
#include "AMT_Probes.h"  // For ProbeRequest, ProbeResult, ProbeDirection, ProbeStatus
#include "amt_core.h"    // For StateEvidence, AMTMarketState, etc.
#include <cstring>
#include <set>
#include <vector>
#include <algorithm>

namespace AMT {

// ============================================================================
// LOG ENUMS
// ============================================================================

enum class LogChannel : int {
    SC_MESSAGE = 0,   // Sierra Chart message log window
    PROBE_CSV  = 1,   // Probe lifecycle CSV (fired/resolved)
    EVENTS_CSV = 2,   // Generic events/diagnostic CSV
    AMT_CSV    = 3    // AMT zone tracking CSV
};

enum class LogLevel : int {
    NONE     = 0,   // No logging
    MINIMAL  = 1,   // Critical events only (errors, warnings, lifecycle)
    MODERATE = 2,   // Important state changes
    VERBOSE  = 3    // Full diagnostic detail
};

// Fixed enum for throttle keys (avoids std::string allocations in hot paths)
enum class ThrottleKey : int {
    PROBE_FIRED = 0,
    PROBE_RESOLVED,
    SESSION_CHANGE,
    VBP_DRIFT,
    VBP_MATCH,
    VBP_SESSION_SUMMARY,
    VBP_WARNING,
    SSOT_DIAG,
    STATS_BLOCK,
    WARMUP_PROGRESS,
    EXTREME_CHECK,
    ZONE_FINALIZE,
    WIDTH_MISMATCH,
    MODE_LOCK,
    BLOCK_CHANGE,
    REPLAY_SUMMARY,
    DRIFT_WARNING,
    AMT_CSV_START,
    BACKFILL_COMPLETE,
    SESSION_ARCHIVE,
    PHASE_SNAPSHOT,
    INTENT_SIGNAL,
    ENG_FINALIZE,
    DELTA_DIAG,
    FACIL_DIAG,
    // Phase 1.1: New throttle keys for centralized logging
    INPUT_DIAG,
    DELTA_VERIFY,
    CUM_DELTA_DIAG,
    INIT_PATH,
    STATE_RESET,
    ZONE_POSTURE,
    AMT_STARTUP,
    SESSION_DIAG,
    PERF_DIAG,
    VBP_ERROR,
    PRIOR_VBP,
    BASELINE_PHASE,
    ACTIVE_SESSION,
    ZONE_UPDATE,
    ACCUM_DIAG,
    AUDIT_DIAG,
    GENERAL_INFO,
    GENERAL_WARN,
    GENERAL_ERROR,
    // Stage 2.1: Baseline not ready diagnostics (rate-limited per session per metric)
    BASELINE_NOT_READY_DELTA,
    BASELINE_NOT_READY_LIQUIDITY,
    BASELINE_NOT_READY_FACIL,
    BASELINE_NOT_READY_PROBE,
    // Decision/clarity logging
    AMT_DECISION,
    // AMT state machine logging
    AMT_STATE,
    AMT_STATE_TRANSITION,
    _COUNT
};

enum class LogCategory : int {
    PROBE   = 0,
    AMT     = 1,
    ZONE    = 2,
    SESSION = 3,
    DRIFT   = 4,
    VBP     = 5,
    SYSTEM  = 6,
    DEBUG   = 7,
    REPLAY  = 8,
    WARMUP  = 9,
    EXTREME = 10,
    SSOT    = 11,
    VAL     = 12,
    // Phase 1.1: Additional categories for centralized logging
    INPUT   = 13,
    DELTA   = 14,
    INIT    = 15,
    PERF    = 16,
    BASELINE = 17,
    ACCUM   = 18,
    AUDIT   = 19,
    ERROR_CAT = 20,  // Named to avoid Windows ERROR macro conflict
    PATTERN = 21,    // Phase 4: Pattern evidence logging
    DAYTYPE = 22     // Phase 2: DayType structural classification
};

// ============================================================================
// SESSION EVENT TYPES (for structured CSV logging)
// ============================================================================

enum class SessionEventType : int {
    SESSION_START = 0,      // Session boundary start
    SESSION_END,            // Session boundary end with summary
    ENGAGEMENT_FINAL,       // Zone engagement completed
    MODE_LOCK,              // Market state locked/confirmed
    DELTA_DIAG,             // Per-bar delta diagnostics
    FACIL_DIAG,             // Per-bar facilitation diagnostics
    PHASE_SNAPSHOT,         // Phase state snapshot
    INTENT_SIGNAL,          // Aggression/coherence signal
    VBP_UPDATE,             // VBP profile update
    PROBE_FIRED,            // Probe activated
    PROBE_RESOLVED,         // Probe completed
    EVENT_WARN,             // Warning event (non-critical issues)
    EVENT_ERROR,            // Error event (avoid Windows ERROR macro)
    AMT_STATE_SNAPSHOT,     // AMT state + evidence ledger (on transition or periodic)
    AMT_STATE_TRANSITION    // AMT state transition (full snapshot)
};

inline const char* SessionEventTypeName(SessionEventType t) {
    switch (t) {
        case SessionEventType::SESSION_START:     return "SESSION_START";
        case SessionEventType::SESSION_END:       return "SESSION_END";
        case SessionEventType::ENGAGEMENT_FINAL:  return "ENGAGEMENT_FINAL";
        case SessionEventType::MODE_LOCK:         return "MODE_LOCK";
        case SessionEventType::DELTA_DIAG:        return "DELTA_DIAG";
        case SessionEventType::FACIL_DIAG:        return "FACIL_DIAG";
        case SessionEventType::PHASE_SNAPSHOT:    return "PHASE_SNAPSHOT";
        case SessionEventType::INTENT_SIGNAL:     return "INTENT_SIGNAL";
        case SessionEventType::VBP_UPDATE:        return "VBP_UPDATE";
        case SessionEventType::PROBE_FIRED:       return "PROBE_FIRED";
        case SessionEventType::PROBE_RESOLVED:    return "PROBE_RESOLVED";
        case SessionEventType::EVENT_WARN:        return "WARN";
        case SessionEventType::EVENT_ERROR:       return "ERROR";
        case SessionEventType::AMT_STATE_SNAPSHOT:    return "AMT_STATE";
        case SessionEventType::AMT_STATE_TRANSITION:  return "AMT_TRANSITION";
        default:                                  return "UNKNOWN";
    }
}

// ============================================================================
// SESSION EVENT STRUCT (structured data for CSV)
// ============================================================================

struct SessionEvent {
    // Core identification
    SessionEventType type = SessionEventType::SESSION_START;
    SCDateTime timestamp;
    int bar = 0;

    // Session context
    SCString sessionType;      // "RTH", "GLOBEX"

    // Zone engagement fields (ENGAGEMENT_FINAL)
    int zoneId = 0;
    SCString zoneType;         // "VPB_POC", "PRIOR_VAH", etc.
    double entryPrice = 0.0;
    double exitPrice = 0.0;
    int engagementBars = 0;
    int peakDist = 0;
    int entryDist = 0;
    int exitDist = 0;
    double escapeVel = 0.0;
    double volRatio = 0.0;
    SCString outcome;          // "ACCEPT", "REJECT", "TAG", "TEST", "PROBE"

    // Intent/state fields (MODE_LOCK, INTENT_SIGNAL)
    double deltaConf = 0.0;
    double sessDeltaPct = 0.0;
    int sessDeltaPctl = 0;
    int coherent = 0;
    SCString aggression;       // "RESPONSIVE", "INITIATIVE"
    SCString facilitation;     // "EFFICIENT", "INEFFICIENT"
    SCString marketState;      // "BALANCE", "IMBALANCE", "UNDEFINED"
    SCString phase;            // "ROTATION", "DRIVING_UP", "DRIVING_DOWN", etc.

    // Diagnostic fields (DELTA_DIAG, FACIL_DIAG)
    double volume = 0.0;
    double range = 0.0;
    double volPctl = 0.0;
    double rangePctl = 0.0;

    // VBP fields (SESSION_START, VBP_UPDATE)
    double poc = 0.0;
    double vah = 0.0;
    double val = 0.0;

    // Session summary fields (SESSION_END)
    int totalEngagements = 0;
    int acceptCount = 0;
    int rejectCount = 0;
    int tagCount = 0;
    int probeCount = 0;
    int testCount = 0;

    // Free-form message for additional context
    SCString message;
};

// ============================================================================
// AMT BAR DATA (For CSV logging)
// ============================================================================

struct AmtBarData {
    SCDateTime timestamp;
    int barIndex = 0;
    double price = 0.0;
    double high = 0.0;
    double low = 0.0;
    double volume = 0.0;
    double delta = 0.0;

    // Phase (as string for CSV) - uses SCString for safe memory ownership
    SCString phase;

    // Zone prices
    double pocPrice = 0.0;
    double vahPrice = 0.0;
    double valPrice = 0.0;

    // Proximity (as int)
    int vahProximity = 0;
    int pocProximity = 0;
    int valProximity = 0;

    // Touches
    int vahTouches = 0;
    int pocTouches = 0;
    int valTouches = 0;

    // Strength scores
    double vahStrength = 0.0;
    double pocStrength = 0.0;
    double valStrength = 0.0;

    // Zone existence flags (1 = exists, 0 = no zone)
    int hasVAH = 0;
    int hasPOC = 0;
    int hasVAL = 0;

    // Summary
    int activeZoneCount = 0;
    int totalTouches = 0;
};

// ============================================================================
// LOG MANAGER
// ============================================================================

class LogManager {
public:
    // =========================================================================
    // LIFECYCLE
    // =========================================================================

    void Initialize(s_sc& sc, const char* studyName, const char* baseLogDir)
    {
        if (initialized_)
            return;

        sc_ = &sc;
        chartNumber_ = sc.ChartNumber;
        studyId_ = sc.StudyGraphInstanceID;
        arraySize_ = sc.ArraySize;

        symbol_ = sc.Symbol;
        if (sc.SecondsPerBar > 0)
            timeframe_.Format("%dm", sc.SecondsPerBar / 60);
        else
            timeframe_ = "1m";

        baseDir_ = baseLogDir;

        // Build file paths with multi-instance safety: _C{chart}_S{studyId}
        probesPath_.Format("%s\\%s_%s_C%d_S%d_probes.csv",
            baseLogDir, studyName, symbol_.GetChars(), chartNumber_, studyId_);
        eventsPath_.Format("%s\\%s_%s_C%d_S%d_events.csv",
            baseLogDir, studyName, symbol_.GetChars(), chartNumber_, studyId_);
        amtPath_.Format("%s\\%s_%s_C%d_S%d_amt.csv",
            baseLogDir, studyName, symbol_.GetChars(), chartNumber_, studyId_);

        // Reset throttle state
        std::memset(lastLogBar_, 0, sizeof(lastLogBar_));
        std::memset(sessionCount_, 0, sizeof(sessionCount_));

        initialized_ = true;
    }

    void Shutdown()
    {
        // Phase 2.2: Flush any buffered data before shutdown
        FlushAll();
        initialized_ = false;
    }

    // Clear log files at start of full recalculation to prevent duplicate data
    // Call this when sc.IsFullRecalculation && sc.Index == 0
    void ClearLogsForFullRecalc()
    {
        if (!initialized_ || !sc_) return;

        // Truncate files by opening in rewrite mode (not append)
        // This clears existing content so recalc starts fresh
        int fileHandle = 0;

        // Clear events log
        sc_->OpenFile(eventsPath_, n_ACSIL::FILE_MODE_OPEN_TO_REWRITE_FROM_START, fileHandle);
        if (fileHandle != 0) {
            sc_->CloseFile(fileHandle);
            fileHandle = 0;
        }

        // Clear probes log
        sc_->OpenFile(probesPath_, n_ACSIL::FILE_MODE_OPEN_TO_REWRITE_FROM_START, fileHandle);
        if (fileHandle != 0) {
            sc_->CloseFile(fileHandle);
            fileHandle = 0;
        }

        // Clear amt log
        sc_->OpenFile(amtPath_, n_ACSIL::FILE_MODE_OPEN_TO_REWRITE_FROM_START, fileHandle);
        if (fileHandle != 0) {
            sc_->CloseFile(fileHandle);
            fileHandle = 0;
        }

        // Reset throttle state for fresh logging
        std::memset(lastLogBar_, 0, sizeof(lastLogBar_));
        std::memset(sessionCount_, 0, sizeof(sessionCount_));

        // Clear deduplication hashes - file is now empty, so all events are "new"
        loggedEventHashes_.clear();
        eventQueue_.clear();

        // Phase 2.2: Clear buffers and reset header flags
        eventsBuffer_.clear();
        amtBuffer_.clear();
        eventsHeaderWritten_ = false;
        amtHeaderWritten_ = false;
    }

    bool IsInitialized() const { return initialized_; }

    // =========================================================================
    // CONFIGURATION
    // =========================================================================

    void Configure(
        LogLevel level,
        int logWindowBars,
        bool probeEventsEnabled,
        bool probeResultsEnabled,
        bool scMessageEnabled,
        bool amtCsvEnabled,
        int flushIntervalBars)
    {
        level_ = level;
        logWindowBars_ = logWindowBars;
        probeEventsOn_ = probeEventsEnabled;
        probeResultsOn_ = probeResultsEnabled;
        scMessageOn_ = scMessageEnabled;
        amtCsvOn_ = amtCsvEnabled;
        flushInterval_ = flushIntervalBars;
    }

    // Phase 5: Module-level diagnostic controls
    void ConfigureModuleDiag(bool vbpDiag, bool sessionDiag, bool zoneDiag, bool deltaDiag)
    {
        enableVBPDiag_ = vbpDiag;
        enableSessionDiag_ = sessionDiag;
        enableZoneDiag_ = zoneDiag;
        enableDeltaDiag_ = deltaDiag;
    }

    // Check if module-specific diagnostics are enabled
    bool IsVBPDiagEnabled() const { return enableVBPDiag_; }
    bool IsSessionDiagEnabled() const { return enableSessionDiag_; }
    bool IsZoneDiagEnabled() const { return enableZoneDiag_; }
    bool IsDeltaDiagEnabled() const { return enableDeltaDiag_; }

    void UpdateArraySize(int size) { arraySize_ = size; }

    // Session boundary notification (resets rate limiters and increments session ID)
    void OnSessionChange(const char* sessionType = nullptr, SCDateTime startTime = SCDateTime())
    {
        std::memset(sessionCount_, 0, sizeof(sessionCount_));
        sessionId_++;
        if (sessionType) currentSessionType_ = sessionType;
        if (startTime.IsDateSet()) sessionStartTime_ = startTime;
    }

    int GetSessionId() const { return sessionId_; }
    const SCString& GetSessionType() const { return currentSessionType_; }

    // Live mode control (prevents duplicate event logging during full recalc)
    void SetLiveMode(bool live) {
        // When entering recalc, clear the queue (events will be re-generated)
        // but DON'T clear hashes - they prevent duplicates in the file
        // Only ClearLogsForFullRecalc() should clear both file AND hashes
        if (liveMode_ && !live) {
            eventQueue_.clear();
            // loggedEventHashes_ deliberately NOT cleared here
        }
        // When exiting recalc (not live -> live), flush queued events
        if (!liveMode_ && live) {
            FlushEventQueue();
        }
        liveMode_ = live;
    }
    bool IsLiveMode() const { return liveMode_; }

    // Clear logged event hashes (call on full recalc start)
    void ClearLoggedEvents() {
        loggedEventHashes_.clear();
        eventQueue_.clear();
    }

    // =========================================================================
    // POLICY LAYER
    // =========================================================================

    bool ShouldEmit(LogChannel ch, LogLevel req, int bar = -1)
    {
        if (!initialized_) return false;
        if (static_cast<int>(level_) < static_cast<int>(req)) return false;

        switch (ch) {
            case LogChannel::SC_MESSAGE:
                if (!scMessageOn_) return false;
                if (bar >= 0 && !InLogWindow(bar)) return false;
                break;
            case LogChannel::PROBE_CSV:
                if (!probeResultsOn_) return false;
                break;
            case LogChannel::EVENTS_CSV:
                if (!probeEventsOn_) return false;
                break;
            case LogChannel::AMT_CSV:
                if (!amtCsvOn_) return false;
                break;
        }
        return true;
    }

    // Throttling (allocation-free with enum keys)
    bool ShouldLog(ThrottleKey key, int bar, int cooldown = 1)
    {
        int idx = static_cast<int>(key);
        if (idx < 0 || idx >= static_cast<int>(ThrottleKey::_COUNT))
            return false;

        if (bar - lastLogBar_[idx] >= cooldown) {
            lastLogBar_[idx] = bar;
            return true;
        }
        return false;
    }

    bool ShouldLogRateLimited(ThrottleKey key, int maxPerSession = 10)
    {
        int idx = static_cast<int>(key);
        if (idx < 0 || idx >= static_cast<int>(ThrottleKey::_COUNT))
            return false;

        if (sessionCount_[idx] < maxPerSession) {
            sessionCount_[idx]++;
            return true;
        }
        return false;
    }

    // =========================================================================
    // TRANSPORT LAYER
    // =========================================================================

    void Log(LogChannel ch, LogCategory cat, const char* msg,
             SCDateTime time, int bar = -1, bool warn = false)
    {
        (void)bar;  // Unused in this path
        if (!initialized_) return;

        switch (ch) {
            case LogChannel::SC_MESSAGE:
                LogToSCInternal(cat, msg, warn);
                break;
            case LogChannel::EVENTS_CSV:
                // OLD FORMAT DISABLED - use LogSessionEvent() for structured output
                // LogToEventsCsv(cat, msg, time);
                break;
            case LogChannel::PROBE_CSV:
            case LogChannel::AMT_CSV:
                // Use structured logging methods
                break;
        }
    }

    void LogToSC(LogCategory cat, const char* msg, bool warn = false)
    {
        if (!initialized_ || !scMessageOn_) return;
        LogToSCInternal(cat, msg, warn);
    }

    // =========================================================================
    // CONVENIENCE METHODS (Phase 1.1)
    // These methods provide simple, gated logging with automatic throttling
    // =========================================================================

    // Get array size for external efficiency gating
    int GetArraySize() const { return arraySize_; }

    // Check if bar is in log window (for external efficiency checks)
    bool InLogWindowPublic(int bar) const { return InLogWindow(bar); }

    // Debug level - only logs at VERBOSE level, gated by log window
    void LogDebug(int bar, const char* msg, LogCategory cat = LogCategory::DEBUG)
    {
        if (!ShouldEmit(LogChannel::SC_MESSAGE, LogLevel::VERBOSE, bar)) return;
        LogToSCInternal(cat, msg, false);
    }

    // Info level - logs at MODERATE level, gated by log window
    void LogInfo(int bar, const char* msg, LogCategory cat = LogCategory::SYSTEM)
    {
        if (!ShouldEmit(LogChannel::SC_MESSAGE, LogLevel::MODERATE, bar)) return;
        LogToSCInternal(cat, msg, false);
    }

    // Warning level - logs at MINIMAL level, gated by log window
    void LogWarn(int bar, const char* msg, LogCategory cat = LogCategory::SYSTEM)
    {
        if (!ShouldEmit(LogChannel::SC_MESSAGE, LogLevel::MINIMAL, bar)) return;
        LogToSCInternal(cat, msg, true);
    }

    // Error level - always logs (no level check), still respects log window
    void LogError(int bar, const char* msg, LogCategory cat = LogCategory::ERROR_CAT)
    {
        if (!initialized_ || !scMessageOn_) return;
        if (bar >= 0 && !InLogWindow(bar)) return;
        LogToSCInternal(cat, msg, true);
    }

    // Rate-limited logging - max N times per session
    void LogOnce(ThrottleKey key, int bar, const char* msg,
                 LogCategory cat = LogCategory::SYSTEM, int maxPerSession = 1)
    {
        if (!ShouldLogRateLimited(key, maxPerSession)) return;
        if (!ShouldEmit(LogChannel::SC_MESSAGE, LogLevel::MODERATE, bar)) return;
        LogToSCInternal(cat, msg, false);
    }

    // Throttled logging - cooldown between logs (in bars)
    void LogThrottled(ThrottleKey key, int bar, int cooldown, const char* msg,
                      LogCategory cat = LogCategory::SYSTEM, bool warn = false)
    {
        if (!ShouldLog(key, bar, cooldown)) return;
        if (!ShouldEmit(LogChannel::SC_MESSAGE, LogLevel::MODERATE, bar)) return;
        LogToSCInternal(cat, msg, warn);
    }

    // Direct SC log without category prefix (for backwards compatibility)
    void LogDirect(int bar, const char* msg, bool warn = false)
    {
        if (!initialized_ || !scMessageOn_) return;
        if (bar >= 0 && !InLogWindow(bar)) return;
        if (!sc_) return;
        sc_->AddMessageToLog(msg, warn ? 1 : 0);
    }

    // =========================================================================
    // MODULE-SPECIFIC LOGGING (Phase 5)
    // These methods only log if the module diagnostic flag is enabled
    // =========================================================================

    // VBP module diagnostics - only logs if enableVBPDiag_ is true
    void LogVBPDiag(int bar, const char* msg)
    {
        if (!enableVBPDiag_) return;
        LogInfo(bar, msg, LogCategory::VBP);
    }

    // Session module diagnostics - only logs if enableSessionDiag_ is true
    void LogSessionDiag(int bar, const char* msg)
    {
        if (!enableSessionDiag_) return;
        LogInfo(bar, msg, LogCategory::SESSION);
    }

    // Zone module diagnostics - only logs if enableZoneDiag_ is true
    void LogZoneDiag(int bar, const char* msg)
    {
        if (!enableZoneDiag_) return;
        LogInfo(bar, msg, LogCategory::ZONE);
    }

    // Delta module diagnostics - only logs if enableDeltaDiag_ is true
    void LogDeltaDiag(int bar, const char* msg)
    {
        if (!enableDeltaDiag_) return;
        LogInfo(bar, msg, LogCategory::DELTA);
    }

    // =========================================================================
    // AMT STATE LOGGING
    // =========================================================================
    // Format: state + strength + location/intent + center used + evidence ledger + structure flags
    // Full snapshots on transitions, periodic snapshots otherwise

    /**
     * Log AMT state evidence.
     * User spec: "state + strength + location/intent + center used + evidence ledger + structure flags,
     *             with full snapshots on transitions"
     *
     * @param bar           Current bar index
     * @param state         Current AMT market state (BALANCE/IMBALANCE)
     * @param strength      State strength from leaky accumulator (0-1)
     * @param location      Value location (INSIDE_VA, ABOVE_VA, etc.)
     * @param intent        Value intent (TOWARD/AWAY/AT)
     * @param participation Participation mode (AGGRESSIVE/ABSORPTIVE/BALANCED)
     * @param activityType  Activity type (INITIATIVE/RESPONSIVE/NEUTRAL)
     * @param pocPrice      POC price (value center used)
     * @param vahPrice      VAH price
     * @param valPrice      VAL price
     * @param excessType    Excess type detected (NONE, POOR_HIGH, etc.)
     * @param singlePrint   Single print zone present
     * @param rotation      Rotation detected
     * @param rangeExt      Range extended this bar
     * @param ibBroken      IB broken
     * @param isTransition  Is this a state transition (triggers full snapshot)
     * @param prevState     Previous state (for transitions)
     */
    void LogAMTState(
        int bar,
        const char* state,
        double strength,
        const char* location,
        const char* intent,
        const char* participation,
        const char* activityType,
        double pocPrice,
        double vahPrice,
        double valPrice,
        const char* excessType,
        bool singlePrint,
        bool rotation,
        bool rangeExt,
        bool ibBroken,
        bool isTransition,
        const char* prevState = nullptr
    ) {
        if (!ShouldEmit(LogChannel::SC_MESSAGE, LogLevel::MODERATE, bar)) return;

        // Throttle non-transition logs (one per 10 bars)
        ThrottleKey key = isTransition ? ThrottleKey::AMT_STATE_TRANSITION : ThrottleKey::AMT_STATE;
        int cooldown = isTransition ? 1 : 10;  // Transitions always log, periodic every 10 bars
        if (!ShouldLog(key, bar, cooldown)) return;

        // Build structure flags string
        SCString structFlags;
        structFlags.Format("%s%s%s%s",
            singlePrint ? "SP " : "",
            rotation ? "ROT " : "",
            rangeExt ? "REXT " : "",
            ibBroken ? "IB_BRK" : "");

        // Format log line
        SCString msg;
        if (isTransition && prevState) {
            // Full transition snapshot
            msg.Format("Bar %d | %s->%s str=%.2f | loc=%s int=%s part=%s act=%s | POC=%.2f VAH=%.2f VAL=%.2f | ex=%s | flags=[%s]",
                bar,
                prevState, state,
                strength,
                location, intent, participation, activityType,
                pocPrice, vahPrice, valPrice,
                excessType,
                structFlags.GetChars());
        }
        else {
            // Periodic snapshot
            msg.Format("Bar %d | %s str=%.2f | loc=%s int=%s act=%s | POC=%.2f | ex=%s | [%s]",
                bar,
                state,
                strength,
                location, intent, activityType,
                pocPrice,
                excessType,
                structFlags.GetChars());
        }

        LogToSCInternal(LogCategory::AMT, msg.GetChars(), false);
    }

    /**
     * Log AMT state transition with full evidence snapshot.
     * Called when state flips from BALANCE<->IMBALANCE.
     */
    void LogAMTTransition(
        int bar,
        const char* fromState,
        const char* toState,
        double strength,
        double strengthAtTransition,
        const char* location,
        const char* intent,
        const char* participation,
        const char* activityType,
        double pocPrice,
        double vahPrice,
        double valPrice,
        double price,
        double deltaPct,
        const char* excessType,
        bool singlePrint,
        bool rotation,
        bool rangeExt,
        bool ibBroken,
        int barsInPrevState
    ) {
        if (!ShouldEmit(LogChannel::SC_MESSAGE, LogLevel::MINIMAL, bar)) return;

        // Transitions always log (no throttle)
        SCString structFlags;
        structFlags.Format("%s%s%s%s",
            singlePrint ? "SP " : "",
            rotation ? "ROT " : "",
            rangeExt ? "REXT " : "",
            ibBroken ? "IB_BRK" : "");

        SCString msg;
        msg.Format("[AMT-TRANSITION] Bar %d | %s->%s | str=%.2f (was %.2f) | barsInPrev=%d\n"
                   "    loc=%s int=%s part=%s act=%s\n"
                   "    price=%.2f delta=%.1f%% | POC=%.2f VAH=%.2f VAL=%.2f\n"
                   "    excess=%s | flags=[%s]",
            bar,
            fromState, toState,
            strength, strengthAtTransition, barsInPrevState,
            location, intent, participation, activityType,
            price, deltaPct * 100.0,
            pocPrice, vahPrice, valPrice,
            excessType,
            structFlags.GetChars());

        if (sc_) {
            sc_->AddMessageToLog(msg.GetChars(), 0);
        }
    }

    /**
     * Log AMT state evidence from StateEvidence struct.
     * Convenience wrapper for the full LogAMTState call.
     */
    void LogAMTStateEvidence(int bar, const StateEvidence& evidence, double price = 0.0) {
        const bool isTransition = evidence.IsTransition();

        if (isTransition) {
            // Full transition log
            LogAMTTransition(
                bar,
                AMTMarketStateToString(evidence.previousState),
                AMTMarketStateToString(evidence.currentState),
                evidence.stateStrength,
                evidence.strengthAtTransition,
                ValueLocationToString(evidence.location),
                ValueIntentToString(evidence.activity.intent_),
                ParticipationModeToString(evidence.activity.participation_),
                AMTActivityTypeToString(evidence.activity.activityType),
                evidence.pocPrice,
                evidence.vahPrice,
                evidence.valPrice,
                price,
                evidence.activity.deltaPct,
                ExcessTypeToString(evidence.excessDetected),
                evidence.singlePrintZonePresent,
                evidence.rotationDetected,
                evidence.rangeExtended,
                evidence.ibBroken,
                evidence.barsInState
            );
        }
        else {
            // Periodic state log
            LogAMTState(
                bar,
                AMTMarketStateToString(evidence.currentState),
                evidence.stateStrength,
                ValueLocationToString(evidence.location),
                ValueIntentToString(evidence.activity.intent_),
                ParticipationModeToString(evidence.activity.participation_),
                AMTActivityTypeToString(evidence.activity.activityType),
                evidence.pocPrice,
                evidence.vahPrice,
                evidence.valPrice,
                ExcessTypeToString(evidence.excessDetected),
                evidence.singlePrintZonePresent,
                evidence.rotationDetected,
                evidence.rangeExtended,
                evidence.ibBroken,
                false,  // Not a transition
                nullptr
            );
        }
    }

    // =========================================================================
    // STRUCTURED SESSION EVENT LOGGING
    // =========================================================================

    void LogSessionEvent(const SessionEvent& evt)
    {
        // Use probeEventsOn_ (Input 114) to control structured events CSV
        if (!initialized_ || !probeEventsOn_) return;
        if (!sc_) return;

        // EVENT DEDUPLICATION: Check if this exact event has been logged
        // Hash = (sessionId, barIndex, eventType) ensures each unique event logged once
        const int eventHash = CreateEventHash(sessionId_, evt.bar, static_cast<int>(evt.type));
        if (HasEventBeenLogged(eventHash)) {
            return;  // Already logged this exact event
        }

        // Mark as logged immediately to prevent duplicates
        MarkEventLogged(eventHash);

        // During recalc: queue event for later flush
        // Live mode: write directly to file
        if (!liveMode_) {
            // Queue event with current session ID
            eventQueue_.push_back(std::make_pair(evt, sessionId_));
            return;
        }

        // Live mode: write directly
        WriteEventToFile(evt, sessionId_);
    }

    // Flush queued events to file (call at end of recalc)
    void FlushEventQueue()
    {
        if (!initialized_ || !sc_ || eventQueue_.empty()) return;

        // Sort queue by bar index for chronological order
        std::sort(eventQueue_.begin(), eventQueue_.end(),
            [](const std::pair<SessionEvent, int>& a, const std::pair<SessionEvent, int>& b) {
                return a.first.bar < b.first.bar;
            });

        // Write all queued events
        for (const auto& evtPair : eventQueue_) {
            WriteEventToFile(evtPair.first, evtPair.second);
        }

        eventQueue_.clear();
    }

private:
    void WriteEventToFile(const SessionEvent& evt, int sessionIdToUse)
    {
        int fileHandle = 0;
        unsigned int bytesWritten = 0;

        bool needsHeader = FileNeedsHeader(eventsPath_.GetChars());

        sc_->OpenFile(eventsPath_, n_ACSIL::FILE_MODE_OPEN_TO_APPEND, fileHandle);
        if (fileHandle == 0) return;

        // Write header if needed - structured columns
        if (needsHeader) {
            const char* header =
                "session_id,session_type,ts,bar,event_type,"
                "zone_id,zone_type,entry_price,exit_price,bars,outcome,escape_vel,vol_ratio,"
                "delta_conf,sess_delta_pct,sess_delta_pctl,coherent,aggression,facilitation,market_state,phase,"
                "volume,range,vol_pctl,range_pctl,"
                "poc,vah,val,"
                "total_eng,accept,reject,tag,probe,test,"
                "message\n";
            sc_->WriteFile(fileHandle, header, static_cast<unsigned int>(strlen(header)), &bytesWritten);
        }

        // Format structured line
        SCString ts = FormatDateTime(evt.timestamp);
        SCString line;
        line.Format(
            "%d,%s,%s,%d,%s,"                                           // session_id, session_type, ts, bar, event_type
            "%d,%s,%.2f,%.2f,%d,%s,%.2f,%.2f,"                         // zone fields
            "%.2f,%.4f,%d,%d,%s,%s,%s,%s,"                             // intent/state fields
            "%.0f,%.2f,%.1f,%.1f,"                                     // diagnostic fields
            "%.2f,%.2f,%.2f,"                                          // VBP fields
            "%d,%d,%d,%d,%d,%d,"                                       // session summary fields
            "%s\n",                                                    // message
            sessionIdToUse,
            currentSessionType_.GetLength() > 0 ? currentSessionType_.GetChars() : evt.sessionType.GetChars(),
            ts.GetChars(),
            evt.bar,
            SessionEventTypeName(evt.type),
            // Zone fields
            evt.zoneId,
            evt.zoneType.GetLength() > 0 ? evt.zoneType.GetChars() : "",
            evt.entryPrice,
            evt.exitPrice,
            evt.engagementBars,
            evt.outcome.GetLength() > 0 ? evt.outcome.GetChars() : "",
            evt.escapeVel,
            evt.volRatio,
            // Intent/state fields
            evt.deltaConf,
            evt.sessDeltaPct,
            evt.sessDeltaPctl,
            evt.coherent,
            evt.aggression.GetLength() > 0 ? evt.aggression.GetChars() : "",
            evt.facilitation.GetLength() > 0 ? evt.facilitation.GetChars() : "",
            evt.marketState.GetLength() > 0 ? evt.marketState.GetChars() : "",
            evt.phase.GetLength() > 0 ? evt.phase.GetChars() : "",
            // Diagnostic fields
            evt.volume,
            evt.range,
            evt.volPctl,
            evt.rangePctl,
            // VBP fields
            evt.poc,
            evt.vah,
            evt.val,
            // Session summary fields
            evt.totalEngagements,
            evt.acceptCount,
            evt.rejectCount,
            evt.tagCount,
            evt.probeCount,
            evt.testCount,
            // Message
            EscapeCsv(evt.message.GetChars()).GetChars()
        );

        sc_->WriteFile(fileHandle, line.GetChars(), line.GetLength(), &bytesWritten);
        sc_->CloseFile(fileHandle);
    }

public:

    // =========================================================================
    // PROBE-SPECIFIC LOGGING
    // =========================================================================

    void LogProbeFired(const AMT::ProbeRequest& req, SCDateTime barTime)
    {
        if (!initialized_ || !probeResultsOn_) return;

        // Use SC native file API for reliable live trading writes
        int fileHandle = 0;
        unsigned int bytesWritten = 0;

        bool needsHeader = FileNeedsHeader(probesPath_.GetChars());

        sc_->OpenFile(probesPath_, n_ACSIL::FILE_MODE_OPEN_TO_APPEND, fileHandle);
        if (fileHandle == 0) return;

        // Write header if needed
        if (needsHeader) {
            const char* header = "ts,event,symbol,timeframe,probe_id,scenario_id,"
                "direction,hypothesis,pivot_price,score_total,"
                "tier1_volume,tier2_delta,tier3_progress,"
                "status,mechanism,duration_ms,mfe_ticks,mae_ticks,"
                "obs_bars,message\n";
            sc_->WriteFile(fileHandle, header, static_cast<unsigned int>(strlen(header)), &bytesWritten);
        }

        // Format line
        SCString ts = FormatDateTime(barTime);
        SCString dirStr = (req.direction == AMT::ProbeDirection::LONG) ? "LONG" : "SHORT";
        SCString line;
        line.Format("\"%s\",\"FIRED\",\"%s\",\"%s\",%d,%d,\"%s\",\"%s\",%.4f,%.2f,,,,,,,,,,\n",
            ts.GetChars(), symbol_.GetChars(), timeframe_.GetChars(),
            req.probe_id, req.scenario_id, dirStr.GetChars(),
            req.hypothesis, req.price, req.score);

        sc_->WriteFile(fileHandle, line.GetChars(), line.GetLength(), &bytesWritten);
        sc_->CloseFile(fileHandle);
    }

    void LogProbeResolved(const AMT::ProbeRequest& req, const AMT::ProbeResult& result,
                          int obsBars, SCDateTime barTime)
    {
        if (!initialized_ || !probeResultsOn_) return;

        // Use SC native file API for reliable live trading writes
        int fileHandle = 0;
        unsigned int bytesWritten = 0;

        bool needsHeader = FileNeedsHeader(probesPath_.GetChars());

        sc_->OpenFile(probesPath_, n_ACSIL::FILE_MODE_OPEN_TO_APPEND, fileHandle);
        if (fileHandle == 0) return;

        // Write header if needed
        if (needsHeader) {
            const char* header = "ts,event,symbol,timeframe,probe_id,scenario_id,"
                "direction,hypothesis,pivot_price,score_total,"
                "tier1_volume,tier2_delta,tier3_progress,"
                "status,mechanism,duration_ms,mfe_ticks,mae_ticks,"
                "obs_bars,message\n";
            sc_->WriteFile(fileHandle, header, static_cast<unsigned int>(strlen(header)), &bytesWritten);
        }

        // Format line
        SCString ts = FormatDateTime(barTime);
        SCString dirStr = (req.direction == AMT::ProbeDirection::LONG) ? "LONG" : "SHORT";
        SCString statusStr = AMT::to_string(result.status);
        SCString mechStr = AMT::to_string(result.mechanism);
        SCString line;
        line.Format("\"%s\",\"RESOLVED\",\"%s\",\"%s\",%d,%d,\"%s\",\"%s\",%.4f,%.2f,,,,%s,%s,%d,%.1f,%.1f,%d,\n",
            ts.GetChars(), symbol_.GetChars(), timeframe_.GetChars(),
            req.probe_id, req.scenario_id, dirStr.GetChars(),
            req.hypothesis, req.price, req.score,
            statusStr, mechStr, result.observation_time_ms,
            result.mfe, result.mae, obsBars);

        sc_->WriteFile(fileHandle, line.GetChars(), line.GetLength(), &bytesWritten);
        sc_->CloseFile(fileHandle);
    }

    // =========================================================================
    // AMT CSV LOGGING
    // =========================================================================

    void LogAmtBar(const AmtBarData& d)
    {
        if (!initialized_) return;
        if (!amtCsvOn_) return;

        // Phase 2.2: Use buffered writing for high-frequency bar logging
        // Format timestamp as YYYY-MM-DD HH:MM:SS
        double dt = d.timestamp.GetAsDouble();
        int days = static_cast<int>(dt);
        double timeFrac = dt - days;

        // Convert days to date (simplified - good enough for logging)
        int totalDays = days - 2;  // Adjust for Excel's 1900 leap year bug
        int year = 1900;
        while (year < 3000) {
            int daysInYear = ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0) ? 366 : 365;
            if (totalDays < daysInYear) break;
            totalDays -= daysInYear;
            year++;
        }

        static const int daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        bool isLeap = ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0);
        int month = 0;
        for (int m = 0; m < 12; m++) {
            int dim = daysInMonth[m] + (m == 1 && isLeap ? 1 : 0);
            if (totalDays < dim) { month = m + 1; break; }
            totalDays -= dim;
        }
        int day = totalDays + 1;

        // Convert time fraction to HH:MM:SS
        int totalSecs = static_cast<int>(timeFrac * 86400.0 + 0.5);
        int hour = totalSecs / 3600;
        int minute = (totalSecs % 3600) / 60;
        int second = totalSecs % 60;

        // Format line
        SCString line;
        line.Format("%04d-%02d-%02d %02d:%02d:%02d,%d,%.2f,%.2f,%.2f,%.0f,%d,%s,%.2f,%.2f,%.2f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.4f,%.4f,%.4f,%d,%d\n",
            year, month, day, hour, minute, second,
            d.barIndex,
            d.price, d.high, d.low,
            d.volume,
            static_cast<int>(d.delta),
            d.phase.GetChars(),
            d.pocPrice, d.vahPrice, d.valPrice,
            d.hasVAH, d.hasPOC, d.hasVAL,
            d.vahProximity, d.pocProximity, d.valProximity,
            d.vahTouches, d.pocTouches, d.valTouches,
            d.vahStrength, d.pocStrength, d.valStrength,
            d.activeZoneCount, d.totalTouches);

        // Add to buffer (auto-flushes when full)
        AppendToAmtBuffer(line);
    }

    // =========================================================================
    // FLUSH CONTROL
    // =========================================================================

    void MaybeFlush(int bar, bool forceOnLast)
    {
        bool shouldFlush = forceOnLast ||
            (flushInterval_ > 0 && bar % flushInterval_ == 0);

        if (shouldFlush) {
            FlushAll();
        }
    }

    void FlushAll()
    {
        // Phase 2.2: Flush all buffered writes
        FlushEventsBuffer();
        FlushAmtBuffer();
    }

private:
    // =========================================================================
    // FILE PATHS (SC native file API used for writes)
    // =========================================================================

    SCString probesPath_;
    SCString eventsPath_;
    SCString amtPath_;

    // =========================================================================
    // CONFIGURATION STATE
    // =========================================================================

    LogLevel level_ = LogLevel::MINIMAL;
    int logWindowBars_ = 100;
    int flushInterval_ = 100;
    bool probeEventsOn_ = false;
    bool probeResultsOn_ = true;
    bool scMessageOn_ = true;
    bool amtCsvOn_ = false;

    // Phase 5: Module-level diagnostic enables
    bool enableVBPDiag_ = false;
    bool enableSessionDiag_ = false;
    bool enableZoneDiag_ = false;
    bool enableDeltaDiag_ = false;

    // =========================================================================
    // THROTTLE STATE
    // =========================================================================

    int lastLogBar_[static_cast<int>(ThrottleKey::_COUNT)];
    int sessionCount_[static_cast<int>(ThrottleKey::_COUNT)];

    // =========================================================================
    // STUDY CONTEXT
    // =========================================================================

    s_sc* sc_ = nullptr;
    int chartNumber_ = 0;
    int studyId_ = 0;
    int arraySize_ = 0;

    SCString symbol_;
    SCString timeframe_;
    SCString baseDir_;
    bool initialized_ = false;

    // =========================================================================
    // SESSION TRACKING (for structured CSV grouping)
    // =========================================================================

    int sessionId_ = 0;              // Incremented on each session change
    SCString currentSessionType_;    // "RTH", "GLOBEX"
    SCDateTime sessionStartTime_;    // Start time of current session

    // =========================================================================
    // RECALC CONTROL (prevents duplicate event logging during full recalc)
    // =========================================================================

    bool liveMode_ = false;          // True after initial recalc completes

    // =========================================================================
    // EVENT DEDUPLICATION (prevents duplicate events on recalc)
    // =========================================================================

    std::set<int> loggedEventHashes_;  // Hash of (sessionId, barIndex, eventType)
    std::vector<std::pair<SessionEvent, int>> eventQueue_;  // Queue for events during recalc (event, sessionId)

    // =========================================================================
    // BATCHED FILE WRITING (Phase 2.2)
    // =========================================================================

    static constexpr int BUFFER_FLUSH_SIZE = 50;  // Flush every 50 entries
    std::vector<SCString> eventsBuffer_;          // Buffered events CSV lines
    std::vector<SCString> amtBuffer_;             // Buffered AMT CSV lines
    bool eventsHeaderWritten_ = false;            // Track if header has been written
    bool amtHeaderWritten_ = false;

    // Create unique hash for event deduplication
    // Hash = (sessionId << 20) | (barIndex << 4) | eventType
    int CreateEventHash(int sessionId, int barIndex, int eventType) const {
        return (sessionId << 20) | ((barIndex & 0xFFFF) << 4) | (eventType & 0xF);
    }

    bool HasEventBeenLogged(int hash) const {
        return loggedEventHashes_.find(hash) != loggedEventHashes_.end();
    }

    void MarkEventLogged(int hash) {
        loggedEventHashes_.insert(hash);
    }

    // =========================================================================
    // HELPERS
    // =========================================================================

    // SC-compliant file size check using native ACSIL file API
    bool FileNeedsHeader(const char* path) const
    {
        if (!sc_) return true;

        int fileHandle = 0;
        // Try to open for reading - if file doesn't exist, needs header
        sc_->OpenFile(path, n_ACSIL::FILE_MODE_OPEN_EXISTING_FOR_SEQUENTIAL_READING, fileHandle);
        if (fileHandle == 0) return true;

        // Try to read 1 byte to check if file has content
        char testByte = 0;
        unsigned int bytesRead = 0;
        sc_->ReadFile(fileHandle, &testByte, 1, &bytesRead);
        sc_->CloseFile(fileHandle);

        // If we read 0 bytes, file is empty and needs header
        return (bytesRead == 0);
    }

    void LogToSCInternal(LogCategory cat, const char* msg, bool warn)
    {
        if (!sc_) return;

        SCString formatted;
        formatted.Format("%s %s", CatPrefix(cat), msg);
        sc_->AddMessageToLog(formatted, warn ? 1 : 0);
    }

    void LogToEventsCsv(LogCategory cat, const char* msg, SCDateTime time)
    {
        if (!sc_) return;

        // Use SC native file API for reliable live trading writes
        int fileHandle = 0;
        unsigned int bytesWritten = 0;

        bool needsHeader = FileNeedsHeader(eventsPath_.GetChars());

        sc_->OpenFile(eventsPath_, n_ACSIL::FILE_MODE_OPEN_TO_APPEND, fileHandle);
        if (fileHandle == 0) return;

        // Write header if needed
        if (needsHeader) {
            const char* header = "ts,event,symbol,timeframe,category,message\n";
            sc_->WriteFile(fileHandle, header, static_cast<unsigned int>(strlen(header)), &bytesWritten);
        }

        // Format line - escape msg for CSV
        SCString ts = FormatDateTime(time);
        SCString escapedMsg = EscapeCsv(msg);
        SCString line;
        line.Format("\"%s\",\"MSG\",\"%s\",\"%s\",\"%s\",%s\n",
            ts.GetChars(), symbol_.GetChars(), timeframe_.GetChars(),
            CatPrefix(cat), escapedMsg.GetChars());

        sc_->WriteFile(fileHandle, line.GetChars(), line.GetLength(), &bytesWritten);
        sc_->CloseFile(fileHandle);
    }

    bool InLogWindow(int bar) const
    {
        if (logWindowBars_ == 0) return true;
        return (bar >= arraySize_ - logWindowBars_);
    }

    const char* CatPrefix(LogCategory cat) const
    {
        switch (cat) {
            case LogCategory::PROBE:     return "[PROBE]";
            case LogCategory::AMT:       return "[AMT]";
            case LogCategory::ZONE:      return "[ZONE]";
            case LogCategory::SESSION:   return "[SESSION]";
            case LogCategory::DRIFT:     return "[DRIFT]";
            case LogCategory::VBP:       return "[VBP]";
            case LogCategory::SYSTEM:    return "[SYSTEM]";
            case LogCategory::DEBUG:     return "[DEBUG]";
            case LogCategory::REPLAY:    return "[REPLAY]";
            case LogCategory::WARMUP:    return "[WARMUP]";
            case LogCategory::EXTREME:   return "[EXTREME]";
            case LogCategory::SSOT:      return "[SSOT]";
            case LogCategory::VAL:       return "[VAL]";
            case LogCategory::INPUT:     return "[INPUT]";
            case LogCategory::DELTA:     return "[DELTA]";
            case LogCategory::INIT:      return "[INIT]";
            case LogCategory::PERF:      return "[PERF]";
            case LogCategory::BASELINE:  return "[BASELINE]";
            case LogCategory::ACCUM:     return "[ACCUM]";
            case LogCategory::AUDIT:     return "[AUDIT]";
            case LogCategory::ERROR_CAT: return "[ERROR]";
            default:                     return "[LOG]";
        }
    }

    SCString FormatDateTime(SCDateTime dt) const
    {
        int year, month, day, hour, minute, second;
        dt.GetDateTimeYMDHMS(year, month, day, hour, minute, second);

        double d = dt.GetAsDouble();
        double frac = d - floor(d);
        int ms = static_cast<int>((frac * 86400.0 - floor(frac * 86400.0 + 0.0000001)) * 1000.0);
        if (ms < 0) ms = 0;
        if (ms > 999) ms = 999;

        SCString s;
        s.Format("%04d-%02d-%02d %02d:%02d:%02d.%03d",
            year, month, day, hour, minute, second, ms);
        return s;
    }

    SCString EscapeCsv(const char* input) const
    {
        if (input == nullptr) return "\"\"";
        SCString raw(input);
        SCString out = "\"";
        for (int i = 0; i < raw.GetLength(); ++i) {
            char c = raw.GetChars()[i];
            if (c == '"')
                out += "\"\"";
            else
                out += c;
        }
        out += "\"";
        return out;
    }

    // =========================================================================
    // BATCHED FILE WRITING (Phase 2.2)
    // =========================================================================

    void AppendToEventsBuffer(const SCString& line) {
        eventsBuffer_.push_back(line);
        if (static_cast<int>(eventsBuffer_.size()) >= BUFFER_FLUSH_SIZE) {
            FlushEventsBuffer();
        }
    }

    void AppendToAmtBuffer(const SCString& line) {
        amtBuffer_.push_back(line);
        if (static_cast<int>(amtBuffer_.size()) >= BUFFER_FLUSH_SIZE) {
            FlushAmtBuffer();
        }
    }

    void FlushEventsBuffer() {
        if (eventsBuffer_.empty() || !sc_) return;

        int fileHandle = 0;
        unsigned int bytesWritten = 0;
        sc_->OpenFile(eventsPath_, n_ACSIL::FILE_MODE_OPEN_TO_APPEND, fileHandle);
        if (fileHandle == 0) return;

        // Write header if not yet written this session
        if (!eventsHeaderWritten_ && FileNeedsHeader(eventsPath_.GetChars())) {
            const char* header =
                "session_id,session_type,ts,bar,event_type,"
                "zone_id,zone_type,entry_price,exit_price,bars,outcome,escape_vel,vol_ratio,"
                "delta_conf,sess_delta_pct,sess_delta_pctl,coherent,aggression,facilitation,market_state,phase,"
                "volume,range,vol_pctl,range_pctl,"
                "poc,vah,val,"
                "total_eng,accept,reject,tag,probe,test,"
                "message\n";
            sc_->WriteFile(fileHandle, header, static_cast<unsigned int>(strlen(header)), &bytesWritten);
            eventsHeaderWritten_ = true;
        }

        // Write all buffered lines
        for (const auto& line : eventsBuffer_) {
            sc_->WriteFile(fileHandle, line.GetChars(), line.GetLength(), &bytesWritten);
        }

        sc_->CloseFile(fileHandle);
        eventsBuffer_.clear();
    }

    void FlushAmtBuffer() {
        if (amtBuffer_.empty() || !sc_) return;

        int fileHandle = 0;
        unsigned int bytesWritten = 0;
        sc_->OpenFile(amtPath_, n_ACSIL::FILE_MODE_OPEN_TO_APPEND, fileHandle);
        if (fileHandle == 0) return;

        // Write header if not yet written this session
        if (!amtHeaderWritten_ && FileNeedsHeader(amtPath_.GetChars())) {
            // Header must match LogAmtBar() line.Format() columns exactly
            const char* header =
                "ts,bar,close,high,low,volume,delta,phase,"
                "poc,vah,val,"
                "has_vah,has_poc,has_val,"
                "vah_proximity,poc_proximity,val_proximity,"
                "vah_touches,poc_touches,val_touches,"
                "vah_strength,poc_strength,val_strength,"
                "active_zones,total_touches\n";
            sc_->WriteFile(fileHandle, header, static_cast<unsigned int>(strlen(header)), &bytesWritten);
            amtHeaderWritten_ = true;
        }

        // Write all buffered lines
        for (const auto& line : amtBuffer_) {
            sc_->WriteFile(fileHandle, line.GetChars(), line.GetLength(), &bytesWritten);
        }

        sc_->CloseFile(fileHandle);
        amtBuffer_.clear();
    }
};

} // namespace AMT

#endif // AMT_LOGGER_H
