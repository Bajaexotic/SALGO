// ============================================================================
// AMT_Core.h
// Core enums and constants for Auction Market Theory framework
// Single source of truth for all AMT classifications
// ============================================================================

#ifndef AMT_CORE_H
#define AMT_CORE_H

#include <string>
#include <cmath>
#include <cassert>

namespace AMT {

// ============================================================================
// TIME THRESHOLDS (minutes from RTH open)
// ============================================================================
namespace Thresholds {
    // Phase boundaries (minutes from RTH open, or minutes before RTH close)
    constexpr int PHASE_IB_COMPLETE = 60;       // Initial Balance = first 60 min
    constexpr int PHASE_CLOSING_WINDOW = 45;    // Closing = last 45 min of RTH

    // Evening phase boundaries (seconds from midnight, ET)
    constexpr int POST_CLOSE_END_SEC = 61200;   // 17:00:00
    constexpr int MAINTENANCE_END_SEC = 64800;  // 18:00:00
    constexpr int LONDON_OPEN_SEC = 10800;      // 03:00:00 (DST risk: fixed ET)
    constexpr int PRE_MARKET_START_SEC = 30600; // 08:30:00

    constexpr float CONFIDENCE_HIGH = 0.70f;
    constexpr float CONFIDENCE_TRADEABLE = 0.50f;
    constexpr float CONFIDENCE_LOW = 0.30f;
}

// ============================================================================
// SESSION PHASE ENUM (SSOT for session/phase classification)
// ============================================================================

enum class SessionPhase : int {
    UNKNOWN = -1,

    // EVENING phases (Globex session container)
    GLOBEX = 0,             // [18:00:00, 03:00:00) - Asia/overnight (wraps midnight)
    LONDON_OPEN = 1,        // [03:00:00, 08:30:00) - European session (DST risk: fixed ET)
    PRE_MARKET = 2,         // [08:30:00, 09:30:00) - Pre-RTH activity

    // RTH phases
    INITIAL_BALANCE = 3,    // [09:30:00, 10:30:00) - First 60 min (IB)
    MID_SESSION = 4,        // [10:30:00, 15:30:00) - Core RTH
    CLOSING_SESSION = 5,    // [15:30:00, 16:15:00) - Last 45 min

    // EVENING phases (post-RTH)
    POST_CLOSE = 6,         // [16:15:00, 17:00:00) - Settlement period
    MAINTENANCE = 7         // [17:00:00, 18:00:00) - CME Globex maintenance
};

// Legacy aliases for backward compatibility
constexpr SessionPhase OPENING_DRIVE = SessionPhase::INITIAL_BALANCE;  // Deprecated: use INITIAL_BALANCE
constexpr SessionPhase IB_CONFIRMATION = SessionPhase::INITIAL_BALANCE; // Deprecated: use INITIAL_BALANCE

// ============================================================================
// PRIOR VBP STATE (Tri-State Contract for Prior Session Availability)
// ============================================================================
// Formalizes the distinction between "data unavailable" vs "logic error":
//   - PRIOR_VALID: Prior session data exists and differs from current
//   - PRIOR_MISSING: Insufficient history (chart/study not built yet)
//   - PRIOR_DUPLICATES_CURRENT: Prior exists but matches current (true defect)
//
// Usage:
//   - PRIOR_MISSING: Run in degraded mode, skip prior zones, log once per session
//   - PRIOR_DUPLICATES_CURRENT: Log as BUG with diagnostic context for repro
// ============================================================================

enum class PriorVBPState : int {
    PRIOR_VALID = 0,             // Prior exists and differs from current
    PRIOR_MISSING = 1,           // Insufficient history / profiles not built yet
    PRIOR_DUPLICATES_CURRENT = 2 // Prior should exist but matches current (defect)
};

inline const char* to_string(PriorVBPState state) {
    switch (state) {
        case PriorVBPState::PRIOR_VALID: return "VALID";
        case PriorVBPState::PRIOR_MISSING: return "MISSING";
        case PriorVBPState::PRIOR_DUPLICATES_CURRENT: return "DUPLICATES_CURRENT";
        default: return "UNKNOWN";
    }
}

// ============================================================================
// SESSION KEY (Deterministic Session Identity)
// ============================================================================
// SessionKey uniquely identifies a trading session without ambiguity.
// Unlike SessionPhase (which tracks intra-session phases), SessionKey is:
//   - Deterministic: Always computable from bar time (never UNKNOWN)
//   - Stable: Does not change during a session
//   - Comparable: Supports operator== for transition detection
//
// Usage:
//   SessionKey key = ComputeSessionKey(dateYYYYMMDD, timeOfDaySec, rthStartSec, rthEndSec);
//   bool changed = (key != prevKey);
// ============================================================================

enum class SessionType : int {
    RTH = 0,      // Regular Trading Hours (09:30-16:15 ET for ES)
    GLOBEX = 1    // Globex session (all non-RTH hours)
};

struct SessionKey {
    int tradingDay = 0;           // YYYYMMDD format (e.g., 20241222)
    SessionType sessionType = SessionType::GLOBEX;

    bool operator==(const SessionKey& other) const {
        return tradingDay == other.tradingDay && sessionType == other.sessionType;
    }

    bool operator!=(const SessionKey& other) const {
        return !(*this == other);
    }

    // Required for std::set<SessionKey> ordering
    bool operator<(const SessionKey& other) const {
        if (tradingDay != other.tradingDay)
            return tradingDay < other.tradingDay;
        return static_cast<int>(sessionType) < static_cast<int>(other.sessionType);
    }

    bool IsRTH() const { return sessionType == SessionType::RTH; }
    bool IsGlobex() const { return sessionType == SessionType::GLOBEX; }

    // Check if this key represents a valid session
    bool IsValid() const { return tradingDay > 0; }

    std::string ToString() const {
        if (tradingDay == 0) return "INVALID";
        return std::to_string(tradingDay) + (IsRTH() ? "-RTH" : "-GBX");
    }
};

// ============================================================================
// BASELINE PHASE (Three-Phase Execution Model)
// ============================================================================
// Controls how bars are processed during chart load/recalc:
//   - BOOTSTRAP: One-time O(sessions) discovery at chart start
//   - BASELINE_ACCUMULATION: Bars in eligible prior sessions feed baselines only
//   - ACTIVE_SESSION: Current session bars run full strategy logic
//
// Key invariant: Current session NEVER contributes to baselines.
// ============================================================================

enum class BaselinePhase : int {
    BOOTSTRAP = 0,              // O(sessions) discovery at recalc start
    BASELINE_ACCUMULATION = 1,  // Prior session bars -> baseline only
    ACTIVE_SESSION = 2          // Current session -> full strategy
};

inline const char* BaselinePhaseToString(BaselinePhase phase) {
    switch (phase) {
        case BaselinePhase::BOOTSTRAP: return "BOOTSTRAP";
        case BaselinePhase::BASELINE_ACCUMULATION: return "BASELINE";
        case BaselinePhase::ACTIVE_SESSION: return "ACTIVE";
        default: return "UNKNOWN";
    }
}

// ============================================================================
// BASELINE READINESS (No-Fallback Contract)
// ============================================================================
// Explicit state for baseline availability - replaces silent fallbacks.
//
// Contract:
//   - READY: Baseline has sufficient samples, numeric outputs valid
//   - WARMUP: Insufficient samples, numeric outputs UNDEFINED
//   - STALE: Data too old or context changed, numeric outputs UNDEFINED
//   - UNAVAILABLE: Input source not configured, numeric outputs UNDEFINED
//
// Usage:
//   if (readiness != BaselineReadiness::READY) {
//       // Do NOT use numeric values - set *Valid=false and skip metric
//   }
// ============================================================================

enum class BaselineReadiness : int {
    READY = 0,           // Sufficient samples, outputs valid
    WARMUP = 1,          // Insufficient samples (building up)
    STALE = 2,           // RESERVED: Not enforced in Stage 1/2 (requires timestamp tracking)
    UNAVAILABLE = 3      // Input source not configured
};

inline const char* BaselineReadinessToString(BaselineReadiness r) {
    switch (r) {
        case BaselineReadiness::READY: return "READY";
        case BaselineReadiness::WARMUP: return "WARMUP";
        case BaselineReadiness::STALE: return "STALE";
        case BaselineReadiness::UNAVAILABLE: return "UNAVAILABLE";
        default: return "UNKNOWN";
    }
}

// Helper: Decrement a YYYYMMDD date by 1 day
// Handles month/year rollover (e.g., 20241201 -> 20241130)
inline int DecrementDate(int dateYYYYMMDD) {
    int year = dateYYYYMMDD / 10000;
    int month = (dateYYYYMMDD / 100) % 100;
    int day = dateYYYYMMDD % 100;

    day--;
    if (day < 1) {
        month--;
        if (month < 1) {
            year--;
            month = 12;
        }
        // Days in month (handle leap years for February)
        static const int daysInMonth[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        day = daysInMonth[month];
        if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) {
            day = 29;  // Leap year February
        }
    }

    return year * 10000 + month * 100 + day;
}

// ============================================================================
// EFFORT BASELINE BUCKET (SessionPhase-Based)
// ============================================================================
// Each tradeable SessionPhase has its own baseline bucket. Bars are compared
// against historical bars from the SAME phase (e.g., PRE_MARKET vs PRE_MARKET).
//
// Tradeable phases (7 buckets):
//   GLOBEX, LONDON_OPEN, PRE_MARKET, INITIAL_BALANCE, MID_SESSION,
//   CLOSING_SESSION, POST_CLOSE
//
// MAINTENANCE phase has no trading - excluded from baselines.
// ============================================================================

// Number of tradeable session phases (excludes MAINTENANCE and UNKNOWN)
static constexpr int EFFORT_BUCKET_COUNT = 7;

// Convert SessionPhase to bucket index [0-6], or -1 if not tradeable
inline int SessionPhaseToBucketIndex(SessionPhase phase) {
    switch (phase) {
        case SessionPhase::GLOBEX:          return 0;
        case SessionPhase::LONDON_OPEN:     return 1;
        case SessionPhase::PRE_MARKET:      return 2;
        case SessionPhase::INITIAL_BALANCE: return 3;
        case SessionPhase::MID_SESSION:     return 4;
        case SessionPhase::CLOSING_SESSION: return 5;
        case SessionPhase::POST_CLOSE:      return 6;
        default:                            return -1;  // MAINTENANCE, UNKNOWN
    }
}

// Convert bucket index back to SessionPhase
inline SessionPhase BucketIndexToSessionPhase(int index) {
    switch (index) {
        case 0: return SessionPhase::GLOBEX;
        case 1: return SessionPhase::LONDON_OPEN;
        case 2: return SessionPhase::PRE_MARKET;
        case 3: return SessionPhase::INITIAL_BALANCE;
        case 4: return SessionPhase::MID_SESSION;
        case 5: return SessionPhase::CLOSING_SESSION;
        case 6: return SessionPhase::POST_CLOSE;
        default: return SessionPhase::UNKNOWN;
    }
}

// Check if a SessionPhase is tradeable (has a baseline bucket)
inline bool IsTradeablePhase(SessionPhase phase) {
    return SessionPhaseToBucketIndex(phase) >= 0;
}

// Get phase duration in seconds (for expected bars calculation)
inline int GetPhaseDurationSeconds(SessionPhase phase) {
    switch (phase) {
        case SessionPhase::GLOBEX:          return 9 * 3600;        // 18:00-03:00 = 9h
        case SessionPhase::LONDON_OPEN:     return 5 * 3600 + 1800; // 03:00-08:30 = 5.5h
        case SessionPhase::PRE_MARKET:      return 3600;            // 08:30-09:30 = 1h
        case SessionPhase::INITIAL_BALANCE: return 3600;            // 09:30-10:30 = 1h
        case SessionPhase::MID_SESSION:     return 5 * 3600;        // 10:30-15:30 = 5h
        case SessionPhase::CLOSING_SESSION: return 2700;            // 15:30-16:15 = 45m
        case SessionPhase::POST_CLOSE:      return 2700;            // 16:15-17:00 = 45m
        default:                            return 0;
    }
}

// Expected bars per phase (for coverage threshold calculation)
inline int GetExpectedBarsInPhase(SessionPhase phase, int barIntervalSeconds) {
    if (!IsTradeablePhase(phase) || barIntervalSeconds <= 0)
        return 0;
    return GetPhaseDurationSeconds(phase) / barIntervalSeconds;
}

// ============================================================================
// BUCKET BASELINE STATE (Effort Baselines)
// ============================================================================
// Validity state for effort bucket baselines with explicit diagnostics.
// ============================================================================

enum class BucketBaselineState : int {
    READY = 0,                    // >= 5 sessions with sufficient coverage each
    INSUFFICIENT_SESSIONS = 1,    // < 5 prior sessions contributed to this bucket
    INSUFFICIENT_COVERAGE = 2,    // Sessions exist but bucket coverage below threshold
    NOT_APPLICABLE = 3            // Non-RTH bar (no effort baseline applies)
};

inline const char* BucketBaselineStateToString(BucketBaselineState s) {
    switch (s) {
        case BucketBaselineState::READY:                 return "READY";
        case BucketBaselineState::INSUFFICIENT_SESSIONS: return "INSUFFICIENT_SESSIONS";
        case BucketBaselineState::INSUFFICIENT_COVERAGE: return "INSUFFICIENT_COVERAGE";
        case BucketBaselineState::NOT_APPLICABLE:        return "NOT_APPLICABLE";
        default:                                         return "UNKNOWN";
    }
}

// ============================================================================
// DOM BASELINE STATE (Live Warmup)
// ============================================================================
// DOM metrics use live warmup (15-min from RTH open) since historical
// DOM data is unreliable/missing.
// ============================================================================

enum class DOMBaselineState : int {
    WARMUP_PENDING = 0,   // First 15 minutes after RTH open
    READY = 1             // Frozen after warmup completes
};

inline const char* DOMBaselineStateToString(DOMBaselineState s) {
    switch (s) {
        case DOMBaselineState::WARMUP_PENDING: return "WARMUP_PENDING";
        case DOMBaselineState::READY:          return "READY";
        default:                               return "UNKNOWN";
    }
}

// ============================================================================
// SESSION DELTA BASELINE STATE (Session-Aggregate Metrics)
// ============================================================================
// Session-level metrics (like sessionDeltaRatio) are baselined separately
// from bar-level metrics. They require prior session aggregates.
// ============================================================================

enum class SessionBaselineState : int {
    READY = 0,                    // Sufficient prior sessions
    INSUFFICIENT_SESSIONS = 1,    // < required prior sessions
    NOT_APPLICABLE = 2            // Outside relevant session context
};

inline const char* SessionBaselineStateToString(SessionBaselineState s) {
    switch (s) {
        case SessionBaselineState::READY:                 return "READY";
        case SessionBaselineState::INSUFFICIENT_SESSIONS: return "INSUFFICIENT_SESSIONS";
        case SessionBaselineState::NOT_APPLICABLE:        return "NOT_APPLICABLE";
        default:                                          return "UNKNOWN";
    }
}

// ============================================================================
// RESULT TYPES (Try* API Support - No Fallback Contract)
// ============================================================================
// These types enable explicit validity checking without numeric fallbacks.
// When valid=false, the value field is UNDEFINED and must not be used.
// ============================================================================

struct PercentileResult {
    double value = 0.0;
    bool valid = false;

