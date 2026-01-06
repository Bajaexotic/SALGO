// ============================================================================
// AMT_Imbalance.h - Imbalance/Displacement Detection Engine
// ============================================================================
//
// PURPOSE: This engine answers "Did something actually move the auction?"
//
//   1. Did price displace or rotate? (objective displacement metric)
//   2. Is the move initiative-like vs responsive-like? (AMT framing)
//   3. Did the move occur in acceptable liquidity/vol regimes? (gated inputs)
//   4. What is the trigger output? (event enum + direction + strength + confidence)
//   5. What invalidates it? (low liquidity, high chop, overlapping profiles)
//
// DETECTION MECHANISMS:
//   - Diagonal Imbalance: Numbers Bars SG43/SG44 (footprint stacked imbalances)
//   - Delta Divergence: Price vs CVD divergence at swing points
//   - Absorption: High volume + narrow range = passive limit absorption
//   - Trapped Traders: Buy imbalances in red bars, sell imbalances in green
//   - Value Migration: POC shift, VA overlap percentage
//   - Range Extension: IB break with conviction (from DaltonEngine)
//
// DESIGN PRINCIPLES:
//   - Uses existing baselines from EffortBaselineStore (no new data collection)
//   - Phase-aware (GLOBEX != RTH)
//   - Context-gated via LiquidityEngine and VolatilityEngine
//   - Hysteresis prevents signal whipsaw
//   - NO-FALLBACK contract: explicit validity at every decision point
//   - ZERO Sierra Chart dependencies (testable standalone)
//
// INTEGRATION:
//   ImbalanceEngine imbalanceEngine;
//   imbalanceEngine.SetLiquidityEngine(&liquidityEngine);
//   imbalanceEngine.SetVolatilityEngine(&volatilityEngine);
//   imbalanceEngine.SetDaltonEngine(&daltonEngine);
//   imbalanceEngine.SetPhase(currentPhase);
//
//   ImbalanceResult result = imbalanceEngine.Compute(...);
//   if (result.IsReady() && result.HasSignal()) {
//       if (result.IsBullish() && result.IsInitiative()) {
//           // Strong bullish displacement detected
//       }
//   }
//
// SOURCES:
//   - Sierra Chart Numbers Bars (diagonal delta SG43/SG44)
//   - Volume at Price Threshold Alert V2 (stacked imbalance)
//   - Jim Dalton's Market Profile framework
//   - Kyle's Lambda (1985) for liquidity context
//
// ============================================================================

#pragma once

#include "amt_core.h"
#include "AMT_Snapshots.h"
#include "AMT_Volatility.h"  // For VolatilityRegime enum
#include "AMT_ValueLocation.h"  // For ValueLocationResult (SSOT)
#include "AMT_Invariants.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include <deque>

namespace AMT {

// Forward declarations for external engine types
struct Liq3Result;
struct VolatilityResult;
struct DaltonState;

// ============================================================================
// IMBALANCE TYPE ENUM
// ============================================================================
// What kind of displacement/imbalance was detected.
//
// STACKED_BUY/SELL: 3+ consecutive diagonal imbalances (footprint pattern)
// DELTA_DIVERGENCE: Price/CVD divergence at swing points
// ABSORPTION_*: Passive limit orders absorbing aggression
// TRAPPED_*: Imbalances opposite to bar direction (failed breakout)
// VALUE_MIGRATION: Significant POC/VA shift
// RANGE_EXTENSION: IB broken with conviction
// EXCESS: Single-print tail (auction rejection)
// ============================================================================

enum class ImbalanceType : int {
    NONE = 0,
    STACKED_BUY = 1,        // 3+ diagonal buy imbalances stacked
    STACKED_SELL = 2,       // 3+ diagonal sell imbalances stacked
    DELTA_DIVERGENCE = 3,   // Price/CVD divergence (reversal signal)
    ABSORPTION_BID = 4,     // Passive buying absorbing sell aggression
    ABSORPTION_ASK = 5,     // Passive selling absorbing buy aggression
    TRAPPED_LONGS = 6,      // Buy imbalances in red bar (trapped buyers)
    TRAPPED_SHORTS = 7,     // Sell imbalances in green bar (trapped sellers)
    VALUE_MIGRATION = 8,    // POC/VA shifted significantly
    RANGE_EXTENSION = 9,    // IB broken with conviction
    EXCESS = 10,            // Single-print tail (auction end)
    CLIMAX_HIGH = 11,       // Exhaustion at highs (extreme vol + delta + at extreme)
    CLIMAX_LOW = 12,        // Exhaustion at lows (extreme vol + delta + at extreme)
    POOR_HIGH = 13,         // Weak high (no excess, no acceptance)
    POOR_LOW = 14,          // Weak low (no excess, no acceptance)
    FAILED_AUCTION_VA = 15  // VA boundary breakout + rapid return (trap, distinct from IB)
};

inline const char* ImbalanceTypeToString(ImbalanceType t) {
    switch (t) {
        case ImbalanceType::NONE:             return "NONE";
        case ImbalanceType::STACKED_BUY:      return "STACKED_BUY";
        case ImbalanceType::STACKED_SELL:     return "STACKED_SELL";
        case ImbalanceType::DELTA_DIVERGENCE: return "DELTA_DIV";
        case ImbalanceType::ABSORPTION_BID:   return "ABSORB_BID";
        case ImbalanceType::ABSORPTION_ASK:   return "ABSORB_ASK";
        case ImbalanceType::TRAPPED_LONGS:    return "TRAPPED_LONG";
        case ImbalanceType::TRAPPED_SHORTS:   return "TRAPPED_SHORT";
        case ImbalanceType::VALUE_MIGRATION:  return "VA_MIGRATE";
        case ImbalanceType::RANGE_EXTENSION:  return "RANGE_EXT";
        case ImbalanceType::EXCESS:           return "EXCESS";
        case ImbalanceType::CLIMAX_HIGH:      return "CLIMAX_HIGH";
        case ImbalanceType::CLIMAX_LOW:       return "CLIMAX_LOW";
        case ImbalanceType::POOR_HIGH:         return "POOR_HIGH";
        case ImbalanceType::POOR_LOW:          return "POOR_LOW";
        case ImbalanceType::FAILED_AUCTION_VA: return "FAIL_AUCT_VA";
    }
    return "UNK";
}

// ============================================================================
// CONVICTION TYPE ENUM
// ============================================================================
// Is the move initiative (attacking) or responsive (defending)?
//
// INITIATIVE: Aggressive directional move with conviction
//   - Delta confirms direction
//   - Volume expanding
//   - 1TF pattern
//
// RESPONSIVE: Defensive/absorption/counter-trend activity
//   - Delta diverges from price
//   - Absorption detected
//   - Volume contracting or at extremes
//
// LIQUIDATION: Forced exit (special case)
//   - High stress + low conviction
//   - Often seen at extremes after extended moves
// ============================================================================

enum class ConvictionType : int {
    UNKNOWN = 0,
    INITIATIVE = 1,         // Aggressive directional move
    RESPONSIVE = 2,         // Defensive/absorption/counter
    LIQUIDATION = 3         // Forced exit (high stress, low conviction)
};

inline const char* ConvictionTypeToString(ConvictionType c) {
    switch (c) {
        case ConvictionType::UNKNOWN:     return "UNKNOWN";
        case ConvictionType::INITIATIVE:  return "INITIATIVE";
        case ConvictionType::RESPONSIVE:  return "RESPONSIVE";
        case ConvictionType::LIQUIDATION: return "LIQUIDATION";
    }
    return "UNK";
}

// ============================================================================
// IMBALANCE DIRECTION ENUM
// ============================================================================

enum class ImbalanceDirection : int {
    NEUTRAL = 0,
    BULLISH = 1,
    BEARISH = 2
};

inline const char* ImbalanceDirectionToString(ImbalanceDirection d) {
    switch (d) {
        case ImbalanceDirection::NEUTRAL: return "NEUTRAL";
        case ImbalanceDirection::BULLISH: return "BULLISH";
        case ImbalanceDirection::BEARISH: return "BEARISH";
    }
    return "UNK";
}

// ============================================================================
// IMBALANCE ERROR TAXONOMY (No Silent Failures)
// ============================================================================
// Every failure path must set an explicit errorReason.
// Enables diagnostics and tuning.
// ============================================================================

enum class ImbalanceErrorReason : int {
    NONE = 0,

    // Input validation errors
    ERR_INVALID_PRICE = 1,        // Price data invalid (zero, NaN)
    ERR_INVALID_TICK_SIZE = 2,    // Tick size <= 0
    ERR_NO_DIAGONAL_DATA = 3,     // Diagonal delta not provided

    // Baseline warmup states (not errors, expected during init)
    WARMUP_DIAGONAL = 10,         // Diagonal delta baseline warming
    WARMUP_SWING = 11,            // Swing tracking needs more bars
    WARMUP_POC = 12,              // POC history needs more data
    WARMUP_ABSORPTION = 13,       // Absorption baseline warming
    WARMUP_MULTIPLE = 14,         // Multiple baselines warming

    // Context gate blocks
    BLOCKED_LIQUIDITY_VOID = 20,  // LiquidityState::LIQ_VOID
    BLOCKED_LIQUIDITY_THIN = 21,  // LiquidityState::LIQ_THIN (optional)
    BLOCKED_VOLATILITY_EVENT = 22,// VolatilityRegime::EVENT
    BLOCKED_CHOP = 23,            // High rotation, overlapping profiles

    // Engine reference errors
    ERR_NO_EFFORT_STORE = 30,     // EffortBaselineStore not configured
    ERR_NO_LIQUIDITY_ENGINE = 31, // LiquidityEngine not configured (if required)
    ERR_NO_VOLATILITY_ENGINE = 32 // VolatilityEngine not configured (if required)
};

inline const char* ImbalanceErrorToString(ImbalanceErrorReason r) {
    switch (r) {
        case ImbalanceErrorReason::NONE:                    return "NONE";
        case ImbalanceErrorReason::ERR_INVALID_PRICE:       return "INVALID_PRICE";
        case ImbalanceErrorReason::ERR_INVALID_TICK_SIZE:   return "INVALID_TICK";
        case ImbalanceErrorReason::ERR_NO_DIAGONAL_DATA:    return "NO_DIAG_DATA";
        case ImbalanceErrorReason::WARMUP_DIAGONAL:         return "WARMUP_DIAG";
        case ImbalanceErrorReason::WARMUP_SWING:            return "WARMUP_SWING";
        case ImbalanceErrorReason::WARMUP_POC:              return "WARMUP_POC";
        case ImbalanceErrorReason::WARMUP_ABSORPTION:       return "WARMUP_ABSORB";
        case ImbalanceErrorReason::WARMUP_MULTIPLE:         return "WARMUP_MULTI";
        case ImbalanceErrorReason::BLOCKED_LIQUIDITY_VOID:  return "BLOCK_LIQ_VOID";
        case ImbalanceErrorReason::BLOCKED_LIQUIDITY_THIN:  return "BLOCK_LIQ_THIN";
        case ImbalanceErrorReason::BLOCKED_VOLATILITY_EVENT:return "BLOCK_VOL_EVENT";
        case ImbalanceErrorReason::BLOCKED_CHOP:            return "BLOCK_CHOP";
        case ImbalanceErrorReason::ERR_NO_EFFORT_STORE:     return "NO_EFFORT_STORE";
        case ImbalanceErrorReason::ERR_NO_LIQUIDITY_ENGINE: return "NO_LIQ_ENGINE";
        case ImbalanceErrorReason::ERR_NO_VOLATILITY_ENGINE:return "NO_VOL_ENGINE";
    }
    return "UNKNOWN";
}

inline bool IsImbalanceWarmup(ImbalanceErrorReason r) {
    return r == ImbalanceErrorReason::WARMUP_DIAGONAL ||
           r == ImbalanceErrorReason::WARMUP_SWING ||
           r == ImbalanceErrorReason::WARMUP_POC ||
           r == ImbalanceErrorReason::WARMUP_ABSORPTION ||
           r == ImbalanceErrorReason::WARMUP_MULTIPLE;
}

inline bool IsImbalanceBlocked(ImbalanceErrorReason r) {
    return r == ImbalanceErrorReason::BLOCKED_LIQUIDITY_VOID ||
           r == ImbalanceErrorReason::BLOCKED_LIQUIDITY_THIN ||
           r == ImbalanceErrorReason::BLOCKED_VOLATILITY_EVENT ||
           r == ImbalanceErrorReason::BLOCKED_CHOP;
}

// ============================================================================
// CONTEXT GATE RESULT
// ============================================================================
// Results from checking LiquidityEngine and VolatilityEngine gates.
// Tells us if the market context is suitable for trusting imbalance signals.
// ============================================================================

struct ContextGateResult {
    // Individual gate results
    bool liquidityOK = false;           // Not in VOID (or THIN if configured)
    bool volatilityOK = false;          // Not in EVENT regime
    bool chopOK = false;                // Not in high-chop overlapping profile

    // Combined result
    bool allGatesPass = false;

