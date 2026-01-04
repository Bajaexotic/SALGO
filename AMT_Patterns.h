// ============================================================================
// AMT_Patterns.h
// Volume, DOM, Delta, and Profile pattern enums with conversion functions
// Extracted from AuctionSensor_v1.cpp for modularization
// ============================================================================

#ifndef AMT_PATTERNS_H
#define AMT_PATTERNS_H

#include "amt_core.h"
#include <vector>
#include <cstdint>

namespace AMT {

// ============================================================================
// VOLUME PROFILE PATTERNS
// ============================================================================

enum class VolumeProfilePattern : int
{
    VOLUME_SHELF = 1,         // Trade the bounce
    VOLUME_CLUSTER = 2,       // Expect chop
    VOLUME_GAP = 3,           // Magnet target
    VOLUME_VACUUM = 4,        // Speed/Slippage
    LEDGE_PATTERN = 5,        // Trade edges
    VOLUME_MIGRATION = 6,     // Trend confirmation
    VOLUME_BREAKOUT = 7,      // Valid break
    LOW_VOLUME_BREAKOUT = 8   // Trap warning
};

enum class TPOMechanics : int
{
    TPO_OVERLAP = 1,     // Balance
    TPO_SEPARATION = 2   // Imbalance
};

// ============================================================================
// DOM PATTERNS (Static)
// ============================================================================

enum class BalanceDOMPattern : int
{
    STACKED_BIDS = 1,      // Support Wall
    STACKED_ASKS = 2,      // Resistance Wall
    ORDER_RELOADING = 3,   // Iceberg Defense
    SPOOF_ORDER_FLIP = 4   // Manipulation
};

enum class ImbalanceDOMPattern : int
{
    CHASING_ORDERS_BUY = 1,     // Momentum Step-Up
    CHASING_ORDERS_SELL = 2,    // Momentum Step-Down
    BID_ASK_RATIO_EXTREME = 3,  // Trend Confidence (>3:1)
    ABSORPTION_FAILURE = 4      // Stop Run Trigger
};

// ============================================================================
// DELTA PATTERNS
// ============================================================================

enum class BalanceDeltaPattern : int
{
    ABSORPTION_AT_HIGH = 1,     // Short Signal
    ABSORPTION_AT_LOW = 2,      // Long Signal
    DELTA_DIVERGENCE_FADE = 3,  // Weakness at edge
    AGGRESSIVE_INITIATION = 4   // Breakout Signal
};

enum class ImbalanceDeltaPattern : int
{
    STRONG_CONVERGENCE = 1,  // Healthy Trend
    WEAK_PULLBACK = 2,       // Add-on Signal
    EFFORT_NO_RESULT = 3,    // Reversal Warning
    CLIMAX_EXHAUSTION = 4    // Capitulation
};

// ============================================================================
// DOM EVENTS (Dynamic)
// ============================================================================

enum class DOMControlPattern : int
{
    BUYERS_LIFTING_ASKS = 1,    // Aggressive Buy
    SELLERS_HITTING_BIDS = 2,   // Aggressive Sell
    LIQUIDITY_PULLING = 3,      // Weakness
    LIQUIDITY_STACKING = 4,     // Strength
    EXHAUSTION_DIVERGENCE = 5   // Reversal Trigger
};

enum class DOMEvent : int
{
    LIQUIDITY_DISAPPEARANCE = 1,
    ORDER_FLOW_REVERSAL = 2,
    SWEEP_LIQUIDATION = 3,
    LARGE_LOT_EXECUTION = 4
};

// ============================================================================
// PROFILE SHAPES
// ============================================================================

enum class BalanceProfileShape : int
{
    UNDEFINED = 0,
    NORMAL_DISTRIBUTION = 1,
    D_SHAPED = 2,
    BALANCED = 3
};

enum class ImbalanceProfileShape : int
{
    UNDEFINED = 0,
    P_SHAPED = 1,
    B_SHAPED_LOWER = 2,
    B_SHAPED_BIMODAL = 3,
    THIN_VERTICAL = 4
};

// ============================================================================
// DAY STRUCTURE
// ============================================================================

// Phase 2: Binary structural classification (acceptance-based)
// BALANCED vs IMBALANCED, without sub-type semantics
enum class DayStructure : int
{
    UNDEFINED = 0,   // Not yet classified (IB not complete, insufficient evidence)
    BALANCED = 1,    // Price within IB, or RE attempts fail to gain acceptance
    IMBALANCED = 2   // Sustained acceptance outside IB (RE accepted)
};

// Phase 3: Sub-type classification (semantic mapping from structure + shape)
enum class BalanceStructure : int
{
    NONE = 0,
    NORMAL_DAY = 1,              // BALANCED + NORMAL_DISTRIBUTION shape
    NON_TREND_DAY = 2,           // Narrow IB, low volume, no conviction
    NEUTRAL_DAY_CENTER = 3,      // BALANCED + THIN_VERTICAL (elongated but balanced)
    NEUTRAL_DAY_EXTREME = 4,     // Both sides tested, close at day extreme
    DOUBLE_DISTRIBUTION_DAY = 5, // BALANCED + D_SHAPED (two-sided auction)
    BALANCED_OTHER = 6           // Explicit fallback for unclassified balance
};

enum class ImbalanceStructure : int
{
    NONE = 0,
    TREND_DAY = 1,               // IMBALANCED + P_SHAPE/B_SHAPE (directional conviction)
    NORMAL_VARIATION_DAY = 2,    // IB break one side, extension < 2x IB, moderate delta
    EXPANSION_DAY = 3,           // IMBALANCED + THIN_VERTICAL (directional but thin)
    REVERSAL_DAY = 4,            // Directional move -> opposite direction move
    IMBALANCED_OTHER = 5         // Explicit fallback for unclassified imbalance
};

// ============================================================================
// CONFIDENCE
// ============================================================================

struct ConfidenceWeights
{
    float dom = 0.35f;
    float delta = 0.25f;
    float profile = 0.20f;
    float tpo = 0.10f;
    float liquidity = 0.10f;
    float composition = 0.10f;  // Market composition (avg_trade_size proxy for institutional vs retail)
};

// Score result with validity flag (NO-FALLBACK POLICY)
// When scoreValid=false, the score is a dead value and must not be used
struct ScoreResult
{
    float score = 0.0f;
    bool scoreValid = false;