    // Explicit construction
    static PercentileResult Valid(double v) { return {v, true}; }
    static PercentileResult Invalid() { return {0.0, false}; }
};

struct MeanResult {
    double value = 0.0;
    bool valid = false;

    static MeanResult Valid(double v) { return {v, true}; }
    static MeanResult Invalid() { return {0.0, false}; }
};

struct RatioResult {
    double value = 0.0;
    bool valid = false;

    static RatioResult Valid(double v) { return {v, true}; }
    static RatioResult Invalid() { return {0.0, false}; }
};

// Helper for ratio computation with NO_EVIDENCE on denominator=0
inline RatioResult ComputeRatio(double numerator, double denominator) {
    if (denominator <= 0.0)
        return RatioResult::Invalid();  // NO_EVIDENCE, not "neutral 0.5"
    return RatioResult::Valid(numerator / denominator);
}

// Helper for bid percentage with NO_EVIDENCE on zero volume
inline RatioResult ComputeBidPctOfTotal(double bidVol, double askVol) {
    const double total = bidVol + askVol;
    if (total <= 0.0)
        return RatioResult::Invalid();  // NO_EVIDENCE
    return RatioResult::Valid(100.0 * bidVol / total);
}

// Compute session key from time components
// ============================================================================
// SSOT: Is time-of-day within RTH window?
// This is the SINGLE authoritative function for RTH boundary detection.
// All session type determinations should use this function.
// ============================================================================
inline bool IsTimeInRTH(int timeOfDaySec, int rthStartSec, int rthEndSec)
{
    return (timeOfDaySec >= rthStartSec && timeOfDaySec < rthEndSec);
}

// Session continuity rules:
//   - RTH session: trading day = calendar date
//   - GLOBEX session: trading day = the RTH that PRECEDES this GLOBEX period
//     - Evening GLOBEX (after RTH close): tradingDay = today (RTH just ended)
//     - Morning GLOBEX (before RTH open): tradingDay = PREVIOUS day (RTH coming later)
//
// This means GLOBEX from Monday 16:15 to Tuesday 09:29 is ONE session (20241223-GBX)
// and Tuesday's RTH starts a new session (20241224-RTH).
inline SessionKey ComputeSessionKey(
    int dateYYYYMMDD,    // Calendar date of the bar
    int timeOfDaySec,    // Seconds since midnight (0-86399)
    int rthStartSec,     // RTH start in seconds (e.g., 34200 for 09:30)
    int rthEndSec        // RTH end in seconds (e.g., 58500 for 16:15)
)
{
    SessionKey key;

    // Determine if current time is within RTH (uses SSOT function)
    const bool isRTH = IsTimeInRTH(timeOfDaySec, rthStartSec, rthEndSec);
    key.sessionType = isRTH ? SessionType::RTH : SessionType::GLOBEX;

    if (isRTH) {
        // RTH session: trading day = calendar date
        key.tradingDay = dateYYYYMMDD;
    } else {
        // GLOBEX session: trading day = the RTH that PRECEDES this GLOBEX period
        if (timeOfDaySec >= rthEndSec) {
            // Evening GLOBEX (after RTH close): belongs to today's RTH that just ended
            key.tradingDay = dateYYYYMMDD;
        } else {
            // Morning GLOBEX (before RTH open): belongs to YESTERDAY's RTH
            // We need to decrement the date by 1 day
            // Simple approach: subtract 1 from day, handle month/year rollover
            key.tradingDay = DecrementDate(dateYYYYMMDD);
        }
    }

    return key;
}

// Helper: Check if SessionPhase is an RTH phase
inline bool IsRTHSession(SessionPhase phase) {
    return phase == SessionPhase::INITIAL_BALANCE ||
           phase == SessionPhase::MID_SESSION ||
           phase == SessionPhase::CLOSING_SESSION;
}

// Helper: Check if SessionPhase is a GLOBEX phase
inline bool IsGlobexSession(SessionPhase phase) {
    return phase == SessionPhase::GLOBEX ||
           phase == SessionPhase::LONDON_OPEN ||
           phase == SessionPhase::PRE_MARKET ||
           phase == SessionPhase::POST_CLOSE ||
           phase == SessionPhase::MAINTENANCE;
}

// Convert SessionPhase to SessionType
inline SessionType PhaseToSessionType(SessionPhase phase) {
    return IsRTHSession(phase) ? SessionType::RTH : SessionType::GLOBEX;
}

// ============================================================================
// BAR_REGIME / MARKET_STATE: REMOVED
// ============================================================================
// These enums have been removed. Use AMTMarketState as the SSOT for market
// regime (Balance/Imbalance). AMTMarketState is derived from Dalton's 1TF/2TF
// detection mechanism via DaltonEngine.
//
// Migration complete: All consumers now use AMTMarketState directly.
// ============================================================================

// ============================================================================
// AUCTION_REGIME: DEPRECATED - Use AMTMarketState + CurrentPhase instead
// ============================================================================
// DEPRECATION: Per AMT research, the fundamental model is TWO states:
//   - BALANCE (2TF, rotation)
//   - IMBALANCE (1TF, trending)
// ============================================================================
// DEPRECATED (Dec 2024): AuctionRegime four-phase cycle
//
// This enum has been superseded by AMTMarketState (SSOT from Dalton).
// The four-phase cycle (BALANCE→IMBALANCE→EXCESS→REBALANCE) has been removed
// from AMT_Phase.h. Market state now comes from Dalton's 1TF/2TF analysis.
//
// Migration mapping:
//   - EXCESS → CurrentPhase::FAILED_AUCTION
//   - REBALANCE → CurrentPhase::PULLBACK within IMBALANCE state
//   - BALANCE/IMBALANCE → AMTMarketState from Dalton
//
// This enum is retained for:
//   - Legacy log parsing tools (extract_session_stats.py)
//   - Backward compatibility during migration
//
// DO NOT USE in new code. Use AMTMarketState + CurrentPhase instead.
// ============================================================================
enum class AuctionRegime : int {  // DEPRECATED - Use AMTMarketState instead
    UNKNOWN = 0,    // Cannot determine (insufficient data)
    BALANCE = 1,    // Equilibrium within value (VAL <= price <= VAH)
    IMBALANCE = 2,  // Disequilibrium, trending, accepted outside value
    EXCESS = 3,     // DEPRECATED: Use CurrentPhase::FAILED_AUCTION instead
    REBALANCE = 4   // DEPRECATED: Model as early BALANCE (forming new value)
};

// Deprecated - kept for log parsing compatibility
inline const char* AuctionRegimeToString(AuctionRegime r) {
    switch (r) {
        case AuctionRegime::UNKNOWN:   return "UNKNOWN";
        case AuctionRegime::BALANCE:   return "BALANCE";
        case AuctionRegime::IMBALANCE: return "IMBALANCE";
        case AuctionRegime::EXCESS:    return "EXCESS";
        case AuctionRegime::REBALANCE: return "REBALANCE";
        default:                       return "INVALID";
    }
}

enum class AggressionType : int {
    NEUTRAL = 0,
    INITIATIVE = 1,
    RESPONSIVE = 2
};

enum class ControlSide : int {
    NEUTRAL = 0,
    BUYER = 1,
    SELLER = 2
};

enum class AuctionFacilitation : int {
    UNKNOWN = 0,      // Stage 2.1: Baseline not ready, facilitation cannot be determined
    EFFICIENT = 1,
    INEFFICIENT = 2,
    LABORED = 3,
    FAILED = 4
};

enum class CurrentPhase : int {
    UNKNOWN = 0,          // VA inputs invalid or warmup

