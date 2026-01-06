#pragma once
// ============================================================================
// AMT_DeltaEngine.h - Delta Participation Pressure Engine
// ============================================================================
//
// PHILOSOPHY: Delta is PARTICIPATION PRESSURE, not "bull/bear".
// It measures WHO is more aggressive in fulfilling their order, not WHO is right.
//
// KEY INSIGHT: A strong negative delta at a low doesn't mean "sellers winning" -
// it means aggressive sellers are HITTING into passive buyers. The buyers who
// absorb without moving price are often the informed party.
//
// FIVE QUESTIONS THIS ENGINE ANSWERS:
//
//   1. CHARACTER: Is aggression sustained or episodic? (trend vs burst)
//      - SUSTAINED: Multiple bars of aligned delta (conviction, follow)
//      - EPISODIC: Single-bar spikes that fade (noise, fade)
//
//   2. ALIGNMENT: Is delta aligned with price or diverging? (efficiency flag)
//      - CONVERGENT: Delta and price agree (efficient, trustworthy)
//      - DIVERGENT: Delta opposes price (absorption, reversal warning)
//      - NEUTRAL: Low delta, low signal content
//
//   3. NOISE FLOOR: What's the baseline-relative magnitude today? (normalization)
//      - Phase-aware percentiles (GLOBEX != RTH)
//      - Separate bar-level and session-level baselines
//
//   4. CONFIDENCE GATE: When should I downgrade confidence?
//      - Low volume (thin tape)
//      - High chop (frequent reversals)
//      - Extreme one-sidedness (exhaustion risk)
//
//   5. DOWNSTREAM DECISIONS: What trading constraints apply?
//      - Block continuation triggers on divergence
//      - Require delta alignment for breakout confirmation
//      - Reduce size on episodic patterns
//
// ARCHITECTURE:
//   - Follows LiquidityEngine/VolatilityEngine pattern
//   - Phase-aware baselines via EffortBaselineStore and SessionDeltaBaseline
//   - Hysteresis prevents character/alignment whipsaw
//   - NO-FALLBACK contract: every output has explicit validity
//
// INTEGRATION:
//   DeltaEngine deltaEngine;
//   deltaEngine.SetEffortStore(&effortStore);
//   deltaEngine.SetSessionDeltaBaseline(&sessionDeltaBaseline);
//   deltaEngine.SetPhase(currentPhase);
//
//   DeltaResult result = deltaEngine.Compute(barDelta, barVolume, priceChange,
//                                             sessionCumDelta, sessionVolume);
//   if (result.IsReady()) {
//       if (result.character == DeltaCharacter::SUSTAINED &&
//           result.alignment == DeltaAlignment::CONVERGENT) {
//           // High confidence continuation signal
//       }
//   }
//
// ============================================================================

#include "amt_core.h"
#include "AMT_Snapshots.h"
#include "AMT_Invariants.h"
#include "AMT_ValueLocation.h"  // For ValueZone (SSOT) and ValueLocationResult
#include "AMT_Volatility.h"     // For VolatilityRegime (context gating)
#include <algorithm>
#include <cmath>
#include <deque>

namespace AMT {

// ============================================================================
// DELTA CHARACTER - Sustained vs Episodic
// ============================================================================
// Answers: "Is this aggression a trend or a burst?"
//
// SUSTAINED: Aggression persists across multiple bars in same direction
//   - Evidence of conviction
//   - Trend-following appropriate
//   - Higher confidence in continuation
//
// EPISODIC: Single-bar spike that doesn't persist
//   - Often exhaustion or news reaction
//   - Fade opportunity or noise
//   - Lower confidence in follow-through
//
// BUILDING: Aggression increasing bar-over-bar
//   - Momentum accelerating
//   - Trend intensifying
//
// FADING: Aggression decreasing bar-over-bar
//   - Momentum exhausting
//   - Potential reversal setup
//
// REVERSAL: Aggression flipped direction
//   - Active trend change
//   - High signal content
// ============================================================================

enum class DeltaCharacter : int {
    UNKNOWN = 0,          // Baseline not ready
    NEUTRAL,              // Delta within noise band (no signal)
    EPISODIC,             // Single-bar spike (burst, may fade)
    SUSTAINED,            // Multi-bar aligned (trend, conviction)
    BUILDING,             // Increasing magnitude (acceleration)
    FADING,               // Decreasing magnitude (deceleration)
    REVERSAL              // Direction flip (high signal)
};

inline const char* DeltaCharacterToString(DeltaCharacter c) {
    switch (c) {
        case DeltaCharacter::UNKNOWN:   return "UNKNOWN";
        case DeltaCharacter::NEUTRAL:   return "NEUTRAL";
        case DeltaCharacter::EPISODIC:  return "EPISODIC";
        case DeltaCharacter::SUSTAINED: return "SUSTAINED";
        case DeltaCharacter::BUILDING:  return "BUILDING";
        case DeltaCharacter::FADING:    return "FADING";
        case DeltaCharacter::REVERSAL:  return "REVERSAL";
    }
    return "UNK";
}

inline const char* DeltaCharacterShort(DeltaCharacter c) {
    switch (c) {
        case DeltaCharacter::UNKNOWN:   return "?";
        case DeltaCharacter::NEUTRAL:   return "N";
        case DeltaCharacter::EPISODIC:  return "E";
        case DeltaCharacter::SUSTAINED: return "S";
        case DeltaCharacter::BUILDING:  return "B";
        case DeltaCharacter::FADING:    return "F";
        case DeltaCharacter::REVERSAL:  return "R";
    }
    return "?";
}

// ============================================================================
// DELTA ALIGNMENT - Price vs Delta Relationship
// ============================================================================
// Answers: "Is aggression producing efficient price movement?"
//
// CONVERGENT: Delta direction matches price direction
//   - Price up + positive delta = aggressive buyers moving price up (efficient)
//   - Price down + negative delta = aggressive sellers moving price down (efficient)
//   - High confidence in trend
//
// DIVERGENT: Delta direction opposes price direction
//   - Price up + negative delta = price rising on selling (absorption at low)
//   - Price down + positive delta = price falling on buying (absorption at high)
//   - ABSORPTION signal: passive side is informed, aggressive side is wrong
//   - Reversal warning
//
// NEUTRAL: Neither direction has meaningful delta
//   - Low participation, low signal content
//   - Avoid trading
// ============================================================================

enum class DeltaAlignment : int {
    UNKNOWN = 0,          // Baseline not ready
    NEUTRAL,              // Low delta, low signal (avoid)
    CONVERGENT,           // Delta aligns with price (efficient, follow)
    DIVERGENT,            // Delta opposes price (absorption, fade)
    ABSORPTION_BID,       // Passive buyers absorbing at low (bullish divergence)
    ABSORPTION_ASK        // Passive sellers absorbing at high (bearish divergence)
};

inline const char* DeltaAlignmentToString(DeltaAlignment a) {
    switch (a) {
        case DeltaAlignment::UNKNOWN:        return "UNKNOWN";
        case DeltaAlignment::NEUTRAL:        return "NEUTRAL";
        case DeltaAlignment::CONVERGENT:     return "CONVERGENT";
        case DeltaAlignment::DIVERGENT:      return "DIVERGENT";
        case DeltaAlignment::ABSORPTION_BID: return "ABSORB_BID";
        case DeltaAlignment::ABSORPTION_ASK: return "ABSORB_ASK";
    }
    return "UNK";
}

inline const char* DeltaAlignmentShort(DeltaAlignment a) {
    switch (a) {
        case DeltaAlignment::UNKNOWN:        return "?";
        case DeltaAlignment::NEUTRAL:        return "N";
        case DeltaAlignment::CONVERGENT:     return "C";
        case DeltaAlignment::DIVERGENT:      return "D";
        case DeltaAlignment::ABSORPTION_BID: return "Ab";
        case DeltaAlignment::ABSORPTION_ASK: return "Aa";
    }
    return "?";
}

// ============================================================================
// DELTA CONFIDENCE - When to Trust Delta Signals
// ============================================================================
// Not all delta readings are equally trustworthy.
//
// FULL: Volume adequate, no red flags
//   - Normal trading conditions
//   - Full weight to delta signals
//
// DEGRADED: Some concern, proceed with caution
//   - Low volume but not critically thin
//   - High chop but not extreme
//   - Reduce position size or require confirmation
//
// LOW: Significant concern, tighten requirements
//   - Very low volume (thin tape)
//   - Extreme one-sidedness (exhaustion risk)
//   - Require additional confirmation
//
// BLOCKED: Do not use delta for decisions
//   - Critical conditions (holiday, flash crash)
//   - Baseline not ready
//   - Skip delta-dependent signals
// ============================================================================

enum class DeltaConfidence : int {
    UNKNOWN = 0,
    BLOCKED,              // Do not use delta (critical conditions)
    LOW,                  // Significant concern (tighten requirements)
    DEGRADED,             // Some concern (proceed with caution)
    FULL                  // Normal conditions (full weight)
};

inline const char* DeltaConfidenceToString(DeltaConfidence c) {
    switch (c) {
        case DeltaConfidence::UNKNOWN:  return "UNKNOWN";
        case DeltaConfidence::BLOCKED:  return "BLOCKED";
        case DeltaConfidence::LOW:      return "LOW";
        case DeltaConfidence::DEGRADED: return "DEGRADED";
        case DeltaConfidence::FULL:     return "FULL";
    }
    return "UNK";
}

// ============================================================================
// DELTA ERROR TAXONOMY
// ============================================================================
// Explicit tracking of why delta may be invalid.

enum class DeltaErrorReason : int {
    NONE = 0,

    // Warmup states (expected, not errors)
    WARMUP_BAR_BASELINE = 10,        // Bar-level delta baseline not ready
    WARMUP_SESSION_BASELINE = 11,    // Session-level delta baseline not ready
    WARMUP_BOTH = 12,                // Both baselines not ready
    WARMUP_VOLUME = 13,              // Volume baseline not ready

    // Input errors
    ERR_INVALID_INPUT = 20,          // NaN or invalid delta/volume
    ERR_ZERO_VOLUME = 21,            // Zero volume (can't compute deltaPct)
    ERR_NO_BASELINE_STORE = 22,      // EffortBaselineStore not configured

    // Confidence degradation reasons (multiple can apply)
    WARN_THIN_TAPE = 30,             // Volume below P10 (thin tape warning)
    WARN_HIGH_CHOP = 31,             // Frequent reversals detected
    WARN_EXHAUSTION = 32,            // Extreme one-sidedness (>P95 delta)
    WARN_GLOBEX_HOURS = 33,          // GLOBEX session (inherently lower confidence)

    // Session events
    SESSION_RESET = 40,              // Session just reset, no delta history

    // Context gate blocks (from external engines)
    BLOCKED_LIQUIDITY_VOID = 50,     // LiquidityState::LIQ_VOID
    BLOCKED_LIQUIDITY_THIN = 51,     // LiquidityState::LIQ_THIN (configurable)
    BLOCKED_VOLATILITY_EVENT = 52,   // VolatilityRegime::EVENT

