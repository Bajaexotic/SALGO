// ============================================================================
// AMT_Volatility.h - Volatility Regime Classification Engine
// ============================================================================
//
// PURPOSE: Volatility is a context gate - it tells you whether triggers are
// trustworthy. This engine answers:
//
//   1. What regime am I in? (COMPRESSION / NORMAL / EXPANSION / EVENT)
//   2. Is the regime stable or transitioning? (hysteresis / persistence)
//   3. What range expansion should I expect? (normalized range metric)
//   4. Do I block or tighten requirements? (tradability rules)
//   5. What invalidates the estimate? (insufficient history, session reset)
//
// DESIGN PRINCIPLES:
//   - Uses existing bar_range from EffortBaselineStore (no new data collection)
//   - Phase-aware baselines (GLOBEX != RTH)
//   - Hysteresis prevents regime whipsaw (MarketStateBucket pattern)
//   - ATR normalization for cross-symbol/timeframe comparability
//   - NO-FALLBACK contract: explicit validity at every decision point
//
// INTEGRATION:
//   VolatilityEngine volEngine;
//   volEngine.SetEffortStore(&effortStore);
//   volEngine.SetPhase(currentPhase);
//
//   VolatilityResult result = volEngine.Compute(barRangeTicks, atrValue);
//   if (result.IsReady()) {
//       if (result.regime == VolatilityRegime::COMPRESSION) {
//           // Tighten entry requirements, expect false breakouts
//       }
//   }
//
// ============================================================================

#pragma once

#include "amt_core.h"
#include "AMT_Snapshots.h"
#include <algorithm>
#include <cmath>
#include <array>

namespace AMT {

// ============================================================================
// SYNTHETIC BAR AGGREGATOR
// ============================================================================
// Aggregates N 1-minute bars into synthetic periods for regime detection.
// This separates execution timeframe (1-min) from regime timeframe (5-15 min).
//
// PURPOSE:
//   - Volatility regime is a session-level concept, not minute-level noise
//   - 1-min bars have high noise-to-signal ratio
//   - 3-bar hysteresis on 1-min = 3 minutes (too short for regime changes)
//   - Aggregating to 5-min synthetic bars: 3-bar hysteresis = 15 minutes
//
// DESIGN:
//   - Rolling window of N bars (configurable, default: 5 for 5-min equivalent)
//   - Synthetic range = max(highs) - min(lows) across window
//   - Synthetic duration = sum of bar durations
//   - Range velocity computed from synthetic values
//
// USAGE:
//   SyntheticBarAggregator aggregator(5);  // 5-bar (5-min) synthetic
//   aggregator.Push(barHigh, barLow, barDurationSec);
//   if (aggregator.IsReady()) {
//       double synthRange = aggregator.GetSyntheticRangeTicks(tickSize);
//       double synthDuration = aggregator.GetSyntheticDurationSec();
//       // Pass to VolatilityEngine::Compute()
//   }
// ============================================================================

struct SyntheticBarData {
    double high = 0.0;
    double low = 0.0;
    double durationSec = 0.0;
    bool valid = false;
};

class SyntheticBarAggregator {
public:
    static constexpr int MAX_AGGREGATION_BARS = 15;
    static constexpr int DEFAULT_AGGREGATION_BARS = 5;  // 5-min equivalent on 1-min chart

private:
    std::array<SyntheticBarData, MAX_AGGREGATION_BARS> buffer_;
    int writeIdx_ = 0;
    int validCount_ = 0;
    int aggregationBars_ = DEFAULT_AGGREGATION_BARS;

    // Cached synthetic values (computed on Push)
    double syntheticHigh_ = 0.0;
    double syntheticLow_ = 0.0;
    double syntheticDurationSec_ = 0.0;
    bool cacheValid_ = false;

    // Synthetic bar boundary tracking
    // Used to detect when a NEW synthetic bar forms (every N raw bars)
    // This signals when to push to synthetic baseline
    int rawBarCounter_ = 0;           // Raw bars since session start
    bool newSyntheticBarFormed_ = false;  // Set true when boundary crossed

public:
    // =========================================================================
    // CONFIGURATION
    // =========================================================================

    /**
     * Set number of bars to aggregate.
     * @param bars Number of 1-min bars per synthetic period (1-15)
     */
    void SetAggregationBars(int bars) {
        aggregationBars_ = (std::max)(1, (std::min)(bars, MAX_AGGREGATION_BARS));
        // Reset on config change
        Reset();
    }

    int GetAggregationBars() const { return aggregationBars_; }

    /**
     * Get effective confirmation bars for regime hysteresis.
     * Scales down since each "bar" is now N minutes, not 1 minute.
     * Target: ~15 minutes of confirmation regardless of aggregation.
     */
    int GetEffectiveConfirmationBars(int baseConfirmationBars) const {
        // On 1-min bars with 5-bar aggregation:
        //   base=3 stays 3 (3 synthetic bars = 15 minutes)
        // On 1-min bars with 1-bar aggregation:
        //   base=3 needs scaling: 15 minutes = 15 bars
        return baseConfirmationBars;  // Already appropriate for synthetic bars
    }

    // =========================================================================
    // DATA INGESTION
    // =========================================================================

    /**
     * Push a new bar's data into the aggregator.
     * Call once per closed 1-min bar.
     * @return true if this bar completes a new synthetic bar (boundary crossed)
     */
    bool Push(double high, double low, double durationSec) {
        SyntheticBarData& slot = buffer_[writeIdx_];
        slot.high = high;
        slot.low = low;
        slot.durationSec = durationSec;
        slot.valid = true;

        writeIdx_ = (writeIdx_ + 1) % MAX_AGGREGATION_BARS;
        if (validCount_ < MAX_AGGREGATION_BARS) {
            validCount_++;
        }

        // Track synthetic bar boundary
        rawBarCounter_++;
        newSyntheticBarFormed_ = (rawBarCounter_ % aggregationBars_ == 0) && IsReady();

        // Recompute cached values
        ComputeSynthetic();

        return newSyntheticBarFormed_;
    }

    /**
     * Check if we have enough bars to produce valid synthetic data.
     */
    bool IsReady() const {
        return validCount_ >= aggregationBars_;
    }

    /**
     * Check if a new synthetic bar was just formed on the last Push().
     * Use this to know when to push to the synthetic baseline.
     */
    bool DidNewSyntheticBarForm() const {
        return newSyntheticBarFormed_;
    }

    /**
     * Get raw bar count since session/reset.
     */
    int GetRawBarCount() const {
        return rawBarCounter_;
    }

    // =========================================================================
    // SYNTHETIC VALUE ACCESSORS
    // =========================================================================

    /**
     * Get synthetic range in ticks.
     * Range = max(highs) - min(lows) across aggregation window.
     */
    double GetSyntheticRangeTicks(double tickSize) const {
        if (!cacheValid_ || tickSize <= 0.0) return 0.0;
        return (syntheticHigh_ - syntheticLow_) / tickSize;
    }

    /**
     * Get synthetic range in price units.
     */
    double GetSyntheticRangePrice() const {
        if (!cacheValid_) return 0.0;
        return syntheticHigh_ - syntheticLow_;
    }

    /**
     * Get total duration of the synthetic bar in seconds.
     */
    double GetSyntheticDurationSec() const {
        return cacheValid_ ? syntheticDurationSec_ : 0.0;
    }

    /**
     * Get synthetic high price.
     */
    double GetSyntheticHigh() const {
        return cacheValid_ ? syntheticHigh_ : 0.0;
    }