    // BALANCE phases (2TF - fade extremes)
    ROTATION = 1,         // Inside VA, two-sided trade
    TESTING_BOUNDARY = 2, // At VA edge, probing

    // IMBALANCE phases (1TF - follow direction)
    DRIVING_UP = 3,       // 1TF bullish, buyers in control
    DRIVING_DOWN = 4,     // 1TF bearish, sellers in control

    // Special events (override default behavior)
    RANGE_EXTENSION = 5,  // IB break with initiative
    PULLBACK = 6,         // Counter-move in trend
    FAILED_AUCTION = 7,   // Rejection at extreme
    ACCEPTING_VALUE = 8   // Consolidating in new value area
};

// Dalton's actionable output: "Fade the extremes, go with breakouts"
enum class TradingBias : int {
    WAIT = 0,       // Unclear, don't trade
    FADE = 1,       // Fade the move (buy dips, sell rallies)
    FOLLOW = 2      // Go with the move (follow breakouts)
};

// Volume confirmation (Dalton's key diagnostic)
enum class VolumeConfirmation : int {
    UNKNOWN = 0,
    WEAK = 1,       // Low volume - rejection likely
    NEUTRAL = 2,    // Average - inconclusive
    STRONG = 3      // High volume - acceptance likely
};

// ============================================================================
// DALTON ACCEPTANCE (Time-Price Validation)
// ============================================================================
// "Price acts as advertisement; Time acts as acceptance; Volume validates value"
// A move is just a probe until time validates it.
// Rule of thumb: "One hour of trading at a new level constitutes initial acceptance"
// NOTE: Named DaltonAcceptance to avoid collision with ExtremeBehaviorState struct in AMT_Phase.h
enum class DaltonAcceptance : int {
    PROBING = 0,              // Just arrived at level, no validation yet
    INITIAL_ACCEPTANCE = 1,   // 1+ hour at level, profile widening (TPOs stacking)
    CONFIRMED_ACCEPTANCE = 2, // Strong TPO stacking + volume confirms value
    REJECTION = 3             // Failed to hold, returned to origin
};

inline const char* DaltonAcceptanceToString(DaltonAcceptance s) {
    switch (s) {
        case DaltonAcceptance::PROBING:              return "PROBING";
        case DaltonAcceptance::INITIAL_ACCEPTANCE:   return "INITIAL";
        case DaltonAcceptance::CONFIRMED_ACCEPTANCE: return "CONFIRMED";
        case DaltonAcceptance::REJECTION:            return "REJECTION";
        default:                                     return "?";
    }
}

// ============================================================================
// VALUE MIGRATION (Multi-Day VA Relationship)
// ============================================================================
// Tracks movement of Value Area relative to prior day.
// Critical for distinguishing trend days from balance days.
enum class ValueMigration : int {
    UNKNOWN = 0,
    OVERLAPPING = 1,  // Balance/Consolidation - reversion strategies dominate
    HIGHER = 2,       // Uptrend developing - buy pullbacks to prior VAH
    LOWER = 3,        // Downtrend developing - sell rallies to prior VAL
    INSIDE = 4        // Contraction - volatility expansion imminent
};

inline const char* ValueMigrationToString(ValueMigration m) {
    switch (m) {
        case ValueMigration::UNKNOWN:     return "UNKNOWN";
        case ValueMigration::OVERLAPPING: return "OVERLAP";
        case ValueMigration::HIGHER:      return "HIGHER";
        case ValueMigration::LOWER:       return "LOWER";
        case ValueMigration::INSIDE:      return "INSIDE";
        default:                          return "?";
    }
}

// Compute value migration from current and prior VA
inline ValueMigration ComputeValueMigration(
    double curVAH, double curVAL,
    double priorVAH, double priorVAL)
{
    if (priorVAH <= 0.0 || priorVAL <= 0.0) return ValueMigration::UNKNOWN;
    if (curVAH <= 0.0 || curVAL <= 0.0) return ValueMigration::UNKNOWN;
    if (curVAH <= curVAL || priorVAH <= priorVAL) return ValueMigration::UNKNOWN;

    // Inside: current VA entirely contained within prior VA
    if (curVAH <= priorVAH && curVAL >= priorVAL) {
        return ValueMigration::INSIDE;
    }

    // Higher: current VA entirely above prior VA (curVAL >= priorVAH)
    if (curVAL >= priorVAH) {
        return ValueMigration::HIGHER;
    }

    // Lower: current VA entirely below prior VA (curVAH <= priorVAL)
    if (curVAH <= priorVAL) {
        return ValueMigration::LOWER;
    }

    // Default: overlapping (some overlap but not contained)
    return ValueMigration::OVERLAPPING;
}

// ============================================================================
// SPIKE CONTEXT (Late-Day Imbalance - Unvalidated Moves)
// ============================================================================
// A spike is a price probe in final ~30 minutes that hasn't been validated by time.
// Next-day opening relative to spike determines if it was real or a trap.
enum class SpikeOpenRelation : int {
    NONE = 0,           // No spike detected
    ABOVE_SPIKE = 1,    // Bullish acceptance - Gap & Go
    WITHIN_SPIKE = 2,   // Partial acceptance - expect consolidation
    BELOW_SPIKE = 3     // Rejection - spike was trap, trade back to origin
};

inline const char* SpikeOpenRelationToString(SpikeOpenRelation r) {
    switch (r) {
        case SpikeOpenRelation::NONE:         return "NONE";
        case SpikeOpenRelation::ABOVE_SPIKE:  return "ABOVE";
        case SpikeOpenRelation::WITHIN_SPIKE: return "WITHIN";
        case SpikeOpenRelation::BELOW_SPIKE:  return "BELOW";
        default:                              return "?";
    }
}

// ============================================================================
// LEVEL ACCEPTANCE FRAMEWORK (Unified Acceptance/Rejection for All Levels)
// ============================================================================
// Every significant price level is a hypothesis that price tests.
// When price finds responsive activity → REJECTION
// When price finds no resistance → ACCEPTANCE (and continues)
//
// This is THE CORE of Auction Market Theory:
// - HVN should attract (acceptance expected, rejection = momentum)
// - LVN should repel (rejection expected, acceptance = TREND SIGNAL)
// - VAH/VAL are boundaries (either outcome is significant)
// ============================================================================

enum class LevelType : int {
    UNKNOWN = 0,

    // Volume Nodes (from profile)
    HVN = 1,                    // High Volume Node - fair value, magnet
    LVN = 2,                    // Low Volume Node - unfair value, repels

    // Value Area Boundaries
    POC = 10,                   // Point of Control - ultimate fair value
    VAH = 11,                   // Value Area High - upper boundary
    VAL = 12,                   // Value Area Low - lower boundary

    // Session Extremes
    SESSION_HIGH = 20,          // Current session high
    SESSION_LOW = 21,           // Current session low

    // Prior Session Reference
    PRIOR_POC = 30,             // Prior session POC
    PRIOR_VAH = 31,             // Prior session VAH
    PRIOR_VAL = 32,             // Prior session VAL
    PRIOR_HIGH = 33,            // Prior session high
    PRIOR_LOW = 34,             // Prior session low

    // Initial Balance
    IB_HIGH = 40,               // Initial Balance high
    IB_LOW = 41,                // Initial Balance low

    // Developing (intraday)
    DEVELOPING_POC = 50,        // Current developing POC
    DEVELOPING_VAH = 51,        // Current developing VAH
    DEVELOPING_VAL = 52         // Current developing VAL
};

inline const char* LevelTypeToString(LevelType t) {
    switch (t) {
        case LevelType::UNKNOWN:        return "UNKNOWN";
        case LevelType::HVN:            return "HVN";
        case LevelType::LVN:            return "LVN";
        case LevelType::POC:            return "POC";
        case LevelType::VAH:            return "VAH";
        case LevelType::VAL:            return "VAL";
        case LevelType::SESSION_HIGH:   return "SESS_HI";
        case LevelType::SESSION_LOW:    return "SESS_LO";
        case LevelType::PRIOR_POC:      return "PRIOR_POC";
        case LevelType::PRIOR_VAH:      return "PRIOR_VAH";
        case LevelType::PRIOR_VAL:      return "PRIOR_VAL";
        case LevelType::PRIOR_HIGH:     return "PRIOR_HI";
        case LevelType::PRIOR_LOW:      return "PRIOR_LO";
        case LevelType::IB_HIGH:        return "IB_HI";
        case LevelType::IB_LOW:         return "IB_LO";
        case LevelType::DEVELOPING_POC: return "DEV_POC";
        case LevelType::DEVELOPING_VAH: return "DEV_VAH";
        case LevelType::DEVELOPING_VAL: return "DEV_VAL";
        default:                        return "?";
    }
}

enum class LevelTestOutcome : int {
    UNTESTED = 0,           // Price hasn't reached level yet
    TESTING = 1,            // Currently at level, outcome pending
    ACCEPTED = 2,           // Held at level, building value (time + volume)
    REJECTED = 3,           // Failed to hold, returned to origin
    BROKEN_THROUGH = 4      // Blew through with conviction (different from accepted)
};

inline const char* LevelTestOutcomeToString(LevelTestOutcome o) {
    switch (o) {
        case LevelTestOutcome::UNTESTED:       return "UNTESTED";
        case LevelTestOutcome::TESTING:        return "TESTING";
        case LevelTestOutcome::ACCEPTED:       return "ACCEPTED";
        case LevelTestOutcome::REJECTED:       return "REJECTED";
        case LevelTestOutcome::BROKEN_THROUGH: return "BROKEN";
        default:                               return "?";
    }
}

/**
 * Determine if outcome matches expected behavior for level type.
 * Expected outcomes are "normal" - unexpected outcomes are actionable signals.
 */
inline bool IsExpectedOutcome(LevelType type, LevelTestOutcome outcome) {
    switch (type) {
        case LevelType::HVN:
            // HVN SHOULD attract and hold (acceptance expected)
            return outcome == LevelTestOutcome::ACCEPTED ||
                   outcome == LevelTestOutcome::TESTING;

        case LevelType::LVN:
            // LVN SHOULD repel (rejection or break-through expected)
            return outcome == LevelTestOutcome::REJECTED ||
                   outcome == LevelTestOutcome::BROKEN_THROUGH;

        case LevelType::VAH:
        case LevelType::VAL:
        case LevelType::PRIOR_VAH:
        case LevelType::PRIOR_VAL:
            // Boundaries can go either way - both are "expected"
            return true;

        case LevelType::POC:
        case LevelType::PRIOR_POC:
        case LevelType::DEVELOPING_POC:
            // POC should act as magnet (acceptance expected)
            return outcome == LevelTestOutcome::ACCEPTED ||
                   outcome == LevelTestOutcome::TESTING;

        case LevelType::SESSION_HIGH:
        case LevelType::SESSION_LOW:
        case LevelType::PRIOR_HIGH:
        case LevelType::PRIOR_LOW:
            // Extremes - rejection is more common
            return outcome == LevelTestOutcome::REJECTED;

        case LevelType::IB_HIGH:
        case LevelType::IB_LOW:
            // IB boundaries - rejection is "normal day"
            return outcome == LevelTestOutcome::REJECTED;

        default:
            return true;
    }
}

/**
 * Determine if outcome is an actionable trading signal.
 * Unexpected outcomes at key levels are the signals!
 */
inline bool IsActionableSignal(LevelType type, LevelTestOutcome outcome) {
    if (outcome == LevelTestOutcome::UNTESTED ||
        outcome == LevelTestOutcome::TESTING) {
        return false;  // No resolution yet
    }

    // Unexpected outcomes are always actionable
    if (!IsExpectedOutcome(type, outcome)) {
        return true;
    }

    // VA boundaries are always actionable (determines direction)
    if (type == LevelType::VAH || type == LevelType::VAL ||
        type == LevelType::PRIOR_VAH || type == LevelType::PRIOR_VAL) {
        return true;
    }

    // IB breaks are actionable (range extension signal)
    if ((type == LevelType::IB_HIGH || type == LevelType::IB_LOW) &&
        (outcome == LevelTestOutcome::ACCEPTED ||
         outcome == LevelTestOutcome::BROKEN_THROUGH)) {
        return true;
    }

    return false;
}

// PhaseReason: Explains WHY we're in current phase (AMT concepts)
enum class PhaseReason : int {
    NONE = 0,

