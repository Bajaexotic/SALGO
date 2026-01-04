// ============================================================================
// AMT_TuningTelemetry.h
// TELEMETRY ONLY: Advisory computations for friction/volatility tuning analysis
//
// CONTRACT: This header provides OBSERVATIONAL data for telemetry/diagnostics.
// NO behavioral changes, NO gating, NO decision modifications.
// All advisory fields are computed for logging ONLY.
// ============================================================================

#ifndef AMT_TUNING_TELEMETRY_H
#define AMT_TUNING_TELEMETRY_H

#include "amt_core.h"

namespace AMT {

// ============================================================================
// TUNING ADVISORY OFFSETS (from Consumer Tuning v0 spec)
// These would be applied IF tuning were enabled - telemetry only for now
//
// NOTE: LOCKED is a HARD BLOCK, not a threshold adjustment.
// Use wouldBlockIfLocked boolean as the authoritative indicator.
// thresholdOffset contains only real-valued adjustments (no sentinels).
// ============================================================================
namespace TuningOffsets {
    // Friction-based threshold offsets (would modify confidence threshold)
    // NOTE: LOCKED has no offset - it's a hard block (see wouldBlockIfLocked)
    static constexpr float WIDE_THRESHOLD_OFFSET = 0.05f;     // Would require higher confidence
    static constexpr float TIGHT_THRESHOLD_OFFSET = -0.02f;   // Would allow lower confidence
    static constexpr float NORMAL_THRESHOLD_OFFSET = 0.0f;    // No change

    // Volatility-based confirmation bar deltas (would modify confirmation requirement)
    static constexpr int INDECISIVE_CONFIRMATION_DELTA = +1;  // High range + low travel
    static constexpr int BREAKOUT_POTENTIAL_CONFIRMATION_DELTA = -1;  // Low range + high travel
    static constexpr int TRENDING_CONFIRMATION_DELTA = 0;     // High range + high travel
    static constexpr int COMPRESSED_CONFIRMATION_DELTA = 0;   // Low range + low travel
    static constexpr int DEFAULT_CONFIRMATION_DELTA = 0;      // Baseline
}

// ============================================================================
// VolatilityCharacter: 2D volatility classification labels
// Derived from range percentile + close-change percentile
// ============================================================================
enum class VolatilityCharacter : int {
    UNKNOWN = 0,           // Baselines not ready
    COMPRESSED = 1,        // Low range + low travel
    TRENDING = 2,          // High range + high travel
    INDECISIVE = 3,        // High range + low travel (choppy)
    BREAKOUT_POTENTIAL = 4,// Low range + high travel (coiled)
    NORMAL = 5             // Neither extreme
};

inline const char* to_string(VolatilityCharacter c) {
    switch (c) {
        case VolatilityCharacter::COMPRESSED: return "COMPRESSED";
        case VolatilityCharacter::TRENDING: return "TRENDING";
        case VolatilityCharacter::INDECISIVE: return "INDECISIVE";
        case VolatilityCharacter::BREAKOUT_POTENTIAL: return "BREAKOUT_POT";
        case VolatilityCharacter::NORMAL: return "NORMAL";
        default: return "UNKNOWN";
    }
}

// ============================================================================
// Classify 2D volatility character from percentiles
// Returns UNKNOWN if closeChangeValid is false
// ============================================================================
inline VolatilityCharacter Classify2DVolatilityCharacter(
    double rangePctile,
    double closeChangePctile,
    bool closeChangeValid)
{
    if (!closeChangeValid) {
        return VolatilityCharacter::UNKNOWN;
    }

    const bool highRange = (rangePctile >= 75.0);
    const bool lowRange = (rangePctile <= 25.0);
    const bool highTravel = (closeChangePctile >= 75.0);
    const bool lowTravel = (closeChangePctile <= 25.0);

    if (lowRange && lowTravel) return VolatilityCharacter::COMPRESSED;
    if (highRange && highTravel) return VolatilityCharacter::TRENDING;
    if (highRange && lowTravel) return VolatilityCharacter::INDECISIVE;
    if (lowRange && highTravel) return VolatilityCharacter::BREAKOUT_POTENTIAL;

    return VolatilityCharacter::NORMAL;
}

// ============================================================================
// TuningAdvisory: Computed advisories (TELEMETRY ONLY)
// These values are NEVER used for decisions - only logged for analysis
//
// LOCKED HANDLING:
//   - wouldBlockIfLocked is the AUTHORITATIVE indicator for hard blocks
//   - thresholdOffset is 0.0 for LOCKED (not a threshold adjustment)
//   - Never use thresholdOffset as a sentinel for blocking
// ============================================================================
struct TuningAdvisory {
    // Friction advisories
    bool wouldBlockIfLocked = false;     // AUTHORITATIVE: True iff frictionValid && friction==LOCKED
    float thresholdOffset = 0.0f;        // Real-valued offset only (no sentinels)

