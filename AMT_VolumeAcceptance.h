// ============================================================================
// AMT_VolumeAcceptance.h - Volume Acceptance Engine
// ============================================================================
//
// PURPOSE: Volume answers "was this move accepted by the market?"
//
//   1. Did volume support the move or reject it? (AcceptanceState)
//   2. Is value forming higher/lower or unchanged? (ValueMigrationState)
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
// VALUE MIGRATION STATE ENUM
// ============================================================================
// Is value forming higher, lower, or unchanged?
// This is the "value migration proxy" from POC and VA movement.
// ============================================================================

enum class ValueMigrationState : int {
    UNKNOWN = 0,          // Insufficient data
    UNCHANGED = 1,        // POC/VA stable, no migration
    MIGRATING_HIGHER = 2, // POC/VA shifting up
    MIGRATING_LOWER = 3,  // POC/VA shifting down
    ROTATING = 4          // VA expanding both directions (balance)
};

inline const char* ValueMigrationStateToString(ValueMigrationState s) {
    switch (s) {
        case ValueMigrationState::UNKNOWN:          return "UNKNOWN";
        case ValueMigrationState::UNCHANGED:        return "UNCHANGED";
        case ValueMigrationState::MIGRATING_HIGHER: return "HIGHER";
        case ValueMigrationState::MIGRATING_LOWER:  return "LOWER";
        case ValueMigrationState::ROTATING:         return "ROTATING";
    }
    return "UNK";
}

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
    VERY_HIGH = 5    // > P90 - extreme activity (institutional?)
};

inline const char* VolumeIntensityToString(VolumeIntensity v) {
    switch (v) {
        case VolumeIntensity::UNKNOWN:   return "UNKNOWN";
        case VolumeIntensity::VERY_LOW:  return "VERY_LOW";
        case VolumeIntensity::LOW:       return "LOW";
        case VolumeIntensity::NORMAL:    return "NORMAL";
        case VolumeIntensity::HIGH:      return "HIGH";
        case VolumeIntensity::VERY_HIGH: return "VERY_HIGH";
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
    }
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
    }

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
    ValueMigrationState migration = ValueMigrationState::UNKNOWN;
    double pocMigrationTicks = 0.0;      // POC shift from session open
    int migrationDirection = 0;          // +1=up, -1=down, 0=stable

    // Q3: What is 'high' volume today?
    VolumeIntensity intensity = VolumeIntensity::UNKNOWN;
    double volumePercentile = 50.0;      // Current bar vs baseline
    double volumeRatioToAvg = 1.0;       // Current / average

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
    // PRICE LOCATION CONTEXT
    // =========================================================================
    bool priceAboveVA = false;           // Current price > VAH
    bool priceBelowVA = false;           // Current price < VAL
    bool priceInVA = false;              // Price within VA
    double distanceToVAHticks = 0.0;     // Ticks to VAH
    double distanceToVALticks = 0.0;     // Ticks to VAL
    double distanceToPOCticks = 0.0;     // Ticks to POC

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
    // HYSTERESIS STATE
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
                            intensity == VolumeIntensity::VERY_HIGH);
    }

    bool IsLowVolume() const {
        return IsReady() && (intensity == VolumeIntensity::LOW ||
                            intensity == VolumeIntensity::VERY_LOW);
    }

    bool IsMigratingUp() const {
        return IsReady() && migration == ValueMigrationState::MIGRATING_HIGHER;
    }

    bool IsMigratingDown() const {
        return IsReady() && migration == ValueMigrationState::MIGRATING_LOWER;
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
    double veryHighThreshold = 90.0;     // > P90 = very high

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
        // STEP 15: CHECK WARMUP STATE
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
        return VolumeIntensity::VERY_HIGH;
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

    ValueMigrationState DetermineValueMigration(const VolumeAcceptanceResult& result) const {
        // Check POC migration as primary signal
        if (std::abs(pocTracker.migrationRate) >= config.pocMigrationRateThreshold) {
            if (pocTracker.migrationDirection > 0) {
                return ValueMigrationState::MIGRATING_HIGHER;
            } else if (pocTracker.migrationDirection < 0) {
                return ValueMigrationState::MIGRATING_LOWER;
            }
        }

        // Check VA expansion as secondary signal
        if (std::abs(result.vaExpansionTicks) >= config.vaExpansionMinTicks) {
            if (result.vaExpansionBias > 0) {
                return ValueMigrationState::MIGRATING_HIGHER;
            } else if (result.vaExpansionBias < 0) {
                return ValueMigrationState::MIGRATING_LOWER;
            }
        }

        // Balanced expansion = rotating
        if (result.vaExpansionTicks > config.vaExpansionMinTicks &&
            result.vaExpansionBias == 0) {
            return ValueMigrationState::ROTATING;
        }

        return ValueMigrationState::UNCHANGED;
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

    ValueMigrationState GetMigration() const {
        return IsReady() ? result.migration : ValueMigrationState::UNKNOWN;
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
