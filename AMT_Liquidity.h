#pragma once
// ============================================================================
// AMT_Liquidity.h - True Liquidity Measurement (Kyle's 4-Component Model)
// ============================================================================
//
// DEFINITION: Liquidity = executable near-touch depth vs aggressive pressure,
//             with refill capacity and execution cost (spread).
//
// KYLE'S FRAMEWORK (1985) - All 3 Dimensions:
//   1. Depth      (DepthMass)   - Distance-weighted resting volume within Dmax ticks
//   2. Resiliency (Resilience)  - Refill speed after depletion
//   3. Tightness  (Spread)      - Bid-ask spread (execution cost)
//
// PLUS:
//   4. Stress     - Aggressive demand relative to near-touch depth
//
// COMPOSITE FORMULA (with spread penalty):
//   resilienceContrib = stressRank * resilienceRank + (1 - stressRank) * 1.0
//   spreadPenalty     = 1.0 - (spreadWeight * spreadRank)  // 15% max penalty
//   LIQ = depthRank * (1 - stressRank) * resilienceContrib * spreadPenalty
//
// ADDITIONAL SIGNALS:
//   - toxicityProxy: Order flow asymmetry (VPIN-lite) for adverse selection
//   - peakLiquidity: Maximum depth during bar (from GetMax* API)
//   - consumedLiquidity: Peak - Ending (depth absorbed by aggression)
//
// LIQSTATE:
//   VOID   (LIQ <= 0.10 OR DepthRank <= 0.10)
//   THIN   (0.10 < LIQ <= 0.25 OR StressRank >= 0.90)
//   NORMAL (0.25 < LIQ < 0.75)
//   THICK  (LIQ >= 0.75)
//   LIQ_NOT_READY (baseline insufficient)
//
// NO FALLBACKS: If any core baseline not ready, emit error state.
//
// ============================================================================

#include "amt_core.h"
#include "AMT_Snapshots.h"
#include <algorithm>
#include <cmath>
#include <deque>

namespace AMT {

// ============================================================================
// CONFIGURATION
// ============================================================================

struct LiquidityConfig {
    int dmaxTicks = 4;              // Max distance from reference price (ES default: 4 ticks = 1 point)
    int maxDomLevels = 10;          // Max DOM levels to scan per side
    size_t baselineMinSamples = 10; // Minimum samples before baseline ready
    int baselineWindow = 300;       // Rolling window size (bars)
    double epsilon = 1.0;           // Small constant to avoid div-by-zero in stress calc

    // Kyle's Tightness component (spread impact on composite LIQ)
    double spreadWeight = 0.15;     // Weight of spread penalty in composite (0.15 = 15% max penalty)
    double spreadMaxTicks = 4.0;    // Spread above this is considered "wide" (ES: 4 ticks = 1 point)

    // ========================================================================
    // V1 STALENESS DETECTION
    // ========================================================================
    // DOM data can become stale if market data feed lags or disconnects.
    // Stale data = invalid for execution decisions (hard block).
    int staleThresholdMs = 2000;    // DOM data older than this is stale (2 seconds default)

    // ========================================================================
    // V1 EXECUTION FRICTION WEIGHTS
    // ========================================================================
    // executionFriction = w1*(1-depthRank) + w2*stressRank + w3*(1-resilienceRank) + w4*spreadRank
    // Higher friction = worse execution conditions.
    // Weights sum to 1.0 for bounded [0,1] output.
    double frictionWeightDepth = 0.35;       // Lack of depth increases friction
    double frictionWeightStress = 0.25;      // High stress increases friction
    double frictionWeightResilience = 0.20;  // Low resilience increases friction
    double frictionWeightSpread = 0.20;      // Wide spread increases friction

    // ========================================================================
    // V1 ACTION THRESHOLDS
    // ========================================================================
    // Thresholds for deriving LiquidityAction from friction score.
    // HARD_BLOCK: friction >= hardBlockThreshold (or stale/VOID)
    // WIDEN_TOLERANCE: friction >= widenThreshold && < hardBlockThreshold (or THIN)
    // PROCEED: friction < widenThreshold (and NORMAL/THICK)
    double hardBlockFrictionThreshold = 0.80;  // Severe friction -> hard block
    double widenFrictionThreshold = 0.50;      // Moderate friction -> widen tolerance
};

// ============================================================================
// LIQUIDITY ERROR TAXONOMY (No Silent Failures)
// ============================================================================
// Every failure path must set an explicit errorReason.
// Callers can log [LIQ-ERR] and increment session counters.
// ============================================================================

// ============================================================================
// V1 LIQUIDITY ACTION (Consumer Guidance)
// ============================================================================
// Recommended action for consumers based on liquidity conditions.
// The engine RECOMMENDS, consumers DECIDE whether to apply.
//
// HARD_BLOCK: Do not execute. Wait for conditions to improve.
//   - Triggered by: stale DOM, VOID state, severe friction
// WIDEN_TOLERANCE: Proceed with caution. Increase slippage tolerance or reduce size.
//   - Triggered by: THIN state, moderate friction
// PROCEED: Execute normally. Liquidity is adequate.
//   - Triggered by: NORMAL/THICK state, low friction, fresh data
// ============================================================================

enum class LiquidityAction : int {
    PROCEED = 0,          // Liquidity adequate, execute normally
    WIDEN_TOLERANCE = 1,  // Proceed with caution, may need larger slippage
    HARD_BLOCK = 2        // Do not execute, conditions unsafe
};

inline const char* LiquidityActionToString(LiquidityAction a) {
    switch (a) {
        case LiquidityAction::PROCEED:          return "PROCEED";
        case LiquidityAction::WIDEN_TOLERANCE:  return "WIDEN";
        case LiquidityAction::HARD_BLOCK:       return "BLOCK";
    }
    return "UNK";
}

enum class LiquidityErrorReason : int {
    NONE = 0,                       // No error, liqValid = true

    // Input validation errors (prevent Compute() from running at call site)
    ERR_DOM_INPUTS_INVALID = 1,     // domInputsValid == false
    ERR_REF_PRICE_INVALID = 2,      // referencePrice <= 0.0
    ERR_TICK_SIZE_INVALID = 3,      // tickSize <= 0.0
    ERR_HIST_DEPTH_UNAVAILABLE = 4, // Historical depth API returned no data

    // DOM extraction errors (inside ComputeDepthMass)
    ERR_NO_DOM_LEVELS = 5,          // bidLevels + askLevels == 0 (no valid levels within Dmax)

    // V1: Staleness error (DOM data too old)
    ERR_DEPTH_STALE = 6,            // DOM timestamp > staleThresholdMs ago

    // Baseline warmup states (not errors, but explicit tracking)
    WARMUP_DEPTH = 10,              // depthBaseline not ready
    WARMUP_STRESS = 11,             // stressBaseline not ready
    WARMUP_RESILIENCE = 12,         // resilienceBaseline not ready (includes first-bar)
    WARMUP_MULTIPLE = 13,           // Multiple baselines not ready

    // Internal consistency errors (should never occur - bug detectors)
    ERR_PERCENTILE_EMPTY = 20,      // PercentileRank called on empty baseline
};

inline const char* LiquidityErrorReasonToString(LiquidityErrorReason r) {
    switch (r) {
        case LiquidityErrorReason::NONE:                     return "NONE";
        case LiquidityErrorReason::ERR_DOM_INPUTS_INVALID:   return "DOM_INPUTS_INVALID";
        case LiquidityErrorReason::ERR_REF_PRICE_INVALID:    return "REF_PRICE_INVALID";
        case LiquidityErrorReason::ERR_TICK_SIZE_INVALID:    return "TICK_SIZE_INVALID";
        case LiquidityErrorReason::ERR_HIST_DEPTH_UNAVAILABLE: return "HIST_DEPTH_UNAVAIL";
        case LiquidityErrorReason::ERR_NO_DOM_LEVELS:        return "NO_DOM_LEVELS";
        case LiquidityErrorReason::ERR_DEPTH_STALE:          return "DEPTH_STALE";
        case LiquidityErrorReason::WARMUP_DEPTH:             return "WARMUP_DEPTH";
        case LiquidityErrorReason::WARMUP_STRESS:            return "WARMUP_STRESS";
        case LiquidityErrorReason::WARMUP_RESILIENCE:        return "WARMUP_RES";
        case LiquidityErrorReason::WARMUP_MULTIPLE:          return "WARMUP_MULTI";
        case LiquidityErrorReason::ERR_PERCENTILE_EMPTY:     return "PERCENTILE_EMPTY";
    }
    return "UNKNOWN";
}

// ============================================================================
// LIQUIDITY ERROR COUNTERS (Session-Scoped)
// ============================================================================
// Owned by StudyState, reset at session boundary.
// Enables summary reporting and tuning diagnostics.
// ============================================================================

struct LiquidityErrorCounters {
    int domInputsInvalidCount = 0;
    int refPriceInvalidCount = 0;
    int tickSizeInvalidCount = 0;
    int histDepthUnavailableCount = 0;
    int noDomLevelsCount = 0;
    int depthStaleCount = 0;       // V1: DOM data staleness errors
    int warmupBarsCount = 0;
    int percentileEmptyCount = 0;  // Should ALWAYS be 0 (bug detector)
    int totalErrorBars = 0;        // Any bar where liqValid == false
    int totalValidBars = 0;        // Bars where liqValid == true

    void Reset() {
        domInputsInvalidCount = 0;
        refPriceInvalidCount = 0;
        tickSizeInvalidCount = 0;
        histDepthUnavailableCount = 0;
        noDomLevelsCount = 0;
        depthStaleCount = 0;
        warmupBarsCount = 0;
        percentileEmptyCount = 0;
        totalErrorBars = 0;
        totalValidBars = 0;
    }

    void IncrementFor(LiquidityErrorReason reason) {
        if (reason == LiquidityErrorReason::NONE) {
            totalValidBars++;
            return;
        }
        totalErrorBars++;
        switch (reason) {
            case LiquidityErrorReason::ERR_DOM_INPUTS_INVALID:   domInputsInvalidCount++; break;
            case LiquidityErrorReason::ERR_REF_PRICE_INVALID:    refPriceInvalidCount++; break;
            case LiquidityErrorReason::ERR_TICK_SIZE_INVALID:    tickSizeInvalidCount++; break;
            case LiquidityErrorReason::ERR_HIST_DEPTH_UNAVAILABLE: histDepthUnavailableCount++; break;
            case LiquidityErrorReason::ERR_NO_DOM_LEVELS:        noDomLevelsCount++; break;
            case LiquidityErrorReason::ERR_DEPTH_STALE:          depthStaleCount++; break;
            case LiquidityErrorReason::WARMUP_DEPTH:
            case LiquidityErrorReason::WARMUP_STRESS:
            case LiquidityErrorReason::WARMUP_RESILIENCE:
            case LiquidityErrorReason::WARMUP_MULTIPLE:          warmupBarsCount++; break;
            case LiquidityErrorReason::ERR_PERCENTILE_EMPTY:     percentileEmptyCount++; break;
            default: break;
        }
    }
};

// ============================================================================
// LIQUIDITY STATE HELPERS
// LiquidityState enum is now in amt_core.h (unified, no legacy mapping needed)
// ============================================================================

inline const char* LiquidityStateToString(LiquidityState s) {
    switch (s) {
        case LiquidityState::LIQ_NOT_READY: return "NOT_READY";
        case LiquidityState::LIQ_VOID:      return "VOID";
        case LiquidityState::LIQ_THIN:      return "THIN";
        case LiquidityState::LIQ_NORMAL:    return "NORMAL";
        case LiquidityState::LIQ_THICK:     return "THICK";
    }
    return "UNK";
}

// Check if state is usable (not an error)
inline bool IsLiquidityStateReady(LiquidityState s) {
    return s != LiquidityState::LIQ_NOT_READY;
}

// ============================================================================
// DEPTH MASS RESULT
// ============================================================================

struct DepthMassResult {
    double bidMass = 0.0;      // Distance-weighted bid depth within Dmax (end-of-bar)
    double askMass = 0.0;      // Distance-weighted ask depth within Dmax (end-of-bar)
    double totalMass = 0.0;    // bidMass + askMass
    double imbalance = 0.0;    // (bid - ask) / (bid + ask), [-1, +1]
    int bidLevels = 0;         // Number of bid levels within Dmax
    int askLevels = 0;         // Number of ask levels within Dmax
    bool valid = false;        // True if calculation succeeded