    // Context gate degradation (not blocked, but reduced confidence)
    DEGRADED_VOLATILITY_COMPRESSION = 53,  // COMPRESSION regime
    DEGRADED_HIGH_STRESS = 54        // High liquidity stress (stressRank >= 0.90)
};

inline const char* DeltaErrorToString(DeltaErrorReason r) {
    switch (r) {
        case DeltaErrorReason::NONE:                    return "NONE";
        case DeltaErrorReason::WARMUP_BAR_BASELINE:     return "WARMUP_BAR";
        case DeltaErrorReason::WARMUP_SESSION_BASELINE: return "WARMUP_SESSION";
        case DeltaErrorReason::WARMUP_BOTH:             return "WARMUP_BOTH";
        case DeltaErrorReason::WARMUP_VOLUME:           return "WARMUP_VOLUME";
        case DeltaErrorReason::ERR_INVALID_INPUT:       return "INVALID_INPUT";
        case DeltaErrorReason::ERR_ZERO_VOLUME:         return "ZERO_VOLUME";
        case DeltaErrorReason::ERR_NO_BASELINE_STORE:   return "NO_BASELINE";
        case DeltaErrorReason::WARN_THIN_TAPE:          return "THIN_TAPE";
        case DeltaErrorReason::WARN_HIGH_CHOP:          return "HIGH_CHOP";
        case DeltaErrorReason::WARN_EXHAUSTION:         return "EXHAUSTION";
        case DeltaErrorReason::WARN_GLOBEX_HOURS:       return "GLOBEX";
        case DeltaErrorReason::SESSION_RESET:           return "SESSION_RESET";
        case DeltaErrorReason::BLOCKED_LIQUIDITY_VOID:  return "BLOCKED_LIQ_VOID";
        case DeltaErrorReason::BLOCKED_LIQUIDITY_THIN:  return "BLOCKED_LIQ_THIN";
        case DeltaErrorReason::BLOCKED_VOLATILITY_EVENT: return "BLOCKED_VOL_EVENT";
        case DeltaErrorReason::DEGRADED_VOLATILITY_COMPRESSION: return "DEGRADE_COMPRESS";
        case DeltaErrorReason::DEGRADED_HIGH_STRESS:    return "DEGRADE_STRESS";
    }
    return "UNK";
}

inline bool IsDeltaWarmup(DeltaErrorReason r) {
    return r == DeltaErrorReason::WARMUP_BAR_BASELINE ||
           r == DeltaErrorReason::WARMUP_SESSION_BASELINE ||
           r == DeltaErrorReason::WARMUP_BOTH ||
           r == DeltaErrorReason::WARMUP_VOLUME;
}

inline bool IsDeltaWarning(DeltaErrorReason r) {
    return r == DeltaErrorReason::WARN_THIN_TAPE ||
           r == DeltaErrorReason::WARN_HIGH_CHOP ||
           r == DeltaErrorReason::WARN_EXHAUSTION ||
           r == DeltaErrorReason::WARN_GLOBEX_HOURS;
}

inline bool IsDeltaContextBlocked(DeltaErrorReason r) {
    return r == DeltaErrorReason::BLOCKED_LIQUIDITY_VOID ||
           r == DeltaErrorReason::BLOCKED_LIQUIDITY_THIN ||
           r == DeltaErrorReason::BLOCKED_VOLATILITY_EVENT;
}

inline bool IsDeltaContextDegraded(DeltaErrorReason r) {
    return r == DeltaErrorReason::DEGRADED_VOLATILITY_COMPRESSION ||
           r == DeltaErrorReason::DEGRADED_HIGH_STRESS;
}

// ============================================================================
// THIN TAPE TYPE - Enhanced Thin Tape Classification
// ============================================================================
// Distinguishes different types of low activity conditions:
//
// TRUE_THIN: Low volume + low trades = genuine low participation
//   - No real market interest
//   - Signals unreliable
//
// HFT_FRAGMENTED: Low volume + high trades = many small orders
//   - HFT activity but no real size
//   - Price can move on noise
//
// INSTITUTIONAL: High volume + low trades = large block orders
//   - Informed institutional activity
//   - Signals MORE reliable
// ============================================================================

enum class ThinTapeType : int {
    NONE = 0,           // Normal activity
    TRUE_THIN,          // Low volume + low trades (no participation)
    HFT_FRAGMENTED,     // Low volume + high trades (HFT noise)
    INSTITUTIONAL       // High volume + low trades (block trades)
};

inline const char* ThinTapeTypeToString(ThinTapeType t) {
    switch (t) {
        case ThinTapeType::NONE:           return "NONE";
        case ThinTapeType::TRUE_THIN:      return "TRUE_THIN";
        case ThinTapeType::HFT_FRAGMENTED: return "HFT_FRAG";
        case ThinTapeType::INSTITUTIONAL:  return "INSTIT";
    }
    return "?";
}

// ============================================================================
// DELTA LOCATION CONTEXT (AMT Value-Relative Awareness)
// ============================================================================
// The DeltaEngine CONSUMES location context from ValueLocationEngine.
// It does NOT own or compute value levels - it interprets delta relative to them.
//
// KEY AMT INSIGHT: Delta is only meaningful relative to where the auction is.
//   - At POC: lower delta expected (rotation)
//   - At VAH/VAL edges: higher delta expected (breakout/rejection attempts)
//   - Outside value: sustained delta expected (discovery/acceptance)
// ============================================================================

enum class ValueZoneSimple : int {
    UNKNOWN = 0,
    IN_VALUE,           // Between VAH and VAL
    AT_VALUE_EDGE,      // At or near VAH/VAL (within tolerance)
    OUTSIDE_VALUE,      // Beyond VAH or VAL
    IN_DISCOVERY        // Far outside value, sustained move
};

inline const char* ValueZoneSimpleToString(ValueZoneSimple z) {
    switch (z) {
        case ValueZoneSimple::UNKNOWN:        return "UNKNOWN";
        case ValueZoneSimple::IN_VALUE:       return "IN_VALUE";
        case ValueZoneSimple::AT_VALUE_EDGE:  return "AT_EDGE";
        case ValueZoneSimple::OUTSIDE_VALUE:  return "OUTSIDE";
        case ValueZoneSimple::IN_DISCOVERY:   return "DISCOVERY";
    }
    return "?";
}

// ============================================================================
// SSOT MAPPING: ValueZone (SSOT) -> ValueZoneSimple (simplified for delta)
// ============================================================================
// ValueZone is the SSOT from ValueLocationEngine (9 states).
// ValueZoneSimple is a simplified 5-state representation for delta interpretation.
// This mapping ensures DeltaEngine consumes from SSOT rather than computing its own.

inline ValueZoneSimple MapValueZoneToSimple(ValueZone zone) {
    switch (zone) {
        // POC and value interior -> IN_VALUE
        case ValueZone::AT_POC:
        case ValueZone::UPPER_VALUE:
        case ValueZone::LOWER_VALUE:
            return ValueZoneSimple::IN_VALUE;

        // Value edges -> AT_VALUE_EDGE
        case ValueZone::AT_VAH:
        case ValueZone::AT_VAL:
            return ValueZoneSimple::AT_VALUE_EDGE;

        // Near outside -> OUTSIDE_VALUE
        case ValueZone::NEAR_ABOVE_VALUE:
        case ValueZone::NEAR_BELOW_VALUE:
            return ValueZoneSimple::OUTSIDE_VALUE;

        // Far outside -> IN_DISCOVERY
        case ValueZone::FAR_ABOVE_VALUE:
        case ValueZone::FAR_BELOW_VALUE:
            return ValueZoneSimple::IN_DISCOVERY;

        default:
            return ValueZoneSimple::UNKNOWN;
    }
}

struct DeltaLocationContext {
    // Zone classification (simplified for delta interpretation)
    ValueZoneSimple zone = ValueZoneSimple::UNKNOWN;

    // Distance from key levels (in ticks, signed: + = above, - = below)
    double distanceFromPOCTicks = 0.0;
    double distanceFromVAHTicks = 0.0;
    double distanceFromVALTicks = 0.0;

    // Convenience flags
    bool isInValue = false;          // Between VAH and VAL
    bool isAtEdge = false;           // At VAH or VAL (within tolerance)
    bool isOutsideValue = false;     // Beyond VAH or below VAL
    bool isInDiscovery = false;      // Far outside + sustained

    // Migration context (is value moving toward or away from price?)
    bool isMigratingTowardPrice = false;  // POC moving toward current price
    bool isMigratingAwayFromPrice = false; // POC moving away (value rejecting)

    // Structure context
    bool isAboveSessionHigh = false;
    bool isBelowSessionLow = false;
    bool isAtIBExtreme = false;      // At IB high or low

    // Validity
    bool isValid = false;

    // =========================================================================
    // PREFERRED: Build from ValueLocationResult (SSOT-compliant)
    // =========================================================================
    // ValueLocationEngine is the SSOT for value location. This method consumes
    // its output rather than duplicating the classification logic.
    // NOTE: sessionHigh/Low and ibHigh/Low are already computed into
    // distToSessionHighTicks/etc in ValueLocationResult, so not needed here.
    static DeltaLocationContext BuildFromValueLocation(
        const ValueLocationResult& valLocResult,
        double edgeToleranceTicks = 2.0)
    {
        DeltaLocationContext ctx;

        if (!valLocResult.IsReady()) {
            ctx.isValid = false;
            return ctx;
        }

        // Map SSOT ValueZone to simplified ValueZoneSimple
        ctx.zone = MapValueZoneToSimple(valLocResult.confirmedZone);

        // Copy distances from SSOT
        ctx.distanceFromPOCTicks = valLocResult.distFromPOCTicks;
        ctx.distanceFromVAHTicks = valLocResult.distFromVAHTicks;
        ctx.distanceFromVALTicks = valLocResult.distFromVALTicks;

        // Set convenience flags based on zone
        ctx.isInValue = (ctx.zone == ValueZoneSimple::IN_VALUE);
        ctx.isAtEdge = (ctx.zone == ValueZoneSimple::AT_VALUE_EDGE);
        ctx.isOutsideValue = (ctx.zone == ValueZoneSimple::OUTSIDE_VALUE ||
                              ctx.zone == ValueZoneSimple::IN_DISCOVERY);
        ctx.isInDiscovery = (ctx.zone == ValueZoneSimple::IN_DISCOVERY);

        // Migration context from SSOT
        ctx.isMigratingTowardPrice = (valLocResult.valueMigration == ValueMigration::HIGHER &&
                                       valLocResult.distFromPOCTicks > 0) ||
                                     (valLocResult.valueMigration == ValueMigration::LOWER &&
                                       valLocResult.distFromPOCTicks < 0);
        ctx.isMigratingAwayFromPrice = (valLocResult.valueMigration == ValueMigration::HIGHER &&
                                         valLocResult.distFromPOCTicks < 0) ||
                                       (valLocResult.valueMigration == ValueMigration::LOWER &&
                                         valLocResult.distFromPOCTicks > 0);

        // Structure context from SSOT (ValueLocationResult has session/IB tick distances)
        ctx.isAboveSessionHigh = valLocResult.distToSessionHighTicks > 0 &&
                                 valLocResult.confirmedZone == ValueZone::FAR_ABOVE_VALUE;
        ctx.isBelowSessionLow = valLocResult.distToSessionLowTicks < 0 &&
                                valLocResult.confirmedZone == ValueZone::FAR_BELOW_VALUE;

        // IB extreme detection from SSOT tick distances
        ctx.isAtIBExtreme = (std::abs(valLocResult.distToIBHighTicks) <= edgeToleranceTicks) ||
                            (std::abs(valLocResult.distToIBLowTicks) <= edgeToleranceTicks);

        ctx.isValid = true;
        return ctx;
    }