    // Detailed state for diagnostics
    LiquidityState liqState = LiquidityState::LIQ_NOT_READY;
    VolatilityRegime volRegime = VolatilityRegime::UNKNOWN;
    double executionFriction = 1.0;     // From LiquidityEngine [0, 1]
    double vaOverlapPct = 1.0;          // VA overlap with prior [0, 1]
    int rotationFactor = 0;             // From DaltonEngine

    // Why blocked (if any)
    ImbalanceErrorReason blockReason = ImbalanceErrorReason::NONE;
};

// ============================================================================
// SWING POINT (For Divergence Detection)
// ============================================================================
// Tracks price swing highs/lows with corresponding delta values.
// Used to detect price/delta divergences.
// ============================================================================

struct SwingPoint {
    double price = 0.0;
    double delta = 0.0;          // Cumulative delta at swing
    int barIndex = 0;
    bool isHigh = false;         // true = swing high, false = swing low
    bool valid = false;
};

// ============================================================================
// POC TRACKER (For Value Migration)
// ============================================================================
// Tracks POC position over time to detect meaningful shifts.
// ============================================================================

struct POCTracker {
    double anchorPrice = 0.0;    // Stable POC reference
    double currentPrice = 0.0;   // Latest POC
    int stableCount = 0;         // Consecutive bars at same level
    int barIndex = 0;
    bool valid = false;

    void Reset() {
        anchorPrice = 0.0;
        currentPrice = 0.0;
        stableCount = 0;
        barIndex = 0;
        valid = false;
    }
};

struct FailedAuctionTracker {
    bool active = false;         // Currently tracking a breakout
    int breakoutBar = 0;         // Bar when breakout started
    bool brokeAbove = false;     // True if broke above VAH, false if below VAL
    int barsOutside = 0;         // Bars spent outside value

    void Reset() {
        active = false;
        breakoutBar = 0;
        brokeAbove = false;
        barsOutside = 0;
    }
};

// ============================================================================
// AUCTION LEVEL CONTEXT (Jan 2025)
// ============================================================================
// Provides actionable levels for the current imbalance state:
// - Where the move would be ACCEPTED (confirmed)
// - Where the move would be REJECTED (failed)
// - Target objective if imbalance continues
// - Prior session reference levels
// ============================================================================

struct AuctionLevelContext {
    // Acceptance levels (where move becomes "real")
    double acceptanceLevel = 0.0;      // Price that would confirm acceptance
    bool acceptanceLevelValid = false;

    // Failure levels (where move is rejected)
    double failureLevel = 0.0;         // Price that would confirm rejection
    bool failureLevelValid = false;

    // Auction objective (target if imbalance continues)
    double auctionObjective = 0.0;     // Next reference level target
    bool auctionObjectiveValid = false;

    // Prior reference levels (from DaltonEngine.SessionBridge)
    double priorPOC = 0.0;
    double priorVAH = 0.0;
    double priorVAL = 0.0;
    bool priorLevelsValid = false;

    // Consumed excess from SSOT (ExcessDetector)
    ExcessType consumedExcess = ExcessType::NONE;

    // Helpers
    bool HasAcceptanceLevel() const { return acceptanceLevelValid; }
    bool HasFailureLevel() const { return failureLevelValid; }
    bool HasAuctionObjective() const { return auctionObjectiveValid; }
    bool HasPriorLevels() const { return priorLevelsValid; }
};

// ============================================================================
// IMBALANCE RESULT (Per-Bar Output)
// ============================================================================
// Complete snapshot of imbalance detection state for the current bar.
// ============================================================================

struct ImbalanceResult {
    // ========================================================================
    // PRIMARY DETECTION
    // ========================================================================
    ImbalanceType type = ImbalanceType::NONE;
    ImbalanceDirection direction = ImbalanceDirection::NEUTRAL;
    ConvictionType conviction = ConvictionType::UNKNOWN;

    // ========================================================================
    // DISPLACEMENT METRICS
    // ========================================================================
    double displacementScore = 0.0;      // [0, 1] composite displacement
    double displacementTicks = 0.0;      // Raw displacement in ticks

    // ========================================================================
    // DIAGONAL IMBALANCE (from Numbers Bars SG43/SG44)
    // ========================================================================
    double diagonalPosDelta = 0.0;       // Raw positive diagonal delta
    double diagonalNegDelta = 0.0;       // Raw negative diagonal delta
    double diagonalNetDelta = 0.0;       // pos - neg
    double diagonalRatio = 0.5;          // pos / (pos + neg), 0.5 = neutral
    double diagonalPercentile = 50.0;    // Percentile vs baseline

    int stackedBuyLevels = 0;            // Consecutive buy imbalances
    int stackedSellLevels = 0;           // Consecutive sell imbalances
    bool hasStackedImbalance = false;    // 3+ stacked either side
    bool hasBigImbalance = false;        // 1000%+ ratio detected

    // ========================================================================
    // DELTA DIVERGENCE
    // ========================================================================
    bool hasDeltaDivergence = false;
    bool divergenceBullish = false;      // Price lower low, delta higher low
    bool divergenceBearish = false;      // Price higher high, delta lower high
    double divergenceStrength = 0.0;     // [0, 1] how pronounced

    // ========================================================================
    // ABSORPTION (AMT: Extreme delta + narrow range = passive side absorbing)
    // ========================================================================
    bool absorptionDetected = false;
    bool absorptionBidSide = false;      // Passive BIDS absorbing aggressive sells
    bool absorptionAskSide = false;      // Passive ASKS absorbing aggressive buys
    double absorptionScore = 0.0;        // [0, 1] how strong (range + delta extremeness)
    double absorptionDeltaPct = 0.0;     // Delta/Volume ratio (negative = sells, positive = buys)

    // ========================================================================
    // TRAPPED TRADERS
    // ========================================================================
    bool trappedTradersDetected = false;
    bool trappedLongs = false;           // Buy imbalances in down bar
    bool trappedShorts = false;          // Sell imbalances in up bar

    // ========================================================================
    // VALUE AREA CONTEXT
    // ========================================================================
    double pocShiftTicks = 0.0;          // POC movement since anchor
    double pocShiftPercentile = 50.0;    // Percentile of shift magnitude
    double vaOverlapPct = 1.0;           // [0, 1] VA overlap with prior
    bool pocMigrating = false;           // POC shifting consistently
    ValueMigration valueMigration = ValueMigration::UNKNOWN;

    // ========================================================================
    // RANGE EXTENSION (from IB context)
    // ========================================================================
    bool rangeExtensionDetected = false;
    bool extensionAboveIB = false;
    bool extensionBelowIB = false;
    double extensionRatio = 1.0;         // Session range / IB range

    // ========================================================================
    // EXCESS (Auction Rejection)
    // ========================================================================
    bool excessDetected = false;
    bool excessHigh = false;             // Rejection at high
    bool excessLow = false;              // Rejection at low

    // ========================================================================
    // CLIMAX (Exhaustion at Extremes)
    // ========================================================================
    bool climaxDetected = false;
    bool climaxHigh = false;             // Exhaustion at highs
    bool climaxLow = false;              // Exhaustion at lows
    double climaxScore = 0.0;            // [0, 1] how extreme
    double volumePercentile = 0.0;       // Volume intensity (for climax)
    double deltaPercentile = 0.0;        // Delta intensity (for climax)

    // ========================================================================
    // POOR HIGH/LOW (Weak Auction Ends)
    // ========================================================================
    bool poorHighDetected = false;       // No excess, no strong bid absorption
    bool poorLowDetected = false;        // No excess, no strong ask absorption
    double poorHighScore = 0.0;          // [0, 1] weakness at high
    double poorLowScore = 0.0;           // [0, 1] weakness at low

    // ========================================================================
    // FAILED AUCTION (Breakout Trap)
    // ========================================================================
    bool failedAuctionDetected = false;
    bool failedBreakoutAbove = false;    // Broke out above, rapid return
    bool failedBreakoutBelow = false;    // Broke out below, rapid return
    int barsOutside = 0;                 // Bars spent outside before return
    double failedAuctionScore = 0.0;     // [0, 1] trap quality

    // ========================================================================
    // STRENGTH & CONFIDENCE
    // ========================================================================
    double strengthScore = 0.0;          // [0, 1] raw signal strength
    double confidenceScore = 0.0;        // [0, 1] gated confidence
    int signalCount = 0;                 // How many signals firing

    // ========================================================================
    // CONTEXT GATES
    // ========================================================================
    ContextGateResult contextGate;

    // ========================================================================
    // AUCTION LEVEL CONTEXT (Jan 2025)
    // ========================================================================
    AuctionLevelContext levels;

    // ========================================================================
    // DOM CONTEXT (From LiquidityEngine)
    // ========================================================================
    double consumedBidMass = 0.0;         // Liquidity consumed on bid side
    double consumedAskMass = 0.0;         // Liquidity consumed on ask side
    double consumedTotalMass = 0.0;       // Total consumed depth
    double toxicityProxy = 0.0;           // Asymmetric consumption [0,1]
    bool hasDOMContext = false;           // True if DOM data was provided
    bool highConsumedDepth = false;       // Above threshold (confirms absorption)

    // ========================================================================
    // SPATIAL LIQUIDITY (From LiquidityEngine.spatialGating)
    // ========================================================================
    // Walls block price movement, voids accelerate it. POLR = Path of Least Resistance.
    double nearestBidWallTicks = -1.0;    // Distance to nearest bid wall (support)
    double nearestAskWallTicks = -1.0;    // Distance to nearest ask wall (resistance)
    double nearestBidVoidTicks = -1.0;    // Distance to nearest bid void (acceleration down)
    double nearestAskVoidTicks = -1.0;    // Distance to nearest ask void (acceleration up)
    int pathOfLeastResistance = 0;        // -1=DOWN, 0=BALANCED, +1=UP
    bool hasSpatialContext = false;       // True if spatial data was provided
    bool wallBlocksBullish = false;       // Ask wall very close, blocks bullish signals
    bool wallBlocksBearish = false;       // Bid wall very close, blocks bearish signals
    bool voidAcceleratesBullish = false;  // Ask void close, boosts bullish conviction
    bool voidAcceleratesBearish = false;  // Bid void close, boosts bearish conviction
    double convictionAdjustment = 0.0;    // Applied adjustment from spatial [-0.3, +0.3]

    // ========================================================================
    // HYSTERESIS STATE
    // ========================================================================
    ImbalanceType confirmedType = ImbalanceType::NONE;    // After hysteresis
    ImbalanceType candidateType = ImbalanceType::NONE;    // Being confirmed
    int confirmationBars = 0;            // Bars confirming candidate
    int barsInState = 0;                 // Bars in confirmed state
    bool isTransitioning = false;        // State change in progress

    // ========================================================================
    // EVENTS (Only True on Detection Bars)
    // ========================================================================
    bool imbalanceEntered = false;       // New imbalance detected this bar
    bool imbalanceResolved = false;      // Imbalance completed/failed this bar
    bool convictionChanged = false;      // Initiative <-> Responsive this bar
    bool typeChanged = false;            // ImbalanceType changed this bar

    // ========================================================================
    // VALIDITY / ERROR
    // ========================================================================
    ImbalanceErrorReason errorReason = ImbalanceErrorReason::NONE;
    SessionPhase phase = SessionPhase::UNKNOWN;
    int errorBar = -1;

    // ========================================================================
    // ACCESSORS
    // ========================================================================

    bool IsReady() const {
        return errorReason == ImbalanceErrorReason::NONE;
    }

    bool IsWarmup() const {
        return IsImbalanceWarmup(errorReason);
    }

    bool IsBlocked() const {
        return IsImbalanceBlocked(errorReason);
    }

    bool IsHardError() const {
        return errorReason != ImbalanceErrorReason::NONE &&
               !IsWarmup() && !IsBlocked();
    }

    bool HasSignal() const {
        return IsReady() && type != ImbalanceType::NONE;
    }

    bool HasConfirmedSignal() const {
        return IsReady() && confirmedType != ImbalanceType::NONE;
    }

    bool IsBullish() const { return direction == ImbalanceDirection::BULLISH; }
    bool IsBearish() const { return direction == ImbalanceDirection::BEARISH; }
    bool IsNeutral() const { return direction == ImbalanceDirection::NEUTRAL; }

    bool IsInitiative() const { return conviction == ConvictionType::INITIATIVE; }
    bool IsResponsive() const { return conviction == ConvictionType::RESPONSIVE; }
    bool IsLiquidation() const { return conviction == ConvictionType::LIQUIDATION; }

    // Signal quality check (strong + initiative + context OK)
    bool IsHighQualitySignal() const {
        return HasConfirmedSignal() &&
               IsInitiative() &&
               contextGate.allGatesPass &&
               confidenceScore >= 0.6;
    }

