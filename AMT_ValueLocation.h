// ============================================================================
// AMT_ValueLocation.h
// Value-Location/Structure Engine
// ============================================================================
//
// PURPOSE: This engine answers "Where am I relative to value and structure?"
//
//   1. Where am I relative to value? (ValueZone classification)
//   2. Am I in balance or imbalance structurally? (VA overlap, range development)
//   3. What session context applies? (Session phase, IB status)
//   4. What nearby reference levels matter? (Prior levels, IB, HVN/LVN)
//   5. How does location gate strategies? (Fade in value, breakout from balance)
//
// DESIGN PRINCIPLES:
//   - DELEGATE, DON'T DUPLICATE: Aggregates existing SSOT data
//   - Uses ZoneManager.GetStrongestZoneAtPrice() for nearest zone
//   - Uses StructureTracker for session/IB extremes
//   - Uses existing ValueZone (9-state), ValueMigration, LevelType enums
//   - Phase-aware context (GLOBEX != RTH)
//   - NO-FALLBACK contract: explicit validity at every decision point
//   - ZERO Sierra Chart dependencies (testable standalone)
//   - Hysteresis for location state transitions
//
// ============================================================================

#ifndef AMT_VALUELOCATION_H
#define AMT_VALUELOCATION_H

#include "amt_core.h"
#include "AMT_Zones.h"  // For ZoneManager, StructureTracker, ZoneProximity
#include <vector>
#include <algorithm>
#include <cmath>
#include <string>

namespace AMT {

// ============================================================================
// NOTE: ValueZone, ValueZoneToString(), and ValueZoneToValueAreaRegion()
// are defined in amt_core.h (SSOT location for all core enums)
// ============================================================================

// ============================================================================
// VA OVERLAP STATE (Balance vs Separation)
// ============================================================================
// Classifies the structural relationship between current and prior VA.
// Key for determining balance (fade extremes) vs trend (follow direction).
// ============================================================================

enum class VAOverlapState : int {
    UNKNOWN = 0,
    SEPARATED_ABOVE = 1,      // Current VA entirely above prior (< 30% overlap)
    SEPARATED_BELOW = 2,      // Current VA entirely below prior (< 30% overlap)
    OVERLAPPING = 3,          // Significant overlap (> 50%) - balance/rotation
    CONTAINED = 4,            // Current VA inside prior VA (contraction)
    EXPANDING = 5             // Current VA wider than prior (expansion/trend development)
};

inline const char* VAOverlapStateToString(VAOverlapState state) {
    switch (state) {
        case VAOverlapState::UNKNOWN:         return "UNKNOWN";
        case VAOverlapState::SEPARATED_ABOVE: return "SEP_ABOVE";
        case VAOverlapState::SEPARATED_BELOW: return "SEP_BELOW";
        case VAOverlapState::OVERLAPPING:     return "OVERLAP";
        case VAOverlapState::CONTAINED:       return "CONTAINED";
        case VAOverlapState::EXPANDING:       return "EXPANDING";
        default:                              return "?";
    }
}

// ============================================================================
// VALUE LOCATION ERROR REASON
// ============================================================================
// Explicit error taxonomy following NO-FALLBACK contract.
// ============================================================================

enum class ValueLocationErrorReason : int {
    NONE = 0,

    // Warmup states (expected during initialization)
    WARMUP_PROFILE = 10,      // Profile not mature (insufficient bars/levels)
    WARMUP_PRIOR = 11,        // Prior session not available
    WARMUP_IB = 12,           // IB not complete (first 60 min RTH)
    WARMUP_MULTIPLE = 13,     // Multiple warmups needed

    // Validation errors
    ERR_INVALID_PRICE = 20,   // Price data invalid (zero, negative, NaN)
    ERR_INVALID_VA = 21,      // VAH <= VAL (inverted value area)
    ERR_INVALID_TICK = 22,    // Tick size <= 0

