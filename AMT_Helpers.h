// ============================================================================
// AMT_Helpers.h
// Time conversion and tick distance utilities (CANONICAL)
// All time and distance calculations MUST use these helpers
// ============================================================================

#ifndef AMT_HELPERS_H
#define AMT_HELPERS_H

// Only include sierrachart.h if not already mocked (for standalone testing)
#ifndef SIERRACHART_H
#include "sierrachart.h"
#endif

#include "amt_core.h"
#include <cmath>
#include <array>

namespace AMT {

// ============================================================================
// TIME UTILITIES - Mandatory Conversion Helpers
// ============================================================================

/**
 * SCDateTime stores time as fractional days since 1899-12-30.
 * Direct GetAsDouble() arithmetic is BANNED except in these helpers.
 * 
 * ROUNDING POLICY:
 *   - Age/expiration checks: FLOOR (conservative - keeps things alive longer)
 *   - Duration measurements: ROUND (intuitive)
 *   - Strict triggers: CEIL (conservative - triggers sooner)
 */

/**
 * Get elapsed seconds - FLOOR (default, for age checks)
 * Conservative: 1799.9 seconds → 1799 (not yet expired if limit is 1800)
 */
inline int GetElapsedSecondsFloor(SCDateTime start, SCDateTime end) {
    double daysDiff = end.GetAsDouble() - start.GetAsDouble();
    return static_cast<int>(daysDiff * 86400.0);  // Floor (truncate toward zero)
}

/**
 * Get elapsed seconds - ROUND (for duration measurements)
 * Intuitive: 59.6 seconds → 60 seconds
 */
inline int GetElapsedSecondsRound(SCDateTime start, SCDateTime end) {
    double daysDiff = end.GetAsDouble() - start.GetAsDouble();
    return static_cast<int>(std::round(daysDiff * 86400.0));
}

/**
 * Get elapsed seconds - CEIL (for strict triggers)
 * Conservative: 2.1 bars worth of time → 3 bars
 */
inline int GetElapsedSecondsCeil(SCDateTime start, SCDateTime end) {
    double daysDiff = end.GetAsDouble() - start.GetAsDouble();
    return static_cast<int>(std::ceil(daysDiff * 86400.0));
}

/**
 * DEFAULT: Use floor for most cases (age checks, expiration)
 */
inline int GetElapsedSeconds(SCDateTime start, SCDateTime end) {
    return GetElapsedSecondsFloor(start, end);
}

/**
 * Check if timestamp is older than threshold
 * Uses FLOOR (conservative - keeps things alive slightly longer)
 */
inline bool IsOlderThan(SCDateTime timestamp, SCDateTime now, int maxAgeSeconds) {
    int age = GetElapsedSecondsFloor(timestamp, now);
    return age > maxAgeSeconds;  // Strictly older than threshold
}

/**
 * Get duration (for display/logging)
 * Uses ROUND (intuitive for humans)
 */
inline int GetDurationSeconds(SCDateTime start, SCDateTime end) {
    return GetElapsedSecondsRound(start, end);
}

/**
 * Add seconds to timestamp
 */
inline SCDateTime AddSeconds(SCDateTime timestamp, int seconds) {
    double days = seconds / 86400.0;
    return timestamp + days;
}

// Note: For time formatting in Sierra Chart, use sc.FormatDateTime() or
// sc.DateTimeToString() from the study interface, not a standalone function.
// Example: sc.FormatDateTime(sc.BaseDateTimeIn[idx], formatString)

// ============================================================================
// TICK DISTANCE UTILITIES
// ============================================================================

/**
 * CANONICAL POLICY:
 * 
 * 1. OVERLAP DETECTION (finding zones): CEIL
 *    Goal: Never miss a relevant zone
 *    Example: 2.1 ticks → ceil(2.1) = 3, within 3-tick tolerance ✓
 * 
 * 2. PROXIMITY DETECTION (at zone trigger): EXACT (no rounding)
 *    Goal: Never false trigger, exact threshold enforcement
 *    Example: coreWidth = 3.0 ticks
 *             exactDist = 2.9 → 2.9 <= 3.0 → AT_ZONE ✓
 *             exactDist = 3.1 → 3.1 > 3.0 → NOT at zone ✓
 * 
 * 3. DISPLAY: ROUND
 *    Goal: Intuitive human reading
 *    Example: 2.6 ticks → 3 ticks (display only)
 * 
 * ONE SENTENCE SUMMARY:
 *   CEIL for inclusion, EXACT for triggering, ROUND for display.
 */

/**
 * OVERLAP: Find zones within tolerance (CEIL)
 * Conservative inclusion - never miss a zone
 */
inline int GetTickDistanceForOverlap(double price1, double price2, double tickSize) {
    double exactDist = std::fabs(price1 - price2) / tickSize;
    return static_cast<int>(std::ceil(exactDist));
}

/**
 * PROXIMITY: Exact distance for threshold checks (NO ROUNDING)
 * Exact threshold enforcement - no rounding paradoxes
 * NOTE: For tick-based SSOT, prefer GetTickDistanceFromTicks when both positions
 *       are already in tick format.
 */
inline double GetExactTickDistance(double price1, double price2, double tickSize) {
    return std::fabs(price1 - price2) / tickSize;
}

/**
 * SSOT: Integer tick distance (for tick-based calculations)
 * Use this when both positions are already in tick format.
 * Returns absolute difference in ticks.
 */
inline long long GetTickDistanceFromTicks(long long ticks1, long long ticks2) {
    return std::abs(ticks1 - ticks2);
}

/**
 * DISPLAY: Rounded for human reading (ROUND)
 * Intuitive display - 2.6 ticks → 3
 */
inline int GetTickDistanceForDisplay(double price1, double price2, double tickSize) {
    double exactDist = std::fabs(price1 - price2) / tickSize;
    return static_cast<int>(std::round(exactDist));
}

/**
 * Check if within tolerance (for zone finding)
 * Uses CEIL (overlap detection)
 */
inline bool IsWithinTicks(double price, double anchor, double tickSize, int toleranceTicks) {
    int dist = GetTickDistanceForOverlap(price, anchor, tickSize);
    return dist <= toleranceTicks;
}

// ============================================================================
// USAGE ENFORCEMENT
// ============================================================================

// BANNED PATTERNS (will cause bugs):
//
//   Direct SCDateTime arithmetic:
//     int age = currentTime.GetAsDouble() - startTime.GetAsDouble();
//     This gives AGE IN DAYS, not seconds!
//
//   Generic tick distance without context:
//     int dist = GetTickDistance(price, anchor, tickSize);  // Which rounding?
//
//   Using ROUND for trading decisions:
//     int dist = round(exactDist);
//     if (dist <= tolerance) { /* DON'T */ }
//
// CORRECT PATTERNS:
//
//   Time calculations:
//     int ageSeconds = GetElapsedSeconds(startTime, currentTime);
//     if (ageSeconds > maxAgeSeconds) { ... }
//
//   Zone finding (CEIL):
//     int distCeil = GetTickDistanceForOverlap(price, anchor, tickSize);
//     if (distCeil <= tolerance) { /* found */ }
//
//   Zone triggering (EXACT):
//     double distExact = GetExactTickDistance(price, anchor, tickSize);
//     if (distExact <= coreWidth) { /* at zone */ }
//
//   Display (ROUND):
//     int distDisplay = GetTickDistanceForDisplay(price, anchor, tickSize);
//     sc.AddMessageToLog(SCString().Format("Distance: %d ticks", distDisplay), 0);

// ============================================================================
// ADDITIONAL UTILITY FUNCTIONS
// ============================================================================

/**
 * Convert SCDateTime to seconds since midnight
 */
inline int TimeToSeconds(const SCDateTime& dt) {
    return dt.GetHour() * 3600 + dt.GetMinute() * 60 + dt.GetSecond();
}

// NOTE: PriceToTicks() moved to AMT_config.h as canonical SSOT
// Use AMT::PriceToTicks(price, tickSize) from AMT_config.h

/**
 * Safe array access with fallback
 */
inline double SafeGetAt(const SCFloatArray& a, int idx, double fallback = 0.0) {
    if (idx < 0 || idx >= a.GetArraySize())
        return fallback;
    return a[idx];
}

/**
 * Validate price is finite and positive
 */
inline bool IsValidPrice(double p) {
    return std::isfinite(p) && std::fabs(p) > 1e-12;
}

/**
 * Calculate auction facilitation state (Percentile-Based, Primary SSOT)
 *
 * Classification based on effort (volume) vs progress (range) relationship:
 *   - LABORED: High effort (>=highPctl) with low progress (<=lowPctl)
 *   - INEFFICIENT: Low effort (<=lowPctl) with high slippage (>=highPctl)
 *   - FAILED: Extreme low effort (<=extremePctl) AND low range (<=extremePctl)
 *   - EFFICIENT: All other conditions
 *
 * @param volPctile    Percentile rank of current bar volume [0-100]
 * @param rangePctile  Percentile rank of current bar range [0-100]
 * @param highPctl     Upper quartile threshold (default 75.0)
 * @param lowPctl      Lower quartile threshold (default 25.0)
 * @param extremePctl  Extreme tail threshold (default 10.0)
 * @return AuctionFacilitation enum value
 */
inline AuctionFacilitation CalculateFacilitation(
    double volPctile,
    double rangePctile,
    double highPctl,
    double lowPctl,
    double extremePctl)
{
    // LABORED: High effort, low progress (market absorbing, not moving)
    if (volPctile >= highPctl && rangePctile <= lowPctl)
        return AuctionFacilitation::LABORED;

    // INEFFICIENT: Low effort, high movement (thin market, slippage risk)
    if (volPctile <= lowPctl && rangePctile >= highPctl)
        return AuctionFacilitation::INEFFICIENT;

    // FAILED: Very low effort AND range (auction stalling, no participation)
    if (volPctile <= extremePctl && rangePctile <= extremePctl)
        return AuctionFacilitation::FAILED;

    // EFFICIENT: Normal conditions
    return AuctionFacilitation::EFFICIENT;
}

// ============================================================================
// FACILITATION AGGREGATOR - Synthetic Bar Aggregation for Facilitation
// ============================================================================
// Aggregates N 1-minute bars of vol_sec values for regime-level facilitation.
// Matches VolatilityEngine's synthetic bar pattern.
//
// PURPOSE:
//   - Facilitation is a regime-level concept, not minute-level noise
//   - 1-min vol_sec has high variance due to micro-bursts
//   - Aggregating to 5-min synthetic periods provides smoother signal
//
// DESIGN:
//   - Rolling window of N bars (configurable, default: 5 for 5-min equivalent)
//   - Synthetic vol_sec = mean(vol_sec) over window
//   - Synthetic range from VolatilityEngine's aggregator (shared)
//   - Signals when new synthetic bar forms (every N raw bars)
// ============================================================================

class FacilitationAggregator {
public:
    static constexpr int MAX_AGGREGATION_BARS = 15;
    static constexpr int DEFAULT_AGGREGATION_BARS = 5;  // 5-min equivalent on 1-min chart

private:
    std::array<double, MAX_AGGREGATION_BARS> volSecBuffer_;
    int writeIdx_ = 0;
    int validCount_ = 0;
    int aggregationBars_ = DEFAULT_AGGREGATION_BARS;

