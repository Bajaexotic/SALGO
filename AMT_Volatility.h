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
// TRUE RANGE HELPER (DRY - used by SyntheticBarAggregator and baseline pre-warm)
// ============================================================================
// True Range extends simple high/low to include gap from previous close.
// Critical for capturing overnight gaps at RTH open.
//
// Formula:
//   TrueHigh = max(simpleHigh, prevClose)
//   TrueLow = min(simpleLow, prevClose)
//   TrueRange = TrueHigh - TrueLow
// ============================================================================

inline void ComputeTrueRange(
    double simpleHigh, double simpleLow, double prevClose, bool prevCloseValid,
    double& outTrueHigh, double& outTrueLow)
{
    if (prevCloseValid) {
        outTrueHigh = (std::max)(simpleHigh, prevClose);
        outTrueLow = (std::min)(simpleLow, prevClose);
    } else {
        // No previous close - True Range = simple range
        outTrueHigh = simpleHigh;
        outTrueLow = simpleLow;
    }
}

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
    double close = 0.0;       // For True Range calculation
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

    // True Range tracking (captures gaps between synthetic bars)
    // At RTH open, the gap from GLOBEX close is critical for regime classification
    double prevSyntheticClose_ = 0.0;     // Close of previous synthetic bar
    bool prevSyntheticCloseValid_ = false;
    double syntheticTrueHigh_ = 0.0;      // max(syntheticHigh_, prevSyntheticClose_)
    double syntheticTrueLow_ = 0.0;       // min(syntheticLow_, prevSyntheticClose_)
    double lastCloseInWindow_ = 0.0;      // Close of most recent bar in current window

    // Efficiency Ratio tracking (Kaufman-style chop detection)
    // ER = |Close(end) - Close(start)| / Σ|Close(i) - Close(i-1)|
    // Range: 0.0 (pure chop) to 1.0 (perfect trend)
    double firstCloseInWindow_ = 0.0;     // Close of first bar in window (for net change)
    double pathLengthSum_ = 0.0;          // Σ|close[i] - close[i-1]| (total path traveled)
    double lastPushedClose_ = 0.0;        // Previous bar's close (for path calculation)
    bool lastPushedCloseValid_ = false;   // Track if we have a valid previous close
    int barsInCurrentWindow_ = 0;         // Count bars in current synthetic window

    // Minimum path for valid ER calculation (2 ticks)
    static constexpr double MIN_PATH_FOR_VALID_ER_TICKS = 2.0;

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
     * @param high Bar high price
     * @param low Bar low price
     * @param close Bar close price (for True Range calculation)
     * @param durationSec Bar duration in seconds
     * @return true if this bar completes a new synthetic bar (boundary crossed)
     */
    bool Push(double high, double low, double close, double durationSec) {
        SyntheticBarData& slot = buffer_[writeIdx_];
        slot.high = high;
        slot.low = low;
        slot.close = close;
        slot.durationSec = durationSec;
        slot.valid = true;

        writeIdx_ = (writeIdx_ + 1) % MAX_AGGREGATION_BARS;
        if (validCount_ < MAX_AGGREGATION_BARS) {
            validCount_++;
        }

        // Track synthetic bar boundary
        rawBarCounter_++;
        const bool boundaryReached = (rawBarCounter_ % aggregationBars_ == 0) && IsReady();

        // =========================================================================
        // Efficiency Ratio tracking (Kaufman-style)
        // Track path length within current synthetic window
        // =========================================================================
        if (barsInCurrentWindow_ == 0) {
            // First bar of new synthetic window - record starting close
            firstCloseInWindow_ = close;
            pathLengthSum_ = 0.0;
        } else if (lastPushedCloseValid_) {
            // Subsequent bars - accumulate path length
            pathLengthSum_ += std::abs(close - lastPushedClose_);
        }
        lastPushedClose_ = close;
        lastPushedCloseValid_ = true;
        barsInCurrentWindow_++;

        // Recompute cached values
        ComputeSynthetic();

        // When synthetic bar completes, save close for next True Range calculation
        // and reset efficiency tracking for next window
        if (boundaryReached) {
            prevSyntheticClose_ = lastCloseInWindow_;
            prevSyntheticCloseValid_ = true;

            // Reset efficiency tracking for next synthetic window
            // (but preserve lastPushedClose_ for first path segment of next window)
            barsInCurrentWindow_ = 0;
        }

        newSyntheticBarFormed_ = boundaryReached;
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

    // =========================================================================
    // TRUE RANGE ACCESSORS (captures gaps between synthetic bars)
    // =========================================================================

    /**
     * Get synthetic TRUE RANGE in ticks.
     * True Range extends high/low to include gap from previous synthetic close.
     * Critical for RTH open when overnight gap exists.
     */
    double GetSyntheticTrueRangeTicks(double tickSize) const {
        if (!cacheValid_ || tickSize <= 0.0) return 0.0;
        return (syntheticTrueHigh_ - syntheticTrueLow_) / tickSize;
    }

    /**
     * Get synthetic TRUE RANGE in price units.
     */
    double GetSyntheticTrueRangePrice() const {
        if (!cacheValid_) return 0.0;
        return syntheticTrueHigh_ - syntheticTrueLow_;
    }

    /**
     * Check if True Range includes a gap (true high/low differs from simple high/low).
     */
    bool HasGap() const {
        if (!cacheValid_ || !prevSyntheticCloseValid_) return false;
        return (syntheticTrueHigh_ != syntheticHigh_) || (syntheticTrueLow_ != syntheticLow_);
    }

    /**
     * Get the gap component in ticks (True Range - Simple Range).
     * Positive = gap included in True Range.
     */
    double GetGapTicks(double tickSize) const {
        if (!cacheValid_ || tickSize <= 0.0) return 0.0;
        double simpleRange = syntheticHigh_ - syntheticLow_;
        double trueRange = syntheticTrueHigh_ - syntheticTrueLow_;
        return (trueRange - simpleRange) / tickSize;
    }

    /**
     * Get True Range velocity (ticks per minute, using True Range).
     */
    double GetSyntheticTrueRangeVelocity(double tickSize) const {
        if (!cacheValid_ || tickSize <= 0.0) return 0.0;
        double durationMin = syntheticDurationSec_ / 60.0;
        if (durationMin <= 0.001) return 0.0;
        return GetSyntheticTrueRangeTicks(tickSize) / durationMin;
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
    // EFFICIENCY RATIO ACCESSORS (Kaufman-style chop detection)
    // =========================================================================

    /**
     * Check if efficiency ratio is valid for this synthetic window.
     * Returns false if path length is too small to measure meaningfully.
     * @param tickSize Tick size for minimum path calculation
     */
    bool IsEfficiencyValid(double tickSize) const {
        if (!cacheValid_ || tickSize <= 0.0) return false;
        // Need at least MIN_PATH_FOR_VALID_ER_TICKS of travel to measure efficiency
        return (pathLengthSum_ / tickSize) >= MIN_PATH_FOR_VALID_ER_TICKS;
    }

    /**
     * Get Kaufman Efficiency Ratio for current synthetic window.
     * ER = |Close(end) - Close(start)| / Σ|Close(i) - Close(i-1)|
     * @param tickSize Tick size (used only for validity check)
     * @return 0.0 (pure chop) to 1.0 (perfect trend), 0.5 if invalid
     */
    double GetEfficiencyRatio(double tickSize) const {
        if (!IsEfficiencyValid(tickSize)) return 0.5;  // Neutral when undefined
        if (pathLengthSum_ < 1e-10) return 0.5;  // Avoid div by zero

        double netChange = std::abs(lastCloseInWindow_ - firstCloseInWindow_);
        double er = netChange / pathLengthSum_;

        // Clamp to [0, 1] for safety (should naturally be in range)
        return (std::max)(0.0, (std::min)(1.0, er));
    }

    /**
     * Get net close-to-close change in price units.
     * This is the "progress" component of efficiency.
     */
    double GetNetCloseChange() const {
        if (!cacheValid_) return 0.0;
        return std::abs(lastCloseInWindow_ - firstCloseInWindow_);
    }

    /**
     * Get total path length in price units.
     * This is the "travel" component of efficiency.
     */
    double GetPathLength() const {
        return pathLengthSum_;
    }

    /**
     * Get total path length in ticks.
     */
    double GetPathLengthTicks(double tickSize) const {
        if (tickSize <= 0.0) return 0.0;
        return pathLengthSum_ / tickSize;
    }

    /**
     * Get net close-to-close change in ticks.
     */
    double GetNetChangeTicks(double tickSize) const {
        if (tickSize <= 0.0 || !cacheValid_) return 0.0;
        return std::abs(lastCloseInWindow_ - firstCloseInWindow_) / tickSize;
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

        // True Range state
        prevSyntheticClose_ = 0.0;
        prevSyntheticCloseValid_ = false;
        syntheticTrueHigh_ = 0.0;
        syntheticTrueLow_ = 0.0;
        lastCloseInWindow_ = 0.0;

        // Efficiency Ratio state
        firstCloseInWindow_ = 0.0;
        pathLengthSum_ = 0.0;
        lastPushedClose_ = 0.0;
        lastPushedCloseValid_ = false;
        barsInCurrentWindow_ = 0;
    }

private:
    /**
     * Compute synthetic values from buffer.
     * Uses the most recent aggregationBars_ entries.
     * Also computes True Range extending to previous synthetic bar's close.
     */
    void ComputeSynthetic() {
        if (validCount_ < aggregationBars_) {
            cacheValid_ = false;
            return;
        }

        double maxHigh = -1e30;
        double minLow = 1e30;
        double totalDuration = 0.0;
        double lastClose = 0.0;

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

            // Most recent bar (i=0) provides the close for this synthetic bar
            if (i == 0) {
                lastClose = slot.close;
            }

            idx = (idx - 1 + MAX_AGGREGATION_BARS) % MAX_AGGREGATION_BARS;
        }

        syntheticHigh_ = maxHigh;
        syntheticLow_ = minLow;
        syntheticDurationSec_ = totalDuration;
        lastCloseInWindow_ = lastClose;

        // Compute True Range using DRY helper
        ComputeTrueRange(maxHigh, minLow, prevSyntheticClose_, prevSyntheticCloseValid_,
                         syntheticTrueHigh_, syntheticTrueLow_);

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
// VOLATILITY TREND (Direction of Volatility Change)
// ============================================================================
// Measures whether volatility is expanding, contracting, or stable.
// Uses log ratio of current vs prior synthetic range.
// Symmetric thresholds: +/- 0.18 (~1.2x / 0.83x)

enum class VolatilityTrend : int {
    UNKNOWN = 0,
    CONTRACTING = 1,   // volMomentum < -0.18 (shrinking ranges)
    STABLE = 2,        // -0.18 <= volMomentum <= +0.18
    EXPANDING = 3      // volMomentum > +0.18 (widening ranges)
};

inline const char* VolatilityTrendToString(VolatilityTrend t) {
    switch (t) {
        case VolatilityTrend::UNKNOWN:     return "UNKNOWN";
        case VolatilityTrend::CONTRACTING: return "CONTRACTING";
        case VolatilityTrend::STABLE:      return "STABLE";
        case VolatilityTrend::EXPANDING:   return "EXPANDING";
    }
    return "UNK";
}

// ============================================================================
// VOLATILITY STABILITY (How Consistent is Volatility Itself)
// ============================================================================
// Measures coefficient of variation of recent ranges.
// High CV = vol is whipsawing (dangerous), Low CV = predictable vol.

enum class VolatilityStability : int {
    UNKNOWN = 0,
    UNSTABLE = 1,    // CV > 0.5 - volatility is whipsawing
    MODERATE = 2,    // 0.2 < CV <= 0.5
    STABLE = 3       // CV <= 0.2 - predictable volatility
};

inline const char* VolatilityStabilityToString(VolatilityStability s) {
    switch (s) {
        case VolatilityStability::UNKNOWN:  return "UNKNOWN";
        case VolatilityStability::UNSTABLE: return "UNSTABLE";
        case VolatilityStability::MODERATE: return "MODERATE";
        case VolatilityStability::STABLE:   return "STABLE";
    }
    return "UNK";
}

// ============================================================================
// GAP CONTEXT (DIAGNOSTIC ONLY)
// ============================================================================
// Gap context describes where the market opened relative to prior value area.
// This is computed at SENSOR level (not volatility engine) and injected here.
// Strictly diagnostic - NOT a gate or regime modifier.

enum class GapLocation : int {
    UNKNOWN = 0,      // Gap not determined (before RTH or insufficient data)
    ABOVE_VALUE = 1,  // Open > prior VAH (gap up above value)
    BELOW_VALUE = 2,  // Open < prior VAL (gap down below value)
    IN_VALUE = 3      // Open between prior VAL and VAH (inside value)
};

inline const char* GapLocationToString(GapLocation g) {
    switch (g) {
        case GapLocation::UNKNOWN:     return "UNKNOWN";
        case GapLocation::ABOVE_VALUE: return "ABOVE_VALUE";
        case GapLocation::BELOW_VALUE: return "BELOW_VALUE";
        case GapLocation::IN_VALUE:    return "IN_VALUE";
    }
    return "UNK";
}

enum class EarlyResponse : int {
    UNKNOWN = 0,    // Not determined (too early in session or no gap)
    ACCEPTING = 1,  // Moving further from value (gap acceptance)
    REJECTING = 2,  // Returning toward value (gap rejection)
    UNRESOLVED = 3  // Mixed/ambiguous (no clear direction)
};

inline const char* EarlyResponseToString(EarlyResponse r) {
    switch (r) {
        case EarlyResponse::UNKNOWN:    return "UNKNOWN";
        case EarlyResponse::ACCEPTING:  return "ACCEPTING";
        case EarlyResponse::REJECTING:  return "REJECTING";
        case EarlyResponse::UNRESOLVED: return "UNRESOLVED";
    }
    return "UNK";
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

    // Chop-derived multipliers (Efficiency Ratio based)
    // chopSeverity ranges 0.0 (efficient) to 1.0 (max chop)
    // These use smooth scaling based on chopSeverity rather than discrete states
    double chopSizeMultiplier = 1.0;           // = 1.0 - 0.5 * chopSeverity
    double chopConfirmationMultiplier = 1.0;   // = 1.0 + chopSeverity

    // Convenience check
    bool IsRestricted() const {
        return !allowNewEntries || requireHigherConfidence;
    }

    // Get combined size multiplier (regime × pace × chop)
    double GetCombinedSizeMultiplier() const {
        return positionSizeMultiplier * paceSizeMultiplier * chopSizeMultiplier;
    }

    // Get combined confirmation multiplier (pace × chop)
    double GetCombinedConfirmationMultiplier() const {
        return paceConfirmationMultiplier * chopConfirmationMultiplier;
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

    // TRUE RANGE DIAGNOSTICS (captures gaps)
    double syntheticSimpleRangeTicks = 0.0;    // Simple range (H-L) without gap
    bool syntheticHasGap = false;              // True if True Range > Simple Range
    double syntheticGapTicks = 0.0;            // Gap component in ticks

    // =========================================================================
    // EFFICIENCY RATIO (Kaufman-style chop detection)
    // =========================================================================
    // ER = |Close(end) - Close(start)| / Σ|Close(i) - Close(i-1)|
    // Range: 0.0 (pure chop) to 1.0 (perfect trend)
    //
    // 2x2 Matrix:
    //   High Vol + High ER = Discovery with follow-through (tradeable)
    //   High Vol + Low ER = Violent chop (DANGER - account killer)
    //   Low Vol + High ER = Grindy directional drift (selective)
    //   Low Vol + Low ER = Compression rotation (stand down)

    double efficiencyRatio = 0.5;              // 0.0 (chop) to 1.0 (trend), 0.5 = neutral
    bool efficiencyValid = false;              // False if path too small to measure
    double efficiencyPercentile = 50.0;        // vs phase baseline (when baselined)

    // Chop severity (scalar 0-1 for smooth downstream scaling)
    // chopSeverity = 1.0 - efficiencyRatio (when valid)
    // 0.0 = efficient (trending), 1.0 = max chop
    double chopSeverity = 0.0;
    bool chopActive = false;                   // True when high vol + high chopSeverity

    // Path metrics (for diagnostics)
    double pathLengthTicks = 0.0;              // Total close-to-close travel
    double netChangeTicks = 0.0;               // Net close-to-close change

    // =========================================================================
    // SHOCK DETECTOR (Single-bar Anomaly)
    // =========================================================================
    // Shock = extreme bar (P99+ range or velocity) - orthogonal to regime.
    // Shocks can occur INSIDE any regime. They matter for:
    //   - Slippage risk (execution physics temporarily degraded)
    //   - Signal reliability (confirmations less trustworthy)
    //   - Stop distance (wider stops needed)
    //
    // Aftershock = decay window after shock. Microstructure hangover persists
    // for N synthetic bars (wide spreads, depth discontinuities, erratic fills).

    bool shockFlag = false;                    // True if this bar is a shock (P99+)
    bool aftershockActive = false;             // True if within decay window of a shock
    int barsSinceShock = 999;                  // Bars since last shock (999 = no recent shock)
    double shockMagnitude = 0.0;               // Z-score or percentile of shock (for severity)

    // =========================================================================
    // VOLATILITY MOMENTUM + STABILITY
    // =========================================================================
    // Momentum: Direction of volatility change (expanding/contracting).
    // volMomentum = log(currentRange / priorRange)
    // Symmetric thresholds: +/- 0.18 (~1.2x expansion / 0.83x contraction)
    //
    // Stability: How consistent is volatility itself?
    // CV = stddev / mean of recent ranges.
    // High CV = vol is whipsawing (dangerous), Low CV = predictable.

    VolatilityTrend volTrend = VolatilityTrend::UNKNOWN;
    double volMomentum = 0.0;                  // Log ratio (+ = expanding, - = contracting)
    bool volMomentumValid = false;             // False if prior range not available

    VolatilityStability volStability = VolatilityStability::UNKNOWN;
    double volCV = 0.0;                        // Coefficient of variation of recent ranges
    bool stabilityValid = false;               // False if mean too small for CV
    double stabilityConfidenceMultiplier = 1.0; // 0.7 for UNSTABLE, 1.0 otherwise

    // =========================================================================
    // MINIMUM STOP DISTANCE (Physics Constraint)
    // =========================================================================
    // Physics-based floor for stop distance - not a strategy, a constraint.
    //
    // minStopTicks = p75RangeTicks * paceMultiplier * regimeMultiplier
    //
    // CRITICAL INVARIANT: If the physics-based stop floor exceeds the structural
    // invalidation stop for the setup, THE TRADE IS INADMISSIBLE. Don't widen
    // the stop beyond structural invalidation - skip the trade entirely.
    //
    // This keeps stop guidance as a CONSTRAINT, not a suggestion that turns
    // bad trades into "tradable" ones by stretching risk.

    struct StopGuidance {
        double minStopTicks = 0.0;             // Absolute floor - structural stop MUST exceed this
        double suggestedTicks = 0.0;           // Comfortable buffer (1.5x floor)
        bool isConstraintActive = false;       // True if calculated (not warmup)
        double paceMultiplier = 1.0;           // For diagnostics
        double regimeMultiplier = 1.0;         // For diagnostics
        double shockMultiplier = 1.0;          // For diagnostics
        double baseRangeTicks = 0.0;           // P75 range ticks (base)

        // Helper for admissibility check
        // Returns true if the structural stop is wide enough to be physics-safe
        bool IsAdmissible(double structuralStopTicks) const {
            if (!isConstraintActive) return true;  // No constraint = admissible
            return structuralStopTicks >= minStopTicks;
        }

        // Returns reason string if inadmissible (for logging)
        const char* GetInadmissibleReason(double structuralStopTicks) const {
            if (!isConstraintActive) return nullptr;
            if (structuralStopTicks < minStopTicks) {
                return "structural_stop_below_physics_floor";
            }
            return nullptr;
        }
    } stopGuidance;

    // =========================================================================
    // GAP CONTEXT (DIAGNOSTIC ONLY - Injected by Sensor)
    // =========================================================================
    // These fields are populated by AuctionSensor at RTH open, not by the
    // volatility engine. They describe where the market opened relative to
    // prior value area and early response behavior.
    //
    // NOTE: Strictly diagnostic. NOT a gate or regime modifier. Do not use
    // these to block or modify trades - they are for observational logging only.

    GapLocation gapLocation = GapLocation::UNKNOWN;
    EarlyResponse gapResponse = EarlyResponse::UNKNOWN;
    double gapFromValueTicks = 0.0;     // Distance from VAH (if above) or VAL (if below)
    int barsIntoSession = 0;            // For tracking early response window

    // Setters for sensor injection
    void SetGapContext(GapLocation loc, double distTicks) {
        gapLocation = loc;
        gapFromValueTicks = distTicks;
    }

    void SetGapResponse(EarlyResponse resp, int bars) {
        gapResponse = resp;
        barsIntoSession = bars;
    }

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

    // =========================================================================
    // SHOCK ACCESSORS
    // =========================================================================

    // True if shock or aftershock active (execution physics degraded)
    bool IsShockOrAftershock() const { return shockFlag || aftershockActive; }

    // True if currently in shock (this bar, not aftershock)
    bool IsShock() const { return shockFlag; }

    // True if in aftershock decay window (not shock, but recent shock)
    bool IsAftershock() const { return aftershockActive && !shockFlag; }

    // Get size multiplier for shock conditions
    // Shock = 50% reduction, Aftershock = 25% reduction
    double GetShockSizeMultiplier() const {
        if (shockFlag) return 0.5;
        if (aftershockActive) return 0.75;
        return 1.0;
    }

    // =========================================================================
    // STOP GUIDANCE ACCESSORS
    // =========================================================================

    // Check if stop guidance is ready
    bool IsStopGuidanceReady() const { return stopGuidance.isConstraintActive; }

    // Get minimum stop in ticks
    double GetMinStopTicks() const { return stopGuidance.minStopTicks; }

    // Get suggested stop in ticks (1.5x floor)
    double GetSuggestedStopTicks() const { return stopGuidance.suggestedTicks; }

    // Check if a structural stop is admissible (physics-safe)
    bool IsStopAdmissible(double structuralStopTicks) const {
        return stopGuidance.IsAdmissible(structuralStopTicks);
    }

    // Get comprehensive size multiplier including all conditions
    // Combines: regime × pace × chop × shock × stability
    double GetFullSizeMultiplier() const {
        double mult = tradability.positionSizeMultiplier;      // Regime base
        mult *= tradability.paceSizeMultiplier;                // Pace adjustment
        mult *= tradability.chopSizeMultiplier;                // Chop adjustment
        mult *= GetShockSizeMultiplier();                      // Shock adjustment
        mult *= stabilityConfidenceMultiplier;                 // Stability adjustment
        return mult;
    }

    // Get comprehensive confirmation multiplier including all conditions
    double GetFullConfirmationMultiplier() const {
        double mult = tradability.paceConfirmationMultiplier;  // Pace base
        mult *= tradability.chopConfirmationMultiplier;        // Chop adjustment
        return mult;
    }

    // =========================================================================
    // GAP CONTEXT ACCESSORS (Diagnostic Only)
    // =========================================================================

    bool HasGapContext() const { return gapLocation != GapLocation::UNKNOWN; }
    bool IsGapUp() const { return gapLocation == GapLocation::ABOVE_VALUE; }
    bool IsGapDown() const { return gapLocation == GapLocation::BELOW_VALUE; }
    bool IsInValue() const { return gapLocation == GapLocation::IN_VALUE; }

    bool IsGapAccepting() const { return gapResponse == EarlyResponse::ACCEPTING; }
    bool IsGapRejecting() const { return gapResponse == EarlyResponse::REJECTING; }
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
    // SHOCK DETECTOR STATE
    // =========================================================================
    // Shock = single-bar extreme (P99+ range or velocity)
    // Aftershock = decay window after shock (microstructure hangover)
    int barsSinceLastShock_ = 999;      // Large = no recent shock
    static constexpr int AFTERSHOCK_DECAY_BARS = 3;  // 3 synthetic bars = ~15 min decay
    static constexpr double SHOCK_PERCENTILE_THRESHOLD = 99.0;  // P99 = shock

    // =========================================================================
    // VOLATILITY MOMENTUM + STABILITY STATE
    // =========================================================================
    // Momentum tracking
    double priorSyntheticRange_ = 0.0;      // Previous synthetic bar range
    bool priorSyntheticRangeValid_ = false; // True after first synthetic bar

    // Stability tracking (small rolling window)
    RollingDist recentVolatility_;          // Recent synthetic ranges for CV calculation
    static constexpr int STABILITY_WINDOW = 10;     // Track 10 synthetic bars
    static constexpr double VOL_MOMENTUM_THRESHOLD = 0.18;  // +/- 0.18 = symmetric
    static constexpr double MIN_MEAN_FOR_CV_TICKS = 3.0;    // Min mean for valid CV

    // =========================================================================
    // CONSTRUCTOR / INITIALIZATION
    // =========================================================================

    VolatilityEngine() {
        atrBaseline.reset(300);  // Track 300 bars of ATR
        syntheticAggregator.SetAggregationBars(config.syntheticAggregationBars);
        recentVolatility_.reset(STABILITY_WINDOW);  // Track recent synthetic ranges
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
    VolatilityResult ComputeFromRawBar(double barHigh, double barLow, double barClose,
                                        double barDurationSec, double tickSize,
                                        double atrValue = 0.0) {
        rawBarsProcessed++;

        // Always push to aggregator (even if synthetic mode is off, for flexibility)
        // Include close for True Range calculation
        syntheticAggregator.Push(barHigh, barLow, barClose, barDurationSec);

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

            // Get synthetic values - USE TRUE RANGE for regime classification
            // This captures overnight gaps at RTH open
            const double synthTrueRangeTicks = syntheticAggregator.GetSyntheticTrueRangeTicks(tickSize);
            const double synthSimpleRangeTicks = syntheticAggregator.GetSyntheticRangeTicks(tickSize);
            const double synthDurationSec = syntheticAggregator.GetSyntheticDurationSec();
            const double synthVelocity = syntheticAggregator.GetSyntheticTrueRangeVelocity(tickSize);
            const bool newSynthBar = syntheticAggregator.DidNewSyntheticBarForm();
            const bool hasGap = syntheticAggregator.HasGap();
            const double gapTicks = syntheticAggregator.GetGapTicks(tickSize);

            // Compute with TRUE RANGE using SYNTHETIC BASELINE
            VolatilityResult result = Compute(synthTrueRangeTicks, synthDurationSec, atrValue,
                                               true /* useSyntheticBaseline */);
            result.usingSyntheticBars = true;
            result.syntheticAggregationBars = syntheticAggregator.GetAggregationBars();
            result.syntheticBarsCollected = syntheticAggregator.GetAggregationBars();
            result.syntheticRangeTicks = synthTrueRangeTicks;  // Report True Range
            result.syntheticDurationSec = synthDurationSec;
            result.newSyntheticBarFormed = newSynthBar;
            result.syntheticRangeVelocity = synthVelocity;

            // True Range diagnostics
            result.syntheticSimpleRangeTicks = synthSimpleRangeTicks;
            result.syntheticHasGap = hasGap;
            result.syntheticGapTicks = gapTicks;

            // EFFICIENCY RATIO (Kaufman-style chop detection)
            // Computed from synthetic window: net change / total path
            result.efficiencyValid = syntheticAggregator.IsEfficiencyValid(tickSize);
            result.efficiencyRatio = syntheticAggregator.GetEfficiencyRatio(tickSize);
            result.pathLengthTicks = syntheticAggregator.GetPathLengthTicks(tickSize);
            result.netChangeTicks = syntheticAggregator.GetNetChangeTicks(tickSize);

            // Query efficiency percentile from phase-aware baseline
            if (result.efficiencyValid && effortStore != nullptr) {
                const auto& bucket = effortStore->Get(currentPhase);
                auto erPctile = bucket.synthetic_efficiency.TryPercentile(result.efficiencyRatio);
                if (erPctile.valid) {
                    result.efficiencyPercentile = erPctile.value;
                }
            }

            // chopSeverity = 1 - ER (when valid, else 0.5 = neutral)
            if (result.efficiencyValid) {
                result.chopSeverity = 1.0 - result.efficiencyRatio;
            } else {
                result.chopSeverity = 0.5;  // Neutral when undefined
            }

            // chopActive = high volatility + high chop severity
            // High vol = EXPANSION or EVENT regime
            // High chop = chopSeverity > 0.6 (i.e., ER < 0.4)
            const bool highVol = (result.regime == VolatilityRegime::EXPANSION ||
                                  result.regime == VolatilityRegime::EVENT);
            const bool highChop = (result.efficiencyValid && result.chopSeverity > 0.6);
            result.chopActive = highVol && highChop;

            // Chop-scaled multipliers for tradability
            // Size reduction: scale down position when chop is high
            // Formula: 1.0 - 0.5 * chopSeverity (so at max chop, 50% reduction)
            result.tradability.chopSizeMultiplier = 1.0 - 0.5 * result.chopSeverity;

            // Confirmation increase: require more confirmation when chop is high
            // Formula: 1.0 + chopSeverity (so at max chop, 2x confirmation)
            result.tradability.chopConfirmationMultiplier = 1.0 + result.chopSeverity;

            // SHOCK DETECTION (P99+ range or velocity = shock)
            // Shocks are orthogonal to regime - can occur inside any regime
            // Note: Only update shock counter when a new synthetic bar forms
            const double maxPctile = (std::max)(result.rangePercentile,
                                                 result.rangeVelocityPercentile);
            result.shockFlag = (result.rangeReady && maxPctile >= SHOCK_PERCENTILE_THRESHOLD);

            if (newSynthBar) {
                // Only update counter on synthetic bar boundaries
                if (result.shockFlag) {
                    result.shockMagnitude = maxPctile;
                    barsSinceLastShock_ = 0;
                } else if (barsSinceLastShock_ < 999) {
                    barsSinceLastShock_++;
                }
            } else if (result.shockFlag) {
                // Shock can still be detected mid-window (based on current aggregate)
                result.shockMagnitude = maxPctile;
            }

            result.barsSinceShock = barsSinceLastShock_;
            result.aftershockActive = (barsSinceLastShock_ <= AFTERSHOCK_DECAY_BARS);

            // VOLATILITY MOMENTUM + STABILITY (only update on synthetic bar boundaries)
            if (newSynthBar && synthTrueRangeTicks > 0.001) {
                // Momentum: log ratio of current vs prior range
                if (priorSyntheticRangeValid_ && priorSyntheticRange_ > 0.001) {
                    result.volMomentum = std::log(synthTrueRangeTicks / priorSyntheticRange_);
                    result.volMomentumValid = true;

                    // Classify trend (symmetric thresholds)
                    if (result.volMomentum > VOL_MOMENTUM_THRESHOLD) {
                        result.volTrend = VolatilityTrend::EXPANDING;
                    } else if (result.volMomentum < -VOL_MOMENTUM_THRESHOLD) {
                        result.volTrend = VolatilityTrend::CONTRACTING;
                    } else {
                        result.volTrend = VolatilityTrend::STABLE;
                    }
                }

                // Update prior for next bar
                priorSyntheticRange_ = synthTrueRangeTicks;
                priorSyntheticRangeValid_ = true;

                // Stability: CV of recent ranges (with mean floor guardrail)
                recentVolatility_.push(synthTrueRangeTicks);
                if (recentVolatility_.size() >= 5) {
                    const double meanRange = recentVolatility_.mean();

                    // Only compute CV if mean is meaningful (avoids CV explosion on tiny moves)
                    if (meanRange >= MIN_MEAN_FOR_CV_TICKS) {
                        // Compute stddev manually (RollingDist doesn't have stddev)
                        double sumSq = 0.0;
                        for (double v : recentVolatility_.values) {
                            double diff = v - meanRange;
                            sumSq += diff * diff;
                        }
                        double variance = sumSq / static_cast<double>(recentVolatility_.size());
                        double stddev = std::sqrt(variance);

                        result.volCV = stddev / meanRange;
                        result.stabilityValid = true;

                        // Classify stability
                        if (result.volCV > 0.5) {
                            result.volStability = VolatilityStability::UNSTABLE;
                            result.stabilityConfidenceMultiplier = 0.7;  // Reduce confidence
                        } else if (result.volCV > 0.2) {
                            result.volStability = VolatilityStability::MODERATE;
                            result.stabilityConfidenceMultiplier = 1.0;
                        } else {
                            result.volStability = VolatilityStability::STABLE;
                            result.stabilityConfidenceMultiplier = 1.0;
                        }
                    }
                }
            }

            // =================================================================
            // STOP GUIDANCE COMPUTATION (Synthetic Mode)
            // =================================================================
            // Physics-based stop floor: p75Range × pace × regime × shock
            // This is a CONSTRAINT, not a suggestion.

            if (result.IsReady() && result.p75RangeTicks > 0.0) {
                // Base: 75th percentile of recent ranges
                result.stopGuidance.baseRangeTicks = result.p75RangeTicks;

                // Pace adjustment
                result.stopGuidance.paceMultiplier = 1.0;
                if (result.pace == AuctionPace::FAST) {
                    result.stopGuidance.paceMultiplier = 1.3;
                } else if (result.pace == AuctionPace::EXTREME) {
                    result.stopGuidance.paceMultiplier = 1.5;
                }

                // Regime adjustment (requireWideStop flag)
                result.stopGuidance.regimeMultiplier = result.tradability.requireWideStop ? 1.5 : 1.0;

                // Shock/aftershock adjustment
                result.stopGuidance.shockMultiplier = 1.0;
                if (result.shockFlag) {
                    result.stopGuidance.shockMultiplier = 1.5;
                } else if (result.aftershockActive) {
                    result.stopGuidance.shockMultiplier = 1.2;
                }

                // Compute minimum stop floor
                result.stopGuidance.minStopTicks = result.stopGuidance.baseRangeTicks
                    * result.stopGuidance.paceMultiplier
                    * result.stopGuidance.regimeMultiplier
                    * result.stopGuidance.shockMultiplier;

                // Suggested = 1.5x minimum (comfortable buffer)
                result.stopGuidance.suggestedTicks = result.stopGuidance.minStopTicks * 1.5;

                result.stopGuidance.isConstraintActive = true;
            }

            return result;
        } else {
            // Raw mode: compute from individual bar using RAW BASELINE
            const double rawRangeTicks = (tickSize > 0.0) ? (barHigh - barLow) / tickSize : 0.0;
            VolatilityResult result = Compute(rawRangeTicks, barDurationSec, atrValue,
                                               false /* useSyntheticBaseline */);
            result.usingSyntheticBars = false;

            // Efficiency ratio not meaningful in raw mode (no synthetic window)
            // Set neutral/undefined values
            result.efficiencyValid = false;
            result.efficiencyRatio = 0.5;  // Neutral
            result.chopSeverity = 0.5;     // Neutral
            result.chopActive = false;
            result.pathLengthTicks = 0.0;
            result.netChangeTicks = 0.0;
            result.tradability.chopSizeMultiplier = 1.0;         // No adjustment
            result.tradability.chopConfirmationMultiplier = 1.0; // No adjustment

            // SHOCK DETECTION (works in raw mode too)
            const double maxPctile = (std::max)(result.rangePercentile,
                                                 result.rangeVelocityPercentile);
            result.shockFlag = (result.rangeReady && maxPctile >= SHOCK_PERCENTILE_THRESHOLD);

            if (result.shockFlag) {
                result.shockMagnitude = maxPctile;
                barsSinceLastShock_ = 0;
            } else if (barsSinceLastShock_ < 999) {
                barsSinceLastShock_++;
            }

            result.barsSinceShock = barsSinceLastShock_;
            result.aftershockActive = (barsSinceLastShock_ <= AFTERSHOCK_DECAY_BARS);

            // Momentum/stability not meaningful in raw mode (no prior tracking)
            result.volMomentumValid = false;
            result.volTrend = VolatilityTrend::UNKNOWN;
            result.stabilityValid = false;
            result.volStability = VolatilityStability::UNKNOWN;
            result.stabilityConfidenceMultiplier = 1.0;

            // STOP GUIDANCE (works in raw mode too, uses same formula)
            if (result.IsReady() && result.p75RangeTicks > 0.0) {
                result.stopGuidance.baseRangeTicks = result.p75RangeTicks;

                result.stopGuidance.paceMultiplier = 1.0;
                if (result.pace == AuctionPace::FAST) {
                    result.stopGuidance.paceMultiplier = 1.3;
                } else if (result.pace == AuctionPace::EXTREME) {
                    result.stopGuidance.paceMultiplier = 1.5;
                }

                result.stopGuidance.regimeMultiplier = result.tradability.requireWideStop ? 1.5 : 1.0;

                result.stopGuidance.shockMultiplier = 1.0;
                if (result.shockFlag) {
                    result.stopGuidance.shockMultiplier = 1.5;
                } else if (result.aftershockActive) {
                    result.stopGuidance.shockMultiplier = 1.2;
                }

                result.stopGuidance.minStopTicks = result.stopGuidance.baseRangeTicks
                    * result.stopGuidance.paceMultiplier
                    * result.stopGuidance.regimeMultiplier
                    * result.stopGuidance.shockMultiplier;

                result.stopGuidance.suggestedTicks = result.stopGuidance.minStopTicks * 1.5;
                result.stopGuidance.isConstraintActive = true;
            }

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

        // Shock detector reset
        barsSinceLastShock_ = 999;  // No recent shocks

        // Volatility momentum/stability reset
        priorSyntheticRange_ = 0.0;
        priorSyntheticRangeValid_ = false;
        recentVolatility_.reset(STABILITY_WINDOW);

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