    // Configuration errors
    ERR_NO_ZONE_MGR = 30,     // ZoneManager not provided
    ERR_INVALID_PHASE = 31    // Invalid session phase
};

inline const char* ValueLocationErrorReasonToString(ValueLocationErrorReason r) {
    switch (r) {
        case ValueLocationErrorReason::NONE:             return "NONE";
        case ValueLocationErrorReason::WARMUP_PROFILE:   return "WARMUP_PROFILE";
        case ValueLocationErrorReason::WARMUP_PRIOR:     return "WARMUP_PRIOR";
        case ValueLocationErrorReason::WARMUP_IB:        return "WARMUP_IB";
        case ValueLocationErrorReason::WARMUP_MULTIPLE:  return "WARMUP_MULTIPLE";
        case ValueLocationErrorReason::ERR_INVALID_PRICE: return "ERR_INVALID_PRICE";
        case ValueLocationErrorReason::ERR_INVALID_VA:   return "ERR_INVALID_VA";
        case ValueLocationErrorReason::ERR_INVALID_TICK: return "ERR_INVALID_TICK";
        case ValueLocationErrorReason::ERR_NO_ZONE_MGR:  return "ERR_NO_ZONE_MGR";
        case ValueLocationErrorReason::ERR_INVALID_PHASE: return "ERR_INVALID_PHASE";
        default:                                         return "UNKNOWN";
    }
}

inline bool IsValueLocationWarmup(ValueLocationErrorReason r) {
    return r >= ValueLocationErrorReason::WARMUP_PROFILE &&
           r <= ValueLocationErrorReason::WARMUP_MULTIPLE;
}

inline bool IsValueLocationHardError(ValueLocationErrorReason r) {
    return r != ValueLocationErrorReason::NONE && !IsValueLocationWarmup(r);
}

// ============================================================================
// CONFIGURATION
// ============================================================================

struct ValueLocationConfig {
    // =========================================================================
    // TOLERANCE THRESHOLDS (in ticks)
    // =========================================================================
    int pocToleranceTicks = 2;        // Within 2 ticks = AT_POC
    int vaBoundaryTicks = 3;          // Within 3 ticks = AT_VAH/VAL
    int extensionThresholdTicks = 8;  // Beyond 8 ticks = FAR above/below (ES: 2 points)
    int hvnLvnToleranceTicks = 4;     // Proximity to HVN/LVN

    // =========================================================================
    // VA OVERLAP THRESHOLDS
    // =========================================================================
    double overlapHighThreshold = 0.50;   // > 50% = OVERLAPPING
    double overlapLowThreshold = 0.30;    // < 30% = SEPARATED

    // =========================================================================
    // HYSTERESIS
    // =========================================================================
    int minConfirmationBars = 2;          // Bars to confirm zone change
    int maxPersistenceBars = 15;          // Max bars location persists without refresh

    // =========================================================================
    // REFERENCE LEVEL PROXIMITY
    // =========================================================================
    int referenceNearTicks = 6;           // "Near" threshold
    int referenceApproachingTicks = 12;   // "Approaching" threshold
    int maxReferenceLevels = 12;          // Max levels to track in nearby list

    // NOTE: Strategy gating config REMOVED (Jan 2025)
    // Policy decisions belong in decision/arbitration layer, not location SSOT.
};

// ============================================================================
// REFERENCE LEVEL PROXIMITY
// ============================================================================
// Tracks distance to a specific reference level.
// Uses existing LevelType from amt_core.h.
// ============================================================================

struct ReferenceLevelProximity {
    LevelType type = LevelType::UNKNOWN;
    double price = 0.0;
    double distanceTicks = 0.0;
    bool isNear = false;           // Within referenceNearTicks
    bool isApproaching = false;    // Within referenceApproachingTicks
    bool isAbove = false;          // Price above this level
    bool valid = false;            // Level data is valid

    // For sorting by proximity (closest first)
    bool operator<(const ReferenceLevelProximity& other) const {
        return std::abs(distanceTicks) < std::abs(other.distanceTicks);
    }
};

// ============================================================================
// NOTE: StrategyGating REMOVED (Jan 2025)
// Policy decisions (ShouldFade, ShouldBreakout) belong in a decision/arbitration
// layer that consumes all engine outputs, NOT in the location SSOT.
// ValueLocationEngine outputs descriptive primitives only.
// ============================================================================

// ============================================================================
// VALUE LOCATION RESULT (Per-Bar Output)
// ============================================================================

struct ValueLocationResult {
    // =========================================================================
    // PRIMARY LOCATION (Q1: Where am I relative to value?)
    // =========================================================================
    ValueZone zone = ValueZone::UNKNOWN;                    // Fine-grained (9 states) - SSOT

    // Distance metrics (in ticks, signed: + = above, - = below)
    double distFromPOCTicks = 0.0;
    double distFromVAHTicks = 0.0;
    double distFromVALTicks = 0.0;
    double distFromMidpointTicks = 0.0;  // VA midpoint

    // Percentile within VA [0, 100]
    // 0 = at VAL, 50 = at midpoint, 100 = at VAH
    double vaPercentile = 50.0;
    bool vaPercentileValid = false;

    // Nearest profile zone (delegated from ZoneManager)
    int nearestZoneId = -1;
    ZoneType nearestZoneType = ZoneType::NONE;
    ZoneProximity nearestZoneProximity = ZoneProximity::INACTIVE;
    double nearestZoneDistTicks = 0.0;

    // =========================================================================
    // STRUCTURAL CONTEXT (Q2: Balance or Imbalance structurally?)
    // =========================================================================
    VAOverlapState overlapState = VAOverlapState::UNKNOWN;
    double vaOverlapPct = 1.0;            // [0, 1] overlap ratio
    double vaWidthTicks = 0.0;            // Current VA width
    double priorVAWidthTicks = 0.0;       // Prior VA width
    double vaExpansionRatio = 1.0;        // current / prior width
    bool isVAExpanding = false;           // VA growing
    bool isVAContracting = false;         // VA shrinking
    ValueMigration valueMigration = ValueMigration::UNKNOWN;  // Existing enum

