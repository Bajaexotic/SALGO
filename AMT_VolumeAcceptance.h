// ============================================================================
// AMT_VolumeAcceptance.h - Volume Acceptance Engine
// ============================================================================
//
// PURPOSE: Volume answers "was this move accepted by the market?"
//
//   1. Did volume support the move or reject it? (AcceptanceState)
//   2. Is value forming higher/lower or unchanged? (ValueMigration)
//   3. What is 'high' volume today? (baseline-relative VolumeIntensity)
//   4. What confirmation does it provide to triggers? (ConfirmationMultiplier)
//
// DETECTION MECHANISMS:
//   - Volume-Price Confirmation: High volume + sustained move = acceptance
//   - POC Migration: POC shifting in direction of move = acceptance
//   - VA Expansion: Value area expanding in direction = acceptance
//   - Time-at-Price: Bars spent outside value with volume = acceptance
//   - Delta Confirmation: Delta aligning with price direction = acceptance
//
// REJECTION DETECTION:
//   - Low volume breakout: Price extends without volume = rejection
//   - Fast return: Price quickly returns to value = rejection
//   - POC stability: POC not following price = rejection
//   - Wick structure: Long wicks at extremes = rejection
//
// DATA SOURCES (Sierra Chart):
//   - VbP Study SG2/3/4: POC, VAH, VAL (developing values for migration)
//   - Numbers Bars SG13: Total Volume per bar
//   - Numbers Bars SG53/54: Bid/Ask Volume per second
//   - Cumulative Delta: For delta confirmation
//   - EffortBaselineStore: Phase-bucketed volume baselines
//
// DESIGN PRINCIPLES:
//   - Uses existing baselines from EffortBaselineStore (no new data collection)
//   - Phase-aware (GLOBEX != RTH volume profiles differ dramatically)
//   - Hysteresis prevents acceptance/rejection whipsaw
//   - NO-FALLBACK contract: explicit validity at every decision point
//   - ZERO Sierra Chart dependencies (testable standalone)
//
// INTEGRATION:
//   VolumeAcceptanceEngine volumeEngine;
//   volumeEngine.SetEffortStore(&effortStore);
//   volumeEngine.SetPhase(currentPhase);
//
//   VolumeAcceptanceResult result = volumeEngine.Compute(...);
//   if (result.IsReady()) {
//       if (result.IsAccepted() && result.IsHighVolume()) {
//           // Strong volume-confirmed acceptance
//       }
//   }
//
// SOURCES:
//   - Jim Dalton's "Markets in Profile" - Value Area acceptance theory
//   - Trading Riot AMT - "price breaks fair value on significant volume"
//   - Sierra Chart VbP/Numbers Bars studies
//
// ============================================================================

#pragma once

#include "amt_core.h"
#include "AMT_Snapshots.h"
#include "AMT_ValueLocation.h"  // For ValueLocationResult (SSOT)
#include <algorithm>
#include <cmath>
#include <deque>

namespace AMT {

// ============================================================================
// ACCEPTANCE STATE ENUM
// ============================================================================
// Primary answer: Did the market accept or reject this price level/move?
//
// ACCEPTED: Volume confirms price is staying at new level
//   - High relative volume
//   - Price sustaining outside prior value
//   - POC migrating toward price
//
// REJECTED: Volume confirms price is being pushed back
//   - Low volume on extension
//   - Quick return to value
//   - Absorption at extremes
//
// TESTING: Price probing new levels, acceptance undetermined
//   - At value edge
//   - Mixed signals
//   - Need more bars to confirm
// ============================================================================

enum class AcceptanceState : int {
    UNKNOWN = 0,      // Baseline not ready or insufficient data
    TESTING = 1,      // At value edge, probing new levels
    ACCEPTED = 2,     // Volume confirms move accepted
    REJECTED = 3      // Volume indicates move rejected
};

inline const char* AcceptanceStateToString(AcceptanceState s) {
    switch (s) {
        case AcceptanceState::UNKNOWN:  return "UNKNOWN";
        case AcceptanceState::TESTING:  return "TESTING";
        case AcceptanceState::ACCEPTED: return "ACCEPTED";
        case AcceptanceState::REJECTED: return "REJECTED";
    }
    return "UNK";
}

inline const char* AcceptanceStateToShortString(AcceptanceState s) {
    switch (s) {
        case AcceptanceState::UNKNOWN:  return "UNK";
        case AcceptanceState::TESTING:  return "TEST";
        case AcceptanceState::ACCEPTED: return "ACC";
        case AcceptanceState::REJECTED: return "REJ";
    }
    return "?";
}

// ============================================================================
// NOTE: ValueMigrationState has been consolidated into ValueMigration (amt_core.h)
// Use ValueMigration::UNCHANGED, HIGHER, LOWER, ROTATING instead.
// ============================================================================

// ============================================================================
// VOLUME INTENSITY ENUM
// ============================================================================
// What is 'high' volume today? (baseline-relative classification)
// ============================================================================

enum class VolumeIntensity : int {
    UNKNOWN = 0,     // Baseline not ready
    VERY_LOW = 1,    // < P10 - extremely quiet
    LOW = 2,         // P10-P25 - below normal
    NORMAL = 3,      // P25-P75 - typical activity
    HIGH = 4,        // P75-P90 - elevated activity
    VERY_HIGH = 5,   // P90-P95 - significantly elevated
    EXTREME = 6,     // P95-P99 - rare event, likely institutional
    SHOCK = 7        // >= P99 - exceptional, potential news/event
};

inline const char* VolumeIntensityToString(VolumeIntensity v) {
    switch (v) {
        case VolumeIntensity::UNKNOWN:   return "UNKNOWN";
        case VolumeIntensity::VERY_LOW:  return "VERY_LOW";
        case VolumeIntensity::LOW:       return "LOW";
        case VolumeIntensity::NORMAL:    return "NORMAL";
        case VolumeIntensity::HIGH:      return "HIGH";
        case VolumeIntensity::VERY_HIGH: return "VERY_HIGH";
        case VolumeIntensity::EXTREME:   return "EXTREME";
        case VolumeIntensity::SHOCK:     return "SHOCK";
    }
    return "UNK";
}

inline const char* VolumeIntensityToShortString(VolumeIntensity v) {
    switch (v) {
        case VolumeIntensity::UNKNOWN:   return "?";
        case VolumeIntensity::VERY_LOW:  return "VL";
        case VolumeIntensity::LOW:       return "LO";
        case VolumeIntensity::NORMAL:    return "NM";
        case VolumeIntensity::HIGH:      return "HI";
        case VolumeIntensity::VERY_HIGH: return "VH";
        case VolumeIntensity::EXTREME:   return "EX";
        case VolumeIntensity::SHOCK:     return "SH";
    }
    return "?";
}

// ============================================================================
// POC BEHAVIOR ENUM (Value Build Classification)
// ============================================================================
// Classifies POC movement pattern over recent window.
// Used by ValueBuildEngine to determine BUILD vs MIGRATE vs STALL.
//
// STABLE: POC not moving significantly - BUILD signature
//   - Variance low, no consistent drift
//   - Value concentrating at current level
//
// DRIFTING: POC moving consistently in one direction - MIGRATE signature
//   - Directional persistence high
//   - Value relocating to new level
//
// ERRATIC: POC oscillating without persistence - STALL signature
//   - High variance, frequent reversals
//   - Market churning without resolution
// ============================================================================

enum class POCBehavior : int {
    UNKNOWN = 0,    // Insufficient history or invalid data
    STABLE = 1,     // POC not moving significantly - BUILD signature
    DRIFTING = 2,   // POC moving consistently in one direction - MIGRATE signature
    ERRATIC = 3     // POC oscillating without persistence - STALL signature
};

inline const char* POCBehaviorToString(POCBehavior b) {
    switch (b) {
        case POCBehavior::UNKNOWN:  return "UNKNOWN";
        case POCBehavior::STABLE:   return "STABLE";
        case POCBehavior::DRIFTING: return "DRIFTING";
        case POCBehavior::ERRATIC:  return "ERRATIC";
    }
    return "UNK";
}

inline const char* POCBehaviorToShortString(POCBehavior b) {
    switch (b) {
        case POCBehavior::UNKNOWN:  return "?";
        case POCBehavior::STABLE:   return "STB";
        case POCBehavior::DRIFTING: return "DRF";
        case POCBehavior::ERRATIC:  return "ERR";
    }
    return "?";
}

// ============================================================================
// VA BEHAVIOR ENUM (Value Build Classification)
// ============================================================================
// Classifies Value Area development pattern over recent window.
// Used by ValueBuildEngine to determine BUILD vs MIGRATE vs STALL.
//
// THICKENING: Value concentrating at current level - BUILD signature
//   - VA width stable or contracting
//   - Volume concentrating around POC
//
// SHIFTING: Value area relocating with POC - MIGRATE signature
//   - VA midpoint moving with POC drift
//   - Acceptance in motion
//
// EXPANDING: Value area growing aimlessly - STALL signature
//   - VA width increasing
//   - Volume spread across prices without focus
// ============================================================================

enum class VABehavior : int {
    UNKNOWN = 0,      // Insufficient history or invalid data
    THICKENING = 1,   // Value concentrating at current level - BUILD signature
    SHIFTING = 2,     // Value area relocating with POC - MIGRATE signature
    EXPANDING = 3     // Value area growing aimlessly - STALL signature
};

inline const char* VABehaviorToString(VABehavior b) {
    switch (b) {
        case VABehavior::UNKNOWN:    return "UNKNOWN";
        case VABehavior::THICKENING: return "THICKENING";
        case VABehavior::SHIFTING:   return "SHIFTING";
        case VABehavior::EXPANDING:  return "EXPANDING";
    }
    return "UNK";
}

inline const char* VABehaviorToShortString(VABehavior b) {
    switch (b) {
        case VABehavior::UNKNOWN:    return "?";
        case VABehavior::THICKENING: return "THK";
        case VABehavior::SHIFTING:   return "SHF";
        case VABehavior::EXPANDING:  return "EXP";
    }
    return "?";
}

// ============================================================================
// VALUE BUILD STATE ENUM
// ============================================================================
// Primary classification of how value is being built/accepted.
// Computed by VolumeAcceptanceEngine and included in VolumeAcceptanceResult.
//
// BUILD: Acceptance in place (POC stable + VA thickening)
//   - Value concentrating at current level
//   - POC not moving, VA width stable
//
// MIGRATE: Acceptance in motion (POC drifting + VA shifting)
//   - Value relocating to new level
//   - POC following price, VA moving with it
//
// STALL: Participation without resolution (POC erratic + VA expanding)
//   - Market churning, no clear direction
//   - High variance in POC, VA spreading
//
// FAIL: Attempted acceptance denied (fast return + rejection)
//   - Price returned quickly to prior value
//   - Market rejected new price level
// ============================================================================

enum class ValueBuildState : int {
    UNKNOWN = 0,     // Insufficient data
    BUILD = 1,       // Acceptance in place (value thickening)
    MIGRATE = 2,     // Acceptance in motion (value relocating)
    STALL = 3,       // Participation without resolution (churn)
    FAIL = 4         // Attempted acceptance denied (rejection)
};

inline const char* ValueBuildStateToString(ValueBuildState s) {
    switch (s) {
        case ValueBuildState::UNKNOWN: return "UNKNOWN";
        case ValueBuildState::BUILD:   return "BUILD";
        case ValueBuildState::MIGRATE: return "MIGRATE";
        case ValueBuildState::STALL:   return "STALL";
        case ValueBuildState::FAIL:    return "FAIL";
    }
    return "UNK";
}

inline const char* ValueBuildStateToShortString(ValueBuildState s) {
    switch (s) {
        case ValueBuildState::UNKNOWN: return "?";
        case ValueBuildState::BUILD:   return "BLD";
        case ValueBuildState::MIGRATE: return "MIG";
        case ValueBuildState::STALL:   return "STL";
        case ValueBuildState::FAIL:    return "FAL";
    }
    return "?";
}

// ============================================================================
// ACCEPTANCE ERROR REASON
// ============================================================================
// Explicit error tracking (no silent fallbacks).
// ============================================================================

enum class AcceptanceErrorReason : int {
    NONE = 0,

    // Warmup states (expected, not errors)
    WARMUP_VOLUME_BASELINE = 10,   // Volume baseline not ready
    WARMUP_POC_HISTORY = 11,       // POC migration needs more history
    WARMUP_VA_HISTORY = 12,        // VA tracking needs more history
    WARMUP_MULTIPLE = 13,          // Multiple baselines warming

    // Input validation errors
    ERR_INVALID_VOLUME = 20,       // Volume data invalid (negative, NaN)
    ERR_INVALID_PRICE = 21,        // Price data invalid
    ERR_INVALID_VA = 22,           // Value area invalid (VAH <= VAL)

    // Configuration errors
    ERR_NO_EFFORT_STORE = 30,      // EffortBaselineStore not configured
    ERR_INVALID_PHASE = 31,        // Non-tradeable phase

