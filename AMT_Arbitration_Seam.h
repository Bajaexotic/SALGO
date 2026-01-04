// ============================================================================
// AMT_Arbitration_Seam.h
// SSOT: M0 Arbitration Ladder (extracted for testability)
// This file has ZERO Sierra dependencies. Safe for standalone compilation.
// ============================================================================

#ifndef AMT_ARBITRATION_SEAM_H
#define AMT_ARBITRATION_SEAM_H

// Only dependency: amt_core.h for ZoneProximity and AMTMarketState enums
// This header has no Sierra dependencies.
#include "amt_core.h"

// ============================================================================
// ARB CONSTANTS (copied from AuctionSensor_v1.cpp:126-136)
// ============================================================================

namespace AMT_Arb {
    static constexpr int ARB_INVALID_ANCHOR_IDS = 1;
    static constexpr int ARB_INVALID_ZONE_PTRS  = 2;
    static constexpr int ARB_NOT_READY          = 3;
    static constexpr int ARB_INVALID_VBP_PRICES = 4;
    static constexpr int ARB_INVALID_VA_ORDER   = 5;
    static constexpr int ARB_VBP_STALE          = 6;
    static constexpr int ARB_DEFAULT_BASELINE   = 7;
    static constexpr int ARB_BASELINE_EXTREME   = 8;
    static constexpr int ARB_ENGAGED            = 10;
    static constexpr int ARB_DIRECTIONAL        = 11;
    static constexpr int MAX_VBP_STALE_BARS     = 50;

    // ========================================================================
    // EXTREME DELTA THRESHOLDS (Persistence-Validated)
    // ========================================================================
    //
    // DEFINITION: "Extreme delta" now requires BOTH per-bar AND session persistence:
    //   isExtremeDelta := isExtremeDeltaBar && isExtremeDeltaSession
    //
    // This eliminates false positives from single-bar spikes that lack session conviction.
    //
    // Per-bar threshold (BOTH directions):
    //   isExtremeDeltaBar := (deltaConsistency > HIGH_THRESHOLD || deltaConsistency < LOW_THRESHOLD)
    //   deltaConsistency is normalized [0,1] where 0.5 = neutral
    //   > 0.7 = 70%+ buying (extreme buying)
    //   < 0.3 = 70%+ selling (extreme selling)
    //   BUG FIX: Previously only checked > 0.7, missing extreme selling!
    //
    // Session persistence threshold (MAGNITUDE-ONLY):
    //   isExtremeDeltaSession := (sessionDeltaPctile >= SESSION_EXTREME_PCTILE_THRESHOLD)
    //   sessionDeltaPctile is percentile rank of |sessionDeltaPct| in its rolling distribution
    //   Distribution stores |sessionDeltaPct| (absolute magnitude)
    //   Top 15% (>= 85th percentile) = extreme magnitude in EITHER direction
    //   Direction is handled separately by coherence check (not by percentile)
    // ========================================================================
    static constexpr double EXTREME_DELTA_HIGH_THRESHOLD = 0.7;   // 70%+ buying
    static constexpr double EXTREME_DELTA_LOW_THRESHOLD = 0.3;    // 70%+ selling (1 - 0.7 = 0.3)
    static constexpr double SESSION_EXTREME_PCTILE_THRESHOLD = 85.0;  // 85th percentile = top 15%

    // Legacy alias for backward compatibility
    static constexpr double EXTREME_DELTA_THRESHOLD = EXTREME_DELTA_HIGH_THRESHOLD;
}

// ============================================================================
// INPUT/OUTPUT STRUCTS
// ============================================================================

struct ArbitrationInput {
    // Anchor IDs
    int pocId = -1;
    int vahId = -1;
    int valId = -1;

    // Zone validity (true if GetZone() returned non-null)
    bool pocValid = false;
    bool vahValid = false;
    bool valValid = false;

    // Zone proximity (only meaningful if corresponding *Valid is true)
    AMT::ZoneProximity pocProximity = AMT::ZoneProximity::INACTIVE;
    AMT::ZoneProximity vahProximity = AMT::ZoneProximity::INACTIVE;
    AMT::ZoneProximity valProximity = AMT::ZoneProximity::INACTIVE;