    // =========================================================================
    // SESSION CONTEXT (Q3: What session context applies?)
    // =========================================================================
    SessionPhase sessionPhase = SessionPhase::UNKNOWN;
    bool isIBComplete = false;            // Initial Balance frozen
    double ibRangeTicks = 0.0;            // IB range
    double sessionRangeTicks = 0.0;       // Current session range
    double rangeExtensionRatio = 1.0;     // sessionRange / ibRange

    // =========================================================================
    // REFERENCE LEVELS (Q4: What nearby reference levels matter?)
    // =========================================================================
    std::vector<ReferenceLevelProximity> nearbyLevels;  // Sorted by distance
    LevelType nearestLevelType = LevelType::UNKNOWN;
    double nearestLevelDistance = 0.0;
    int levelsWithin5Ticks = 0;           // Confluence count
    int levelsWithin10Ticks = 0;

    // Specific level distances (in ticks)
    double distToSessionHighTicks = 0.0;
    double distToSessionLowTicks = 0.0;
    double distToIBHighTicks = 0.0;
    double distToIBLowTicks = 0.0;
    double distToPriorPOCTicks = 0.0;
    double distToPriorVAHTicks = 0.0;
    double distToPriorVALTicks = 0.0;

    // =========================================================================
    // HVN/LVN DISTANCE PRIMITIVES (measurable, not just booleans)
    // =========================================================================
    double nearestHVNDistTicks = 0.0;     // Signed: + = HVN above price, - = below
    bool nearestHVNValid = false;         // True if at least one HVN exists
    double nearestLVNDistTicks = 0.0;     // Signed: + = LVN above price, - = below
    bool nearestLVNValid = false;         // True if at least one LVN exists
    int nearbyHVNCount = 0;               // Count within referenceApproachingTicks
    int nearbyLVNCount = 0;               // Count within referenceApproachingTicks

    // =========================================================================
    // NOTE: StrategyGating REMOVED (Jan 2025)
    // Policy decisions belong in decision/arbitration layer, not location SSOT.
    // =========================================================================

    // =========================================================================
    // NOTE: Hysteresis state fields REMOVED from result (Jan 2025)
    // Engine owns hysteresis state (confirmedZone, candidateZone, etc.)
    // Result contains only per-bar facts and events.
    // =========================================================================

    // =========================================================================
    // EVENTS (Only true on transition bars)
    // =========================================================================
    bool zoneChanged = false;             // Zone transition this bar
    bool enteredValue = false;            // Just entered VA
    bool exitedValue = false;             // Just left VA
    bool crossedPOC = false;              // Crossed POC this bar
    bool reachedExtreme = false;          // Reached session high/low

    // =========================================================================
    // VALIDITY / ERROR
    // =========================================================================
    ValueLocationErrorReason errorReason = ValueLocationErrorReason::NONE;
    int errorBar = -1;
    // NOTE: phase field REMOVED (Jan 2025) - use sessionPhase (single SSOT)

    // =========================================================================
    // ACCESSORS
    // =========================================================================
    bool IsReady() const {
        return errorReason == ValueLocationErrorReason::NONE;
    }

    bool IsWarmup() const {
        return IsValueLocationWarmup(errorReason);
    }

    bool IsHardError() const {
        return IsValueLocationHardError(errorReason);
    }

    // Location queries - use ValueZone (SSOT) only
    bool IsInsideValue() const {
        return IsReady() && (zone == ValueZone::UPPER_VALUE ||
                             zone == ValueZone::LOWER_VALUE ||
                             zone == ValueZone::AT_POC);
    }
    bool IsAboveValue() const {
        return IsReady() && (zone == ValueZone::FAR_ABOVE_VALUE ||
                             zone == ValueZone::NEAR_ABOVE_VALUE);
    }
    bool IsBelowValue() const {
        return IsReady() && (zone == ValueZone::FAR_BELOW_VALUE ||
                             zone == ValueZone::NEAR_BELOW_VALUE);
    }
    bool IsAtVAH() const {
        return IsReady() && zone == ValueZone::AT_VAH;
    }
    bool IsAtVAL() const {
        return IsReady() && zone == ValueZone::AT_VAL;
    }
    bool IsAtBoundary() const {
        return IsReady() &&
               (zone == ValueZone::AT_VAH || zone == ValueZone::AT_VAL);
    }
    bool IsAtPOC() const {
        return IsReady() && zone == ValueZone::AT_POC;
    }
    bool IsOutsideValue() const {
        return IsAboveValue() || IsBelowValue();
    }
    bool IsFarOutside() const {
        return zone == ValueZone::FAR_ABOVE_VALUE || zone == ValueZone::FAR_BELOW_VALUE;
    }