    // Session events
    SESSION_RESET = 40             // Just transitioned, no session evidence yet
};

inline const char* AcceptanceErrorToString(AcceptanceErrorReason r) {
    switch (r) {
        case AcceptanceErrorReason::NONE:                  return "NONE";
        case AcceptanceErrorReason::WARMUP_VOLUME_BASELINE: return "WARMUP_VOL";
        case AcceptanceErrorReason::WARMUP_POC_HISTORY:    return "WARMUP_POC";
        case AcceptanceErrorReason::WARMUP_VA_HISTORY:     return "WARMUP_VA";
        case AcceptanceErrorReason::WARMUP_MULTIPLE:       return "WARMUP_MULTI";
        case AcceptanceErrorReason::ERR_INVALID_VOLUME:    return "INVALID_VOL";
        case AcceptanceErrorReason::ERR_INVALID_PRICE:     return "INVALID_PRICE";
        case AcceptanceErrorReason::ERR_INVALID_VA:        return "INVALID_VA";
        case AcceptanceErrorReason::ERR_NO_EFFORT_STORE:   return "NO_EFFORT_STORE";
        case AcceptanceErrorReason::ERR_INVALID_PHASE:     return "INVALID_PHASE";
        case AcceptanceErrorReason::SESSION_RESET:         return "SESSION_RESET";
    }
    return "UNK_ERR";
}

inline bool IsAcceptanceWarmup(AcceptanceErrorReason r) {
    return r == AcceptanceErrorReason::WARMUP_VOLUME_BASELINE ||
           r == AcceptanceErrorReason::WARMUP_POC_HISTORY ||
           r == AcceptanceErrorReason::WARMUP_VA_HISTORY ||
           r == AcceptanceErrorReason::WARMUP_MULTIPLE;
}

// ============================================================================
// CONFIRMATION REQUIREMENT
// ============================================================================
// What confirmation does volume provide to triggers?
// Used by downstream consumers to gate or enhance signals.
// ============================================================================

struct ConfirmationRequirement {
    // Multiplier for signal confidence (0.5 = halve, 1.0 = neutral, 2.0 = double)
    double confidenceMultiplier = 1.0;

    // Specific requirements
    bool requiresHighVolume = false;      // Signal needs high volume to trigger
    bool requiresAcceptance = false;      // Signal needs acceptance state
    bool allowsLowVolume = true;          // Low volume doesn't block
    bool enhancedByVolume = false;        // High volume enhances signal

    // Thresholds
    double minVolumePercentile = 0.0;     // Minimum volume percentile required
    double minAcceptanceScore = 0.0;      // Minimum acceptance score required

    // Convenience check
    bool IsRestrictive() const {
        return requiresHighVolume || requiresAcceptance;
    }

    bool IsSatisfied(double volumePctile, double acceptanceScore) const {
        if (requiresHighVolume && volumePctile < minVolumePercentile) return false;
        if (requiresAcceptance && acceptanceScore < minAcceptanceScore) return false;
        return true;
    }
};

// ============================================================================
// POC MIGRATION TRACKER
// ============================================================================
// Tracks POC position over time to detect meaningful value migration.
// ============================================================================

struct POCMigrationTracker {
    // Developing POC values (updated as bar develops)
    double currentPOC = 0.0;
    double priorBarPOC = 0.0;
    double sessionOpenPOC = 0.0;      // POC at session start

    // Migration metrics
    double migrationTicks = 0.0;       // Current POC - session open POC
    double migrationRate = 0.0;        // Ticks per bar average
    int migrationDirection = 0;        // +1 = up, -1 = down, 0 = stable

    // Stability tracking
    int barsAtLevel = 0;               // Consecutive bars POC at same tick
    int barsStable = 0;                // Bars within stability threshold
    bool isStable = false;             // POC not moving significantly

    // History for trend detection
    std::deque<double> pocHistory;
    static constexpr size_t MAX_HISTORY = 20;

    // ========================================================================
    // VALUE BUILD METRICS (Jan 2025)
    // ========================================================================
    // Extended metrics for POCBehavior classification (BUILD/MIGRATE/STALL).
    // ========================================================================

    // Change variance - how erratic is POC movement?
    double changeVariance = 0.0;       // Variance of bar-to-bar POC changes (ticks^2)
    double changeStdDev = 0.0;         // Std dev of changes (ticks)

    // Directional persistence - how consistently does POC drift one way?
    double directionPersistence = 0.0; // [0, 1] ratio of consistent direction moves
    int consecutiveSameDir = 0;        // Consecutive same-direction moves

    // Reversal tracking - how often does POC direction flip?
    int reversalCount = 0;             // Count of direction reversals in window
    int lastDirection = 0;             // Last non-zero direction (+1/-1)

    // Behavior classification (computed)
    POCBehavior behavior = POCBehavior::UNKNOWN;
    bool behaviorValid = false;

    // Configuration thresholds
    static constexpr double STABLE_VARIANCE_THRESHOLD = 2.0;    // Below = STABLE (2 ticks^2)
    static constexpr double DRIFT_PERSISTENCE_THRESHOLD = 0.6;  // Above = DRIFTING
    static constexpr int ERRATIC_REVERSAL_THRESHOLD = 4;        // Above = ERRATIC (in 20-bar window)

    void Reset() {
        currentPOC = 0.0;
        priorBarPOC = 0.0;
        sessionOpenPOC = 0.0;
        migrationTicks = 0.0;
        migrationRate = 0.0;
        migrationDirection = 0;
        barsAtLevel = 0;
        barsStable = 0;
        isStable = false;
        pocHistory.clear();

        // Value build metrics
        changeVariance = 0.0;
        changeStdDev = 0.0;
        directionPersistence = 0.0;
        consecutiveSameDir = 0;
        reversalCount = 0;
        lastDirection = 0;
        behavior = POCBehavior::UNKNOWN;
        behaviorValid = false;
    }

    void Update(double poc, double tickSize, int stabilityThresholdTicks = 2) {
        priorBarPOC = currentPOC;
        currentPOC = poc;

        if (sessionOpenPOC == 0.0) {
            sessionOpenPOC = poc;
        }

        // Calculate migration
        migrationTicks = (poc - sessionOpenPOC) / tickSize;

        // Update history
        pocHistory.push_back(poc);
        while (pocHistory.size() > MAX_HISTORY) {
            pocHistory.pop_front();
        }

        // Calculate migration rate
        if (pocHistory.size() >= 2) {
            migrationRate = (pocHistory.back() - pocHistory.front()) /
                           (tickSize * static_cast<double>(pocHistory.size()));
        }

        // Stability check
        double shiftTicks = std::abs(poc - priorBarPOC) / tickSize;
        if (shiftTicks < stabilityThresholdTicks) {
            barsStable++;
            if (shiftTicks < 1.0) barsAtLevel++;
        } else {
            barsStable = 0;
            barsAtLevel = 0;
        }

        isStable = (barsStable >= 3);

        // Direction
        if (migrationTicks > 2.0) {
            migrationDirection = 1;
        } else if (migrationTicks < -2.0) {
            migrationDirection = -1;
        } else {
            migrationDirection = 0;
        }

        // ====================================================================
        // VALUE BUILD METRICS COMPUTATION
        // ====================================================================
        ComputeValueBuildMetrics(tickSize);
        ClassifyBehavior();
    }

    // Compute variance, persistence, and reversal metrics from history
    void ComputeValueBuildMetrics(double tickSize) {
        behaviorValid = false;

        // Need at least 5 bars for meaningful statistics
        if (pocHistory.size() < 5) {
            return;
        }

        // Compute bar-to-bar changes in ticks
        std::vector<double> changes;
        changes.reserve(pocHistory.size() - 1);

        int sameDirectionCount = 0;
        int totalMoves = 0;
        reversalCount = 0;
        int prevDir = 0;

        for (size_t i = 1; i < pocHistory.size(); ++i) {
            double changeTicks = (pocHistory[i] - pocHistory[i - 1]) / tickSize;
            changes.push_back(changeTicks);

            // Track direction for persistence
            int dir = 0;
            if (changeTicks > 0.5) dir = 1;
            else if (changeTicks < -0.5) dir = -1;

            if (dir != 0) {
                totalMoves++;
                if (prevDir != 0 && dir == prevDir) {
                    sameDirectionCount++;
                }
                // Detect reversal (direction flip)
                if (prevDir != 0 && dir != prevDir) {
                    reversalCount++;
                }
                prevDir = dir;
            }
        }

        if (changes.empty()) {
            return;
        }

        // Compute mean
        double sum = 0.0;
        for (double c : changes) {
            sum += c;
        }
        double mean = sum / static_cast<double>(changes.size());

        // Compute variance
        double varSum = 0.0;
        for (double c : changes) {
            double diff = c - mean;
            varSum += diff * diff;
        }
        changeVariance = varSum / static_cast<double>(changes.size());
        changeStdDev = std::sqrt(changeVariance);

        // Compute directional persistence
        // Ratio of same-direction moves to total moves
        if (totalMoves > 1) {
            // Maximum possible same-dir moves = totalMoves - 1
            directionPersistence = static_cast<double>(sameDirectionCount) /
                                   static_cast<double>(totalMoves - 1);
        } else {
            directionPersistence = 0.0;
        }

        // Track consecutive same-direction for current direction
        if (prevDir != 0) {
            if (lastDirection == prevDir) {
                consecutiveSameDir++;
            } else {
                consecutiveSameDir = 1;
                lastDirection = prevDir;
            }
        }

        behaviorValid = true;
    }

    // Classify POC behavior based on computed metrics
    void ClassifyBehavior() {
        if (!behaviorValid) {
            behavior = POCBehavior::UNKNOWN;
            return;
        }

        // Classification priority:
        // 1. ERRATIC - high reversals indicate churn
        // 2. STABLE - low variance indicates value building in place
        // 3. DRIFTING - high persistence indicates value relocating

        if (reversalCount >= ERRATIC_REVERSAL_THRESHOLD) {
            behavior = POCBehavior::ERRATIC;
        }
        else if (changeVariance < STABLE_VARIANCE_THRESHOLD && isStable) {
            behavior = POCBehavior::STABLE;
        }
        else if (directionPersistence >= DRIFT_PERSISTENCE_THRESHOLD) {
            behavior = POCBehavior::DRIFTING;
        }
        else if (changeVariance < STABLE_VARIANCE_THRESHOLD) {
            // Low variance but not stable for 3 bars - still classify as STABLE
            behavior = POCBehavior::STABLE;
        }
        else {
            // Moderate variance, moderate persistence - ambiguous
            // Default to ERRATIC if high variance, else DRIFTING
            if (changeVariance > STABLE_VARIANCE_THRESHOLD * 2.0) {
                behavior = POCBehavior::ERRATIC;
            } else {
                behavior = POCBehavior::DRIFTING;
            }
        }
    }

    // Helpers for downstream consumers
    bool IsBehaviorValid() const { return behaviorValid; }
    bool IsStablePOC() const { return behaviorValid && behavior == POCBehavior::STABLE; }
    bool IsDriftingPOC() const { return behaviorValid && behavior == POCBehavior::DRIFTING; }
    bool IsErraticPOC() const { return behaviorValid && behavior == POCBehavior::ERRATIC; }
};

// ============================================================================
// VALUE AREA TRACKER
// ============================================================================
// Tracks Value Area expansion/contraction and overlap.
// ============================================================================

struct ValueAreaTracker {
    // Current session VA
    double currentVAH = 0.0;
    double currentVAL = 0.0;
    double currentVAWidth = 0.0;

    // Prior bar VA (for expansion detection)
    double priorVAH = 0.0;
    double priorVAL = 0.0;

    // Session open VA (for session-level tracking)
    double sessionOpenVAH = 0.0;
    double sessionOpenVAL = 0.0;

    // Prior session VA (for overnight gap context)
    double priorSessionVAH = 0.0;
    double priorSessionVAL = 0.0;

    // Expansion metrics
    double expansionHighTicks = 0.0;   // How much VAH expanded
    double expansionLowTicks = 0.0;    // How much VAL expanded
    double netExpansionTicks = 0.0;    // Total width change

    // Overlap with prior
    double overlapPct = 1.0;           // [0, 1] overlap with prior session VA

    // Direction
    int expansionBias = 0;             // +1 = expanding up, -1 = down, 0 = balanced

    // ========================================================================
    // VALUE BUILD METRICS (Jan 2025)
    // ========================================================================
    // Extended metrics for VABehavior classification (BUILD/MIGRATE/STALL).
    // ========================================================================

    // History for tracking
    std::deque<double> midpointHistory;    // VA midpoint history
    std::deque<double> widthHistory;       // VA width history
    static constexpr size_t MAX_VA_HISTORY = 20;

    // Midpoint shift rate - how fast is VA center moving?
    double midpointShiftRate = 0.0;        // Ticks/bar average
    double midpointTotalShift = 0.0;       // Total shift from session start

    // Width change rate - is VA concentrating or spreading?
    double widthChangeRate = 0.0;          // Ticks/bar average (positive = spreading)
    double avgWidth = 0.0;                 // Average width over window
    double widthStdDev = 0.0;              // Std dev of width

    // Behavior classification (computed)
    VABehavior behavior = VABehavior::UNKNOWN;
    bool behaviorValid = false;

    // Configuration thresholds
    static constexpr double THICKENING_WIDTH_CHANGE_MAX = 0.5;   // Width change < 0.5 ticks/bar = thickening
    static constexpr double SHIFTING_MIDPOINT_MIN = 0.3;         // Midpoint shift > 0.3 ticks/bar = shifting
    static constexpr double EXPANDING_WIDTH_CHANGE_MIN = 1.0;    // Width change > 1.0 ticks/bar = expanding