    // Peak liquidity (maximum depth during bar timeframe)
    double peakBidMass = 0.0;  // Max distance-weighted bid depth during bar
    double peakAskMass = 0.0;  // Max distance-weighted ask depth during bar
    double peakTotalMass = 0.0; // peakBidMass + peakAskMass
    bool peakValid = false;    // True if peak quantities were computed

    // Liquidity consumed = peak - ending (depth that was available but consumed)
    double consumedBidMass = 0.0;
    double consumedAskMass = 0.0;
    double consumedTotalMass = 0.0;
};

// ============================================================================
// SPATIAL LIQUIDITY PROFILE - Error Taxonomy
// ============================================================================

enum class SpatialErrorReason : int {
    NONE = 0,
    ERR_NO_LEVEL_DATA = 1,        // No bid or ask levels provided
    ERR_INVALID_REF_PRICE = 2,    // Reference price <= 0
    ERR_INVALID_TICK_SIZE = 3,    // Tick size <= 0
    WARMUP_DEPTH_BASELINE = 10,   // Depth baseline not ready for sigma calculation
    INSUFFICIENT_LEVELS = 20,     // < 3 levels per side (can't compute meaningful stats)
    ONE_SIDED_BOOK = 21           // Only bid or only ask levels (asymmetric book)
};

inline const char* SpatialErrorReasonToString(SpatialErrorReason r) {
    switch (r) {
        case SpatialErrorReason::NONE:                  return "NONE";
        case SpatialErrorReason::ERR_NO_LEVEL_DATA:     return "NO_LEVEL_DATA";
        case SpatialErrorReason::ERR_INVALID_REF_PRICE: return "INVALID_REF_PRICE";
        case SpatialErrorReason::ERR_INVALID_TICK_SIZE: return "INVALID_TICK_SIZE";
        case SpatialErrorReason::WARMUP_DEPTH_BASELINE: return "WARMUP_DEPTH";
        case SpatialErrorReason::INSUFFICIENT_LEVELS:   return "INSUFFICIENT_LEVELS";
        case SpatialErrorReason::ONE_SIDED_BOOK:        return "ONE_SIDED_BOOK";
    }
    return "UNKNOWN";
}

// ============================================================================
// SPATIAL LIQUIDITY PROFILE - Per-Level Information
// ============================================================================

struct LevelInfo {
    double priceTicks = 0.0;      // Price in ticks from tick=0
    double volume = 0.0;          // Raw volume at level
    double distanceTicks = 0.0;   // Distance from reference price (always >= 0)
    double weight = 0.0;          // 1 / (1 + distance) weighting
    bool isBid = true;            // True for bid side, false for ask side
};

// ============================================================================
// SPATIAL LIQUIDITY PROFILE - Wall Detection (Depth > 2.5σ)
// ============================================================================
// Walls are significant depth concentrations that act as barriers to price movement.
// Based on statistical outlier detection: depth > mean + 2.5 * stddev.

struct WallInfo {
    double priceTicks = 0.0;      // Price level in ticks
    double volume = 0.0;          // Volume at this level
    double sigmaScore = 0.0;      // (depth - mean) / stddev
    int distanceFromRef = 0;      // Distance from reference price in ticks
    bool isBid = true;            // Bid wall (support) vs Ask wall (resistance)
    bool isIceberg = false;       // Detected refill pattern (future enhancement)

    bool IsSignificant() const { return sigmaScore >= 2.5; }
    bool IsStrong() const { return sigmaScore >= 3.0; }
    bool IsExtreme() const { return sigmaScore >= 4.0; }
};

// ============================================================================
// SPATIAL LIQUIDITY PROFILE - Void Detection (Depth < 10% of mean or gaps)
// ============================================================================
// Voids are areas of thin liquidity where price can accelerate through.

struct VoidInfo {
    double startTicks = 0.0;      // Start price in ticks
    double endTicks = 0.0;        // End price in ticks
    int gapTicks = 0;             // Size of gap in ticks
    double avgDepthRatio = 0.0;   // Average depth / mean depth (< 0.10 = void)
    bool isAboveRef = true;       // Above or below reference price

    bool IsVoid() const { return avgDepthRatio < 0.10; }
    bool IsThin() const { return avgDepthRatio < 0.25 && avgDepthRatio >= 0.10; }
};

// ============================================================================
// SPATIAL LIQUIDITY PROFILE - Directional Resistance (OBI + POLR)
// ============================================================================
// Order Book Imbalance (OBI) and Path of Least Resistance (POLR) analysis.
// Based on Cont et al. (2014): OBI explains ~65% of midpoint variation.

struct DirectionalResistance {
    double bidDepthWithinN = 0.0;     // Total bid depth within analysis range
    double askDepthWithinN = 0.0;     // Total ask depth within analysis range
    int rangeTicksUsed = 0;           // How many ticks analyzed
    double orderBookImbalance = 0.0;  // OBI: (bid - ask) / (bid + ask), [-1, +1]
    double polrRatio = 0.0;           // Ratio of lower/higher resistance
    bool polrIsUp = true;             // True if easier to move up (more bid than ask)
    bool valid = false;

    // Get directional bias from OBI
    // Positive = more bid depth = support below = bias up
    // Negative = more ask depth = resistance above = bias down
    double GetDirectionalBias() const {
        if (!valid) return 0.0;
        const double total = bidDepthWithinN + askDepthWithinN;
        if (total < 1.0) return 0.0;
        return (bidDepthWithinN - askDepthWithinN) / total;
    }
};

// ============================================================================
// SPATIAL LIQUIDITY PROFILE - Execution Risk Estimate
// ============================================================================
// Estimates slippage and risk for trading in a given direction.
// Uses Kyle's Lambda (1985): price impact = volume * lambda, where lambda ~ 1/depth.

struct ExecutionRiskEstimate {
    int targetTicks = 0;              // Target distance in ticks
    double estimatedSlippageTicks = 0.0;  // Kyle's lambda-based slippage estimate
    double cumulativeDepth = 0.0;     // Total depth in path
    double kyleLambda = 0.0;          // Price impact coefficient
    int wallsTraversed = 0;           // Number of walls in path
    int voidsTraversed = 0;           // Number of voids in path
    bool isHighRisk = false;          // True if risk factors present
    bool hasWallBlock = false;        // Strong wall blocking path
    bool hasVoidAcceleration = false; // Void may cause price acceleration
    bool valid = false;
};

// ============================================================================
// SPATIAL LIQUIDITY PROFILE - Trade Gating
// ============================================================================
// Determines if trades should be blocked/adjusted based on spatial profile.

struct SpatialTradeGating {
    bool longBlocked = false;         // Block long entries
    double longRiskMultiplier = 1.0;  // Risk multiplier for longs (1.0 = normal)
    bool shortBlocked = false;        // Block short entries
    double shortRiskMultiplier = 1.0; // Risk multiplier for shorts
    bool blockedByBidWall = false;    // Long blocked by bid wall (selling into wall)
    bool blockedByAskWall = false;    // Short blocked by ask wall (buying into wall)
    bool acceleratedByBidVoid = false;   // Void below may cause downward acceleration
    bool acceleratedByAskVoid = false;   // Void above may cause upward acceleration
    bool valid = false;

    bool AnyBlocked() const { return longBlocked || shortBlocked; }
    bool HasAcceleration() const { return acceleratedByBidVoid || acceleratedByAskVoid; }
};

// ============================================================================
// SPATIAL LIQUIDITY PROFILE - Main Result Struct
// ============================================================================
// Complete spatial analysis of the order book around current price.

struct SpatialLiquidityProfile {
    // Level data (raw input converted to LevelInfo)
    std::vector<LevelInfo> bidLevels;
    std::vector<LevelInfo> askLevels;
    double referencePrice = 0.0;
    double tickSize = 0.0;

    // Statistical basis for wall/void detection
    double meanDepth = 0.0;           // Mean depth across all levels
    double stddevDepth = 0.0;         // Stddev of depth across all levels
    bool statsValid = false;          // True if enough data for statistics

    // Walls (significant depth concentrations)
    std::vector<WallInfo> walls;
    int bidWallCount = 0;             // Walls on bid side (support)
    int askWallCount = 0;             // Walls on ask side (resistance)
    double nearestBidWallTicks = -1.0;   // Distance to nearest bid wall (-1 = none)
    double nearestAskWallTicks = -1.0;   // Distance to nearest ask wall (-1 = none)

    // Voids (thin liquidity areas)
    std::vector<VoidInfo> voids;
    int bidVoidCount = 0;             // Voids below price
    int askVoidCount = 0;             // Voids above price
    double nearestBidVoidTicks = -1.0;   // Distance to nearest bid void (-1 = none)
    double nearestAskVoidTicks = -1.0;   // Distance to nearest ask void (-1 = none)

    // Directional analysis (OBI + POLR)
    DirectionalResistance direction;

    // Execution risk estimates
    ExecutionRiskEstimate riskUp;     // Risk for upward move
    ExecutionRiskEstimate riskDown;   // Risk for downward move

    // Trade gating
    SpatialTradeGating gating;

    // Validity
    bool valid = false;
    SpatialErrorReason errorReason = SpatialErrorReason::NONE;
    int errorBar = -1;                // Bar index when computed
    bool wallBaselineReady = false;   // True if baseline has enough data for sigma

    // Helpers
    bool IsReady() const { return valid; }
    bool HasWalls() const { return !walls.empty(); }
    bool HasVoids() const { return !voids.empty(); }
    bool HasBidWall() const { return bidWallCount > 0; }
    bool HasAskWall() const { return askWallCount > 0; }
    bool HasBidVoid() const { return bidVoidCount > 0; }
    bool HasAskVoid() const { return askVoidCount > 0; }

