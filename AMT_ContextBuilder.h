// ============================================================================
// AMT_ContextBuilder.h
// SSOT: AuctionContext Builder (Phases 1-3 Implementation)
//
// PURPOSE: Single authoritative builder for AuctionContext population.
// All semantic interpretation happens here. Consumers read the result.
//
// CONTRACT:
//   - AuctionContext is written ONLY by Build()
//   - Build() is called ONCE per bar after all observables are collected
//   - All fields either have valid values OR explicit validity=false
//   - No silent defaults - every field is intentionally set
//
// CONSTRAINTS (Builder MUST NOT):
//   - Make trading decisions
//   - Modify any input struct
//   - Access Sierra Chart APIs directly
//   - Write to logs
//   - Store state between calls (stateless builder)
//   - Use fallback defaults silently
// ============================================================================

#ifndef AMT_CONTEXT_BUILDER_H
#define AMT_CONTEXT_BUILDER_H

#include "amt_core.h"
#include "AMT_Patterns.h"
#include "AMT_Arbitration_Seam.h"
#include "AMT_Liquidity.h"       // LiquidityState, Liq3Result
#include <cmath>
#include <algorithm>

namespace AMT {

// ============================================================================
// BUILDER INPUT STRUCT
// Collects all required inputs for context building.
// This struct is populated by the caller before calling Build().
// ============================================================================

struct ContextBuilderInput {
    // =========================================================================
    // REGIME INPUTS
    // =========================================================================

    // Session phase (from SessionPhaseCoordinator)
    SessionPhase sessionPhase = SessionPhase::UNKNOWN;

    // Current phase (from PhaseSnapshot)
    CurrentPhase currentPhase = CurrentPhase::ROTATION;
    bool phaseSnapshotValid = false;
    bool phaseIsDirectional = false;  // From PhaseSnapshot.IsDirectional()

    // Market state tracker output (SSOT: DaltonEngine via 1TF/2TF)
    AMTMarketState confirmedState = AMTMarketState::BALANCE;
    AMTMarketState priorConfirmedState = AMTMarketState::BALANCE;

    // Facilitation (already computed in main study)
    AuctionFacilitation facilitation = AuctionFacilitation::UNKNOWN;
    bool facilitationComputed = false;

    // =========================================================================
    // CONTROL INPUTS (Delta-based)
    // =========================================================================

    // Delta consistency (from confidence.deltaConsistency)
    double deltaConsistency = 0.5;
    bool deltaConsistencyValid = false;

    // Session delta (from session accumulator)
    double sessionCumDelta = 0.0;
    double sessionTotalVolume = 0.0;

    // Session delta baseline readiness
    bool sessionDeltaBaselineReady = false;
    double sessionDeltaPctile = 50.0;  // Percentile if baseline ready

    // =========================================================================
    // ENVIRONMENT INPUTS (Range and Depth)
    // =========================================================================

    // Range (for volatility classification)
    double barRangeTicks = 0.0;
    bool rangeBaselineReady = false;
    double rangePctile = 50.0;  // Percentile if baseline ready

    // Close change (for 2D volatility refinement - directional travel)
    double closeChangeTicks = 0.0;
    bool closeChangeBaselineReady = false;
    double closeChangePctile = 50.0;  // Percentile if baseline ready

    // DOM depth (for liquidity classification)
    double curDepth = 0.0;
    bool depthBaselineReady = false;
    double depthPctile = 50.0;  // Percentile if baseline ready
    bool domInputsConfigured = false;

    // 3-Component Liquidity Model (Dec 2024)
    // Direct output from LiquidityEngine - bypasses old ClassifyLiquidity()
    LiquidityState liqState = LiquidityState::LIQ_NOT_READY;
    bool liqStateValid = false;

    // =========================================================================
    // NARRATIVE INPUTS (Zone engagement state)
    // =========================================================================