    void Reset() {
        currentVAH = 0.0;
        currentVAL = 0.0;
        currentVAWidth = 0.0;
        priorVAH = 0.0;
        priorVAL = 0.0;
        sessionOpenVAH = 0.0;
        sessionOpenVAL = 0.0;
        expansionHighTicks = 0.0;
        expansionLowTicks = 0.0;
        netExpansionTicks = 0.0;
        overlapPct = 1.0;
        expansionBias = 0;

        // Value build metrics
        midpointHistory.clear();
        widthHistory.clear();
        midpointShiftRate = 0.0;
        midpointTotalShift = 0.0;
        widthChangeRate = 0.0;
        avgWidth = 0.0;
        widthStdDev = 0.0;
        behavior = VABehavior::UNKNOWN;
        behaviorValid = false;
    }

    void Update(double vah, double val, double tickSize) {
        if (vah <= val) return;  // Invalid

        priorVAH = currentVAH;
        priorVAL = currentVAL;
        currentVAH = vah;
        currentVAL = val;
        currentVAWidth = (vah - val) / tickSize;

        if (sessionOpenVAH == 0.0) {
            sessionOpenVAH = vah;
            sessionOpenVAL = val;
        }

        // Calculate expansion since session open
        expansionHighTicks = (vah - sessionOpenVAH) / tickSize;
        expansionLowTicks = (sessionOpenVAL - val) / tickSize;

        double openWidth = (sessionOpenVAH - sessionOpenVAL) / tickSize;
        netExpansionTicks = currentVAWidth - openWidth;

        // Expansion bias
        if (expansionHighTicks > expansionLowTicks + 2.0) {
            expansionBias = 1;  // Expanding upward
        } else if (expansionLowTicks > expansionHighTicks + 2.0) {
            expansionBias = -1; // Expanding downward
        } else {
            expansionBias = 0;  // Balanced
        }

        // ====================================================================
        // VALUE BUILD METRICS COMPUTATION
        // ====================================================================
        ComputeValueBuildMetrics(tickSize);
        ClassifyBehavior();
    }

    // Compute midpoint shift rate, width change rate from history
    void ComputeValueBuildMetrics(double tickSize) {
        behaviorValid = false;

        // Compute midpoint
        double midpoint = (currentVAH + currentVAL) / 2.0;
        double midpointTicks = midpoint / tickSize;

        // Update history
        midpointHistory.push_back(midpointTicks);
        widthHistory.push_back(currentVAWidth);

        while (midpointHistory.size() > MAX_VA_HISTORY) {
            midpointHistory.pop_front();
        }
        while (widthHistory.size() > MAX_VA_HISTORY) {
            widthHistory.pop_front();
        }

        // Need at least 5 bars for meaningful statistics
        if (midpointHistory.size() < 5) {
            return;
        }

        // Compute midpoint shift rate (first vs last in window)
        double firstMidpoint = midpointHistory.front();
        double lastMidpoint = midpointHistory.back();
        double histSize = static_cast<double>(midpointHistory.size());
        midpointShiftRate = (lastMidpoint - firstMidpoint) / histSize;

        // Total shift from session start
        double sessionOpenMidpoint = ((sessionOpenVAH + sessionOpenVAL) / 2.0) / tickSize;
        midpointTotalShift = lastMidpoint - sessionOpenMidpoint;

        // Compute width change rate (first vs last in window)
        double firstWidth = widthHistory.front();
        double lastWidth = widthHistory.back();
        widthChangeRate = (lastWidth - firstWidth) / histSize;

        // Compute average width and std dev
        double widthSum = 0.0;
        for (double w : widthHistory) {
            widthSum += w;
        }
        avgWidth = widthSum / histSize;

        double varSum = 0.0;
        for (double w : widthHistory) {
            double diff = w - avgWidth;
            varSum += diff * diff;
        }
        widthStdDev = std::sqrt(varSum / histSize);

        behaviorValid = true;
    }

    // Classify VA behavior based on computed metrics
    void ClassifyBehavior() {
        if (!behaviorValid) {
            behavior = VABehavior::UNKNOWN;
            return;
        }

        // Classification logic:
        //
        // THICKENING: VA width stable/contracting, midpoint stable
        //   - Value concentrating at current level (BUILD signature)
        //
        // SHIFTING: VA midpoint moving consistently
        //   - Value area relocating with POC (MIGRATE signature)
        //
        // EXPANDING: VA width growing rapidly
        //   - Volume spreading across prices without focus (STALL signature)

        double absMidShift = std::abs(midpointShiftRate);

        if (widthChangeRate > EXPANDING_WIDTH_CHANGE_MIN) {
            // VA expanding rapidly - STALL signature
            behavior = VABehavior::EXPANDING;
        }
        else if (absMidShift >= SHIFTING_MIDPOINT_MIN) {
            // VA midpoint moving meaningfully - MIGRATE signature
            behavior = VABehavior::SHIFTING;
        }
        else if (std::abs(widthChangeRate) <= THICKENING_WIDTH_CHANGE_MAX) {
            // VA width stable, midpoint stable - BUILD signature
            behavior = VABehavior::THICKENING;
        }
        else {
            // Moderate width expansion, low midpoint shift
            // Default to EXPANDING if width is growing
            if (widthChangeRate > 0.0) {
                behavior = VABehavior::EXPANDING;
            } else {
                behavior = VABehavior::THICKENING;
            }
        }
    }

    // Helpers for downstream consumers
    bool IsBehaviorValid() const { return behaviorValid; }
    bool IsThickeningVA() const { return behaviorValid && behavior == VABehavior::THICKENING; }
    bool IsShiftingVA() const { return behaviorValid && behavior == VABehavior::SHIFTING; }
    bool IsExpandingVA() const { return behaviorValid && behavior == VABehavior::EXPANDING; }

    void SetPriorSession(double vah, double val) {
        priorSessionVAH = vah;
        priorSessionVAL = val;
    }

    void ComputeOverlap() {
        if (priorSessionVAH <= priorSessionVAL || currentVAH <= currentVAL) {
            overlapPct = 1.0;
            return;
        }

        double overlapHigh = (std::min)(currentVAH, priorSessionVAH);
        double overlapLow = (std::max)(currentVAL, priorSessionVAL);
        double overlapRange = (std::max)(0.0, overlapHigh - overlapLow);

        double currentRange = currentVAH - currentVAL;
        double priorRange = priorSessionVAH - priorSessionVAL;
        double avgRange = (currentRange + priorRange) / 2.0;

        if (avgRange > 0.0) {
            overlapPct = overlapRange / avgRange;
            overlapPct = (std::min)(1.0, (std::max)(0.0, overlapPct));
        }
    }
};

// ============================================================================
// VOLUME LOCATION CONTEXT (Value-Relative Awareness)
// ============================================================================
// Provides volume acceptance engine with location context from ValueLocationEngine.
// Uses full 9-state ValueZone for direction-aware acceptance/rejection decisions.
//
// AMT INSIGHT: Volume acceptance is location-dependent:
//   - At POC: Rotation expected, lower acceptance threshold
//   - At VAH/VAL: Breakout/rejection testing, absorption matters
//   - Outside Value: Must sustain with volume to accept
//   - In Discovery: High conviction required for new value
// ============================================================================

struct VolumeLocationContext {
    // =========================================================================
    // PRIMARY ZONE CLASSIFICATION (9-State SSOT)
    // =========================================================================

    // Full 9-state zone from ValueLocationEngine (SSOT)
    ValueZone zone = ValueZone::UNKNOWN;

    // Distance metrics (signed, in ticks)
    double distanceFromPOCTicks = 0.0;
    double distanceFromVAHTicks = 0.0;   // Positive = above VAH
    double distanceFromVALTicks = 0.0;   // Negative = below VAL

    // =========================================================================
    // HVN/LVN OVERLAY (Orthogonal to Zone)
    // =========================================================================
    // High Volume Nodes = support/resistance; Low Volume Nodes = acceleration

    bool atHVN = false;                  // At a High Volume Node (cluster)
    bool atLVN = false;                  // At a Low Volume Node (void)
    int nearbyHVNs = 0;                  // Count of HVNs within N ticks
    int nearbyLVNs = 0;                  // Count of LVNs within N ticks

    // =========================================================================
    // STRUCTURAL OVERLAY
    // =========================================================================

    bool isBalanceStructure = false;     // VA overlap suggests balance (fade)
    bool isTrendStructure = false;       // VA separated suggests trend (follow)

    // =========================================================================
    // MIGRATION OVERLAY
    // =========================================================================

    ValueMigration migration = ValueMigration::UNCHANGED;
    bool pocMigratingTowardPrice = false;
    bool pocMigratingAwayFromPrice = false;
    double pocMigrationRate = 0.0;       // Ticks per bar

    // =========================================================================
    // SESSION CONTEXT
    // =========================================================================

    bool isAboveSessionHigh = false;
    bool isBelowSessionLow = false;
    bool isAtSessionExtreme = false;     // At or beyond session high/low

    // =========================================================================
    // VALIDITY
    // =========================================================================

    bool isValid = false;

    // =========================================================================
    // SSOT-COMPLIANT BUILDER (Primary method)
    // =========================================================================

    /**
     * Build VolumeLocationContext from ValueLocationResult (SSOT consumer).
     *
     * SSOT: ValueLocationEngine is the Single Source of Truth for location.
     * This method CONSUMES that SSOT to provide volume-specific context.
     *
     * @param valLocResult   Result from ValueLocationEngine (SSOT)
     * @param pocMigration   POC migration state from VolumeAcceptanceEngine
     * @param pocRate        POC migration rate in ticks/bar
     * @param atHVN          True if price is at a High Volume Node
     * @param atLVN          True if price is at a Low Volume Node
     * @param hvnCount       Count of nearby HVNs
     * @param lvnCount       Count of nearby LVNs
     * @param vaOverlapping  True if current/prior VA have significant overlap
     * @param sessionHigh    Current session high price
     * @param sessionLow     Current session low price
     * @param currentPrice   Current price for session extreme check
     * @param tickSize       Tick size for comparison
     */
    static VolumeLocationContext BuildFromValueLocation(
        const ValueLocationResult& valLocResult,
        ValueMigration pocMigration = ValueMigration::UNCHANGED,
        double pocRate = 0.0,
        bool atHVN = false,
        bool atLVN = false,
        int hvnCount = 0,
        int lvnCount = 0,
        bool vaOverlapping = false,
        double sessionHigh = 0.0,
        double sessionLow = 0.0,
        double currentPrice = 0.0,
        double tickSize = 0.25)
    {
        VolumeLocationContext ctx;

        if (!valLocResult.IsReady()) {
            ctx.isValid = false;
            return ctx;
        }

        // Copy zone directly from SSOT (no mapping, full 9-state)
        ctx.zone = valLocResult.zone;

        // Copy distances from SSOT
        ctx.distanceFromPOCTicks = valLocResult.distFromPOCTicks;
        ctx.distanceFromVAHTicks = valLocResult.distFromVAHTicks;
        ctx.distanceFromVALTicks = valLocResult.distFromVALTicks;

        // HVN/LVN overlay
        ctx.atHVN = atHVN;
        ctx.atLVN = atLVN;
        ctx.nearbyHVNs = hvnCount;
        ctx.nearbyLVNs = lvnCount;

        // Structural overlay
        ctx.isBalanceStructure = vaOverlapping;
        ctx.isTrendStructure = !vaOverlapping;

        // Migration overlay
        ctx.migration = pocMigration;
        ctx.pocMigrationRate = pocRate;

        // Determine migration direction relative to price
        if (pocRate > 0.3) {
            // POC migrating up
            ctx.pocMigratingTowardPrice = (valLocResult.distFromPOCTicks > 0);
            ctx.pocMigratingAwayFromPrice = (valLocResult.distFromPOCTicks < 0);
        } else if (pocRate < -0.3) {
            // POC migrating down
            ctx.pocMigratingTowardPrice = (valLocResult.distFromPOCTicks < 0);
            ctx.pocMigratingAwayFromPrice = (valLocResult.distFromPOCTicks > 0);
        }

        // Session context
        if (sessionHigh > 0.0 && sessionLow > 0.0 && tickSize > 0.0) {
            ctx.isAboveSessionHigh = currentPrice > sessionHigh;
            ctx.isBelowSessionLow = currentPrice < sessionLow;
            ctx.isAtSessionExtreme = ctx.isAboveSessionHigh || ctx.isBelowSessionLow ||
                std::abs(currentPrice - sessionHigh) <= tickSize * 2.0 ||
                std::abs(currentPrice - sessionLow) <= tickSize * 2.0;
        }

        ctx.isValid = true;
        return ctx;
    }

    // =========================================================================
    // HELPER METHODS (Derived from zone - single source of truth)
    // =========================================================================

    /** True if inside value area (AT_POC, UPPER_VALUE, LOWER_VALUE) */
    bool IsInValue() const {
        return zone == ValueZone::AT_POC ||
               zone == ValueZone::UPPER_VALUE ||
               zone == ValueZone::LOWER_VALUE;
    }

    /** True if at value area edge (AT_VAH or AT_VAL) */
    bool IsAtEdge() const {
        return zone == ValueZone::AT_VAH || zone == ValueZone::AT_VAL;
    }