    // =========================================================================
    // DEPRECATED: Build from raw values (duplicates ValueLocationEngine logic)
    // =========================================================================
    // This method computes its own location classification, which duplicates
    // ValueLocationEngine. Use BuildFromValueLocation() instead.
    [[deprecated("Use BuildFromValueLocation() with ValueLocationResult from ValueLocationEngine (SSOT)")]]
    static DeltaLocationContext Build(
        double currentPrice,
        double poc, double vah, double val,
        double tickSize,
        double edgeToleranceTicks = 2.0,
        double discoveryThresholdTicks = 8.0,
        double sessionHigh = 0.0, double sessionLow = 0.0,
        double ibHigh = 0.0, double ibLow = 0.0,
        double priorPOC = 0.0)
    {
        DeltaLocationContext ctx;

        if (tickSize <= 0.0 || vah <= val) {
            ctx.isValid = false;
            return ctx;
        }

        // Calculate distances
        ctx.distanceFromPOCTicks = (currentPrice - poc) / tickSize;
        ctx.distanceFromVAHTicks = (currentPrice - vah) / tickSize;
        ctx.distanceFromVALTicks = (currentPrice - val) / tickSize;

        // Classify zone
        double distFromVAH = std::abs(ctx.distanceFromVAHTicks);
        double distFromVAL = std::abs(ctx.distanceFromVALTicks);

        if (distFromVAH <= edgeToleranceTicks) {
            ctx.zone = ValueZoneSimple::AT_VALUE_EDGE;
            ctx.isAtEdge = true;
        } else if (distFromVAL <= edgeToleranceTicks) {
            ctx.zone = ValueZoneSimple::AT_VALUE_EDGE;
            ctx.isAtEdge = true;
        } else if (currentPrice > vah) {
            if (ctx.distanceFromVAHTicks > discoveryThresholdTicks) {
                ctx.zone = ValueZoneSimple::IN_DISCOVERY;
                ctx.isInDiscovery = true;
            } else {
                ctx.zone = ValueZoneSimple::OUTSIDE_VALUE;
            }
            ctx.isOutsideValue = true;
        } else if (currentPrice < val) {
            if (std::abs(ctx.distanceFromVALTicks) > discoveryThresholdTicks) {
                ctx.zone = ValueZoneSimple::IN_DISCOVERY;
                ctx.isInDiscovery = true;
            } else {
                ctx.zone = ValueZoneSimple::OUTSIDE_VALUE;
            }
            ctx.isOutsideValue = true;
        } else {
            ctx.zone = ValueZoneSimple::IN_VALUE;
            ctx.isInValue = true;
        }

        // Migration context
        if (priorPOC > 0.0) {
            double pocShift = poc - priorPOC;
            double priceFromPOC = currentPrice - poc;
            // Migrating toward if POC moving in same direction as price relative to POC
            ctx.isMigratingTowardPrice = (pocShift * priceFromPOC > 0);
            ctx.isMigratingAwayFromPrice = (pocShift * priceFromPOC < 0);
        }

        // Structure context
        if (sessionHigh > 0.0 && sessionLow > 0.0) {
            ctx.isAboveSessionHigh = currentPrice > sessionHigh;
            ctx.isBelowSessionLow = currentPrice < sessionLow;
        }
        if (ibHigh > 0.0 && ibLow > 0.0) {
            ctx.isAtIBExtreme = (std::abs(currentPrice - ibHigh) <= edgeToleranceTicks * tickSize) ||
                               (std::abs(currentPrice - ibLow) <= edgeToleranceTicks * tickSize);
        }

        ctx.isValid = true;
        return ctx;
    }
};

// ============================================================================
// DELTA AUCTION PREDICTION (AMT Implication Flags)
// ============================================================================
// NOTE: This is DIFFERENT from amt_core.h's AuctionOutcome (PENDING/ACCEPTED/REJECTED)
// which is used for zone acceptance tracking.
//
// DeltaAuctionPrediction describes what delta analysis PREDICTS will happen:
//   - ACCEPTANCE_LIKELY: sustained + convergent + outside value + holding
//   - REJECTION_LIKELY: absorption + at edge + exhaustion signatures
//   - ROTATION_LIKELY: episodic + chop + at/near value center
// These are state descriptors for downstream engines, NOT trade signals.
// ============================================================================

enum class DeltaAuctionPrediction : int {
    UNKNOWN = 0,
    ACCEPTANCE_LIKELY,    // Market accepting new value (trend continuation)
    REJECTION_LIKELY,     // Market rejecting price level (reversal setup)
    ROTATION_LIKELY       // Market rotating in balance (mean reversion)
};

inline const char* DeltaAuctionPredictionToString(DeltaAuctionPrediction o) {
    switch (o) {
        case DeltaAuctionPrediction::UNKNOWN:           return "UNKNOWN";
        case DeltaAuctionPrediction::ACCEPTANCE_LIKELY: return "ACCEPT";
        case DeltaAuctionPrediction::REJECTION_LIKELY:  return "REJECT";
        case DeltaAuctionPrediction::ROTATION_LIKELY:   return "ROTATE";
    }
    return "?";
}

// ============================================================================
// DELTA CONTEXT GATE (from LiquidityEngine + VolatilityEngine + DaltonEngine)
// ============================================================================
// Results from checking external engine gates.
// Tells us if market context is suitable for trusting delta signals.
//
// This follows the pattern established by ContextGateResult in ImbalanceEngine.
// ============================================================================

struct DeltaContextGateResult {
    // Individual gate results
    bool liquidityOK = false;           // Not in VOID (or THIN if configured)
    bool volatilityOK = false;          // Not in EVENT regime
    bool compressionDegraded = false;   // In COMPRESSION (not blocked, but distrust breakouts)

    // Combined results
    bool allGatesPass = false;          // liquidityOK && volatilityOK
    bool contextValid = false;          // At least one context input was valid

    // Detailed state for diagnostics
    LiquidityState liqState = LiquidityState::LIQ_NOT_READY;
    VolatilityRegime volRegime = VolatilityRegime::UNKNOWN;
    double stressRank = 0.0;            // From LiquidityEngine [0, 1]
    bool highStress = false;            // stressRank >= threshold

    // Optional: Dalton market state awareness
    AMTMarketState daltonState = AMTMarketState::UNKNOWN;
    bool is1TF = false;                 // 1TF = trending (ONE_TIME_FRAMING)
    bool hasDaltonContext = false;      // Was Dalton state provided?

    // Block reason (if any)
    DeltaErrorReason blockReason = DeltaErrorReason::NONE;

    // Diagnostic accessors
    bool IsBlocked() const {
        return blockReason != DeltaErrorReason::NONE && IsDeltaContextBlocked(blockReason);
    }

    bool IsDegraded() const {
        return compressionDegraded || highStress || IsDeltaContextDegraded(blockReason);
    }
};

// ============================================================================
// TRADING CONSTRAINTS (Downstream Decisions)
// ============================================================================
// What constraints to apply based on delta state.

struct DeltaTradingConstraints {
    bool allowContinuation = true;       // Can take continuation signals
    bool allowBreakout = true;           // Can take breakout signals
    bool allowFade = true;               // Can fade (mean reversion)
    bool requireDeltaAlignment = false;  // Must have CONVERGENT delta
    bool requireSustained = false;       // Must have SUSTAINED character
    double positionSizeMultiplier = 1.0; // Scale position
    double confidenceWeight = 1.0;       // Weight in composite score

    bool IsRestricted() const {
        return !allowContinuation || !allowBreakout || requireDeltaAlignment;
    }
};

// ============================================================================
// DELTA RESULT (Per-Bar Output)
// ============================================================================
// Complete snapshot of delta state for current bar.

struct DeltaResult {
    // =========================================================================
    // RAW MEASUREMENTS
    // =========================================================================
    double barDelta = 0.0;              // Net delta this bar (ask - bid volume)
    double barVolume = 0.0;             // Total volume this bar
    double barDeltaPct = 0.0;           // Delta as % of volume (-1 to +1)
    double priceChangeTicks = 0.0;      // Price change in ticks (close - open)

    // Session aggregates
    double sessionCumDelta = 0.0;       // Cumulative session delta
    double sessionVolume = 0.0;         // Cumulative session volume
    double sessionDeltaPct = 0.0;       // Session delta as % of session volume

    // =========================================================================
    // BASELINE-RELATIVE (Noise Floor)
    // =========================================================================
    double barDeltaPctile = 0.0;        // Bar delta percentile vs phase baseline
    double sessionDeltaPctile = 0.0;    // Session delta percentile vs phase baseline
    double volumePctile = 0.0;          // Volume percentile vs phase baseline
    bool barBaselineReady = false;
    bool sessionBaselineReady = false;
    bool volumeBaselineReady = false;

    // =========================================================================
    // CHARACTER CLASSIFICATION
    // =========================================================================
    DeltaCharacter character = DeltaCharacter::UNKNOWN;
    DeltaCharacter rawCharacter = DeltaCharacter::UNKNOWN;  // Before hysteresis
    int barsInCharacter = 0;            // Consecutive bars in this character

    // Persistence tracking
    int sustainedBars = 0;              // Consecutive aligned delta bars
    int lastReversalBar = -1;           // Bar of last direction change
    int barsSinceReversal = 0;          // Bars since last reversal
    double magnitudeTrend = 0.0;        // Slope of magnitude (+ = building, - = fading)

    // =========================================================================
    // ALIGNMENT CLASSIFICATION
    // =========================================================================
    DeltaAlignment alignment = DeltaAlignment::UNKNOWN;
    int barsInAlignment = 0;            // Consecutive bars with same alignment

    // Divergence tracking (for absorption detection)
    int divergentBars = 0;              // Consecutive divergent bars
    double divergenceStrength = 0.0;    // How strong is the divergence (0-1)
    double absorptionScore = 0.0;       // Absorption intensity (0-1)

    // =========================================================================
    // CONFIDENCE ASSESSMENT
    // =========================================================================
    DeltaConfidence confidence = DeltaConfidence::UNKNOWN;
    uint32_t warningFlags = 0;          // Bitmask of warning conditions

    // Individual checks
    bool isThinTape = false;            // Volume below threshold
    bool isHighChop = false;            // Frequent reversals
    bool isExhaustion = false;          // Extreme one-sidedness
    bool isGlobexSession = false;       // Lower liquidity session

    // =========================================================================
    // ASYMMETRIC HYSTERESIS DIAGNOSTICS (Jan 2025)
    // =========================================================================
    int characterConfirmationRequired = 0;   // Bars required for this transition
    int alignmentConfirmationRequired = 0;   // Bars required for this transition
    int barsInConfirmedCharacter = 0;        // Bars since last character change
    int barsInConfirmedAlignment = 0;        // Bars since last alignment change

    // =========================================================================
    // EXTENDED BASELINE METRICS (Jan 2025)
    // =========================================================================
    // From trades_sec baseline (thin tape classification)
    double tradesPctile = 0.0;               // Trades per second percentile
    bool tradesBaselineReady = false;        // Is trades baseline ready?
    ThinTapeType thinTapeType = ThinTapeType::NONE;

    // From bar_range baseline (volatility-adaptive thresholds)
    double rangePctile = 0.0;                // Bar range percentile
    bool rangeBaselineReady = false;         // Is range baseline ready?
    double effectiveNoiseFloor = 25.0;       // Adjusted noise floor percentile
    double effectiveStrongSignal = 75.0;     // Adjusted strong signal percentile
    bool rangeAdaptiveApplied = false;       // Was range adjustment applied?

    // From avg_trade_size baseline (institutional detection)
    double avgTradeSizePctile = 0.0;         // Avg trade size percentile
    bool avgTradeBaselineReady = false;      // Is avg trade baseline ready?
    bool isInstitutionalActivity = false;    // Above P80 avg trade size
    bool isRetailActivity = false;           // Below P20 avg trade size

    // Extended inputs tracking
    bool hasExtendedInputs = false;          // Were extended inputs provided?

    // =========================================================================
    // TRADING CONSTRAINTS
    // =========================================================================
    DeltaTradingConstraints constraints;

    // =========================================================================
    // EVENTS (Only True on Transition Bars)
    // =========================================================================
    bool characterChanged = false;      // Character classification changed
    bool alignmentChanged = false;      // Alignment classification changed
    bool reversalDetected = false;      // Delta direction reversed
    bool divergenceStarted = false;     // Just entered divergence
    bool convergenceRestored = false;   // Just exited divergence

    // =========================================================================
    // LOCATION CONTEXT (AMT Value-Relative Awareness)
    // =========================================================================
    DeltaLocationContext location;      // Where price is relative to value

    // =========================================================================
    // CONTEXT GATES (from LiquidityEngine + VolatilityEngine + DaltonEngine)
    // =========================================================================
    DeltaContextGateResult contextGate;

    // =========================================================================
    // AUCTION OUTCOME IMPLICATIONS
    // Delta + location + character -> auction outcome likelihood
    // These are state descriptors, NOT trade signals
    // =========================================================================
    DeltaAuctionPrediction likelyOutcome = DeltaAuctionPrediction::UNKNOWN;
    double acceptanceLikelihood = 0.0;  // [0-1] Probability of value accepting this price
    double rejectionLikelihood = 0.0;   // [0-1] Probability of price rejection
    double rotationLikelihood = 0.0;    // [0-1] Probability of balanced rotation

    // =========================================================================
    // VALIDITY / ERROR
    // =========================================================================
    DeltaErrorReason errorReason = DeltaErrorReason::NONE;
    SessionPhase phase = SessionPhase::UNKNOWN;
    int bar = -1;

    // =========================================================================
    // EXTREME DELTA CLASSIFICATION (SSOT - Dec 2024)
    // =========================================================================
    // Persistence-validated extreme delta detection.
    // Per contracts.md: isExtremeDelta := isExtremeDeltaBar && isExtremeDeltaSession
    //
    // Bar-level: > 70% one-sided (deltaConsistency > 0.7 or < 0.3)
    // Session-level: top 15% magnitude (sessionDeltaPctile >= 85)
    // Combined: both must be true to eliminate single-bar false positives
    bool isExtremeDeltaBar = false;      // Per-bar: extreme one-sided delta
    bool isExtremeDeltaSession = false;  // Session: extreme magnitude percentile
    bool isExtremeDelta = false;         // Combined: bar && session
    bool directionalCoherence = false;   // Session delta sign matches bar direction