    /**
     * Get synthetic low price.
     */
    double GetSyntheticLow() const {
        return cacheValid_ ? syntheticLow_ : 0.0;
    }

    /**
     * Get range velocity for synthetic bar (ticks per minute).
     */
    double GetSyntheticRangeVelocity(double tickSize) const {
        if (!cacheValid_ || tickSize <= 0.0) return 0.0;
        double durationMin = syntheticDurationSec_ / 60.0;
        if (durationMin < 0.001) return 0.0;
        return GetSyntheticRangeTicks(tickSize) / durationMin;
    }

    // =========================================================================
    // RESET
    // =========================================================================

    void Reset() {
        for (auto& slot : buffer_) {
            slot = SyntheticBarData{};
        }
        writeIdx_ = 0;
        validCount_ = 0;
        cacheValid_ = false;
        syntheticHigh_ = 0.0;
        syntheticLow_ = 0.0;
        syntheticDurationSec_ = 0.0;
        rawBarCounter_ = 0;
        newSyntheticBarFormed_ = false;
    }

private:
    /**
     * Compute synthetic values from buffer.
     * Uses the most recent aggregationBars_ entries.
     */
    void ComputeSynthetic() {
        if (validCount_ < aggregationBars_) {
            cacheValid_ = false;
            return;
        }

        double maxHigh = -1e30;
        double minLow = 1e30;
        double totalDuration = 0.0;

        // Walk backwards from most recent entry
        int idx = (writeIdx_ - 1 + MAX_AGGREGATION_BARS) % MAX_AGGREGATION_BARS;
        for (int i = 0; i < aggregationBars_; ++i) {
            const SyntheticBarData& slot = buffer_[idx];
            if (!slot.valid) {
                cacheValid_ = false;
                return;
            }

            if (slot.high > maxHigh) maxHigh = slot.high;
            if (slot.low < minLow) minLow = slot.low;
            totalDuration += slot.durationSec;

            idx = (idx - 1 + MAX_AGGREGATION_BARS) % MAX_AGGREGATION_BARS;
        }

        syntheticHigh_ = maxHigh;
        syntheticLow_ = minLow;
        syntheticDurationSec_ = totalDuration;
        cacheValid_ = true;
    }
};

// ============================================================================
// VOLATILITY REGIME ENUM
// ============================================================================
// Four distinct regimes with different trading implications.
//
// COMPRESSION: Low volatility, tight ranges.
//   - Breakouts are unreliable (many false moves)
//   - Mean reversion more likely
//   - Reduce position size, widen stops
//
// NORMAL: Typical volatility for this phase/symbol.
//   - Standard trading rules apply
//   - Full confidence in signals
//
// EXPANSION: Elevated volatility, wide ranges.
//   - Trend continuation more likely
//   - Breakouts are more reliable
//   - But: stops need to be wider
//
// EVENT: Extreme volatility spike (news, circuit breaker, gap).
//   - Highly unusual, exceeds normal expansion
//   - May want to pause trading entirely
//   - Often precedes regime shift
// ============================================================================

enum class VolatilityRegime : int {
    UNKNOWN = 0,      // Baseline not ready or invalid
    COMPRESSION,      // Below P25 - tight ranges, unreliable breakouts
    NORMAL,           // P25-P75 - typical volatility
    EXPANSION,        // Above P75 - wide ranges, trend continuation
    EVENT             // Above P95 - extreme spike, consider pausing
};

inline const char* VolatilityRegimeToString(VolatilityRegime r) {
    switch (r) {
        case VolatilityRegime::UNKNOWN:     return "UNKNOWN";
        case VolatilityRegime::COMPRESSION: return "COMPRESSION";
        case VolatilityRegime::NORMAL:      return "NORMAL";
        case VolatilityRegime::EXPANSION:   return "EXPANSION";
        case VolatilityRegime::EVENT:       return "EVENT";
    }
    return "UNK";
}

inline const char* VolatilityRegimeToShortString(VolatilityRegime r) {
    switch (r) {
        case VolatilityRegime::UNKNOWN:     return "UNK";
        case VolatilityRegime::COMPRESSION: return "COMP";
        case VolatilityRegime::NORMAL:      return "NORM";
        case VolatilityRegime::EXPANSION:   return "EXP";
        case VolatilityRegime::EVENT:       return "EVT";
    }
    return "?";
}

// ============================================================================
// AUCTION PACE ENUM (Rate of Discovery)
// ============================================================================
// Measures how quickly price is probing for acceptance/rejection.
// This is the AMT "rate of auction" - how costly it is to be wrong in TIME.
//
// SLOW: Price is probing gently, patient entries possible
// NORMAL: Typical auction pace for this session phase
// FAST: Rapid price discovery, need stricter requirements
// EXTREME: Frantic tape, consider reducing activity
//
// Direction-agnostic: measures pace, not bias.
// ============================================================================

enum class AuctionPace : int {
    UNKNOWN  = 0,  // Warmup / invalid
    SLOW     = 1,  // < P25 - slow discovery, patient entries
    NORMAL   = 2,  // P25-P75 - typical auction pace
    FAST     = 3,  // P75-P95 - rapid probing, tighten requirements
    EXTREME  = 4   // > P95 - frantic tape, reduce/pause activity
};

inline const char* AuctionPaceToString(AuctionPace p) {
    switch (p) {
        case AuctionPace::UNKNOWN:  return "UNKNOWN";
        case AuctionPace::SLOW:     return "SLOW";
        case AuctionPace::NORMAL:   return "NORMAL";
        case AuctionPace::FAST:     return "FAST";
        case AuctionPace::EXTREME:  return "EXTREME";
    }
    return "UNK";
}

inline const char* AuctionPaceToShortString(AuctionPace p) {
    switch (p) {
        case AuctionPace::UNKNOWN:  return "UNK";
        case AuctionPace::SLOW:     return "SLO";
        case AuctionPace::NORMAL:   return "NRM";
        case AuctionPace::FAST:     return "FST";
        case AuctionPace::EXTREME:  return "EXT";
    }
    return "?";
}

// ============================================================================
// VOLATILITY ERROR REASON
// ============================================================================
// Explicit error tracking (no silent fallbacks).

enum class VolatilityErrorReason : int {
    NONE = 0,

    // Warmup states (expected, not errors)
    WARMUP_BASELINE = 10,         // Phase baseline not ready (< MIN_SAMPLES)
    WARMUP_ATR = 11,              // ATR needs more bars
    WARMUP_PRIOR = 12,            // Prior session data not ready
    WARMUP_SYNTHETIC = 13,        // Synthetic bar aggregator not ready

    // Configuration errors
    ERR_NO_EFFORT_STORE = 20,     // EffortBaselineStore not configured
    ERR_INVALID_PHASE = 21,       // Non-tradeable phase
    ERR_INVALID_INPUT = 22,       // barRangeTicks <= 0 or NaN