    /** True if outside value (NEAR_ABOVE/BELOW) */
    bool IsOutsideValue() const {
        return zone == ValueZone::NEAR_ABOVE_VALUE ||
               zone == ValueZone::NEAR_BELOW_VALUE;
    }

    /** True if in discovery (FAR_ABOVE/BELOW) */
    bool IsInDiscovery() const {
        return zone == ValueZone::FAR_ABOVE_VALUE ||
               zone == ValueZone::FAR_BELOW_VALUE;
    }

    /** True if above value area (AT_VAH, NEAR_ABOVE, FAR_ABOVE) */
    bool IsAboveValue() const {
        return zone == ValueZone::AT_VAH ||
               zone == ValueZone::NEAR_ABOVE_VALUE ||
               zone == ValueZone::FAR_ABOVE_VALUE;
    }

    /** True if below value area (AT_VAL, NEAR_BELOW, FAR_BELOW) */
    bool IsBelowValue() const {
        return zone == ValueZone::AT_VAL ||
               zone == ValueZone::NEAR_BELOW_VALUE ||
               zone == ValueZone::FAR_BELOW_VALUE;
    }

    /** True if at POC */
    bool IsAtPOC() const {
        return zone == ValueZone::AT_POC;
    }

    // =========================================================================
    // VOLUME-SPECIFIC CONTEXT HELPERS
    // =========================================================================

    /** True if at an HVN (potential support/resistance) */
    bool IsAtSupportResistance() const {
        return atHVN || IsAtEdge();
    }

    /** True if at an LVN (potential acceleration zone) */
    bool IsAtAccelerationZone() const {
        return atLVN;
    }

    /** True if POC is following price (acceptance signal) */
    bool IsPOCFollowingPrice() const {
        return pocMigratingTowardPrice && std::abs(pocMigrationRate) > 0.3;
    }

    /** True if POC is retreating from price (rejection signal) */
    bool IsPOCRetreatingFromPrice() const {
        return pocMigratingAwayFromPrice && std::abs(pocMigrationRate) > 0.3;
    }

    /** True if structure suggests fade opportunity */
    bool IsFadeContext() const {
        return isBalanceStructure && (IsAtEdge() || IsOutsideValue());
    }

    /** True if structure suggests trend continuation */
    bool IsTrendContext() const {
        return isTrendStructure && (IsOutsideValue() || IsInDiscovery());
    }
};

// ============================================================================
// VOLUME ACCEPTANCE RESULT (Per-Bar Output)
// ============================================================================
// Complete snapshot of volume acceptance state for the current bar.
// ============================================================================

struct VolumeAcceptanceResult {
    // =========================================================================
    // PRIMARY OUTPUTS (The 4 Questions)
    // =========================================================================

    // Q1: Did volume support or reject the move?
    AcceptanceState state = AcceptanceState::UNKNOWN;
    double acceptanceScore = 0.0;        // [0, 1] composite score

    // Q2: Is value forming higher/lower?
    ValueMigration migration = ValueMigration::UNKNOWN;
    double pocMigrationTicks = 0.0;      // POC shift from session open
    int migrationDirection = 0;          // +1=up, -1=down, 0=stable

    // Q3: What is 'high' volume today?
    VolumeIntensity intensity = VolumeIntensity::UNKNOWN;
    double volumePercentile = 50.0;      // Current bar vs baseline
    double volumeRatioToAvg = 1.0;       // Current / average

    // Q3b: Is this an extreme volume event?
    bool isExtremeVolume = false;        // >= P95 (rare event)
    bool isShockVolume = false;          // >= P99 (exceptional event)

    // Q4: What confirmation does it provide?
    ConfirmationRequirement confirmation;
    double confirmationMultiplier = 1.0; // For downstream consumers

    // =========================================================================
    // RAW VOLUME METRICS
    // =========================================================================
    double totalVolume = 0.0;            // Total volume this bar
    double volumePerSecond = 0.0;        // Rate-normalized volume
    double bidVolume = 0.0;              // Volume at bid
    double askVolume = 0.0;              // Volume at ask
    double delta = 0.0;                  // Ask - Bid
    double deltaRatio = 0.0;             // Delta / Volume [-1, 1]

    // =========================================================================
    // POC TRACKING
    // =========================================================================
    double currentPOC = 0.0;
    double priorPOC = 0.0;
    double pocShiftTicks = 0.0;          // This bar's POC shift
    double pocShiftPercentile = 50.0;    // vs historical shifts
    bool pocMigrating = false;           // Consistent direction
    int pocStabilityBars = 0;            // Bars POC stable

    // =========================================================================
    // VALUE AREA TRACKING
    // =========================================================================
    double currentVAH = 0.0;
    double currentVAL = 0.0;
    double vaWidth = 0.0;                // VAH - VAL in ticks
    double vaExpansionTicks = 0.0;       // Expansion since session open
    double vaOverlapPct = 1.0;           // Overlap with prior session
    int vaExpansionBias = 0;             // +1=up, -1=down, 0=balanced

    // =========================================================================
    // PRICE LOCATION CONTEXT (Legacy - Kept for Compatibility)
    // =========================================================================
    bool priceAboveVA = false;           // Current price > VAH
    bool priceBelowVA = false;           // Current price < VAL
    bool priceInVA = false;              // Price within VA
    double distanceToVAHticks = 0.0;     // Ticks to VAH
    double distanceToVALticks = 0.0;     // Ticks to VAL
    double distanceToPOCticks = 0.0;     // Ticks to POC

    // =========================================================================
    // SSOT LOCATION CONTEXT (9-State ValueZone)
    // =========================================================================
    // Full location context from ValueLocationEngine (SSOT consumer).
    // Use this for direction-aware acceptance/rejection decisions.
    VolumeLocationContext locationCtx;
    bool hasLocationContext = false;     // True if locationCtx is populated

    // Location-conditioned flags (set based on locationCtx + acceptance state)
    bool hvnTestDetected = false;        // Testing an HVN (support/resistance)
    bool lvnTraverseDetected = false;    // Traversing an LVN (acceleration)
    bool structureConflict = false;      // Balance structure but outside value

    // =========================================================================
    // ACCEPTANCE COMPONENTS (For Diagnostics)
    // =========================================================================
    double volumeComponent = 0.0;        // Volume contribution to acceptance
    double priceActionComponent = 0.0;   // Price behavior contribution
    double timeComponent = 0.0;          // Time spent at level contribution
    double deltaComponent = 0.0;         // Delta confirmation contribution
    double pocMigrationComponent = 0.0;  // POC migration contribution

    // =========================================================================
    // REJECTION SIGNALS
    // =========================================================================
    bool lowVolumeBreakout = false;      // Extended on low volume
    bool fastReturn = false;             // Quick return to value
    bool wickRejection = false;          // Long wick rejection pattern
    bool deltaRejection = false;         // Delta diverged from direction
    double rejectionScore = 0.0;         // [0, 1] composite rejection

    // =========================================================================
    // VALUE BUILD METRICS (Jan 2025)
    // =========================================================================
    // Extended metrics for ValueBuildEngine consumption.
    // Enables BUILD/MIGRATE/STALL/FAIL state classification.
    // =========================================================================

    // POC Behavior classification
    POCBehavior pocBehavior = POCBehavior::UNKNOWN;
    bool pocBehaviorValid = false;
    double pocChangeVariance = 0.0;      // Variance of POC changes (ticks^2)
    double pocDirectionPersistence = 0.0; // [0, 1] consistency of drift
    int pocReversalCount = 0;            // Reversals in window

    // VA Behavior classification
    VABehavior vaBehavior = VABehavior::UNKNOWN;
    bool vaBehaviorValid = false;
    double vaMidpointShiftRate = 0.0;    // Ticks/bar VA center movement
    double vaWidthChangeRate = 0.0;      // Ticks/bar VA width change

    // Hold Outside VA tracking
    int barsOutsideVA = 0;               // Consecutive bars price outside VA
    int barsOutsideVAWithVolume = 0;     // Bars outside VA with >= normal volume
    bool isHoldingOutside = false;       // Held outside >= 3 bars with volume

    // POC Follows Price tracking
    double pocPriceCorrelation = 0.0;    // [-1, 1] correlation of POC moves with price
    bool pocFollowsPrice = false;        // POC is tracking price direction
    bool pocRetreatsFromPrice = false;   // POC is moving away from price

    // =========================================================================
    // VALUE BUILD STATE (Jan 2025)
    // =========================================================================
    // Primary classification of how value is being built.
    // Computed directly by VolumeAcceptanceEngine (not a separate engine).
    // =========================================================================

    ValueBuildState valueBuildState = ValueBuildState::UNKNOWN;
    bool valueBuildValid = false;        // True if valueBuildState is meaningful

    // Component scores [0, 1] for diagnostics
    double buildScore = 0.0;             // Confidence for BUILD state
    double migrateScore = 0.0;           // Confidence for MIGRATE state
    double stallScore = 0.0;             // Confidence for STALL state
    double failScore = 0.0;              // Confidence for FAIL state

    // Hysteresis state for value build
    ValueBuildState confirmedValueBuildState = ValueBuildState::UNKNOWN;
    ValueBuildState candidateValueBuildState = ValueBuildState::UNKNOWN;
    int valueBuildConfirmationBars = 0;  // Bars candidate has been stable
    int barsInValueBuildState = 0;       // Bars in confirmed state
    bool valueBuildTransitioning = false; // True if candidate != confirmed

    // Event (only true on transition bar)
    bool valueBuildStateChanged = false;

    // =========================================================================
    // HYSTERESIS STATE (Acceptance)
    // =========================================================================
    AcceptanceState confirmedState = AcceptanceState::UNKNOWN;
    AcceptanceState candidateState = AcceptanceState::UNKNOWN;
    int confirmationBars = 0;
    int barsInState = 0;
    bool isTransitioning = false;

    // =========================================================================
    // EVENTS (Only True on Transition Bars)
    // =========================================================================
    bool acceptanceConfirmed = false;    // Just confirmed acceptance
    bool rejectionConfirmed = false;     // Just confirmed rejection
    bool stateChanged = false;           // Any state change

    // =========================================================================
    // VALIDITY / ERROR
    // =========================================================================
    AcceptanceErrorReason errorReason = AcceptanceErrorReason::NONE;
    SessionPhase phase = SessionPhase::UNKNOWN;
    int errorBar = -1;

    // =========================================================================
    // ACCESSORS
    // =========================================================================

    bool IsReady() const {
        return errorReason == AcceptanceErrorReason::NONE;
    }

    bool IsWarmup() const {
        return IsAcceptanceWarmup(errorReason);
    }

    bool IsHardError() const {
        return errorReason != AcceptanceErrorReason::NONE && !IsWarmup();
    }

    bool IsAccepted() const {
        return IsReady() && confirmedState == AcceptanceState::ACCEPTED;
    }

    bool IsRejected() const {
        return IsReady() && confirmedState == AcceptanceState::REJECTED;
    }

    bool IsTesting() const {
        return IsReady() && confirmedState == AcceptanceState::TESTING;
    }

    bool IsHighVolume() const {
        return IsReady() && (intensity == VolumeIntensity::HIGH ||
                            intensity == VolumeIntensity::VERY_HIGH ||
                            intensity == VolumeIntensity::EXTREME ||
                            intensity == VolumeIntensity::SHOCK);
    }

    bool IsLowVolume() const {
        return IsReady() && (intensity == VolumeIntensity::LOW ||
                            intensity == VolumeIntensity::VERY_LOW);
    }

    bool IsExtreme() const {
        return IsReady() && isExtremeVolume;
    }

    bool IsShock() const {
        return IsReady() && isShockVolume;
    }

    bool IsMigratingUp() const {
        return IsReady() && migration == ValueMigration::HIGHER;
    }

    bool IsMigratingDown() const {
        return IsReady() && migration == ValueMigration::LOWER;
    }

    // Composite quality check
    bool IsHighQualityAcceptance() const {
        return IsAccepted() &&
               IsHighVolume() &&
               acceptanceScore >= 0.7 &&
               std::abs(deltaRatio) >= 0.3;  // Delta confirms
    }

    bool IsHighQualityRejection() const {
        return IsRejected() &&
               rejectionScore >= 0.7 &&
               (lowVolumeBreakout || fastReturn || wickRejection);
    }

    // =========================================================================
    // VALUE BUILD ACCESSORS (Jan 2025)
    // =========================================================================
    // Helpers for ValueBuildEngine to query behavior classifications.
    // =========================================================================

    /** True if POC behavior classification is valid */
    bool HasPOCBehavior() const { return pocBehaviorValid; }

    /** True if VA behavior classification is valid */
    bool HasVABehavior() const { return vaBehaviorValid; }

    /** True if both POC and VA behavior are valid */
    bool HasValueBuildContext() const { return pocBehaviorValid && vaBehaviorValid; }

    // POC Behavior queries
    bool IsStablePOC() const { return pocBehaviorValid && pocBehavior == POCBehavior::STABLE; }
    bool IsDriftingPOC() const { return pocBehaviorValid && pocBehavior == POCBehavior::DRIFTING; }
    bool IsErraticPOC() const { return pocBehaviorValid && pocBehavior == POCBehavior::ERRATIC; }

    // VA Behavior queries
    bool IsThickeningVA() const { return vaBehaviorValid && vaBehavior == VABehavior::THICKENING; }
    bool IsShiftingVA() const { return vaBehaviorValid && vaBehavior == VABehavior::SHIFTING; }
    bool IsExpandingVA() const { return vaBehaviorValid && vaBehavior == VABehavior::EXPANDING; }