    // =========================================================================
    // ACCESSORS
    // =========================================================================

    bool IsReady() const {
        return barBaselineReady && errorReason == DeltaErrorReason::NONE;
    }

    bool IsWarmup() const {
        return IsDeltaWarmup(errorReason);
    }

    bool HasWarnings() const {
        return warningFlags != 0;
    }

    bool IsContextBlocked() const {
        return IsDeltaContextBlocked(errorReason);
    }

    bool IsContextDegraded() const {
        return contextGate.IsDegraded();
    }

    // Direction helpers
    int DeltaSign() const {
        if (barDeltaPct > 0.01) return 1;
        if (barDeltaPct < -0.01) return -1;
        return 0;
    }

    int PriceSign() const {
        if (priceChangeTicks > 0.5) return 1;
        if (priceChangeTicks < -0.5) return -1;
        return 0;
    }

    bool IsAligned() const {
        return alignment == DeltaAlignment::CONVERGENT;
    }

    bool IsDiverging() const {
        return alignment == DeltaAlignment::DIVERGENT ||
               alignment == DeltaAlignment::ABSORPTION_BID ||
               alignment == DeltaAlignment::ABSORPTION_ASK;
    }

    bool IsSustained() const {
        return character == DeltaCharacter::SUSTAINED;
    }

    bool IsBuilding() const {
        return character == DeltaCharacter::BUILDING;
    }

    bool IsFading() const {
        return character == DeltaCharacter::FADING;
    }

    // Composite signal strength (0-1)
    double GetSignalStrength() const {
        if (!IsReady()) return 0.0;
        double strength = 0.0;

        // Character contribution
        if (character == DeltaCharacter::SUSTAINED) strength += 0.3;
        else if (character == DeltaCharacter::BUILDING) strength += 0.4;
        else if (character == DeltaCharacter::EPISODIC) strength += 0.1;

        // Alignment contribution
        if (alignment == DeltaAlignment::CONVERGENT) strength += 0.3;
        else if (alignment == DeltaAlignment::DIVERGENT) strength += 0.2;

        // Magnitude contribution (normalized)
        strength += (std::min)(barDeltaPctile / 100.0, 1.0) * 0.3;

        return (std::min)(strength, 1.0);
    }

    // =========================================================================
    // LOCATION-AWARE ACCESSORS
    // =========================================================================

    bool HasLocationContext() const {
        return location.isValid;
    }

    bool IsInValue() const {
        return location.isValid && location.zone == ValueZoneSimple::IN_VALUE;
    }

    bool IsAtValueEdge() const {
        return location.isValid && location.zone == ValueZoneSimple::AT_VALUE_EDGE;
    }

    bool IsOutsideValue() const {
        return location.isValid && location.zone == ValueZoneSimple::OUTSIDE_VALUE;
    }

    bool IsInDiscovery() const {
        return location.isValid && location.zone == ValueZoneSimple::IN_DISCOVERY;
    }

    // =========================================================================
    // AUCTION OUTCOME ACCESSORS
    // =========================================================================

    bool IsAcceptanceLikely() const {
        return likelyOutcome == DeltaAuctionPrediction::ACCEPTANCE_LIKELY;
    }

    bool IsRejectionLikely() const {
        return likelyOutcome == DeltaAuctionPrediction::REJECTION_LIKELY;
    }

    bool IsRotationLikely() const {
        return likelyOutcome == DeltaAuctionPrediction::ROTATION_LIKELY;
    }

    // Get the dominant likelihood (highest probability)
    double GetDominantLikelihood() const {
        return (std::max)({acceptanceLikelihood, rejectionLikelihood, rotationLikelihood});
    }

    // Is this a high-conviction outcome (> 0.6)?
    bool IsHighConvictionOutcome() const {
        return GetDominantLikelihood() > 0.6;
    }

    // Combined assessment: strong delta + high conviction outcome
    bool IsHighQualitySignalWithContext() const {
        if (!IsReady() || !HasLocationContext()) return false;
        return GetSignalStrength() > 0.6 && IsHighConvictionOutcome();
    }

    // =========================================================================
    // EXTREME DELTA ACCESSORS
    // =========================================================================

    // Is this an extreme delta bar (persistence-validated)?
    bool IsExtreme() const {
        return isExtremeDelta;
    }

    // Is extreme delta coherent with session direction (for initiative classification)?
    bool IsExtremeInitiative() const {
        return isExtremeDelta && directionalCoherence;
    }

    // Is extreme delta incoherent (absorption/responsive)?
    bool IsExtremeResponsive() const {
        return isExtremeDelta && !directionalCoherence;
    }
};

// ============================================================================
// DELTA CONFIGURATION
// ============================================================================

struct DeltaConfig {
    // =========================================================================
    // NOISE THRESHOLDS
    // =========================================================================
    // Delta below this percentile is considered noise
    double noiseFloorPctile = 25.0;     // Below P25 = noise
    double weakSignalPctile = 50.0;     // P25-P50 = weak
    double strongSignalPctile = 75.0;   // Above P75 = strong
    double extremePctile = 90.0;        // Above P90 = extreme

    // =========================================================================
    // CHARACTER CLASSIFICATION
    // =========================================================================
    int sustainedMinBars = 3;           // Bars to confirm sustained
    double buildingMagnitudeThreshold = 0.1;  // Magnitude increase per bar
    double fadingMagnitudeThreshold = -0.1;   // Magnitude decrease per bar
    int reversalLookback = 10;          // Bars to check for reversal frequency

    // =========================================================================
    // ALIGNMENT CLASSIFICATION
    // =========================================================================
    double alignmentDeltaThreshold = 0.15;    // Min |deltaPct| for alignment signal
    double alignmentPriceThreshold = 0.5;     // Min price move (ticks) for signal
    double absorptionStrengthMin = 0.5;       // Min divergence for absorption signal

    // =========================================================================
    // CONFIDENCE THRESHOLDS
    // =========================================================================
    double thinTapeVolumePctile = 10.0;       // Below P10 = thin tape
    double exhaustionDeltaPctile = 95.0;      // Above P95 = exhaustion risk
    int highChopReversalsThreshold = 4;       // 4+ reversals in lookback = chop

    // =========================================================================
    // HYSTERESIS
    // =========================================================================
    int characterConfirmBars = 2;       // Bars to confirm character change
    int alignmentConfirmBars = 2;       // Bars to confirm alignment change

    // =========================================================================
    // CONSTRAINTS
    // =========================================================================
    bool blockContinuationOnDivergence = true;
    bool requireAlignmentForBreakout = true;
    bool requireSustainedForContinuation = true;
    double lowConfidencePositionScale = 0.5;
    double degradedConfidencePositionScale = 0.75;

    // =========================================================================
    // CONTEXT GATES (from external engines)
    // =========================================================================
    bool requireLiquidityGate = true;    // Check liquidity state
    bool requireVolatilityGate = true;   // Check volatility regime
    bool blockOnVoid = true;             // Block on LIQ_VOID
    bool blockOnThin = false;            // Optionally block on LIQ_THIN
    bool blockOnEvent = true;            // Block on EVENT volatility
    bool degradeOnCompression = true;    // Distrust breakouts in COMPRESSION
    double highStressThreshold = 0.90;   // stressRank >= this = degrade
    bool useDaltonContext = false;       // Optional market state awareness

    // =========================================================================
    // ASYMMETRIC HYSTERESIS (Jan 2025)
    // =========================================================================
    // Different confirmation requirements for different transitions.
    // Danger signals (REVERSAL, BUILDING, DIVERGENT) enter fast (1 bar).
    // Calm signals (exiting SUSTAINED, exiting CONVERGENT) exit slow (3 bars).
    //
    // Character transitions:
    int reversalEntryBars = 1;              // Any -> REVERSAL: react fast
    int buildingEntryBars = 1;              // Any -> BUILDING: acceleration is time-sensitive
    int sustainedExitBars = 3;              // SUSTAINED -> other: confirm trend really ending
    int otherCharacterTransitionBars = 2;   // Default for other transitions

    // Alignment transitions:
    int divergenceEntryBars = 1;            // Any -> DIVERGENT/ABSORPTION: react fast
    int convergenceExitBars = 3;            // CONVERGENT -> other: confirm alignment really lost
    int otherAlignmentTransitionBars = 2;   // Default for other transitions

    // =========================================================================
    // EXTENDED BASELINE METRICS (Jan 2025)
    // =========================================================================
    // Uses additional metrics from EffortBaselineStore beyond delta_pct and vol_sec.

    // Thin tape classification (trades_sec metric)
    double lowTradesPctile = 25.0;          // Below P25 = low trades
    double highTradesPctile = 75.0;         // Above P75 = high trades
    double lowVolumePctile = 10.0;          // Below P10 = low volume (for thin tape)
    double highVolumePctile = 75.0;         // Above P75 = high volume (for institutional)
    int thinTapeConfidencePenalty = 3;      // TRUE_THIN: major concern (-3 confidence)
    int hftFragmentedConfidencePenalty = 1; // HFT_FRAGMENTED: minor concern (-1)
    int institutionalConfidenceBoost = 1;   // INSTITUTIONAL: boost (+1)

    // Range-adaptive thresholds (bar_range metric)
    bool useRangeAdaptiveThresholds = true;
    double compressionRangePctile = 25.0;   // Below P25 = compression
    double expansionRangePctile = 75.0;     // Above P75 = expansion
    double compressionNoiseMultiplier = 0.7;  // In compression: 70% of normal noise floor
    double expansionNoiseMultiplier = 1.3;    // In expansion: 130% of normal noise floor

    // Average trade size context (avg_trade_size metric)
    bool useAvgTradeSizeContext = true;
    double institutionalAvgTradePctile = 80.0;  // Above P80 = institutional size
    double retailAvgTradePctile = 20.0;         // Below P20 = retail/HFT size
};

// ============================================================================
// DELTA HISTORY TRACKER (Session-Scoped State)
// ============================================================================
// Tracks recent delta history for character/pattern detection.

struct DeltaHistoryTracker {
    static constexpr int MAX_HISTORY = 20;

    struct BarRecord {
        double deltaPct = 0.0;
        double pctile = 0.0;
        int sign = 0;               // +1, -1, 0
        double priceChangeTicks = 0.0;
        int bar = -1;
        bool isReversal = false;    // Did direction change from prior?
    };

    std::deque<BarRecord> history;
    int lastSign = 0;
    int consecutiveAligned = 0;     // Bars with same sign
    int reversalsInLookback = 0;
    int lastReversalBar = -1;

    void Reset() {
        history.clear();
        lastSign = 0;
        consecutiveAligned = 0;
        reversalsInLookback = 0;
        lastReversalBar = -1;
    }

    void Push(const BarRecord& record, int lookback = 10) {
        // Detect reversal
        bool isReversal = false;
        if (lastSign != 0 && record.sign != 0 && record.sign != lastSign) {
            isReversal = true;
            lastReversalBar = record.bar;
        }

        // Track consecutive aligned bars
        if (record.sign == lastSign && record.sign != 0) {
            consecutiveAligned++;
        } else if (record.sign != 0) {
            consecutiveAligned = 1;
        }

        if (record.sign != 0) {
            lastSign = record.sign;
        }

        // Store record
        BarRecord rec = record;
        rec.isReversal = isReversal;
        history.push_back(rec);

        // Trim to max size
        while (history.size() > MAX_HISTORY) {
            history.pop_front();
        }

        // Count reversals in lookback
        reversalsInLookback = 0;
        for (const auto& h : history) {
            if (h.bar >= record.bar - lookback && h.isReversal) {
                reversalsInLookback++;
            }
        }
    }

    // Get magnitude trend (are we building or fading?)
    double GetMagnitudeTrend(int bars = 5) const {
        if (history.size() < 2) return 0.0;

        int count = (std::min)(static_cast<int>(history.size()), bars);
        if (count < 2) return 0.0;

        // Linear regression on |pctile| over last N bars
        double sumX = 0.0, sumY = 0.0, sumXY = 0.0, sumX2 = 0.0;
        int startIdx = static_cast<int>(history.size()) - count;

        for (int i = 0; i < count; ++i) {
            double x = static_cast<double>(i);
            double y = history[startIdx + i].pctile;
            sumX += x;
            sumY += y;
            sumXY += x * y;
            sumX2 += x * x;
        }

        double n = static_cast<double>(count);
        double denom = n * sumX2 - sumX * sumX;
        if (std::abs(denom) < 0.001) return 0.0;

        return (n * sumXY - sumX * sumY) / denom;
    }