    // Engaged zone context
    int engagedZoneId = -1;
    ZoneType engagedZoneType = ZoneType::NONE;
    ZoneProximity engagedZoneProximity = ZoneProximity::INACTIVE;
    AuctionOutcome engagementOutcome = AuctionOutcome::PENDING;  // Current engagement outcome

    // Zone boundary info (for intent classification)
    bool atUpperBoundary = false;  // At VAH
    bool atLowerBoundary = false;  // At VAL
    bool atPOC = false;

    // Volume context (for intent detection)
    double barVolume = 0.0;
    bool volumeIncreasing = false;  // Volume above recent average

    // =========================================================================
    // PHASE 4 INPUTS (Pattern Evidence - append-only, descriptive)
    // These inputs enable binary pattern detection. Patterns do not affect
    // any other field in AuctionContext. They are evidence for logging only.
    // =========================================================================

    // Volume percentile (for volume patterns)
    double volumePctile = 50.0;
    bool volumeBaselineReady = false;

    // Delta direction signals
    double deltaPct = 0.0;  // Bar delta as percentage of volume (-1 to +1)

    // DOM signals (for DOM patterns)
    double bidStackPull = 0.0;  // Positive = stacking, negative = pulling
    double askStackPull = 0.0;
    double domBidSize = 0.0;
    double domAskSize = 0.0;