    // Session events
    SESSION_RESET = 30,           // Just transitioned, no session evidence yet
    SYMBOL_CHANGED = 31           // Symbol changed, baselines invalidated
};

inline const char* VolatilityErrorToString(VolatilityErrorReason r) {
    switch (r) {
        case VolatilityErrorReason::NONE:              return "NONE";
        case VolatilityErrorReason::WARMUP_BASELINE:   return "WARMUP_BASELINE";
        case VolatilityErrorReason::WARMUP_ATR:        return "WARMUP_ATR";
        case VolatilityErrorReason::WARMUP_PRIOR:      return "WARMUP_PRIOR";
        case VolatilityErrorReason::WARMUP_SYNTHETIC:  return "WARMUP_SYNTHETIC";
        case VolatilityErrorReason::ERR_NO_EFFORT_STORE: return "NO_EFFORT_STORE";
        case VolatilityErrorReason::ERR_INVALID_PHASE: return "INVALID_PHASE";
        case VolatilityErrorReason::ERR_INVALID_INPUT: return "INVALID_INPUT";
        case VolatilityErrorReason::SESSION_RESET:     return "SESSION_RESET";
        case VolatilityErrorReason::SYMBOL_CHANGED:    return "SYMBOL_CHANGED";
    }
    return "UNK_ERR";
}

inline bool IsVolatilityWarmup(VolatilityErrorReason r) {
    return r == VolatilityErrorReason::WARMUP_BASELINE ||
           r == VolatilityErrorReason::WARMUP_ATR ||
           r == VolatilityErrorReason::WARMUP_PRIOR ||
           r == VolatilityErrorReason::WARMUP_SYNTHETIC;
}

// ============================================================================
// TRADABILITY RULES
// ============================================================================
// What constraints to apply based on volatility regime.

struct TradabilityRules {
    bool allowNewEntries = true;       // Can open new positions
    bool requireTightStop = false;     // Must use tighter stops
    bool requireWideStop = false;      // Must use wider stops
    bool requireHigherConfidence = false;  // Need stronger signal confirmation
    bool blockBreakouts = false;       // Don't trust breakout signals
    bool preferMeanReversion = false;  // Fade moves rather than follow
    double positionSizeMultiplier = 1.0;  // Scale position size (regime-based)

    // Pace-derived multipliers (combined with regime multipliers by consumers)
    double paceConfirmationMultiplier = 1.0;  // FAST/EXTREME require more confirmation
    double paceSizeMultiplier = 1.0;          // FAST/EXTREME reduce position size

    // Convenience check
    bool IsRestricted() const {
        return !allowNewEntries || requireHigherConfidence;
    }

    // Get combined size multiplier (regime × pace)
    double GetCombinedSizeMultiplier() const {
        return positionSizeMultiplier * paceSizeMultiplier;
    }
};

// ============================================================================
// VOLATILITY RESULT (Per-Bar Output)
// ============================================================================
// Complete snapshot of volatility state for the current bar.

struct VolatilityResult {
    // =========================================================================
    // CURRENT MEASUREMENT
    // =========================================================================
    double barRangeTicks = 0.0;        // Raw: High - Low in ticks
    double rangePercentile = 0.0;      // vs phase-aware historical baseline
    bool rangeReady = false;           // Baseline has enough samples

    double atrValue = 0.0;             // ATR value (if provided)
    double atrPercentile = 0.0;        // ATR vs baseline (optional)
    bool atrReady = false;             // ATR baseline ready

    // Normalized range: barRange / ATR (when ATR available)
    // Values > 1.0 = wider than average, < 1.0 = tighter
    double normalizedRange = 0.0;
    bool normalizedRangeValid = false;

    // =========================================================================
    // REGIME CLASSIFICATION
    // =========================================================================
    VolatilityRegime regime = VolatilityRegime::UNKNOWN;
    VolatilityRegime rawRegime = VolatilityRegime::UNKNOWN;  // Before hysteresis

    // =========================================================================
    // STABILITY / PERSISTENCE
    // =========================================================================
    int barsInRegime = 0;              // Consecutive bars in confirmed regime
    int stabilityBars = 0;             // Bars without regime change
    bool isStable = false;             // regime == rawRegime (no transition pending)

    // Hysteresis state (for diagnostics)
    VolatilityRegime candidateRegime = VolatilityRegime::UNKNOWN;
    int candidateConfirmationBars = 0;
    double confirmationProgress = 0.0; // 0.0-1.0
    bool isTransitioning = false;      // candidateRegime != confirmedRegime

    // =========================================================================
    // EXPECTED RANGE (Forward-Looking)
    // =========================================================================
    // Based on regime and historical data, what range expansion to expect
    double expectedRangeMultiplier = 1.0;  // 1.0 = normal, <1 = compression, >1 = expansion
    double p75RangeTicks = 0.0;            // 75th percentile range (upper normal bound)
    double p25RangeTicks = 0.0;            // 25th percentile range (lower normal bound)
    double p95RangeTicks = 0.0;            // 95th percentile range (event threshold)

    // =========================================================================
    // TRADABILITY
    // =========================================================================
    TradabilityRules tradability;

    // =========================================================================
    // EVENTS (Only True on Transition Bars)
    // =========================================================================
    bool compressionEntered = false;   // Just entered compression
    bool expansionEntered = false;     // Just entered expansion
    bool eventDetected = false;        // Just entered event (extreme)
    bool regimeChanged = false;        // Any regime change this bar

    // =========================================================================
    // PRIOR SESSION CONTEXT
    // =========================================================================
    double priorSessionAvgRange = 0.0;    // Prior session's avg range
    double priorSessionVolatility = 0.0;  // Prior session's volatility metric
    bool priorReady = false;              // Has valid prior session data
    int sessionsContributed = 0;          // Sessions in prior baseline

    // =========================================================================
    // AUCTION PACE (Rate of Discovery)
    // =========================================================================
    // Measures how quickly price is probing (ticks per minute).
    // Direction-agnostic: measures pace, not bias.
    AuctionPace pace = AuctionPace::UNKNOWN;
    AuctionPace rawPace = AuctionPace::UNKNOWN;    // Before hysteresis
    double rangeVelocity = 0.0;                    // Raw: ticks/minute
    double rangeVelocityPercentile = 0.0;          // vs phase baseline
    bool paceReady = false;                        // Pace baseline has enough samples

    // Pace hysteresis state (for diagnostics)
    AuctionPace candidatePace = AuctionPace::UNKNOWN;
    int candidatePaceConfirmationBars = 0;
    double paceConfirmationProgress = 0.0;         // 0.0-1.0
    bool isPaceTransitioning = false;              // candidatePace != confirmedPace
    int barsInPace = 0;                            // Consecutive bars in confirmed pace

    // Pace events (only true on transition bars)
    bool slowPaceEntered = false;
    bool fastPaceEntered = false;
    bool extremePaceEntered = false;
    bool paceChanged = false;

    // =========================================================================
    // SYNTHETIC BAR TRACKING
    // =========================================================================
    // When using 1-min bars, regime is computed from aggregated synthetic bars.
    // These fields track the synthetic bar state.
    bool usingSyntheticBars = false;           // True if regime from synthetic bars
    int syntheticAggregationBars = 0;          // N bars per synthetic period
    int syntheticBarsCollected = 0;            // Bars collected so far
    double syntheticRangeTicks = 0.0;          // Synthetic bar range (max high - min low)
    double syntheticDurationSec = 0.0;         // Synthetic bar total duration
    bool newSyntheticBarFormed = false;        // True if this bar completed a new synthetic bar
    double syntheticRangeVelocity = 0.0;       // Synthetic velocity (ticks/min) for baseline

    // =========================================================================
    // VALIDITY / ERROR
    // =========================================================================
    VolatilityErrorReason errorReason = VolatilityErrorReason::NONE;
    SessionPhase phase = SessionPhase::UNKNOWN;
    int errorBar = -1;  // Bar where error occurred

    // =========================================================================
    // ACCESSORS
    // =========================================================================

    bool IsReady() const {
        return rangeReady && errorReason == VolatilityErrorReason::NONE;
    }

    bool IsWarmup() const {
        return IsVolatilityWarmup(errorReason);
    }

    bool IsHardError() const {
        return errorReason != VolatilityErrorReason::NONE && !IsWarmup();
    }