    // Structure queries
    bool IsBalanceStructure() const {
        return overlapState == VAOverlapState::OVERLAPPING ||
               overlapState == VAOverlapState::CONTAINED;
    }
    bool IsTrendStructure() const {
        return overlapState == VAOverlapState::SEPARATED_ABOVE ||
               overlapState == VAOverlapState::SEPARATED_BELOW;
    }

    /**
     * Get ValueAreaRegion (5 states) from this result's ValueZone (9 states).
     * SSOT: Use this instead of CalculateVARegion().
     */
    ValueAreaRegion GetValueAreaRegion() const {
        return ValueZoneToValueAreaRegion(zone);
    }

    // =========================================================================
    // HVN/LVN DERIVED QUERIES (convenience, based on distance primitives)
    // =========================================================================
    bool IsAtHVN(int toleranceTicks = 4) const {
        return nearestHVNValid && std::abs(nearestHVNDistTicks) <= toleranceTicks;
    }
    bool IsAtLVN(int toleranceTicks = 4) const {
        return nearestLVNValid && std::abs(nearestLVNDistTicks) <= toleranceTicks;
    }
    bool IsNearHVN(int approachingTicks = 12) const {
        return nearestHVNValid && std::abs(nearestHVNDistTicks) <= approachingTicks;
    }
    bool IsNearLVN(int approachingTicks = 12) const {
        return nearestLVNValid && std::abs(nearestLVNDistTicks) <= approachingTicks;
    }

    // NOTE: ShouldFade(), ShouldBreakout() REMOVED (Jan 2025)
    // Policy decisions belong in decision/arbitration layer, not location SSOT.

    // Format for logging
    std::string FormatForLog() const {
        char buf[512];
        snprintf(buf, sizeof(buf),
            "ZONE=%s | POC_T=%+.1f VAH_T=%+.1f VAL_T=%+.1f | VA_PCT=%.1f",
            ValueZoneToString(zone),
            distFromPOCTicks, distFromVAHTicks, distFromVALTicks,
            vaPercentile);
        return std::string(buf);
    }

    std::string FormatStructureForLog() const {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "OVERLAP=%s OVL_PCT=%.1f%% | WIDTH=%.0ft PRIOR=%.0ft RATIO=%.2f",
            VAOverlapStateToString(overlapState),
            vaOverlapPct * 100.0,
            vaWidthTicks, priorVAWidthTicks, vaExpansionRatio);
        return std::string(buf);
    }

    std::string FormatSessionForLog() const {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "PHASE=%s | IB=%s RANGE=%.0ft EXT=%.2f",
            SessionPhaseToString(sessionPhase),
            isIBComplete ? "FROZEN" : "OPEN",
            sessionRangeTicks, rangeExtensionRatio);
        return std::string(buf);
    }

    std::string FormatReferencesForLog() const {
        char buf[256];
        snprintf(buf, sizeof(buf),
            "NEAR=%s(%.1ft) | HVN_T=%+.1f(%s) LVN_T=%+.1f(%s) | CNT: HVN=%d LVN=%d | WITHIN_5T=%d",
            LevelTypeToString(nearestLevelType),
            nearestLevelDistance,
            nearestHVNDistTicks, nearestHVNValid ? "Y" : "N",
            nearestLVNDistTicks, nearestLVNValid ? "Y" : "N",
            nearbyHVNCount, nearbyLVNCount,
            levelsWithin5Ticks);
        return std::string(buf);
    }

    // NOTE: FormatGatingForLog() REMOVED (Jan 2025) - gating removed from SSOT
};

// ============================================================================
// VALUE LOCATION ENGINE
// ============================================================================

class ValueLocationEngine {
public:
    // =========================================================================
    // CONFIGURATION
    // =========================================================================
    ValueLocationConfig config;

    // =========================================================================
    // CURRENT STATE
    // =========================================================================
    SessionPhase currentPhase = SessionPhase::UNKNOWN;

    // Hysteresis
    ValueZone confirmedZone = ValueZone::UNKNOWN;
    ValueZone candidateZone = ValueZone::UNKNOWN;
    int candidateConfirmationBars = 0;
    int barsInConfirmedZone = 0;

    // Previous bar tracking (for events)
    ValueZone prevZone = ValueZone::UNKNOWN;
    double prevPrice = 0.0;
    bool prevInsideValue = false;

    // Session stats
    int sessionBars = 0;

    // =========================================================================
    // CONSTRUCTOR / INITIALIZATION
    // =========================================================================
    ValueLocationEngine() = default;

    void SetPhase(SessionPhase phase) { currentPhase = phase; }
    void SetConfig(const ValueLocationConfig& cfg) { config = cfg; }