    // Cached synthetic values
    double syntheticVolSec_ = 0.0;
    bool cacheValid_ = false;

    // Synthetic bar boundary tracking
    int rawBarCounter_ = 0;
    bool newSyntheticBarFormed_ = false;

public:
    FacilitationAggregator() {
        for (auto& v : volSecBuffer_) v = 0.0;
    }

    /**
     * Set number of bars to aggregate.
     * @param bars Number of 1-min bars per synthetic period (1-15)
     */
    void SetAggregationBars(int bars) {
        aggregationBars_ = (std::max)(1, (std::min)(bars, MAX_AGGREGATION_BARS));
    }

    int GetAggregationBars() const { return aggregationBars_; }

    /**
     * Push a new raw bar's vol_sec value.
     * @return true if this bar completes a new synthetic bar
     */
    bool Push(double volSec) {
        volSecBuffer_[writeIdx_] = volSec;
        writeIdx_ = (writeIdx_ + 1) % aggregationBars_;
        if (validCount_ < aggregationBars_) validCount_++;

        rawBarCounter_++;
        bool boundaryReached = (rawBarCounter_ % aggregationBars_ == 0);

        ComputeSynthetic();
        newSyntheticBarFormed_ = boundaryReached;
        return newSyntheticBarFormed_;
    }