    // Volatility advisories
    VolatilityCharacter character = VolatilityCharacter::UNKNOWN;
    int confirmationDelta = 0;           // Confirmation bars that WOULD be added/removed

    // Computed from inputs - call ComputeAdvisories() to populate
    void ComputeAdvisories(ExecutionFriction friction, bool frictionValid,
                           double rangePctile, double closeChangePctile, bool closeChangeValid)
    {
        // Friction advisory
        if (frictionValid) {
            wouldBlockIfLocked = (friction == ExecutionFriction::LOCKED);
            switch (friction) {
                case ExecutionFriction::LOCKED:
                    // LOCKED is a hard block, not a threshold adjustment
                    // Use wouldBlockIfLocked as the authoritative indicator
                    thresholdOffset = 0.0f;
                    break;
                case ExecutionFriction::WIDE:
                    thresholdOffset = TuningOffsets::WIDE_THRESHOLD_OFFSET;
                    break;
                case ExecutionFriction::TIGHT:
                    thresholdOffset = TuningOffsets::TIGHT_THRESHOLD_OFFSET;
                    break;
                default:
                    thresholdOffset = TuningOffsets::NORMAL_THRESHOLD_OFFSET;
                    break;
            }
        } else {
            wouldBlockIfLocked = false;
            thresholdOffset = 0.0f;
        }

        // Volatility advisory
        character = Classify2DVolatilityCharacter(rangePctile, closeChangePctile, closeChangeValid);
        switch (character) {
            case VolatilityCharacter::INDECISIVE:
                confirmationDelta = TuningOffsets::INDECISIVE_CONFIRMATION_DELTA;
                break;
            case VolatilityCharacter::BREAKOUT_POTENTIAL:
                confirmationDelta = TuningOffsets::BREAKOUT_POTENTIAL_CONFIRMATION_DELTA;
                break;
            default:
                confirmationDelta = TuningOffsets::DEFAULT_CONFIRMATION_DELTA;
                break;
        }
    }
};

// ============================================================================
// EngagementTelemetryRecord: Full telemetry record for engagement start
// Emitted when a zone transitions to AT_ZONE (StartEngagement boundary)
// ============================================================================
struct EngagementTelemetryRecord {
    // Zone identity
    int zoneId = -1;
    ZoneType zoneType = ZoneType::NONE;

    // Bar/time context
    int bar = -1;
    double price = 0.0;

    // Friction state
    ExecutionFriction friction = ExecutionFriction::UNKNOWN;
    bool frictionValid = false;
    double spreadTicks = 0.0;      // Only if available, else 0
    double spreadPctile = 0.0;     // Only if available, else 0
    bool spreadBaselineReady = false;

    // Volatility state
    VolatilityState volatility = VolatilityState::NORMAL;
    bool volatilityValid = false;
    bool closeChangeValid = false;  // 2D refinement available?
    double rangePctile = 0.0;
    double closeChangePctile = 0.0;

    // Market composition (optional)
    float marketComposition = 0.0f;
    bool marketCompositionValid = false;

    // Advisories (TELEMETRY ONLY)
    TuningAdvisory advisory;
};

// ============================================================================
// ArbitrationTelemetryRecord: Full telemetry record for arbitration decision
// Emitted when arbitration ladder produces a decision
// ============================================================================
struct ArbitrationTelemetryRecord {
    // Arbitration outcome
    int arbReason = 0;
    bool useZones = false;
    int engagedZoneId = -1;

    // Bar context
    int bar = -1;
    double price = 0.0;

    // Friction state (same as engagement)
    ExecutionFriction friction = ExecutionFriction::UNKNOWN;
    bool frictionValid = false;

    // Volatility state (same as engagement)
    VolatilityState volatility = VolatilityState::NORMAL;
    bool volatilityValid = false;
    VolatilityCharacter character = VolatilityCharacter::UNKNOWN;

    // Market composition
    float marketComposition = 0.0f;
    bool marketCompositionValid = false;

    // Advisories (TELEMETRY ONLY)
    TuningAdvisory advisory;
};

} // namespace AMT

#endif // AMT_TUNING_TELEMETRY_H