    // =========================================================================
    // MAIN COMPUTATION
    // =========================================================================
    ValueLocationResult Compute(
        // Price data (required)
        double close, double tickSize, int barIndex,
        // Core profile levels (from SessionManager)
        double poc, double vah, double val,
        // Prior session (from ZoneSessionState, pass 0 if unavailable)
        double priorPOC, double priorVAH, double priorVAL,
        // Structure tracking (delegate to StructureTracker)
        const StructureTracker& structure,
        // Active zones (delegate to ZoneManager for nearest zone)
        const ZoneManager& zm,
        // HVN/LVN (from SessionVolumeProfile)
        const std::vector<double>* hvnLevels,
        const std::vector<double>* lvnLevels,
        // Market state context (from DaltonEngine)
        AMTMarketState marketState
    );

    // =========================================================================
    // SESSION BOUNDARY METHODS
    // =========================================================================
    void ResetForSession() {
        confirmedZone = ValueZone::UNKNOWN;
        candidateZone = ValueZone::UNKNOWN;
        candidateConfirmationBars = 0;
        barsInConfirmedZone = 0;

        prevZone = ValueZone::UNKNOWN;
        prevPrice = 0.0;
        prevInsideValue = false;
        sessionBars = 0;
    }

    void Reset() {
        ResetForSession();
        currentPhase = SessionPhase::UNKNOWN;
    }

private:
    // =========================================================================
    // INTERNAL METHODS
    // =========================================================================

    // Determine fine-grained zone from price vs POC/VAH/VAL
    ValueZone DetermineZone(double price, double poc, double vah,
                            double val, double tickSize) const;

    // Compute VA overlap state and percentage
    void ComputeVAOverlap(ValueLocationResult& result,
                          double vah, double val,
                          double priorVAH, double priorVAL,
                          double tickSize) const;

    // Build reference level list (sorted by distance) + populate HVN/LVN distances
    void BuildReferenceLevels(ValueLocationResult& result,
                              double price, double tickSize,
                              double poc, double vah, double val,
                              double priorPOC, double priorVAH, double priorVAL,
                              const StructureTracker& structure,
                              const ZoneManager& zm,
                              const std::vector<double>* hvnLevels,
                              const std::vector<double>* lvnLevels) const;

    // NOTE: ComputeGating() REMOVED (Jan 2025)
    // Policy decisions belong in decision/arbitration layer, not location SSOT.

    // Apply hysteresis to zone transitions (engine state only, not copied to result)
    void UpdateHysteresis(ValueLocationResult& result, ValueZone rawZone);

    // Detect events (entries, exits, crossings)
    void DetectEvents(ValueLocationResult& result, double price,
                      double poc, double vah, double val, double tickSize);
};

// ============================================================================
// IMPLEMENTATION
// ============================================================================

inline ValueZone ValueLocationEngine::DetermineZone(
    double price, double poc, double vah, double val, double tickSize) const
{
    if (tickSize <= 0.0) return ValueZone::UNKNOWN;
    if (vah <= val) return ValueZone::UNKNOWN;
    if (poc <= 0.0 || vah <= 0.0 || val <= 0.0) return ValueZone::UNKNOWN;

    // Convert to tick distances
    const double distFromPOC = (price - poc) / tickSize;
    const double distFromVAH = (price - vah) / tickSize;
    const double distFromVAL = (price - val) / tickSize;

    // Check boundaries first (with tolerance)
    if (std::abs(distFromPOC) <= config.pocToleranceTicks) {
        return ValueZone::AT_POC;
    }
    if (std::abs(distFromVAH) <= config.vaBoundaryTicks) {
        return ValueZone::AT_VAH;
    }
    if (std::abs(distFromVAL) <= config.vaBoundaryTicks) {
        return ValueZone::AT_VAL;
    }

    // Above VAH
    if (distFromVAH > config.vaBoundaryTicks) {
        if (distFromVAH > config.extensionThresholdTicks) {
            return ValueZone::FAR_ABOVE_VALUE;
        }
        return ValueZone::NEAR_ABOVE_VALUE;
    }

    // Below VAL
    if (distFromVAL < -config.vaBoundaryTicks) {
        if (distFromVAL < -config.extensionThresholdTicks) {
            return ValueZone::FAR_BELOW_VALUE;
        }
        return ValueZone::NEAR_BELOW_VALUE;
    }

    // Inside value - determine upper or lower half
    if (distFromPOC > 0) {
        return ValueZone::UPPER_VALUE;
    }
    return ValueZone::LOWER_VALUE;
}