    // Composite Value Build state indicators (used by ValueBuildEngine)
    /** BUILD signature: POC stable + VA thickening + holding outside */
    bool HasBuildSignature() const {
        return HasValueBuildContext() &&
               IsStablePOC() &&
               IsThickeningVA() &&
               isHoldingOutside;
    }

    /** MIGRATE signature: POC drifting + VA shifting + POC follows price */
    bool HasMigrateSignature() const {
        return HasValueBuildContext() &&
               IsDriftingPOC() &&
               (IsShiftingVA() || IsThickeningVA()) &&  // SHIFTING primary, THICKENING ok
               pocFollowsPrice;
    }

    /** STALL signature: POC erratic + VA expanding */
    bool HasStallSignature() const {
        return HasValueBuildContext() &&
               IsErraticPOC() &&
               IsExpandingVA();
    }

    /** FAIL signature: fast return detected */
    bool HasFailSignature() const {
        return fastReturn || (IsRejected() && !isHoldingOutside);
    }

    // Value Build State accessors (primary outputs)
    /** True if value build state is valid (has required context) */
    bool HasValueBuildState() const { return valueBuildValid; }

    /** True if confirmed state is BUILD (acceptance in place) */
    bool IsBuild() const {
        return valueBuildValid && confirmedValueBuildState == ValueBuildState::BUILD;
    }

    /** True if confirmed state is MIGRATE (acceptance in motion) */
    bool IsMigrate() const {
        return valueBuildValid && confirmedValueBuildState == ValueBuildState::MIGRATE;
    }

    /** True if confirmed state is STALL (participation without resolution) */
    bool IsStall() const {
        return valueBuildValid && confirmedValueBuildState == ValueBuildState::STALL;
    }

    /** True if confirmed state is FAIL (attempted acceptance denied) */
    bool IsFail() const {
        return valueBuildValid && confirmedValueBuildState == ValueBuildState::FAIL;
    }

    /** True if value is being accepted (BUILD or MIGRATE) */
    bool IsAcceptingValue() const {
        return valueBuildValid &&
               (confirmedValueBuildState == ValueBuildState::BUILD ||
                confirmedValueBuildState == ValueBuildState::MIGRATE);
    }

    /** Get dominant score among the 4 states */
    double GetValueBuildDominantScore() const {
        return (std::max)({buildScore, migrateScore, stallScore, failScore});
    }

    // =========================================================================
    // LOCATION CONTEXT ACCESSORS
    // =========================================================================

    /** True if location context was attached */
    bool HasLocationContext() const {
        return hasLocationContext && locationCtx.isValid;
    }

    /** Delegate to location context helper methods */
    bool IsInValue() const {
        return HasLocationContext() && locationCtx.IsInValue();
    }

    bool IsAtEdge() const {
        return HasLocationContext() && locationCtx.IsAtEdge();
    }

    bool IsOutsideValue() const {
        return HasLocationContext() && locationCtx.IsOutsideValue();
    }

    bool IsInDiscovery() const {
        return HasLocationContext() && locationCtx.IsInDiscovery();
    }

    bool IsAboveValue() const {
        return HasLocationContext() && locationCtx.IsAboveValue();
    }

    bool IsBelowValue() const {
        return HasLocationContext() && locationCtx.IsBelowValue();
    }

    /** Location-conditioned quality checks */
    bool IsAcceptanceAtEdge() const {
        return IsAccepted() && IsAtEdge();
    }

    bool IsRejectionAtEdge() const {
        return IsRejected() && IsAtEdge();
    }

    bool IsAcceptanceOutsideValue() const {
        return IsAccepted() && (IsOutsideValue() || IsInDiscovery());
    }

    /** True if structure conflicts with location (balance structure but outside value) */
    bool HasStructureConflict() const {
        return structureConflict ||
               (HasLocationContext() &&
                locationCtx.isBalanceStructure &&
                (IsOutsideValue() || IsInDiscovery()) &&
                IsHighVolume());
    }

    /** True if POC is confirming the acceptance */
    bool IsPOCConfirmingAcceptance() const {
        return HasLocationContext() &&
               IsAccepted() &&
               locationCtx.IsPOCFollowingPrice();
    }

    /** True if POC is confirming the rejection */
    bool IsPOCConfirmingRejection() const {
        return HasLocationContext() &&
               IsRejected() &&
               locationCtx.IsPOCRetreatingFromPrice();
    }

    /** Location-aware high quality acceptance */
    bool IsHighQualityAcceptanceWithContext() const {
        if (!HasLocationContext()) return IsHighQualityAcceptance();
        return IsHighQualityAcceptance() &&
               (locationCtx.IsPOCFollowingPrice() || locationCtx.IsOutsideValue());
    }

    /** Location-aware high quality rejection */
    bool IsHighQualityRejectionWithContext() const {
        if (!HasLocationContext()) return IsHighQualityRejection();
        return IsHighQualityRejection() &&
               locationCtx.IsAtEdge();
    }
};

// ============================================================================
// VOLUME ACCEPTANCE CONFIGURATION
// ============================================================================

struct VolumeAcceptanceConfig {
    // =========================================================================
    // VOLUME INTENSITY THRESHOLDS (Percentiles)
    // =========================================================================
    double veryLowThreshold = 10.0;      // < P10 = very low
    double lowThreshold = 25.0;          // P10-P25 = low
    double highThreshold = 75.0;         // P75-P90 = high
    double veryHighThreshold = 90.0;     // P90-P95 = very high
    double extremeThreshold = 95.0;      // P95-P99 = extreme (rare, institutional)
    double shockThreshold = 99.0;        // >= P99 = shock (exceptional, news/event)

    // =========================================================================
    // ACCEPTANCE THRESHOLDS
    // =========================================================================
    double acceptanceScoreThreshold = 0.6;  // Score to confirm acceptance
    double rejectionScoreThreshold = 0.6;   // Score to confirm rejection

    // Volume confirmation for moves
    double volumeConfirmationPctile = 60.0; // Minimum volume for confirmed move
    double lowVolumeBreakoutPctile = 30.0;  // Volume below this = low volume breakout

    // =========================================================================
    // POC MIGRATION THRESHOLDS
    // =========================================================================
    double pocMigrationMinTicks = 2.0;      // Minimum meaningful POC shift
    int pocStabilityBars = 3;               // Bars for POC stability
    double pocMigrationRateThreshold = 0.3; // Ticks/bar for "migrating"

    // =========================================================================
    // VALUE AREA THRESHOLDS
    // =========================================================================
    double vaOverlapHighThreshold = 0.7;    // >70% = overlapping (balance)
    double vaOverlapLowThreshold = 0.3;     // <30% = extension (trend)
    double vaExpansionMinTicks = 4.0;       // Minimum meaningful expansion

    // =========================================================================
    // REJECTION THRESHOLDS
    // =========================================================================
    int fastReturnBars = 3;                 // Return within N bars = fast
    double wickRejectionRatio = 0.4;        // Wick > 40% of range = rejection
    double deltaRejectionThreshold = 0.3;   // Delta divergence threshold

    // =========================================================================
    // HYSTERESIS
    // =========================================================================
    int minConfirmationBars = 2;            // Bars to confirm state change
    int maxPersistenceBars = 15;            // Max bars state persists

    // =========================================================================
    // CONFIRMATION MULTIPLIERS (Per State)
    // =========================================================================
    double acceptedHighVolumeMultiplier = 1.5;   // High volume + accepted
    double acceptedNormalVolumeMultiplier = 1.0; // Normal volume + accepted
    double testingMultiplier = 0.8;              // Testing state
    double rejectedMultiplier = 0.5;             // Rejected = reduce confidence
    double lowVolumeMultiplier = 0.7;            // Low volume = caution

    // =========================================================================
    // COMPONENT WEIGHTS (For Acceptance Score)
    // =========================================================================
    double weightVolume = 0.30;         // Volume component weight
    double weightPriceAction = 0.20;    // Price behavior weight
    double weightTime = 0.15;           // Time at level weight
    double weightDelta = 0.20;          // Delta confirmation weight
    double weightPOCMigration = 0.15;   // POC movement weight

    // =========================================================================
    // BASELINE REQUIREMENTS
    // =========================================================================
    size_t baselineMinSamples = 10;     // Minimum samples before ready
    int pocHistoryMinBars = 5;          // Minimum POC history
};

// ============================================================================
// VOLUME ACCEPTANCE ENGINE
// ============================================================================
// Main engine for detecting volume acceptance/rejection.
//
// USAGE:
//   1. Create engine and configure
//   2. Set effortStore reference (required)
//   3. Call SetPhase() each bar with current session phase
//   4. Call Compute() with bar data
//   5. Check result.IsReady() before using state
//
// SESSION BOUNDARY:
//   Call ResetForSession() at start of new session
//
// ============================================================================

class VolumeAcceptanceEngine {
public:
    // =========================================================================
    // CONFIGURATION
    // =========================================================================
    VolumeAcceptanceConfig config;

    // =========================================================================
    // REFERENCES (Not Owned)
    // =========================================================================
    const EffortBaselineStore* effortStore = nullptr;

    // =========================================================================
    // CURRENT STATE
    // =========================================================================
    SessionPhase currentPhase = SessionPhase::UNKNOWN;

    // Trackers
    POCMigrationTracker pocTracker;
    ValueAreaTracker vaTracker;

    // Hysteresis state
    AcceptanceState confirmedState = AcceptanceState::UNKNOWN;
    AcceptanceState candidateState = AcceptanceState::UNKNOWN;
    int candidateConfirmationBars = 0;
    int barsInConfirmedState = 0;

    // Session evidence
    int sessionBars = 0;
    double sessionTotalVolume = 0.0;
    double sessionHighPrice = 0.0;
    double sessionLowPrice = 0.0;
    int barsAboveVA = 0;
    int barsBelowVA = 0;
    int barsInVA = 0;

    // =========================================================================
    // VALUE BUILD STATE (Jan 2025)
    // =========================================================================
    // Tracks holdOutsideVA and POC-price correlation for value build detection.
    // =========================================================================

    // Hold outside VA tracking (acceptance signal)
    int consecutiveBarsOutside = 0;       // Consecutive bars outside VA
    int consecutiveBarsOutsideWithVol = 0; // Above with >= normal volume
    static constexpr int HOLD_OUTSIDE_THRESHOLD = 3; // Bars needed to confirm

    // Value Build State hysteresis (BUILD/MIGRATE/STALL/FAIL)
    ValueBuildState confirmedValueBuildState = ValueBuildState::UNKNOWN;
    ValueBuildState candidateValueBuildState = ValueBuildState::UNKNOWN;
    int valueBuildCandidateConfirmBars = 0;
    int barsInConfirmedValueBuildState = 0;
    static constexpr int VALUE_BUILD_MIN_CONFIRM_BARS = 2;
    static constexpr int VALUE_BUILD_MAX_PERSISTENCE_BARS = 20;

    // POC-Price correlation tracking
    struct POCPriceRecord {
        double pocChangeTicks = 0.0;
        double priceChangeTicks = 0.0;
    };
    std::deque<POCPriceRecord> pocPriceHistory;
    static constexpr size_t MAX_POC_PRICE_HISTORY = 10;
    double prevClose = 0.0;              // For price change calculation

    // Recent price history (for fast return detection)
    struct PriceRecord {
        double close = 0.0;
        double high = 0.0;
        double low = 0.0;
        bool inVA = false;
        int barIndex = 0;
    };
    std::deque<PriceRecord> priceHistory;
    static constexpr size_t MAX_PRICE_HISTORY = 20;

    // Baselines (local, not phase-bucketed for POC shifts)
    RollingDist pocShiftBaseline;
    RollingDist volumeRatioBaseline;

    // =========================================================================
    // CONSTRUCTOR / INITIALIZATION
    // =========================================================================

    VolumeAcceptanceEngine() {
        pocShiftBaseline.reset(300);
        volumeRatioBaseline.reset(300);
    }

    void SetEffortStore(const EffortBaselineStore* store) {
        effortStore = store;
    }

    void SetPhase(SessionPhase phase) {
        currentPhase = phase;
    }

    void SetConfig(const VolumeAcceptanceConfig& cfg) {
        config = cfg;
    }