    // Timeframe Pattern (1TF/2TF) - detection mechanism for state
    ONE_TF_UP = 10,         // 1TF bullish (each low > prev low)
    ONE_TF_DOWN = 11,       // 1TF bearish (each high < prev high)
    TWO_TF = 12,            // 2TF overlapping (balanced)

    // Location-based
    AT_POC = 20,
    AT_VAH = 21,
    AT_VAL = 22,
    AT_HVN = 23,            // At High Volume Node
    AT_LVN = 24,            // At Low Volume Node
    INSIDE_VALUE = 25,
    OUTSIDE_VALUE = 26,

    // Auction Events
    POOR_HIGH = 30,         // Weak high (no excess)
    POOR_LOW = 31,          // Weak low (no excess)
    EXCESS_HIGH = 32,       // Strong rejection high
    EXCESS_LOW = 33,        // Strong rejection low
    SINGLE_PRINTS = 34,     // Thin volume detected

    // Activity
    RESPONSIVE = 40,
    INITIATIVE = 41,

    // IB Events
    IB_BREAK_UP = 50,
    IB_BREAK_DOWN = 51,
    FAILED_BREAKOUT = 52
};

enum class AuctionIntent : int {
    NEUTRAL = 0,
    ACCUMULATION = 1,
    DISTRIBUTION = 2,
    ABSORPTION = 3,      // Selling into rising price (bullish)
    EXHAUSTION = 4       // Buying into falling price (bearish)
};

enum class AuctionOutcome : int {
    PENDING = 0,
    ACCEPTED = 1,        // Time + Volume confirmed the level
    REJECTED = 2         // Quick reversal away from level
};

enum class TransitionMechanic : int {
    NONE = 0,
    BALANCE_TO_IMBALANCE = 1,
    IMBALANCE_TO_BALANCE = 2,
    FAILED_TRANSITION = 3
};

enum class VolatilityState : int {
    LOW = 1,
    NORMAL = 2,
    HIGH = 3,
    EXTREME = 4
};

// Liquidity state from 3-component model (DepthMass, Stress, Resilience)
// Uses historical depth data via c_ACSILDepthBars for temporal coherence
enum class LiquidityState : int {
    LIQ_NOT_READY = -1,    // Baseline insufficient - explicit error
    LIQ_VOID = 0,          // LIQ <= 0.10 or DepthRank <= 0.10
    LIQ_THIN = 1,          // 0.10 < LIQ <= 0.25 or StressRank >= 0.90
    LIQ_NORMAL = 2,        // 0.25 < LIQ < 0.75
    LIQ_THICK = 3          // LIQ >= 0.75
};

// Execution friction classification from spreadTicks baseline
// UNKNOWN when baseline not ready - no silent fallback
enum class ExecutionFriction : int {
    UNKNOWN = 0,     // Baseline not ready or spread data unavailable
    TIGHT = 1,       // <=25th percentile: low cost, confident execution
    NORMAL = 2,      // 25th-75th percentile: typical execution cost
    WIDE = 3,        // >=75th percentile: high cost, slippage risk
    LOCKED = 4       // Spread = 0: market locked, special handling
};

// ============================================================================
// PROFILE SHAPE (merged balance/imbalance)
// ============================================================================

enum class ProfileShape : int {
    UNDEFINED = 0,

    // Balance patterns
    NORMAL_DISTRIBUTION = 1,
    D_SHAPED = 2,           // Balanced, POC centered
    BALANCED = 3,

    // Imbalance patterns
    P_SHAPED = 4,           // POC high, fat top, thin bottom (short covering rally)
    B_SHAPED = 5,           // POC low, fat bottom, thin top (long liquidation)
    THIN_VERTICAL = 6,      // Trend day, no rotation
    DOUBLE_DISTRIBUTION = 7, // Bi-modal (morning + afternoon POCs)

    COUNT = 8               // For array sizing (must be last)
};

// ============================================================================
// SHAPE ERROR (classification failure reasons)
// ============================================================================

enum class ShapeError : int {
    NONE = 0,                   // No error - classification succeeded
    INVALID_VA = 1,             // VA levels invalid or inverted
    HISTOGRAM_EMPTY = 2,        // No histogram data
    INSUFFICIENT_DATA = 3,      // Not enough bins/volume
    THRESHOLDS_INVALID = 4,     // Volume thresholds not computed
    AMBIGUOUS_BIMODAL = 5,      // Possible bimodal but inconclusive
    INCONCLUSIVE_BALANCE = 6,   // Balance pattern but peakiness too low
    VA_TOO_NARROW = 7,          // VA width below minimum
    INSUFFICIENT_CLUSTERS = 8   // Not enough HVN clusters for bimodal
};

// ============================================================================
// ZONE FRAMEWORK ENUMS
// ============================================================================

/**
 * ZoneType: Structural identity (WHAT it is)
 */
enum class ZoneType : int {
    // Current session value area (VBP-derived)
    VPB_POC = 1,
    VPB_VAH = 2,
    VPB_VAL = 3,

    // Prior session references
    PRIOR_POC = 4,
    PRIOR_VAH = 5,
    PRIOR_VAL = 6,

    // TPO-derived zones
    TPO_POC = 7,
    TPO_VAH = 8,
    TPO_VAL = 9,

    // Intraday structure
    IB_HIGH = 10,
    IB_LOW = 11,
    SESSION_HIGH = 12,
    SESSION_LOW = 13,

    // Benchmark
    VWAP = 14,