    // Get path-of-least-resistance direction
    // Returns: +1 = easier up, -1 = easier down, 0 = balanced
    int GetPOLRDirection() const {
        if (!direction.valid) return 0;
        const double bias = direction.GetDirectionalBias();
        if (bias > 0.15) return 1;   // More bid depth = support = easier up
        if (bias < -0.15) return -1; // More ask depth = resistance = easier down
        return 0;
    }

    // Get human-readable POLR string
    const char* GetPOLRString() const {
        const int dir = GetPOLRDirection();
        if (dir > 0) return "UP";
        if (dir < 0) return "DOWN";
        return "BAL";
    }
};

// ============================================================================
// SPATIAL LIQUIDITY CONFIG
// ============================================================================

struct SpatialConfig {
    int analysisRangeTicks = 10;      // Range to analyze (ticks from reference)
    int riskTargetTicks = 4;          // Target for risk estimation (ES: 4 ticks = 1 point)
    double wallSigmaThreshold = 2.5;  // Sigma score for wall detection
    double voidDepthRatio = 0.10;     // Depth < 10% of mean = void
    double thinDepthRatio = 0.25;     // Depth < 25% of mean = thin
    size_t minLevelsForStats = 3;     // Minimum levels per side for statistics
    double polrBiasThreshold = 0.15;  // OBI threshold for directional bias
    double highRiskSlippage = 2.0;    // Slippage threshold for high risk flag
    double wallBlockDistance = 3;     // Wall within N ticks blocks trade
};

// ============================================================================
// STRESS RESULT
// ============================================================================

struct StressResult {
    double aggressiveBuy = 0.0;   // Volume lifting offers (AskVolume)
    double aggressiveSell = 0.0;  // Volume hitting bids (BidVolume)
    double aggressiveTotal = 0.0; // Total aggressive volume
    double stress = 0.0;          // AggressiveTotal / (DepthMassTotal + epsilon)
    bool valid = false;
};

// ============================================================================
// RESILIENCE RESULT
// ============================================================================

struct ResilienceResult {
    double depthChange = 0.0;     // Current depth - previous depth
    double refillRaw = 0.0;       // max(0, depthChange) - only positive refills
    double refillRate = 0.0;      // refillRaw / barDurationSec
    bool valid = false;
};

// ============================================================================
// LIQUIDITY 3-COMPONENT RESULT (Per-Bar Output)
// ============================================================================

struct Liq3Result {
    // Raw components
    DepthMassResult depth;
    StressResult stress;
    ResilienceResult resilience;

    // Percentile ranks (empirical, 0-100)
    double depthRank = 0.0;       // Percentile of depthMass vs baseline
    double stressRank = 0.0;      // Percentile of stress vs baseline (higher = worse)
    double resilienceRank = 0.0;  // Percentile of refillRate vs baseline

    // Validity flags per component
    bool depthRankValid = false;
    bool stressRankValid = false;
    bool resilienceRankValid = false;

    // Composite output
    double liq = 0.0;             // Final LIQ scalar [0, 1]
    LiquidityState liqState = LiquidityState::LIQ_NOT_READY;
    bool liqValid = false;

    // Error tracking (No Silent Failures)
    LiquidityErrorReason errorReason = LiquidityErrorReason::NONE;
    int errorBar = -1;            // Bar index when error occurred (for diagnostics)

    // Diagnostic: which baselines are missing
    bool depthBaselineReady = false;
    bool stressBaselineReady = false;
    bool resilienceBaselineReady = false;

    // Historical best bid/ask (for Execution Friction - temporal coherence)
    // Extracted from c_ACSILDepthBars at closed bar
    double histBestBid = 0.0;     // Best bid price at bar close
    double histBestAsk = 0.0;     // Best ask price at bar close
    double histSpreadTicks = 0.0; // Spread in ticks at bar close
    bool histBidAskValid = false; // True if historical bid/ask available

    // Peak liquidity (from GetMax* functions - maximum depth during bar)
    double peakDepthMass = 0.0;      // Peak total depth during bar
    double peakBidMass = 0.0;        // Peak bid depth during bar
    double peakAskMass = 0.0;        // Peak ask depth during bar
    bool peakValid = false;          // True if peak data was computed

    // Liquidity consumed during bar (peak - ending)
    // High consumed = depth was absorbed/hit during the bar
    double consumedDepthMass = 0.0;  // Total consumed
    double consumedBidMass = 0.0;    // Consumed on bid side
    double consumedAskMass = 0.0;    // Consumed on ask side

    // Direct Stack/Pull API (sc.GetBidMarketDepthStackPullSum)
    // Replaces study input dependency for real-time pulling/stacking data
    double directBidStackPull = 0.0; // From sc.GetBidMarketDepthStackPullSum()
    double directAskStackPull = 0.0; // From sc.GetAskMarketDepthStackPullSum()
    bool directStackPullValid = false;

    // ========================================================================
    // KYLE'S TIGHTNESS COMPONENT (Spread)
    // ========================================================================
    // Kyle (1985) defines liquidity via 3 dimensions: Depth, Resiliency, Tightness
    // Tightness = bid-ask spread. Narrower spread = lower execution cost = higher liquidity.
    double spreadRank = 0.0;        // Percentile of spread vs baseline (0-1, higher = wider = worse)
    bool spreadRankValid = false;
    bool spreadBaselineReady = false;

    // ========================================================================
    // ORDER FLOW TOXICITY PROXY (VPIN-lite)
    // ========================================================================
    // Detects asymmetric liquidity consumption indicating informed order flow.
    // Based on VPIN (Easley, López de Prado, O'Hara 2012) concept.
    // Formula: |consumedBid - consumedAsk| / (consumedBid + consumedAsk + ε)
    // Range: [0, 1]. High = one side consuming disproportionately = adverse selection risk.
    double toxicityProxy = 0.0;     // Asymmetric consumption ratio
    bool toxicityValid = false;     // True if consumed data available

    // ========================================================================
    // FOOTPRINT DIAGONAL DELTA (from Numbers Bars SG43/SG44)
    // ========================================================================
    // SSOT for diagonal delta - moved from EffortSnapshot (Dec 2024)
    // Diagonal delta compares bid@N vs ask@N+1, detecting footprint imbalances.
    // Positive = buyers lifting offers aggressively, Negative = sellers hitting bids.
    double diagonalPosDeltaSum = 0.0;  // SG43: Positive diagonal delta sum
    double diagonalNegDeltaSum = 0.0;  // SG44: Negative diagonal delta sum
    double diagonalNetDelta = 0.0;     // Derived: pos - neg (+ = bullish imbalance)
    bool diagonalDeltaValid = false;

    // ========================================================================
    // AVERAGE TRADE SIZE (from Numbers Bars SG51/SG52)
    // ========================================================================
    // SSOT for avg trade size - moved from EffortSnapshot (Dec 2024)
    // Large trades = institutional, Small trades = retail/HFT.
    double avgBidTradeSize = 0.0;      // SG51: Average trade size at bid
    double avgAskTradeSize = 0.0;      // SG52: Average trade size at ask
    double avgTradeSizeRatio = 0.0;    // Derived: ask/bid (>1 = larger ask trades = institutional buying)
    bool avgTradeSizeValid = false;

    // ========================================================================
    // SPATIAL LIQUIDITY PROFILE (Summary Fields)
    // ========================================================================
    // Summary fields from SpatialLiquidityProfile for downstream consumers.
    // Full profile available via LiquidityEngine::ComputeSpatialProfile().
    // ========================================================================
    SpatialTradeGating spatialGating;     // Trade gating based on walls/voids
    double orderBookImbalance = 0.0;      // OBI: (bid - ask) / total, [-1, +1]
    int pathOfLeastResistance = 0;        // POLR: +1=up, -1=down, 0=balanced
    double nearestBidWallTicks = -1.0;    // Distance to nearest bid wall (-1 = none)
    double nearestAskWallTicks = -1.0;    // Distance to nearest ask wall (-1 = none)
    double nearestBidVoidTicks = -1.0;    // Distance to nearest bid void (-1 = none)
    double nearestAskVoidTicks = -1.0;    // Distance to nearest ask void (-1 = none)
    bool hasSpatialProfile = false;       // True if spatial profile was computed

    // Spatial profile helpers
    bool HasSpatialWalls() const { return nearestBidWallTicks >= 0 || nearestAskWallTicks >= 0; }
    bool HasSpatialVoids() const { return nearestBidVoidTicks >= 0 || nearestAskVoidTicks >= 0; }
    bool IsSpatialBlocked() const { return hasSpatialProfile && spatialGating.AnyBlocked(); }

    // Helper: Check if this is a warmup state (not a true error)
    bool IsWarmup() const {
        return errorReason == LiquidityErrorReason::WARMUP_DEPTH ||
               errorReason == LiquidityErrorReason::WARMUP_STRESS ||
               errorReason == LiquidityErrorReason::WARMUP_RESILIENCE ||
               errorReason == LiquidityErrorReason::WARMUP_MULTIPLE;
    }

    // Helper: Check if this is a hard error (not warmup)
    bool IsHardError() const {
        return !liqValid && !IsWarmup() && errorReason != LiquidityErrorReason::NONE;
    }

    // ========================================================================
    // V1: STALENESS DETECTION
    // ========================================================================
    // DOM data can become stale if market data feed lags or disconnects.
    // Stale data = invalid for execution decisions (triggers HARD_BLOCK).
    //
    // depthAgeMs: Age of DOM data in milliseconds (current time - DOM timestamp)
    // depthStale: True if depthAgeMs > staleThresholdMs (default 2000ms)
    //
    // If stale: liqValid = false, errorReason = ERR_DEPTH_STALE, action = HARD_BLOCK
    // ========================================================================
    int depthAgeMs = -1;              // -1 = not provided, >= 0 = valid age
    bool depthStale = false;          // True if DOM data is stale

    // ========================================================================
    // V1: UNIFIED EXECUTION FRICTION SCORE
    // ========================================================================
    // Single composite metric derived from existing components:
    //   friction = w1*(1-depthRank) + w2*stressRank + w3*(1-resilienceRank) + w4*spreadRank
    //
    // Bounded [0, 1]. Higher = worse execution conditions.
    //   0.0 = perfect liquidity (deep, calm, resilient, tight)
    //   1.0 = worst case (shallow, stressed, fragile, wide)
    //
    // Invalid if ANY required subcomponent (depth, stress, resilience) is invalid.
    // Spread is optional - if unavailable, assumes neutral (0.5 rank).
    // ========================================================================
    double executionFriction = 0.0;   // [0, 1] composite friction score
    bool frictionValid = false;       // True if all required components valid

    // ========================================================================
    // V1: ACTION GUIDANCE FOR CONSUMERS
    // ========================================================================
    // Recommended action based on liquidity conditions.
    // The engine RECOMMENDS, consumers DECIDE whether to apply.
    //
    // Derivation (deterministic):
    //   HARD_BLOCK if: depthStale OR liqState=VOID OR friction >= hardBlockThreshold
    //   WIDEN_TOLERANCE if: liqState=THIN OR friction >= widenThreshold
    //   PROCEED otherwise (NORMAL/THICK and low friction)
    // ========================================================================
    LiquidityAction recommendedAction = LiquidityAction::HARD_BLOCK;  // Fail-safe default