    // =========================================================================
    // MAIN COMPUTATION
    // =========================================================================
    // Call once per closed bar with all available data.
    //
    // Required parameters:
    //   close, high, low - Bar prices
    //   tickSize - Tick size for normalization
    //   barIndex - Current bar index
    //   totalVolume - Total bar volume
    //
    // Optional parameters (pass 0 if unavailable):
    //   poc, vah, val - Current profile levels
    //   bidVolume, askVolume - Volume split
    //   delta - Cumulative delta
    //   priorPOC, priorVAH, priorVAL - Prior session levels
    //
    // DEPRECATED: Use ComputeFromValueLocation() for SSOT compliance
    [[deprecated("Use ComputeFromValueLocation() with ValueLocationResult from ValueLocationEngine (SSOT)")]]
    VolumeAcceptanceResult Compute(
        // Price data (required)
        double close, double high, double low,
        double tickSize, int barIndex,
        // Volume data (required)
        double totalVolume,
        // Optional volume split
        double bidVolume = 0.0, double askVolume = 0.0,
        double delta = 0.0,
        // Profile data (optional, pass 0 if unavailable)
        double poc = 0.0, double vah = 0.0, double val = 0.0,
        // Prior session levels (optional)
        double priorPOC = 0.0, double priorVAH = 0.0, double priorVAL = 0.0,
        // Rate data (optional)
        double volumePerSecond = 0.0
    ) {
        VolumeAcceptanceResult result;
        result.phase = currentPhase;

        // =====================================================================
        // INPUT VALIDATION
        // =====================================================================
        if (!std::isfinite(close) || !std::isfinite(high) || !std::isfinite(low) ||
            high <= 0.0 || low <= 0.0 || high < low) {
            result.errorReason = AcceptanceErrorReason::ERR_INVALID_PRICE;
            result.errorBar = barIndex;
            return result;
        }

        if (!std::isfinite(totalVolume) || totalVolume < 0.0) {
            result.errorReason = AcceptanceErrorReason::ERR_INVALID_VOLUME;
            result.errorBar = barIndex;
            return result;
        }

        if (tickSize <= 0.0 || !std::isfinite(tickSize)) {
            result.errorReason = AcceptanceErrorReason::ERR_INVALID_PRICE;
            result.errorBar = barIndex;
            return result;
        }

        if (effortStore == nullptr) {
            result.errorReason = AcceptanceErrorReason::ERR_NO_EFFORT_STORE;
            result.errorBar = barIndex;
            return result;
        }

        if (vah > 0.0 && val > 0.0 && vah <= val) {
            result.errorReason = AcceptanceErrorReason::ERR_INVALID_VA;
            result.errorBar = barIndex;
            return result;
        }

        sessionBars++;
        sessionTotalVolume += totalVolume;

        // Track session extremes
        if (sessionHighPrice == 0.0 || high > sessionHighPrice) sessionHighPrice = high;
        if (sessionLowPrice == 0.0 || low < sessionLowPrice) sessionLowPrice = low;

        // =====================================================================
        // STEP 1: POPULATE RAW METRICS
        // =====================================================================
        result.totalVolume = totalVolume;
        result.volumePerSecond = volumePerSecond;
        result.bidVolume = bidVolume;
        result.askVolume = askVolume;
        result.delta = delta;

        if (totalVolume > 0.0) {
            result.deltaRatio = delta / totalVolume;
            result.deltaRatio = (std::max)(-1.0, (std::min)(1.0, result.deltaRatio));
        }

        result.currentPOC = poc;
        result.currentVAH = vah;
        result.currentVAL = val;

        // =====================================================================
        // STEP 2: QUERY PHASE-AWARE BASELINE
        // =====================================================================
        const int phaseIdx = SessionPhaseToBucketIndex(currentPhase);
        if (phaseIdx < 0) {
            result.errorReason = AcceptanceErrorReason::ERR_INVALID_PHASE;
            result.errorBar = barIndex;
            return result;
        }

        const auto& bucket = effortStore->Get(currentPhase);

        // Get volume percentile
        auto volPctile = bucket.vol_sec.TryPercentile(volumePerSecond > 0 ? volumePerSecond : totalVolume);
        if (!volPctile.valid) {
            result.errorReason = AcceptanceErrorReason::WARMUP_VOLUME_BASELINE;
            result.errorBar = barIndex;
            // Continue to populate other metrics for diagnostics
        } else {
            result.volumePercentile = volPctile.value;
        }

        // =====================================================================
        // STEP 3: CLASSIFY VOLUME INTENSITY
        // =====================================================================
        if (volPctile.valid) {
            result.intensity = ClassifyVolumeIntensity(result.volumePercentile);

            // Set extreme flags (for downstream consumers)
            result.isExtremeVolume = (result.volumePercentile >= config.extremeThreshold);
            result.isShockVolume = (result.volumePercentile >= config.shockThreshold);

            // Calculate ratio to average (use TryMean for safety)
            auto meanResult = bucket.vol_sec.TryMean();
            if (meanResult.valid && meanResult.value > 0.0) {
                double avgVolume = meanResult.value;
                double currentVol = volumePerSecond > 0 ? volumePerSecond : totalVolume;
                result.volumeRatioToAvg = currentVol / avgVolume;
            }
        }

        // =====================================================================
        // STEP 4: UPDATE POC TRACKING
        // =====================================================================
        if (poc > 0.0) {
            result.priorPOC = pocTracker.currentPOC;
            pocTracker.Update(poc, tickSize);

            result.pocMigrationTicks = pocTracker.migrationTicks;
            result.migrationDirection = pocTracker.migrationDirection;
            result.pocStabilityBars = pocTracker.barsStable;

            // POC shift this bar
            if (result.priorPOC > 0.0) {
                result.pocShiftTicks = (poc - result.priorPOC) / tickSize;
                pocShiftBaseline.push(std::abs(result.pocShiftTicks));

                auto shiftPctile = pocShiftBaseline.TryPercentile(std::abs(result.pocShiftTicks));
                if (shiftPctile.valid) {
                    result.pocShiftPercentile = shiftPctile.value;
                }
            }

            // Is POC migrating?
            result.pocMigrating = (std::abs(pocTracker.migrationRate) >= config.pocMigrationRateThreshold);
        }

        // =====================================================================
        // STEP 5: UPDATE VALUE AREA TRACKING
        // =====================================================================
        if (vah > 0.0 && val > 0.0) {
            vaTracker.Update(vah, val, tickSize);

            result.vaWidth = vaTracker.currentVAWidth;
            result.vaExpansionTicks = vaTracker.netExpansionTicks;
            result.vaExpansionBias = vaTracker.expansionBias;

            // Set prior session if provided
            if (priorVAH > 0.0 && priorVAL > 0.0) {
                vaTracker.SetPriorSession(priorVAH, priorVAL);
                vaTracker.ComputeOverlap();
                result.vaOverlapPct = vaTracker.overlapPct;
            }
        }

        // =====================================================================
        // STEP 6: COMPUTE PRICE LOCATION
        // =====================================================================
        if (vah > 0.0 && val > 0.0) {
            result.priceAboveVA = (close > vah);
            result.priceBelowVA = (close < val);
            result.priceInVA = !result.priceAboveVA && !result.priceBelowVA;

            result.distanceToVAHticks = (vah - close) / tickSize;
            result.distanceToVALticks = (close - val) / tickSize;
            result.distanceToPOCticks = (poc > 0.0) ? (close - poc) / tickSize : 0.0;

            // Track bars in/out of VA
            if (result.priceAboveVA) barsAboveVA++;
            else if (result.priceBelowVA) barsBelowVA++;
            else barsInVA++;
        }

        // =====================================================================
        // STEP 6b: COMPUTE VALUE BUILD METRICS (Jan 2025)
        // =====================================================================
        {
            bool isOutsideVA = result.priceAboveVA || result.priceBelowVA;
            bool isHighVol = (result.intensity == VolumeIntensity::HIGH ||
                              result.intensity == VolumeIntensity::VERY_HIGH ||
                              result.intensity == VolumeIntensity::EXTREME ||
                              result.intensity == VolumeIntensity::SHOCK);
            ComputeValueBuildMetrics(result, close, tickSize, isOutsideVA, isHighVol);
        }

        // =====================================================================
        // STEP 7: UPDATE PRICE HISTORY
        // =====================================================================
        PriceRecord record;
        record.close = close;
        record.high = high;
        record.low = low;
        record.inVA = result.priceInVA;
        record.barIndex = barIndex;
        priceHistory.push_back(record);
        while (priceHistory.size() > MAX_PRICE_HISTORY) {
            priceHistory.pop_front();
        }

        // =====================================================================
        // STEP 8: DETECT REJECTION SIGNALS
        // =====================================================================
        DetectRejectionSignals(result, close, high, low, tickSize, barIndex);

        // =====================================================================
        // STEP 9: COMPUTE ACCEPTANCE COMPONENTS
        // =====================================================================
        ComputeAcceptanceComponents(result, close, tickSize);

        // =====================================================================
        // STEP 10: DETERMINE VALUE MIGRATION STATE
        // =====================================================================
        result.migration = DetermineValueMigration(result);

        // =====================================================================
        // STEP 11: COMPUTE ACCEPTANCE/REJECTION SCORES
        // =====================================================================
        ComputeAcceptanceScore(result);
        ComputeRejectionScore(result);

        // =====================================================================
        // STEP 12: DETERMINE RAW STATE
        // =====================================================================
        AcceptanceState rawState = DetermineRawState(result);
        result.state = rawState;

        // =====================================================================
        // STEP 13: APPLY HYSTERESIS
        // =====================================================================
        UpdateHysteresis(result, rawState);

        // =====================================================================
        // STEP 14: COMPUTE CONFIRMATION MULTIPLIER
        // =====================================================================
        ComputeConfirmation(result);

        // =====================================================================
        // STEP 15: COMPUTE VALUE BUILD STATE (BUILD/MIGRATE/STALL/FAIL)
        // =====================================================================
        ComputeValueBuildState(result);

        // =====================================================================
        // STEP 16: CHECK WARMUP STATE
        // =====================================================================
        if (result.errorReason == AcceptanceErrorReason::NONE) {
            AcceptanceErrorReason warmupReason = CheckWarmupState();
            if (warmupReason != AcceptanceErrorReason::NONE) {
                result.errorReason = warmupReason;
            }
        }

        return result;
    }

    // =========================================================================
    // SSOT-COMPLIANT COMPUTE (Jan 2025)
    // =========================================================================
    //
    // Preferred entry point. Consumes ValueLocationResult from ValueLocationEngine
    // instead of receiving raw POC/VAH/VAL values. This ensures:
    //   - Single source of truth for value-relative location
    //   - Consistent VA overlap and acceptance calculations
    //   - Pre-computed value migration available for acceptance detection
    //
    VolumeAcceptanceResult ComputeFromValueLocation(
        const ValueLocationResult& valLocResult,
        // Price data (required)
        double close, double high, double low,
        double tickSize, int barIndex,
        // Volume data (required)
        double totalVolume,
        // Optional volume split
        double bidVolume = 0.0, double askVolume = 0.0,
        double delta = 0.0,
        // Rate data (optional)
        double volumePerSecond = 0.0
    ) {
        // Extract POC/VAH/VAL from SSOT result
        double poc = 0.0, vah = 0.0, val = 0.0;
        double priorPOC = 0.0, priorVAH = 0.0, priorVAL = 0.0;

        if (valLocResult.IsReady()) {
            // Derive prices from SSOT distance fields
            poc = close - (valLocResult.distFromPOCTicks * tickSize);
            vah = close - (valLocResult.distFromVAHTicks * tickSize);
            val = close - (valLocResult.distFromVALTicks * tickSize);

            // Prior levels from SSOT
            priorPOC = close - (valLocResult.distToPriorPOCTicks * tickSize);
            priorVAH = close - (valLocResult.distToPriorVAHTicks * tickSize);
            priorVAL = close - (valLocResult.distToPriorVALTicks * tickSize);
        }

        // Suppress deprecation warning: SSOT method calling deprecated method is expected
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
        // Delegate to full Compute() with extracted values
        return Compute(
            close, high, low,
            tickSize, barIndex,
            totalVolume,
            bidVolume, askVolume, delta,
            poc, vah, val,
            priorPOC, priorVAH, priorVAL,
            volumePerSecond
        );
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    }

    // =========================================================================
    // SESSION BOUNDARY METHODS
    // =========================================================================

    void ResetForSession() {
        pocTracker.Reset();
        vaTracker.Reset();

        confirmedState = AcceptanceState::UNKNOWN;
        candidateState = AcceptanceState::UNKNOWN;
        candidateConfirmationBars = 0;
        barsInConfirmedState = 0;

        sessionBars = 0;
        sessionTotalVolume = 0.0;
        sessionHighPrice = 0.0;
        sessionLowPrice = 0.0;
        barsAboveVA = 0;
        barsBelowVA = 0;
        barsInVA = 0;

        // Value build tracking
        consecutiveBarsOutside = 0;
        consecutiveBarsOutsideWithVol = 0;
        pocPriceHistory.clear();
        prevClose = 0.0;

        // Value build state hysteresis
        confirmedValueBuildState = ValueBuildState::UNKNOWN;
        candidateValueBuildState = ValueBuildState::UNKNOWN;
        valueBuildCandidateConfirmBars = 0;
        barsInConfirmedValueBuildState = 0;

        priceHistory.clear();

        // Note: baselines are NOT reset - they carry forward
    }

    void Reset() {
        ResetForSession();
        pocShiftBaseline.reset(300);
        volumeRatioBaseline.reset(300);
    }

    // =========================================================================
    // PRIOR SESSION SETUP
    // =========================================================================

    void SetPriorSessionLevels(double poc, double vah, double val) {
        vaTracker.SetPriorSession(vah, val);
    }

    // =========================================================================
    // PRE-WARM SUPPORT
    // =========================================================================

    void PreWarmFromBar(double volume, double poc, double pocShift) {
        if (std::isfinite(volume) && volume > 0.0) {
            volumeRatioBaseline.push(volume);
        }
        if (std::isfinite(pocShift)) {
            pocShiftBaseline.push(std::abs(pocShift));
        }
    }

    // =========================================================================
    // DIAGNOSTIC STATE
    // =========================================================================