inline void ValueLocationEngine::ComputeVAOverlap(
    ValueLocationResult& result,
    double vah, double val,
    double priorVAH, double priorVAL,
    double tickSize) const
{
    // Compute current VA width
    result.vaWidthTicks = (vah - val) / tickSize;

    // Check if prior VA is valid
    if (priorVAH <= 0.0 || priorVAL <= 0.0 || priorVAH <= priorVAL) {
        result.overlapState = VAOverlapState::UNKNOWN;
        result.vaOverlapPct = 0.0;
        return;
    }

    result.priorVAWidthTicks = (priorVAH - priorVAL) / tickSize;
    result.vaExpansionRatio = result.vaWidthTicks / result.priorVAWidthTicks;
    result.isVAExpanding = result.vaExpansionRatio > 1.1;
    result.isVAContracting = result.vaExpansionRatio < 0.9;

    // Compute overlap
    double overlapHigh = (std::min)(vah, priorVAH);
    double overlapLow = (std::max)(val, priorVAL);
    double overlapWidth = (std::max)(0.0, overlapHigh - overlapLow);

    double priorWidth = priorVAH - priorVAL;
    double currentWidth = vah - val;
    double smallerWidth = (std::min)(priorWidth, currentWidth);

    if (smallerWidth > 0.0) {
        result.vaOverlapPct = overlapWidth / smallerWidth;
    } else {
        result.vaOverlapPct = 0.0;
    }

    // Classify overlap state
    // Check for separation first
    if (val >= priorVAH) {
        result.overlapState = VAOverlapState::SEPARATED_ABOVE;
    } else if (vah <= priorVAL) {
        result.overlapState = VAOverlapState::SEPARATED_BELOW;
    }
    // Check for containment
    else if (vah <= priorVAH && val >= priorVAL) {
        result.overlapState = VAOverlapState::CONTAINED;
    }
    // Check overlap thresholds
    else if (result.vaOverlapPct < config.overlapLowThreshold) {
        // Low overlap - determine direction
        double currentMid = (vah + val) / 2.0;
        double priorMid = (priorVAH + priorVAL) / 2.0;
        if (currentMid > priorMid) {
            result.overlapState = VAOverlapState::SEPARATED_ABOVE;
        } else {
            result.overlapState = VAOverlapState::SEPARATED_BELOW;
        }
    } else if (result.vaOverlapPct >= config.overlapHighThreshold) {
        result.overlapState = VAOverlapState::OVERLAPPING;
    } else if (result.isVAExpanding) {
        result.overlapState = VAOverlapState::EXPANDING;
    } else {
        result.overlapState = VAOverlapState::OVERLAPPING;
    }

    // Use existing ValueMigration computation
    result.valueMigration = ComputeValueMigration(vah, val, priorVAH, priorVAL);
}

inline void ValueLocationEngine::BuildReferenceLevels(
    ValueLocationResult& result,
    double price, double tickSize,
    double poc, double vah, double val,
    double priorPOC, double priorVAH, double priorVAL,
    const StructureTracker& structure,
    const ZoneManager& zm,
    const std::vector<double>* hvnLevels,
    const std::vector<double>* lvnLevels) const
{
    result.nearbyLevels.clear();
    result.levelsWithin5Ticks = 0;
    result.levelsWithin10Ticks = 0;

    // HVN/LVN distance primitives - initialize
    result.nearestHVNDistTicks = 0.0;
    result.nearestHVNValid = false;
    result.nearestLVNDistTicks = 0.0;
    result.nearestLVNValid = false;
    result.nearbyHVNCount = 0;
    result.nearbyLVNCount = 0;

    // Track nearest HVN/LVN for distance primitives
    double nearestHVNAbsDist = 1e9;
    double nearestLVNAbsDist = 1e9;

    auto addLevel = [&](LevelType type, double levelPrice) {
        if (levelPrice <= 0.0) return;

        ReferenceLevelProximity ref;
        ref.type = type;
        ref.price = levelPrice;
        ref.distanceTicks = (price - levelPrice) / tickSize;
        ref.isAbove = price > levelPrice;
        ref.isNear = std::abs(ref.distanceTicks) <= config.referenceNearTicks;
        ref.isApproaching = std::abs(ref.distanceTicks) <= config.referenceApproachingTicks;
        ref.valid = true;

        result.nearbyLevels.push_back(ref);

        double absDist = std::abs(ref.distanceTicks);
        if (absDist <= 5.0) result.levelsWithin5Ticks++;
        if (absDist <= 10.0) result.levelsWithin10Ticks++;
    };

    // Current profile levels
    addLevel(LevelType::POC, poc);
    addLevel(LevelType::VAH, vah);
    addLevel(LevelType::VAL, val);

    // Prior session levels
    if (priorPOC > 0.0) {
        addLevel(LevelType::PRIOR_POC, priorPOC);
        result.distToPriorPOCTicks = (price - priorPOC) / tickSize;
    }
    if (priorVAH > 0.0) {
        addLevel(LevelType::PRIOR_VAH, priorVAH);
        result.distToPriorVAHTicks = (price - priorVAH) / tickSize;
    }
    if (priorVAL > 0.0) {
        addLevel(LevelType::PRIOR_VAL, priorVAL);
        result.distToPriorVALTicks = (price - priorVAL) / tickSize;
    }

    // Session structure levels (delegated to StructureTracker)
    double sessHigh = structure.GetSessionHigh();
    double sessLow = structure.GetSessionLow();
    double ibHigh = structure.GetIBHigh();
    double ibLow = structure.GetIBLow();

    if (sessHigh > 0.0) {
        addLevel(LevelType::SESSION_HIGH, sessHigh);
        result.distToSessionHighTicks = (price - sessHigh) / tickSize;
    }
    if (sessLow > 0.0) {
        addLevel(LevelType::SESSION_LOW, sessLow);
        result.distToSessionLowTicks = (price - sessLow) / tickSize;
    }
    if (ibHigh > 0.0) {
        addLevel(LevelType::IB_HIGH, ibHigh);
        result.distToIBHighTicks = (price - ibHigh) / tickSize;
    }
    if (ibLow > 0.0) {
        addLevel(LevelType::IB_LOW, ibLow);
        result.distToIBLowTicks = (price - ibLow) / tickSize;
    }

    // HVN/LVN levels - populate distance primitives
    if (hvnLevels) {
        for (double hvnPrice : *hvnLevels) {
            if (hvnPrice > 0.0) {
                addLevel(LevelType::HVN, hvnPrice);
                double signedDist = (price - hvnPrice) / tickSize;  // + = price above HVN
                double absDist = std::abs(signedDist);

                // Track nearest HVN
                if (absDist < nearestHVNAbsDist) {
                    nearestHVNAbsDist = absDist;
                    result.nearestHVNDistTicks = signedDist;
                    result.nearestHVNValid = true;
                }

                // Count nearby
                if (absDist <= config.referenceApproachingTicks) {
                    result.nearbyHVNCount++;
                }
            }
        }
    }
    if (lvnLevels) {
        for (double lvnPrice : *lvnLevels) {
            if (lvnPrice > 0.0) {
                addLevel(LevelType::LVN, lvnPrice);
                double signedDist = (price - lvnPrice) / tickSize;  // + = price above LVN
                double absDist = std::abs(signedDist);

                // Track nearest LVN
                if (absDist < nearestLVNAbsDist) {
                    nearestLVNAbsDist = absDist;
                    result.nearestLVNDistTicks = signedDist;
                    result.nearestLVNValid = true;
                }

                // Count nearby
                if (absDist <= config.referenceApproachingTicks) {
                    result.nearbyLVNCount++;
                }
            }
        }
    }

    // Sort by absolute distance
    std::sort(result.nearbyLevels.begin(), result.nearbyLevels.end());

    // Trim to max
    if (result.nearbyLevels.size() > static_cast<size_t>(config.maxReferenceLevels)) {
        result.nearbyLevels.resize(config.maxReferenceLevels);
    }

    // Set nearest level info
    if (!result.nearbyLevels.empty()) {
        result.nearestLevelType = result.nearbyLevels[0].type;
        result.nearestLevelDistance = std::abs(result.nearbyLevels[0].distanceTicks);
    }
}