    bool IsCompression() const { return IsReady() && regime == VolatilityRegime::COMPRESSION; }
    bool IsNormal() const { return IsReady() && regime == VolatilityRegime::NORMAL; }
    bool IsExpansion() const { return IsReady() && regime == VolatilityRegime::EXPANSION; }
    bool IsEvent() const { return IsReady() && regime == VolatilityRegime::EVENT; }

    bool IsElevated() const { return IsExpansion() || IsEvent(); }
    bool IsRestricted() const { return IsCompression() || IsEvent(); }

    // Check if tradability allows new entries
    bool CanEnterNewPosition() const { return tradability.allowNewEntries; }

    // Get position size adjustment
    double GetPositionSizeMultiplier() const { return tradability.positionSizeMultiplier; }

    // =========================================================================
    // PACE ACCESSORS
    // =========================================================================

    bool IsPaceReady() const { return paceReady; }

    bool IsSlowPace() const { return paceReady && pace == AuctionPace::SLOW; }
    bool IsNormalPace() const { return paceReady && pace == AuctionPace::NORMAL; }
    bool IsFastPace() const { return paceReady && pace == AuctionPace::FAST; }
    bool IsExtremePace() const { return paceReady && pace == AuctionPace::EXTREME; }

    // Check if pace is elevated (fast or extreme)
    bool IsElevatedPace() const { return IsFastPace() || IsExtremePace(); }

    // Get combined position size multiplier (regime × pace)
    double GetCombinedPositionSizeMultiplier() const {
        return tradability.positionSizeMultiplier * tradability.paceSizeMultiplier;
    }

    // Get combined confirmation multiplier (regime × pace)
    double GetCombinedConfirmationMultiplier() const {
        return tradability.paceConfirmationMultiplier;
    }
};

// ============================================================================
// VOLATILITY CONFIGURATION
// ============================================================================

struct VolatilityConfig {
    // =========================================================================
    // REGIME THRESHOLDS (Percentiles)
    // =========================================================================
    double compressionThreshold = 25.0;  // < P25 = compressed
    double expansionThreshold = 75.0;    // > P75 = expanded
    double eventThreshold = 95.0;        // > P95 = event (extreme)

    // =========================================================================
    // HYSTERESIS CONFIGURATION (Asymmetric)
    // =========================================================================
    // Fast escalation to danger, slow de-escalation to calm
    //
    // EVENT entry = 1 bar (immediate protection when danger detected)
    // EVENT exit = 3 bars (confirm sustained calm before relaxing)
    // Other transitions = 2 bars (moderate speed for operational adjustments)

    int eventEntryBars = 1;              // Bars to enter EVENT (fast protection)
    int eventExitBars = 3;               // Bars to exit EVENT (confirm calm)
    int otherTransitionBars = 2;         // Bars for non-EVENT transitions

    // Legacy field (kept for compatibility, but asymmetric logic takes precedence)
    int minConfirmationBars = 3;         // Default fallback

    double confirmationMargin = 5.0;     // Percentile margin for state change
    int maxStabilityBars = 50;           // Max bars to track stability

    // =========================================================================
    // TRADABILITY RULES
    // =========================================================================
    // What to do in each regime

    // COMPRESSION tradability
    bool compressionBlockNewEntries = false;   // Allow entries, but with caution
    bool compressionBlockBreakouts = true;     // Don't trust breakouts
    bool compressionPreferMeanReversion = true;
    double compressionPositionScale = 0.75;    // Scale down position size

    // EXPANSION tradability
    bool expansionRequireWideStop = true;      // Need wider stops
    double expansionPositionScale = 1.0;       // Normal position size

    // EVENT tradability
    bool eventBlockNewEntries = true;          // Pause new entries
    double eventPositionScale = 0.5;           // Half position if forced

    // =========================================================================
    // BASELINE REQUIREMENTS
    // =========================================================================
    size_t baselineMinSamples = 10;            // Minimum samples before ready
    int requiredSessions = 5;                  // Sessions needed for prior

    // =========================================================================
    // ATR CONFIGURATION
    // =========================================================================
    int atrLength = 14;                        // ATR calculation length
    bool useATRNormalization = true;           // Enable ATR-normalized metrics

    // =========================================================================
    // EXPECTED RANGE MULTIPLIERS (Per Regime)
    // =========================================================================
    // These indicate how much range expansion to expect in each regime
    double compressionExpectedMultiplier = 0.6;  // Expect 60% of normal range
    double normalExpectedMultiplier = 1.0;       // Expect normal range
    double expansionExpectedMultiplier = 1.5;    // Expect 150% of normal range
    double eventExpectedMultiplier = 2.5;        // Expect 250% of normal range

    // =========================================================================
    // AUCTION PACE CONFIGURATION
    // =========================================================================
    // Pace thresholds (percentiles of range velocity)
    double slowPaceThreshold = 25.0;       // < P25 = slow
    double fastPaceThreshold = 75.0;       // > P75 = fast
    double extremePaceThreshold = 95.0;    // > P95 = extreme

    // Pace hysteresis (separate from regime hysteresis)
    int paceMinConfirmationBars = 2;       // Bars to confirm pace change

    // Pace tradability multipliers
    // SLOW: Fewer confirmations needed, normal size
    double slowPaceConfirmationMultiplier = 0.8;
    double slowPaceSizeMultiplier = 1.0;

    // NORMAL: Standard
    double normalPaceConfirmationMultiplier = 1.0;
    double normalPaceSizeMultiplier = 1.0;

    // FAST: Stricter entry, smaller size
    double fastPaceConfirmationMultiplier = 1.5;
    double fastPaceSizeMultiplier = 0.75;

    // EXTREME: Much stricter, half size
    double extremePaceConfirmationMultiplier = 2.0;
    double extremePaceSizeMultiplier = 0.5;

    // =========================================================================
    // SYNTHETIC BAR AGGREGATION (For 1-Min Charts)
    // =========================================================================
    // When using 1-min bars, regime detection operates on synthetic N-bar periods.
    // This separates execution timeframe from regime timeframe.
    //
    // Default: 5 bars = 5-minute synthetic period
    // - Regime detection uses synthetic range (max high - min low over window)
    // - 3-bar confirmation on synthetic = 15 minutes (not 3 minutes)
    // - Execution still uses 1-min precision
    //
    bool useSyntheticBars = true;          // Enable synthetic aggregation
    int syntheticAggregationBars = 5;      // Number of 1-min bars per synthetic period
};

// ============================================================================
// VOLATILITY ENGINE
// ============================================================================
// Main engine for volatility regime classification with hysteresis.
//
// USAGE:
//   1. Create engine and configure
//   2. Set effortStore reference (required)
//   3. Call SetPhase() each bar with current session phase
//   4. Call Compute() with bar range and optional ATR
//   5. Check result.IsReady() before using regime
//
// SESSION BOUNDARY:
//   1. Call FinalizeSession() at end of session (updates priors)
//   2. Call ResetForSession() at start of new session
//
// ============================================================================

class VolatilityEngine {
public:
    // =========================================================================
    // CONFIGURATION
    // =========================================================================
    VolatilityConfig config;

    // =========================================================================
    // REFERENCES (Not Owned)
    // =========================================================================
    const EffortBaselineStore* effortStore = nullptr;

    // =========================================================================
    // CURRENT STATE
    // =========================================================================
    SessionPhase currentPhase = SessionPhase::UNKNOWN;