    struct DiagnosticState {
        size_t volumeBaselineSamples = 0;
        size_t pocShiftBaselineSamples = 0;
        int sessionBars = 0;
        int barsAboveVA = 0;
        int barsBelowVA = 0;
        int barsInVA = 0;
        AcceptanceState confirmedState = AcceptanceState::UNKNOWN;
        int barsInState = 0;
        double pocMigrationTicks = 0.0;
        double vaOverlapPct = 1.0;
    };

    DiagnosticState GetDiagnosticState() const {
        DiagnosticState d;
        d.volumeBaselineSamples = volumeRatioBaseline.size();
        d.pocShiftBaselineSamples = pocShiftBaseline.size();
        d.sessionBars = sessionBars;
        d.barsAboveVA = barsAboveVA;
        d.barsBelowVA = barsBelowVA;
        d.barsInVA = barsInVA;
        d.confirmedState = confirmedState;
        d.barsInState = barsInConfirmedState;
        d.pocMigrationTicks = pocTracker.migrationTicks;
        d.vaOverlapPct = vaTracker.overlapPct;
        return d;
    }

private:
    // =========================================================================
    // VOLUME INTENSITY CLASSIFICATION
    // =========================================================================

    VolumeIntensity ClassifyVolumeIntensity(double percentile) const {
        if (percentile < config.veryLowThreshold) return VolumeIntensity::VERY_LOW;
        if (percentile < config.lowThreshold) return VolumeIntensity::LOW;
        if (percentile <= config.highThreshold) return VolumeIntensity::NORMAL;
        if (percentile <= config.veryHighThreshold) return VolumeIntensity::HIGH;
        if (percentile <= config.extremeThreshold) return VolumeIntensity::VERY_HIGH;
        if (percentile < config.shockThreshold) return VolumeIntensity::EXTREME;
        return VolumeIntensity::SHOCK;
    }

    // =========================================================================
    // VALUE BUILD METRICS COMPUTATION (Jan 2025)
    // =========================================================================
    // Computes POC behavior, VA behavior, holdOutside, and POC-price correlation.
    // These metrics enable ValueBuildEngine to classify BUILD/MIGRATE/STALL/FAIL.
    // =========================================================================

    void ComputeValueBuildMetrics(VolumeAcceptanceResult& result,
                                   double close, double tickSize,
                                   bool isOutsideVA, bool isHighVolume) {
        // =====================================================================
        // 1. Copy POC behavior from tracker
        // =====================================================================
        result.pocBehavior = pocTracker.behavior;
        result.pocBehaviorValid = pocTracker.behaviorValid;
        result.pocChangeVariance = pocTracker.changeVariance;
        result.pocDirectionPersistence = pocTracker.directionPersistence;
        result.pocReversalCount = pocTracker.reversalCount;

        // =====================================================================
        // 2. Copy VA behavior from tracker
        // =====================================================================
        result.vaBehavior = vaTracker.behavior;
        result.vaBehaviorValid = vaTracker.behaviorValid;
        result.vaMidpointShiftRate = vaTracker.midpointShiftRate;
        result.vaWidthChangeRate = vaTracker.widthChangeRate;

        // =====================================================================
        // 3. Update holdOutsideVA tracking
        // =====================================================================
        if (isOutsideVA) {
            consecutiveBarsOutside++;
            if (isHighVolume) {
                consecutiveBarsOutsideWithVol++;
            } else {
                // Reset volume count if low volume outside
                consecutiveBarsOutsideWithVol = 0;
            }
        } else {
            // Returned to value - reset counters
            consecutiveBarsOutside = 0;
            consecutiveBarsOutsideWithVol = 0;
        }

        result.barsOutsideVA = consecutiveBarsOutside;
        result.barsOutsideVAWithVolume = consecutiveBarsOutsideWithVol;
        result.isHoldingOutside = (consecutiveBarsOutsideWithVol >= HOLD_OUTSIDE_THRESHOLD);

        // =====================================================================
        // 4. Update POC-price correlation tracking
        // =====================================================================
        if (prevClose > 0.0 && result.priorPOC > 0.0 && result.currentPOC > 0.0) {
            POCPriceRecord rec;
            rec.pocChangeTicks = (result.currentPOC - result.priorPOC) / tickSize;
            rec.priceChangeTicks = (close - prevClose) / tickSize;

            pocPriceHistory.push_back(rec);
            while (pocPriceHistory.size() > MAX_POC_PRICE_HISTORY) {
                pocPriceHistory.pop_front();
            }

            // Compute correlation if enough history
            if (pocPriceHistory.size() >= 5) {
                result.pocPriceCorrelation = ComputePOCPriceCorrelation();

                // POC follows price if correlation is positive and significant
                result.pocFollowsPrice = (result.pocPriceCorrelation >= 0.3);
                result.pocRetreatsFromPrice = (result.pocPriceCorrelation <= -0.3);
            }
        }

        // Update prevClose for next bar
        prevClose = close;
    }

    // Compute Pearson correlation between POC changes and price changes
    double ComputePOCPriceCorrelation() const {
        if (pocPriceHistory.size() < 3) return 0.0;

        // Compute means
        double pocSum = 0.0, priceSum = 0.0;
        for (const auto& rec : pocPriceHistory) {
            pocSum += rec.pocChangeTicks;
            priceSum += rec.priceChangeTicks;
        }
        double n = static_cast<double>(pocPriceHistory.size());
        double pocMean = pocSum / n;
        double priceMean = priceSum / n;

        // Compute covariance and variances
        double cov = 0.0, pocVar = 0.0, priceVar = 0.0;
        for (const auto& rec : pocPriceHistory) {
            double pocDiff = rec.pocChangeTicks - pocMean;
            double priceDiff = rec.priceChangeTicks - priceMean;
            cov += pocDiff * priceDiff;
            pocVar += pocDiff * pocDiff;
            priceVar += priceDiff * priceDiff;
        }

        // Compute correlation
        double denom = std::sqrt(pocVar * priceVar);
        if (denom < 0.001) return 0.0;  // Avoid division by near-zero

        double corr = cov / denom;
        return (std::max)(-1.0, (std::min)(1.0, corr));
    }

    // =========================================================================
    // VALUE BUILD STATE COMPUTATION (Jan 2025)
    // =========================================================================
    // Computes BUILD/MIGRATE/STALL/FAIL state directly in VolumeAcceptanceEngine.
    // Uses hysteresis to prevent state whipsaw.
    // =========================================================================

    void ComputeValueBuildState(VolumeAcceptanceResult& result) {
        // Check if we have valid Value Build context
        if (!result.HasValueBuildContext()) {
            result.valueBuildValid = false;
            result.valueBuildState = ValueBuildState::UNKNOWN;
            return;
        }

        result.valueBuildValid = true;

        // =====================================================================
        // 1. Compute component scores
        // =====================================================================

        // BUILD score:
        //   - POC STABLE is strong positive
        //   - VA THICKENING is strong positive
        //   - Holding outside with volume is positive
        //   - High acceptance score is positive
        result.buildScore = 0.0;
        if (result.pocBehavior == POCBehavior::STABLE) result.buildScore += 0.35;
        if (result.vaBehavior == VABehavior::THICKENING) result.buildScore += 0.30;
        if (result.isHoldingOutside) result.buildScore += 0.20;
        if (result.acceptanceScore >= 0.6) result.buildScore += 0.15;

        // MIGRATE score:
        //   - POC DRIFTING is strong positive
        //   - VA SHIFTING is positive
        //   - POC follows price is strong positive
        //   - Migration direction aligned with POC drift is positive
        result.migrateScore = 0.0;
        if (result.pocBehavior == POCBehavior::DRIFTING) result.migrateScore += 0.30;
        if (result.vaBehavior == VABehavior::SHIFTING) result.migrateScore += 0.25;
        if (result.pocFollowsPrice) result.migrateScore += 0.30;
        if (result.pocMigrating) result.migrateScore += 0.15;

        // STALL score:
        //   - POC ERRATIC is strong positive
        //   - VA EXPANDING is strong positive
        //   - Low acceptance and rejection scores (indecision) is positive
        //   - In-value with low volume is positive
        result.stallScore = 0.0;
        if (result.pocBehavior == POCBehavior::ERRATIC) result.stallScore += 0.40;
        if (result.vaBehavior == VABehavior::EXPANDING) result.stallScore += 0.30;
        if (result.acceptanceScore < 0.4 && result.rejectionScore < 0.4) result.stallScore += 0.20;
        if (result.priceInVA && result.volumePercentile < 40.0) result.stallScore += 0.10;

        // FAIL score:
        //   - Fast return is strong positive
        //   - Rejection confirmed is strong positive
        //   - Not holding outside after being outside is positive
        //   - Low acceptance score is positive
        result.failScore = 0.0;
        if (result.fastReturn) result.failScore += 0.40;
        if (result.confirmedState == AcceptanceState::REJECTED) result.failScore += 0.30;
        if (!result.isHoldingOutside && result.barsOutsideVA > 0) result.failScore += 0.15;
        if (result.acceptanceScore < 0.3) result.failScore += 0.15;

        // Normalize scores to [0, 1]
        result.buildScore = (std::min)(1.0, (std::max)(0.0, result.buildScore));
        result.migrateScore = (std::min)(1.0, (std::max)(0.0, result.migrateScore));
        result.stallScore = (std::min)(1.0, (std::max)(0.0, result.stallScore));
        result.failScore = (std::min)(1.0, (std::max)(0.0, result.failScore));

        // =====================================================================
        // 2. Determine raw state from highest score
        // =====================================================================
        ValueBuildState rawState = DetermineRawValueBuildState(result);
        result.valueBuildState = rawState;

        // =====================================================================
        // 3. Apply hysteresis
        // =====================================================================
        UpdateValueBuildHysteresis(result, rawState);
    }

    ValueBuildState DetermineRawValueBuildState(const VolumeAcceptanceResult& result) const {
        // Priority-based classification (FAIL > STALL > MIGRATE > BUILD)
        // FAIL has highest priority (safety first)
        if (result.failScore >= 0.6) {
            return ValueBuildState::FAIL;
        }

        // STALL has second priority (uncertainty)
        if (result.stallScore >= 0.5) {
            return ValueBuildState::STALL;
        }

        // Between BUILD and MIGRATE, pick the higher score
        if (result.migrateScore >= 0.5 && result.migrateScore > result.buildScore) {
            return ValueBuildState::MIGRATE;
        }

        if (result.buildScore >= 0.5) {
            return ValueBuildState::BUILD;
        }

        // No clear winner - default to STALL (indecision)
        if (result.stallScore >= 0.3 || result.failScore >= 0.3) {
            return (result.stallScore >= result.failScore) ?
                   ValueBuildState::STALL : ValueBuildState::FAIL;
        }

        return ValueBuildState::UNKNOWN;
    }

    void UpdateValueBuildHysteresis(VolumeAcceptanceResult& result, ValueBuildState rawState) {
        result.valueBuildStateChanged = false;

        // Increment time in state
        barsInConfirmedValueBuildState++;

        // Check for state change
        if (rawState != confirmedValueBuildState) {
            if (rawState == candidateValueBuildState) {
                // Same candidate as before - increment confirmation
                valueBuildCandidateConfirmBars++;

                if (valueBuildCandidateConfirmBars >= VALUE_BUILD_MIN_CONFIRM_BARS) {
                    // Confirmed new state
                    confirmedValueBuildState = candidateValueBuildState;
                    barsInConfirmedValueBuildState = 0;
                    result.valueBuildStateChanged = true;
                }
            } else {
                // New candidate - start confirmation
                candidateValueBuildState = rawState;
                valueBuildCandidateConfirmBars = 1;
            }
        } else {
            // Raw matches confirmed - reset candidate
            candidateValueBuildState = confirmedValueBuildState;
            valueBuildCandidateConfirmBars = 0;
        }

        // Check for persistence timeout
        if (barsInConfirmedValueBuildState >= VALUE_BUILD_MAX_PERSISTENCE_BARS &&
            confirmedValueBuildState != ValueBuildState::UNKNOWN) {
            // State has persisted too long without refresh - consider decaying to UNKNOWN
            // For now, we just note it in transitioning flag
        }

        // Populate result
        result.confirmedValueBuildState = confirmedValueBuildState;
        result.candidateValueBuildState = candidateValueBuildState;
        result.valueBuildConfirmationBars = valueBuildCandidateConfirmBars;
        result.barsInValueBuildState = barsInConfirmedValueBuildState;
        result.valueBuildTransitioning = (candidateValueBuildState != confirmedValueBuildState);
    }

    // =========================================================================
    // REJECTION SIGNAL DETECTION
    // =========================================================================