    /** Check if we have enough bars to produce valid synthetic data. */
    bool IsReady() const {
        return validCount_ >= aggregationBars_;
    }

    /** Check if a new synthetic bar was just formed on the last Push(). */
    bool DidNewSyntheticBarForm() const {
        return newSyntheticBarFormed_;
    }

    /** Get synthetic vol_sec (mean over window). */
    double GetSyntheticVolSec() const {
        return cacheValid_ ? syntheticVolSec_ : 0.0;
    }

    /** Reset aggregator state (call at session start). */
    void Reset() {
        for (auto& v : volSecBuffer_) v = 0.0;
        writeIdx_ = 0;
        validCount_ = 0;
        syntheticVolSec_ = 0.0;
        cacheValid_ = false;
        rawBarCounter_ = 0;
        newSyntheticBarFormed_ = false;
    }

private:
    void ComputeSynthetic() {
        if (validCount_ < aggregationBars_) {
            cacheValid_ = false;
            return;
        }

        double sum = 0.0;
        for (int i = 0; i < aggregationBars_; i++) {
            sum += volSecBuffer_[i];
        }
        syntheticVolSec_ = sum / aggregationBars_;
        cacheValid_ = true;
    }
};

// ============================================================================
// FACILITATION TRACKER - Temporal Persistence for Facilitation States
// ============================================================================

/**
 * FacilitationTracker - Adds temporal persistence to stateless facilitation
 *
 * DESIGN PRINCIPLES:
 * 1. Asymmetric hysteresis: Danger states (FAILED, LABORED) enter fast (1 bar),
 *    calm state (EFFICIENT) requires confirmation (2 bars)
 * 2. Persistence tracking: How many bars in confirmed state
 * 3. Transition detection: Events for state changes
 * 4. Outside core logic: Wraps CalculateFacilitation(), doesn't modify it
 *
 * TRADING IMPLICATIONS:
 * - Sustained LABORED (5+ bars): Absorption zone forming, reversal setup
 * - Sustained FAILED (10+ bars): Dead market (lunch, holiday)
 * - LABORED -> EFFICIENT transition: Absorption exhausted, breakout potential
 * - EFFICIENT -> LABORED transition: Market hitting resistance/support
 */
struct FacilitationTracker {
    // Hysteresis configuration
    // Danger signals (FAILED, LABORED, INEFFICIENT) enter fast - react quickly
    // Calm signal (EFFICIENT) exits slow - confirm problem is really resolved
    static constexpr int DANGER_CONFIRM_BARS = 1;   // Enter FAILED/LABORED/INEFFICIENT immediately
    static constexpr int CALM_CONFIRM_BARS = 2;     // Require 2 bars to confirm EFFICIENT