    // GUARDED ACCESSOR: asserts validity before returning dead-value field
    float GetScore() const
    {
        assert(scoreValid && "BUG: reading score when all metrics are invalid");
        return score;
    }
};

struct ConfidenceAttribute
{
    // Numeric values
    float domStrength = 0.0f;
    float tpoAcceptance = 0.0f;
    float volumeProfileClarity = 0.0f;
    // DELTA SPLIT (Dec 2024 fix):
    // - deltaConsistency: aggressor FRACTION [0,1] where 0.5=neutral, >0.7=extreme buying, <0.3=extreme selling
    //   Used for threshold checks (isExtremeDeltaBar, barDeltaPositive, side classification)
    // - deltaStrength: MAGNITUDE [0,1] where 0=neutral, 1=max one-sided
    //   Used for confidence scoring (direction-agnostic signal strength)
    float deltaConsistency = 0.5f;  // Default neutral (was 0.0f - BUG)
    float deltaStrength = 0.0f;     // NEW: magnitude for scoring
    float liquidityAvailability = 0.0f;
    // Market composition: percentile of avg_trade_size [0,1]
    // High value = larger lots = institutional presence = higher conviction
    // Low value = smaller lots = retail-dominated = higher noise
    float marketComposition = 0.0f;

    // Stage 3: Validity flags for ALL confidence metrics
    // When false, the numeric value is INVALID and must not be used in scoring
    // Unimplemented metrics default to false - they will never contribute to score
    // until their computation logic is added and sets *Valid = true
    bool domStrengthValid = false;           // UNIMPLEMENTED - no production code computes this
    bool tpoAcceptanceValid = false;         // UNIMPLEMENTED - no production code computes this
    bool volumeProfileClarityValid = false;  // UNIMPLEMENTED - no production code computes this
    bool deltaConsistencyValid = false;      // Set by AuctionSensor_v1.cpp when volume sufficient
    bool deltaStrengthValid = false;         // NEW: Set by AuctionSensor_v1.cpp when volume sufficient
    bool liquidityAvailabilityValid = false; // Set by AuctionSensor_v1.cpp when baseline ready
    bool marketCompositionValid = false;     // Set by AuctionSensor_v1.cpp when avg_trade_size baseline ready & numTrades > 0