    // Spatial context helpers
    bool HasSpatialContext() const { return hasSpatialContext; }
    bool IsWallBlocked() const { return wallBlocksBullish || wallBlocksBearish; }
    bool IsVoidAccelerated() const { return voidAcceleratesBullish || voidAcceleratesBearish; }
    bool IsSpatiallyFavorable() const { return convictionAdjustment > 0.0; }
    bool IsSpatiallyUnfavorable() const { return convictionAdjustment < -0.1; }
};

// ============================================================================
// IMBALANCE CONFIGURATION
// ============================================================================

struct ImbalanceConfig {
    // ========================================================================
    // STACKED IMBALANCE THRESHOLDS
    // ========================================================================
    int minStackedLevels = 3;            // Minimum consecutive levels for stacked
    double diagonalRatioThreshold = 3.0; // 300% ratio = imbalance at single level (fallback)
    double bigImbalanceThreshold = 10.0; // 1000% ratio = "big" imbalance (fallback)
    // Percentile-based thresholds (adaptive to market conditions)
    bool usePercentileBasedRatios = true;// Use adaptive thresholds when baseline ready
    double diagonalRatioPctileThreshold = 75.0;  // >P75 ratio = imbalance
    double bigImbalancePctileThreshold = 90.0;   // >P90 ratio = "big" imbalance

    // ========================================================================
    // DIVERGENCE DETECTION
    // ========================================================================
    int divergenceLookback = 5;          // Bars to check for swing points
    double divergenceMinTicks = 2.0;     // Minimum swing size in ticks
    int minSwingBars = 2;                // Minimum bars between swings

    // ========================================================================
    // ABSORPTION DETECTION (AMT Definition)
    // ========================================================================
    double absorptionVolumeThreshold = 75.0;  // Volume percentile (high volume)
    double absorptionRangeThreshold = 25.0;   // Range percentile (narrow range)
    double absorptionDeltaThreshold = 0.30;   // MIN |delta/volume| for absorption (extreme delta)
    double consumedDepthThreshold = 0.6;      // Consumed/Peak ratio for "high" consumption
    double toxicityThreshold = 0.3;           // Toxicity proxy for asymmetric consumption

    // ========================================================================
    // VALUE MIGRATION
    // ========================================================================
    double pocShiftMinTicks = 4.0;       // Minimum meaningful POC shift
    int pocStabilityBars = 3;            // Bars to confirm POC position
    double vaOverlapHighThreshold = 0.7; // >70% = overlapping (balance)
    double vaOverlapLowThreshold = 0.3;  // <30% = extension (imbalance)

    // ========================================================================
    // CLIMAX DETECTION (Exhaustion at Extremes)
    // ========================================================================
    double climaxVolumeThreshold = 90.0;  // Volume percentile (>= P90 = extreme)
    double climaxDeltaThreshold = 85.0;   // Delta percentile (>= P85 = extreme)
    double extremeProximityTicks = 4.0;   // Within N ticks of session extreme

    // ========================================================================
    // POOR HIGH/LOW DETECTION
    // ========================================================================
    double poorHighLowVolThreshold = 40.0;  // Volume < P40 at extreme = weak
    double poorHighLowDeltaThreshold = 50.0;// Delta < P50 at extreme = no conviction

    // ========================================================================
    // FAILED AUCTION DETECTION
    // ========================================================================
    int failedAuctionLookback = 5;        // Bars to look back for breakout
    int failedAuctionMaxBars = 3;         // Max bars outside before return = trap
    double failedAuctionMinBreak = 2.0;   // Min ticks outside VA for "breakout"

    // ========================================================================
    // SPATIAL LIQUIDITY (Wall/Void Conviction Adjustment)
    // ========================================================================
    bool useSpatialConviction = true;     // Enable spatial liquidity conviction adjustment
    double wallBlockDistanceTicks = 3.0;  // Wall within N ticks blocks signal in that direction
    double voidAccelDistanceTicks = 5.0;  // Void within N ticks boosts signal in that direction
    double maxWallPenalty = 0.30;         // Max conviction reduction from nearby wall
    double maxVoidBoost = 0.20;           // Max conviction boost from nearby void
    double polrBoost = 0.10;              // Boost when signal aligns with path of least resistance

    // ========================================================================
    // CONTEXT GATES
    // ========================================================================
    bool requireLiquidityGate = true;
    bool requireVolatilityGate = true;
    bool blockOnVoid = true;             // Block on LIQ_VOID
    bool blockOnThin = false;            // Optionally block on LIQ_THIN
    bool blockOnEvent = true;            // Block on EVENT volatility
    double chopRotationThreshold = 4.0;  // |rotation| < this with overlap = chop

    // ========================================================================
    // HYSTERESIS
    // ========================================================================
    int minConfirmationBars = 2;         // Bars to confirm state change
    int maxPersistenceBars = 10;         // Max bars signal persists without refresh

    // ========================================================================
    // BASELINE REQUIREMENTS
    // ========================================================================
    size_t baselineMinSamples = 10;      // Minimum samples before ready
    int baselineWindow = 300;            // Rolling window size (bars)

    // ========================================================================
    // ENGINE REFERENCES
    // ========================================================================
    bool requireEffortStore = false;     // If true, Compute() fails without effortStore

    // ========================================================================
    // STRENGTH/CONFIDENCE WEIGHTS
    // ========================================================================
    double weightStacked = 0.30;         // Weight for stacked imbalance
    double weightDivergence = 0.25;      // Weight for divergence
    double weightAbsorption = 0.20;      // Weight for absorption
    double weightValueMigration = 0.15;  // Weight for VA migration
    double weightRangeExtension = 0.10;  // Weight for IB extension
};

// ============================================================================
// IMBALANCE ENGINE
// ============================================================================
// Main engine for detecting market imbalances and displacements.
//
// USAGE:
//   1. Create engine and configure
//   2. Set external engine references (optional but recommended)
//   3. Call SetPhase() each bar with current session phase
//   4. Call Compute() with bar data
//   5. Check result.IsReady() and result.HasSignal()
//
// SESSION BOUNDARY:
//   Call ResetForSession() at start of new session
//
// ============================================================================

class ImbalanceEngine {
public:
    // ========================================================================
    // CONFIGURATION
    // ========================================================================
    ImbalanceConfig config;

    // ========================================================================
    // EXTERNAL ENGINE REFERENCES (Not Owned)
    // ========================================================================
    // These are optional but recommended for context gating.
    // If not set, corresponding gates default to "pass".
    const EffortBaselineStore* effortStore = nullptr;

    // ========================================================================
    // CURRENT STATE
    // ========================================================================
    SessionPhase currentPhase = SessionPhase::UNKNOWN;

    // ========================================================================
    // SWING TRACKING (For Divergence)
    // ========================================================================
    std::vector<SwingPoint> swingHighs;
    std::vector<SwingPoint> swingLows;
    double lastHigh = 0.0;
    double lastLow = 0.0;
    double lastDelta = 0.0;
    int lastSwingBar = 0;

    // ========================================================================
    // POC TRACKING (For Value Migration)
    // ========================================================================
    POCTracker pocTracker;
    double prevPOC = 0.0;
    double prevVAH = 0.0;
    double prevVAL = 0.0;

    // ========================================================================
    // FAILED AUCTION TRACKING
    // ========================================================================
    FailedAuctionTracker failedAuctionTracking;

    // ========================================================================
    // BASELINES (Phase-Bucketed)
    // ========================================================================
    // Each tradeable phase (GLOBEX, LONDON, PRE_MKT, IB, MID_SESS, CLOSING, POST_CLOSE)
    // has its own baseline distribution. GLOBEX != RTH characteristics.
    std::array<RollingDist, EFFORT_BUCKET_COUNT> diagonalNetBaseline;     // |diagonalNet| baseline per phase
    std::array<RollingDist, EFFORT_BUCKET_COUNT> diagonalRatioBaseline;   // max(buyRatio, sellRatio) baseline per phase
    std::array<RollingDist, EFFORT_BUCKET_COUNT> pocShiftBaseline;        // |pocShift| baseline per phase
    std::array<RollingDist, EFFORT_BUCKET_COUNT> absorptionBaseline;      // absorption score baseline per phase

    // ========================================================================
    // HYSTERESIS STATE
    // ========================================================================
    ImbalanceType confirmedType = ImbalanceType::NONE;
    ImbalanceType candidateType = ImbalanceType::NONE;
    int candidateConfirmationBars = 0;
    int barsInConfirmedState = 0;
    ConvictionType lastConviction = ConvictionType::UNKNOWN;

    // ========================================================================
    // SESSION STATS
    // ========================================================================
    int sessionBars = 0;
    int stackedBuyCount = 0;
    int stackedSellCount = 0;
    int divergenceCount = 0;
    int absorptionCount = 0;

    // ========================================================================
    // CONSTRUCTOR / INITIALIZATION
    // ========================================================================

    ImbalanceEngine() {
        for (int i = 0; i < EFFORT_BUCKET_COUNT; ++i) {
            diagonalNetBaseline[i].reset(300);
            diagonalRatioBaseline[i].reset(300);
            pocShiftBaseline[i].reset(300);
            absorptionBaseline[i].reset(300);
        }
    }

    void SetEffortStore(const EffortBaselineStore* store) {
        effortStore = store;
    }

    void SetPhase(SessionPhase phase) {
        currentPhase = phase;
    }

    void SetConfig(const ImbalanceConfig& cfg) {
        config = cfg;
    }