    // Hysteresis state (not phase-bucketed - tracks current session regime)
    VolatilityRegime confirmedRegime = VolatilityRegime::UNKNOWN;
    VolatilityRegime candidateRegime = VolatilityRegime::UNKNOWN;
    int candidateConfirmationBars = 0;
    int barsInConfirmedRegime = 0;
    int stabilityBars = 0;

    // Session evidence (for prior calculation)
    int sessionBars = 0;
    int compressionBars = 0;
    int normalBars = 0;
    int expansionBars = 0;
    int eventBars = 0;
    double sessionRangeSum = 0.0;
    double sessionRangeSqSum = 0.0;  // For variance

    // =========================================================================
    // PRIOR SESSION DATA (EWMA-Blended)
    // =========================================================================
    double priorAvgRange = -1.0;       // Prior session average range
    double priorVolatility = -1.0;     // Prior session volatility metric
    double priorCompressionRatio = -1.0;
    double priorExpansionRatio = -1.0;
    bool priorReady = false;
    int sessionsContributed = 0;

    static constexpr double PRIOR_INERTIA = 0.8;  // EWMA blend factor

    // =========================================================================
    // ATR TRACKING (Optional)
    // =========================================================================
    RollingDist atrBaseline;           // ATR distribution for normalization
    double lastATRValue = 0.0;
    bool atrBaselineReady = false;

    // =========================================================================
    // PACE HYSTERESIS STATE (Separate from Regime)
    // =========================================================================
    AuctionPace confirmedPace = AuctionPace::UNKNOWN;
    AuctionPace candidatePace = AuctionPace::UNKNOWN;
    int candidatePaceConfirmationBars = 0;
    int barsInConfirmedPace = 0;

    // Pace session evidence
    int slowPaceBars = 0;
    int normalPaceBars = 0;
    int fastPaceBars = 0;
    int extremePaceBars = 0;
    double sessionVelocitySum = 0.0;

    // =========================================================================
    // SYNTHETIC BAR AGGREGATOR (For 1-Min Charts)
    // =========================================================================
    SyntheticBarAggregator syntheticAggregator;
    bool syntheticModeActive = false;   // True if using synthetic bars
    int rawBarsProcessed = 0;           // Raw bars since last synthetic update

    // =========================================================================
    // CONSTRUCTOR / INITIALIZATION
    // =========================================================================

    VolatilityEngine() {
        atrBaseline.reset(300);  // Track 300 bars of ATR
        syntheticAggregator.SetAggregationBars(config.syntheticAggregationBars);
    }

    void SetEffortStore(const EffortBaselineStore* store) {
        effortStore = store;
    }

    void SetPhase(SessionPhase phase) {
        currentPhase = phase;
    }

    void SetConfig(const VolatilityConfig& cfg) {
        config = cfg;
        // Sync aggregator with config
        syntheticAggregator.SetAggregationBars(config.syntheticAggregationBars);
        syntheticModeActive = config.useSyntheticBars;
    }

    /**
     * Enable or disable synthetic bar mode.
     * When enabled, ComputeFromRawBar aggregates before computing regime.
     */
    void SetSyntheticMode(bool enabled, int aggregationBars = 5) {
        syntheticModeActive = enabled;
        config.useSyntheticBars = enabled;
        config.syntheticAggregationBars = aggregationBars;
        syntheticAggregator.SetAggregationBars(aggregationBars);
    }

    bool IsSyntheticModeActive() const { return syntheticModeActive; }
    int GetSyntheticAggregationBars() const { return syntheticAggregator.GetAggregationBars(); }
    bool IsSyntheticReady() const { return syntheticAggregator.IsReady(); }