    // Bar index (for timestamping pattern evidence)
    int currentBar = 0;
};

// ============================================================================
// VOLATILITY CLASSIFICATION
// Thresholds are adaptive based on session baseline percentiles
// 2D refinement when close-change baseline is available:
//   - High range + low travel = INDECISIVE (whipsaw) → maps to HIGH
//   - Low range + high travel = potential breakout → maps to NORMAL
// ============================================================================

// Core classification logic (range + optional close-change)
inline VolatilityState ClassifyVolatility(double rangePctile,
                                          double closeChangePctile,
                                          bool closeChangeValid) {
    // If close change baseline not ready, fall back to range-only
    if (!closeChangeValid) {
        // Existing range-only logic
        if (rangePctile >= 90.0) return VolatilityState::EXTREME;
        if (rangePctile >= 75.0) return VolatilityState::HIGH;
        if (rangePctile <= 25.0) return VolatilityState::LOW;
        return VolatilityState::NORMAL;
    }

    // Two-dimensional classification
    const bool highRange = (rangePctile >= 75.0);
    const bool lowRange = (rangePctile <= 25.0);
    const bool highTravel = (closeChangePctile >= 75.0);
    const bool lowTravel = (closeChangePctile <= 25.0);

    // EXTREME: Very high range AND high directional travel
    if (rangePctile >= 90.0 && highTravel) return VolatilityState::EXTREME;

    // HIGH: High range with high travel (confirmed volatility)
    if (highRange && highTravel) return VolatilityState::HIGH;

    // LOW: Low range AND low travel (compressed, quiet market)
    if (lowRange && lowTravel) return VolatilityState::LOW;

    // Refinement cases (map to existing states, distinguished by logging):
    // High range + low travel = INDECISIVE whipsaw → still HIGH volatility
    if (highRange && lowTravel) {
        return VolatilityState::HIGH;  // Character: INDECISIVE
    }

    // Low range + high travel = breakout potential → NORMAL (not LOW)
    if (lowRange && highTravel) {
        return VolatilityState::NORMAL;  // Character: BREAKOUT_POTENTIAL
    }

    return VolatilityState::NORMAL;
}

// Backward-compatible overload (range-only)
inline VolatilityState ClassifyVolatility(double rangePctile) {
    return ClassifyVolatility(rangePctile, 50.0, false);
}

// ============================================================================
// LIQUIDITY CLASSIFICATION
// Thresholds are adaptive based on DOM depth baseline percentiles
// ============================================================================

inline LiquidityState ClassifyLiquidity(double depthPctile) {
    // Quartile-based classification
    if (depthPctile <= 10.0) return LiquidityState::LIQ_VOID;
    if (depthPctile <= 25.0) return LiquidityState::LIQ_THIN;
    if (depthPctile >= 75.0) return LiquidityState::LIQ_THICK;
    return LiquidityState::LIQ_NORMAL;
}

// ============================================================================
// CONTROL SIDE CLASSIFICATION
// Derived from delta sign when aggression is valid
// ============================================================================

inline ControlSide ClassifySide(double sessionDeltaPct, double deltaConsistency) {
    // Use session delta sign for persistent direction
    // Use bar delta (deltaConsistency) as secondary signal
    if (sessionDeltaPct > 0.02) return ControlSide::BUYER;   // >2% net buying
    if (sessionDeltaPct < -0.02) return ControlSide::SELLER; // >2% net selling

    // Fall back to bar-level signal
    if (deltaConsistency > 0.6) return ControlSide::BUYER;   // 60%+ at ask
    if (deltaConsistency < 0.4) return ControlSide::SELLER;  // 60%+ at bid

    return ControlSide::NEUTRAL;
}

// ============================================================================
// INTENT CLASSIFICATION
// Based on zone engagement context and flow characteristics
// ============================================================================

inline AuctionIntent ClassifyIntent(
    const ContextBuilderInput& in,
    AggressionType aggression,
    bool isRejection)
{
    // No zone engagement = NEUTRAL
    if (in.engagedZoneId < 0) {
        return AuctionIntent::NEUTRAL;
    }

    // ABSORPTION: Initiative hitting boundary with rejection developing
    if (aggression == AggressionType::INITIATIVE && isRejection) {
        return AuctionIntent::ABSORPTION;
    }

    // EXHAUSTION: Extreme delta at boundary showing failure
    // (Initiative without follow-through)
    if (in.deltaConsistencyValid && in.deltaConsistency > 0.7) {
        if ((in.atUpperBoundary && in.sessionCumDelta > 0) ||
            (in.atLowerBoundary && in.sessionCumDelta < 0)) {
            // Delta pushing INTO boundary but not breaking through
            return AuctionIntent::EXHAUSTION;
        }
    }

    // ACCUMULATION: Responsive at VAL (buying at support)
    // AMT: Responsive at lower boundary suggests absorption of selling pressure
    if (in.atLowerBoundary && aggression == AggressionType::RESPONSIVE) {
        return AuctionIntent::ACCUMULATION;
    }

    // DISTRIBUTION: Responsive at VAH (selling at resistance)
    // AMT: Responsive at upper boundary suggests absorption of buying pressure
    if (in.atUpperBoundary && aggression == AggressionType::RESPONSIVE) {
        return AuctionIntent::DISTRIBUTION;
    }

    return AuctionIntent::NEUTRAL;
}

// ============================================================================
// TRANSITION CLASSIFICATION
// Based on confirmed state changes
// ============================================================================

inline TransitionMechanic ClassifyTransition(
    AMTMarketState priorState,
    AMTMarketState currentState,
    bool stateChanged)
{
    if (!stateChanged) {
        return TransitionMechanic::NONE;
    }

    // Skip transitions involving UNKNOWN (initialization artifact)
    if (priorState == AMTMarketState::UNKNOWN ||
        currentState == AMTMarketState::UNKNOWN) {
        return TransitionMechanic::NONE;
    }

    if (priorState == AMTMarketState::BALANCE &&
        currentState == AMTMarketState::IMBALANCE) {
        return TransitionMechanic::BALANCE_TO_IMBALANCE;
    }

    if (priorState == AMTMarketState::IMBALANCE &&
        currentState == AMTMarketState::BALANCE) {
        return TransitionMechanic::IMBALANCE_TO_BALANCE;
    }

    return TransitionMechanic::NONE;
}

// ============================================================================
// AUCTION CONTEXT BUILDER
// ============================================================================

struct AuctionContextBuilder {