    // ========================================================================
    // MAIN COMPUTATION
    // ========================================================================
    // Call once per closed bar with all available data.
    //
    // Required parameters:
    //   high, low, close, open - Bar OHLC
    //   prevHigh, prevLow, prevClose - Previous bar data (for swing detection)
    //   tickSize - Tick size for normalization
    //   barIndex - Current bar index
    //
    // Optional parameters (pass -1 or nullptr if not available):
    //   poc, vah, val - Current profile levels
    //   prevPOC, prevVAH, prevVAL - Previous stable profile levels
    //   diagonalPosDelta, diagonalNegDelta - From Numbers Bars SG43/SG44
    //   totalVolume, barDelta, cumulativeDelta - Volume/delta data
    //   liqState, volRegime - External engine results for gating
    //   executionFriction - From LiquidityEngine
    //   ibHigh, ibLow - Initial Balance levels for range extension
    //   sessionHigh, sessionLow - Session extremes
    //
    // DEPRECATED: Use ComputeFromValueLocation() for SSOT compliance
    [[deprecated("Use ComputeFromValueLocation() with ValueLocationResult from ValueLocationEngine (SSOT)")]]
    ImbalanceResult Compute(
        // Price data (required)
        double high, double low, double close, double open,
        double prevHigh, double prevLow, double prevClose,
        double tickSize, int barIndex,
        // Profile data (optional, pass 0 if unavailable)
        double poc = 0.0, double vah = 0.0, double val = 0.0,
        double inputPrevPOC = 0.0, double inputPrevVAH = 0.0, double inputPrevVAL = 0.0,
        // Diagonal delta from Numbers Bars (optional, pass -1 if unavailable)
        double diagonalPosDelta = -1.0, double diagonalNegDelta = -1.0,
        // Volume/delta (optional, pass -1 if unavailable)
        double totalVolume = -1.0, double barDelta = -1.0, double cumulativeDelta = -1.0,
        // Context gates (optional)
        LiquidityState liqState = LiquidityState::LIQ_NOT_READY,
        VolatilityRegime volRegime = VolatilityRegime::UNKNOWN,
        double executionFriction = -1.0,
        // IB/Session context (optional)
        double ibHigh = 0.0, double ibLow = 0.0,
        double sessionHigh = 0.0, double sessionLow = 0.0,
        int rotationFactor = 0, bool is1TF = false,
        // DOM consumed depth (optional, from LiquidityEngine)
        double consumedBidMass = -1.0, double consumedAskMass = -1.0,
        double toxicityProxy = -1.0,
        // DOM spatial liquidity (optional, from LiquidityEngine.spatialGating)
        double nearestBidWallTicks = -1.0, double nearestAskWallTicks = -1.0,
        double nearestBidVoidTicks = -1.0, double nearestAskVoidTicks = -1.0,
        int pathOfLeastResistance = 0,  // -1=DOWN, 0=BALANCED, +1=UP
        // SSOT consumed excess (from ExcessDetector, Jan 2025)
        ExcessType consumedExcess = ExcessType::NONE,
        // Prior session levels (from DaltonEngine.SessionBridge, for AuctionLevelContext)
        double priorPOC = 0.0, double priorVAH = 0.0, double priorVAL = 0.0
    ) {
        ImbalanceResult result;
        result.phase = currentPhase;

        // ====================================================================
        // INPUT VALIDATION
        // ====================================================================
        if (!std::isfinite(high) || !std::isfinite(low) ||
            !std::isfinite(close) || !std::isfinite(open) ||
            high <= 0.0 || low <= 0.0) {
            result.errorReason = ImbalanceErrorReason::ERR_INVALID_PRICE;
            result.errorBar = barIndex;
            return result;
        }

        if (tickSize <= 0.0 || !std::isfinite(tickSize)) {
            result.errorReason = ImbalanceErrorReason::ERR_INVALID_TICK_SIZE;
            result.errorBar = barIndex;
            return result;
        }

        // ====================================================================
        // EFFORT STORE VALIDATION
        // ====================================================================
        if (config.requireEffortStore && effortStore == nullptr) {
            result.errorReason = ImbalanceErrorReason::ERR_NO_EFFORT_STORE;
            result.errorBar = barIndex;
            return result;
        }

        sessionBars++;

        // ====================================================================
        // STEP 1: CONTEXT GATES
        // ====================================================================
        result.contextGate = ApplyContextGates(liqState, volRegime, executionFriction, rotationFactor);

        // Check for blocking conditions
        if (result.contextGate.blockReason != ImbalanceErrorReason::NONE) {
            result.errorReason = result.contextGate.blockReason;
            result.errorBar = barIndex;
            // Continue processing to populate diagnostics, but signal is blocked
        }

        // ====================================================================
        // STEP 2: DIAGONAL IMBALANCE DETECTION
        // ====================================================================
        if (diagonalPosDelta >= 0.0 && diagonalNegDelta >= 0.0) {
            DetectDiagonalImbalance(result, diagonalPosDelta, diagonalNegDelta,
                                     open, close, barIndex);
        }

        // ====================================================================
        // STEP 3: DELTA DIVERGENCE DETECTION
        // ====================================================================
        if (cumulativeDelta > -1e9) {  // Valid cumulative delta
            DetectDeltaDivergence(result, high, low, cumulativeDelta,
                                   prevHigh, prevLow, tickSize, barIndex);
        }

        // ====================================================================
        // STEP 4: ABSORPTION DETECTION (with DOM consumed depth)
        // ====================================================================
        // LOCATION-GATING (Jan 2025): Absorption is only meaningful at significant
        // levels (VAH/VAL, session extremes, IB extremes). Absorption in the middle
        // of the value area is noise - passive activity there is normal rotation.
        if (totalVolume > 0.0 && barDelta > -1e9) {
            // Check if we're at a meaningful level for absorption
            const double tolerance = tickSize * 3.0;  // 3 ticks tolerance
            const bool atVAH = (vah > 0.0 && std::abs(close - vah) <= tolerance);
            const bool atVAL = (val > 0.0 && std::abs(close - val) <= tolerance);
            const bool atSessionHigh = (sessionHigh > 0.0 && std::abs(high - sessionHigh) <= tolerance);
            const bool atSessionLow = (sessionLow > 0.0 && std::abs(low - sessionLow) <= tolerance);
            const bool atIBHigh = (ibHigh > 0.0 && std::abs(high - ibHigh) <= tolerance);
            const bool atIBLow = (ibLow > 0.0 && std::abs(low - ibLow) <= tolerance);

            const bool atMeaningfulLevel = atVAH || atVAL ||
                                            atSessionHigh || atSessionLow ||
                                            atIBHigh || atIBLow;

            if (atMeaningfulLevel) {
                DetectAbsorption(result, high, low, totalVolume, barDelta, tickSize,
                                 consumedBidMass, consumedAskMass, toxicityProxy);
            }
            // If not at meaningful level, skip absorption detection (it's noise)
        }

        // ====================================================================
        // STEP 5: VALUE MIGRATION
        // ====================================================================
        if (poc > 0.0 && vah > 0.0 && val > 0.0) {
            ComputeValueMigration(result, poc, vah, val,
                                   inputPrevPOC > 0.0 ? inputPrevPOC : prevPOC,
                                   inputPrevVAH > 0.0 ? inputPrevVAH : prevVAH,
                                   inputPrevVAL > 0.0 ? inputPrevVAL : prevVAL,
                                   tickSize, barIndex);

            // Update previous values for next bar
            prevPOC = poc;
            prevVAH = vah;
            prevVAL = val;
        }

        // ====================================================================
        // STEP 6: RANGE EXTENSION
        // ====================================================================
        if (ibHigh > 0.0 && ibLow > 0.0 && sessionHigh > 0.0 && sessionLow > 0.0) {
            DetectRangeExtension(result, high, low, ibHigh, ibLow,
                                  sessionHigh, sessionLow, is1TF);
        }

        // ====================================================================
        // STEP 7: CONSUME EXCESS FROM SSOT (ExcessDetector, Jan 2025)
        // ====================================================================
        // NOTE: Excess and Poor High/Low detection is now centralized in ExcessDetector
        // (AMT_Signals.h). This engine CONSUMES the SSOT result instead of computing its own.
        // The caller passes consumedExcess from ExcessDetector.GetCombinedExcess().
        ConsumeExcessFromSSOT(result, consumedExcess);

        // ====================================================================
        // STEP 8: CLIMAX DETECTION (requires baselines)
        // ====================================================================
        if (totalVolume > 0.0 && sessionHigh > 0.0 && sessionLow > 0.0) {
            DetectClimax(result, high, low, totalVolume, barDelta,
                         sessionHigh, sessionLow, tickSize);
        }

        // ====================================================================
        // STEP 9: POPULATE AUCTION LEVEL CONTEXT (Jan 2025)
        // ====================================================================
        PopulateAuctionLevelContext(result, poc, vah, val, close, tickSize,
                                     priorPOC, priorVAH, priorVAL);

        // ====================================================================
        // STEP 10: FAILED AUCTION VA DETECTION
        // ====================================================================
        if (vah > 0.0 && val > 0.0) {
            DetectFailedAuctionVA(result, high, low, close, vah, val, tickSize, barIndex);
        }

        // ====================================================================
        // STEP 11: DETERMINE PRIMARY TYPE
        // ====================================================================
        ImbalanceType rawType = DetermineType(result);
        result.type = rawType;

        // ====================================================================
        // STEP 12: DETERMINE DIRECTION
        // ====================================================================
        result.direction = DetermineDirection(result, close, open, barDelta);

        // ====================================================================
        // STEP 13: DETERMINE CONVICTION
        // ====================================================================
        result.conviction = DetermineConviction(result, liqState, is1TF, barDelta, totalVolume);

        // Track conviction changes
        if (result.conviction != lastConviction && lastConviction != ConvictionType::UNKNOWN) {
            result.convictionChanged = true;
        }
        lastConviction = result.conviction;

        // ====================================================================
        // STEP 13.5: APPLY SPATIAL CONTEXT (Wall/Void Adjustment)
        // ====================================================================
        ApplySpatialContext(result,
                            nearestBidWallTicks, nearestAskWallTicks,
                            nearestBidVoidTicks, nearestAskVoidTicks,
                            pathOfLeastResistance);

        // ====================================================================
        // STEP 14: COMPUTE STRENGTH & CONFIDENCE
        // ====================================================================
        ComputeStrengthAndConfidence(result);

        // ====================================================================
        // STEP 15: APPLY HYSTERESIS
        // ====================================================================
        UpdateHysteresis(result, rawType);

        // ====================================================================
        // STEP 16: COMPUTE DISPLACEMENT SCORE
        // ====================================================================
        result.displacementScore = ComputeDisplacementScore(result, rotationFactor, is1TF);
        result.displacementTicks = std::abs(result.pocShiftTicks);

        // ====================================================================
        // STEP 17: SET VALIDITY
        // ====================================================================
        // If not already blocked by context gates, check warmup
        if (result.errorReason == ImbalanceErrorReason::NONE) {
            ImbalanceErrorReason warmupReason = CheckWarmupState();
            if (warmupReason != ImbalanceErrorReason::NONE) {
                result.errorReason = warmupReason;
            }
        }

        // Update session stats
        if (result.hasStackedImbalance) {
            if (result.stackedBuyLevels >= config.minStackedLevels) stackedBuyCount++;
            if (result.stackedSellLevels >= config.minStackedLevels) stackedSellCount++;
        }
        if (result.hasDeltaDivergence) divergenceCount++;
        if (result.absorptionDetected) absorptionCount++;

        return result;
    }

    // ========================================================================
    // SSOT-COMPLIANT COMPUTE (Jan 2025)
    // ========================================================================
    //
    // Preferred entry point. Consumes ValueLocationResult from ValueLocationEngine
    // instead of receiving raw POC/VAH/VAL values. This ensures:
    //   - Single source of truth for value-relative location
    //   - Consistent VA overlap and migration calculations
    //   - Pre-computed distances available for displacement detection
    //
    ImbalanceResult ComputeFromValueLocation(
        const ValueLocationResult& valLocResult,
        // Price data (required)
        double high, double low, double close, double open,
        double prevHigh, double prevLow, double prevClose,
        double tickSize, int barIndex,
        // Diagonal delta from Numbers Bars (optional, pass -1 if unavailable)
        double diagonalPosDelta = -1.0, double diagonalNegDelta = -1.0,
        // Volume/delta (optional, pass -1 if unavailable)
        double totalVolume = -1.0, double barDelta = -1.0, double cumulativeDelta = -1.0,
        // Context gates (optional)
        LiquidityState liqState = LiquidityState::LIQ_NOT_READY,
        VolatilityRegime volRegime = VolatilityRegime::UNKNOWN,
        double executionFriction = -1.0,
        // IB/Session context (optional)
        double ibHigh = 0.0, double ibLow = 0.0,
        double sessionHigh = 0.0, double sessionLow = 0.0,
        int rotationFactor = 0, bool is1TF = false,
        // DOM consumed depth (optional, from LiquidityEngine)
        double consumedBidMass = -1.0, double consumedAskMass = -1.0,
        double toxicityProxy = -1.0,
        // DOM spatial liquidity (optional, from LiquidityEngine.spatialGating)
        double nearestBidWallTicks = -1.0, double nearestAskWallTicks = -1.0,
        double nearestBidVoidTicks = -1.0, double nearestAskVoidTicks = -1.0,
        int pathOfLeastResistance = 0,  // -1=DOWN, 0=BALANCED, +1=UP
        // SSOT consumed excess (from ExcessDetector, Jan 2025)
        ExcessType consumedExcess = ExcessType::NONE,
        // Prior session levels (from DaltonEngine.SessionBridge, for AuctionLevelContext)
        double priorPOC = 0.0, double priorVAH = 0.0, double priorVAL = 0.0
    ) {
        // Extract POC/VAH/VAL from SSOT result
        // If valLocResult is not ready, we can still compute with zeros (optional inputs)
        double poc = 0.0, vah = 0.0, val = 0.0;
        double inputPrevPOC = 0.0, inputPrevVAH = 0.0, inputPrevVAL = 0.0;

        if (valLocResult.IsReady()) {
            // Derive prices from SSOT distance fields
            // POC = close - (distFromPOCTicks * tickSize)
            poc = close - (valLocResult.distFromPOCTicks * tickSize);
            vah = close - (valLocResult.distFromVAHTicks * tickSize);
            val = close - (valLocResult.distFromVALTicks * tickSize);

            // Prior levels from SSOT
            inputPrevPOC = close - (valLocResult.distToPriorPOCTicks * tickSize);
            inputPrevVAH = close - (valLocResult.distToPriorVAHTicks * tickSize);
            inputPrevVAL = close - (valLocResult.distToPriorVALTicks * tickSize);
        }

        // Suppress deprecation warning: SSOT method calling deprecated method is expected
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
        // Delegate to full Compute() with extracted values
        return Compute(
            high, low, close, open,
            prevHigh, prevLow, prevClose,
            tickSize, barIndex,
            poc, vah, val,
            inputPrevPOC, inputPrevVAH, inputPrevVAL,
            diagonalPosDelta, diagonalNegDelta,
            totalVolume, barDelta, cumulativeDelta,
            liqState, volRegime, executionFriction,
            ibHigh, ibLow, sessionHigh, sessionLow,
            rotationFactor, is1TF,
            consumedBidMass, consumedAskMass, toxicityProxy,
            nearestBidWallTicks, nearestAskWallTicks,
            nearestBidVoidTicks, nearestAskVoidTicks,
            pathOfLeastResistance,
            consumedExcess,
            priorPOC, priorVAH, priorVAL
        );
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    }

    // ========================================================================
    // SESSION BOUNDARY METHODS
    // ========================================================================

    void ResetForSession() {
        swingHighs.clear();
        swingLows.clear();
        lastHigh = 0.0;
        lastLow = 0.0;
        lastDelta = 0.0;
        lastSwingBar = 0;

        pocTracker.Reset();
        prevPOC = 0.0;
        prevVAH = 0.0;
        prevVAL = 0.0;

        failedAuctionTracking.Reset();

        confirmedType = ImbalanceType::NONE;
        candidateType = ImbalanceType::NONE;
        candidateConfirmationBars = 0;
        barsInConfirmedState = 0;
        lastConviction = ConvictionType::UNKNOWN;

        sessionBars = 0;
        stackedBuyCount = 0;
        stackedSellCount = 0;
        divergenceCount = 0;
        absorptionCount = 0;

        // Note: baselines are NOT reset - they carry forward
    }

    void Reset() {
        ResetForSession();
        for (int i = 0; i < EFFORT_BUCKET_COUNT; ++i) {
            diagonalNetBaseline[i].reset(config.baselineWindow);
            diagonalRatioBaseline[i].reset(config.baselineWindow);
            pocShiftBaseline[i].reset(config.baselineWindow);
            absorptionBaseline[i].reset(config.baselineWindow);
        }
    }

    // ========================================================================
    // PRE-WARM SUPPORT
    // ========================================================================