    // =========================================================================
    // MAIN COMPUTATION
    // =========================================================================
    // Call once per closed bar with bar range, duration, and optional ATR.
    //
    // barRangeTicks: High - Low in ticks for the bar (or synthetic range)
    // barDurationSec: Duration of the bar in seconds (for pace calculation)
    // atrValue: ATR value (0 if not available)
    // useSyntheticBaseline: If true, query synthetic_bar_range instead of bar_range
    //
    VolatilityResult Compute(double barRangeTicks, double barDurationSec,
                             double atrValue = 0.0, bool useSyntheticBaseline = false) {
        VolatilityResult result;
        result.barRangeTicks = barRangeTicks;
        result.atrValue = atrValue;
        result.phase = currentPhase;

        // ---------------------------------------------------------------------
        // Input Validation
        // ---------------------------------------------------------------------
        if (!std::isfinite(barRangeTicks) || barRangeTicks < 0.0) {
            result.errorReason = VolatilityErrorReason::ERR_INVALID_INPUT;
            return result;
        }

        if (effortStore == nullptr) {
            result.errorReason = VolatilityErrorReason::ERR_NO_EFFORT_STORE;
            return result;
        }

        const int phaseIdx = SessionPhaseToBucketIndex(currentPhase);
        if (phaseIdx < 0) {
            result.errorReason = VolatilityErrorReason::ERR_INVALID_PHASE;
            return result;
        }

        // ---------------------------------------------------------------------
        // Query Phase-Aware Baseline
        // ---------------------------------------------------------------------
        // Use synthetic baseline when in synthetic mode, otherwise use raw bar baseline
        const auto& bucket = effortStore->Get(currentPhase);
        const RollingDist& rangeBaseline = useSyntheticBaseline
            ? bucket.synthetic_bar_range
            : bucket.bar_range;

        auto rangePctile = rangeBaseline.TryPercentile(barRangeTicks);

        if (!rangePctile.valid) {
            result.errorReason = useSyntheticBaseline
                ? VolatilityErrorReason::WARMUP_SYNTHETIC
                : VolatilityErrorReason::WARMUP_BASELINE;
            return result;
        }

        result.rangePercentile = rangePctile.value;
        result.rangeReady = true;
        result.errorReason = VolatilityErrorReason::NONE;

        // ---------------------------------------------------------------------
        // Get Percentile Reference Points
        // ---------------------------------------------------------------------
        // These help understand the expected range in each regime
        result.p25RangeTicks = GetPercentileValue(rangeBaseline, 25.0);
        result.p75RangeTicks = GetPercentileValue(rangeBaseline, 75.0);
        result.p95RangeTicks = GetPercentileValue(rangeBaseline, 95.0);

        // ---------------------------------------------------------------------
        // ATR Processing (Optional)
        // ---------------------------------------------------------------------
        if (atrValue > 0.0 && std::isfinite(atrValue)) {
            lastATRValue = atrValue;
            atrBaseline.push(atrValue);

            if (atrBaseline.size() >= config.baselineMinSamples) {
                auto atrPctile = atrBaseline.TryPercentile(atrValue);
                if (atrPctile.valid) {
                    result.atrPercentile = atrPctile.value;
                    result.atrReady = true;
                }
            }

            // Normalized range = barRange / ATR
            if (atrValue > 0.0) {
                result.normalizedRange = barRangeTicks / atrValue;
                result.normalizedRangeValid = true;
            }
        }

        // ---------------------------------------------------------------------
        // Classify Raw Regime (Before Hysteresis)
        // ---------------------------------------------------------------------
        VolatilityRegime rawRegime = ClassifyRegime(result.rangePercentile);
        result.rawRegime = rawRegime;

        // ---------------------------------------------------------------------
        // Apply Hysteresis
        // ---------------------------------------------------------------------
        UpdateHysteresis(rawRegime);

        // Populate result from hysteresis state
        result.regime = confirmedRegime;
        result.barsInRegime = barsInConfirmedRegime;
        result.stabilityBars = stabilityBars;
        result.candidateRegime = candidateRegime;
        result.candidateConfirmationBars = candidateConfirmationBars;
        result.confirmationProgress = static_cast<double>(candidateConfirmationBars) /
                                      config.minConfirmationBars;
        result.isTransitioning = (candidateRegime != confirmedRegime && candidateConfirmationBars > 0);
        result.isStable = !result.isTransitioning;

        // ---------------------------------------------------------------------
        // Detect Regime Change Events
        // ---------------------------------------------------------------------
        result.regimeChanged = (rawRegime != confirmedRegime);
        result.compressionEntered = (confirmedRegime == VolatilityRegime::COMPRESSION &&
                                     barsInConfirmedRegime == 1);
        result.expansionEntered = (confirmedRegime == VolatilityRegime::EXPANSION &&
                                   barsInConfirmedRegime == 1);
        result.eventDetected = (confirmedRegime == VolatilityRegime::EVENT &&
                                barsInConfirmedRegime == 1);

        // ---------------------------------------------------------------------
        // AUCTION PACE COMPUTATION (Rate of Discovery)
        // ---------------------------------------------------------------------
        // Calculate range velocity (ticks per minute)
        const double barDurationMin = barDurationSec / 60.0;
        const double rangeVelocity = (barDurationMin > 0.001)
            ? barRangeTicks / barDurationMin
            : 0.0;

        result.rangeVelocity = rangeVelocity;

        // Query pace baseline (use synthetic_range_velocity when in synthetic mode)
        const RollingDist& velocityBaseline = useSyntheticBaseline
            ? bucket.synthetic_range_velocity
            : bucket.range_velocity;
        auto velocityPctile = velocityBaseline.TryPercentile(rangeVelocity);

        if (velocityPctile.valid) {
            result.rangeVelocityPercentile = velocityPctile.value;
            result.paceReady = true;

            // Classify raw pace
            AuctionPace rawPace = ClassifyPace(result.rangeVelocityPercentile);
            result.rawPace = rawPace;

            // Apply pace hysteresis (separate from regime)
            UpdatePaceHysteresis(rawPace);

            // Populate result from pace hysteresis state
            result.pace = confirmedPace;
            result.barsInPace = barsInConfirmedPace;
            result.candidatePace = candidatePace;
            result.candidatePaceConfirmationBars = candidatePaceConfirmationBars;
            result.paceConfirmationProgress = static_cast<double>(candidatePaceConfirmationBars) /
                                              config.paceMinConfirmationBars;
            result.isPaceTransitioning = (candidatePace != confirmedPace && candidatePaceConfirmationBars > 0);

            // Detect pace change events
            result.paceChanged = (rawPace != confirmedPace);
            result.slowPaceEntered = (confirmedPace == AuctionPace::SLOW && barsInConfirmedPace == 1);
            result.fastPaceEntered = (confirmedPace == AuctionPace::FAST && barsInConfirmedPace == 1);
            result.extremePaceEntered = (confirmedPace == AuctionPace::EXTREME && barsInConfirmedPace == 1);

            // Update pace session evidence
            switch (rawPace) {
                case AuctionPace::SLOW:    slowPaceBars++; break;
                case AuctionPace::NORMAL:  normalPaceBars++; break;
                case AuctionPace::FAST:    fastPaceBars++; break;
                case AuctionPace::EXTREME: extremePaceBars++; break;
                default: break;
            }
            sessionVelocitySum += rangeVelocity;
        }

        // ---------------------------------------------------------------------
        // Expected Range Multiplier
        // ---------------------------------------------------------------------
        result.expectedRangeMultiplier = GetExpectedMultiplier(confirmedRegime);

        // ---------------------------------------------------------------------
        // Populate Tradability Rules (regime + pace combined)
        // ---------------------------------------------------------------------
        result.tradability = ComputeTradability(confirmedRegime, confirmedPace);

        // ---------------------------------------------------------------------
        // Prior Session Context
        // ---------------------------------------------------------------------
        if (priorReady) {
            result.priorSessionAvgRange = priorAvgRange;
            result.priorSessionVolatility = priorVolatility;
            result.priorReady = true;
            result.sessionsContributed = sessionsContributed;
        }

        // ---------------------------------------------------------------------
        // Update Session Evidence
        // ---------------------------------------------------------------------
        sessionBars++;
        sessionRangeSum += barRangeTicks;
        sessionRangeSqSum += barRangeTicks * barRangeTicks;

        switch (rawRegime) {
            case VolatilityRegime::COMPRESSION: compressionBars++; break;
            case VolatilityRegime::NORMAL:      normalBars++; break;
            case VolatilityRegime::EXPANSION:   expansionBars++; break;
            case VolatilityRegime::EVENT:       eventBars++; break;
            default: break;
        }

        return result;
    }

    // =========================================================================
    // SYNTHETIC BAR COMPUTATION
    // =========================================================================
    // Use this method instead of Compute() when on 1-min charts.
    // It aggregates raw bars into synthetic periods for regime detection.
    //
    // barHigh: Raw bar high price
    // barLow: Raw bar low price
    // barDurationSec: Duration of the bar in seconds
    // tickSize: Tick size for conversion
    // atrValue: ATR value (0 if not available)
    //
    // Returns: VolatilityResult with usingSyntheticBars flag set appropriately
    //
    VolatilityResult ComputeFromRawBar(double barHigh, double barLow, double barDurationSec,
                                        double tickSize, double atrValue = 0.0) {
        rawBarsProcessed++;

        // Always push to aggregator (even if synthetic mode is off, for flexibility)
        syntheticAggregator.Push(barHigh, barLow, barDurationSec);

        if (syntheticModeActive && config.useSyntheticBars) {
            // Synthetic mode: use aggregated data
            if (!syntheticAggregator.IsReady()) {
                // Warmup: not enough bars yet
                VolatilityResult result;
                result.phase = currentPhase;
                result.errorReason = VolatilityErrorReason::WARMUP_SYNTHETIC;
                result.usingSyntheticBars = true;
                result.syntheticAggregationBars = syntheticAggregator.GetAggregationBars();
                result.syntheticBarsCollected = syntheticAggregator.IsReady() ? syntheticAggregator.GetAggregationBars() : rawBarsProcessed;
                return result;
            }

            // Get synthetic values
            const double synthRangeTicks = syntheticAggregator.GetSyntheticRangeTicks(tickSize);
            const double synthDurationSec = syntheticAggregator.GetSyntheticDurationSec();
            const double synthVelocity = syntheticAggregator.GetSyntheticRangeVelocity(tickSize);
            const bool newSynthBar = syntheticAggregator.DidNewSyntheticBarForm();

            // Compute with synthetic data using SYNTHETIC BASELINE
            VolatilityResult result = Compute(synthRangeTicks, synthDurationSec, atrValue,
                                               true /* useSyntheticBaseline */);
            result.usingSyntheticBars = true;
            result.syntheticAggregationBars = syntheticAggregator.GetAggregationBars();
            result.syntheticBarsCollected = syntheticAggregator.GetAggregationBars();
            result.syntheticRangeTicks = synthRangeTicks;
            result.syntheticDurationSec = synthDurationSec;
            result.newSyntheticBarFormed = newSynthBar;
            result.syntheticRangeVelocity = synthVelocity;
            return result;
        } else {
            // Raw mode: compute from individual bar using RAW BASELINE
            const double rawRangeTicks = (tickSize > 0.0) ? (barHigh - barLow) / tickSize : 0.0;
            VolatilityResult result = Compute(rawRangeTicks, barDurationSec, atrValue,
                                               false /* useSyntheticBaseline */);
            result.usingSyntheticBars = false;
            return result;
        }
    }

    // =========================================================================
    // SESSION BOUNDARY METHODS
    // =========================================================================