    // Initialization state
    bool zonesInitialized = false;

    // VbP profile values
    double vbpPoc = 0.0;
    double vbpVah = 0.0;
    double vbpVal = 0.0;
    int barsSinceLastCompute = 0;

    // Snapshot state
    bool isDirectional = false;
    double deltaConsistency = 0.5;       // Per-bar aggressor fraction [0, 1] where 0.5=neutral
    bool deltaConsistencyValid = false;  // True when bar has sufficient volume (not thin bar)

    // ========================================================================
    // SESSION-SCOPED DELTA (First-Class Decision Input)
    // ========================================================================
    double sessionDeltaPct = 0.0;        // sessionCumDelta / sessionTotalVolume (SSOT)
    double sessionDeltaPctile = 50.0;    // Percentile rank in rolling distribution [0, 100]
    bool sessionDeltaValid = false;      // True once session has sufficient data
};

struct ArbitrationResult {
    int arbReason = AMT_Arb::ARB_DEFAULT_BASELINE;
    bool useZones = false;
    int engagedZoneId = -1;
    int pocProx = -1;
    AMT::AMTMarketState rawState = AMT::AMTMarketState::BALANCE;

    // ========================================================================
    // EXTREME DELTA DECOMPOSITION (Persistence-Validated)
    // ========================================================================
    bool isExtremeDeltaBar = false;      // Per-bar: deltaConsistency > 0.7
    bool isExtremeDeltaSession = false;  // Session: sessionDeltaPctile >= 85
    bool isExtremeDelta = false;         // Combined: bar && session (new definition)

    // ========================================================================
    // AGGRESSION CLASSIFICATION (Directional Coherence Required)
    // ========================================================================
    // detectedAggression uses directional coherence:
    //   - INITIATIVE: isExtremeDelta AND sign(sessionDeltaPct) matches direction
    //   - RESPONSIVE: otherwise (includes incoherent extreme or non-extreme)
    AMT::AggressionType detectedAggression = AMT::AggressionType::RESPONSIVE;
    bool directionalCoherence = false;   // True if session delta sign matches direction
};

// ============================================================================
// LADDER FUNCTION (SSOT - Persistence-Validated Extreme Delta)
// ============================================================================