    // Persistence thresholds for downstream consumers
    static constexpr int LABORED_PERSISTENT_BARS = 5;   // Absorption zone likely forming
    static constexpr int FAILED_PERSISTENT_BARS = 10;   // Dead market confirmed
    static constexpr int INEFFICIENT_PERSISTENT_BARS = 3; // Sustained vacuum/gap risk

    // Current state
    AuctionFacilitation confirmedState = AuctionFacilitation::UNKNOWN;
    AuctionFacilitation candidateState = AuctionFacilitation::UNKNOWN;
    int barsInCandidate = 0;
    int barsInConfirmed = 0;

    // Transition tracking
    bool stateJustChanged = false;
    AuctionFacilitation priorConfirmedState = AuctionFacilitation::UNKNOWN;
    int lastTransitionBar = -1;

    // Raw state for diagnostics
    AuctionFacilitation lastRawState = AuctionFacilitation::UNKNOWN;
    double lastVolPctile = 0.0;
    double lastRangePctile = 0.0;

    /**
     * Reset tracker to initial state (call at session start)
     */
    void Reset() {
        confirmedState = AuctionFacilitation::UNKNOWN;
        candidateState = AuctionFacilitation::UNKNOWN;
        barsInCandidate = 0;
        barsInConfirmed = 0;
        stateJustChanged = false;
        priorConfirmedState = AuctionFacilitation::UNKNOWN;
        lastTransitionBar = -1;
        lastRawState = AuctionFacilitation::UNKNOWN;
        lastVolPctile = 0.0;
        lastRangePctile = 0.0;
    }

