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
    SESSION_RESET = 40               // Session just reset, no delta history
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
    // VALIDITY / ERROR
    // =========================================================================
    DeltaErrorReason errorReason = DeltaErrorReason::NONE;
    SessionPhase phase = SessionPhase::UNKNOWN;
    int bar = -1;

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

    DeltaAlignment confirmedAlignment_ = DeltaAlignment::UNKNOWN;
    DeltaAlignment candidateAlignment_ = DeltaAlignment::UNKNOWN;
    int alignmentConfirmBars_ = 0;

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
        confirmedAlignment_ = DeltaAlignment::UNKNOWN;
        candidateAlignment_ = DeltaAlignment::UNKNOWN;
        alignmentConfirmBars_ = 0;
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

        // Apply hysteresis
        if (rawCharacter != candidateCharacter_) {
            candidateCharacter_ = rawCharacter;
            characterConfirmBars_ = 1;
        } else {
            characterConfirmBars_++;
        }

        if (characterConfirmBars_ >= config.characterConfirmBars) {
            if (confirmedCharacter_ != candidateCharacter_) {
                result.characterChanged = true;
            }
            confirmedCharacter_ = candidateCharacter_;
        }

        result.character = confirmedCharacter_;
        result.barsInCharacter = characterConfirmBars_;
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

        // Apply hysteresis
        if (rawAlignment != candidateAlignment_) {
            candidateAlignment_ = rawAlignment;
            alignmentConfirmBars_ = 1;
        } else {
            alignmentConfirmBars_++;
        }

        if (alignmentConfirmBars_ >= config.alignmentConfirmBars) {
            if (confirmedAlignment_ != candidateAlignment_) {
                result.alignmentChanged = true;
            }
            confirmedAlignment_ = candidateAlignment_;
        }

        result.alignment = confirmedAlignment_;
        result.barsInAlignment = alignmentConfirmBars_;

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