    // Helper: Is data fresh enough for execution?
    bool IsDataFresh() const {
        return !depthStale && depthAgeMs >= 0;
    }

    // Helper: Can we proceed with execution?
    bool CanProceed() const {
        return recommendedAction == LiquidityAction::PROCEED;
    }

    // Helper: Should we block execution?
    bool ShouldBlock() const {
        return recommendedAction == LiquidityAction::HARD_BLOCK;
    }
};

// ============================================================================
// ROLLING BASELINE (Empirical Percentile)
// ============================================================================

class EmpiricalBaseline {
public:
    void Reset(int window = 300) {
        values_.clear();
        window_ = window;
    }

    void Push(double val) {
        values_.push_back(val);
        if (static_cast<int>(values_.size()) > window_) {
            values_.pop_front();
        }
    }

    size_t Size() const { return values_.size(); }

    bool IsReady(size_t minSamples) const {
        return values_.size() >= minSamples;
    }

    // ========================================================================
    // PERCENTILE COMPUTATION (No Silent Fallbacks)
    // ========================================================================
    // INVARIANT: Caller MUST check IsReady() before calling PercentileRank().
    // If called on empty baseline, returns SENTINEL (-1.0) to detect bug.
    // Valid return range: [0.0, 100.0]. Sentinel: -1.0.
    // ========================================================================

    // Sentinel value indicating invalid call (empty baseline)
    static constexpr double PERCENTILE_INVALID_SENTINEL = -1.0;

    // Empirical percentile rank: what fraction of stored values are < val
    // Returns [0, 100] on success, PERCENTILE_INVALID_SENTINEL (-1.0) on empty baseline
    // CALLER MUST check IsReady() first - this is a bug detector, not a fallback
    double PercentileRank(double val) const {
        if (values_.empty()) {
            // BUG: Caller should have checked IsReady() first
            // Return sentinel that will be detected and flagged as ERR_PERCENTILE_EMPTY
            return PERCENTILE_INVALID_SENTINEL;
        }

        int countBelow = 0;
        for (double v : values_) {
            if (v < val) ++countBelow;
        }
        return (static_cast<double>(countBelow) / static_cast<double>(values_.size())) * 100.0;
    }

    // Check if a percentile result is the sentinel (indicates bug)
    static bool IsPercentileSentinel(double pctile) {
        return pctile < 0.0;  // Sentinel is -1.0, valid range is [0, 100]
    }

    // Try variant that checks readiness - PREFERRED API
    PercentileResult TryPercentileRank(double val, size_t minSamples) const {
        if (!IsReady(minSamples)) {
            return PercentileResult::Invalid();
        }
        const double result = PercentileRank(val);
        if (IsPercentileSentinel(result)) {
            // Should not happen if IsReady() is correct, but guard anyway
            return PercentileResult::Invalid();
        }
        return PercentileResult::Valid(result);
    }

private:
    std::deque<double> values_;
    int window_ = 300;
};

// ============================================================================
// LIQUIDITY ENGINE
// ============================================================================

class LiquidityEngine {
public:
    LiquidityConfig config;

    // ========================================================================
    // PHASE-AWARE BASELINE ARCHITECTURE (Jan 2025 SSOT Refactor)
    // ========================================================================
    // SSOT for depth and spread is DOMWarmup (phase-bucketed).
    // Stress and resilience remain local (unique to LiquidityEngine).
    //
    // Why phase-aware? GLOBEX vs RTH are different beasts. RTH_AM vs RTH_PM
    // have different liquidity profiles. Comparing against same-phase history
    // gives more accurate percentile rankings.
    // ========================================================================

    // Phase-aware baseline source (SSOT for depth and spread)
    // Set via SetDOMWarmup() at initialization. If null, uses local fallback.
    DOMWarmup* domWarmup = nullptr;
    SessionPhase currentPhase = SessionPhase::UNKNOWN;

    // Local baselines (unique to LiquidityEngine - not in DOMWarmup)
    EmpiricalBaseline stressBaseline;     // Aggressive demand / depth ratio
    EmpiricalBaseline resilienceBaseline; // Kyle's Resiliency (refill rate)

    // DEPRECATED: Local depth/spread baselines (fallback only if DOMWarmup not set)
    // These are kept for backward compatibility but SSOT is DOMWarmup.
    EmpiricalBaseline depthBaselineFallback;   // Use DOMWarmup.depthMassCore instead
    EmpiricalBaseline spreadBaselineFallback;  // Use DOMWarmup.spreadTicks instead

    // Previous bar state (for resilience calculation)
    double prevDepthMassTotal = 0.0;
    bool hasPrevDepth = false;

    // Set the phase-aware baseline source (call at study init)
    void SetDOMWarmup(DOMWarmup* warmup) {
        domWarmup = warmup;
    }

    // Set current phase (call each bar before Compute)
    void SetPhase(SessionPhase phase) {
        currentPhase = phase;
    }

    // Check if phase-aware baselines are available
    // Only UNKNOWN and MAINTENANCE are non-tradeable (no baseline bucket)
    bool HasPhaseAwareBaselines() const {
        return domWarmup != nullptr &&
               currentPhase != SessionPhase::UNKNOWN &&
               currentPhase != SessionPhase::MAINTENANCE;
    }

    void Reset() {
        stressBaseline.Reset(config.baselineWindow);
        resilienceBaseline.Reset(config.baselineWindow);
        depthBaselineFallback.Reset(config.baselineWindow);
        spreadBaselineFallback.Reset(config.baselineWindow);
        prevDepthMassTotal = 0.0;
        hasPrevDepth = false;
        currentPhase = SessionPhase::UNKNOWN;
        // Note: domWarmup pointer is NOT reset - it's owned by StudyState
    }

    // ========================================================================
    // COMPONENT 1: DEPTH MASS (Distance-Weighted, Band-Limited)
    // ========================================================================
    //
    // Formula: DepthMass = Sum[ V(d) / (1 + d) ] for d in [0, Dmax]
    // Where d = distance from reference price in ticks
    //
    // Must be called with DOM level data from Sierra Chart:
    //   sc.GetBidMarketDepthEntryAtLevel(level, price, volume)
    //   sc.GetAskMarketDepthEntryAtLevel(level, price, volume)
    //
    template<typename GetBidLevel, typename GetAskLevel>
    DepthMassResult ComputeDepthMass(
        double referencePrice,
        double tickSize,
        int maxLevels,
        GetBidLevel getBidLevel,  // Lambda: (int level) -> {price, volume, valid}
        GetAskLevel getAskLevel   // Lambda: (int level) -> {price, volume, valid}
    ) const {
        DepthMassResult result;

        if (tickSize <= 0.0 || referencePrice <= 0.0) {
            return result;
        }

        const int dmax = config.dmaxTicks;
        const int levels = (std::min)(maxLevels, config.maxDomLevels);

        // Accumulate bid side
        for (int i = 0; i < levels; ++i) {
            double price = 0.0, volume = 0.0;
            bool valid = getBidLevel(i, price, volume);

            if (!valid || price <= 0.0 || volume <= 0.0) continue;

            // Distance in ticks (bid is below reference)
            const double distTicks = (referencePrice - price) / tickSize;

            // Only include if within Dmax
            if (distTicks >= 0.0 && distTicks <= static_cast<double>(dmax)) {
                const double weight = 1.0 / (1.0 + distTicks);
                result.bidMass += volume * weight;
                ++result.bidLevels;
            }
        }

        // Accumulate ask side
        for (int i = 0; i < levels; ++i) {
            double price = 0.0, volume = 0.0;
            bool valid = getAskLevel(i, price, volume);

            if (!valid || price <= 0.0 || volume <= 0.0) continue;

            // Distance in ticks (ask is above reference)
            const double distTicks = (price - referencePrice) / tickSize;

            // Only include if within Dmax
            if (distTicks >= 0.0 && distTicks <= static_cast<double>(dmax)) {
                const double weight = 1.0 / (1.0 + distTicks);
                result.askMass += volume * weight;
                ++result.askLevels;
            }
        }

        result.totalMass = result.bidMass + result.askMass;

        // Imbalance: (bid - ask) / (bid + ask)
        if (result.totalMass > 0.0) {
            result.imbalance = (result.bidMass - result.askMass) / result.totalMass;
        }

        result.valid = (result.bidLevels > 0 || result.askLevels > 0);
        return result;
    }

    // ========================================================================
    // COMPONENT 2: STRESS (Aggressive Demand / Depth)
    // ========================================================================
    //
    // Formula: Stress = AggressiveTotal / (DepthMassTotal + epsilon)
    //
    // Where:
    //   AggressiveTotal = AskVolume + BidVolume (market orders)
    //   AskVolume = buys lifting offers
    //   BidVolume = sells hitting bids
    //
    StressResult ComputeStress(
        double askVolume,         // Aggressive buys (from sc.AskVolume)
        double bidVolume,         // Aggressive sells (from sc.BidVolume)
        double depthMassTotal     // From DepthMassResult.totalMass
    ) const {
        StressResult result;

        result.aggressiveBuy = (std::max)(0.0, askVolume);
        result.aggressiveSell = (std::max)(0.0, bidVolume);
        result.aggressiveTotal = result.aggressiveBuy + result.aggressiveSell;

        // Stress = aggressive demand / available depth
        result.stress = result.aggressiveTotal / (depthMassTotal + config.epsilon);

        result.valid = true;
        return result;
    }

    // ========================================================================
    // COMPONENT 3: RESILIENCE (Bar-to-Bar Refill Proxy)
    // ========================================================================
    //
    // Formula: ResilienceRaw = max(0, DepthMass(t) - DepthMass(t-1))
    //          RefillRate = ResilienceRaw / barDurationSec
    //
    // Interpretation: After a hit, does depth recover quickly?
    //
    ResilienceResult ComputeResilience(
        double currentDepthMass,
        double barDurationSec
    ) {
        ResilienceResult result;

        if (!hasPrevDepth) {
            // First bar - no previous to compare
            prevDepthMassTotal = currentDepthMass;
            hasPrevDepth = true;
            result.valid = false;
            return result;
        }

        result.depthChange = currentDepthMass - prevDepthMassTotal;
        result.refillRaw = (std::max)(0.0, result.depthChange);

        // Normalize by time
        if (barDurationSec > 0.0) {
            result.refillRate = result.refillRaw / barDurationSec;
        }

        // Update previous for next bar
        prevDepthMassTotal = currentDepthMass;

        result.valid = true;
        return result;
    }