    void PreWarmFromBar(double diagonalNet, double pocShift, double absorptionScore,
                        SessionPhase phase = SessionPhase::MID_SESSION) {
        const int phaseIdx = SessionPhaseToBucketIndex(phase);
        if (phaseIdx < 0 || phaseIdx >= EFFORT_BUCKET_COUNT) {
            return;  // Invalid phase, skip
        }

        if (std::isfinite(diagonalNet)) {
            diagonalNetBaseline[phaseIdx].push(std::abs(diagonalNet));
        }
        if (std::isfinite(pocShift) && pocShift != 0.0) {
            pocShiftBaseline[phaseIdx].push(std::abs(pocShift));
        }
        if (std::isfinite(absorptionScore) && absorptionScore > 0.0) {
            absorptionBaseline[phaseIdx].push(absorptionScore);
        }
    }

    // ========================================================================
    // DIAGNOSTIC STATE
    // ========================================================================

    struct DiagnosticState {
        size_t diagonalBaselineSamples = 0;
        size_t pocShiftBaselineSamples = 0;
        size_t absorptionBaselineSamples = 0;
        int swingHighCount = 0;
        int swingLowCount = 0;
        ImbalanceType confirmedType = ImbalanceType::NONE;
        int barsInState = 0;
        int sessionBars = 0;
        int stackedBuyCount = 0;
        int stackedSellCount = 0;
        SessionPhase currentPhase = SessionPhase::UNKNOWN;
    };

    DiagnosticState GetDiagnosticState() const {
        DiagnosticState d;
        d.currentPhase = currentPhase;
        const int phaseIdx = SessionPhaseToBucketIndex(currentPhase);
        if (phaseIdx >= 0 && phaseIdx < EFFORT_BUCKET_COUNT) {
            d.diagonalBaselineSamples = diagonalNetBaseline[phaseIdx].size();
            d.pocShiftBaselineSamples = pocShiftBaseline[phaseIdx].size();
            d.absorptionBaselineSamples = absorptionBaseline[phaseIdx].size();
        }
        d.swingHighCount = static_cast<int>(swingHighs.size());
        d.swingLowCount = static_cast<int>(swingLows.size());
        d.confirmedType = confirmedType;
        d.barsInState = barsInConfirmedState;
        d.sessionBars = sessionBars;
        d.stackedBuyCount = stackedBuyCount;
        d.stackedSellCount = stackedSellCount;
        return d;
    }

private:
    // ========================================================================
    // CONTEXT GATE APPLICATION
    // ========================================================================

    ContextGateResult ApplyContextGates(
        LiquidityState liqState,
        VolatilityRegime volRegime,
        double executionFriction,
        int rotationFactor
    ) {
        ContextGateResult gate;
        gate.liqState = liqState;
        gate.volRegime = volRegime;
        gate.executionFriction = executionFriction >= 0.0 ? executionFriction : 1.0;
        gate.rotationFactor = rotationFactor;

        // Liquidity gate
        if (config.requireLiquidityGate) {
            if (liqState == LiquidityState::LIQ_NOT_READY) {
                gate.liquidityOK = true;  // Pass if not available (degraded mode)
            } else if (liqState == LiquidityState::LIQ_VOID) {
                gate.liquidityOK = false;
                if (config.blockOnVoid) {
                    gate.blockReason = ImbalanceErrorReason::BLOCKED_LIQUIDITY_VOID;
                }
            } else if (liqState == LiquidityState::LIQ_THIN && config.blockOnThin) {
                gate.liquidityOK = false;
                gate.blockReason = ImbalanceErrorReason::BLOCKED_LIQUIDITY_THIN;
            } else {
                gate.liquidityOK = true;
            }
        } else {
            gate.liquidityOK = true;
        }

        // Volatility gate
        if (config.requireVolatilityGate) {
            if (volRegime == VolatilityRegime::UNKNOWN) {
                gate.volatilityOK = true;  // Pass if not available
            } else if (volRegime == VolatilityRegime::EVENT && config.blockOnEvent) {
                gate.volatilityOK = false;
                if (gate.blockReason == ImbalanceErrorReason::NONE) {
                    gate.blockReason = ImbalanceErrorReason::BLOCKED_VOLATILITY_EVENT;
                }
            } else {
                gate.volatilityOK = true;
            }
        } else {
            gate.volatilityOK = true;
        }

        // Chop gate (set later in value migration, default to OK)
        gate.chopOK = true;

        gate.allGatesPass = gate.liquidityOK && gate.volatilityOK && gate.chopOK;
        return gate;
    }

    // ========================================================================
    // DIAGONAL IMBALANCE DETECTION
    // ========================================================================

    void DetectDiagonalImbalance(ImbalanceResult& result,
                                  double posDelta, double negDelta,
                                  double open, double close, int barIndex) {
        result.diagonalPosDelta = posDelta;
        result.diagonalNegDelta = negDelta;
        result.diagonalNetDelta = posDelta - negDelta;

        const double total = posDelta + negDelta;
        if (total > 0.0) {
            result.diagonalRatio = posDelta / total;
        } else {
            result.diagonalRatio = 0.5;
        }

        // Compute buy/sell ratios
        // Buy ratio > 1 = more buying aggression (pos delta stronger)
        // Sell ratio > 1 = more selling aggression (neg delta stronger)
        const double buyRatio = (negDelta > 0.0) ? (posDelta / negDelta) : 999.0;
        const double sellRatio = (posDelta > 0.0) ? (negDelta / posDelta) : 999.0;
        const double maxRatio = (std::max)(buyRatio, sellRatio);

        // Push to phase-aware baselines
        const int phaseIdx = SessionPhaseToBucketIndex(currentPhase);
        bool usePercentile = false;
        double ratioPercentile = 0.0;

        if (phaseIdx >= 0 && phaseIdx < EFFORT_BUCKET_COUNT) {
            // Push net delta magnitude to baseline
            diagonalNetBaseline[phaseIdx].push(std::abs(result.diagonalNetDelta));

            // Push the dominant ratio to baseline (for percentile-based detection)
            if (maxRatio < 900.0) {  // Don't push extreme values (999.0 = one side zero)
                diagonalRatioBaseline[phaseIdx].push(maxRatio);
            }

            // Calculate net delta percentile
            if (diagonalNetBaseline[phaseIdx].size() >= config.baselineMinSamples) {
                auto pctile = diagonalNetBaseline[phaseIdx].TryPercentile(std::abs(result.diagonalNetDelta));
                if (pctile.valid) {
                    // SSOT Invariant: Percentiles must be in [0, 100]
                    AMT_SSOT_ASSERT_RANGE(pctile.value, 0.0, 100.0, "IMB diagonalPercentile");
                    result.diagonalPercentile = pctile.value;
                }
            }

            // Calculate ratio percentile for adaptive thresholding
            if (config.usePercentileBasedRatios &&
                diagonalRatioBaseline[phaseIdx].size() >= config.baselineMinSamples) {
                auto pctile = diagonalRatioBaseline[phaseIdx].TryPercentile(maxRatio);
                if (pctile.valid) {
                    // SSOT Invariant: Percentiles must be in [0, 100]
                    AMT_SSOT_ASSERT_RANGE(pctile.value, 0.0, 100.0, "IMB ratioPercentile");
                    usePercentile = true;
                    ratioPercentile = pctile.value;
                }
            }
        }

        // ====================================================================
        // IMBALANCE DETECTION (adaptive vs fixed thresholds)
        // ====================================================================
        bool hasBuyImbalance = false;
        bool hasSellImbalance = false;
        bool hasBigImbalance = false;

        if (usePercentile) {
            // PERCENTILE-BASED: Compare ratio to historical distribution
            // This adapts to the instrument and market conditions
            hasBuyImbalance = (buyRatio > sellRatio) &&
                               (ratioPercentile >= config.diagonalRatioPctileThreshold);
            hasSellImbalance = (sellRatio > buyRatio) &&
                                (ratioPercentile >= config.diagonalRatioPctileThreshold);
            hasBigImbalance = (ratioPercentile >= config.bigImbalancePctileThreshold);
        } else {
            // FIXED THRESHOLDS: Fallback when baseline not ready
            hasBuyImbalance = (buyRatio >= config.diagonalRatioThreshold);
            hasSellImbalance = (sellRatio >= config.diagonalRatioThreshold);
            hasBigImbalance = (buyRatio >= config.bigImbalanceThreshold ||
                                sellRatio >= config.bigImbalanceThreshold);
        }

        // For stacked imbalance, we track consecutive bars with imbalance
        if (hasBuyImbalance) {
            result.stackedBuyLevels = (std::max)(1, result.stackedBuyLevels + 1);
            result.stackedSellLevels = 0;
        } else if (hasSellImbalance) {
            result.stackedSellLevels = (std::max)(1, result.stackedSellLevels + 1);
            result.stackedBuyLevels = 0;
        } else {
            result.stackedBuyLevels = 0;
            result.stackedSellLevels = 0;
        }

        result.hasStackedImbalance = (result.stackedBuyLevels >= config.minStackedLevels ||
                                       result.stackedSellLevels >= config.minStackedLevels);
        result.hasBigImbalance = hasBigImbalance;

        // Check for trapped traders
        // Buy imbalances in a red (down) bar = trapped longs
        // Sell imbalances in a green (up) bar = trapped shorts
        const bool isUpBar = close > open;
        const bool isDownBar = close < open;

        if (result.stackedBuyLevels >= config.minStackedLevels && isDownBar) {
            result.trappedTradersDetected = true;
            result.trappedLongs = true;
        }
        if (result.stackedSellLevels >= config.minStackedLevels && isUpBar) {
            result.trappedTradersDetected = true;
            result.trappedShorts = true;
        }
    }

    // ========================================================================
    // DELTA DIVERGENCE DETECTION
    // ========================================================================

    void DetectDeltaDivergence(ImbalanceResult& result,
                                double high, double low, double cumDelta,
                                double prevHigh, double prevLow,
                                double tickSize, int barIndex) {
        // Update swing tracking
        const bool newSwingHigh = (high > lastHigh && barIndex > lastSwingBar + config.minSwingBars);
        const bool newSwingLow = (low < lastLow && barIndex > lastSwingBar + config.minSwingBars);

        if (newSwingHigh) {
            SwingPoint sp;
            sp.price = high;
            sp.delta = cumDelta;
            sp.barIndex = barIndex;
            sp.isHigh = true;
            sp.valid = true;
            swingHighs.push_back(sp);

            // Keep limited history
            while (swingHighs.size() > 10) {
                swingHighs.erase(swingHighs.begin());
            }

            lastHigh = high;
            lastSwingBar = barIndex;
        }

        if (newSwingLow) {
            SwingPoint sp;
            sp.price = low;
            sp.delta = cumDelta;
            sp.barIndex = barIndex;
            sp.isHigh = false;
            sp.valid = true;
            swingLows.push_back(sp);

            while (swingLows.size() > 10) {
                swingLows.erase(swingLows.begin());
            }

            lastLow = low;
            lastSwingBar = barIndex;
        }

        lastDelta = cumDelta;

        // Check for divergence at current price vs recent swings
        // Bearish divergence: price higher high, delta lower high
        if (swingHighs.size() >= 2) {
            const auto& prev = swingHighs[swingHighs.size() - 2];
            const auto& curr = swingHighs[swingHighs.size() - 1];

            const double priceChange = (curr.price - prev.price) / tickSize;
            const double deltaChange = curr.delta - prev.delta;

            if (priceChange > config.divergenceMinTicks && deltaChange < 0) {
                result.hasDeltaDivergence = true;
                result.divergenceBearish = true;
                result.divergenceStrength = (std::min)(1.0, std::abs(deltaChange) / 1000.0);
            }
        }

        // Bullish divergence: price lower low, delta higher low
        if (swingLows.size() >= 2) {
            const auto& prev = swingLows[swingLows.size() - 2];
            const auto& curr = swingLows[swingLows.size() - 1];

            const double priceChange = (prev.price - curr.price) / tickSize;  // Lower = positive
            const double deltaChange = curr.delta - prev.delta;

            if (priceChange > config.divergenceMinTicks && deltaChange > 0) {
                result.hasDeltaDivergence = true;
                result.divergenceBullish = true;
                result.divergenceStrength = (std::min)(1.0, std::abs(deltaChange) / 1000.0);
            }
        }

        // Initialize tracking if needed
        if (lastHigh == 0.0) lastHigh = high;
        if (lastLow == 0.0) lastLow = low;
    }

    // ========================================================================
    // ABSORPTION DETECTION
    // ========================================================================