    int GetBarsInDirection() const {
        return consecutiveAligned;
    }

    int GetBarsSinceReversal(int currentBar) const {
        if (lastReversalBar < 0) return currentBar;  // No reversal yet
        return currentBar - lastReversalBar;
    }

    bool IsHighChop(int threshold = 4) const {
        return reversalsInLookback >= threshold;
    }
};

// ============================================================================
// DELTA INPUT (Extended Input Structure - Jan 2025)
// ============================================================================
// Clean interface for passing extended inputs to DeltaEngine.
// Maintains backward compatibility - extended fields are optional.
//
// Core fields (required): barDelta, barVolume, priceChangeTicks, sessionCumDelta,
//                         sessionVolume, currentBar
// Extended fields (optional): barRangeTicks, numTrades, tradesPerSec
// ============================================================================

struct DeltaInput {
    // =========================================================================
    // CORE INPUTS (required for basic operation)
    // =========================================================================
    double barDelta = 0.0;              // Bar delta (bidVol - askVol)
    double barVolume = 0.0;             // Total bar volume
    double priceChangeTicks = 0.0;      // Bar price change in ticks
    double sessionCumDelta = 0.0;       // Session cumulative delta
    double sessionVolume = 0.0;         // Session total volume
    int currentBar = -1;                // Current bar index

    // =========================================================================
    // EXTENDED INPUTS (optional, for enhanced metrics - Jan 2025)
    // =========================================================================
    double barRangeTicks = 0.0;         // High - Low in ticks (for range-adaptive thresholds)
    double numTrades = 0.0;             // Number of trades in bar (for thin tape classification)
    double tradesPerSec = 0.0;          // Trades per second rate
    double avgBidTradeSize = 0.0;       // Average bid trade size (for institutional detection)
    double avgAskTradeSize = 0.0;       // Average ask trade size

    // =========================================================================
    // VALIDITY FLAGS
    // =========================================================================
    bool hasExtendedInputs = false;     // True if extended fields are populated

    // =========================================================================
    // BUILDER PATTERN FOR CONVENIENCE
    // =========================================================================
    DeltaInput& WithCore(double delta, double vol, double priceTicks,
                         double sessDelta, double sessVol, int bar) {
        barDelta = delta;
        barVolume = vol;
        priceChangeTicks = priceTicks;
        sessionCumDelta = sessDelta;
        sessionVolume = sessVol;
        currentBar = bar;
        return *this;
    }

    DeltaInput& WithExtended(double rangeTicks, double trades, double tradesSec,
                             double avgBid = 0.0, double avgAsk = 0.0) {
        barRangeTicks = rangeTicks;
        numTrades = trades;
        tradesPerSec = tradesSec;
        avgBidTradeSize = avgBid;
        avgAskTradeSize = avgAsk;
        hasExtendedInputs = true;
        return *this;
    }

    // Convenience: calculate derived values
    double GetDeltaPct() const {
        return (barVolume > 0.0) ? (barDelta / barVolume) : 0.0;
    }

    double GetSessionDeltaPct() const {
        return (sessionVolume > 0.0) ? (sessionCumDelta / sessionVolume) : 0.0;
    }

    double GetAvgTradeSize() const {
        return (avgBidTradeSize + avgAskTradeSize) / 2.0;
    }
};

// ============================================================================
// DELTA ENGINE
// ============================================================================

class DeltaEngine {
public:
    DeltaConfig config;

private:
    // Baseline references (external SSOT)
    const EffortBaselineStore* effortStore_ = nullptr;
    const SessionDeltaBaseline* sessionBaseline_ = nullptr;

    // Current phase for phase-aware baselines
    SessionPhase currentPhase_ = SessionPhase::UNKNOWN;

    // Session-scoped state
    DeltaHistoryTracker history_;

    // Hysteresis state
    DeltaCharacter confirmedCharacter_ = DeltaCharacter::UNKNOWN;
    DeltaCharacter candidateCharacter_ = DeltaCharacter::UNKNOWN;
    int characterConfirmBars_ = 0;
    int barsInConfirmedCharacter_ = 0;  // Jan 2025: tracks time in confirmed state

    DeltaAlignment confirmedAlignment_ = DeltaAlignment::UNKNOWN;
    DeltaAlignment candidateAlignment_ = DeltaAlignment::UNKNOWN;
    int alignmentConfirmBars_ = 0;
    int barsInConfirmedAlignment_ = 0;  // Jan 2025: tracks time in confirmed state

    // Divergence tracking
    int divergentStreak_ = 0;
    double divergenceAccum_ = 0.0;

    // Session tracking
    int sessionBars_ = 0;
    int lastBar_ = -1;

public:
    // =========================================================================
    // CONFIGURATION
    // =========================================================================

    void SetEffortStore(const EffortBaselineStore* store) {
        effortStore_ = store;
    }

    void SetSessionDeltaBaseline(const SessionDeltaBaseline* baseline) {
        sessionBaseline_ = baseline;
    }

    void SetPhase(SessionPhase phase) {
        currentPhase_ = phase;
    }

    // =========================================================================
    // LIFECYCLE
    // =========================================================================

    void Reset() {
        history_.Reset();
        confirmedCharacter_ = DeltaCharacter::UNKNOWN;
        candidateCharacter_ = DeltaCharacter::UNKNOWN;
        characterConfirmBars_ = 0;
        barsInConfirmedCharacter_ = 0;
        confirmedAlignment_ = DeltaAlignment::UNKNOWN;
        candidateAlignment_ = DeltaAlignment::UNKNOWN;
        alignmentConfirmBars_ = 0;
        barsInConfirmedAlignment_ = 0;
        divergentStreak_ = 0;
        divergenceAccum_ = 0.0;
        sessionBars_ = 0;
        lastBar_ = -1;
    }

    void ResetForSession() {
        history_.Reset();
        divergentStreak_ = 0;
        divergenceAccum_ = 0.0;
        sessionBars_ = 0;
        lastBar_ = -1;
        // Preserve hysteresis state across sessions (prior context)
    }

    // =========================================================================
    // MAIN COMPUTATION
    // =========================================================================

    DeltaResult Compute(
        double barDelta,
        double barVolume,
        double priceChangeTicks,
        double sessionCumDelta,
        double sessionVolume,
        int currentBar)
    {
        DeltaResult result;
        result.bar = currentBar;
        result.phase = currentPhase_;

        // Prevent duplicate processing
        if (currentBar == lastBar_) {
            return result;  // Return empty for same bar
        }
        lastBar_ = currentBar;
        sessionBars_++;

        // =====================================================================
        // INPUT VALIDATION
        // =====================================================================

        if (effortStore_ == nullptr) {
            result.errorReason = DeltaErrorReason::ERR_NO_BASELINE_STORE;
            return result;
        }

        if (std::isnan(barDelta) || std::isnan(barVolume)) {
            result.errorReason = DeltaErrorReason::ERR_INVALID_INPUT;
            return result;
        }

        if (barVolume <= 0.0) {
            result.errorReason = DeltaErrorReason::ERR_ZERO_VOLUME;
            return result;
        }

        // =====================================================================
        // RAW MEASUREMENTS
        // =====================================================================

        result.barDelta = barDelta;
        result.barVolume = barVolume;
        result.barDeltaPct = barDelta / barVolume;  // -1 to +1
        result.priceChangeTicks = priceChangeTicks;

        result.sessionCumDelta = sessionCumDelta;
        result.sessionVolume = sessionVolume;
        result.sessionDeltaPct = (sessionVolume > 0.0)
            ? sessionCumDelta / sessionVolume : 0.0;

        // =====================================================================
        // BASELINE PERCENTILES
        // =====================================================================

        // Get phase bucket
        if (!IsTradeablePhase(currentPhase_)) {
            result.errorReason = DeltaErrorReason::SESSION_RESET;
            return result;
        }

        const auto& bucket = effortStore_->Get(currentPhase_);

        // Bar delta percentile (magnitude-based)
        if (bucket.delta_pct.size() >= 10) {
            result.barDeltaPctile = bucket.delta_pct.percentile(std::abs(result.barDeltaPct));
            result.barBaselineReady = true;
        } else {
            result.barBaselineReady = false;
        }

        // Volume percentile
        if (bucket.vol_sec.size() >= 10) {
            result.volumePctile = bucket.vol_sec.percentile(barVolume);
            result.volumeBaselineReady = true;
        } else {
            result.volumeBaselineReady = false;
        }

        // Session delta percentile
        if (sessionBaseline_ != nullptr) {
            auto pctile = sessionBaseline_->TryGetPercentile(currentPhase_, result.sessionDeltaPct);
            if (pctile.valid) {
                // SSOT Invariant: Percentiles must be in [0, 100]
                AMT_SSOT_ASSERT_RANGE(pctile.value, 0.0, 100.0, "DeltaEngine sessionDeltaPctile");
                result.sessionDeltaPctile = pctile.value;
                result.sessionBaselineReady = true;
            }
        }

        // Check warmup
        if (!result.barBaselineReady && !result.sessionBaselineReady) {
            result.errorReason = DeltaErrorReason::WARMUP_BOTH;
            return result;
        } else if (!result.barBaselineReady) {
            result.errorReason = DeltaErrorReason::WARMUP_BAR_BASELINE;
            return result;
        }

        // =====================================================================
        // EXTREME DELTA CLASSIFICATION (SSOT - Dec 2024)
        // =====================================================================
        // Per contracts.md: persistence-validated extreme delta detection
        // Requires BOTH bar-level extremity AND session-level persistence
        // to eliminate false positives from single-bar delta spikes.
        {
            // deltaConsistency = 0.5 + 0.5 * barDeltaPct maps [-1,+1] to [0,1]
            // where 0.5 = neutral, >0.7 = 70%+ buying, <0.3 = 70%+ selling
            const double deltaConsistency = 0.5 + 0.5 * result.barDeltaPct;

            // Bar-level extreme: > 70% one-sided (either direction)
            result.isExtremeDeltaBar = result.barBaselineReady &&
                (deltaConsistency > 0.7 || deltaConsistency < 0.3);

            // Session-level extreme: top 15% magnitude (>= 85th percentile)
            result.isExtremeDeltaSession = result.sessionBaselineReady &&
                (result.sessionDeltaPctile >= 85.0);

            // Combined: both must be true for persistence-validated extreme
            result.isExtremeDelta = result.isExtremeDeltaBar && result.isExtremeDeltaSession;

            // Directional coherence: session delta sign matches bar delta direction
            // Bar is bullish if deltaConsistency > 0.5, session positive if cumDelta > 0
            const bool barBullish = (deltaConsistency > 0.5);
            const bool sessionPositive = (result.sessionDeltaPct > 0.0);
            result.directionalCoherence = (barBullish == sessionPositive);
        }

        // =====================================================================
        // UPDATE HISTORY
        // =====================================================================

        DeltaHistoryTracker::BarRecord rec;
        rec.deltaPct = result.barDeltaPct;
        rec.pctile = result.barDeltaPctile;
        rec.sign = result.DeltaSign();
        rec.priceChangeTicks = priceChangeTicks;
        rec.bar = currentBar;

        history_.Push(rec, config.reversalLookback);

        // =====================================================================
        // CHARACTER CLASSIFICATION
        // =====================================================================

        DeltaCharacter rawCharacter = ClassifyCharacter(result);
        result.rawCharacter = rawCharacter;

        // Apply hysteresis with asymmetric confirmation (Jan 2025)
        if (rawCharacter != candidateCharacter_) {
            candidateCharacter_ = rawCharacter;
            characterConfirmBars_ = 1;
        } else {
            characterConfirmBars_++;
        }

        // Asymmetric lookup: danger signals enter fast, calm signals exit slow
        const int requiredCharBars = GetCharacterConfirmationBars(
            confirmedCharacter_, candidateCharacter_);
        result.characterConfirmationRequired = requiredCharBars;

        if (characterConfirmBars_ >= requiredCharBars) {
            if (confirmedCharacter_ != candidateCharacter_) {
                result.characterChanged = true;
                barsInConfirmedCharacter_ = 0;  // Reset on transition
            }
            confirmedCharacter_ = candidateCharacter_;
        }
        barsInConfirmedCharacter_++;

        result.character = confirmedCharacter_;
        result.barsInCharacter = characterConfirmBars_;
        result.barsInConfirmedCharacter = barsInConfirmedCharacter_;
        result.sustainedBars = history_.GetBarsInDirection();
        result.barsSinceReversal = history_.GetBarsSinceReversal(currentBar);
        result.lastReversalBar = history_.lastReversalBar;
        result.magnitudeTrend = history_.GetMagnitudeTrend();

        // Detect reversals
        if (!history_.history.empty() && history_.history.back().isReversal) {
            result.reversalDetected = true;
        }

        // =====================================================================
        // ALIGNMENT CLASSIFICATION
        // =====================================================================

        DeltaAlignment rawAlignment = ClassifyAlignment(result);
        DeltaAlignment prevAlignment = confirmedAlignment_;

        // Apply hysteresis with asymmetric confirmation (Jan 2025)
        if (rawAlignment != candidateAlignment_) {
            candidateAlignment_ = rawAlignment;
            alignmentConfirmBars_ = 1;
        } else {
            alignmentConfirmBars_++;
        }

        // Asymmetric lookup: divergence enters fast, convergence exits slow
        const int requiredAlignBars = GetAlignmentConfirmationBars(
            confirmedAlignment_, candidateAlignment_);
        result.alignmentConfirmationRequired = requiredAlignBars;

        if (alignmentConfirmBars_ >= requiredAlignBars) {
            if (confirmedAlignment_ != candidateAlignment_) {
                result.alignmentChanged = true;
                barsInConfirmedAlignment_ = 0;  // Reset on transition
            }
            confirmedAlignment_ = candidateAlignment_;
        }
        barsInConfirmedAlignment_++;

        result.alignment = confirmedAlignment_;
        result.barsInAlignment = alignmentConfirmBars_;
        result.barsInConfirmedAlignment = barsInConfirmedAlignment_;

        // Track divergence
        if (result.IsDiverging()) {
            divergentStreak_++;
            divergenceAccum_ += std::abs(result.barDeltaPct);
        } else {
            if (divergentStreak_ > 0 && prevAlignment != DeltaAlignment::UNKNOWN) {
                result.convergenceRestored = true;
            }
            divergentStreak_ = 0;
            divergenceAccum_ = 0.0;
        }

        result.divergentBars = divergentStreak_;
        result.divergenceStrength = (divergentStreak_ > 0)
            ? (std::min)(divergentStreak_ / 5.0, 1.0) : 0.0;
        result.absorptionScore = (divergentStreak_ > 0)
            ? (std::min)(divergenceAccum_ / (divergentStreak_ * 0.5), 1.0) : 0.0;

        // Detect divergence start
        if (result.IsDiverging() && divergentStreak_ == 1) {
            result.divergenceStarted = true;
        }

        // =====================================================================
        // CONFIDENCE ASSESSMENT
        // =====================================================================

        result.confidence = AssessConfidence(result);

        // Individual flags
        result.isThinTape = result.volumePctile < config.thinTapeVolumePctile;
        result.isHighChop = history_.IsHighChop(config.highChopReversalsThreshold);
        result.isExhaustion = result.barDeltaPctile > config.exhaustionDeltaPctile;
        result.isGlobexSession = (currentPhase_ == SessionPhase::GLOBEX);

        // Warning flags bitmask
        if (result.isThinTape) result.warningFlags |= (1 << 0);
        if (result.isHighChop) result.warningFlags |= (1 << 1);
        if (result.isExhaustion) result.warningFlags |= (1 << 2);
        if (result.isGlobexSession) result.warningFlags |= (1 << 3);

        // =====================================================================
        // TRADING CONSTRAINTS
        // =====================================================================

        ApplyConstraints(result);

        return result;
    }