    // ========================================================================
    // FULL COMPUTATION: Compute All Components + Composite LIQ
    // ========================================================================
    //
    // Call this once per bar with:
    //   - DOM level accessors
    //   - Reference price (last trade or mid)
    //   - Aggressive volumes
    //   - Bar duration
    //   - Spread in ticks (for Kyle's Tightness component)
    //   - Consumed bid/ask mass (for toxicity proxy, optional)
    //
    // Returns full Liq3Result with validity flags and error states.
    //
    template<typename GetBidLevel, typename GetAskLevel>
    Liq3Result Compute(
        double referencePrice,
        double tickSize,
        int maxLevels,
        GetBidLevel getBidLevel,
        GetAskLevel getAskLevel,
        double askVolume,
        double bidVolume,
        double barDurationSec,
        double spreadTicks = -1.0,      // Kyle's Tightness: bid-ask spread in ticks (-1 = not provided)
        double consumedBidMass = -1.0,  // For toxicity: consumed bid liquidity (-1 = not provided)
        double consumedAskMass = -1.0,  // For toxicity: consumed ask liquidity (-1 = not provided)
        // V1: Staleness detection parameters
        int64_t currentTimeMs = -1,     // Current time in milliseconds (-1 = not provided, skip staleness check)
        int64_t domTimestampMs = -1     // DOM data timestamp in milliseconds (-1 = not provided)
    ) {
        Liq3Result snap;

        // --------------------------------------------------------------------
        // V1 Step 0: Staleness Detection (Hard Validity Gate)
        // --------------------------------------------------------------------
        // If DOM timestamp is provided, compute age and check for staleness.
        // Stale data = HARD_BLOCK, no further computation (fail-safe).
        if (currentTimeMs >= 0 && domTimestampMs >= 0) {
            const int64_t ageMs = currentTimeMs - domTimestampMs;
            snap.depthAgeMs = static_cast<int>(ageMs);

            if (ageMs > config.staleThresholdMs) {
                snap.depthStale = true;
                snap.errorReason = LiquidityErrorReason::ERR_DEPTH_STALE;
                snap.liqState = LiquidityState::LIQ_NOT_READY;
                snap.recommendedAction = LiquidityAction::HARD_BLOCK;
                return snap;  // Early return - stale data is unusable
            }
        }
        // If timestamps not provided, depthAgeMs stays -1 (unknown, not stale)

        // --------------------------------------------------------------------
        // Step 1: Compute DepthMass
        // --------------------------------------------------------------------
        snap.depth = ComputeDepthMass(referencePrice, tickSize, maxLevels,
                                       getBidLevel, getAskLevel);

        // Check for NO_DOM_LEVELS error (F6)
        if (!snap.depth.valid) {
            snap.errorReason = LiquidityErrorReason::ERR_NO_DOM_LEVELS;
            snap.liqState = LiquidityState::LIQ_NOT_READY;
            return snap;  // Early return - cannot proceed without depth
        }

        // --------------------------------------------------------------------
        // Step 2: Compute Stress
        // --------------------------------------------------------------------
        snap.stress = ComputeStress(askVolume, bidVolume, snap.depth.totalMass);

        // --------------------------------------------------------------------
        // Step 3: Compute Resilience
        // --------------------------------------------------------------------
        snap.resilience = ComputeResilience(snap.depth.totalMass, barDurationSec);

        // --------------------------------------------------------------------
        // Step 4: Push to baselines
        // SSOT: Depth and spread go to DOMWarmup (phase-aware).
        //       Stress and resilience stay local (unique to LiquidityEngine).
        // --------------------------------------------------------------------

        // Stress and resilience: always push to local baselines (unique)
        if (snap.stress.valid) {
            stressBaseline.Push(snap.stress.stress);
        }
        if (snap.resilience.valid) {
            resilienceBaseline.Push(snap.resilience.refillRate);
        }

        // Depth and spread: push to DOMWarmup if available (SSOT), else fallback
        if (snap.depth.valid) {
            if (HasPhaseAwareBaselines()) {
                domWarmup->Get(currentPhase).depthMassCore.push(snap.depth.totalMass);
            } else {
                depthBaselineFallback.Push(snap.depth.totalMass);
            }
        }
        if (spreadTicks >= 0.0) {
            snap.histSpreadTicks = spreadTicks;  // Store for diagnostics
            if (HasPhaseAwareBaselines()) {
                domWarmup->Get(currentPhase).spreadTicks.push(spreadTicks);
            } else {
                spreadBaselineFallback.Push(spreadTicks);
            }
        }

        // --------------------------------------------------------------------
        // Step 4b: Compute Order Flow Toxicity Proxy (VPIN-lite)
        // --------------------------------------------------------------------
        // Formula: |consumedBid - consumedAsk| / (consumedBid + consumedAsk + ε)
        // High asymmetry = one side consuming disproportionately = informed flow
        if (consumedBidMass >= 0.0 && consumedAskMass >= 0.0) {
            const double consumedTotal = consumedBidMass + consumedAskMass;
            if (consumedTotal > config.epsilon) {
                snap.toxicityProxy = std::abs(consumedBidMass - consumedAskMass) / consumedTotal;
                snap.toxicityValid = true;
            }
            // Store consumed values in result
            snap.consumedBidMass = consumedBidMass;
            snap.consumedAskMass = consumedAskMass;
            snap.consumedDepthMass = consumedTotal;
        }

        // --------------------------------------------------------------------
        // Step 5: Check baseline readiness (phase-aware for depth/spread)
        // --------------------------------------------------------------------
        // Depth: use DOMWarmup if available (SSOT), else fallback
        if (HasPhaseAwareBaselines()) {
            const auto& bucket = domWarmup->Get(currentPhase);
            snap.depthBaselineReady = bucket.depthMassCore.size() >= config.baselineMinSamples;
            snap.spreadBaselineReady = bucket.spreadTicks.size() >= config.baselineMinSamples;
        } else {
            snap.depthBaselineReady = depthBaselineFallback.IsReady(config.baselineMinSamples);
            snap.spreadBaselineReady = spreadBaselineFallback.IsReady(config.baselineMinSamples);
        }
        // Stress and resilience: always local
        snap.stressBaselineReady = stressBaseline.IsReady(config.baselineMinSamples);
        snap.resilienceBaselineReady = resilienceBaseline.IsReady(config.baselineMinSamples);

        // --------------------------------------------------------------------
        // Step 6: Compute percentile ranks (phase-aware for depth/spread)
        // --------------------------------------------------------------------

        // DEPTH: Query DOMWarmup (SSOT) or fallback
        if (snap.depthBaselineReady && snap.depth.valid) {
            double rawRank = 0.0;
            if (HasPhaseAwareBaselines()) {
                // Phase-aware percentile from DOMWarmup
                rawRank = domWarmup->Get(currentPhase).depthMassCore.percentile(snap.depth.totalMass);
            } else {
                rawRank = depthBaselineFallback.PercentileRank(snap.depth.totalMass);
                if (EmpiricalBaseline::IsPercentileSentinel(rawRank)) {
                    snap.errorReason = LiquidityErrorReason::ERR_PERCENTILE_EMPTY;
                    snap.liqState = LiquidityState::LIQ_NOT_READY;
                    return snap;
                }
            }
            snap.depthRank = rawRank / 100.0;
            snap.depthRankValid = true;
        }

        // STRESS: Always local baseline (unique to LiquidityEngine)
        if (snap.stressBaselineReady && snap.stress.valid) {
            const double rawRank = stressBaseline.PercentileRank(snap.stress.stress);
            if (EmpiricalBaseline::IsPercentileSentinel(rawRank)) {
                snap.errorReason = LiquidityErrorReason::ERR_PERCENTILE_EMPTY;
                snap.liqState = LiquidityState::LIQ_NOT_READY;
                return snap;
            }
            snap.stressRank = rawRank / 100.0;
            snap.stressRankValid = true;
        }

        // RESILIENCE: Always local baseline (unique to LiquidityEngine)
        if (snap.resilienceBaselineReady && snap.resilience.valid) {
            const double rawRank = resilienceBaseline.PercentileRank(snap.resilience.refillRate);
            if (EmpiricalBaseline::IsPercentileSentinel(rawRank)) {
                snap.errorReason = LiquidityErrorReason::ERR_PERCENTILE_EMPTY;
                snap.liqState = LiquidityState::LIQ_NOT_READY;
                return snap;
            }
            snap.resilienceRank = rawRank / 100.0;
            snap.resilienceRankValid = true;
        }

        // SPREAD (Kyle's Tightness): Query DOMWarmup (SSOT) or fallback
        if (snap.spreadBaselineReady && spreadTicks >= 0.0) {
            double rawRank = 0.0;
            if (HasPhaseAwareBaselines()) {
                // Phase-aware percentile from DOMWarmup
                rawRank = domWarmup->Get(currentPhase).spreadTicks.percentile(spreadTicks);
            } else {
                rawRank = spreadBaselineFallback.PercentileRank(spreadTicks);
                if (EmpiricalBaseline::IsPercentileSentinel(rawRank)) {
                    // Non-fatal for spread - just skip
                    rawRank = 50.0;  // Neutral fallback
                }
            }
            snap.spreadRank = rawRank / 100.0;
            snap.spreadRankValid = true;
        }

        // --------------------------------------------------------------------
        // Step 7: Compute composite LIQ with stress-weighted resilience + spread penalty
        // --------------------------------------------------------------------
        // Kyle's 3 Dimensions: Depth, Resiliency, Tightness (+ Stress as demand pressure)
        //
        // Resilience is a CONDITIONAL signal - only meaningful when stress tests it.
        // During quiet periods (low stress), resilience=0 means "untested", not "bad".
        //
        // Spread penalty: wider spread (high spreadRank) = higher execution cost = lower LIQ
        // Formula:
        //   resilienceContrib = stressRank * resilienceRank + (1 - stressRank) * 1.0
        //   spreadPenalty = 1.0 - (spreadWeight * spreadRank)  // 0.15 weight = 15% max penalty
        //   LIQ = depthRank * (1 - stressRank) * resilienceContrib * spreadPenalty
        //
        // When stress=0: resilienceContrib=1.0, LIQ=depthRank * spreadPenalty
        // When stress=1: resilienceContrib=resilienceRank, LIQ approaches 0 (overwhelmed)
        // When spread=wide (rank=1.0): 15% penalty applied
        // --------------------------------------------------------------------
        if (snap.depthRankValid && snap.stressRankValid && snap.resilienceRankValid) {
            // Stress-weighted resilience: only count resilience when stress tests it
            const double resilienceContrib =
                snap.stressRank * snap.resilienceRank + (1.0 - snap.stressRank) * 1.0;

            // Spread penalty: wider spread reduces liquidity (Kyle's Tightness)
            // If spread not available, no penalty (spreadPenalty = 1.0)
            double spreadPenalty = 1.0;
            if (snap.spreadRankValid) {
                spreadPenalty = 1.0 - (config.spreadWeight * snap.spreadRank);
                spreadPenalty = (std::max)(0.5, spreadPenalty);  // Floor at 50% penalty max
            }

            snap.liq = snap.depthRank * (1.0 - snap.stressRank) * resilienceContrib * spreadPenalty;
            snap.liq = (std::max)(0.0, (std::min)(1.0, snap.liq));
            snap.liqValid = true;
            snap.errorReason = LiquidityErrorReason::NONE;  // Success
        }

        // --------------------------------------------------------------------
        // Step 8: Classify LIQSTATE with safety overrides + set warmup errors
        // --------------------------------------------------------------------
        if (!snap.liqValid) {
            snap.liqState = LiquidityState::LIQ_NOT_READY;

            // Set specific warmup error reason if not already set
            if (snap.errorReason == LiquidityErrorReason::NONE) {
                const int notReadyCount =
                    (snap.depthBaselineReady ? 0 : 1) +
                    (snap.stressBaselineReady ? 0 : 1) +
                    (snap.resilienceBaselineReady ? 0 : 1);

                if (notReadyCount > 1) {
                    snap.errorReason = LiquidityErrorReason::WARMUP_MULTIPLE;
                } else if (!snap.depthBaselineReady) {
                    snap.errorReason = LiquidityErrorReason::WARMUP_DEPTH;
                } else if (!snap.stressBaselineReady) {
                    snap.errorReason = LiquidityErrorReason::WARMUP_STRESS;
                } else if (!snap.resilienceBaselineReady) {
                    snap.errorReason = LiquidityErrorReason::WARMUP_RESILIENCE;
                }
            }
        }
        else {
            // Safety override 1: DepthRank <= 0.10 -> force VOID
            if (snap.depthRank <= 0.10) {
                snap.liqState = LiquidityState::LIQ_VOID;
            }
            // Safety override 2: StressRank >= 0.90 -> force THIN
            else if (snap.stressRank >= 0.90) {
                snap.liqState = LiquidityState::LIQ_THIN;
            }
            // Normal classification by LIQ
            else if (snap.liq <= 0.10) {
                snap.liqState = LiquidityState::LIQ_VOID;
            }
            else if (snap.liq <= 0.25) {
                snap.liqState = LiquidityState::LIQ_THIN;
            }
            else if (snap.liq >= 0.75) {
                snap.liqState = LiquidityState::LIQ_THICK;
            }
            else {
                snap.liqState = LiquidityState::LIQ_NORMAL;
            }
        }

        // --------------------------------------------------------------------
        // V1 Step 9: Compute Execution Friction
        // --------------------------------------------------------------------
        // friction = w1*(1-depthRank) + w2*stressRank + w3*(1-resilienceRank) + w4*spreadRank
        // Higher friction = worse execution conditions. Bounded [0, 1].
        // Invalid if ANY required component (depth, stress, resilience) is invalid.
        if (snap.depthRankValid && snap.stressRankValid && snap.resilienceRankValid) {
            // Spread is optional - use neutral 0.5 if not available
            const double effectiveSpreadRank = snap.spreadRankValid ? snap.spreadRank : 0.5;

            snap.executionFriction =
                config.frictionWeightDepth * (1.0 - snap.depthRank) +      // Lack of depth
                config.frictionWeightStress * snap.stressRank +            // High stress
                config.frictionWeightResilience * (1.0 - snap.resilienceRank) +  // Low resilience
                config.frictionWeightSpread * effectiveSpreadRank;         // Wide spread

            // Clamp to [0, 1] (should already be bounded by construction)
            snap.executionFriction = (std::max)(0.0, (std::min)(1.0, snap.executionFriction));
            snap.frictionValid = true;
        }

        // --------------------------------------------------------------------
        // V1 Step 10: Derive Recommended Action
        // --------------------------------------------------------------------
        // Deterministic mapping from LIQSTATE + friction + staleness:
        //   HARD_BLOCK if: depthStale OR liqState=VOID OR friction >= hardBlockThreshold
        //   WIDEN_TOLERANCE if: liqState=THIN OR friction >= widenThreshold
        //   PROCEED otherwise
        if (snap.depthStale) {
            // Already set in staleness check, but ensure consistency
            snap.recommendedAction = LiquidityAction::HARD_BLOCK;
        }
        else if (snap.liqState == LiquidityState::LIQ_NOT_READY) {
            // During warmup, block execution (no valid data)
            snap.recommendedAction = LiquidityAction::HARD_BLOCK;
        }
        else if (snap.liqState == LiquidityState::LIQ_VOID) {
            // VOID = unsafe, always block
            snap.recommendedAction = LiquidityAction::HARD_BLOCK;
        }
        else if (snap.frictionValid && snap.executionFriction >= config.hardBlockFrictionThreshold) {
            // Severe friction = block
            snap.recommendedAction = LiquidityAction::HARD_BLOCK;
        }
        else if (snap.liqState == LiquidityState::LIQ_THIN) {
            // THIN = proceed with caution
            snap.recommendedAction = LiquidityAction::WIDEN_TOLERANCE;
        }
        else if (snap.frictionValid && snap.executionFriction >= config.widenFrictionThreshold) {
            // Moderate friction = widen tolerance
            snap.recommendedAction = LiquidityAction::WIDEN_TOLERANCE;
        }
        else {
            // NORMAL or THICK with low friction = proceed
            snap.recommendedAction = LiquidityAction::PROCEED;
        }

        return snap;
    }