    NONE = 0
};

/**
 * ZoneRole: Behavioral classification (HOW it behaves)
 * 
 * DOMINANCE HIERARCHY:
 *   Tier 3: VALUE_BOUNDARY (VAH, VAL) - Highest priority
 *   Tier 2: VALUE_CORE (POC) - Dominates range boundaries
 *   Tier 1: RANGE_BOUNDARY (IB, session extremes) - Weaker than POC
 *   Tier 0: MEAN_REFERENCE (VWAP) - Weakest structural level
 */
enum class ZoneRole : int {
    VALUE_BOUNDARY = 3,  // VAH, VAL (highest priority)
    VALUE_CORE = 2,      // POC (dominates range)
    RANGE_BOUNDARY = 1,  // IB edges, session extremes
    MEAN_REFERENCE = 0   // VWAP (weakest)
};

/**
 * AnchorMechanism: How the zone is derived
 */
enum class AnchorMechanism : int {
    VOLUME_PROFILE = 1,  // POC, VAH, VAL
    TIME_RANGE = 2,      // IB high/low, session high/low
    WEIGHTED_MEAN = 3,   // VWAP
    FIXED_LEVEL = 4      // Manual levels, prior closes
};

/**
 * ZoneSource: When/where it came from (provenance)
 * 
 * FRESHNESS HIERARCHY:
 *   Tier 4: CURRENT_RTH - Most relevant (live RTH)
 *   Tier 3: INTRADAY_CALC - Real-time (VWAP, more current than prior)
 *   Tier 2: PRIOR_RTH - Yesterday's structure
 *   Tier 1: CURRENT_GLOBEX - Overnight action (less relevant in RTH)
 *   Tier 0: PRIOR_GLOBEX - Stale overnight structure
 */
enum class ZoneSource : int {
    CURRENT_RTH = 4,
    INTRADAY_CALC = 3,
    PRIOR_RTH = 2,
    CURRENT_GLOBEX = 1,
    PRIOR_GLOBEX = 0
};

/**
 * ZoneProximity: Distance state (where price is relative to zone)
 *
 * State machine:
 *   INACTIVE <-> APPROACHING <-> AT_ZONE -> DEPARTED -> INACTIVE
 *
 * DEPARTED is a transient state: Price was previously AT_ZONE and has
 * exited the halo; used until resolution timer expires (bars or time).
 * While DEPARTED: probes should not fire, but resolution timer runs.
 * Upon resolution: transitions to INACTIVE (zone engagement complete).
 */
enum class ZoneProximity : int {
    INACTIVE = 0,    // Far away (> halo), no recent engagement
    APPROACHING = 1, // In halo distance, not yet at core
    AT_ZONE = 2,     // In core width (active engagement)
    DEPARTED = 3     // Was AT_ZONE, exited halo; awaiting resolution
};

/**
 * ValueAreaRegion: Position within value distribution
 */
enum class ValueAreaRegion : int {
    OUTSIDE_ABOVE = 1,  // Price > VAH (excess high)
    UPPER_VA = 2,       // POC < price ≤ VAH (upper 30%)
    CORE_VA = 3,        // Around POC (middle 40%, ±15% of range)
    LOWER_VA = 4,       // VAL ≤ price < POC (lower 30%)
    OUTSIDE_BELOW = 5   // Price < VAL (excess low)
};

/**
 * ZoneStrength: Strength tier based on touches and age
 */
enum class ZoneStrength : int {
    VIRGIN = 4,      // Never tested, maximum strength
    STRONG = 3,      // 1 touch, score > 1.2
    MODERATE = 2,    // 2-3 touches, score 0.8-1.2
    WEAK = 1,        // 4+ touches, score < 0.8
    EXPIRED = 0      // Too old or too many touches, ignore
};

/**
 * VolumeNodeType: Volume classification (HVN/LVN + delta context)
 */
enum class VolumeNodeType : int {
    HVN_RESPONSIVE = 1,  // High volume + opposite delta (defense at boundary)
    HVN_INITIATIVE = 2,  // High volume + aligned delta (attack at boundary)
    HVN_BALANCED = 3,    // High volume + neutral delta (acceptance/two-way)
    LVN_GAP = 4,         // Low volume, rejected price
    LVN_SINGLE_PRINT = 5, // Extreme low volume, one-sided move
    NORMAL = 6           // Average volume, no special characteristics
};

// ============================================================================
// ORTHOGONAL VOLUME CLASSIFICATION (SSOT - replaces mixed VolumeNodeType)
// ============================================================================

/**
 * VAPDensityClass: Pure density classification (SSOT)
 * Determined solely by volume vs threshold, no behavioral overlay
 */
enum class VAPDensityClass : int {
    HIGH = 1,      // Volume > hvnThreshold (mean + hvnSigmaCoeff * σ)
    NORMAL = 0,    // Between thresholds
    LOW = -1       // Volume < lvnThreshold (mean - lvnSigmaCoeff * σ)
};

/**
 * FlowIntent: Behavioral overlay based on delta and boundary context
 * Orthogonal to density - can apply to any density level
 */
enum class FlowIntent : int {
    INITIATIVE = 1,    // Delta aligned with boundary pressure (attack)
    RESPONSIVE = -1,   // Delta opposite to boundary pressure (defense)
    NEUTRAL = 0        // Mixed or unclear delta
};

/**
 * NodeFlags: Additional characteristics (bitfield, can combine)
 */
enum class NodeFlags : unsigned int {
    NONE = 0,
    SINGLE_PRINT = 1 << 0,      // Extreme low volume, one-sided move
    GAP = 1 << 1,               // Volume gap (quick rejection area)
    PLATEAU = 1 << 2,           // Equal-volume shelf (multiple adjacent HVN bars)
    CLUSTER_PEAK = 1 << 3,      // Peak of a multi-tick HVN cluster
    PRIOR_SESSION = 1 << 4,     // From prior session (preserved reference)
    ABSORPTION = 1 << 5         // High volume absorption detected
};

// Bitwise operators for NodeFlags
inline NodeFlags operator|(NodeFlags a, NodeFlags b) {
    return static_cast<NodeFlags>(static_cast<unsigned int>(a) | static_cast<unsigned int>(b));
}
inline NodeFlags operator&(NodeFlags a, NodeFlags b) {
    return static_cast<NodeFlags>(static_cast<unsigned int>(a) & static_cast<unsigned int>(b));
}
inline bool HasFlag(NodeFlags flags, NodeFlags test) {
    return (static_cast<unsigned int>(flags) & static_cast<unsigned int>(test)) != 0;
}

/**
 * VolumeThresholds: Cached SSOT for HVN/LVN classification
 * Computed once per refresh interval, used by all classification paths
 */
struct VolumeThresholds {
    // Computed statistics
    double mean = 0.0;
    double stddev = 0.0;
    double hvnThreshold = 0.0;    // mean + hvnSigmaCoeff * σ
    double lvnThreshold = 0.0;    // mean - lvnSigmaCoeff * σ

    // Source data info
    int sampleSize = 0;
    double totalVolume = 0.0;
    double maxLevelVolume = 0.0;  // Maximum volume across all price levels
    double volumeAtPOC = 0.0;     // Volume at the VbP study's POC price (may differ from maxLevelVolume)

    // POC volume verification
    // maxLevelVolume vs volumeAtPOC may differ if VbP uses smoothing, ties, or grouping rules
    bool pocVolumeVerified = false;  // True if volumeAtPOC == maxLevelVolume (within tolerance)

    // Validity
    bool valid = false;
    int computedAtBar = -1;

    // Classification using SSOT thresholds
    VAPDensityClass ClassifyVolume(double volume) const {
        if (!valid || sampleSize == 0) return VAPDensityClass::NORMAL;
        if (volume > hvnThreshold) return VAPDensityClass::HIGH;
        if (volume < lvnThreshold && volume > 0) return VAPDensityClass::LOW;
        return VAPDensityClass::NORMAL;
    }

    // Check if thresholds need refresh
    bool NeedsRefresh(int currentBar, int refreshInterval) const {
        if (!valid) return true;
        return (currentBar - computedAtBar) >= refreshInterval;
    }

    void Reset() {
        mean = 0.0;
        stddev = 0.0;
        hvnThreshold = 0.0;
        lvnThreshold = 0.0;
        sampleSize = 0;
        totalVolume = 0.0;
        maxLevelVolume = 0.0;
        volumeAtPOC = 0.0;
        pocVolumeVerified = false;
        valid = false;
        computedAtBar = -1;
    }
};

/**
 * VolumeNodeClassification: Composite classification (orthogonal outputs)
 * Replaces the mixed VolumeNodeType enum for new code paths
 */
struct VolumeNodeClassification {
    VAPDensityClass density = VAPDensityClass::NORMAL;
    FlowIntent intent = FlowIntent::NEUTRAL;
    NodeFlags flags = NodeFlags::NONE;

    // Convenience accessors
    bool IsHVN() const { return density == VAPDensityClass::HIGH; }
    bool IsLVN() const { return density == VAPDensityClass::LOW; }
    bool IsSinglePrint() const { return HasFlag(flags, NodeFlags::SINGLE_PRINT); }
    bool IsGap() const { return HasFlag(flags, NodeFlags::GAP); }
    bool IsPlateau() const { return HasFlag(flags, NodeFlags::PLATEAU); }
    bool IsPriorSession() const { return HasFlag(flags, NodeFlags::PRIOR_SESSION); }

    // Convert to legacy VolumeNodeType for backward compatibility
    VolumeNodeType ToLegacyType() const {
        if (density == VAPDensityClass::HIGH) {
            switch (intent) {
                case FlowIntent::RESPONSIVE: return VolumeNodeType::HVN_RESPONSIVE;
                case FlowIntent::INITIATIVE: return VolumeNodeType::HVN_INITIATIVE;
                default: return VolumeNodeType::HVN_BALANCED;
            }
        }
        if (density == VAPDensityClass::LOW) {
            if (HasFlag(flags, NodeFlags::SINGLE_PRINT)) {
                return VolumeNodeType::LVN_SINGLE_PRINT;
            }
            return VolumeNodeType::LVN_GAP;
        }
        return VolumeNodeType::NORMAL;
    }
};

/**
 * VolumeCluster: Contiguous node segment (replaces flat price lists)
 */
struct VolumeCluster {
    double lowPrice = 0.0;
    double highPrice = 0.0;
    double peakPrice = 0.0;       // Price with highest volume in cluster
    double peakVolume = 0.0;
    int widthTicks = 0;
    VAPDensityClass density = VAPDensityClass::NORMAL;
    NodeFlags flags = NodeFlags::NONE;

    // Check if price is within cluster
    bool Contains(double price, double tickSize) const {
        return price >= (lowPrice - tickSize * 0.5) &&
               price <= (highPrice + tickSize * 0.5);
    }

    // Get center price
    double GetCenter() const {
        return (lowPrice + highPrice) / 2.0;
    }
};

/**
 * PriorSessionNode: Preserved reference from prior session
 */
struct PriorSessionNode {
    double price = 0.0;
    VAPDensityClass density = VAPDensityClass::NORMAL;
    double strengthAtClose = 0.0;  // How strong was this level at session end
    int touchCount = 0;
    int sessionAge = 0;            // How many sessions ago (1 = yesterday)
    SessionPhase sessionType = SessionPhase::UNKNOWN;  // RTH, GLOBEX, etc.