    // Stage 3: Calculate score using ONLY valid components, renormalized
    // All six metrics are now validity-gated
    // Returns ScoreResult with scoreValid=false when all metrics invalid (NO-FALLBACK POLICY)
    ScoreResult calculate_score(const ConfidenceWeights& w) const
    {
        float totalWeight = 0.0f;
        float score = 0.0f;

        // All metrics are validity-gated
        if (domStrengthValid) {
            score += domStrength * w.dom;
            totalWeight += w.dom;
        }
        if (volumeProfileClarityValid) {
            score += volumeProfileClarity * w.profile;
            totalWeight += w.profile;
        }
        if (tpoAcceptanceValid) {
            score += tpoAcceptance * w.tpo;
            totalWeight += w.tpo;
        }
        // Use deltaStrength (magnitude) for scoring, not deltaConsistency (fraction)
        // Scoring needs direction-agnostic signal strength
        if (deltaStrengthValid) {
            score += deltaStrength * w.delta;
            totalWeight += w.delta;
        }
        if (liquidityAvailabilityValid) {
            score += liquidityAvailability * w.liquidity;
            totalWeight += w.liquidity;
        }
        if (marketCompositionValid) {
            score += marketComposition * w.composition;
            totalWeight += w.composition;
        }

        ScoreResult result;
        if (totalWeight > 0.0f) {
            result.score = score / totalWeight;
            result.scoreValid = true;
        } else {
            // All metrics invalid - score is dead value
            result.score = 0.0f;  // Dead value - accessor asserts validity
            result.scoreValid = false;
        }
        return result;
    }
};

// ============================================================================
// AUCTION CONTEXT
// Aggregates all market state into a single snapshot
// ============================================================================
// CONTRACT: AuctionContext is the SINGLE SEMANTIC INTERPRETATION LAYER.
//   - Written ONCE per bar by AuctionContextBuilder::Build()
//   - READ-ONLY by all consumers (arbitration, logging, zones)
//   - No downstream code may recompute or override these semantics
//   - All fields have explicit validity flags (no silent defaults)
// ============================================================================

struct AuctionContext
{
    // =========================================================================
    // PHASE 2: REGIME FIELDS
    // =========================================================================
    // state (AMTMarketState): Per-bar tactical regime (BALANCE/IMBALANCE)
    //   SSOT: Derived from DaltonEngine via 1TF/2TF detection
    //   Used for: zone engagement, delta classification, per-bar decisions
    //   NOT SSOT for shape family constraint (use dayStructure for that)
    // phase: Current structural phase (CurrentPhase enum)
    // session: Session phase (SessionPhase enum) - RTH, GLOBEX, etc.
    // facilitation: Auction efficiency classification
    AMTMarketState      state = AMTMarketState::BALANCE;  // SSOT: Dalton 1TF/2TF
    bool                stateValid = false;

    CurrentPhase        phase = CurrentPhase::ROTATION;
    bool                phaseValid = false;

    SessionPhase        session = SessionPhase::GLOBEX;
    bool                sessionValid = false;  // Always valid once set

    AuctionFacilitation facilitation = AuctionFacilitation::EFFICIENT;
    bool                facilitationValid = false;