    // ========================================================================
    // DIAGNOSTIC: Get baseline sample counts
    // ========================================================================
    // Returns samples from phase-aware DOMWarmup if available, else fallback.
    void GetDiagnostics(size_t& depthSamples, size_t& stressSamples, size_t& resSamples, size_t& spreadSamples) const {
        if (HasPhaseAwareBaselines()) {
            const auto& bucket = domWarmup->Get(currentPhase);
            depthSamples = bucket.depthMassCore.size();
            spreadSamples = bucket.spreadTicks.size();
        } else {
            depthSamples = depthBaselineFallback.Size();
            spreadSamples = spreadBaselineFallback.Size();
        }
        stressSamples = stressBaseline.Size();
        resSamples = resilienceBaseline.Size();
    }

    // Legacy overload for backwards compatibility
    void GetDiagnostics(size_t& depthSamples, size_t& stressSamples, size_t& resSamples) const {
        if (HasPhaseAwareBaselines()) {
            depthSamples = domWarmup->Get(currentPhase).depthMassCore.size();
        } else {
            depthSamples = depthBaselineFallback.Size();
        }
        stressSamples = stressBaseline.Size();
        resSamples = resilienceBaseline.Size();
    }

    // ========================================================================
    // PRE-WARM: Populate baselines from historical data (eliminates cold-start)
    // ========================================================================
    // NOTE: For phase-aware architecture, depth/spread should be pre-warmed
    // via DOMWarmup directly (in PreWarmLiquidityBaselines), not here.
    // This function only handles stress/resilience (unique to LiquidityEngine).
    //
    // For backward compatibility, depth can still be pushed to fallback.
    // ========================================================================

    // Push stress and resilience to local baselines (phase-aware depth goes to DOMWarmup)
    // Returns true if at least one value was pushed
    bool PushHistoricalSample(double stress, double refillRate) {
        bool pushed = false;

        if (stress >= 0.0) {
            stressBaseline.Push(stress);
            pushed = true;
        }

        if (refillRate >= 0.0) {
            resilienceBaseline.Push(refillRate);
            pushed = true;
        }

        return pushed;
    }

    // Legacy overload - pushes depth to fallback (use DOMWarmup for phase-aware)
    bool PushHistoricalSample(double depthMass, double stress, double refillRate) {
        bool pushed = false;

        if (depthMass >= 0.0) {
            depthBaselineFallback.Push(depthMass);
            pushed = true;
        }

        if (stress >= 0.0) {
            stressBaseline.Push(stress);
            pushed = true;
        }

        if (refillRate >= 0.0) {
            resilienceBaseline.Push(refillRate);
            pushed = true;
        }

        return pushed;
    }

    // Compute depth mass from bid/ask level arrays (for pre-warm)
    // bidLevels/askLevels: vector of {price, volume} pairs, sorted by distance from ref
    DepthMassResult ComputeDepthMassFromLevels(
        double referencePrice,
        double tickSize,
        const std::vector<std::pair<double, double>>& bidLevels,
        const std::vector<std::pair<double, double>>& askLevels
    ) const {
        DepthMassResult result;

        if (tickSize <= 0.0 || referencePrice <= 0.0) {
            return result;
        }

        const int dmax = config.dmaxTicks;

        // Accumulate bid side
        for (const auto& level : bidLevels) {
            const double price = level.first;
            const double volume = level.second;
            if (price <= 0.0 || volume <= 0.0) continue;

            const double distTicks = (referencePrice - price) / tickSize;
            if (distTicks >= 0.0 && distTicks <= static_cast<double>(dmax)) {
                const double weight = 1.0 / (1.0 + distTicks);
                result.bidMass += volume * weight;
                ++result.bidLevels;
            }
        }

        // Accumulate ask side
        for (const auto& level : askLevels) {
            const double price = level.first;
            const double volume = level.second;
            if (price <= 0.0 || volume <= 0.0) continue;

            const double distTicks = (price - referencePrice) / tickSize;
            if (distTicks >= 0.0 && distTicks <= static_cast<double>(dmax)) {
                const double weight = 1.0 / (1.0 + distTicks);
                result.askMass += volume * weight;
                ++result.askLevels;
            }
        }

        result.totalMass = result.bidMass + result.askMass;

        if (result.totalMass > 0.0) {
            result.imbalance = (result.bidMass - result.askMass) / result.totalMass;
        }

        result.valid = (result.bidLevels > 0 || result.askLevels > 0);
        return result;
    }

    // ========================================================================
    // COMPUTE DEPTH MASS WITH PEAK (Last + Max Quantities)
    // ========================================================================
    // Computes both ending depth (GetLast*) and peak depth (GetMax*) in one pass.
    // Peak = maximum depth that existed during bar (available liquidity).
    // Consumed = Peak - Ending (liquidity that was taken during the bar).
    //
    // Template parameters:
    //   GetLastBid/Ask: Lambda returning end-of-bar quantity at level
    //   GetMaxBid/Ask: Lambda returning maximum quantity during bar at level
    //
    template<typename GetLastBid, typename GetLastAsk, typename GetMaxBid, typename GetMaxAsk>
    DepthMassResult ComputeDepthMassWithPeak(
        double referencePrice,
        double tickSize,
        int maxLevels,
        GetLastBid getLastBid,   // Lambda: (int level) -> {price, volume, valid}
        GetLastAsk getLastAsk,   // Lambda: (int level) -> {price, volume, valid}
        GetMaxBid getMaxBid,     // Lambda: (int level) -> {price, volume, valid}
        GetMaxAsk getMaxAsk      // Lambda: (int level) -> {price, volume, valid}
    ) const {
        DepthMassResult result;

        if (tickSize <= 0.0 || referencePrice <= 0.0) {
            return result;
        }

        const int dmax = config.dmaxTicks;
        const int levels = (std::min)(maxLevels, config.maxDomLevels);

        // Accumulate bid side (both last and max)
        for (int i = 0; i < levels; ++i) {
            double price = 0.0, lastVol = 0.0, maxVol = 0.0;
            bool lastValid = getLastBid(i, price, lastVol);

            if (!lastValid || price <= 0.0) continue;

            // Distance in ticks (bid is below reference)
            const double distTicks = (referencePrice - price) / tickSize;

            // Only include if within Dmax
            if (distTicks >= 0.0 && distTicks <= static_cast<double>(dmax)) {
                const double weight = 1.0 / (1.0 + distTicks);

                // End-of-bar depth
                if (lastVol > 0.0) {
                    result.bidMass += lastVol * weight;
                    ++result.bidLevels;
                }

                // Peak depth (max during bar)
                double maxPrice = 0.0;
                if (getMaxBid(i, maxPrice, maxVol) && maxVol > 0.0) {
                    result.peakBidMass += maxVol * weight;
                    result.peakValid = true;
                }
            }
        }

        // Accumulate ask side (both last and max)
        for (int i = 0; i < levels; ++i) {
            double price = 0.0, lastVol = 0.0, maxVol = 0.0;
            bool lastValid = getLastAsk(i, price, lastVol);

            if (!lastValid || price <= 0.0) continue;

            // Distance in ticks (ask is above reference)
            const double distTicks = (price - referencePrice) / tickSize;

            // Only include if within Dmax
            if (distTicks >= 0.0 && distTicks <= static_cast<double>(dmax)) {
                const double weight = 1.0 / (1.0 + distTicks);

                // End-of-bar depth
                if (lastVol > 0.0) {
                    result.askMass += lastVol * weight;
                    ++result.askLevels;
                }

                // Peak depth (max during bar)
                double maxPrice = 0.0;
                if (getMaxAsk(i, maxPrice, maxVol) && maxVol > 0.0) {
                    result.peakAskMass += maxVol * weight;
                    result.peakValid = true;
                }
            }
        }

        // Compute totals
        result.totalMass = result.bidMass + result.askMass;
        result.peakTotalMass = result.peakBidMass + result.peakAskMass;

        // Imbalance from end-of-bar depth
        if (result.totalMass > 0.0) {
            result.imbalance = (result.bidMass - result.askMass) / result.totalMass;
        }

        // Compute consumed liquidity (peak - ending)
        // Positive = depth was consumed during bar
        result.consumedBidMass = (std::max)(0.0, result.peakBidMass - result.bidMass);
        result.consumedAskMass = (std::max)(0.0, result.peakAskMass - result.askMass);
        result.consumedTotalMass = result.consumedBidMass + result.consumedAskMass;

        result.valid = (result.bidLevels > 0 || result.askLevels > 0);
        return result;
    }