    void DetectAbsorption(ImbalanceResult& result,
                           double high, double low, double volume, double delta,
                           double tickSize,
                           double consumedBidMass = -1.0,
                           double consumedAskMass = -1.0,
                           double toxicityProxy = -1.0) {
        // AMT ABSORPTION DEFINITION (Dalton/Steidlmayer):
        // One side AGGRESSIVELY attacks (extreme delta) while the other side
        // PASSIVELY absorbs (price doesn't move = narrow range despite high volume).
        //
        // This is NOT "delta near zero" - it's delta EXTREME with no price result.
        // Example: Strong selling (delta -30%) hits passive bids, price barely moves.
        //          The bid side is ABSORBING the aggressive selling.
        //
        // DOM CONFIRMATION (if available):
        // - High consumed depth = liquidity was absorbed
        // - Consumed on opposite side to delta direction = confirms absorption
        // - High toxicity = asymmetric consumption = informed absorption

        const double range = (high - low) / tickSize;
        const double deltaPct = (volume > 0.0) ? delta / volume : 0.0;
        const double absDeltaPct = std::abs(deltaPct);

        // Absorption requires EXTREME one-sided delta (>= 30% threshold)
        const bool hasExtremeDelta = (absDeltaPct >= config.absorptionDeltaThreshold);

        // Narrow range component (price didn't move despite aggression)
        // Score 1.0 at 0 ticks, 0.0 at 10+ ticks
        const double rangeScore = (std::max)(0.0, 1.0 - (range / 10.0));

        // Delta extremeness score (higher = more one-sided aggression)
        // Maps 0.30 -> 0.0, 0.70+ -> 1.0
        const double deltaExtremeScore = (std::max)(0.0, (std::min)(1.0,
            (absDeltaPct - config.absorptionDeltaThreshold) / 0.40));

        // ====================================================================
        // DOM CONSUMED DEPTH CONFIRMATION
        // ====================================================================
        // Store DOM context in result
        const bool hasDOMData = (consumedBidMass >= 0.0 && consumedAskMass >= 0.0);
        if (hasDOMData) {
            result.hasDOMContext = true;
            result.consumedBidMass = consumedBidMass;
            result.consumedAskMass = consumedAskMass;
            result.consumedTotalMass = consumedBidMass + consumedAskMass;
            if (toxicityProxy >= 0.0) {
                result.toxicityProxy = toxicityProxy;
            }
        }

        // DOM confirmation: consumed depth on the absorbing side
        // - Negative delta = sells hit bids = consumed BID depth
        // - Positive delta = buys hit asks = consumed ASK depth
        double domAbsorptionBonus = 0.0;
        if (hasDOMData && hasExtremeDelta) {
            const double relevantConsumed = (deltaPct < 0)
                ? consumedBidMass   // Sells absorbed by bids
                : consumedAskMass;  // Buys absorbed by asks

            // High consumed depth on the absorbing side confirms absorption
            // Use relative consumption (consumed / (consumed + remaining))
            const double totalConsumed = consumedBidMass + consumedAskMass;
            if (totalConsumed > 0.0) {
                const double consumedRatio = relevantConsumed / totalConsumed;
                // Bonus if more consumed on the absorbing side
                if (consumedRatio > 0.5) {
                    domAbsorptionBonus = (consumedRatio - 0.5) * 0.4;  // Up to 0.2 bonus
                    result.highConsumedDepth = true;
                }
            }

            // Toxicity confirmation (asymmetric consumption = informed absorption)
            if (toxicityProxy >= config.toxicityThreshold) {
                domAbsorptionBonus += 0.1;  // Additional bonus for toxic flow
            }
        }

        // Absorption score: narrow range + extreme delta + DOM confirmation
        // Both components must be present for meaningful absorption
        double absorptionScore = 0.0;
        if (hasExtremeDelta && range > 0.0 && volume > 0.0) {
            // Combine: narrow range (resistance to movement) + delta extremeness (aggression)
            absorptionScore = rangeScore * 0.5 + deltaExtremeScore * 0.3 + domAbsorptionBonus;
            // Clamp to [0, 1]
            absorptionScore = (std::min)(1.0, absorptionScore);
        }

        // Push to phase-aware baseline
        const int phaseIdx = SessionPhaseToBucketIndex(currentPhase);
        if (phaseIdx >= 0 && phaseIdx < EFFORT_BUCKET_COUNT && absorptionScore > 0.0) {
            absorptionBaseline[phaseIdx].push(absorptionScore);
        }

        result.absorptionScore = absorptionScore;
        result.absorptionDeltaPct = deltaPct;  // Store for diagnostics

        // Detect absorption when:
        // 1. Extreme delta present (one-sided aggression)
        // 2. Narrow range (price didn't move)
        // 3. Score exceeds baseline threshold (unusual absorption event)
        // 4. OR DOM confirms high consumed depth (additional path)
        const bool domConfirmed = (hasDOMData && result.highConsumedDepth);

        if (hasExtremeDelta && (rangeScore >= 0.5 || domConfirmed)) {
            if (phaseIdx >= 0 && phaseIdx < EFFORT_BUCKET_COUNT &&
                absorptionBaseline[phaseIdx].size() >= config.baselineMinSamples) {
                auto pctile = absorptionBaseline[phaseIdx].TryPercentile(absorptionScore);
                // Lower threshold if DOM confirms (75 -> 65)
                const double effectiveThreshold = domConfirmed
                    ? (config.absorptionVolumeThreshold - 10.0)
                    : config.absorptionVolumeThreshold;
                if (pctile.valid && pctile.value >= effectiveThreshold) {
                    result.absorptionDetected = true;

                    // Determine absorbing side (OPPOSITE to delta direction):
                    // - Negative delta = aggressive SELLING, absorbed by passive BIDS
                    // - Positive delta = aggressive BUYING, absorbed by passive ASKS
                    if (deltaPct < 0) {
                        result.absorptionBidSide = true;  // Passive BIDS absorbing aggressive sells
                    } else {
                        result.absorptionAskSide = true;  // Passive ASKS absorbing aggressive buys
                    }
                }
            } else if (absorptionScore >= 0.7 || (domConfirmed && absorptionScore >= 0.5)) {
                // Strong absorption signal even without baseline (early session)
                // Lower threshold if DOM confirms
                result.absorptionDetected = true;
                if (deltaPct < 0) {
                    result.absorptionBidSide = true;
                } else {
                    result.absorptionAskSide = true;
                }
            }
        }
    }

    // ========================================================================
    // VALUE MIGRATION
    // ========================================================================

    void ComputeValueMigration(ImbalanceResult& result,
                                double poc, double vah, double val,
                                double pPOC, double pVAH, double pVAL,
                                double tickSize, int barIndex) {
        // POC shift
        if (pPOC > 0.0) {
            result.pocShiftTicks = (poc - pPOC) / tickSize;

            // Track POC stability
            const double shiftMagnitude = std::abs(result.pocShiftTicks);
            if (shiftMagnitude < 1.0) {
                pocTracker.stableCount++;
            } else {
                if (pocTracker.stableCount >= config.pocStabilityBars) {
                    pocTracker.anchorPrice = pocTracker.currentPrice;
                }
                pocTracker.stableCount = 0;
            }
            pocTracker.currentPrice = poc;
            pocTracker.barIndex = barIndex;
            pocTracker.valid = true;

            result.pocMigrating = (shiftMagnitude >= config.pocShiftMinTicks);

            // Push to phase-aware baseline
            const int phaseIdx = SessionPhaseToBucketIndex(currentPhase);
            if (phaseIdx >= 0 && phaseIdx < EFFORT_BUCKET_COUNT && shiftMagnitude > 0.0) {
                pocShiftBaseline[phaseIdx].push(shiftMagnitude);
            }

            // Calculate percentile from phase-aware baseline
            if (phaseIdx >= 0 && phaseIdx < EFFORT_BUCKET_COUNT) {
                if (pocShiftBaseline[phaseIdx].size() >= config.baselineMinSamples) {
                    auto pctile = pocShiftBaseline[phaseIdx].TryPercentile(shiftMagnitude);
                    if (pctile.valid) {
                        result.pocShiftPercentile = pctile.value;
                    }
                }
            }
        }

        // VA overlap calculation
        if (pVAH > 0.0 && pVAL > 0.0 && vah > val) {
            // Calculate overlap between current VA and previous VA
            const double overlapHigh = (std::min)(vah, pVAH);
            const double overlapLow = (std::max)(val, pVAL);
            const double overlapRange = (std::max)(0.0, overlapHigh - overlapLow);

            const double currentVARange = vah - val;
            const double prevVARange = pVAH - pVAL;
            const double avgVARange = (currentVARange + prevVARange) / 2.0;

            if (avgVARange > 0.0) {
                result.vaOverlapPct = overlapRange / avgVARange;
                result.vaOverlapPct = (std::min)(1.0, (std::max)(0.0, result.vaOverlapPct));
            }

            // Update context gate VA overlap
            result.contextGate.vaOverlapPct = result.vaOverlapPct;

            // Check for chop (high rotation + high overlap)
            if (result.vaOverlapPct > config.vaOverlapHighThreshold &&
                std::abs(result.contextGate.rotationFactor) < config.chopRotationThreshold) {
                result.contextGate.chopOK = false;
                if (result.contextGate.blockReason == ImbalanceErrorReason::NONE) {
                    // Don't block on chop by default, just flag it
                    // result.contextGate.blockReason = ImbalanceErrorReason::BLOCKED_CHOP;
                }
            }

            // Determine value migration type
            if (result.vaOverlapPct > config.vaOverlapHighThreshold) {
                result.valueMigration = ValueMigration::OVERLAPPING;
            } else if (result.vaOverlapPct < config.vaOverlapLowThreshold) {
                // Extension day
                if (poc > pPOC) {
                    result.valueMigration = ValueMigration::HIGHER;
                } else {
                    result.valueMigration = ValueMigration::LOWER;
                }
            } else {
                result.valueMigration = ValueMigration::INSIDE;
            }
        }
    }

    // ========================================================================
    // RANGE EXTENSION DETECTION
    // ========================================================================

    void DetectRangeExtension(ImbalanceResult& result,
                               double high, double low,
                               double ibHigh, double ibLow,
                               double sessionHigh, double sessionLow,
                               bool is1TF) {
        const double ibRange = ibHigh - ibLow;
        const double sessionRange = sessionHigh - sessionLow;

        if (ibRange > 0.0) {
            result.extensionRatio = sessionRange / ibRange;

            result.extensionAboveIB = (sessionHigh > ibHigh);
            result.extensionBelowIB = (sessionLow < ibLow);

            // Range extension = broke IB + 1TF pattern (conviction)
            if ((result.extensionAboveIB || result.extensionBelowIB) &&
                result.extensionRatio > 1.5 && is1TF) {
                result.rangeExtensionDetected = true;
            }
        }
    }

    // ========================================================================
    // CONSUME EXCESS FROM SSOT (Jan 2025)
    // ========================================================================
    // Maps ExcessType from ExcessDetector (SSOT) to ImbalanceResult fields.
    // This replaces the deprecated DetectExcess() and DetectPoorHighLow() methods.
    // ========================================================================

    void ConsumeExcessFromSSOT(ImbalanceResult& result, ExcessType consumedExcess) {
        // Store the consumed excess in levels context
        result.levels.consumedExcess = consumedExcess;

        // Map to result fields for backward compatibility
        switch (consumedExcess) {
            case ExcessType::EXCESS_HIGH:
                result.excessDetected = true;
                result.excessHigh = true;
                break;
            case ExcessType::EXCESS_LOW:
                result.excessDetected = true;
                result.excessLow = true;
                break;
            case ExcessType::POOR_HIGH:
                result.poorHighDetected = true;
                result.poorHighScore = 0.6;  // Default score for SSOT-detected poor high
                break;
            case ExcessType::POOR_LOW:
                result.poorLowDetected = true;
                result.poorLowScore = 0.6;  // Default score for SSOT-detected poor low
                break;
            case ExcessType::NONE:
            default:
                // No excess detected
                break;
        }
    }

    // ========================================================================
    // POPULATE AUCTION LEVEL CONTEXT (Jan 2025)
    // ========================================================================
    // Computes acceptance/failure levels and auction objectives based on
    // current imbalance state and profile context.
    // ========================================================================

    void PopulateAuctionLevelContext(ImbalanceResult& result,
                                      double poc, double vah, double val,
                                      double close, double tickSize,
                                      double priorPOC, double priorVAH, double priorVAL) {
        AuctionLevelContext& ctx = result.levels;

        // Store prior levels if available
        if (priorPOC > 0.0 || priorVAH > 0.0 || priorVAL > 0.0) {
            ctx.priorPOC = priorPOC;
            ctx.priorVAH = priorVAH;
            ctx.priorVAL = priorVAL;
            ctx.priorLevelsValid = (priorPOC > 0.0 && priorVAH > 0.0 && priorVAL > 0.0);
        }

        // Can't compute levels without current profile
        if (vah <= 0.0 || val <= 0.0 || poc <= 0.0) return;

        const bool aboveVAH = (close > vah);
        const bool belowVAL = (close < val);

        if (aboveVAH) {
            // Price above value: acceptance requires sustained move
            // Failure = return to VAH (re-entry into value)
            ctx.failureLevel = vah;
            ctx.failureLevelValid = true;

            // Acceptance = POC migration toward price (future target)
            // Use prior POC + some distance as acceptance level
            ctx.acceptanceLevel = vah + (vah - poc) * 0.5;  // Extension target
            ctx.acceptanceLevelValid = true;

            // Objective = prior VAH or extension target
            if (ctx.priorLevelsValid && priorVAH > vah) {
                ctx.auctionObjective = priorVAH;
            } else {
                ctx.auctionObjective = vah + (vah - val);  // Full VA range extension
            }
            ctx.auctionObjectiveValid = true;

        } else if (belowVAL) {
            // Price below value: acceptance requires sustained move
            // Failure = return to VAL (re-entry into value)
            ctx.failureLevel = val;
            ctx.failureLevelValid = true;

            // Acceptance = POC migration toward price
            ctx.acceptanceLevel = val - (poc - val) * 0.5;
            ctx.acceptanceLevelValid = true;

            // Objective = prior VAL or extension target
            if (ctx.priorLevelsValid && priorVAL < val) {
                ctx.auctionObjective = priorVAL;
            } else {
                ctx.auctionObjective = val - (vah - val);  // Full VA range extension
            }
            ctx.auctionObjectiveValid = true;

        } else {
            // Inside value: no strong directional objective
            // Failure levels are both edges
            ctx.failureLevel = poc;  // Failure to break POC
            ctx.failureLevelValid = false;  // Not meaningful inside value
        }
    }