    // =========================================================================
    // PHASE 1: CONTROL FIELDS
    // =========================================================================
    // aggression: Attack mode (INITIATIVE) vs defense (RESPONSIVE)
    // side: Who is in control (BUYER/SELLER/NEUTRAL)
    AggressionType      aggression = AggressionType::NEUTRAL;
    bool                aggressionValid = false;

    ControlSide         side = ControlSide::NEUTRAL;
    bool                sideValid = false;

    // =========================================================================
    // PHASE 1: ENVIRONMENT FIELDS
    // =========================================================================
    // volatility: Market volatility classification from range baseline
    // liquidity: DOM depth classification from depth baseline
    // friction: Execution friction from spread baseline (cost/slippage)
    VolatilityState     volatility = VolatilityState::NORMAL;
    bool                volatilityValid = false;

    LiquidityState      liquidity = LiquidityState::LIQ_NORMAL;
    bool                liquidityValid = false;

    ExecutionFriction   friction = ExecutionFriction::UNKNOWN;
    bool                frictionValid = false;

    // =========================================================================
    // PHASE 3: NARRATIVE FIELDS
    // =========================================================================
    // intent: What market participants are attempting
    // outcome: Result of auction attempt (per-engagement)
    // transition: Regime change type
    AuctionIntent       intent = AuctionIntent::NEUTRAL;
    bool                intentValid = false;

    AuctionOutcome      outcome = AuctionOutcome::PENDING;
    bool                outcomeValid = false;

    TransitionMechanic  transition = TransitionMechanic::NONE;
    bool                transitionValid = false;

    // =========================================================================
    // DAY STRUCTURE (SSOT for shape family constraint)
    // =========================================================================
    // dayStructure: Session-level binary structural classification
    //   BALANCED: No RE accepted (rotating within IB)
    //   IMBALANCED: RE accepted (directional conviction)
    //   SSOT for profile shape family constraint - shape must match this family
    DayStructure        dayStructure = DayStructure::UNDEFINED;
    bool                dayStructureValid = false;

    // Phase 3: Sub-type classification (deferred - remain NONE until Phase 3)
    BalanceStructure    balanceType = BalanceStructure::NONE;
    ImbalanceStructure  imbalanceType = ImbalanceStructure::NONE;

    // =========================================================================
    // PROFILE SHAPES
    // =========================================================================
    // Raw shape: Geometric classification (from ClassifyProfileShape)
    // Resolved shape: After family constraint (matches dayStructure family)
    // Conflict: True if rawShape was outside dayStructure's allowed family
    ProfileShape          rawShape = ProfileShape::UNDEFINED;       // Geometric only
    ProfileShape          resolvedShape = ProfileShape::UNDEFINED;  // After family constraint
    bool                  shapeConflict = false;                    // Raw vs family mismatch
    bool                  shapeFrozen = false;                      // True when both ready

    // Legacy shape fields (derived from resolvedShape for backward compatibility)
    BalanceProfileShape   balanceShape = BalanceProfileShape::UNDEFINED;
    ImbalanceProfileShape imbalanceShape = ImbalanceProfileShape::UNDEFINED;

    // =========================================================================
    // DERIVED FLAGS (Computed by builder, used by consumers)
    // =========================================================================
    // Extreme delta detection (persistence-validated)
    bool isExtremeDeltaBar = false;      // Per-bar: deltaConsistency > 0.7
    bool isExtremeDeltaSession = false;  // Session: percentile >= 85
    bool isExtremeDelta = false;         // Combined: bar && session
    bool directionalCoherence = false;   // Session delta sign matches bar delta

    // Raw session delta metrics (for logging/diagnostics)
    double sessionDeltaPct = 0.0;        // sessionCumDelta / sessionTotalVolume
    double sessionDeltaPctile = 50.0;    // Percentile rank [0-100]
    bool   sessionDeltaValid = false;    // True once session has sufficient data