    /**
     * Get confirmation bars required for a state transition
     * Asymmetric: danger enters fast, calm exits slow (only from danger states)
     */
    int GetConfirmationBars(AuctionFacilitation targetState) const {
        switch (targetState) {
            case AuctionFacilitation::FAILED:
            case AuctionFacilitation::LABORED:
            case AuctionFacilitation::INEFFICIENT:
                return DANGER_CONFIRM_BARS;  // Enter danger fast
            case AuctionFacilitation::EFFICIENT:
                // Only require 2 bars when exiting danger state
                // From UNKNOWN (init), confirm immediately
                if (confirmedState == AuctionFacilitation::LABORED ||
                    confirmedState == AuctionFacilitation::FAILED ||
                    confirmedState == AuctionFacilitation::INEFFICIENT) {
                    return CALM_CONFIRM_BARS;  // Exit danger slow
                }
                return 1;  // Not exiting danger, confirm immediately
            default:
                return 1;
        }
    }

    /**
     * Update tracker with new raw facilitation state
     * @param rawState    State from CalculateFacilitation()
     * @param currentBar  Current bar index (for transition tracking)
     * @param volPctile   Volume percentile (for diagnostics)
     * @param rangePctile Range percentile (for diagnostics)
     */
    void Update(AuctionFacilitation rawState, int currentBar,
                double volPctile = 0.0, double rangePctile = 0.0) {
        // Store diagnostics
        lastRawState = rawState;
        lastVolPctile = volPctile;
        lastRangePctile = rangePctile;

        // Reset transition flag
        stateJustChanged = false;

        // Handle UNKNOWN specially - propagate immediately, no hysteresis
        if (rawState == AuctionFacilitation::UNKNOWN) {
            if (confirmedState != AuctionFacilitation::UNKNOWN) {
                priorConfirmedState = confirmedState;
                stateJustChanged = true;
                lastTransitionBar = currentBar;
            }
            confirmedState = AuctionFacilitation::UNKNOWN;
            candidateState = AuctionFacilitation::UNKNOWN;
            barsInCandidate = 0;
            barsInConfirmed = 0;
            return;
        }

        // If raw state matches confirmed state, just increment persistence
        if (rawState == confirmedState) {
            barsInConfirmed++;
            candidateState = rawState;
            barsInCandidate = 0;  // No pending transition
            return;
        }

        // Raw state differs from confirmed - check hysteresis
        if (rawState == candidateState) {
            // Same candidate as before, increment count
            barsInCandidate++;
        } else {
            // New candidate, reset count
            candidateState = rawState;
            barsInCandidate = 1;
        }

        // Check if candidate has enough confirmation
        const int requiredBars = GetConfirmationBars(candidateState);
        if (barsInCandidate >= requiredBars) {
            // Transition confirmed
            priorConfirmedState = confirmedState;
            confirmedState = candidateState;
            barsInConfirmed = barsInCandidate;  // Include confirmation bars
            stateJustChanged = true;
            lastTransitionBar = currentBar;
        }
    }

    // ========================================================================
    // Query Helpers
    // ========================================================================

    /** Is the tracker in a valid (non-UNKNOWN) confirmed state? */
    bool IsReady() const {
        return confirmedState != AuctionFacilitation::UNKNOWN;
    }

    /** Did state just change this bar? */
    bool JustChanged() const {
        return stateJustChanged;
    }

    /** Did we just enter a specific state? */
    bool JustEntered(AuctionFacilitation state) const {
        return stateJustChanged && confirmedState == state;
    }

    /** Did we just exit a specific state? */
    bool JustExited(AuctionFacilitation state) const {
        return stateJustChanged && priorConfirmedState == state;
    }