    // =========================================================================
    // LOCATION-AWARE COMPUTE (AMT Value-Relative)
    // =========================================================================

    DeltaResult Compute(
        double barDelta,
        double barVolume,
        double priceChangeTicks,
        double sessionCumDelta,
        double sessionVolume,
        int currentBar,
        const DeltaLocationContext& locationCtx)
    {
        // Compute base delta result
        DeltaResult result = Compute(
            barDelta, barVolume, priceChangeTicks,
            sessionCumDelta, sessionVolume, currentBar);

        if (!result.IsReady()) {
            return result;  // Can't add location analysis if base computation failed
        }

        // Attach location context
        result.location = locationCtx;

        // Apply location-sensitive adjustments and compute outcome likelihoods
        if (locationCtx.isValid) {
            ApplyLocationAdjustments(result);
            ComputeOutcomeLikelihoods(result);
        }

        return result;
    }

    // =========================================================================
    // FULL CONTEXT-AWARE COMPUTE (Location + Context Gates)
    // =========================================================================
    // This is the recommended entry point when all context is available.
    // Accepts location context + liquidity/volatility/dalton context.

    DeltaResult Compute(
        double barDelta,
        double barVolume,
        double priceChangeTicks,
        double sessionCumDelta,
        double sessionVolume,
        int currentBar,
        const DeltaLocationContext& locationCtx,
        LiquidityState liqState,
        VolatilityRegime volRegime,
        double stressRank = 0.0,
        AMTMarketState daltonState = AMTMarketState::UNKNOWN,
        bool is1TF = false)
    {
        // Compute base delta result with location
        DeltaResult result = Compute(
            barDelta, barVolume, priceChangeTicks,
            sessionCumDelta, sessionVolume, currentBar, locationCtx);

        // Apply context gates (even if base computation had issues, for diagnostics)
        result.contextGate = ApplyContextGates(liqState, volRegime, stressRank, daltonState, is1TF);

        // Check for blocking conditions
        if (result.contextGate.blockReason != DeltaErrorReason::NONE) {
            result.errorReason = result.contextGate.blockReason;
        }

        // Re-apply constraints with context awareness (overrides base constraints)
        if (result.barBaselineReady) {
            ApplyConstraintsWithContext(result);
        }

        // Adjust confidence based on context degradation
        if (result.contextGate.IsDegraded() && result.confidence > DeltaConfidence::DEGRADED) {
            result.confidence = DeltaConfidence::DEGRADED;
        }

        return result;
    }

    // Convenience overload without Dalton context
    DeltaResult Compute(
        double barDelta,
        double barVolume,
        double priceChangeTicks,
        double sessionCumDelta,
        double sessionVolume,
        int currentBar,
        const DeltaLocationContext& locationCtx,
        LiquidityState liqState,
        VolatilityRegime volRegime,
        double stressRank)
    {
        return Compute(barDelta, barVolume, priceChangeTicks,
                       sessionCumDelta, sessionVolume, currentBar, locationCtx,
                       liqState, volRegime, stressRank,
                       AMTMarketState::UNKNOWN, false);
    }

    // =========================================================================
    // DELTA INPUT COMPUTE (Jan 2025 - Extended Metrics)
    // =========================================================================
    // Uses DeltaInput struct for clean input handling and extended metrics.
    // When hasExtendedInputs=true, applies thin tape classification and
    // range-adaptive thresholds.

    DeltaResult Compute(const DeltaInput& input) {
        // Compute base result
        DeltaResult result = Compute(
            input.barDelta, input.barVolume, input.priceChangeTicks,
            input.sessionCumDelta, input.sessionVolume, input.currentBar);

        if (!result.IsReady()) {
            return result;
        }

        // Track extended inputs status
        result.hasExtendedInputs = input.hasExtendedInputs;

        // Process extended metrics if available
        if (input.hasExtendedInputs) {
            ProcessExtendedMetrics(result, input);
        }

        return result;
    }

    DeltaResult Compute(const DeltaInput& input, const DeltaLocationContext& locationCtx) {
        // Compute base with location
        DeltaResult result = Compute(
            input.barDelta, input.barVolume, input.priceChangeTicks,
            input.sessionCumDelta, input.sessionVolume, input.currentBar,
            locationCtx);

        if (!result.IsReady()) {
            return result;
        }

        result.hasExtendedInputs = input.hasExtendedInputs;

        if (input.hasExtendedInputs) {
            ProcessExtendedMetrics(result, input);
        }

        return result;
    }

    DeltaResult Compute(
        const DeltaInput& input,
        const DeltaLocationContext& locationCtx,
        LiquidityState liqState,
        VolatilityRegime volRegime,
        double stressRank = 0.0,
        AMTMarketState daltonState = AMTMarketState::UNKNOWN,
        bool is1TF = false)
    {
        // Compute full context result
        DeltaResult result = Compute(
            input.barDelta, input.barVolume, input.priceChangeTicks,
            input.sessionCumDelta, input.sessionVolume, input.currentBar,
            locationCtx, liqState, volRegime, stressRank, daltonState, is1TF);

        result.hasExtendedInputs = input.hasExtendedInputs;

        // Process extended metrics (even if base had context blocks, for diagnostics)
        if (input.hasExtendedInputs) {
            ProcessExtendedMetrics(result, input);
        }

        return result;
    }

private:
    // =========================================================================
    // LOCATION-SENSITIVE ADJUSTMENTS
    // Delta interpretation varies by location relative to value
    // =========================================================================

    void ApplyLocationAdjustments(DeltaResult& result) {
        const auto& loc = result.location;

        // Location-based confidence adjustment
        // At edges: Delta divergence is more significant (potential absorption)
        // Outside value: Sustained delta is more significant (acceptance/rejection)
        // In value: Delta signals are less decisive (rotation expected)

        if (loc.zone == ValueZoneSimple::AT_VALUE_EDGE) {
            // At VAH/VAL: Divergence signals absorption, may indicate reversal
            if (result.IsDiverging()) {
                // Boost divergence significance at edges
                result.divergenceStrength *= 1.3;
                result.absorptionScore *= 1.3;
                // Cap at 1.0
                result.divergenceStrength = (std::min)(result.divergenceStrength, 1.0);
                result.absorptionScore = (std::min)(result.absorptionScore, 1.0);
            }
        }
        else if (loc.zone == ValueZoneSimple::OUTSIDE_VALUE) {
            // Outside value: Convergent delta supports acceptance
            // Sustained + aligned = stronger acceptance signal
            if (result.IsAligned() && result.IsSustained()) {
                // Increase constraint permissions for continuation
                result.constraints.allowContinuation = true;
            }
        }
        else if (loc.zone == ValueZoneSimple::IN_DISCOVERY) {
            // Discovery zone: High-conviction signals only
            // Require stronger delta for action
            if (!result.IsSustained() || !result.IsAligned()) {
                // Reduce position size in discovery without clear conviction
                result.constraints.positionSizeMultiplier *= 0.75;
            }
        }
        else if (loc.zone == ValueZoneSimple::IN_VALUE) {
            // Inside value: Expect rotation, delta less decisive
            // Breakout signals need extra confirmation
            result.constraints.requireDeltaAlignment = true;
        }
    }

    // =========================================================================
    // AUCTION OUTCOME LIKELIHOODS
    // These are state descriptors, NOT trade signals
    // =========================================================================