    // =========================================================================
    // ACTIVE PATTERNS (Vectors) - Phase 4 (Deferred)
    // =========================================================================
    std::vector<VolumeProfilePattern>   volumePatterns;
    std::vector<TPOMechanics>           tpoMechanics;
    std::vector<BalanceDOMPattern>      balanceDOMPatterns;
    std::vector<ImbalanceDOMPattern>    imbalanceDOMPatterns;
    std::vector<BalanceDeltaPattern>    balanceDeltaPatterns;
    std::vector<ImbalanceDeltaPattern>  imbalanceDeltaPatterns;
    std::vector<DOMControlPattern>      domControlPatterns;
    std::vector<DOMEvent>               domEvents;

    // =========================================================================
    // CONFIDENCE METRICS (Existing - validity flags already present)
    // =========================================================================
    ConfidenceAttribute confidence;

    // =========================================================================
    // ACCESSORS
    // =========================================================================

    // Helper: Is price in directional regime?
    bool IsDirectional() const {
        if (!phaseValid) return false;
        return phase == CurrentPhase::DRIVING_UP ||
               phase == CurrentPhase::DRIVING_DOWN ||
               phase == CurrentPhase::RANGE_EXTENSION ||
               phase == CurrentPhase::FAILED_AUCTION;
    }

    // Helper: Is aggression INITIATIVE?
    bool IsInitiative() const {
        return aggressionValid && aggression == AggressionType::INITIATIVE;
    }

    // Helper: Is market state IMBALANCE?
    bool IsImbalanced() const {
        return stateValid && state == AMTMarketState::IMBALANCE;
    }

    void reset_confidence()
    {
        confidence = ConfidenceAttribute();
    }

    void clear_patterns()
    {
        volumePatterns.clear();
        tpoMechanics.clear();
        balanceDOMPatterns.clear();
        imbalanceDOMPatterns.clear();
        balanceDeltaPatterns.clear();
        imbalanceDeltaPatterns.clear();
        domControlPatterns.clear();
        domEvents.clear();
    }

