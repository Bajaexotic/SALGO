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

} // namespace AMT

#endif // AMT_HELPERS_H