    /**
     * Build complete AuctionContext from inputs.
     * This is the SINGLE AUTHORITATIVE place where AuctionContext is populated.
     *
     * @param in  Collected inputs from observables and baselines
     * @return    Fully populated AuctionContext with validity flags
     */
    static AuctionContext Build(const ContextBuilderInput& in) {
        AuctionContext ctx;

        // Reset all validity flags first
        ctx.invalidate_all();

        // =====================================================================
        // PHASE 2: REGIME FIELDS
        // =====================================================================

        // Session (always valid once we have a value)
        ctx.session = in.sessionPhase;
        ctx.sessionValid = (in.sessionPhase != SessionPhase::UNKNOWN);

        // Phase (from PhaseSnapshot)
        ctx.phase = in.currentPhase;
        ctx.phaseValid = in.phaseSnapshotValid;

        // State (SSOT: DaltonEngine via 1TF/2TF detection)
        ctx.state = in.confirmedState;
        ctx.stateValid = (in.confirmedState != AMTMarketState::UNKNOWN);

        // Facilitation
        ctx.facilitation = in.facilitation;
        ctx.facilitationValid = in.facilitationComputed;

        // =====================================================================
        // PHASE 1: CONTROL FIELDS (Delta-based)
        // =====================================================================

        // Compute session delta percentage
        double sessionDeltaPct = 0.0;
        if (in.sessionTotalVolume > 0.0) {
            sessionDeltaPct = in.sessionCumDelta / in.sessionTotalVolume;
        }
        ctx.sessionDeltaPct = sessionDeltaPct;

        // Session delta percentile (from baseline if ready)
        ctx.sessionDeltaPctile = in.sessionDeltaPctile;
        ctx.sessionDeltaValid = in.sessionDeltaBaselineReady;

        // Extreme delta detection (persistence-validated)
        // BUG FIX: Check BOTH directions - extreme buying (>0.7) AND extreme selling (<0.3)
        ctx.isExtremeDeltaBar = in.deltaConsistencyValid &&
            (in.deltaConsistency > AMT_Arb::EXTREME_DELTA_HIGH_THRESHOLD ||
             in.deltaConsistency < AMT_Arb::EXTREME_DELTA_LOW_THRESHOLD);

        ctx.isExtremeDeltaSession = in.sessionDeltaBaselineReady &&
            (in.sessionDeltaPctile >= AMT_Arb::SESSION_EXTREME_PCTILE_THRESHOLD);

        ctx.isExtremeDelta = ctx.isExtremeDeltaBar && ctx.isExtremeDeltaSession;

        // Directional coherence
        const bool deltaPositive = (sessionDeltaPct > 0.0);
        const bool barDeltaPositive = in.deltaConsistencyValid &&
            (in.deltaConsistency > 0.5);
        ctx.directionalCoherence = in.sessionDeltaBaselineReady &&
            (deltaPositive == barDeltaPositive);

        // Aggression classification (coherence-gated)
        if (in.deltaConsistencyValid) {
            ctx.aggression = (ctx.isExtremeDelta && ctx.directionalCoherence)
                ? AggressionType::INITIATIVE
                : AggressionType::RESPONSIVE;
            ctx.aggressionValid = true;
        } else {
            ctx.aggression = AggressionType::NEUTRAL;
            ctx.aggressionValid = false;
        }

        // Side classification
        if (in.deltaConsistencyValid || in.sessionTotalVolume > 0.0) {
            ctx.side = ClassifySide(sessionDeltaPct, in.deltaConsistency);
            ctx.sideValid = true;
        } else {
            ctx.side = ControlSide::NEUTRAL;
            ctx.sideValid = false;
        }

        // =====================================================================
        // PHASE 1: ENVIRONMENT FIELDS
        // =====================================================================

        // Volatility (from range baseline + optional close-change refinement)
        if (in.rangeBaselineReady) {
            ctx.volatility = ClassifyVolatility(in.rangePctile,
                                                 in.closeChangePctile,
                                                 in.closeChangeBaselineReady);
            ctx.volatilityValid = true;
        } else {
            ctx.volatility = VolatilityState::NORMAL;
            ctx.volatilityValid = false;
        }

        // Liquidity (3-Component Model - NO FALLBACKS)
        // Uses LiquidityEngine output (depth mass, stress, resilience)
        // If model not ready, liquidityValid=false - no silent defaults
        if (in.liqStateValid && IsLiquidityStateReady(in.liqState)) {
            // 3-Component model is ready and valid
            ctx.liquidity = in.liqState;
            ctx.liquidityValid = true;
        } else {
            // Model not ready: LIQ_NOT_READY or no data
            // Set valid=false; legacy field value is meaningless
            ctx.liquidity = LiquidityState::LIQ_NORMAL;  // Placeholder, MUST NOT be used
            ctx.liquidityValid = false;
        }

        // =====================================================================
        // PHASE 3: NARRATIVE FIELDS
        // =====================================================================

        // Transition (from state change)
        const bool stateChanged = (in.confirmedState != in.priorConfirmedState);
        ctx.transition = ClassifyTransition(
            in.priorConfirmedState,
            in.confirmedState,
            stateChanged);
        ctx.transitionValid = true;  // Always valid (NONE is valid outcome)

        // Intent classification (requires zone engagement)
        // Simple rejection heuristic: at boundary + defensive (responsive) posture
        const bool isRejection = (in.atUpperBoundary || in.atLowerBoundary) &&
            (ctx.aggression == AggressionType::RESPONSIVE);

        ctx.intent = ClassifyIntent(in, ctx.aggression, isRejection);
        ctx.intentValid = (in.engagedZoneId >= 0);  // Only valid when engaged

        // Outcome - from current zone engagement (if any)
        // Valid when engaged to a zone; shows engagement lifecycle state
        ctx.outcome = in.engagementOutcome;
        ctx.outcomeValid = (in.engagedZoneId >= 0);  // Valid when engaged

        // =====================================================================
        // PHASE 4: PATTERN EVIDENCE (Append-Only, Descriptive)
        // =====================================================================
        // CONTRACT: Patterns are EVIDENCE, not CAUSES.
        //   - Patterns do NOT influence any other field in AuctionContext
        //   - Patterns are append-only per bar
        //   - Patterns describe what happened, not what it means
        //   - All detection is binary (observed / not observed)
        //   - Consumers: logging, diagnostics, replay tools ONLY
        //   - NOT consumed by: arbitration, entry/exit, regime detection
        // =====================================================================

        // Clear pattern vectors (fresh slate each bar)
        ctx.clear_patterns();

        // ---------------------------------------------------------------------
        // VOLUME PROFILE PATTERNS
        // Binary detection from volume/range percentile relationships
        // ---------------------------------------------------------------------

        // VOLUME_GAP: Low volume + High range = price moved through vacuum
        if (in.volumeBaselineReady && in.rangeBaselineReady) {
            if (in.volumePctile < 25.0 && in.rangePctile > 75.0) {
                ctx.volumePatterns.push_back(VolumeProfilePattern::VOLUME_GAP);
            }
        }

        // VOLUME_VACUUM: Liquidity void detected
        if (ctx.liquidityValid && ctx.liquidity == LiquidityState::LIQ_VOID) {
            ctx.volumePatterns.push_back(VolumeProfilePattern::VOLUME_VACUUM);
        }

        // VOLUME_BREAKOUT: High volume + directional phase
        if (in.volumeBaselineReady && in.volumePctile > 75.0 && ctx.IsDirectional()) {
            ctx.volumePatterns.push_back(VolumeProfilePattern::VOLUME_BREAKOUT);
        }

        // LOW_VOLUME_BREAKOUT: Low volume + directional phase (potential trap)
        if (in.volumeBaselineReady && in.volumePctile < 25.0 && ctx.IsDirectional()) {
            ctx.volumePatterns.push_back(VolumeProfilePattern::LOW_VOLUME_BREAKOUT);
        }

        // ---------------------------------------------------------------------
        // TPO MECHANICS
        // Binary detection from market state
        // ---------------------------------------------------------------------

        if (ctx.stateValid) {
            if (ctx.state == AMTMarketState::BALANCE) {
                ctx.tpoMechanics.push_back(TPOMechanics::TPO_OVERLAP);
            } else if (ctx.state == AMTMarketState::IMBALANCE) {
                ctx.tpoMechanics.push_back(TPOMechanics::TPO_SEPARATION);
            }
        }

        // ---------------------------------------------------------------------
        // DOM PATTERNS (Balance Context)
        // Detected when market is in BALANCE state
        // ---------------------------------------------------------------------

        if (ctx.stateValid && ctx.state == AMTMarketState::BALANCE && in.domInputsConfigured) {
            // STACKED_BIDS: Positive bid stack/pull signal
            if (in.bidStackPull > 0.0 && in.bidStackPull > in.askStackPull) {
                ctx.balanceDOMPatterns.push_back(BalanceDOMPattern::STACKED_BIDS);
            }

            // STACKED_ASKS: Positive ask stack/pull signal
            if (in.askStackPull > 0.0 && in.askStackPull > in.bidStackPull) {
                ctx.balanceDOMPatterns.push_back(BalanceDOMPattern::STACKED_ASKS);
            }
        }

        // ---------------------------------------------------------------------
        // DOM PATTERNS (Imbalance Context)
        // Detected when market is in IMBALANCE state
        // ---------------------------------------------------------------------

        if (ctx.stateValid && ctx.state == AMTMarketState::IMBALANCE) {
            // CHASING_ORDERS_BUY: Buyer control + initiative aggression
            if (ctx.sideValid && ctx.side == ControlSide::BUYER &&
                ctx.aggressionValid && ctx.aggression == AggressionType::INITIATIVE) {
                ctx.imbalanceDOMPatterns.push_back(ImbalanceDOMPattern::CHASING_ORDERS_BUY);
            }

            // CHASING_ORDERS_SELL: Seller control + initiative aggression
            if (ctx.sideValid && ctx.side == ControlSide::SELLER &&
                ctx.aggressionValid && ctx.aggression == AggressionType::INITIATIVE) {
                ctx.imbalanceDOMPatterns.push_back(ImbalanceDOMPattern::CHASING_ORDERS_SELL);
            }

            // BID_ASK_RATIO_EXTREME: Extreme bid/ask depth ratio (>3:1 or <1:3)
            if (in.domInputsConfigured && in.domBidSize > 0.0 && in.domAskSize > 0.0) {
                const double ratio = in.domBidSize / in.domAskSize;
                if (ratio > 3.0 || ratio < 0.333) {
                    ctx.imbalanceDOMPatterns.push_back(ImbalanceDOMPattern::BID_ASK_RATIO_EXTREME);
                }
            }
        }

        // ---------------------------------------------------------------------
        // DELTA PATTERNS (Balance Context)
        // Detected at zone boundaries during balance
        // ---------------------------------------------------------------------

        if (ctx.stateValid && ctx.state == AMTMarketState::BALANCE) {
            // ABSORPTION_AT_HIGH: At upper boundary + responsive + selling delta
            if (in.atUpperBoundary && ctx.aggressionValid &&
                ctx.aggression == AggressionType::RESPONSIVE && in.deltaPct < -0.3) {
                ctx.balanceDeltaPatterns.push_back(BalanceDeltaPattern::ABSORPTION_AT_HIGH);
            }

            // ABSORPTION_AT_LOW: At lower boundary + responsive + buying delta
            if (in.atLowerBoundary && ctx.aggressionValid &&
                ctx.aggression == AggressionType::RESPONSIVE && in.deltaPct > 0.3) {
                ctx.balanceDeltaPatterns.push_back(BalanceDeltaPattern::ABSORPTION_AT_LOW);
            }

            // DELTA_DIVERGENCE_FADE: At boundary + delta pushing into boundary
            if ((in.atUpperBoundary && in.deltaPct > 0.3) ||
                (in.atLowerBoundary && in.deltaPct < -0.3)) {
                ctx.balanceDeltaPatterns.push_back(BalanceDeltaPattern::DELTA_DIVERGENCE_FADE);
            }
        }

        // ---------------------------------------------------------------------
        // DELTA PATTERNS (Imbalance Context)
        // Detected during directional moves
        // ---------------------------------------------------------------------

        if (ctx.stateValid && ctx.state == AMTMarketState::IMBALANCE) {
            // STRONG_CONVERGENCE: Directional phase + coherent delta
            if (ctx.directionalCoherence && ctx.IsDirectional()) {
                ctx.imbalanceDeltaPatterns.push_back(ImbalanceDeltaPattern::STRONG_CONVERGENCE);
            }

            // CLIMAX_EXHAUSTION: Extreme delta at boundary (potential reversal)
            if (ctx.isExtremeDelta && (in.atUpperBoundary || in.atLowerBoundary)) {
                ctx.imbalanceDeltaPatterns.push_back(ImbalanceDeltaPattern::CLIMAX_EXHAUSTION);
            }

            // EFFORT_NO_RESULT: High volume but no price extension
            if (in.volumeBaselineReady && in.volumePctile > 75.0 &&
                in.rangeBaselineReady && in.rangePctile < 25.0) {
                ctx.imbalanceDeltaPatterns.push_back(ImbalanceDeltaPattern::EFFORT_NO_RESULT);
            }
        }

        // ---------------------------------------------------------------------
        // DOM CONTROL PATTERNS
        // General order flow observations
        // ---------------------------------------------------------------------

        if (in.domInputsConfigured) {
            // BUYERS_LIFTING_ASKS: Positive delta + volume increasing
            if (in.deltaPct > 0.3 && in.volumeIncreasing) {
                ctx.domControlPatterns.push_back(DOMControlPattern::BUYERS_LIFTING_ASKS);
            }

            // SELLERS_HITTING_BIDS: Negative delta + volume increasing
            if (in.deltaPct < -0.3 && in.volumeIncreasing) {
                ctx.domControlPatterns.push_back(DOMControlPattern::SELLERS_HITTING_BIDS);
            }

            // LIQUIDITY_STACKING: High depth percentile
            if (in.depthBaselineReady && in.depthPctile > 75.0) {
                ctx.domControlPatterns.push_back(DOMControlPattern::LIQUIDITY_STACKING);
            }

            // LIQUIDITY_PULLING: Low depth percentile
            if (in.depthBaselineReady && in.depthPctile < 25.0) {
                ctx.domControlPatterns.push_back(DOMControlPattern::LIQUIDITY_PULLING);
            }
        }

        // ---------------------------------------------------------------------
        // DOM EVENTS
        // Discrete occurrences (not states)
        // ---------------------------------------------------------------------

        if (in.domInputsConfigured) {
            // LIQUIDITY_DISAPPEARANCE: Depth in bottom 5% (near-void condition)
            if (in.depthBaselineReady && in.depthPctile < 5.0) {
                ctx.domEvents.push_back(DOMEvent::LIQUIDITY_DISAPPEARANCE);
            }

            // LARGE_LOT_EXECUTION: Volume spike (top 10%)
            if (in.volumeBaselineReady && in.volumePctile > 90.0) {
                ctx.domEvents.push_back(DOMEvent::LARGE_LOT_EXECUTION);
            }
        }

        return ctx;
    }
};

} // namespace AMT

#endif // AMT_CONTEXT_BUILDER_H