    void DetectRejectionSignals(VolumeAcceptanceResult& result,
                                double close, double high, double low,
                                double tickSize, int barIndex) {
        // Low volume breakout
        if ((result.priceAboveVA || result.priceBelowVA) &&
            result.volumePercentile < config.lowVolumeBreakoutPctile) {
            result.lowVolumeBreakout = true;
        }

        // Fast return to value
        if (priceHistory.size() >= static_cast<size_t>(config.fastReturnBars + 1)) {
            // Check if we were outside VA and now back inside
            bool wasOutside = false;
            for (size_t i = priceHistory.size() - config.fastReturnBars - 1;
                 i < priceHistory.size() - 1; i++) {
                if (!priceHistory[i].inVA) {
                    wasOutside = true;
                    break;
                }
            }
            if (wasOutside && result.priceInVA) {
                result.fastReturn = true;
            }
        }

        // Wick rejection
        double range = high - low;
        if (range > 0.0) {
            double upperWick = high - (std::max)(close, low);  // Simplified
            double lowerWick = (std::min)(close, high) - low;

            // If price was above VA and has long upper wick = rejection
            if (result.priceAboveVA && (upperWick / range) > config.wickRejectionRatio) {
                result.wickRejection = true;
            }
            // If price was below VA and has long lower wick = rejection
            if (result.priceBelowVA && (lowerWick / range) > config.wickRejectionRatio) {
                result.wickRejection = true;
            }
        }

        // Delta rejection (delta opposite to price direction outside VA)
        if (result.priceAboveVA && result.deltaRatio < -config.deltaRejectionThreshold) {
            result.deltaRejection = true;  // Price up, delta negative
        }
        if (result.priceBelowVA && result.deltaRatio > config.deltaRejectionThreshold) {
            result.deltaRejection = true;  // Price down, delta positive
        }
    }

    // =========================================================================
    // ACCEPTANCE COMPONENT COMPUTATION
    // =========================================================================

    void ComputeAcceptanceComponents(VolumeAcceptanceResult& result,
                                     double close, double tickSize) {
        // Volume component: High volume = acceptance
        if (result.volumePercentile >= config.volumeConfirmationPctile) {
            result.volumeComponent = (result.volumePercentile - 50.0) / 50.0;
            result.volumeComponent = (std::min)(1.0, (std::max)(0.0, result.volumeComponent));
        } else if (result.volumePercentile < config.lowVolumeBreakoutPctile) {
            result.volumeComponent = 0.0;  // Low volume = no acceptance
        } else {
            result.volumeComponent = 0.5;  // Normal volume = neutral
        }

        // Price action component: Sustained move in direction
        if (result.priceAboveVA && barsAboveVA >= 3) {
            result.priceActionComponent = (std::min)(1.0, barsAboveVA / 10.0);
        } else if (result.priceBelowVA && barsBelowVA >= 3) {
            result.priceActionComponent = (std::min)(1.0, barsBelowVA / 10.0);
        } else {
            result.priceActionComponent = 0.3;  // In VA = lower acceptance
        }

        // Time component: More time at level = more acceptance
        double timeRatio = 0.0;
        if (result.priceAboveVA) {
            timeRatio = static_cast<double>(barsAboveVA) / (std::max)(1, sessionBars);
        } else if (result.priceBelowVA) {
            timeRatio = static_cast<double>(barsBelowVA) / (std::max)(1, sessionBars);
        } else {
            timeRatio = static_cast<double>(barsInVA) / (std::max)(1, sessionBars);
        }
        result.timeComponent = (std::min)(1.0, timeRatio * 2.0);

        // Delta component: Delta confirms direction
        if (result.priceAboveVA && result.deltaRatio > 0.3) {
            result.deltaComponent = (std::min)(1.0, result.deltaRatio);
        } else if (result.priceBelowVA && result.deltaRatio < -0.3) {
            result.deltaComponent = (std::min)(1.0, std::abs(result.deltaRatio));
        } else if (result.priceInVA) {
            result.deltaComponent = 0.5;  // Neutral
        } else {
            result.deltaComponent = 0.0;  // Delta divergence
        }

        // POC migration component: POC following price = acceptance
        if (result.pocMigrating) {
            bool migrationConfirms =
                (result.priceAboveVA && result.migrationDirection > 0) ||
                (result.priceBelowVA && result.migrationDirection < 0);
            result.pocMigrationComponent = migrationConfirms ? 0.8 : 0.2;
        } else {
            result.pocMigrationComponent = 0.4;  // Stable POC = neutral
        }
    }

    // =========================================================================
    // VALUE MIGRATION DETERMINATION
    // =========================================================================

    ValueMigration DetermineValueMigration(const VolumeAcceptanceResult& result) const {
        // Check POC migration as primary signal
        if (std::abs(pocTracker.migrationRate) >= config.pocMigrationRateThreshold) {
            if (pocTracker.migrationDirection > 0) {
                return ValueMigration::HIGHER;
            } else if (pocTracker.migrationDirection < 0) {
                return ValueMigration::LOWER;
            }
        }

        // Check VA expansion as secondary signal
        if (std::abs(result.vaExpansionTicks) >= config.vaExpansionMinTicks) {
            if (result.vaExpansionBias > 0) {
                return ValueMigration::HIGHER;
            } else if (result.vaExpansionBias < 0) {
                return ValueMigration::LOWER;
            }
        }

        // Balanced expansion = rotating
        if (result.vaExpansionTicks > config.vaExpansionMinTicks &&
            result.vaExpansionBias == 0) {
            return ValueMigration::ROTATING;
        }

        return ValueMigration::UNCHANGED;
    }

    // =========================================================================
    // ACCEPTANCE/REJECTION SCORE COMPUTATION
    // =========================================================================

    void ComputeAcceptanceScore(VolumeAcceptanceResult& result) const {
        result.acceptanceScore =
            config.weightVolume * result.volumeComponent +
            config.weightPriceAction * result.priceActionComponent +
            config.weightTime * result.timeComponent +
            config.weightDelta * result.deltaComponent +
            config.weightPOCMigration * result.pocMigrationComponent;

        result.acceptanceScore = (std::min)(1.0, (std::max)(0.0, result.acceptanceScore));
    }

    void ComputeRejectionScore(VolumeAcceptanceResult& result) const {
        double score = 0.0;
        int signals = 0;

        if (result.lowVolumeBreakout) { score += 0.3; signals++; }
        if (result.fastReturn) { score += 0.35; signals++; }
        if (result.wickRejection) { score += 0.25; signals++; }
        if (result.deltaRejection) { score += 0.2; signals++; }

        // Bonus for multiple rejection signals
        if (signals >= 2) {
            score *= 1.2;
        }

        result.rejectionScore = (std::min)(1.0, score);
    }

    // =========================================================================
    // RAW STATE DETERMINATION
    // =========================================================================

    AcceptanceState DetermineRawState(const VolumeAcceptanceResult& result) const {
        // If baseline not ready, unknown
        if (result.intensity == VolumeIntensity::UNKNOWN) {
            return AcceptanceState::UNKNOWN;
        }

        // Strong rejection signals override
        if (result.rejectionScore >= config.rejectionScoreThreshold) {
            return AcceptanceState::REJECTED;
        }

        // Strong acceptance
        if (result.acceptanceScore >= config.acceptanceScoreThreshold) {
            return AcceptanceState::ACCEPTED;
        }

        // At value edge = testing
        if ((result.priceAboveVA && std::abs(result.distanceToVAHticks) < 4.0) ||
            (result.priceBelowVA && std::abs(result.distanceToVALticks) < 4.0)) {
            return AcceptanceState::TESTING;
        }

        // Inside VA = typically accepted at current value
        if (result.priceInVA) {
            return AcceptanceState::ACCEPTED;
        }

        // Default to testing
        return AcceptanceState::TESTING;
    }

    // =========================================================================
    // HYSTERESIS
    // =========================================================================

    void UpdateHysteresis(VolumeAcceptanceResult& result, AcceptanceState rawState) {
        result.confirmedState = confirmedState;
        result.barsInState = barsInConfirmedState;

        // Initial state
        if (confirmedState == AcceptanceState::UNKNOWN &&
            rawState != AcceptanceState::UNKNOWN) {
            candidateState = rawState;
            candidateConfirmationBars = 1;
        }
        // Confirming candidate
        else if (rawState == candidateState && candidateState != confirmedState) {
            candidateConfirmationBars++;
            if (candidateConfirmationBars >= config.minConfirmationBars) {
                AcceptanceState prevConfirmed = confirmedState;
                confirmedState = candidateState;
                barsInConfirmedState = 1;

                // Track events
                if (confirmedState == AcceptanceState::ACCEPTED &&
                    prevConfirmed != AcceptanceState::ACCEPTED) {
                    result.acceptanceConfirmed = true;
                }
                if (confirmedState == AcceptanceState::REJECTED &&
                    prevConfirmed != AcceptanceState::REJECTED) {
                    result.rejectionConfirmed = true;
                }
                result.stateChanged = (prevConfirmed != confirmedState);
            }
        }
        // Reinforcing confirmed state
        else if (rawState == confirmedState) {
            barsInConfirmedState++;
            candidateState = confirmedState;
            candidateConfirmationBars = 0;
        }
        // New candidate
        else if (rawState != AcceptanceState::UNKNOWN) {
            candidateState = rawState;
            candidateConfirmationBars = 1;
            barsInConfirmedState++;
        }
        // Unknown state
        else {
            if (confirmedState != AcceptanceState::UNKNOWN) {
                barsInConfirmedState++;
                // Decay to unknown if too long without confirmation
                if (barsInConfirmedState > config.maxPersistenceBars) {
                    confirmedState = AcceptanceState::UNKNOWN;
                    barsInConfirmedState = 0;
                    result.stateChanged = true;
                }
            }
            candidateState = AcceptanceState::UNKNOWN;
            candidateConfirmationBars = 0;
        }

        result.confirmedState = confirmedState;
        result.candidateState = candidateState;
        result.confirmationBars = candidateConfirmationBars;
        result.barsInState = barsInConfirmedState;
        result.isTransitioning = (candidateState != confirmedState &&
                                  candidateConfirmationBars > 0);
    }

    // =========================================================================
    // CONFIRMATION COMPUTATION
    // =========================================================================

    void ComputeConfirmation(VolumeAcceptanceResult& result) const {
        // Base multiplier from state
        double multiplier = 1.0;

        switch (result.confirmedState) {
            case AcceptanceState::ACCEPTED:
                if (result.intensity == VolumeIntensity::HIGH ||
                    result.intensity == VolumeIntensity::VERY_HIGH) {
                    multiplier = config.acceptedHighVolumeMultiplier;
                } else {
                    multiplier = config.acceptedNormalVolumeMultiplier;
                }
                break;

            case AcceptanceState::TESTING:
                multiplier = config.testingMultiplier;
                break;

            case AcceptanceState::REJECTED:
                multiplier = config.rejectedMultiplier;
                break;

            default:
                multiplier = 1.0;
                break;
        }

        // Low volume penalty
        if (result.intensity == VolumeIntensity::LOW ||
            result.intensity == VolumeIntensity::VERY_LOW) {
            multiplier *= config.lowVolumeMultiplier;
        }

        result.confirmationMultiplier = multiplier;

        // Set confirmation requirements based on state
        result.confirmation.confidenceMultiplier = multiplier;
        result.confirmation.requiresHighVolume =
            (result.confirmedState == AcceptanceState::TESTING);
        result.confirmation.requiresAcceptance =
            (result.confirmedState == AcceptanceState::REJECTED);
        result.confirmation.allowsLowVolume =
            (result.confirmedState == AcceptanceState::ACCEPTED);
        result.confirmation.enhancedByVolume = true;
        result.confirmation.minVolumePercentile = config.volumeConfirmationPctile;
        result.confirmation.minAcceptanceScore = config.acceptanceScoreThreshold;
    }

    // =========================================================================
    // WARMUP CHECK
    // =========================================================================

    AcceptanceErrorReason CheckWarmupState() const {
        int notReady = 0;

        if (volumeRatioBaseline.size() < config.baselineMinSamples) notReady++;
        if (pocShiftBaseline.size() < config.baselineMinSamples / 2) notReady++;
        if (pocTracker.pocHistory.size() < static_cast<size_t>(config.pocHistoryMinBars)) notReady++;

        if (notReady > 1) {
            return AcceptanceErrorReason::WARMUP_MULTIPLE;
        }
        if (volumeRatioBaseline.size() < config.baselineMinSamples) {
            return AcceptanceErrorReason::WARMUP_VOLUME_BASELINE;
        }
        if (pocTracker.pocHistory.size() < static_cast<size_t>(config.pocHistoryMinBars)) {
            return AcceptanceErrorReason::WARMUP_POC_HISTORY;
        }

        return AcceptanceErrorReason::NONE;
    }
};

// ============================================================================
// VOLUME ACCEPTANCE DECISION INPUT (For BaselineDecisionGate Integration)
// ============================================================================
// Wrapper struct matching the pattern of other decision inputs.

struct VolumeAcceptanceDecisionInput {
    VolumeAcceptanceResult result;

    bool IsReady() const { return result.IsReady(); }
    bool IsWarmup() const { return result.IsWarmup(); }

    AcceptanceState GetState() const {
        return IsReady() ? result.confirmedState : AcceptanceState::UNKNOWN;
    }

    VolumeIntensity GetIntensity() const {
        return IsReady() ? result.intensity : VolumeIntensity::UNKNOWN;
    }

    ValueMigration GetMigration() const {
        return IsReady() ? result.migration : ValueMigration::UNKNOWN;
    }

    double GetConfirmationMultiplier() const {
        return IsReady() ? result.confirmationMultiplier : 1.0;
    }

    double GetAcceptanceScore() const {
        return IsReady() ? result.acceptanceScore : 0.0;
    }

    double GetVolumePercentile() const {
        return IsReady() ? result.volumePercentile : 50.0;
    }

    bool IsHighQualityAcceptance() const {
        return result.IsHighQualityAcceptance();
    }

    bool IsHighQualityRejection() const {
        return result.IsHighQualityRejection();
    }
};

} // namespace AMT