    // Call at end of session to update priors
    void FinalizeSession() {
        if (sessionBars < 20) return;  // Too short for prior update

        // Calculate session metrics
        const double sessionAvgRange = sessionRangeSum / sessionBars;
        const double sessionVariance = (sessionRangeSqSum / sessionBars) -
                                       (sessionAvgRange * sessionAvgRange);
        const double sessionVolatility = std::sqrt((std::max)(0.0, sessionVariance));

        const double compressionRatio = static_cast<double>(compressionBars) / sessionBars;
        const double expansionRatio = static_cast<double>(expansionBars + eventBars) / sessionBars;

        if (!priorReady) {
            // First valid session
            priorAvgRange = sessionAvgRange;
            priorVolatility = sessionVolatility;
            priorCompressionRatio = compressionRatio;
            priorExpansionRatio = expansionRatio;
            priorReady = true;
            sessionsContributed = 1;
        } else {
            // EWMA update
            priorAvgRange = PRIOR_INERTIA * priorAvgRange + (1.0 - PRIOR_INERTIA) * sessionAvgRange;
            priorVolatility = PRIOR_INERTIA * priorVolatility + (1.0 - PRIOR_INERTIA) * sessionVolatility;
            priorCompressionRatio = PRIOR_INERTIA * priorCompressionRatio + (1.0 - PRIOR_INERTIA) * compressionRatio;
            priorExpansionRatio = PRIOR_INERTIA * priorExpansionRatio + (1.0 - PRIOR_INERTIA) * expansionRatio;
            sessionsContributed++;
        }
    }

    // Call at start of new session
    void ResetForSession() {
        // Regime hysteresis reset
        confirmedRegime = VolatilityRegime::UNKNOWN;
        candidateRegime = VolatilityRegime::UNKNOWN;
        candidateConfirmationBars = 0;
        barsInConfirmedRegime = 0;
        stabilityBars = 0;

        // Pace hysteresis reset
        confirmedPace = AuctionPace::UNKNOWN;
        candidatePace = AuctionPace::UNKNOWN;
        candidatePaceConfirmationBars = 0;
        barsInConfirmedPace = 0;

        // Session evidence reset
        sessionBars = 0;
        compressionBars = 0;
        normalBars = 0;
        expansionBars = 0;
        eventBars = 0;
        sessionRangeSum = 0.0;
        sessionRangeSqSum = 0.0;

        // Pace session evidence reset
        slowPaceBars = 0;
        normalPaceBars = 0;
        fastPaceBars = 0;
        extremePaceBars = 0;
        sessionVelocitySum = 0.0;

        // Synthetic bar aggregator reset
        syntheticAggregator.Reset();
        rawBarsProcessed = 0;

        // priors PRESERVED
    }

    // Full reset (including priors)
    void Reset() {
        ResetForSession();

        priorAvgRange = -1.0;
        priorVolatility = -1.0;
        priorCompressionRatio = -1.0;
        priorExpansionRatio = -1.0;
        priorReady = false;
        sessionsContributed = 0;

        atrBaseline.reset(300);
        lastATRValue = 0.0;
        atrBaselineReady = false;
    }

    // =========================================================================
    // PRE-WARM SUPPORT
    // =========================================================================
    // Call with historical bar data to populate baselines before live trading.

    void PreWarmFromBar(double barRangeTicks, double atrValue, SessionPhase phase) {
        // ATR baseline (engine-local)
        if (atrValue > 0.0 && std::isfinite(atrValue)) {
            atrBaseline.push(atrValue);
            lastATRValue = atrValue;
        }

        // bar_range goes to EffortBaselineStore (caller's responsibility)
        // We just track session stats for prior calculation
        if (barRangeTicks > 0.0 && std::isfinite(barRangeTicks)) {
            sessionBars++;
            sessionRangeSum += barRangeTicks;
            sessionRangeSqSum += barRangeTicks * barRangeTicks;
        }
    }

    // =========================================================================
    // QUERY / DIAGNOSTIC
    // =========================================================================

    struct DiagnosticState {
        VolatilityRegime confirmedRegime;
        VolatilityRegime candidateRegime;
        int candidateConfirmationBars;
        int barsInConfirmedRegime;
        int stabilityBars;
        int sessionBars;
        double priorAvgRange;
        bool priorReady;
        int sessionsContributed;
        size_t atrBaselineSamples;
    };

    DiagnosticState GetDiagnosticState() const {
        DiagnosticState d;
        d.confirmedRegime = confirmedRegime;
        d.candidateRegime = candidateRegime;
        d.candidateConfirmationBars = candidateConfirmationBars;
        d.barsInConfirmedRegime = barsInConfirmedRegime;
        d.stabilityBars = stabilityBars;
        d.sessionBars = sessionBars;
        d.priorAvgRange = priorAvgRange;
        d.priorReady = priorReady;
        d.sessionsContributed = sessionsContributed;
        d.atrBaselineSamples = atrBaseline.size();
        return d;
    }

private:
    // =========================================================================
    // REGIME CLASSIFICATION
    // =========================================================================

    VolatilityRegime ClassifyRegime(double percentile) const {
        if (percentile >= config.eventThreshold) {
            return VolatilityRegime::EVENT;
        }
        if (percentile >= config.expansionThreshold) {
            return VolatilityRegime::EXPANSION;
        }
        if (percentile <= config.compressionThreshold) {
            return VolatilityRegime::COMPRESSION;
        }
        return VolatilityRegime::NORMAL;
    }

    // =========================================================================
    // ASYMMETRIC HYSTERESIS HELPER
    // =========================================================================
    // Returns confirmation bars needed for transition from->to
    // Fast escalation to danger, slow de-escalation to calm

    int GetConfirmationBarsForTransition(VolatilityRegime from, VolatilityRegime to) const {
        // EVENT entry = immediate protection (1 bar)
        if (to == VolatilityRegime::EVENT) {
            return config.eventEntryBars;
        }

        // EVENT exit = confirm calm before relaxing (3 bars)
        if (from == VolatilityRegime::EVENT) {
            return config.eventExitBars;
        }

        // Other transitions = moderate speed (2 bars)
        return config.otherTransitionBars;
    }

    // =========================================================================
    // HYSTERESIS UPDATE (Asymmetric)
    // =========================================================================
    // Fast escalation to EVENT (1 bar), slow exit from EVENT (3 bars)
    // Other transitions use moderate speed (2 bars)

    void UpdateHysteresis(VolatilityRegime rawRegime) {
        // Always increment stability bars if we're in a confirmed regime
        if (confirmedRegime != VolatilityRegime::UNKNOWN) {
            stabilityBars = (std::min)(stabilityBars + 1, config.maxStabilityBars);
        }

        // Initial state: no confirmed regime yet
        if (confirmedRegime == VolatilityRegime::UNKNOWN) {
            if (rawRegime != VolatilityRegime::UNKNOWN) {
                confirmedRegime = rawRegime;
                candidateRegime = rawRegime;
                candidateConfirmationBars = 0;
                barsInConfirmedRegime = 1;
                stabilityBars = 1;
            }
            return;
        }

        barsInConfirmedRegime++;

        // Raw matches confirmed: reinforces current state
        if (rawRegime == confirmedRegime) {
            candidateRegime = confirmedRegime;
            candidateConfirmationBars = 0;
            return;
        }

        // Raw matches candidate: accumulate confirmation
        if (rawRegime == candidateRegime) {
            candidateConfirmationBars++;

            // Get required bars for THIS specific transition (asymmetric)
            const int requiredBars = GetConfirmationBarsForTransition(confirmedRegime, candidateRegime);

            if (candidateConfirmationBars >= requiredBars) {
                // Transition confirmed
                confirmedRegime = candidateRegime;
                barsInConfirmedRegime = 1;
                candidateConfirmationBars = 0;
                stabilityBars = 0;  // Reset stability on regime change
            }
            return;
        }

        // New candidate (different from both confirmed and previous candidate)
        if (rawRegime != VolatilityRegime::UNKNOWN) {
            candidateRegime = rawRegime;
            candidateConfirmationBars = 1;

            // Check for immediate transition (e.g., EVENT entry requires only 1 bar)
            const int requiredBars = GetConfirmationBarsForTransition(confirmedRegime, candidateRegime);
            if (candidateConfirmationBars >= requiredBars) {
                // Immediate transition confirmed
                confirmedRegime = candidateRegime;
                barsInConfirmedRegime = 1;
                candidateConfirmationBars = 0;
                stabilityBars = 0;
            }
        }
    }