// NOTE: ComputeGating() REMOVED (Jan 2025)
// Policy decisions belong in decision/arbitration layer, not location SSOT.

inline void ValueLocationEngine::UpdateHysteresis(
    ValueLocationResult& result, ValueZone rawZone)
{
    // NOTE: Hysteresis state stays in ENGINE, not copied to result (Jan 2025 refactor)
    // Result only gets the zoneChanged EVENT flag

    if (rawZone == confirmedZone) {
        // Still in same zone
        candidateZone = confirmedZone;
        candidateConfirmationBars = 0;
        barsInConfirmedZone++;
    }
    else if (rawZone == candidateZone && candidateZone != confirmedZone) {
        // Building evidence for new zone
        candidateConfirmationBars++;
        if (candidateConfirmationBars >= config.minConfirmationBars) {
            // Confirmed transition
            confirmedZone = candidateZone;
            barsInConfirmedZone = 0;
            result.zoneChanged = true;  // Event flag - this bar confirmed a zone change
        }
    }
    else if (rawZone != confirmedZone && rawZone != candidateZone) {
        // New candidate detected
        candidateZone = rawZone;
        candidateConfirmationBars = 1;
    }

    // NOTE: confirmedZone, candidateZone, confirmationBars, barsInZone, isTransitioning
    // are ENGINE state - not copied to result. Query engine directly if needed.
}

inline void ValueLocationEngine::DetectEvents(
    ValueLocationResult& result,
    double price, double poc, double vah, double val, double tickSize)
{
    result.enteredValue = false;
    result.exitedValue = false;
    result.crossedPOC = false;
    result.reachedExtreme = false;

    bool currentInsideValue = result.IsInsideValue();

    // Entry/exit events (only after first bar - prevPrice > 0 means we have prior state)
    if (prevPrice > 0.0) {
        if (currentInsideValue && !prevInsideValue) {
            result.enteredValue = true;
        }
        if (!currentInsideValue && prevInsideValue) {
            result.exitedValue = true;
        }
    }

    // POC crossing
    if (prevPrice > 0.0 && poc > 0.0) {
        bool prevAbovePOC = prevPrice > poc;
        bool currAbovePOC = price > poc;
        if (prevAbovePOC != currAbovePOC) {
            result.crossedPOC = true;
        }
    }

    // Extreme detection
    if (std::abs(result.distToSessionHighTicks) <= 1 ||
        std::abs(result.distToSessionLowTicks) <= 1) {
        result.reachedExtreme = true;
    }

    // Update state for next bar
    prevZone = result.zone;
    prevPrice = price;
    prevInsideValue = currentInsideValue;
}