    // ========================================================================
    // CLIMAX DETECTION (Exhaustion at Extremes)
    // ========================================================================

    void DetectClimax(ImbalanceResult& result,
                       double high, double low, double volume, double delta,
                       double sessionHigh, double sessionLow, double tickSize) {
        // CLIMAX = Extreme volume + Extreme delta + At session extreme
        // Indicates exhaustion/capitulation - often marks turning points
        //
        // AMT Definition: The final aggressive push where one side is being
        // "forced out" (liquidation) or the other side is "climaxing" their
        // conviction. Price often reverses after climax.

        if (volume <= 0.0 || sessionHigh <= 0.0 || sessionLow <= 0.0) return;

        const int phaseIdx = SessionPhaseToBucketIndex(currentPhase);
        if (phaseIdx < 0 || phaseIdx >= EFFORT_BUCKET_COUNT) return;
        if (effortStore == nullptr) return;

        // Get volume and delta percentiles from phase-aware baselines
        const auto& bucket = effortStore->Get(currentPhase);
        const auto& volBaseline = bucket.vol_sec;
        const auto& deltaBaseline = bucket.delta_pct;

        if (volBaseline.size() < config.baselineMinSamples ||
            deltaBaseline.size() < config.baselineMinSamples) return;

        // Compute percentiles
        const double volPerSec = volume / 60.0;  // Approximate per-second rate
        auto volPctile = volBaseline.TryPercentile(volPerSec);
        if (!volPctile.valid) return;

        const double deltaPct = (volume > 0.0) ? delta / volume : 0.0;
        auto deltaPctile = deltaBaseline.TryPercentile(std::abs(deltaPct));
        if (!deltaPctile.valid) return;

        result.volumePercentile = volPctile.value;
        result.deltaPercentile = deltaPctile.value;

        // Check proximity to session extremes
        const double distToHigh = (sessionHigh - high) / tickSize;
        const double distToLow = (low - sessionLow) / tickSize;
        const bool nearSessionHigh = (distToHigh <= config.extremeProximityTicks);
        const bool nearSessionLow = (distToLow <= config.extremeProximityTicks);

        // CLIMAX requires: extreme volume + extreme delta + at extreme
        const bool hasExtremeVol = (volPctile.value >= config.climaxVolumeThreshold);
        const bool hasExtremeDelta = (deltaPctile.value >= config.climaxDeltaThreshold);

        if (hasExtremeVol && hasExtremeDelta) {
            // Score based on how extreme (normalize to [0,1])
            const double volScore = (volPctile.value - config.climaxVolumeThreshold) /
                                     (100.0 - config.climaxVolumeThreshold);
            const double deltaScore = (deltaPctile.value - config.climaxDeltaThreshold) /
                                       (100.0 - config.climaxDeltaThreshold);
            result.climaxScore = (volScore + deltaScore) / 2.0;

            if (nearSessionHigh && delta > 0) {
                // Buying climax at highs - exhaustion, expect reversal down
                result.climaxDetected = true;
                result.climaxHigh = true;
            } else if (nearSessionLow && delta < 0) {
                // Selling climax at lows - capitulation, expect reversal up
                result.climaxDetected = true;
                result.climaxLow = true;
            }
        }
    }

    // ========================================================================
    // POOR HIGH/LOW DETECTION - REMOVED (Jan 2025)
    // ========================================================================
    // This functionality is now provided by ExcessDetector (AMT_Signals.h).
    // ImbalanceEngine CONSUMES the SSOT via ConsumeExcessFromSSOT().
    // See ExcessType::POOR_HIGH and ExcessType::POOR_LOW.
    // ========================================================================

    // ========================================================================
    // FAILED AUCTION VA DETECTION (Breakout Trap at VA Boundary)
    // ========================================================================
    // NOTE: This is the VA-boundary failed auction, distinct from IB failed
    // auction which is owned by DaltonEngine (failedAuctionAbove/Below).
    // ========================================================================

    void DetectFailedAuctionVA(ImbalanceResult& result,
                              double high, double low, double close,
                              double vah, double val, double tickSize,
                              int barIndex) {
        // FAILED AUCTION = Price breaks out of value, then rapidly returns
        // This is a classic trap - traders who chased the breakout are now
        // underwater. The rapid return signals lack of acceptance.
        //
        // Detection: Track when price first leaves value, count bars outside,
        // detect if price returns quickly (within maxBars)

        if (vah <= 0.0 || val <= 0.0) return;

        const bool currentlyAboveVA = (close > vah);
        const bool currentlyBelowVA = (close < val);
        const bool currentlyInValue = (!currentlyAboveVA && !currentlyBelowVA);

        // Track breakout state
        if (!failedAuctionTracking.active) {
            // Start tracking if price just left value
            if (currentlyAboveVA || currentlyBelowVA) {
                failedAuctionTracking.active = true;
                failedAuctionTracking.breakoutBar = barIndex;
                failedAuctionTracking.brokeAbove = currentlyAboveVA;
                failedAuctionTracking.barsOutside = 1;
            }
        } else {
            // Update tracking
            if (currentlyInValue) {
                // Returned to value - check if it's a failed auction
                const int barsOutside = failedAuctionTracking.barsOutside;
                if (barsOutside > 0 && barsOutside <= config.failedAuctionMaxBars) {
                    // Rapid return = failed auction
                    result.failedAuctionDetected = true;
                    result.failedBreakoutAbove = failedAuctionTracking.brokeAbove;
                    result.failedBreakoutBelow = !failedAuctionTracking.brokeAbove;
                    result.barsOutside = barsOutside;

                    // Score based on how quickly it returned (fewer bars = worse trap)
                    result.failedAuctionScore = 1.0 - (static_cast<double>(barsOutside) /
                                                        (config.failedAuctionMaxBars + 1));
                }
                // Reset tracking
                failedAuctionTracking.Reset();
            } else if ((failedAuctionTracking.brokeAbove && currentlyAboveVA) ||
                       (!failedAuctionTracking.brokeAbove && currentlyBelowVA)) {
                // Still outside in same direction
                failedAuctionTracking.barsOutside++;

                // If too many bars, it's acceptance not failure
                if (failedAuctionTracking.barsOutside > config.failedAuctionLookback) {
                    failedAuctionTracking.Reset();
                }
            } else {
                // Changed direction outside value - reset
                failedAuctionTracking.Reset();
            }
        }
    }

    // ========================================================================
    // DETERMINE PRIMARY TYPE
    // ========================================================================

    ImbalanceType DetermineType(const ImbalanceResult& result) {
        // Priority order based on signal strength and actionability

        // 1. Failed auction VA (highest priority - active trap in progress)
        if (result.failedAuctionDetected) {
            return ImbalanceType::FAILED_AUCTION_VA;
        }

        // 2. Climax (exhaustion at extremes - important reversal signal)
        if (result.climaxDetected) {
            return result.climaxHigh ? ImbalanceType::CLIMAX_HIGH
                                      : ImbalanceType::CLIMAX_LOW;
        }

        // 3. Excess (strong auction rejection)
        if (result.excessHigh) {
            return ImbalanceType::EXCESS;
        }
        if (result.excessLow) {
            return ImbalanceType::EXCESS;
        }

        // 4. Trapped traders (very actionable)
        if (result.trappedLongs) {
            return ImbalanceType::TRAPPED_LONGS;
        }
        if (result.trappedShorts) {
            return ImbalanceType::TRAPPED_SHORTS;
        }

        // 5. Range extension (structural signal)
        if (result.rangeExtensionDetected) {
            return ImbalanceType::RANGE_EXTENSION;
        }

        // 6. Stacked imbalance (footprint signal)
        if (result.hasStackedImbalance) {
            if (result.stackedBuyLevels >= config.minStackedLevels) {
                return ImbalanceType::STACKED_BUY;
            }
            if (result.stackedSellLevels >= config.minStackedLevels) {
                return ImbalanceType::STACKED_SELL;
            }
        }

        // 7. Delta divergence (reversal signal)
        if (result.hasDeltaDivergence) {
            return ImbalanceType::DELTA_DIVERGENCE;
        }

        // 8. Absorption (support/resistance)
        if (result.absorptionDetected) {
            if (result.absorptionBidSide) {
                return ImbalanceType::ABSORPTION_BID;
            }
            if (result.absorptionAskSide) {
                return ImbalanceType::ABSORPTION_ASK;
            }
        }

        // 9. Poor high/low (weaker signal - unfinished business)
        if (result.poorHighDetected) {
            return ImbalanceType::POOR_HIGH;
        }
        if (result.poorLowDetected) {
            return ImbalanceType::POOR_LOW;
        }

        // 10. Value migration (slowest signal)
        if (result.pocMigrating) {
            return ImbalanceType::VALUE_MIGRATION;
        }

        return ImbalanceType::NONE;
    }

    // ========================================================================
    // DETERMINE DIRECTION
    // ========================================================================

    ImbalanceDirection DetermineDirection(const ImbalanceResult& result,
                                           double close, double open, double delta) {
        switch (result.type) {
            case ImbalanceType::STACKED_BUY:
            case ImbalanceType::ABSORPTION_BID:
            case ImbalanceType::TRAPPED_SHORTS:
            case ImbalanceType::CLIMAX_LOW:   // Selling exhausted at lows -> bullish
            case ImbalanceType::POOR_LOW:     // Weak low -> expect revisit from above
                return ImbalanceDirection::BULLISH;

            case ImbalanceType::STACKED_SELL:
            case ImbalanceType::ABSORPTION_ASK:
            case ImbalanceType::TRAPPED_LONGS:
            case ImbalanceType::CLIMAX_HIGH:  // Buying exhausted at highs -> bearish
            case ImbalanceType::POOR_HIGH:    // Weak high -> expect revisit from below
                return ImbalanceDirection::BEARISH;

            case ImbalanceType::FAILED_AUCTION_VA:
                // Direction is opposite to the failed breakout at VA boundary
                return result.failedBreakoutAbove ? ImbalanceDirection::BEARISH
                                                   : ImbalanceDirection::BULLISH;

            case ImbalanceType::EXCESS:
                return result.excessHigh ? ImbalanceDirection::BEARISH
                                         : ImbalanceDirection::BULLISH;

            case ImbalanceType::DELTA_DIVERGENCE:
                return result.divergenceBullish ? ImbalanceDirection::BULLISH
                                                : ImbalanceDirection::BEARISH;

            case ImbalanceType::VALUE_MIGRATION:
                if (result.valueMigration == ValueMigration::HIGHER) {
                    return ImbalanceDirection::BULLISH;
                }
                if (result.valueMigration == ValueMigration::LOWER) {
                    return ImbalanceDirection::BEARISH;
                }
                return ImbalanceDirection::NEUTRAL;

            case ImbalanceType::RANGE_EXTENSION:
                if (result.extensionAboveIB && !result.extensionBelowIB) {
                    return ImbalanceDirection::BULLISH;
                }
                if (result.extensionBelowIB && !result.extensionAboveIB) {
                    return ImbalanceDirection::BEARISH;
                }
                // Both extended = use delta or bar direction
                return (delta > 0 || close > open) ? ImbalanceDirection::BULLISH
                                                   : ImbalanceDirection::BEARISH;

            default:
                return ImbalanceDirection::NEUTRAL;
        }
    }

    // ========================================================================
    // DETERMINE CONVICTION
    // ========================================================================