    // Decay for relevance (bars since session close)
    double GetRelevance(int barsSinceSessionClose) const {
        return strengthAtClose * std::exp(-static_cast<double>(barsSinceSessionClose) / 500.0);
    }
};

// Helper string conversions for new types
// PERFORMANCE: All return const char* (zero allocation) - string literals only
inline const char* VAPDensityToString(VAPDensityClass density) {
    switch (density) {
        case VAPDensityClass::HIGH: return "HVN";
        case VAPDensityClass::LOW: return "LVN";
        default: return "NORMAL";
    }
}

inline const char* FlowIntentToString(FlowIntent intent) {
    switch (intent) {
        case FlowIntent::INITIATIVE: return "INITIATIVE";
        case FlowIntent::RESPONSIVE: return "RESPONSIVE";
        default: return "NEUTRAL";
    }
}

/**
 * TouchType: Classification of zone engagement
 */
enum class TouchType : int {
    TAG = 1,        // Brief contact, no penetration, minimal wear
    PROBE = 2,      // Penetrated beyond core, quick rejection, light wear
    TEST = 3,       // Sustained engagement but ultimately rejected, moderate wear
    ACCEPTANCE = 4, // Met acceptance criteria and held, heavy wear
    UNRESOLVED = 5  // Engagement never completed (session roll, expiry, timeout)
};

/**
 * UnresolvedReason: Why an engagement was force-finalized
 */
enum class UnresolvedReason : int {
    NONE = 0,           // Not unresolved (normal finalization)
    SESSION_ROLL = 1,   // Session boundary crossed (RTH<->Globex)
    ZONE_EXPIRY = 2,    // Zone expired/cleaned up while engaged
    CHART_RESET = 3,    // Chart or study reset
    TIMEOUT = 4         // Engagement exceeded max duration
};

// ============================================================================
// HELPER FUNCTIONS (to string conversions)
// ============================================================================

// PERFORMANCE: Returns const char* (zero allocation) - string literals only
inline const char* ZoneTypeToString(ZoneType type) {
    switch (type) {
        // VBP zones (current session) - use full prefix to disambiguate from PRIOR
        case ZoneType::VPB_POC: return "VPB_POC";
        case ZoneType::VPB_VAH: return "VPB_VAH";
        case ZoneType::VPB_VAL: return "VPB_VAL";
        // PRIOR zones (prior session)
        case ZoneType::PRIOR_POC: return "PRIOR_POC";
        case ZoneType::PRIOR_VAH: return "PRIOR_VAH";
        case ZoneType::PRIOR_VAL: return "PRIOR_VAL";
        // TPO zones (disabled by posture)
        case ZoneType::TPO_POC: return "TPO_POC";
        case ZoneType::TPO_VAH: return "TPO_VAH";
        case ZoneType::TPO_VAL: return "TPO_VAL";
        // Structure zones
        case ZoneType::IB_HIGH: return "IB_HIGH";
        case ZoneType::IB_LOW: return "IB_LOW";
        case ZoneType::SESSION_HIGH: return "SESSION_HIGH";
        case ZoneType::SESSION_LOW: return "SESSION_LOW";
        // VWAP
        case ZoneType::VWAP: return "VWAP";
        default: return "NONE";
    }
}

inline const char* ZoneRoleToString(ZoneRole role) {
    switch (role) {
        case ZoneRole::VALUE_BOUNDARY: return "VALUE_BOUNDARY";
        case ZoneRole::VALUE_CORE: return "VALUE_CORE";
        case ZoneRole::RANGE_BOUNDARY: return "RANGE_BOUNDARY";
        case ZoneRole::MEAN_REFERENCE: return "MEAN_REFERENCE";
        default: return "UNKNOWN";
    }
}

// PERFORMANCE: Returns const char* (zero allocation) - string literals only
inline const char* ZoneProximityToString(ZoneProximity prox) {
    switch (prox) {
        case ZoneProximity::INACTIVE: return "INACTIVE";
        case ZoneProximity::APPROACHING: return "APPROACHING";
        case ZoneProximity::AT_ZONE: return "AT_ZONE";
        case ZoneProximity::DEPARTED: return "DEPARTED";
        // No default: compiler warns on missing enum values
    }
    // Unreachable if all enum values handled; assert in debug
    assert(false && "ZoneProximityToString: unhandled ZoneProximity value");
    return "INVALID";
}

// PERFORMANCE: Returns const char* (zero allocation) - all values are string literals
inline const char* CurrentPhaseToString(CurrentPhase phase) {
    switch (phase) {
        case CurrentPhase::UNKNOWN: return "UNKNOWN";
        case CurrentPhase::ROTATION: return "ROTATION";
        case CurrentPhase::TESTING_BOUNDARY: return "TEST_BND";
        case CurrentPhase::DRIVING_UP: return "DRIVING_UP";
        case CurrentPhase::DRIVING_DOWN: return "DRIVING_DN";
        case CurrentPhase::RANGE_EXTENSION: return "RANGE_EXT";
        case CurrentPhase::PULLBACK: return "PULLBACK";
        case CurrentPhase::FAILED_AUCTION: return "FAILED_AUC";
        case CurrentPhase::ACCEPTING_VALUE: return "ACCEPTING";
        default: return "INVALID";
    }
}

inline const char* TradingBiasToString(TradingBias bias) {
    switch (bias) {
        case TradingBias::WAIT: return "WAIT";
        case TradingBias::FADE: return "FADE";
        case TradingBias::FOLLOW: return "FOLLOW";
        default: return "?";
    }
}

inline const char* VolumeConfirmationToString(VolumeConfirmation vc) {
    switch (vc) {
        case VolumeConfirmation::UNKNOWN: return "?";
        case VolumeConfirmation::WEAK: return "WEAK";
        case VolumeConfirmation::NEUTRAL: return "NEUT";
        case VolumeConfirmation::STRONG: return "STRONG";
        default: return "?";
    }
}

inline const char* PhaseReasonToString(PhaseReason r) {
    switch (r) {
        case PhaseReason::NONE: return "";
        case PhaseReason::ONE_TF_UP: return "1TF_UP";
        case PhaseReason::ONE_TF_DOWN: return "1TF_DN";
        case PhaseReason::TWO_TF: return "2TF";
        case PhaseReason::AT_POC: return "AT_POC";
        case PhaseReason::AT_VAH: return "AT_VAH";
        case PhaseReason::AT_VAL: return "AT_VAL";
        case PhaseReason::AT_HVN: return "AT_HVN";
        case PhaseReason::AT_LVN: return "AT_LVN";
        case PhaseReason::INSIDE_VALUE: return "IN_VA";
        case PhaseReason::OUTSIDE_VALUE: return "OUT_VA";
        case PhaseReason::POOR_HIGH: return "POOR_HI";
        case PhaseReason::POOR_LOW: return "POOR_LO";
        case PhaseReason::EXCESS_HIGH: return "EXCESS_HI";
        case PhaseReason::EXCESS_LOW: return "EXCESS_LO";
        case PhaseReason::SINGLE_PRINTS: return "SINGLE_PRINTS";
        case PhaseReason::RESPONSIVE: return "RESPONSIVE";
        case PhaseReason::INITIATIVE: return "INITIATIVE";
        case PhaseReason::IB_BREAK_UP: return "IB_UP";
        case PhaseReason::IB_BREAK_DOWN: return "IB_DN";
        case PhaseReason::FAILED_BREAKOUT: return "FAILED_BO";
        default: return "?";
    }
}

// PERFORMANCE: Returns const char* (zero allocation) - all values are string literals
inline const char* AuctionOutcomeToString(AuctionOutcome outcome) {
    switch (outcome) {
        case AuctionOutcome::PENDING: return "PENDING";
        case AuctionOutcome::ACCEPTED: return "ACCEPTED";
        case AuctionOutcome::REJECTED: return "REJECTED";
        default: return "UNKNOWN";
    }
}

inline const char* TouchTypeToString(TouchType type) {
    switch (type) {
        case TouchType::TAG: return "TAG";
        case TouchType::PROBE: return "PROBE";
        case TouchType::TEST: return "TEST";
        case TouchType::ACCEPTANCE: return "ACCEPTANCE";
        case TouchType::UNRESOLVED: return "UNRESOLVED";
        default: return "UNKNOWN";
    }
}

inline const char* UnresolvedReasonToString(UnresolvedReason reason) {
    switch (reason) {
        case UnresolvedReason::NONE: return "NONE";
        case UnresolvedReason::SESSION_ROLL: return "SESSION_ROLL";
        case UnresolvedReason::ZONE_EXPIRY: return "ZONE_EXPIRY";
        case UnresolvedReason::CHART_RESET: return "CHART_RESET";
        case UnresolvedReason::TIMEOUT: return "TIMEOUT";
        default: return "UNKNOWN";
    }
}

inline const char* ProfileShapeToString(ProfileShape shape) {
    switch (shape) {
        case ProfileShape::UNDEFINED: return "UNDEFINED";
        case ProfileShape::NORMAL_DISTRIBUTION: return "NORMAL";
        case ProfileShape::D_SHAPED: return "D_SHAPED";
        case ProfileShape::BALANCED: return "BALANCED";
        case ProfileShape::P_SHAPED: return "P_SHAPED";
        case ProfileShape::B_SHAPED: return "B_SHAPED";
        case ProfileShape::THIN_VERTICAL: return "TREND_DAY";
        case ProfileShape::DOUBLE_DISTRIBUTION: return "DOUBLE_DIST";
        // No default: compiler warns on missing enum values
    }
    return "UNDEFINED";
}

inline const char* SessionPhaseToString(SessionPhase phase) {
    switch (phase) {
        case SessionPhase::UNKNOWN:         return "UNKNOWN";
        case SessionPhase::GLOBEX:          return "GLOBEX";
        case SessionPhase::LONDON_OPEN:     return "LONDON";
        case SessionPhase::PRE_MARKET:      return "PRE_MKT";
        case SessionPhase::INITIAL_BALANCE: return "IB";
        case SessionPhase::MID_SESSION:     return "MID_SESS";
        case SessionPhase::CLOSING_SESSION: return "CLOSING";
        case SessionPhase::POST_CLOSE:      return "POST_CLOSE";
        case SessionPhase::MAINTENANCE:     return "MAINT";
        // No default: compiler warns on missing enum values
    }
    return "UNKNOWN";
}

// NOTE: IsRTHSession() and IsGlobexSession() are defined earlier in this file
// (near SessionKey) to avoid forward declaration issues.

// ============================================================================
// AMT SIGNAL TYPES (Value-Relative Activity Classification)
// ============================================================================
// These types implement true Auction Market Theory classification:
//   - Value-relative intent (toward/away from POC)
//   - Participation mode (aggressive/absorptive from delta vs price)
//   - Activity classification (initiative/responsive)
//   - Location-gated inference (location is primary gate, not weighted input)
// ============================================================================

/**
 * AMTMarketState: Fundamental two-state AMT model (SSOT)
 *
 * This is the SINGLE SOURCE OF TRUTH for market regime.
 * All other regime classifications (BarRegime, AuctionRegime) are DEPRECATED.
 *
 * Per Dalton's AMT framework:
 *   - Detection mechanism: 1TF/2TF (One-Time Framing / Two-Time Framing)
 *   - 1TF = IMBALANCE (one side in control, trending)
 *   - 2TF = BALANCE (both sides active, rotation)
 *
 * Data flow:
 *   DaltonEngine.ProcessBar() → DaltonState.phase → StateEvidence.currentState
 *
 * BALANCE:   Horizontal development, two-sided trade, 2TF, ~80% of time
 * IMBALANCE: Vertical development, one-sided conviction, 1TF, ~20% of time
 *
 * For phase derivation (ROTATION, PULLBACK, etc.), use CurrentPhase
 * which is derived from AMTMarketState + location + activity.
 */
enum class AMTMarketState : int {
    UNKNOWN = 0,    // Insufficient data to classify (warmup)
    BALANCE = 1,    // Equilibrium, 2TF, rotating within value area
    IMBALANCE = 2   // Disequilibrium, 1TF, trending/discovering new price levels
};

inline const char* AMTMarketStateToString(AMTMarketState state) {
    switch (state) {
        case AMTMarketState::UNKNOWN:   return "UNKNOWN";
        case AMTMarketState::BALANCE:   return "BALANCE";
        case AMTMarketState::IMBALANCE: return "IMBALANCE";
        default:                        return "INVALID";
    }
}

/**
 * ValueIntent: Direction relative to value center (POC)
 * This is NOT price direction - it's semantic direction relative to accepted value.
 *
 * TOWARD_VALUE: Price moving toward POC (returning to equilibrium)
 * AWAY_FROM_VALUE: Price moving away from POC (seeking new price levels)
 * AT_VALUE: Price at POC (within tolerance, no directional intent)
 */
enum class ValueIntent : int {
    AT_VALUE = 0,       // At POC (within tolerance), no directional intent
    TOWARD_VALUE = 1,   // Moving toward POC (regardless of up/down)
    AWAY_FROM_VALUE = 2 // Moving away from POC (regardless of up/down)
};

inline const char* ValueIntentToString(ValueIntent intent) {
    switch (intent) {
        case ValueIntent::AT_VALUE:       return "AT_VALUE";
        case ValueIntent::TOWARD_VALUE:   return "TOWARD_VALUE";
        case ValueIntent::AWAY_FROM_VALUE: return "AWAY_FROM_VALUE";
        default:                          return "INVALID";
    }
}

/**
 * ParticipationMode: WHO is in control (from delta vs price direction)
 *
 * Delta aligned with price = AGGRESSIVE (initiators driving price)
 * Delta opposite to price = ABSORPTIVE (responsive participants absorbing)
 * Neutral delta = BALANCED (two-sided, no clear control)
 *
 * Examples:
 *   Price up + positive delta = AGGRESSIVE buyers attacking
 *   Price up + negative delta = ABSORPTIVE sellers defending (absorption)
 *   Price down + negative delta = AGGRESSIVE sellers attacking
 *   Price down + positive delta = ABSORPTIVE buyers defending (absorption)
 */
enum class ParticipationMode : int {
    BALANCED = 0,   // Neutral delta, two-sided trade
    AGGRESSIVE = 1, // Delta aligned with price direction (initiators)
    ABSORPTIVE = 2  // Delta opposite to price direction (responsive)
};

inline const char* ParticipationModeToString(ParticipationMode mode) {
    switch (mode) {
        case ParticipationMode::BALANCED:   return "BALANCED";
        case ParticipationMode::AGGRESSIVE: return "AGGRESSIVE";
        case ParticipationMode::ABSORPTIVE: return "ABSORPTIVE";
        default:                            return "INVALID";
    }
}

/**
 * AMTActivityType: The fundamental AMT classification
 * Derived from Intent × Participation (location-gated)
 *
 * INITIATIVE: Away from value + Aggressive OR at extreme testing new highs/lows
 *   - Directional conviction, seeking price discovery
 *   - Expects continuation if accepted, excess if rejected
 *
 * RESPONSIVE: Toward value OR (Away + Absorptive)
 *   - Returning to equilibrium or defending levels
 *   - Expects mean reversion behavior
 *
 * NEUTRAL: At value with balanced participation
 *   - Market in equilibrium, no clear directional pressure
 */
enum class AMTActivityType : int {
    NEUTRAL = 0,    // At value, balanced participation
    INITIATIVE = 1, // Seeking new value (away + aggressive)
    RESPONSIVE = 2  // Defending value or returning to it
};

inline const char* AMTActivityTypeToString(AMTActivityType type) {
    switch (type) {
        case AMTActivityType::NEUTRAL:    return "NEUTRAL";
        case AMTActivityType::INITIATIVE: return "INITIATIVE";
        case AMTActivityType::RESPONSIVE: return "RESPONSIVE";
        default:                          return "INVALID";
    }
}

// ============================================================================
// MIGRATION HELPERS: Map new AMT types to legacy types
// ============================================================================
// MapAMTStateToLegacy has been removed - all code now uses AMTMarketState directly.
// MapAMTActivityToLegacy is still used for AggressionType during transition.
// ============================================================================

/**
 * Map new AMTActivityType to legacy AggressionType
 * Used during migration to feed old consumers from new signal source.
 */
inline AggressionType MapAMTActivityToLegacy(AMTActivityType activity) {
    switch (activity) {
        case AMTActivityType::INITIATIVE: return AggressionType::INITIATIVE;
        case AMTActivityType::RESPONSIVE: return AggressionType::RESPONSIVE;
        default:                          return AggressionType::NEUTRAL;
    }
}

/**
 * ValueLocation: Price position relative to value area (POC-centric)
 * Used for location-gated inference - location is PRIMARY gate, not weighted input.
 */
enum class ValueLocation : int {
    INSIDE_VALUE = 0,   // VAL <= price <= VAH (within value area)
    ABOVE_VALUE = 1,    // price > VAH (excess high territory)
    BELOW_VALUE = 2,    // price < VAL (excess low territory)
    AT_VAH = 3,         // At upper boundary (testing)
    AT_VAL = 4,         // At lower boundary (testing)
    AT_POC = 5          // At value center (equilibrium)
};

inline const char* ValueLocationToString(ValueLocation loc) {
    switch (loc) {
        case ValueLocation::INSIDE_VALUE: return "INSIDE_VA";
        case ValueLocation::ABOVE_VALUE:  return "ABOVE_VA";
        case ValueLocation::BELOW_VALUE:  return "BELOW_VA";
        case ValueLocation::AT_VAH:       return "AT_VAH";
        case ValueLocation::AT_VAL:       return "AT_VAL";
        case ValueLocation::AT_POC:       return "AT_POC";
        default:                          return "INVALID";
    }
}

/**
 * ExcessType: Type of auction failure/excess at extremes
 * Detected via tail + rejection evidence
 */
enum class ExcessType : int {
    NONE = 0,
    POOR_HIGH = 1,      // Incomplete auction at high (no tail, abrupt rejection)
    POOR_LOW = 2,       // Incomplete auction at low (no tail, abrupt rejection)
    EXCESS_HIGH = 3,    // Confirmed excess at high (tail + sustained rejection)
    EXCESS_LOW = 4      // Confirmed excess at low (tail + sustained rejection)
};

inline const char* ExcessTypeToString(ExcessType type) {
    switch (type) {
        case ExcessType::NONE:        return "NONE";
        case ExcessType::POOR_HIGH:   return "POOR_HIGH";
        case ExcessType::POOR_LOW:    return "POOR_LOW";
        case ExcessType::EXCESS_HIGH: return "EXCESS_HIGH";
        case ExcessType::EXCESS_LOW:  return "EXCESS_LOW";
        default:                      return "INVALID";
    }
}

// ============================================================================
// DERIVE CURRENT PHASE FROM PURE AMT SIGNALS
// ============================================================================
// Per Auction Market Theory, phase is DERIVED from primary signals:
//   - AMTMarketState: BALANCE = rotation, IMBALANCE = trending
//   - ValueLocation: WHERE is price relative to value?
//   - AMTActivityType: WHO is in control?
//   - ExcessType: Is there rejection at extremes?
//   - rangeExtended: Is price at session extreme?
//
// This eliminates separate phase detection logic - phase is computed, not detected.
// ============================================================================

/**
 * Derive CurrentPhase from pure AMT signals.
 *
 * PRECEDENCE ORDER (matches DaltonState.DeriveCurrentPhase):
 *   Priority 1: FAILED_AUCTION (excess != NONE)
 *   Priority 2: BALANCE states
 *     - At boundary (AT_VAH/AT_VAL) = TESTING_BOUNDARY (probing)
 *     - Inside value = ROTATION (two-sided trade)
 *   Priority 3: IMBALANCE states
 *     - At boundary + responsive = FAILED_AUCTION (rejection)
 *     - Range extended + initiative = RANGE_EXTENSION
 *     - Responsive = PULLBACK (counter-move in trend)
 *     - Default = UNKNOWN (caller should use timeframe for DRIVING_UP/DOWN)
 *
 * KEY INSIGHT: Boundary check is INSIDE state logic because AT_VAH/AT_VAL
 * has DIFFERENT meanings depending on market state:
 *   - In BALANCE: Probing (normal rotation behavior)
 *   - In IMBALANCE + responsive: Rejection (failed breakout attempt)
 *
 * @param state         Current AMTMarketState (BALANCE/IMBALANCE)
 * @param location      Current ValueLocation
 * @param activity      Current AMTActivityType
 * @param excess        Current ExcessType
 * @param rangeExtended True if price is at session extreme
 * @return              Derived CurrentPhase
 */
inline CurrentPhase DeriveCurrentPhase(
    AMTMarketState state,
    ValueLocation location,
    AMTActivityType activity,
    ExcessType excess,
    bool rangeExtended
) {
    // =========================================================================
    // PRIORITY 1: FAILED_AUCTION (excess/rejection overrides everything)
    // =========================================================================
    if (excess != ExcessType::NONE) {
        return CurrentPhase::FAILED_AUCTION;
    }

    // =========================================================================
    // PRIORITY 2: BALANCE states (2TF - both sides active)
    // =========================================================================
    if (state == AMTMarketState::BALANCE) {
        // At boundary = probing the edge (testing for breakout/rejection)
        if (location == ValueLocation::AT_VAH || location == ValueLocation::AT_VAL) {
            return CurrentPhase::TESTING_BOUNDARY;
        }
        // Inside value = rotation (two-sided trade, mean reversion)
        return CurrentPhase::ROTATION;
    }

    // =========================================================================
    // PRIORITY 3: IMBALANCE states (1TF - one side in control)
    // =========================================================================
    if (state == AMTMarketState::IMBALANCE) {
        // At boundary with responsive activity = rejection (failed breakout)
        // Per Dalton: Price at boundary during imbalance showing responsive
        // activity indicates the breakout attempt is being rejected
        if ((location == ValueLocation::AT_VAH || location == ValueLocation::AT_VAL) &&
            activity == AMTActivityType::RESPONSIVE) {
            return CurrentPhase::FAILED_AUCTION;
        }

        // Range extension with initiative = successful OTF breakout
        if (rangeExtended && activity == AMTActivityType::INITIATIVE) {
            return CurrentPhase::RANGE_EXTENSION;
        }

        // Responsive activity within imbalance = pullback (counter-move)
        if (activity == AMTActivityType::RESPONSIVE) {
            return CurrentPhase::PULLBACK;
        }

        // Default IMBALANCE = UNKNOWN
        // Caller should use timeframe pattern for DRIVING_UP/DOWN
        // This stateless function doesn't have timeframe context
        return CurrentPhase::UNKNOWN;
    }

    // Unknown state
    return CurrentPhase::UNKNOWN;
}

// ============================================================================
// AMT SIGNAL STRUCTS
// ============================================================================

/**
 * ActivityClassification: Activity classification result for a bar
 *
 * PRIMARY AMT SIGNALS (these are the outputs that matter):
 *   - activityType: INITIATIVE / RESPONSIVE / NEUTRAL
 *   - location: WHERE is price relative to value area?
 *
 * INTERNAL (used in computation, not primary signals):
 *   - intent_: Direction relative to POC (input to activityType)
 *   - participation_: Delta vs price alignment (input to activityType)
 *
 * Per AMT research, activity type is the key signal:
 *   - INITIATIVE: Buying above value OR selling below value (unexpected, drives imbalance)
 *   - RESPONSIVE: Buying below value OR selling above value (expected, restores balance)
 *   - NEUTRAL: At value with balanced participation
 */
struct ActivityClassification {
    // ========================================================================
    // PRIMARY AMT SIGNALS
    // ========================================================================
    AMTActivityType activityType = AMTActivityType::NEUTRAL;
    ValueLocation location = ValueLocation::INSIDE_VALUE;