    void ComputeOutcomeLikelihoods(DeltaResult& result) {
        const auto& loc = result.location;

        // Reset likelihoods
        double acceptance = 0.0;
        double rejection = 0.0;
        double rotation = 0.0;

        // Base case: In value area, rotation is default
        if (loc.zone == ValueZoneSimple::IN_VALUE) {
            rotation = 0.6;
            acceptance = 0.2;
            rejection = 0.2;
        }
        else if (loc.zone == ValueZoneSimple::AT_VALUE_EDGE) {
            // At edge: Outcome depends on delta character and alignment

            if (result.IsDiverging()) {
                // Delta opposes price -> Absorption -> Rejection likely
                rejection = 0.4 + (result.divergenceStrength * 0.3);
                rotation = 0.3;
                acceptance = 0.3 - (result.divergenceStrength * 0.2);
            }
            else if (result.IsAligned()) {
                // Delta supports price -> Breakout attempt
                if (result.IsSustained()) {
                    acceptance = 0.5 + (result.sustainedBars * 0.05);
                    rejection = 0.2;
                } else {
                    // Aligned but not sustained -> testing
                    acceptance = 0.35;
                    rejection = 0.35;
                }
                rotation = 0.3;
            }
            else {
                // Neutral delta at edge -> rotation or test
                rotation = 0.5;
                acceptance = 0.25;
                rejection = 0.25;
            }
        }
        else if (loc.zone == ValueZoneSimple::OUTSIDE_VALUE) {
            // Outside value: Acceptance vs rejection decision point

            if (result.IsAligned() && result.IsSustained()) {
                // Strong convergent sustained delta outside value = acceptance
                acceptance = 0.55 + (result.sustainedBars * 0.05);
                rejection = 0.20;
                rotation = 0.25 - (result.sustainedBars * 0.03);
            }
            else if (result.IsDiverging()) {
                // Divergent delta outside value = rejection warning
                rejection = 0.50 + (result.divergenceStrength * 0.25);
                acceptance = 0.20;
                rotation = 0.30 - (result.divergenceStrength * 0.15);
            }
            else {
                // Ambiguous - could go either way
                acceptance = 0.35;
                rejection = 0.35;
                rotation = 0.30;
            }
        }
        else if (loc.zone == ValueZoneSimple::IN_DISCOVERY) {
            // Discovery zone: Far outside value

            if (result.IsAligned() && result.IsSustained() && !result.IsFading()) {
                // Strong directional conviction in discovery = new value forming
                acceptance = 0.65 + (result.sustainedBars * 0.03);
                rejection = 0.15;
                rotation = 0.20 - (result.sustainedBars * 0.02);
            }
            else if (result.IsFading() || result.IsDiverging()) {
                // Fading or diverging in discovery = overextension
                rejection = 0.55 + (result.divergenceStrength * 0.2);
                acceptance = 0.20;
                rotation = 0.25;
            }
            else {
                // Discovery but unclear conviction
                acceptance = 0.40;
                rejection = 0.30;
                rotation = 0.30;
            }
        }

        // POC migration adjustment
        if (loc.isMigratingTowardPrice) {
            // POC following price = acceptance confirmation
            acceptance += 0.10;
            rejection -= 0.05;
        } else if (loc.isMigratingAwayFromPrice) {
            // POC retreating = rejection confirmation
            rejection += 0.10;
            acceptance -= 0.05;
        }

        // Session extreme adjustment
        if (loc.isAboveSessionHigh || loc.isBelowSessionLow) {
            // At session extreme with delta support = higher acceptance odds
            if (result.IsAligned()) {
                acceptance += 0.08;
            } else {
                rejection += 0.08;  // Overextended without support
            }
        }

        // Normalize to sum to 1.0
        const double total = acceptance + rejection + rotation;
        if (total > 0.0) {
            acceptance /= total;
            rejection /= total;
            rotation /= total;
        }

        // Clamp values
        result.acceptanceLikelihood = (std::min)((std::max)(acceptance, 0.0), 1.0);
        result.rejectionLikelihood = (std::min)((std::max)(rejection, 0.0), 1.0);
        result.rotationLikelihood = (std::min)((std::max)(rotation, 0.0), 1.0);

        // Determine likely outcome
        if (result.acceptanceLikelihood >= result.rejectionLikelihood &&
            result.acceptanceLikelihood >= result.rotationLikelihood) {
            result.likelyOutcome = DeltaAuctionPrediction::ACCEPTANCE_LIKELY;
        }
        else if (result.rejectionLikelihood >= result.acceptanceLikelihood &&
                 result.rejectionLikelihood >= result.rotationLikelihood) {
            result.likelyOutcome = DeltaAuctionPrediction::REJECTION_LIKELY;
        }
        else {
            result.likelyOutcome = DeltaAuctionPrediction::ROTATION_LIKELY;
        }
    }

    // =========================================================================
    // CONTEXT GATE APPLICATION
    // =========================================================================

    DeltaContextGateResult ApplyContextGates(
        LiquidityState liqState,
        VolatilityRegime volRegime,
        double stressRank,
        AMTMarketState daltonState,
        bool is1TF
    ) const {
        DeltaContextGateResult gate;
        gate.liqState = liqState;
        gate.volRegime = volRegime;
        gate.stressRank = stressRank;
        gate.daltonState = daltonState;
        gate.is1TF = is1TF;
        gate.hasDaltonContext = (daltonState != AMTMarketState::UNKNOWN);

        // Track if we have valid context
        bool hasLiqContext = (liqState != LiquidityState::LIQ_NOT_READY);
        bool hasVolContext = (volRegime != VolatilityRegime::UNKNOWN);
        gate.contextValid = hasLiqContext || hasVolContext;

        // Liquidity gate
        if (config.requireLiquidityGate && hasLiqContext) {
            if (liqState == LiquidityState::LIQ_VOID && config.blockOnVoid) {
                gate.liquidityOK = false;
                gate.blockReason = DeltaErrorReason::BLOCKED_LIQUIDITY_VOID;
            } else if (liqState == LiquidityState::LIQ_THIN && config.blockOnThin) {
                gate.liquidityOK = false;
                gate.blockReason = DeltaErrorReason::BLOCKED_LIQUIDITY_THIN;
            } else {
                gate.liquidityOK = true;
            }

            // High stress degradation (not block)
            if (stressRank >= config.highStressThreshold) {
                gate.highStress = true;
            }
        } else {
            gate.liquidityOK = true;  // Pass if not required or not available
        }

        // Volatility gate
        if (config.requireVolatilityGate && hasVolContext) {
            if (volRegime == VolatilityRegime::EVENT && config.blockOnEvent) {
                gate.volatilityOK = false;
                if (gate.blockReason == DeltaErrorReason::NONE) {
                    gate.blockReason = DeltaErrorReason::BLOCKED_VOLATILITY_EVENT;
                }
            } else {
                gate.volatilityOK = true;
            }

            // Compression degradation (not block)
            if (volRegime == VolatilityRegime::COMPRESSION && config.degradeOnCompression) {
                gate.compressionDegraded = true;
            }
        } else {
            gate.volatilityOK = true;  // Pass if not required or not available
        }

        gate.allGatesPass = gate.liquidityOK && gate.volatilityOK;
        return gate;
    }

    // =========================================================================
    // CONTEXT-AWARE CONSTRAINTS
    // =========================================================================

    void ApplyConstraintsWithContext(DeltaResult& result) const {
        // Apply base constraints first
        ApplyConstraints(result);

        auto& c = result.constraints;
        const auto& gate = result.contextGate;

        // Context gate modifications
        if (!gate.allGatesPass) {
            // Full block - zero out all trading permissions
            c.allowContinuation = false;
            c.allowBreakout = false;
            c.positionSizeMultiplier = 0.0;
            c.confidenceWeight = 0.0;
            return;
        }

        // Compression regime: distrust breakouts, prefer fade
        if (gate.compressionDegraded) {
            c.allowBreakout = false;
            c.allowFade = true;
            c.positionSizeMultiplier *= 0.75;
            c.confidenceWeight *= 0.75;
        }

        // High stress: tighten requirements
        if (gate.highStress) {
            c.requireDeltaAlignment = true;
            c.requireSustained = true;
            c.positionSizeMultiplier *= 0.75;
        }

        // Optional: Dalton context awareness
        if (gate.hasDaltonContext && config.useDaltonContext) {
            if (gate.daltonState == AMTMarketState::BALANCE) {
                // Balance (2TF): Expect rotation, tighten continuation requirements
                c.requireDeltaAlignment = true;
                c.allowFade = true;
            } else if (gate.daltonState == AMTMarketState::IMBALANCE && gate.is1TF) {
                // Strong trend: relax requirements for continuation
                c.requireSustained = false;
                // Boost for aligned signals in trend
                if (result.IsAligned()) {
                    c.positionSizeMultiplier = (std::min)(1.0, c.positionSizeMultiplier * 1.15);
                }
            }
        }
    }

private:
    // =========================================================================
    // CHARACTER CLASSIFICATION
    // =========================================================================

    DeltaCharacter ClassifyCharacter(const DeltaResult& result) const {
        // Check noise floor
        if (result.barDeltaPctile < config.noiseFloorPctile) {
            return DeltaCharacter::NEUTRAL;
        }

        // Check for reversal
        if (!history_.history.empty() && history_.history.back().isReversal) {
            return DeltaCharacter::REVERSAL;
        }

        // Check magnitude trend
        double trend = history_.GetMagnitudeTrend();
        if (trend > config.buildingMagnitudeThreshold) {
            return DeltaCharacter::BUILDING;
        }
        if (trend < config.fadingMagnitudeThreshold) {
            return DeltaCharacter::FADING;
        }

        // Check sustained vs episodic
        int alignedBars = history_.GetBarsInDirection();
        if (alignedBars >= config.sustainedMinBars) {
            return DeltaCharacter::SUSTAINED;
        }

        return DeltaCharacter::EPISODIC;
    }

    // =========================================================================
    // ALIGNMENT CLASSIFICATION
    // =========================================================================

    DeltaAlignment ClassifyAlignment(const DeltaResult& result) const {
        // Need minimum delta for signal
        if (std::abs(result.barDeltaPct) < config.alignmentDeltaThreshold) {
            return DeltaAlignment::NEUTRAL;
        }

        // Need minimum price movement
        if (std::abs(result.priceChangeTicks) < config.alignmentPriceThreshold) {
            return DeltaAlignment::NEUTRAL;
        }

        int deltaSign = result.DeltaSign();
        int priceSign = result.PriceSign();

        // Aligned: same direction
        if (deltaSign == priceSign) {
            return DeltaAlignment::CONVERGENT;
        }

        // Divergent: opposite direction
        if (deltaSign != priceSign && deltaSign != 0 && priceSign != 0) {
            // Determine absorption type
            // Price up + negative delta = sellers hitting into buying (absorption at bid)
            // Price down + positive delta = buyers lifting into selling (absorption at ask)
            if (priceSign > 0 && deltaSign < 0) {
                return DeltaAlignment::ABSORPTION_BID;  // Bullish divergence
            }
            if (priceSign < 0 && deltaSign > 0) {
                return DeltaAlignment::ABSORPTION_ASK;  // Bearish divergence
            }
            return DeltaAlignment::DIVERGENT;
        }

        return DeltaAlignment::NEUTRAL;
    }

    // =========================================================================
    // ASYMMETRIC HYSTERESIS LOOKUP (Jan 2025)
    // =========================================================================
    // Returns the number of confirmation bars required for a given transition.
    // Danger signals (REVERSAL, BUILDING, DIVERGENT) enter fast (1 bar).
    // Calm signals (exiting SUSTAINED, exiting CONVERGENT) exit slow (3 bars).

    int GetCharacterConfirmationBars(DeltaCharacter from, DeltaCharacter to) const {
        // Fast entry for danger signals
        if (to == DeltaCharacter::REVERSAL) {
            return config.reversalEntryBars;  // Default: 1
        }
        if (to == DeltaCharacter::BUILDING) {
            return config.buildingEntryBars;  // Default: 1
        }

        // Slow exit from stable states
        if (from == DeltaCharacter::SUSTAINED &&
            to != DeltaCharacter::SUSTAINED &&
            to != DeltaCharacter::BUILDING) {
            // Exiting sustained to neutral/episodic/fading requires more confirmation
            return config.sustainedExitBars;  // Default: 3
        }

        // Default transition confirmation
        return config.otherCharacterTransitionBars;  // Default: 2
    }

    int GetAlignmentConfirmationBars(DeltaAlignment from, DeltaAlignment to) const {
        // Fast entry for danger signals (divergence/absorption)
        if (to == DeltaAlignment::DIVERGENT ||
            to == DeltaAlignment::ABSORPTION_BID ||
            to == DeltaAlignment::ABSORPTION_ASK) {
            return config.divergenceEntryBars;  // Default: 1
        }

        // Slow exit from stable convergent state
        if (from == DeltaAlignment::CONVERGENT &&
            to != DeltaAlignment::CONVERGENT) {
            // Exiting convergent requires more confirmation
            return config.convergenceExitBars;  // Default: 3
        }

        // Default transition confirmation
        return config.otherAlignmentTransitionBars;  // Default: 2
    }

    // =========================================================================
    // THIN TAPE CLASSIFICATION (Jan 2025)
    // =========================================================================
    // Distinguishes different types of low activity:
    //   TRUE_THIN: Low vol + low trades (no participation)
    //   HFT_FRAGMENTED: Low vol + high trades (HFT noise)
    //   INSTITUTIONAL: High vol + low trades (block trades)