    ConvictionType DetermineConviction(const ImbalanceResult& result,
                                        LiquidityState liqState,
                                        bool is1TF, double delta, double volume) {
        // Liquidation: High stress + forced selling
        if (liqState == LiquidityState::LIQ_VOID ||
            (result.contextGate.executionFriction > 0.8)) {
            return ConvictionType::LIQUIDATION;
        }

        // Initiative indicators:
        // - 1TF pattern (directional conviction)
        // - Stacked imbalances (aggressive pressure)
        // - Range extension (breakout)
        // - Delta confirming direction
        bool isInitiative = false;

        if (is1TF) isInitiative = true;

        if (result.hasStackedImbalance) isInitiative = true;

        if (result.rangeExtensionDetected) isInitiative = true;

        // Delta confirmation
        if (volume > 0.0) {
            const double deltaRatio = delta / volume;
            if (result.direction == ImbalanceDirection::BULLISH && deltaRatio > 0.3) {
                isInitiative = true;
            }
            if (result.direction == ImbalanceDirection::BEARISH && deltaRatio < -0.3) {
                isInitiative = true;
            }
        }

        // Responsive indicators:
        // - Absorption detected
        // - Delta divergence
        // - Excess (rejection)
        // - Trapped traders
        if (result.absorptionDetected) isInitiative = false;
        if (result.hasDeltaDivergence) isInitiative = false;
        if (result.excessDetected) isInitiative = false;
        if (result.trappedTradersDetected) isInitiative = false;

        return isInitiative ? ConvictionType::INITIATIVE : ConvictionType::RESPONSIVE;
    }

    // ========================================================================
    // APPLY SPATIAL CONTEXT (Wall/Void Conviction Adjustment)
    // ========================================================================
    // Evaluates spatial liquidity from LiquidityEngine to adjust conviction:
    // - Wall in signal direction = REDUCE conviction (blocked)
    // - Void in signal direction = INCREASE conviction (acceleration)
    // - POLR aligned with signal = BOOST conviction
    //
    void ApplySpatialContext(ImbalanceResult& result,
                             double nearestBidWallTicks, double nearestAskWallTicks,
                             double nearestBidVoidTicks, double nearestAskVoidTicks,
                             int polr) {
        // Store raw inputs
        result.nearestBidWallTicks = nearestBidWallTicks;
        result.nearestAskWallTicks = nearestAskWallTicks;
        result.nearestBidVoidTicks = nearestBidVoidTicks;
        result.nearestAskVoidTicks = nearestAskVoidTicks;
        result.pathOfLeastResistance = polr;

        // Check if spatial data is valid (positive tick values indicate presence)
        const bool hasBidWall = (nearestBidWallTicks >= 0.0);
        const bool hasAskWall = (nearestAskWallTicks >= 0.0);
        const bool hasBidVoid = (nearestBidVoidTicks >= 0.0);
        const bool hasAskVoid = (nearestAskVoidTicks >= 0.0);

        result.hasSpatialContext = (hasBidWall || hasAskWall || hasBidVoid || hasAskVoid || polr != 0);

        if (!result.hasSpatialContext || !config.useSpatialConviction) {
            return;  // No spatial data or feature disabled
        }

        double adjustment = 0.0;

        // ====================================================================
        // WALL PENALTY: Wall blocks price in that direction
        // ====================================================================
        // Bullish signal blocked by nearby ask wall (resistance)
        if (result.direction == ImbalanceDirection::BULLISH && hasAskWall) {
            if (nearestAskWallTicks <= config.wallBlockDistanceTicks) {
                result.wallBlocksBullish = true;
                // Penalty inversely proportional to distance
                const double distanceRatio = nearestAskWallTicks / config.wallBlockDistanceTicks;
                const double penalty = config.maxWallPenalty * (1.0 - distanceRatio);
                adjustment -= penalty;
            }
        }

        // Bearish signal blocked by nearby bid wall (support)
        if (result.direction == ImbalanceDirection::BEARISH && hasBidWall) {
            if (nearestBidWallTicks <= config.wallBlockDistanceTicks) {
                result.wallBlocksBearish = true;
                const double distanceRatio = nearestBidWallTicks / config.wallBlockDistanceTicks;
                const double penalty = config.maxWallPenalty * (1.0 - distanceRatio);
                adjustment -= penalty;
            }
        }

        // ====================================================================
        // VOID BOOST: Void accelerates price in that direction
        // ====================================================================
        // Bullish signal boosted by nearby ask void (less resistance above)
        if (result.direction == ImbalanceDirection::BULLISH && hasAskVoid) {
            if (nearestAskVoidTicks <= config.voidAccelDistanceTicks) {
                result.voidAcceleratesBullish = true;
                const double distanceRatio = nearestAskVoidTicks / config.voidAccelDistanceTicks;
                const double boost = config.maxVoidBoost * (1.0 - distanceRatio);
                adjustment += boost;
            }
        }

        // Bearish signal boosted by nearby bid void (less support below)
        if (result.direction == ImbalanceDirection::BEARISH && hasBidVoid) {
            if (nearestBidVoidTicks <= config.voidAccelDistanceTicks) {
                result.voidAcceleratesBearish = true;
                const double distanceRatio = nearestBidVoidTicks / config.voidAccelDistanceTicks;
                const double boost = config.maxVoidBoost * (1.0 - distanceRatio);
                adjustment += boost;
            }
        }

        // ====================================================================
        // POLR BOOST: Path of Least Resistance alignment
        // ====================================================================
        // polr: -1=DOWN, 0=BALANCED, +1=UP
        if (result.direction == ImbalanceDirection::BULLISH && polr > 0) {
            adjustment += config.polrBoost;
        }
        if (result.direction == ImbalanceDirection::BEARISH && polr < 0) {
            adjustment += config.polrBoost;
        }

        // Store final adjustment (clamped to reasonable range)
        result.convictionAdjustment = (std::max)(-0.5, (std::min)(0.5, adjustment));
    }

    // ========================================================================
    // COMPUTE STRENGTH AND CONFIDENCE
    // ========================================================================

    void ComputeStrengthAndConfidence(ImbalanceResult& result) {
        double strength = 0.0;
        int signalCount = 0;

        // Stacked imbalance component
        if (result.hasStackedImbalance) {
            const int levels = (std::max)(result.stackedBuyLevels, result.stackedSellLevels);
            strength += config.weightStacked * (std::min)(1.0, levels / 5.0);
            signalCount++;
        }

        // Divergence component
        if (result.hasDeltaDivergence) {
            strength += config.weightDivergence * result.divergenceStrength;
            signalCount++;
        }

        // Absorption component
        if (result.absorptionDetected) {
            strength += config.weightAbsorption * result.absorptionScore;
            signalCount++;
        }

        // Value migration component
        if (result.pocMigrating) {
            const double shiftStrength = (std::min)(1.0, std::abs(result.pocShiftTicks) / 10.0);
            strength += config.weightValueMigration * shiftStrength;
            signalCount++;
        }

        // Range extension component
        if (result.rangeExtensionDetected) {
            const double extStrength = (std::min)(1.0, (result.extensionRatio - 1.0) / 2.0);
            strength += config.weightRangeExtension * extStrength;
            signalCount++;
        }

        // Bonus for multiple signals
        if (signalCount > 1) {
            strength *= (1.0 + 0.1 * (signalCount - 1));
        }

        result.strengthScore = (std::min)(1.0, strength);
        result.signalCount = signalCount;

        // Confidence = strength * context gate multiplier
        double contextMultiplier = 1.0;
        if (!result.contextGate.liquidityOK) contextMultiplier *= 0.5;
        if (!result.contextGate.volatilityOK) contextMultiplier *= 0.5;
        if (!result.contextGate.chopOK) contextMultiplier *= 0.7;

        // Apply spatial conviction adjustment (from wall/void analysis)
        // Adjustment is additive: positive for acceleration, negative for blocking
        double spatialMultiplier = 1.0 + result.convictionAdjustment;
        spatialMultiplier = (std::max)(0.5, (std::min)(1.5, spatialMultiplier));

        result.confidenceScore = result.strengthScore * contextMultiplier * spatialMultiplier;
        result.confidenceScore = (std::min)(1.0, (std::max)(0.0, result.confidenceScore));
    }

    // ========================================================================
    // HYSTERESIS
    // ========================================================================

    void UpdateHysteresis(ImbalanceResult& result, ImbalanceType rawType) {
        result.confirmedType = confirmedType;
        result.barsInState = barsInConfirmedState;

        // Initial state
        if (confirmedType == ImbalanceType::NONE && rawType != ImbalanceType::NONE) {
            candidateType = rawType;
            candidateConfirmationBars = 1;
        }
        // Confirming candidate
        else if (rawType == candidateType && candidateType != confirmedType) {
            candidateConfirmationBars++;
            if (candidateConfirmationBars >= config.minConfirmationBars) {
                // Transition confirmed
                ImbalanceType prevConfirmed = confirmedType;
                confirmedType = candidateType;
                barsInConfirmedState = 1;
                result.imbalanceEntered = (prevConfirmed == ImbalanceType::NONE);
                result.typeChanged = (prevConfirmed != ImbalanceType::NONE);
            }
        }
        // Reinforcing confirmed state
        else if (rawType == confirmedType) {
            barsInConfirmedState++;
            candidateType = confirmedType;
            candidateConfirmationBars = 0;
        }
        // New candidate (different from both)
        else if (rawType != ImbalanceType::NONE) {
            candidateType = rawType;
            candidateConfirmationBars = 1;
            barsInConfirmedState++;
        }
        // No signal
        else {
            if (confirmedType != ImbalanceType::NONE) {
                barsInConfirmedState++;
                // Check for max persistence
                if (barsInConfirmedState > config.maxPersistenceBars) {
                    result.imbalanceResolved = true;
                    confirmedType = ImbalanceType::NONE;
                    barsInConfirmedState = 0;
                }
            }
            candidateType = ImbalanceType::NONE;
            candidateConfirmationBars = 0;
        }

        result.confirmedType = confirmedType;
        result.candidateType = candidateType;
        result.confirmationBars = candidateConfirmationBars;
        result.barsInState = barsInConfirmedState;
        result.isTransitioning = (candidateType != confirmedType && candidateConfirmationBars > 0);
    }

    // ========================================================================
    // COMPUTE DISPLACEMENT SCORE
    // ========================================================================

    double ComputeDisplacementScore(const ImbalanceResult& result,
                                     int rotationFactor, bool is1TF) {
        // Normalize components to [0, 1]
        double pocComponent = (std::min)(1.0, std::abs(result.pocShiftTicks) / 10.0);
        double vaComponent = 1.0 - result.vaOverlapPct;  // Less overlap = more displacement
        double rotationComponent = (std::min)(1.0, std::abs(rotationFactor) / 6.0);
        double extensionComponent = (std::min)(1.0, (result.extensionRatio - 1.0) / 2.0);
        double diagonalComponent = result.diagonalPercentile / 100.0;

        // 1TF gets bonus (strong directional signal)
        double tfBonus = is1TF ? 0.15 : 0.0;

        // Weighted composite
        return (std::min)(1.0,
            0.20 * pocComponent +
            0.20 * vaComponent +
            0.15 * rotationComponent +
            0.15 * extensionComponent +
            0.15 * diagonalComponent +
            tfBonus
        );
    }

    // ========================================================================
    // CHECK WARMUP STATE
    // ========================================================================

    ImbalanceErrorReason CheckWarmupState() {
        const int phaseIdx = SessionPhaseToBucketIndex(currentPhase);
        if (phaseIdx < 0 || phaseIdx >= EFFORT_BUCKET_COUNT) {
            return ImbalanceErrorReason::WARMUP_DIAGONAL;  // Invalid phase = not ready
        }

        int notReady = 0;

        if (diagonalNetBaseline[phaseIdx].size() < config.baselineMinSamples) notReady++;
        if (pocShiftBaseline[phaseIdx].size() < config.baselineMinSamples / 2) notReady++;  // POC shifts are rarer
        if (swingHighs.size() < 2 || swingLows.size() < 2) notReady++;

        if (notReady > 1) {
            return ImbalanceErrorReason::WARMUP_MULTIPLE;
        }
        if (diagonalNetBaseline[phaseIdx].size() < config.baselineMinSamples) {
            return ImbalanceErrorReason::WARMUP_DIAGONAL;
        }
        if (swingHighs.size() < 2 || swingLows.size() < 2) {
            return ImbalanceErrorReason::WARMUP_SWING;
        }

        return ImbalanceErrorReason::NONE;
    }
};

// ============================================================================
// IMBALANCE DECISION INPUT (For BaselineDecisionGate Integration)
// ============================================================================
// Wrapper struct matching the pattern of other decision inputs.

struct ImbalanceDecisionInput {
    ImbalanceResult result;

    bool IsReady() const { return result.IsReady(); }
    bool IsWarmup() const { return result.IsWarmup(); }
    bool IsBlocked() const { return result.IsBlocked(); }

    bool HasSignal() const { return result.HasSignal(); }
    bool HasConfirmedSignal() const { return result.HasConfirmedSignal(); }

    ImbalanceType GetType() const {
        return IsReady() ? result.confirmedType : ImbalanceType::NONE;
    }

    ImbalanceDirection GetDirection() const {
        return IsReady() ? result.direction : ImbalanceDirection::NEUTRAL;
    }

    ConvictionType GetConviction() const {
        return IsReady() ? result.conviction : ConvictionType::UNKNOWN;
    }

    double GetDisplacementScore() const {
        return IsReady() ? result.displacementScore : 0.0;
    }

    double GetConfidence() const {
        return IsReady() ? result.confidenceScore : 0.0;
    }

    bool IsHighQuality() const { return result.IsHighQualitySignal(); }
};

} // namespace AMT