    // =========================================================================
    // PACE CLASSIFICATION
    // =========================================================================

    AuctionPace ClassifyPace(double percentile) const {
        if (percentile >= config.extremePaceThreshold) {
            return AuctionPace::EXTREME;
        }
        if (percentile >= config.fastPaceThreshold) {
            return AuctionPace::FAST;
        }
        if (percentile <= config.slowPaceThreshold) {
            return AuctionPace::SLOW;
        }
        return AuctionPace::NORMAL;
    }

    // =========================================================================
    // PACE HYSTERESIS UPDATE
    // =========================================================================
    // Same pattern as regime hysteresis, but separate state.

    void UpdatePaceHysteresis(AuctionPace rawPace) {
        // Initial state: no confirmed pace yet
        if (confirmedPace == AuctionPace::UNKNOWN) {
            if (rawPace != AuctionPace::UNKNOWN) {
                confirmedPace = rawPace;
                candidatePace = rawPace;
                candidatePaceConfirmationBars = 0;
                barsInConfirmedPace = 1;
            }
            return;
        }

        barsInConfirmedPace++;

        // Raw matches confirmed: reinforces current state
        if (rawPace == confirmedPace) {
            candidatePace = confirmedPace;
            candidatePaceConfirmationBars = 0;
            return;
        }

        // Raw matches candidate: accumulate confirmation
        if (rawPace == candidatePace) {
            candidatePaceConfirmationBars++;
            if (candidatePaceConfirmationBars >= config.paceMinConfirmationBars) {
                // Transition confirmed
                confirmedPace = candidatePace;
                barsInConfirmedPace = 1;
                candidatePaceConfirmationBars = 0;
            }
            return;
        }

        // New candidate (different from both confirmed and previous candidate)
        if (rawPace != AuctionPace::UNKNOWN) {
            candidatePace = rawPace;
            candidatePaceConfirmationBars = 1;
        }
    }

    // =========================================================================
    // EXPECTED RANGE MULTIPLIER
    // =========================================================================

    double GetExpectedMultiplier(VolatilityRegime regime) const {
        switch (regime) {
            case VolatilityRegime::COMPRESSION: return config.compressionExpectedMultiplier;
            case VolatilityRegime::NORMAL:      return config.normalExpectedMultiplier;
            case VolatilityRegime::EXPANSION:   return config.expansionExpectedMultiplier;
            case VolatilityRegime::EVENT:       return config.eventExpectedMultiplier;
            default: return 1.0;
        }
    }

    // =========================================================================
    // TRADABILITY RULES (Regime + Pace Combined)
    // =========================================================================

    TradabilityRules ComputeTradability(VolatilityRegime regime, AuctionPace pace) const {
        TradabilityRules rules;

        // Apply regime-based rules
        switch (regime) {
            case VolatilityRegime::COMPRESSION:
                rules.allowNewEntries = !config.compressionBlockNewEntries;
                rules.blockBreakouts = config.compressionBlockBreakouts;
                rules.preferMeanReversion = config.compressionPreferMeanReversion;
                rules.requireHigherConfidence = true;
                rules.positionSizeMultiplier = config.compressionPositionScale;
                break;

            case VolatilityRegime::NORMAL:
                // All defaults: full trading allowed
                break;

            case VolatilityRegime::EXPANSION:
                rules.requireWideStop = config.expansionRequireWideStop;
                rules.positionSizeMultiplier = config.expansionPositionScale;
                break;

            case VolatilityRegime::EVENT:
                rules.allowNewEntries = !config.eventBlockNewEntries;
                rules.requireHigherConfidence = true;
                rules.requireWideStop = true;
                rules.positionSizeMultiplier = config.eventPositionScale;
                break;

            default:
                // UNKNOWN: restrict
                rules.allowNewEntries = false;
                rules.requireHigherConfidence = true;
                break;
        }

        // Apply pace-based multipliers
        switch (pace) {
            case AuctionPace::SLOW:
                rules.paceConfirmationMultiplier = config.slowPaceConfirmationMultiplier;
                rules.paceSizeMultiplier = config.slowPaceSizeMultiplier;
                break;

            case AuctionPace::NORMAL:
                rules.paceConfirmationMultiplier = config.normalPaceConfirmationMultiplier;
                rules.paceSizeMultiplier = config.normalPaceSizeMultiplier;
                break;

            case AuctionPace::FAST:
                rules.paceConfirmationMultiplier = config.fastPaceConfirmationMultiplier;
                rules.paceSizeMultiplier = config.fastPaceSizeMultiplier;
                break;

            case AuctionPace::EXTREME:
                rules.paceConfirmationMultiplier = config.extremePaceConfirmationMultiplier;
                rules.paceSizeMultiplier = config.extremePaceSizeMultiplier;
                break;

            default:
                // UNKNOWN: conservative defaults
                rules.paceConfirmationMultiplier = 1.0;
                rules.paceSizeMultiplier = 1.0;
                break;
        }

        return rules;
    }

    // =========================================================================
    // HELPER: Get Percentile Value from Distribution
    // =========================================================================
    // Returns the value at a given percentile in the distribution

    double GetPercentileValue(const RollingDist& dist, double targetPctile) const {
        if (dist.size() < 2) return 0.0;

        // Sort values to find percentile
        std::vector<double> sorted(dist.values.begin(), dist.values.end());
        std::sort(sorted.begin(), sorted.end());

        // Find index for target percentile
        const double idx = (targetPctile / 100.0) * (sorted.size() - 1);
        const size_t lower = static_cast<size_t>(idx);
        const size_t upper = (std::min)(lower + 1, sorted.size() - 1);

        // Linear interpolation
        const double frac = idx - lower;
        return sorted[lower] * (1.0 - frac) + sorted[upper] * frac;
    }
};

// ============================================================================
// VOLATILITY DECISION INPUT (For BaselineDecisionGate Integration)
// ============================================================================
// Wrapper struct matching the pattern of other decision inputs.

struct VolatilityDecisionInput {
    VolatilityResult result;

    bool IsReady() const { return result.IsReady(); }
    bool IsWarmup() const { return result.IsWarmup(); }

    VolatilityRegime GetRegime() const {
        return IsReady() ? result.regime : VolatilityRegime::UNKNOWN;
    }

    bool IsCompression() const { return result.IsCompression(); }
    bool IsExpansion() const { return result.IsExpansion(); }
    bool IsEvent() const { return result.IsEvent(); }

    bool CanTrade() const { return IsReady() && result.tradability.allowNewEntries; }

    TradabilityRules GetTradability() const { return result.tradability; }
};

} // namespace AMT