    /** Is the confirmed state one of the "danger" states? */
    bool IsDangerState() const {
        return confirmedState == AuctionFacilitation::FAILED ||
               confirmedState == AuctionFacilitation::LABORED ||
               confirmedState == AuctionFacilitation::INEFFICIENT;
    }

    /** Has the current state persisted for at least N bars? */
    bool IsPersistent(int minBars) const {
        return barsInConfirmed >= minBars;
    }

    /** Is LABORED state persistent (absorption zone likely)? */
    bool IsLaboredPersistent() const {
        return confirmedState == AuctionFacilitation::LABORED &&
               barsInConfirmed >= LABORED_PERSISTENT_BARS;
    }

    /** Is FAILED state persistent (dead market confirmed)? */
    bool IsFailedPersistent() const {
        return confirmedState == AuctionFacilitation::FAILED &&
               barsInConfirmed >= FAILED_PERSISTENT_BARS;
    }

    /** Is INEFFICIENT state persistent (sustained vacuum risk)? */
    bool IsInefficientPersistent() const {
        return confirmedState == AuctionFacilitation::INEFFICIENT &&
               barsInConfirmed >= INEFFICIENT_PERSISTENT_BARS;
    }

    /**
     * Get state name with persistence count for logging
     * @param buf    Buffer to write to
     * @param bufLen Buffer length
     * @return Pointer to buf with format "STATE(N)" e.g. "LABORED(5)"
     */
    const char* GetStateWithPersistence(char* buf, size_t bufLen) const {
        const char* stateName = "UNKNOWN";
        switch (confirmedState) {
            case AuctionFacilitation::EFFICIENT:    stateName = "EFFICIENT"; break;
            case AuctionFacilitation::LABORED:      stateName = "LABORED"; break;
            case AuctionFacilitation::INEFFICIENT:  stateName = "INEFFICIENT"; break;
            case AuctionFacilitation::FAILED:       stateName = "FAILED"; break;
            default: break;
        }
        snprintf(buf, bufLen, "%s(%d)", stateName, barsInConfirmed);
        return buf;
    }
};

/**
 * Determine exact session phase from time
 * SSOT for session/phase classification using half-open intervals [start, end)
 */
inline SessionPhase DetermineExactPhase(
    int tSec,           // Current time in seconds from midnight
    int rthStartSec,    // RTH start (inclusive), e.g., 34200 for 09:30:00
    int rthEndSec,      // RTH end (EXCLUSIVE), e.g., 58500 for 16:15:00
    int gbxStartSec = 0 // DEPRECATED: Unused parameter, kept for signature compatibility only.
                        // GLOBEX start is hardcoded to MAINTENANCE_END_SEC (18:00).
                        // DO NOT rely on this parameter - it is discarded.
)
{
    (void)gbxStartSec;  // Parameter is unused - GLOBEX start is hardcoded to 18:00

    // RTH phases: [rthStartSec, rthEndSec)
    if (tSec >= rthStartSec && tSec < rthEndSec)
    {
        // Use direct second boundaries to avoid integer division truncation artifacts
        const int ibEndSec = rthStartSec + (Thresholds::PHASE_IB_COMPLETE * 60);      // 10:30:00
        const int closingStartSec = rthEndSec - (Thresholds::PHASE_CLOSING_WINDOW * 60); // 15:30:00

        // INITIAL_BALANCE = first 60 min [09:30:00, 10:30:00)
        if (tSec < ibEndSec)
            return SessionPhase::INITIAL_BALANCE;

        // CLOSING_SESSION = last 45 min [15:30:00, 16:15:00)
        if (tSec >= closingStartSec)
            return SessionPhase::CLOSING_SESSION;

        // MID_SESSION = everything in between [10:30:00, 15:30:00)
        return SessionPhase::MID_SESSION;
    }

    // EVENING phases: [rthEndSec, rthStartSec) wraps midnight

    // POST_CLOSE = [16:15, 17:00)
    if (tSec >= rthEndSec && tSec < Thresholds::POST_CLOSE_END_SEC)
        return SessionPhase::POST_CLOSE;

    // MAINTENANCE = [17:00, 18:00)
    if (tSec >= Thresholds::POST_CLOSE_END_SEC && tSec < Thresholds::MAINTENANCE_END_SEC)
        return SessionPhase::MAINTENANCE;

    // GLOBEX = [18:00, 03:00) - wraps midnight
    if (tSec >= Thresholds::MAINTENANCE_END_SEC || tSec < Thresholds::LONDON_OPEN_SEC)
        return SessionPhase::GLOBEX;

    // LONDON_OPEN = [03:00, 08:30)
    if (tSec >= Thresholds::LONDON_OPEN_SEC && tSec < Thresholds::PRE_MARKET_START_SEC)
        return SessionPhase::LONDON_OPEN;

    // PRE_MARKET = [08:30, 09:30)
    if (tSec >= Thresholds::PRE_MARKET_START_SEC && tSec < rthStartSec)
        return SessionPhase::PRE_MARKET;

    // Fallback (should not reach)
    return SessionPhase::UNKNOWN;
}

/**
 * DetermineSessionPhase - PREFERRED WRAPPER (drift-proof)
 *
 * Accepts INCLUSIVE RTH end time (as stored in sc.Input[1], e.g., 58499 for 16:14:59)
 * and internally adds +1 to convert to EXCLUSIVE boundary for DetermineExactPhase.
 *
 * This wrapper makes drift structurally impossible by:
 * 1. Encapsulating the +1 conversion in a single location
 * 2. Accepting the same value stored in sc.Input[1] without modification
 * 3. Making the boundary contract explicit in the function name and doc
 *
 * @param tSec          Current time in seconds from midnight
 * @param rthStartSec   RTH start (INCLUSIVE), e.g., 34200 for 09:30:00
 * @param rthEndSecIncl RTH end (INCLUSIVE), e.g., 58499 for 16:14:59 (from sc.Input[1])
 * @return SessionPhase for the given time
 */
inline SessionPhase DetermineSessionPhase(
    int tSec,
    int rthStartSec,
    int rthEndSecIncl  // INCLUSIVE end - the actual last RTH second
)
{
    // Convert INCLUSIVE end to EXCLUSIVE end for internal half-open interval logic
    const int rthEndSecExcl = rthEndSecIncl + 1;
    return DetermineExactPhase(tSec, rthStartSec, rthEndSecExcl, rthEndSecExcl);
}

// ============================================================================
// CLOSED BAR DETECTION (Jan 2025)
// ============================================================================
// Sierra Chart's GetBarHasClosedStatus() has a limitation: for the LAST bar
// in the chart (ArraySize - 1), it always returns BHCS_BAR_HAS_NOT_CLOSED
// regardless of whether the bar has actually closed in real-time.
//
// This happens because no new bar exists to push the previous bar into
// "closed" status. Common scenario: market goes to maintenance, last bar
// never gets a successor.
//
// Solution: For time-based charts, compare current time against bar end time.
// For non-time-based charts (volume, range, etc.), rely on bar status.
// ============================================================================

/**
 * Result of closed bar detection.
 * Contains the index of the most recent fully closed bar and validity flags.
 */
struct ClosedBarInfo {
    int index = -1;              // Index of closed bar (-1 if none)
    bool valid = false;          // True if we have a valid closed bar
    bool isLastBar = false;      // True if the closed bar IS the last bar in chart
    bool usedTimeCheck = false;  // True if time-based close detection was used