inline ArbitrationResult EvaluateArbitrationLadder(const ArbitrationInput& in) {
    using namespace AMT_Arb;

    ArbitrationResult out;

    // ========================================================================
    // EXTREME DELTA DETECTION (Persistence-Validated)
    // ========================================================================
    // Per-bar extreme: 70%+ one-sided volume in EITHER direction
    // BUG FIX: Check both buying (>0.7) AND selling (<0.3) extremes
    // VALIDITY GATE: If deltaConsistency is invalid (thin bar), cannot detect extreme
    out.isExtremeDeltaBar = in.deltaConsistencyValid &&
        (in.deltaConsistency > EXTREME_DELTA_HIGH_THRESHOLD ||
         in.deltaConsistency < EXTREME_DELTA_LOW_THRESHOLD);

    // Session extreme: percentile-based persistence check
    // Only valid when session has accumulated sufficient data for distribution
    out.isExtremeDeltaSession = in.sessionDeltaValid &&
        (in.sessionDeltaPctile >= SESSION_EXTREME_PCTILE_THRESHOLD);

    // Combined extreme: requires BOTH bar extremity AND session persistence
    // This is the NEW definition - eliminates false positives from single-bar spikes
    out.isExtremeDelta = out.isExtremeDeltaBar && out.isExtremeDeltaSession;

    // ========================================================================
    // DIRECTIONAL COHERENCE (for Aggression Classification)
    // ========================================================================
    // Coherence: session delta sign must match the directional bias
    // When isDirectional (trending up), session delta should be positive (net buying)
    // When isDirectional (trending down), session delta should be negative (net selling)
    // For now, we use a simplified coherence: session delta sign matches implied direction
    // from the bar's delta (positive delta = buying pressure = upward bias)
    //
    // VALIDITY GATES:
    //   - If sessionDeltaValid is false, cannot determine session direction
    //   - If deltaConsistencyValid is false, bar direction is unknown (treated as neutral/false)
    const bool deltaPositive = (in.sessionDeltaPct > 0.0);
    const bool barDeltaPositive = in.deltaConsistencyValid && (in.deltaConsistency > 0.5);
    out.directionalCoherence = in.sessionDeltaValid && in.deltaConsistencyValid &&
        (deltaPositive == barDeltaPositive);

    // ========================================================================
    // AGGRESSION CLASSIFICATION (Coherence-Gated)
    // ========================================================================
    // INITIATIVE: extreme delta with coherent session direction (attack)
    // RESPONSIVE: non-extreme OR incoherent direction (defense/absorption)
    out.detectedAggression = (out.isExtremeDelta && out.directionalCoherence)
        ? AMT::AggressionType::INITIATIVE
        : AMT::AggressionType::RESPONSIVE;

    // ========================================================================
    // ARBITRATION LADDER
    // ========================================================================
    out.arbReason = ARB_DEFAULT_BASELINE;
    out.useZones = false;
    out.engagedZoneId = -1;

    // Gate 0: INVALID_ANCHOR_IDS
    if (in.pocId < 0 || in.vahId < 0 || in.valId < 0) {
        out.arbReason = ARB_INVALID_ANCHOR_IDS;
    }
    // Gate 1: INVALID_ZONE_PTRS
    else if (!in.pocValid || !in.vahValid || !in.valValid) {
        out.arbReason = ARB_INVALID_ZONE_PTRS;
    }
    // Gate 2: NOT_READY
    else if (!in.zonesInitialized) {
        out.arbReason = ARB_NOT_READY;
    }
    // Gate 3: INVALID_VBP_PRICES
    else if (in.vbpPoc <= 0.0 || in.vbpVah <= 0.0 || in.vbpVal <= 0.0) {
        out.arbReason = ARB_INVALID_VBP_PRICES;
    }
    // Gate 4: INVALID_VA_ORDER
    else if (in.vbpVah <= in.vbpVal) {
        out.arbReason = ARB_INVALID_VA_ORDER;
    }
    // Gate 5: VBP_STALE
    else if (in.barsSinceLastCompute >= MAX_VBP_STALE_BARS) {
        out.arbReason = ARB_VBP_STALE;
    }
    // Gate 6: ENGAGED
    else if (in.pocProximity == AMT::ZoneProximity::AT_ZONE ||
             in.vahProximity == AMT::ZoneProximity::AT_ZONE ||
             in.valProximity == AMT::ZoneProximity::AT_ZONE) {
        out.arbReason = ARB_ENGAGED;
        out.useZones = true;
        // Priority: POC > VAH > VAL
        if (in.pocProximity == AMT::ZoneProximity::AT_ZONE) {
            out.engagedZoneId = in.pocId;
        } else if (in.vahProximity == AMT::ZoneProximity::AT_ZONE) {
            out.engagedZoneId = in.vahId;
        } else {
            out.engagedZoneId = in.valId;
        }
    }
    // Gate 7: DIRECTIONAL
    else if (in.isDirectional) {
        out.arbReason = ARB_DIRECTIONAL;
        out.useZones = true;
    }
    // Gate 8: BASELINE_EXTREME (now uses persistence-validated isExtremeDelta)
    else if (out.isExtremeDelta) {
        out.arbReason = ARB_BASELINE_EXTREME;
    }
    // Gate 9: DEFAULT_BASELINE (implicit, arbReason already set)

    // Derived: pocProx
    out.pocProx = in.pocValid ? static_cast<int>(in.pocProximity) : -1;

    // Derived: rawState (uses persistence-validated isExtremeDelta)
    out.rawState = (in.isDirectional || out.isExtremeDelta)
        ? AMT::AMTMarketState::IMBALANCE
        : AMT::AMTMarketState::BALANCE;

    return out;
}

#endif // AMT_ARBITRATION_SEAM_H