    // Derived metrics (observable)
    double priceVsPOC = 0.0;         // Price distance from POC (signed, ticks)
    double priceChange = 0.0;        // Bar price change (ticks)
    double deltaPct = 0.0;           // Delta as % of volume (-1 to +1)

    // Volume conviction (0.0-2.0, where 1.0 = 50th percentile volume)
    // Per Dalton: Volume confirms conviction
    // - Low volume (VACUUM) = low conviction = less weight on strength
    // - High volume = high conviction = more weight on strength
    // Formula: volumeConviction = volumePercentile / 50.0, clamped to [0, 2]
    double volumeConviction = 1.0;   // Default to normal conviction

    // Validity
    bool valid = false;

    // ========================================================================
    // INTERNAL (used in classification, not primary signals)
    // These are exposed for diagnostic logging only. Do not use for decisions.
    // ========================================================================
    ValueIntent intent_ = ValueIntent::AT_VALUE;
    ParticipationMode participation_ = ParticipationMode::BALANCED;

    // Derive activity type from intent and participation
    // AMT logic: Initiative = away from value + aggressive
    //            Responsive = toward value OR (away + absorptive)
    //            Neutral = at value + balanced
    void DeriveActivityType() {
        if (intent_ == ValueIntent::AT_VALUE && participation_ == ParticipationMode::BALANCED) {
            activityType = AMTActivityType::NEUTRAL;
        }
        else if (intent_ == ValueIntent::AWAY_FROM_VALUE && participation_ == ParticipationMode::AGGRESSIVE) {
            activityType = AMTActivityType::INITIATIVE;
        }
        else {
            // Toward value OR (away + absorptive) = RESPONSIVE
            activityType = AMTActivityType::RESPONSIVE;
        }
    }