    // Pre-warm from a single historical bar's data
    // Returns true if data was valid and pushed
    // prevDepthMass: depth from previous bar (for resilience calc), or -1 if none
    // barDurationSec: bar duration in seconds
    // spreadTicks: bid-ask spread in ticks (-1 = not available)
    // Phase-aware pre-warm: pushes depth/spread to DOMWarmup, stress/resilience to local
    // phase: The session phase for this historical bar (for phase-aware baselines)
    bool PreWarmFromBar(
        double depthMass,
        double askVolume,
        double bidVolume,
        double prevDepthMass,
        double barDurationSec,
        SessionPhase phase,          // Phase for this historical bar
        double spreadTicks = -1.0
    ) {
        if (depthMass < 0.0) return false;

        // Push depth to DOMWarmup (SSOT) or fallback
        // Only UNKNOWN and MAINTENANCE are non-tradeable (no baseline bucket)
        const bool phaseValid = phase != SessionPhase::UNKNOWN &&
                                phase != SessionPhase::MAINTENANCE;
        if (domWarmup && phaseValid) {
            domWarmup->Get(phase).depthMassCore.push(depthMass);
        } else {
            depthBaselineFallback.Push(depthMass);
        }

        // Compute and push stress (always local - unique to LiquidityEngine)
        const double aggressiveTotal = (std::max)(0.0, askVolume) + (std::max)(0.0, bidVolume);
        const double stress = aggressiveTotal / (depthMass + config.epsilon);
        stressBaseline.Push(stress);

        // Compute and push resilience (always local - unique to LiquidityEngine)
        if (prevDepthMass >= 0.0 && barDurationSec > 0.0) {
            const double depthChange = depthMass - prevDepthMass;
            const double refillRaw = (std::max)(0.0, depthChange);
            const double refillRate = refillRaw / barDurationSec;
            resilienceBaseline.Push(refillRate);
        }

        // Push spread to DOMWarmup (SSOT) or fallback
        if (spreadTicks >= 0.0) {
            if (domWarmup && phaseValid) {
                domWarmup->Get(phase).spreadTicks.push(spreadTicks);
            } else {
                spreadBaselineFallback.Push(spreadTicks);
            }
        }

        return true;
    }

    // Legacy overload without phase (uses fallback baselines)
    bool PreWarmFromBar(
        double depthMass,
        double askVolume,
        double bidVolume,
        double prevDepthMass,
        double barDurationSec,
        double spreadTicks = -1.0
    ) {
        return PreWarmFromBar(depthMass, askVolume, bidVolume, prevDepthMass,
                              barDurationSec, SessionPhase::UNKNOWN, spreadTicks);
    }

    // Get pre-warm status
    struct PreWarmStatus {
        size_t depthSamples = 0;
        size_t stressSamples = 0;
        size_t resilienceSamples = 0;
        size_t spreadSamples = 0;
        bool depthReady = false;
        bool stressReady = false;
        bool resilienceReady = false;
        bool spreadReady = false;
        bool allReady = false;  // Core 3 ready (spread optional)
    };

    PreWarmStatus GetPreWarmStatus() const {
        PreWarmStatus status;

        // Depth and spread: check DOMWarmup if available (SSOT), else fallback
        if (HasPhaseAwareBaselines()) {
            const auto& bucket = domWarmup->Get(currentPhase);
            status.depthSamples = bucket.depthMassCore.size();
            status.spreadSamples = bucket.spreadTicks.size();
            status.depthReady = bucket.depthMassCore.size() >= config.baselineMinSamples;
            status.spreadReady = bucket.spreadTicks.size() >= config.baselineMinSamples;
        } else {
            status.depthSamples = depthBaselineFallback.Size();
            status.spreadSamples = spreadBaselineFallback.Size();
            status.depthReady = depthBaselineFallback.IsReady(config.baselineMinSamples);
            status.spreadReady = spreadBaselineFallback.IsReady(config.baselineMinSamples);
        }

        // Stress and resilience: always local baselines
        status.stressSamples = stressBaseline.Size();
        status.resilienceSamples = resilienceBaseline.Size();
        status.stressReady = stressBaseline.IsReady(config.baselineMinSamples);
        status.resilienceReady = resilienceBaseline.IsReady(config.baselineMinSamples);

        status.allReady = status.depthReady && status.stressReady && status.resilienceReady;
        // Note: spread is optional for allReady - core 3 components are sufficient
        return status;
    }

    // ========================================================================
    // SPATIAL LIQUIDITY PROFILE COMPUTATION
    // ========================================================================
    // Analyzes the spatial distribution of liquidity around price to detect:
    //   - Walls: Large depth concentrations (> 2.5σ) that block price movement
    //   - Voids: Thin areas (< 10% mean) where price can accelerate
    //   - OBI: Order book imbalance for directional bias
    //   - POLR: Path of least resistance
    //   - Execution risk: Kyle's Lambda-based slippage estimation
    //
    // Input:
    //   bidLevels/askLevels: Vector of {price, volume} pairs
    //   referencePrice: Current price (mid or last trade)
    //   tickSize: Instrument tick size
    //   analysisRangeTicks: How far from reference to analyze (default: 10)
    //   riskTargetTicks: Distance for risk estimation (default: 4)
    //
    // Returns:
    //   SpatialLiquidityProfile with walls, voids, OBI, POLR, risk, gating
    // ========================================================================

    SpatialConfig spatialConfig;  // Configurable thresholds