    // Reset all validity flags (called at start of each bar)
    void invalidate_all()
    {
        stateValid = false;
        phaseValid = false;
        sessionValid = false;
        facilitationValid = false;
        aggressionValid = false;
        sideValid = false;
        volatilityValid = false;
        liquidityValid = false;
        intentValid = false;
        outcomeValid = false;
        transitionValid = false;
        sessionDeltaValid = false;
        isExtremeDeltaBar = false;
        isExtremeDeltaSession = false;
        isExtremeDelta = false;
        directionalCoherence = false;
    }
};

// ============================================================================
// STRING CONVERSION FUNCTIONS
// ============================================================================

inline const char* to_string(AuctionFacilitation f)
{
    switch (f)
    {
    case AuctionFacilitation::EFFICIENT:   return "EFFICIENT";
    case AuctionFacilitation::INEFFICIENT: return "INEFFICIENT";
    case AuctionFacilitation::LABORED:     return "LABORED";
    case AuctionFacilitation::FAILED:      return "FAILED";
    }
    return "UNK";
}

inline const char* to_string(DayStructure ds)
{
    switch (ds)
    {
    case DayStructure::UNDEFINED:  return "UNDEF";
    case DayStructure::BALANCED:   return "BALANCED";
    case DayStructure::IMBALANCED: return "IMBALANCED";
    }
    return "UNK";
}

inline const char* to_string(BalanceStructure bs)
{
    switch (bs)
    {
    case BalanceStructure::NONE:                    return "NONE";
    case BalanceStructure::NORMAL_DAY:              return "NORMAL_DAY";
    case BalanceStructure::NON_TREND_DAY:           return "NON_TREND";
    case BalanceStructure::NEUTRAL_DAY_CENTER:      return "NEUTRAL_CTR";
    case BalanceStructure::NEUTRAL_DAY_EXTREME:     return "NEUTRAL_EXT";
    case BalanceStructure::DOUBLE_DISTRIBUTION_DAY: return "DBL_DIST";
    case BalanceStructure::BALANCED_OTHER:          return "BAL_OTHER";
    }
    return "UNK";
}

inline const char* to_string(ImbalanceStructure is)
{
    switch (is)
    {
    case ImbalanceStructure::NONE:                 return "NONE";
    case ImbalanceStructure::TREND_DAY:            return "TREND_DAY";
    case ImbalanceStructure::NORMAL_VARIATION_DAY: return "NORM_VAR";
    case ImbalanceStructure::EXPANSION_DAY:        return "EXPANSION";
    case ImbalanceStructure::REVERSAL_DAY:         return "REVERSAL";
    case ImbalanceStructure::IMBALANCED_OTHER:     return "IMB_OTHER";
    }
    return "UNK";
}

inline const char* to_string(VolatilityState v)
{
    switch (v)
    {
    case VolatilityState::LOW:     return "LOW";
    case VolatilityState::NORMAL:  return "NORMAL";
    case VolatilityState::HIGH:    return "HIGH";
    case VolatilityState::EXTREME: return "EXTREME";
    }
    return "UNK";
}

inline const char* to_string(LiquidityState l)
{
    switch (l)
    {
    case LiquidityState::LIQ_NOT_READY:  return "NOT_READY";
    case LiquidityState::LIQ_VOID:       return "VOID";
    case LiquidityState::LIQ_THIN:       return "THIN";
    case LiquidityState::LIQ_NORMAL:     return "NORMAL";
    case LiquidityState::LIQ_THICK:      return "THICK";
    }
    return "UNK";
}

inline const char* to_string(ExecutionFriction f)
{
    switch (f)
    {
    case ExecutionFriction::UNKNOWN: return "UNKNOWN";
    case ExecutionFriction::TIGHT:   return "TIGHT";
    case ExecutionFriction::NORMAL:  return "NORMAL";
    case ExecutionFriction::WIDE:    return "WIDE";
    case ExecutionFriction::LOCKED:  return "LOCKED";
    }
    return "UNK";
}

inline const char* to_string(AuctionOutcome o)
{
    switch (o)
    {
    case AuctionOutcome::PENDING:  return "PENDING";
    case AuctionOutcome::ACCEPTED: return "ACCEPTED";
    case AuctionOutcome::REJECTED: return "REJECTED";
    }
    return "UNK";
}

inline const char* to_string(TransitionMechanic t)
{
    switch (t)
    {
    case TransitionMechanic::NONE:                 return "NONE";
    case TransitionMechanic::BALANCE_TO_IMBALANCE: return "BAL_TO_IMB";
    case TransitionMechanic::IMBALANCE_TO_BALANCE: return "IMB_TO_BAL";
    case TransitionMechanic::FAILED_TRANSITION:    return "FAILED_TRANS";
    }
    return "UNK";
}

inline const char* to_string(CurrentPhase p)
{
    switch (p)
    {
    case CurrentPhase::ROTATION:         return "ROTATION";
    case CurrentPhase::TESTING_BOUNDARY: return "TEST_BND";
    case CurrentPhase::RANGE_EXTENSION:  return "RANGE_EXT";
    case CurrentPhase::PULLBACK:         return "PULLBACK";
    case CurrentPhase::FAILED_AUCTION:   return "FAILED_AUC";
    case CurrentPhase::UNKNOWN:          return "UNK";
    }
    return "UNK";
}

inline const char* to_string(AuctionIntent i)
{
    switch (i)
    {
    case AuctionIntent::NEUTRAL:      return "NEUTRAL";
    case AuctionIntent::ACCUMULATION: return "ACCUM";
    case AuctionIntent::DISTRIBUTION: return "DISTRIB";
    case AuctionIntent::ABSORPTION:   return "ABSORB";
    case AuctionIntent::EXHAUSTION:   return "EXHAUST";
    }
    return "UNK";
}

inline const char* to_string(ControlSide s)
{
    switch (s)
    {
    case ControlSide::NEUTRAL: return "NEUTRAL";
    case ControlSide::BUYER:   return "BUYER";
    case ControlSide::SELLER:  return "SELLER";
    }
    return "UNK";
}

// AMTMarketState to_string (SSOT for market regime)
inline const char* to_string(AMTMarketState s)
{
    switch (s)
    {
    case AMTMarketState::UNKNOWN:    return "UNKNOWN";
    case AMTMarketState::BALANCE:    return "BALANCE";
    case AMTMarketState::IMBALANCE:  return "IMBALANCE";
    }
    return "UNK";
}

inline const char* to_string(AggressionType a)
{
    switch (a)
    {
    case AggressionType::NEUTRAL:    return "NEUTRAL";
    case AggressionType::INITIATIVE: return "INITIATIVE";
    case AggressionType::RESPONSIVE: return "RESPONSIVE";
    }
    return "UNK";
}

inline const char* to_string(BalanceProfileShape s)
{
    switch (s)
    {
    case BalanceProfileShape::UNDEFINED:           return "UNDEF";
    case BalanceProfileShape::NORMAL_DISTRIBUTION: return "NORMAL_DIST";
    case BalanceProfileShape::D_SHAPED:            return "D_SHAPE";
    case BalanceProfileShape::BALANCED:            return "BALANCED";
    }
    return "UNK";
}

inline const char* to_string(ImbalanceProfileShape s)
{
    switch (s)
    {
    case ImbalanceProfileShape::UNDEFINED:        return "UNDEF";
    case ImbalanceProfileShape::P_SHAPED:         return "P_SHAPE";
    case ImbalanceProfileShape::B_SHAPED_LOWER:   return "B_SHAPE_LOW";
    case ImbalanceProfileShape::B_SHAPED_BIMODAL: return "B_SHAPE_BI";
    case ImbalanceProfileShape::THIN_VERTICAL:    return "THIN_VERT";
    }
    return "UNK";
}

// ============================================================================
// PATTERN ENUM TO_STRING FUNCTIONS (Phase 4 Evidence Logging)
// ============================================================================

inline const char* to_string(VolumeProfilePattern p)
{
    switch (p)
    {
    case VolumeProfilePattern::VOLUME_SHELF:       return "VOLUME_SHELF";
    case VolumeProfilePattern::VOLUME_CLUSTER:     return "VOLUME_CLUSTER";
    case VolumeProfilePattern::VOLUME_GAP:         return "VOLUME_GAP";
    case VolumeProfilePattern::VOLUME_VACUUM:      return "VOLUME_VACUUM";
    case VolumeProfilePattern::LEDGE_PATTERN:      return "LEDGE_PATTERN";
    case VolumeProfilePattern::VOLUME_MIGRATION:   return "VOLUME_MIGRATION";
    case VolumeProfilePattern::VOLUME_BREAKOUT:    return "VOLUME_BREAKOUT";
    case VolumeProfilePattern::LOW_VOLUME_BREAKOUT:return "LOW_VOL_BREAKOUT";
    }
    return "UNK";
}

inline const char* to_string(TPOMechanics t)
{
    switch (t)
    {
    case TPOMechanics::TPO_OVERLAP:    return "TPO_OVERLAP";
    case TPOMechanics::TPO_SEPARATION: return "TPO_SEPARATION";
    }
    return "UNK";
}

inline const char* to_string(BalanceDOMPattern p)
{
    switch (p)
    {
    case BalanceDOMPattern::STACKED_BIDS:     return "STACKED_BIDS";
    case BalanceDOMPattern::STACKED_ASKS:     return "STACKED_ASKS";
    case BalanceDOMPattern::ORDER_RELOADING:  return "ORDER_RELOADING";
    case BalanceDOMPattern::SPOOF_ORDER_FLIP: return "SPOOF_ORDER_FLIP";
    }
    return "UNK";
}

inline const char* to_string(ImbalanceDOMPattern p)
{
    switch (p)
    {
    case ImbalanceDOMPattern::CHASING_ORDERS_BUY:    return "CHASING_BUY";
    case ImbalanceDOMPattern::CHASING_ORDERS_SELL:   return "CHASING_SELL";
    case ImbalanceDOMPattern::BID_ASK_RATIO_EXTREME: return "BID_ASK_EXTREME";
    case ImbalanceDOMPattern::ABSORPTION_FAILURE:    return "ABSORB_FAIL";
    }
    return "UNK";
}

inline const char* to_string(BalanceDeltaPattern p)
{
    switch (p)
    {
    case BalanceDeltaPattern::ABSORPTION_AT_HIGH:    return "ABSORB_HIGH";
    case BalanceDeltaPattern::ABSORPTION_AT_LOW:     return "ABSORB_LOW";
    case BalanceDeltaPattern::DELTA_DIVERGENCE_FADE: return "DELTA_DIV_FADE";
    case BalanceDeltaPattern::AGGRESSIVE_INITIATION: return "AGGR_INIT";
    }
    return "UNK";
}

inline const char* to_string(ImbalanceDeltaPattern p)
{
    switch (p)
    {
    case ImbalanceDeltaPattern::STRONG_CONVERGENCE: return "STRONG_CONV";
    case ImbalanceDeltaPattern::WEAK_PULLBACK:      return "WEAK_PULLBACK";
    case ImbalanceDeltaPattern::EFFORT_NO_RESULT:   return "EFFORT_NO_RES";
    case ImbalanceDeltaPattern::CLIMAX_EXHAUSTION:  return "CLIMAX_EXHAUST";
    }
    return "UNK";
}

inline const char* to_string(DOMControlPattern p)
{
    switch (p)
    {
    case DOMControlPattern::BUYERS_LIFTING_ASKS:   return "BUYERS_LIFT";
    case DOMControlPattern::SELLERS_HITTING_BIDS:  return "SELLERS_HIT";
    case DOMControlPattern::LIQUIDITY_PULLING:     return "LIQ_PULLING";
    case DOMControlPattern::LIQUIDITY_STACKING:    return "LIQ_STACKING";
    case DOMControlPattern::EXHAUSTION_DIVERGENCE: return "EXHAUST_DIV";
    }
    return "UNK";
}

inline const char* to_string(DOMEvent e)
{
    switch (e)
    {
    case DOMEvent::LIQUIDITY_DISAPPEARANCE: return "LIQ_DISAPPEAR";
    case DOMEvent::ORDER_FLOW_REVERSAL:     return "FLOW_REVERSAL";
    case DOMEvent::SWEEP_LIQUIDATION:       return "SWEEP_LIQ";
    case DOMEvent::LARGE_LOT_EXECUTION:     return "LARGE_LOT";
    }
    return "UNK";
}

// ============================================================================
// PATTERN LOGGER (Event-Style, Transition-Only)
// ============================================================================
// Logs patterns only when first observed on a bar.
// Does NOT expose to arbitration. Logging/diagnostics ONLY.
// ============================================================================

struct PatternLogger
{
    int lastLoggedBar = -1;

    // Bit flags for patterns logged this bar (to avoid duplicates)
    uint32_t volumePatternsLogged = 0;
    uint32_t tpoMechanicsLogged = 0;
    uint32_t balanceDOMLogged = 0;
    uint32_t imbalanceDOMLogged = 0;
    uint32_t balanceDeltaLogged = 0;
    uint32_t imbalanceDeltaLogged = 0;
    uint32_t domControlLogged = 0;
    uint32_t domEventsLogged = 0;

    bool capabilityLoggedThisSession = false;

    void ResetForNewBar(int bar)
    {
        if (bar != lastLoggedBar) {
            lastLoggedBar = bar;
            volumePatternsLogged = 0;
            tpoMechanicsLogged = 0;
            balanceDOMLogged = 0;
            imbalanceDOMLogged = 0;
            balanceDeltaLogged = 0;
            imbalanceDeltaLogged = 0;
            domControlLogged = 0;
            domEventsLogged = 0;
        }
    }

    void ResetForNewSession()
    {
        capabilityLoggedThisSession = false;
    }
};

} // namespace AMT

#endif // AMT_PATTERNS_H