inline ValueLocationResult ValueLocationEngine::Compute(
    double close, double tickSize, int barIndex,
    double poc, double vah, double val,
    double priorPOC, double priorVAH, double priorVAL,
    const StructureTracker& structure,
    const ZoneManager& zm,
    const std::vector<double>* hvnLevels,
    const std::vector<double>* lvnLevels,
    AMTMarketState /* marketState - no longer used, gating removed */)
{
    ValueLocationResult result;
    result.sessionPhase = currentPhase;  // Single SSOT for phase
    sessionBars++;

    // =========================================================================
    // VALIDATION
    // =========================================================================

    // Price validation
    if (close <= 0.0 || std::isnan(close)) {
        result.errorReason = ValueLocationErrorReason::ERR_INVALID_PRICE;
        result.errorBar = barIndex;
        return result;
    }

    // Tick size validation
    if (tickSize <= 0.0) {
        result.errorReason = ValueLocationErrorReason::ERR_INVALID_TICK;
        result.errorBar = barIndex;
        return result;
    }

    // VA validation
    if (vah <= val || vah <= 0.0 || val <= 0.0 || poc <= 0.0) {
        result.errorReason = ValueLocationErrorReason::ERR_INVALID_VA;
        result.errorBar = barIndex;
        return result;
    }

    // =========================================================================
    // LOCATION DETERMINATION
    // =========================================================================

    // Determine fine-grained zone (9-state SSOT)
    ValueZone rawZone = DetermineZone(close, poc, vah, val, tickSize);
    result.zone = rawZone;

    // Distance metrics
    result.distFromPOCTicks = (close - poc) / tickSize;
    result.distFromVAHTicks = (close - vah) / tickSize;
    result.distFromVALTicks = (close - val) / tickSize;

    double midpoint = (vah + val) / 2.0;
    result.distFromMidpointTicks = (close - midpoint) / tickSize;

    // VA percentile (0 = VAL, 100 = VAH)
    double vaWidth = vah - val;
    if (vaWidth > 0.0) {
        result.vaPercentile = 100.0 * (close - val) / vaWidth;
        result.vaPercentile = (std::max)(0.0, (std::min)(100.0, result.vaPercentile));
        result.vaPercentileValid = true;
    }

    // =========================================================================
    // NEAREST ZONE (DELEGATED TO ZONEMANAGER)
    // =========================================================================

    ZoneRuntime* nearestZone = const_cast<ZoneManager&>(zm).GetStrongestZoneAtPrice(
        close, tickSize, -1);  // -1 = use default halo

    if (nearestZone) {
        result.nearestZoneId = nearestZone->zoneId;
        result.nearestZoneType = nearestZone->type;
        result.nearestZoneProximity = nearestZone->proximity;
        result.nearestZoneDistTicks = (close - nearestZone->GetAnchorPrice()) / tickSize;
    }

    // =========================================================================
    // STRUCTURAL CONTEXT
    // =========================================================================

    ComputeVAOverlap(result, vah, val, priorVAH, priorVAL, tickSize);

    // =========================================================================
    // SESSION CONTEXT
    // =========================================================================

    result.isIBComplete = structure.IsIBFrozen();
    result.sessionRangeTicks = structure.GetSessionRangeTicks();

    double ibHigh = structure.GetIBHigh();
    double ibLow = structure.GetIBLow();
    if (ibHigh > 0.0 && ibLow > 0.0 && ibHigh > ibLow) {
        result.ibRangeTicks = (ibHigh - ibLow) / tickSize;
        if (result.ibRangeTicks > 0.0) {
            result.rangeExtensionRatio = result.sessionRangeTicks / result.ibRangeTicks;
        }
    }

    // =========================================================================
    // REFERENCE LEVELS
    // =========================================================================

    BuildReferenceLevels(result, close, tickSize,
                         poc, vah, val,
                         priorPOC, priorVAH, priorVAL,
                         structure, zm,
                         hvnLevels, lvnLevels);

    // =========================================================================
    // NOTE: Strategy gating REMOVED (Jan 2025)
    // Policy decisions belong in decision/arbitration layer, not location SSOT.
    // =========================================================================

    // =========================================================================
    // HYSTERESIS
    // =========================================================================

    UpdateHysteresis(result, rawZone);

    // =========================================================================
    // EVENTS
    // =========================================================================

    DetectEvents(result, close, poc, vah, val, tickSize);

    // Success
    result.errorReason = ValueLocationErrorReason::NONE;
    return result;
}

} // namespace AMT

#endif // AMT_VALUELOCATION_H