    // Check if activity is consistent with location
    bool IsLocationConsistent() const {
        const bool outsideValue = (location == ValueLocation::ABOVE_VALUE ||
                                   location == ValueLocation::BELOW_VALUE);
        const bool atBoundary = (location == ValueLocation::AT_VAH ||
                                 location == ValueLocation::AT_VAL);

        // Initiative is expected outside value or at boundary
        if (activityType == AMTActivityType::INITIATIVE) {
            return outsideValue || atBoundary;
        }
        // Responsive is expected inside value or returning to it
        if (activityType == AMTActivityType::RESPONSIVE) {
            return !outsideValue || intent_ == ValueIntent::TOWARD_VALUE;
        }
        // Neutral is expected at POC
        return location == ValueLocation::AT_POC || location == ValueLocation::INSIDE_VALUE;
    }

    // Helper to check if price is outside value area
    bool IsOutsideValue() const {
        return location == ValueLocation::ABOVE_VALUE ||
               location == ValueLocation::BELOW_VALUE;
    }
};

/**
 * AcceptanceSignals: Evidence for price acceptance at a level
 * Used by ExtremeAcceptanceTracker and state engine
 */
struct AcceptanceSignals {
    // Time-based acceptance
    int barsAtLevel = 0;             // Bars spent within tolerance of level
    int tpoPeriodsAtLevel = 0;       // TPO periods at level (if available)
    double timeAtLevelSec = 0.0;     // Seconds spent at level

    // Volume-based acceptance
    double volumeAtLevel = 0.0;      // Total volume at level
    double volumePctOfSession = 0.0; // Volume as % of session total
    VAPDensityClass volumeDensity = VAPDensityClass::NORMAL;  // HVN/LVN classification

    // Range-based acceptance
    double rangeTicks = 0.0;         // Range covered while at level
    bool formedRotation = false;     // Did price form rotation (up then down or vice versa)

    // Composite acceptance score (0-1)
    double acceptanceScore = 0.0;

    void Reset() {
        barsAtLevel = 0;
        tpoPeriodsAtLevel = 0;
        timeAtLevelSec = 0.0;
        volumeAtLevel = 0.0;
        volumePctOfSession = 0.0;
        volumeDensity = VAPDensityClass::NORMAL;
        rangeTicks = 0.0;
        formedRotation = false;
        acceptanceScore = 0.0;
    }
};

/**
 * RejectionSignals: Evidence for price rejection at a level
 * Counterpart to AcceptanceSignals
 */
struct RejectionSignals {
    // Speed of rejection
    int barsToReject = 0;           // Bars from touch to reversal
    double velocityAwayTicks = 0.0; // Speed of move away (ticks/bar)

    // Magnitude of rejection
    double rejectionDistTicks = 0.0; // Distance traveled away from level
    double tailTicks = 0.0;          // Single-print tail size (if any)

    // Activity during rejection
    double volumeDuringReject = 0.0; // Volume during rejection move
    double deltaDuringReject = 0.0;  // Net delta during rejection (direction of pressure)
    AMTActivityType activityDuringReject = AMTActivityType::NEUTRAL;

    // Rejection confirmation
    bool confirmedRejection = false; // Multi-bar failure to return
    int failedRetestCount = 0;       // Number of failed retests

    // Composite rejection score (0-1)
    double rejectionScore = 0.0;

    void Reset() {
        barsToReject = 0;
        velocityAwayTicks = 0.0;
        rejectionDistTicks = 0.0;
        tailTicks = 0.0;
        volumeDuringReject = 0.0;
        deltaDuringReject = 0.0;
        activityDuringReject = AMTActivityType::NEUTRAL;
        confirmedRejection = false;
        failedRetestCount = 0;
        rejectionScore = 0.0;
    }
};

/**
 * StateEvidence: Evidence ledger for state transitions
 * Supports location-gated inference and diagnostic logging
 */
struct StateEvidence {
    // Current state and strength (SSOT: DaltonEngine via 1TF/2TF)
    AMTMarketState currentState = AMTMarketState::UNKNOWN;
    double stateStrength = 0.0;      // Confirmation metric (0-1), not state determinant
    int barsInState = 0;             // Consecutive bars in current state

    // Derived phase (SSOT: DaltonState.DeriveCurrentPhase())
    // Per AMT: Phase is derived from state + location + activity + structure
    CurrentPhase derivedPhase = CurrentPhase::UNKNOWN;

    // Activity classification this bar (determines WHO is in control)
    ActivityClassification activity;

    // Location context
    ValueLocation location = ValueLocation::INSIDE_VALUE;
    double distFromPOCTicks = 0.0;   // Signed distance from POC
    double distFromVAHTicks = 0.0;   // Distance from VAH (positive = above)
    double distFromVALTicks = 0.0;   // Distance from VAL (positive = above)

    // Center used for calculations
    double pocPrice = 0.0;           // POC price used as value center
    double vahPrice = 0.0;           // VAH used for boundary
    double valPrice = 0.0;           // VAL used for boundary

    // Acceptance/rejection at extremes
    AcceptanceSignals acceptanceHigh;
    AcceptanceSignals acceptanceLow;
    RejectionSignals rejectionHigh;
    RejectionSignals rejectionLow;
    ExcessType excessDetected = ExcessType::NONE;

    // Structure flags
    bool singlePrintZonePresent = false;  // Single print detected in profile
    bool rotationDetected = false;        // Price formed rotation pattern
    bool rangeExtended = false;           // Session range extended this bar
    bool ibBroken = false;                // Initial balance broken

    // Transition info (for logging on state change)
    AMTMarketState previousState = AMTMarketState::UNKNOWN;
    double strengthAtTransition = 0.0;
    int barAtTransition = 0;

    // Check if this is a state transition
    bool IsTransition() const {
        return currentState != previousState && previousState != AMTMarketState::UNKNOWN;
    }

    /**
     * Get the derived phase.
     *
     * SSOT: DaltonState.DeriveCurrentPhase() is the authoritative source.
     * This method returns the stored derivedPhase (set from Dalton).
     * Falls back to local derivation only if derivedPhase is UNKNOWN.
     *
     * Per AMT: Phase is derived from state + location + activity + structure.
     * Dalton has richer context (failedAuction flags, extension type).
     */
    CurrentPhase DerivePhase() const {
        // SSOT: Use Dalton-derived phase if available
        if (derivedPhase != CurrentPhase::UNKNOWN) {
            return derivedPhase;
        }
        // Fallback: Local derivation (warmup/legacy mode)
        return DeriveCurrentPhase(
            currentState,
            activity.location,
            activity.activityType,
            excessDetected,
            rangeExtended
        );
    }

    void Reset() {
        currentState = AMTMarketState::UNKNOWN;
        stateStrength = 0.0;
        barsInState = 0;
        derivedPhase = CurrentPhase::UNKNOWN;
        activity = ActivityClassification();
        location = ValueLocation::INSIDE_VALUE;
        distFromPOCTicks = 0.0;
        distFromVAHTicks = 0.0;
        distFromVALTicks = 0.0;
        pocPrice = 0.0;
        vahPrice = 0.0;
        valPrice = 0.0;
        acceptanceHigh.Reset();
        acceptanceLow.Reset();
        rejectionHigh.Reset();
        rejectionLow.Reset();
        excessDetected = ExcessType::NONE;
        singlePrintZonePresent = false;
        rotationDetected = false;
        rangeExtended = false;
        ibBroken = false;
        previousState = AMTMarketState::UNKNOWN;
        strengthAtTransition = 0.0;
        barAtTransition = 0;
    }
};

/**
 * SinglePrintZone: Session-persistent single print tracking
 * Single prints are contiguous areas of thin volume (LVN) in the profile
 * that indicate one-sided aggressive activity (no two-sided trade).
 */
struct SinglePrintZone {
    double highPrice = 0.0;         // Top of single print zone
    double lowPrice = 0.0;          // Bottom of single print zone
    int widthTicks = 0;             // Zone width in ticks
    int creationBar = 0;            // Bar when detected
    int creationTPO = 0;            // TPO period when detected (if available)
    bool fillStarted = false;       // Has fill-in started
    int fillTPOCount = 0;           // TPO periods of fill activity
    double fillProgress = 0.0;      // Percentage filled (0-1)
    bool valid = true;              // Still valid (not fully filled)

    // Direction context
    bool isUpwardMove = false;      // True if created during upward move

    double GetCenter() const {
        return (highPrice + lowPrice) / 2.0;
    }

    bool Contains(double price, double tolerance = 0.0) const {
        return price >= (lowPrice - tolerance) && price <= (highPrice + tolerance);
    }
};

// ============================================================================
// LEAKY ACCUMULATOR CONSTANTS
// ============================================================================

namespace AMTConfig {
    // Leaky accumulator parameters for state strength
    constexpr double STRENGTH_DECAY_RATE = 0.95;      // Per-bar decay multiplier
    constexpr double STRENGTH_GAIN_INITIATIVE = 0.15; // Gain per initiative bar
    constexpr double STRENGTH_GAIN_RESPONSIVE = 0.10; // Gain per responsive bar

    // State flip thresholds (strength-modulated)
    constexpr double BALANCE_TO_IMBALANCE_BASE = 0.60;    // Base threshold to flip to IMBALANCE
    constexpr double IMBALANCE_TO_BALANCE_BASE = 0.40;    // Base threshold to flip to BALANCE

    // Location tolerances (in ticks)
    constexpr int POC_TOLERANCE_TICKS = 2;    // Within 2 ticks = AT_POC
    constexpr int VA_BOUNDARY_TICKS = 2;      // Within 2 ticks of VAH/VAL = AT boundary

    // Single print detection
    constexpr int MIN_SINGLE_PRINT_TICKS = 3; // Minimum contiguous ticks for single print
    constexpr double SINGLE_PRINT_VOLUME_THRESHOLD = 0.15; // % of session avg for "thin"

    // Excess confirmation
    constexpr int EXCESS_CONFIRMATION_BARS = 3;   // Bars to confirm excess (multi-bar failure)
    constexpr double TAIL_MIN_TICKS = 2.0;        // Minimum tail size for excess signal
}

} // namespace AMT

#endif // AMT_CORE_H