    ThinTapeType ClassifyThinTapeType(double volumePctile, double tradesPctile) const {
        const bool lowVolume = volumePctile < config.lowVolumePctile;      // Default: P10
        const bool highVolume = volumePctile > config.highVolumePctile;    // Default: P75
        const bool lowTrades = tradesPctile < config.lowTradesPctile;      // Default: P25
        const bool highTrades = tradesPctile > config.highTradesPctile;    // Default: P75

        // TRUE_THIN: Low volume + low trades = genuine low participation
        if (lowVolume && lowTrades) {
            return ThinTapeType::TRUE_THIN;
        }

        // HFT_FRAGMENTED: Low volume + high trades = many small orders (HFT noise)
        if (lowVolume && highTrades) {
            return ThinTapeType::HFT_FRAGMENTED;
        }

        // INSTITUTIONAL: High volume + low trades = large block orders
        if (highVolume && lowTrades) {
            return ThinTapeType::INSTITUTIONAL;
        }

        return ThinTapeType::NONE;
    }

    // Get confidence impact from thin tape classification
    int GetThinTapeConfidenceImpact(ThinTapeType type) const {
        switch (type) {
            case ThinTapeType::TRUE_THIN:
                return -config.thinTapeConfidencePenalty;      // Default: -3
            case ThinTapeType::HFT_FRAGMENTED:
                return -config.hftFragmentedConfidencePenalty; // Default: -1
            case ThinTapeType::INSTITUTIONAL:
                return config.institutionalConfidenceBoost;    // Default: +1
            default:
                return 0;
        }
    }

    // =========================================================================
    // RANGE-ADAPTIVE THRESHOLDS (Jan 2025)
    // =========================================================================
    // In compression, smaller delta is meaningful (lower noise floor).
    // In expansion, require larger delta (higher noise floor).

    void ApplyRangeAdaptiveThresholds(double rangePctile, DeltaResult& result) const {
        if (!config.useRangeAdaptiveThresholds || !result.rangeBaselineReady) {
            // Use default thresholds
            result.effectiveNoiseFloor = config.noiseFloorPctile;
            result.effectiveStrongSignal = config.strongSignalPctile;
            result.rangeAdaptiveApplied = false;
            return;
        }

        double multiplier = 1.0;

        if (rangePctile < config.compressionRangePctile) {
            // Compression: smaller delta is meaningful
            multiplier = config.compressionNoiseMultiplier;  // Default: 0.7
        } else if (rangePctile > config.expansionRangePctile) {
            // Expansion: require larger delta
            multiplier = config.expansionNoiseMultiplier;    // Default: 1.3
        }

        result.effectiveNoiseFloor = config.noiseFloorPctile * multiplier;
        result.effectiveStrongSignal = config.strongSignalPctile * multiplier;
        result.rangeAdaptiveApplied = (multiplier != 1.0);
    }

    // =========================================================================
    // EXTENDED METRICS PROCESSING (Jan 2025)
    // =========================================================================
    // Called from DeltaInput-based Compute() overloads to process trades_sec,
    // bar_range, and avg_trade_size baselines.

    void ProcessExtendedMetrics(DeltaResult& result, const DeltaInput& input) {
        if (!effortStore_) return;

        // Get phase bucket for lookups
        const auto& bucket = effortStore_->Get(currentPhase_);

        // -----------------------------------------------------------------
        // A. Trades per second (thin tape classification)
        // -----------------------------------------------------------------
        if (bucket.trades_sec.size() >= 10 && input.tradesPerSec > 0.0) {
            result.tradesPctile = bucket.trades_sec.percentile(input.tradesPerSec);
            result.tradesBaselineReady = true;

            // Classify thin tape type using volume + trades percentiles
            result.thinTapeType = ClassifyThinTapeType(
                result.volumePctile, result.tradesPctile);

            // Adjust thin tape flag based on new classification
            if (result.thinTapeType == ThinTapeType::TRUE_THIN) {
                result.isThinTape = true;  // Confirm thin tape
            } else if (result.thinTapeType == ThinTapeType::INSTITUTIONAL) {
                result.isThinTape = false;  // Override - institutional is good
            }
        }

        // -----------------------------------------------------------------
        // B. Bar range (volatility-adaptive thresholds)
        // -----------------------------------------------------------------
        if (bucket.bar_range.size() >= 10 && input.barRangeTicks > 0.0) {
            result.rangePctile = bucket.bar_range.percentile(input.barRangeTicks);
            result.rangeBaselineReady = true;

            // Apply range-adaptive noise floor
            ApplyRangeAdaptiveThresholds(result.rangePctile, result);
        }

        // -----------------------------------------------------------------
        // C. Average trade size (institutional detection)
        // -----------------------------------------------------------------
        if (bucket.avg_trade_size.size() >= 10 && input.GetAvgTradeSize() > 0.0) {
            result.avgTradeSizePctile = bucket.avg_trade_size.percentile(input.GetAvgTradeSize());
            result.avgTradeBaselineReady = true;

            // Classify activity type
            result.isInstitutionalActivity =
                (result.avgTradeSizePctile >= config.institutionalAvgTradePctile);
            result.isRetailActivity =
                (result.avgTradeSizePctile <= config.retailAvgTradePctile);
        }

        // -----------------------------------------------------------------
        // D. Confidence adjustment from extended metrics
        // -----------------------------------------------------------------
        if (result.tradesBaselineReady) {
            // Apply thin tape impact to confidence
            int impact = GetThinTapeConfidenceImpact(result.thinTapeType);
            if (impact != 0) {
                // Negative impact -> degrade confidence
                // Positive impact -> preserve/upgrade confidence (no upgrade beyond FULL)
                if (impact < 0 && result.confidence == DeltaConfidence::FULL) {
                    if (impact <= -2) {
                        result.confidence = DeltaConfidence::DEGRADED;
                    } else {
                        result.confidence = DeltaConfidence::LOW;
                    }
                } else if (impact < 0 && result.confidence == DeltaConfidence::LOW) {
                    if (impact <= -2) {
                        result.confidence = DeltaConfidence::BLOCKED;
                    } else {
                        result.confidence = DeltaConfidence::DEGRADED;
                    }
                }
            }
        }
    }

    // =========================================================================
    // CONFIDENCE ASSESSMENT
    // =========================================================================

    DeltaConfidence AssessConfidence(const DeltaResult& result) const {
        int concerns = 0;

        // Critical concerns (BLOCKED)
        if (!result.barBaselineReady) {
            return DeltaConfidence::BLOCKED;
        }

        // Major concerns
        bool thinTape = result.volumePctile < config.thinTapeVolumePctile;
        bool highChop = history_.IsHighChop(config.highChopReversalsThreshold);
        bool exhaustion = result.barDeltaPctile > config.exhaustionDeltaPctile;

        if (thinTape) concerns += 2;
        if (highChop) concerns += 1;
        if (exhaustion) concerns += 1;

        // Session context
        if (currentPhase_ == SessionPhase::GLOBEX) concerns += 1;

        // Map to confidence level
        if (concerns >= 3) return DeltaConfidence::LOW;
        if (concerns >= 1) return DeltaConfidence::DEGRADED;
        return DeltaConfidence::FULL;
    }

    // =========================================================================
    // TRADING CONSTRAINTS
    // =========================================================================

    void ApplyConstraints(DeltaResult& result) const {
        auto& c = result.constraints;

        // Default: all allowed
        c.allowContinuation = true;
        c.allowBreakout = true;
        c.allowFade = true;
        c.requireDeltaAlignment = false;
        c.requireSustained = false;
        c.positionSizeMultiplier = 1.0;
        c.confidenceWeight = 1.0;

        // Apply confidence-based constraints
        switch (result.confidence) {
            case DeltaConfidence::BLOCKED:
                c.allowContinuation = false;
                c.allowBreakout = false;
                c.positionSizeMultiplier = 0.0;
                c.confidenceWeight = 0.0;
                break;

            case DeltaConfidence::LOW:
                c.requireDeltaAlignment = true;
                c.requireSustained = true;
                c.positionSizeMultiplier = config.lowConfidencePositionScale;
                c.confidenceWeight = 0.5;
                break;

            case DeltaConfidence::DEGRADED:
                c.requireDeltaAlignment = config.requireAlignmentForBreakout;
                c.positionSizeMultiplier = config.degradedConfidencePositionScale;
                c.confidenceWeight = 0.75;
                break;

            case DeltaConfidence::FULL:
            default:
                break;
        }

        // Apply alignment-based constraints
        if (result.IsDiverging() && config.blockContinuationOnDivergence) {
            c.allowContinuation = false;
            // BUT: enable fade
            c.allowFade = true;
        }

        // Apply character-based constraints
        if (result.character == DeltaCharacter::EPISODIC) {
            if (config.requireSustainedForContinuation) {
                c.allowContinuation = false;
            }
        }

        // Exhaustion: don't chase
        if (result.isExhaustion) {
            c.allowBreakout = false;
            c.allowFade = true;  // Fade exhaustion
        }
    }

public:
    // =========================================================================
    // ACCESSORS
    // =========================================================================

    const DeltaHistoryTracker& GetHistory() const { return history_; }
    int GetSessionBars() const { return sessionBars_; }
    DeltaCharacter GetConfirmedCharacter() const { return confirmedCharacter_; }
    DeltaAlignment GetConfirmedAlignment() const { return confirmedAlignment_; }
};

// ============================================================================
// LOGGING HELPERS
// ============================================================================

inline std::string DeltaResultToLogString(const DeltaResult& r) {
    std::string s = "[DELTA] ";

    if (!r.IsReady()) {
        s += "ERR=";
        s += DeltaErrorToString(r.errorReason);
        return s;
    }

    // Character and alignment
    s += "CHAR=";
    s += DeltaCharacterShort(r.character);
    s += " ALIGN=";
    s += DeltaAlignmentShort(r.alignment);

    // Percentiles
    s += " | B=";
    char buf[16];
    snprintf(buf, sizeof(buf), "%.0f", r.barDeltaPctile);
    s += buf;
    s += " S=";
    snprintf(buf, sizeof(buf), "%.0f", r.sessionDeltaPctile);
    s += buf;
    s += " V=";
    snprintf(buf, sizeof(buf), "%.0f", r.volumePctile);
    s += buf;

    // Confidence
    s += " | CONF=";
    s += DeltaConfidenceToString(r.confidence);

    // Warnings
    if (r.HasWarnings()) {
        s += " WARN=[";
        if (r.isThinTape) s += "THIN,";
        if (r.isHighChop) s += "CHOP,";
        if (r.isExhaustion) s += "EXH,";
        if (s.back() == ',') s.pop_back();
        s += "]";
    }

    // Events
    if (r.reversalDetected) s += " !REV";
    if (r.divergenceStarted) s += " !DIV";
    if (r.convergenceRestored) s += " !CONV";

    return s;
}

// ============================================================================
// INTEGRATION HELPER - For downstream decision integration
// ============================================================================
// Use this struct to pass delta signals to arbitration/trading logic.

struct DeltaDecisionInput {
    bool isReady = false;

    // Character signals
    bool isSustained = false;
    bool isBuilding = false;
    bool isFading = false;
    bool isReversal = false;

    // Alignment signals
    bool isConvergent = false;
    bool isDivergent = false;
    bool isAbsorption = false;

    // Confidence
    DeltaConfidence confidence = DeltaConfidence::UNKNOWN;

    // Constraints
    bool allowContinuation = false;
    bool allowBreakout = false;
    bool requireAlignment = false;
    double positionScale = 1.0;

    // Derived from DeltaResult
    static DeltaDecisionInput FromResult(const DeltaResult& r) {
        DeltaDecisionInput d;
        d.isReady = r.IsReady();
        if (!d.isReady) return d;

        d.isSustained = r.IsSustained();
        d.isBuilding = r.IsBuilding();
        d.isFading = r.IsFading();
        d.isReversal = r.reversalDetected;

        d.isConvergent = r.IsAligned();
        d.isDivergent = r.IsDiverging();
        d.isAbsorption = (r.alignment == DeltaAlignment::ABSORPTION_BID ||
                          r.alignment == DeltaAlignment::ABSORPTION_ASK);

        d.confidence = r.confidence;

        d.allowContinuation = r.constraints.allowContinuation;
        d.allowBreakout = r.constraints.allowBreakout;
        d.requireAlignment = r.constraints.requireDeltaAlignment;
        d.positionScale = r.constraints.positionSizeMultiplier;

        return d;
    }
};

} // namespace AMT