    SpatialLiquidityProfile ComputeSpatialProfile(
        const std::vector<std::pair<double, double>>& bidLevels,  // {price, volume}
        const std::vector<std::pair<double, double>>& askLevels,  // {price, volume}
        double referencePrice,
        double tickSize,
        int barIndex = -1  // For error tracking
    ) const {
        SpatialLiquidityProfile result;
        result.referencePrice = referencePrice;
        result.tickSize = tickSize;
        result.errorBar = barIndex;

        // --------------------------------------------------------------------
        // Step 1: Input validation
        // --------------------------------------------------------------------
        if (referencePrice <= 0.0) {
            result.errorReason = SpatialErrorReason::ERR_INVALID_REF_PRICE;
            return result;
        }
        if (tickSize <= 0.0) {
            result.errorReason = SpatialErrorReason::ERR_INVALID_TICK_SIZE;
            return result;
        }
        if (bidLevels.empty() && askLevels.empty()) {
            result.errorReason = SpatialErrorReason::ERR_NO_LEVEL_DATA;
            return result;
        }

        const int analysisRange = spatialConfig.analysisRangeTicks;

        // --------------------------------------------------------------------
        // Step 2: Convert to LevelInfo and filter by analysis range
        // --------------------------------------------------------------------
        std::vector<double> allDepths;  // For mean/stddev calculation

        // Process bid levels (below reference)
        for (const auto& level : bidLevels) {
            const double price = level.first;
            const double volume = level.second;
            if (price <= 0.0 || volume <= 0.0) continue;

            const double distTicks = (referencePrice - price) / tickSize;
            if (distTicks < 0.0 || distTicks > static_cast<double>(analysisRange)) continue;

            LevelInfo info;
            info.priceTicks = price / tickSize;
            info.volume = volume;
            info.distanceTicks = distTicks;
            info.weight = 1.0 / (1.0 + distTicks);
            info.isBid = true;
            result.bidLevels.push_back(info);
            allDepths.push_back(volume);
        }

        // Process ask levels (above reference)
        for (const auto& level : askLevels) {
            const double price = level.first;
            const double volume = level.second;
            if (price <= 0.0 || volume <= 0.0) continue;

            const double distTicks = (price - referencePrice) / tickSize;
            if (distTicks < 0.0 || distTicks > static_cast<double>(analysisRange)) continue;

            LevelInfo info;
            info.priceTicks = price / tickSize;
            info.volume = volume;
            info.distanceTicks = distTicks;
            info.weight = 1.0 / (1.0 + distTicks);
            info.isBid = false;
            result.askLevels.push_back(info);
            allDepths.push_back(volume);
        }

        // Check for sufficient data
        if (result.bidLevels.size() < spatialConfig.minLevelsForStats &&
            result.askLevels.size() < spatialConfig.minLevelsForStats) {
            result.errorReason = SpatialErrorReason::INSUFFICIENT_LEVELS;
            return result;
        }

        // --------------------------------------------------------------------
        // Step 3: Compute mean and stddev of depth across all levels
        // --------------------------------------------------------------------
        if (allDepths.size() >= spatialConfig.minLevelsForStats) {
            double sum = 0.0;
            for (double d : allDepths) sum += d;
            result.meanDepth = sum / static_cast<double>(allDepths.size());

            double sumSq = 0.0;
            for (double d : allDepths) {
                const double diff = d - result.meanDepth;
                sumSq += diff * diff;
            }
            result.stddevDepth = std::sqrt(sumSq / static_cast<double>(allDepths.size()));
            result.statsValid = (result.stddevDepth > 0.0);
        }

        // --------------------------------------------------------------------
        // Step 4: Detect walls (depth > mean + wallSigmaThreshold * stddev)
        // --------------------------------------------------------------------
        if (result.statsValid && result.stddevDepth > 0.0) {
            result.wallBaselineReady = true;

            // Check bid levels for walls
            for (const auto& level : result.bidLevels) {
                const double sigmaScore = (level.volume - result.meanDepth) / result.stddevDepth;
                if (sigmaScore >= spatialConfig.wallSigmaThreshold) {
                    WallInfo wall;
                    wall.priceTicks = level.priceTicks;
                    wall.volume = level.volume;
                    wall.sigmaScore = sigmaScore;
                    wall.distanceFromRef = static_cast<int>(level.distanceTicks);
                    wall.isBid = true;
                    result.walls.push_back(wall);
                    result.bidWallCount++;

                    // Track nearest wall
                    if (result.nearestBidWallTicks < 0.0 ||
                        level.distanceTicks < result.nearestBidWallTicks) {
                        result.nearestBidWallTicks = level.distanceTicks;
                    }
                }
            }

            // Check ask levels for walls
            for (const auto& level : result.askLevels) {
                const double sigmaScore = (level.volume - result.meanDepth) / result.stddevDepth;
                if (sigmaScore >= spatialConfig.wallSigmaThreshold) {
                    WallInfo wall;
                    wall.priceTicks = level.priceTicks;
                    wall.volume = level.volume;
                    wall.sigmaScore = sigmaScore;
                    wall.distanceFromRef = static_cast<int>(level.distanceTicks);
                    wall.isBid = false;
                    result.walls.push_back(wall);
                    result.askWallCount++;

                    // Track nearest wall
                    if (result.nearestAskWallTicks < 0.0 ||
                        level.distanceTicks < result.nearestAskWallTicks) {
                        result.nearestAskWallTicks = level.distanceTicks;
                    }
                }
            }
        }

        // --------------------------------------------------------------------
        // Step 5: Detect voids (depth < voidDepthRatio * mean or gaps)
        // --------------------------------------------------------------------
        if (result.statsValid && result.meanDepth > 0.0) {
            const double voidThreshold = result.meanDepth * spatialConfig.voidDepthRatio;

            // Check bid levels for thin areas
            for (const auto& level : result.bidLevels) {
                if (level.volume < voidThreshold) {
                    VoidInfo voidArea;
                    voidArea.startTicks = level.priceTicks;
                    voidArea.endTicks = level.priceTicks;
                    voidArea.gapTicks = 1;
                    voidArea.avgDepthRatio = level.volume / result.meanDepth;
                    voidArea.isAboveRef = false;
                    result.voids.push_back(voidArea);
                    result.bidVoidCount++;

                    if (result.nearestBidVoidTicks < 0.0 ||
                        level.distanceTicks < result.nearestBidVoidTicks) {
                        result.nearestBidVoidTicks = level.distanceTicks;
                    }
                }
            }

            // Check ask levels for thin areas
            for (const auto& level : result.askLevels) {
                if (level.volume < voidThreshold) {
                    VoidInfo voidArea;
                    voidArea.startTicks = level.priceTicks;
                    voidArea.endTicks = level.priceTicks;
                    voidArea.gapTicks = 1;
                    voidArea.avgDepthRatio = level.volume / result.meanDepth;
                    voidArea.isAboveRef = true;
                    result.voids.push_back(voidArea);
                    result.askVoidCount++;

                    if (result.nearestAskVoidTicks < 0.0 ||
                        level.distanceTicks < result.nearestAskVoidTicks) {
                        result.nearestAskVoidTicks = level.distanceTicks;
                    }
                }
            }
        }

        // --------------------------------------------------------------------
        // Step 6: Compute OBI (Order Book Imbalance) and POLR
        // --------------------------------------------------------------------
        double bidDepthTotal = 0.0;
        double askDepthTotal = 0.0;

        for (const auto& level : result.bidLevels) {
            bidDepthTotal += level.volume * level.weight;
        }
        for (const auto& level : result.askLevels) {
            askDepthTotal += level.volume * level.weight;
        }

        result.direction.bidDepthWithinN = bidDepthTotal;
        result.direction.askDepthWithinN = askDepthTotal;
        result.direction.rangeTicksUsed = analysisRange;

        const double totalDepth = bidDepthTotal + askDepthTotal;
        if (totalDepth > 0.0) {
            // OBI: (bid - ask) / (bid + ask)
            // Positive = more bids = bullish bias
            result.direction.orderBookImbalance = (bidDepthTotal - askDepthTotal) / totalDepth;

            // POLR: which direction has less resistance?
            // If bid > ask, easier to go up (ask side has less resistance)
            result.direction.polrIsUp = (bidDepthTotal > askDepthTotal);

            // POLR ratio: how much easier?
            const double minDepth = (std::min)(bidDepthTotal, askDepthTotal);
            const double maxDepth = (std::max)(bidDepthTotal, askDepthTotal);
            result.direction.polrRatio = (maxDepth > 0.0) ? minDepth / maxDepth : 1.0;

            result.direction.valid = true;
        }

        // --------------------------------------------------------------------
        // Step 7: Estimate execution risk (Kyle's Lambda-based)
        // --------------------------------------------------------------------
        const int riskTarget = spatialConfig.riskTargetTicks;

        // Risk for upward move (crossing ask levels)
        result.riskUp.targetTicks = riskTarget;
        double askDepthInTarget = 0.0;
        for (const auto& level : result.askLevels) {
            if (level.distanceTicks <= static_cast<double>(riskTarget)) {
                askDepthInTarget += level.volume;
            }
        }
        result.riskUp.cumulativeDepth = askDepthInTarget;
        if (askDepthInTarget > 0.0) {
            // Kyle's Lambda: price impact proportional to 1/depth
            result.riskUp.kyleLambda = 1.0 / askDepthInTarget;
            // Slippage estimate (simplified): assume average order size relative to depth
            result.riskUp.estimatedSlippageTicks = riskTarget * result.riskUp.kyleLambda * 100.0;
            result.riskUp.estimatedSlippageTicks = (std::min)(result.riskUp.estimatedSlippageTicks, 10.0);
        }
        result.riskUp.wallsTraversed = 0;
        for (const auto& wall : result.walls) {
            if (!wall.isBid && wall.distanceFromRef <= riskTarget) {
                result.riskUp.wallsTraversed++;
                if (wall.IsStrong()) result.riskUp.hasWallBlock = true;
            }
        }
        result.riskUp.voidsTraversed = 0;
        for (const auto& voidArea : result.voids) {
            if (voidArea.isAboveRef && voidArea.startTicks <= referencePrice / tickSize + riskTarget) {
                result.riskUp.voidsTraversed++;
                result.riskUp.hasVoidAcceleration = true;
            }
        }
        result.riskUp.isHighRisk = result.riskUp.hasWallBlock ||
                                   result.riskUp.estimatedSlippageTicks >= spatialConfig.highRiskSlippage;
        result.riskUp.valid = true;

        // Risk for downward move (crossing bid levels)
        result.riskDown.targetTicks = riskTarget;
        double bidDepthInTarget = 0.0;
        for (const auto& level : result.bidLevels) {
            if (level.distanceTicks <= static_cast<double>(riskTarget)) {
                bidDepthInTarget += level.volume;
            }
        }
        result.riskDown.cumulativeDepth = bidDepthInTarget;
        if (bidDepthInTarget > 0.0) {
            result.riskDown.kyleLambda = 1.0 / bidDepthInTarget;
            result.riskDown.estimatedSlippageTicks = riskTarget * result.riskDown.kyleLambda * 100.0;
            result.riskDown.estimatedSlippageTicks = (std::min)(result.riskDown.estimatedSlippageTicks, 10.0);
        }
        result.riskDown.wallsTraversed = 0;
        for (const auto& wall : result.walls) {
            if (wall.isBid && wall.distanceFromRef <= riskTarget) {
                result.riskDown.wallsTraversed++;
                if (wall.IsStrong()) result.riskDown.hasWallBlock = true;
            }
        }
        result.riskDown.voidsTraversed = 0;
        for (const auto& voidArea : result.voids) {
            if (!voidArea.isAboveRef && voidArea.endTicks >= referencePrice / tickSize - riskTarget) {
                result.riskDown.voidsTraversed++;
                result.riskDown.hasVoidAcceleration = true;
            }
        }
        result.riskDown.isHighRisk = result.riskDown.hasWallBlock ||
                                     result.riskDown.estimatedSlippageTicks >= spatialConfig.highRiskSlippage;
        result.riskDown.valid = true;

        // --------------------------------------------------------------------
        // Step 8: Set trade gating based on walls/voids
        // --------------------------------------------------------------------
        result.gating.valid = true;

        // Block longs if strong ask wall nearby (resistance above)
        if (result.nearestAskWallTicks >= 0.0 &&
            result.nearestAskWallTicks <= spatialConfig.wallBlockDistance) {
            for (const auto& wall : result.walls) {
                if (!wall.isBid && wall.IsStrong() &&
                    wall.distanceFromRef <= static_cast<int>(spatialConfig.wallBlockDistance)) {
                    result.gating.longBlocked = true;
                    result.gating.blockedByAskWall = true;
                    break;
                }
            }
        }

        // Block shorts if strong bid wall nearby (support below)
        if (result.nearestBidWallTicks >= 0.0 &&
            result.nearestBidWallTicks <= spatialConfig.wallBlockDistance) {
            for (const auto& wall : result.walls) {
                if (wall.isBid && wall.IsStrong() &&
                    wall.distanceFromRef <= static_cast<int>(spatialConfig.wallBlockDistance)) {
                    result.gating.shortBlocked = true;
                    result.gating.blockedByBidWall = true;
                    break;
                }
            }
        }

        // Note acceleration zones
        result.gating.acceleratedByAskVoid = result.riskUp.hasVoidAcceleration;
        result.gating.acceleratedByBidVoid = result.riskDown.hasVoidAcceleration;

        // Adjust risk multipliers
        if (result.riskUp.isHighRisk && !result.gating.longBlocked) {
            result.gating.longRiskMultiplier = 1.5;
        }
        if (result.riskDown.isHighRisk && !result.gating.shortBlocked) {
            result.gating.shortRiskMultiplier = 1.5;
        }

        // --------------------------------------------------------------------
        // Step 9: Mark as valid
        // --------------------------------------------------------------------
        result.valid = true;
        result.errorReason = SpatialErrorReason::NONE;

        return result;
    }

    // Helper: Copy spatial summary to Liq3Result
    void CopySpatialSummary(Liq3Result& snap, const SpatialLiquidityProfile& spatial) const {
        if (!spatial.valid) return;

        snap.spatialGating = spatial.gating;
        snap.orderBookImbalance = spatial.direction.orderBookImbalance;
        snap.pathOfLeastResistance = spatial.GetPOLRDirection();
        snap.nearestBidWallTicks = spatial.nearestBidWallTicks;
        snap.nearestAskWallTicks = spatial.nearestAskWallTicks;
        snap.nearestBidVoidTicks = spatial.nearestBidVoidTicks;
        snap.nearestAskVoidTicks = spatial.nearestAskVoidTicks;
        snap.hasSpatialProfile = true;
    }
};

} // namespace AMT