    bool IsValid() const { return valid && index >= 0; }
};

// The following functions require full Sierra Chart types (s_sc, n_ACSIL::s_BarPeriod)
// They are only available when compiling with sierrachart.h, not in standalone tests.
// Use AMT_STANDALONE_TEST guard since tests may define SIERRACHART_H for mocking.
#ifndef AMT_STANDALONE_TEST

/**
 * Check if chart uses time-based bars (minutes, seconds, days).
 * Time-based bars have predictable close times; other types (volume, range,
 * tick, renko) close when their condition is met.
 */
inline bool IsTimeBasedChart(const n_ACSIL::s_BarPeriod& barPeriod) {
    return (barPeriod.IntradayChartBarPeriodType == IBPT_DAYS_MINS_SECS);
}

/**
 * Get the expected end time of a specific bar for time-based charts.
 * Returns the DateTime when the bar SHOULD close (bar start + bar duration).
 *
 * @param barStartTime  Start time of the bar (from sc.BaseDateTimeIn[barIdx])
 * @param barPeriod     Bar period parameters (from sc.GetBarPeriodParameters)
 * @return Expected close time as SCDateTime
 */
inline SCDateTime GetBarExpectedEndTime(
    SCDateTime barStartTime,
    const n_ACSIL::s_BarPeriod& barPeriod)
{
    // IntradayChartBarPeriodParameter1 = seconds per bar for time-based charts
    const int barDurationSeconds = barPeriod.IntradayChartBarPeriodParameter1;
    if (barDurationSeconds <= 0) {
        return barStartTime;  // Invalid - return start time (will fail time check)
    }

    // Add bar duration to start time
    return AddSeconds(barStartTime, barDurationSeconds);
}

/**
 * Determine the most recent fully closed bar.
 *
 * CRITICAL: This function correctly handles the "last bar" edge case where
 * GetBarHasClosedStatus always returns NOT_CLOSED because no successor bar exists.
 *
 * For time-based charts (1-min, 5-min, etc.):
 *   - For non-last bars: standard curBarIdx - 1
 *   - For last bar: check if current time >= bar end time
 *
 * For non-time-based charts (volume, range, tick, renko):
 *   - Bars close when their condition is met, so standard check works
 *   - Fall back to curBarIdx - 1 (last bar assumed still forming)
 *
 * @param sc           Sierra Chart interface
 * @param curBarIdx    Current bar index being processed
 * @param barPeriod    Bar period (call sc.GetBarPeriodParameters once, cache it)
 * @return ClosedBarInfo with the closed bar index and validity
 */
inline ClosedBarInfo GetClosedBarInfo(
    s_sc& sc,
    int curBarIdx,
    const n_ACSIL::s_BarPeriod& barPeriod)
{
    ClosedBarInfo result;

    if (curBarIdx < 0 || sc.ArraySize <= 0) {
        return result;  // Invalid state
    }

    const int lastBarIdx = sc.ArraySize - 1;
    const bool isOnLastBar = (curBarIdx == lastBarIdx);

    // Standard case: not on last bar, previous bar is definitely closed
    if (!isOnLastBar && curBarIdx > 0) {
        result.index = curBarIdx - 1;
        result.valid = true;
        result.isLastBar = false;
        result.usedTimeCheck = false;
        return result;
    }

    // Edge case: we ARE on the last bar
    // For time-based charts, check if current time exceeds bar end time
    if (isOnLastBar && IsTimeBasedChart(barPeriod)) {
        SCDateTime barStart = sc.BaseDateTimeIn[lastBarIdx];
        SCDateTime barEnd = GetBarExpectedEndTime(barStart, barPeriod);
        SCDateTime currentTime = sc.GetCurrentDateTime();

        if (currentTime >= barEnd) {
            // Last bar HAS closed (time exceeded)
            result.index = lastBarIdx;
            result.valid = true;
            result.isLastBar = true;
            result.usedTimeCheck = true;
            return result;
        } else {
            // Last bar is still forming - use previous bar if available
            if (lastBarIdx > 0) {
                result.index = lastBarIdx - 1;
                result.valid = true;
                result.isLastBar = false;
                result.usedTimeCheck = false;
            }
            return result;
        }
    }

    // Non-time-based charts or edge cases: use previous bar
    if (curBarIdx > 0) {
        result.index = curBarIdx - 1;
        result.valid = true;
        result.isLastBar = false;
        result.usedTimeCheck = false;
    }

    return result;
}

/**
 * Simplified overload for common case.
 * Calls sc.GetBarPeriodParameters internally - use the caching version above
 * in hot paths to avoid repeated API calls.
 */
inline ClosedBarInfo GetClosedBarInfo(s_sc& sc, int curBarIdx) {
    n_ACSIL::s_BarPeriod barPeriod;
    sc.GetBarPeriodParameters(barPeriod);
    return GetClosedBarInfo(sc, curBarIdx, barPeriod);
}

#endif // AMT_STANDALONE_TEST (end SC-dependent closed bar detection functions)

} // namespace AMT

#endif // AMT_HELPERS_H