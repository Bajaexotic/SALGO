#pragma once
// ============================================================================
// AMT_ImbalanceDeltaPatterns.h - Group 4: Imbalance Delta Pattern Detection
// ============================================================================
//
// This module detects delta/effort patterns specific to IMBALANCE regime
// (TPO_SEPARATION). These patterns describe healthy trends, continuation
// hints, reversal warnings, and exhaustion/capitulation signals.
//
// Patterns:
//   STRONG_CONVERGENCE  - Delta direction matches price progress (healthy trend)
//   WEAK_PULLBACK       - Price retraces but delta doesn't reverse (add-on signal)
//   EFFORT_NO_RESULT    - High effort, no price progress (reversal warning)
//   CLIMAX_EXHAUSTION   - Extreme effort + Group 1 confirmers (capitulation)
//
// Prerequisites:
//   - TPO_SEPARATION regime (imbalance)
//   - DomHistoryBuffer with sufficient samples
//   - DomEventFeatures for delta statistics
//   - Group 1 outputs for CLIMAX_EXHAUSTION confirmation
//
// ============================================================================

#include "AMT_Patterns.h"
#include "AMT_DomEvents.h"
#include <vector>
#include <optional>
#include <algorithm>
#include <cmath>
#include <string>

namespace AMT {

// ============================================================================
// CONFIGURATION
// ============================================================================

struct ImbalanceDeltaConfig
{
    // Regime: require imbalance (TPO_SEPARATION)
    static constexpr bool REQUIRE_TPO_SEPARATION = true;

    // STRONG_CONVERGENCE thresholds
    static constexpr int CONVERGENCE_MIN_TREND_BARS = 3;     // Min bars in trend
    static constexpr int CONVERGENCE_MIN_PRICE_TICKS = 2;    // Min price progress
    static constexpr double CONVERGENCE_DELTA_K = 1.0;       // Delta z-score threshold

    // WEAK_PULLBACK thresholds
    static constexpr int PULLBACK_MIN_RETRACE_TICKS = 1;     // Min retrace
    static constexpr int PULLBACK_MAX_RETRACE_TICKS = 4;     // Max retrace (still continuation)
    static constexpr double PULLBACK_DELTA_MIN_K = 0.5;      // Delta must not reverse strongly

    // EFFORT_NO_RESULT thresholds
    static constexpr double EFFORT_DELTA_K = 2.0;            // High effort z-score
    static constexpr int EFFORT_MAX_PRICE_TICKS = 1;         // Max price movement (stalled)

    // CLIMAX_EXHAUSTION thresholds
    static constexpr double CLIMAX_DELTA_K = 2.5;            // Extreme effort z-score
    static constexpr int CLIMAX_CONFIRM_REQUIRED = 1;        // At least 1 confirmer from Group 1
};

// ============================================================================
// TREND PROGRESS TRACKER (Session-Scoped State)
// ============================================================================
// Tracks trend progress for convergence/pullback detection.
// Must be reset at session boundaries.

struct TrendProgressTracker
{
    // Trend direction: +1 = uptrend, -1 = downtrend, 0 = no trend
    int trendDirection = 0;

    // High-water and low-water marks (in ticks)
    int highWaterTick = 0;
    int lowWaterTick = 0;

    // Trend start bar and current duration
    int trendStartBar = 0;
    int trendDurationBars = 0;

    // Last known mid-price (for retrace detection)
    int lastMidTick = 0;

    // Peak/trough tracking for pullback detection
    int peakTick = 0;    // Highest point in uptrend before retrace
    int troughTick = 0;  // Lowest point in downtrend before retrace

    void Reset()
    {
        trendDirection = 0;
        highWaterTick = 0;
        lowWaterTick = 0;
        trendStartBar = 0;
        trendDurationBars = 0;
        lastMidTick = 0;
        peakTick = 0;
        troughTick = 0;
    }

    // Update trend state based on current price
    void Update(int currentMidTick, int currentBar)
    {
        if (trendDirection == 0)
        {
            // No trend yet, initialize
            highWaterTick = currentMidTick;
            lowWaterTick = currentMidTick;
            peakTick = currentMidTick;
            troughTick = currentMidTick;
            lastMidTick = currentMidTick;
            return;
        }

        // Update high/low water marks
        if (currentMidTick > highWaterTick)
        {
            highWaterTick = currentMidTick;
            if (trendDirection > 0) peakTick = currentMidTick;  // New peak in uptrend
        }
        if (currentMidTick < lowWaterTick)
        {
            lowWaterTick = currentMidTick;
            if (trendDirection < 0) troughTick = currentMidTick;  // New trough in downtrend
        }

        // Track duration
        trendDurationBars = currentBar - trendStartBar + 1;

        lastMidTick = currentMidTick;
    }

    // Establish trend direction (called when price breaks out)
    void EstablishTrend(int direction, int currentMidTick, int currentBar)
    {
        trendDirection = direction;
        trendStartBar = currentBar;
        trendDurationBars = 1;
        highWaterTick = currentMidTick;
        lowWaterTick = currentMidTick;
        peakTick = currentMidTick;
        troughTick = currentMidTick;
        lastMidTick = currentMidTick;
    }

    // Calculate retrace from peak/trough (for pullback detection)
    int GetRetraceTicks() const
    {
        if (trendDirection > 0)
        {
            // Uptrend: retrace = peak - current
            return peakTick - lastMidTick;
        }
        else if (trendDirection < 0)
        {
            // Downtrend: retrace = current - trough
            return lastMidTick - troughTick;
        }
        return 0;
    }

    // Check if we're in a pullback (price retraced but still within continuation zone)
    bool IsInPullback(int minRetrace, int maxRetrace) const
    {
        const int retrace = GetRetraceTicks();
        return retrace >= minRetrace && retrace <= maxRetrace;
    }
};

// ============================================================================
// IMBALANCE DELTA FEATURES
// ============================================================================

struct ImbalanceDeltaFeatures
{
    // Eligibility
    bool isEligible = false;
    const char* ineligibleReason = nullptr;

    // Regime check result
    bool inImbalanceRegime = false;

    // Delta statistics (from DomEventFeatures)
    double deltaSecMedian = 0.0;
    double deltaSecMad = 0.0;
    double deltaSecCurrent = 0.0;
    double deltaSecZScore = 0.0;
    bool deltaStatsValid = false;

    // Absolute delta impulse
    double deltaImpulse = 0.0;  // |deltaSecZScore|

    // Delta direction
    int deltaSign = 0;  // +1 = buy, -1 = sell

    // Price movement
    int netPriceMoveTicks = 0;
    int priceDirection = 0;  // +1, -1, 0

    // Current price position
    int currentMidTick = 0;

    // Trend state (from tracker)
    int trendDirection = 0;
    int trendDurationBars = 0;
    int retraceTicks = 0;
    bool isInPullback = false;

    // Group 1 confirmation flags
    bool hasFlowReversal = false;
    bool hasSweepLiquidation = false;
    bool hasExhaustionDivergence = false;
    int confirmationCount = 0;
};

// ============================================================================
// FEATURE COMPUTATION
// ============================================================================

inline ImbalanceDeltaFeatures ComputeImbalanceDeltaFeatures(
    const DomHistoryBuffer& buffer,
    const DomEventFeatures& baseFeatures,
    const TrendProgressTracker& trendTracker,
    const std::vector<TPOMechanics>& tpoMechanics,
    const std::vector<DOMEvent>& domEvents,
    const std::vector<DOMControlPattern>& domControlPatterns)
{
    ImbalanceDeltaFeatures f;

    // === Regime gate ===
    if (ImbalanceDeltaConfig::REQUIRE_TPO_SEPARATION)
    {
        auto it = std::find(tpoMechanics.begin(), tpoMechanics.end(), TPOMechanics::TPO_SEPARATION);
        f.inImbalanceRegime = (it != tpoMechanics.end());
        if (!f.inImbalanceRegime)
        {
            f.isEligible = false;
            f.ineligibleReason = "NOT_IMBALANCE";
            return f;
        }
    }
    else
    {
        f.inImbalanceRegime = true;  // No regime gating
    }

    // === Check base eligibility ===
    if (!baseFeatures.isEligible)
    {
        f.isEligible = false;
        f.ineligibleReason = "BASE_INELIGIBLE";
        return f;
    }

    if (buffer.Size() < 5)
    {
        f.isEligible = false;
        f.ineligibleReason = "INSUFFICIENT_SAMPLES";
        return f;
    }

    if (!baseFeatures.deltaSecStatsValid)
    {
        f.isEligible = false;
        f.ineligibleReason = "DELTA_STATS_INVALID";
        return f;
    }

    f.isEligible = true;

    // === Copy delta stats ===
    f.deltaSecMedian = baseFeatures.deltaSecMedian;
    f.deltaSecMad = baseFeatures.deltaSecMad;
    f.deltaSecCurrent = baseFeatures.deltaSecCurrent;
    f.deltaSecZScore = baseFeatures.deltaSecZScore;
    f.deltaStatsValid = true;
    f.deltaImpulse = std::abs(f.deltaSecZScore);

    // Delta sign
    f.deltaSign = (f.deltaSecCurrent > 0) ? 1 : ((f.deltaSecCurrent < 0) ? -1 : 0);

    // === Price movement from buffer ===
    const auto& oldest = buffer.samples.front();
    const auto& current = buffer.samples.back();
    f.currentMidTick = (current.bestBidTick + current.bestAskTick) / 2;
    int oldestMidTick = (oldest.bestBidTick + oldest.bestAskTick) / 2;
    f.netPriceMoveTicks = f.currentMidTick - oldestMidTick;
    f.priceDirection = (f.netPriceMoveTicks > 0) ? 1 : ((f.netPriceMoveTicks < 0) ? -1 : 0);

    // === Trend state from tracker ===
    f.trendDirection = trendTracker.trendDirection;
    f.trendDurationBars = trendTracker.trendDurationBars;
    f.retraceTicks = trendTracker.GetRetraceTicks();
    f.isInPullback = trendTracker.IsInPullback(
        ImbalanceDeltaConfig::PULLBACK_MIN_RETRACE_TICKS,
        ImbalanceDeltaConfig::PULLBACK_MAX_RETRACE_TICKS
    );

    // === Group 1 confirmers ===
    for (const auto& ev : domEvents)
    {
        if (ev == DOMEvent::ORDER_FLOW_REVERSAL) f.hasFlowReversal = true;
        if (ev == DOMEvent::SWEEP_LIQUIDATION) f.hasSweepLiquidation = true;
    }
    for (const auto& pat : domControlPatterns)
    {
        if (pat == DOMControlPattern::EXHAUSTION_DIVERGENCE) f.hasExhaustionDivergence = true;
    }
    f.confirmationCount = (f.hasFlowReversal ? 1 : 0) +
                          (f.hasSweepLiquidation ? 1 : 0) +
                          (f.hasExhaustionDivergence ? 1 : 0);

    return f;
}

// ============================================================================
// INDIVIDUAL PATTERN DETECTORS
// ============================================================================

// STRONG_CONVERGENCE: Delta direction matches price progress (healthy trend)
inline std::optional<ImbalanceDeltaHit> DetectStrongConvergence(
    const ImbalanceDeltaFeatures& f,
    int windowMs)
{
    if (!f.isEligible) return std::nullopt;

    // Need established trend with minimum duration
    if (f.trendDirection == 0) return std::nullopt;
    if (f.trendDurationBars < ImbalanceDeltaConfig::CONVERGENCE_MIN_TREND_BARS) return std::nullopt;

    // Need minimum price progress
    const int absMove = std::abs(f.netPriceMoveTicks);
    if (absMove < ImbalanceDeltaConfig::CONVERGENCE_MIN_PRICE_TICKS) return std::nullopt;

    // Delta must align with trend direction
    if (f.deltaSign != f.trendDirection) return std::nullopt;

    // Delta must be meaningful (above threshold)
    if (f.deltaImpulse < ImbalanceDeltaConfig::CONVERGENCE_DELTA_K) return std::nullopt;

    // Must not be in pullback (that's a different pattern)
    if (f.isInPullback) return std::nullopt;

    // === Hit ===
    ImbalanceDeltaHit hit;
    hit.type = ImbalanceDeltaPattern::STRONG_CONVERGENCE;
    hit.windowMs = windowMs;
    hit.priceMoveTicks = f.netPriceMoveTicks;
    hit.trendBars = f.trendDurationBars;

    // Strength: combine delta impulse and trend duration
    const double impulseNorm = (std::min)(f.deltaImpulse / 3.0, 1.0);
    const double durationNorm = (std::min)(static_cast<double>(f.trendDurationBars) / 10.0, 1.0);
    hit.strength01 = static_cast<float>((impulseNorm + durationNorm) / 2.0);

    return hit;
}

// WEAK_PULLBACK: Price retraces but delta doesn't reverse (add-on signal)
inline std::optional<ImbalanceDeltaHit> DetectWeakPullback(
    const ImbalanceDeltaFeatures& f,
    int windowMs)
{
    if (!f.isEligible) return std::nullopt;

    // Need established trend
    if (f.trendDirection == 0) return std::nullopt;

    // Must be in pullback zone
    if (!f.isInPullback) return std::nullopt;

    // Delta must NOT reverse (still aligned with original trend)
    // Allow neutral delta, but not strong reversal
    if (f.trendDirection > 0)
    {
        // Uptrend pullback: delta should not be strongly negative
        if (f.deltaSign < 0 && f.deltaImpulse > ImbalanceDeltaConfig::PULLBACK_DELTA_MIN_K)
            return std::nullopt;
    }
    else
    {
        // Downtrend pullback: delta should not be strongly positive
        if (f.deltaSign > 0 && f.deltaImpulse > ImbalanceDeltaConfig::PULLBACK_DELTA_MIN_K)
            return std::nullopt;
    }

    // === Hit ===
    ImbalanceDeltaHit hit;
    hit.type = ImbalanceDeltaPattern::WEAK_PULLBACK;
    hit.windowMs = windowMs;
    hit.priceMoveTicks = f.retraceTicks * (f.trendDirection > 0 ? -1 : 1);  // Retrace direction
    hit.trendBars = f.trendDurationBars;

    // Strength: based on how shallow the pullback is relative to trend
    const double retractNorm = 1.0 - (static_cast<double>(f.retraceTicks) /
                                       static_cast<double>(ImbalanceDeltaConfig::PULLBACK_MAX_RETRACE_TICKS));
    hit.strength01 = static_cast<float>((std::max)(retractNorm, 0.1));

    return hit;
}

// EFFORT_NO_RESULT: High volume/delta effort but no price progress
inline std::optional<ImbalanceDeltaHit> DetectEffortNoResult(
    const ImbalanceDeltaFeatures& f,
    int windowMs)
{
    if (!f.isEligible) return std::nullopt;

    // Need high delta effort
    if (f.deltaImpulse < ImbalanceDeltaConfig::EFFORT_DELTA_K) return std::nullopt;

    // But no price progress (stalled)
    const int absMove = std::abs(f.netPriceMoveTicks);
    if (absMove > ImbalanceDeltaConfig::EFFORT_MAX_PRICE_TICKS) return std::nullopt;

    // === Hit ===
    ImbalanceDeltaHit hit;
    hit.type = ImbalanceDeltaPattern::EFFORT_NO_RESULT;
    hit.windowMs = windowMs;
    hit.priceMoveTicks = f.netPriceMoveTicks;
    hit.trendBars = 0;  // Not trend-related

    // Strength: based on how extreme the effort is
    hit.strength01 = static_cast<float>((std::min)(f.deltaImpulse / 4.0, 1.0));

    return hit;
}

// CLIMAX_EXHAUSTION: Extreme effort + Group 1 confirmers (capitulation)
inline std::optional<ImbalanceDeltaHit> DetectClimaxExhaustion(
    const ImbalanceDeltaFeatures& f,
    int windowMs)
{
    if (!f.isEligible) return std::nullopt;

    // Need extreme delta effort
    if (f.deltaImpulse < ImbalanceDeltaConfig::CLIMAX_DELTA_K) return std::nullopt;

    // Need at least one Group 1 confirmer
    if (f.confirmationCount < ImbalanceDeltaConfig::CLIMAX_CONFIRM_REQUIRED) return std::nullopt;

    // === Hit ===
    ImbalanceDeltaHit hit;
    hit.type = ImbalanceDeltaPattern::CLIMAX_EXHAUSTION;
    hit.windowMs = windowMs;
    hit.priceMoveTicks = f.netPriceMoveTicks;
    hit.trendBars = f.trendDurationBars;
    hit.hasConfirmation = true;

    // Encode confirmation types as bitmask
    hit.confirmationType = 0;
    if (f.hasFlowReversal) hit.confirmationType |= 1;
    if (f.hasSweepLiquidation) hit.confirmationType |= 2;
    if (f.hasExhaustionDivergence) hit.confirmationType |= 4;

    // Strength: combine delta extremeness and confirmation count
    const double deltaStrength = (std::min)(f.deltaImpulse / 4.0, 1.0);
    const double confirmStrength = static_cast<double>(f.confirmationCount) / 3.0;
    hit.strength01 = static_cast<float>((deltaStrength + confirmStrength) / 2.0);

    return hit;
}

// ============================================================================
// RESULT STRUCT
// ============================================================================

struct ImbalanceDeltaPatternResult
{
    std::vector<ImbalanceDeltaPattern> patterns;
    std::vector<ImbalanceDeltaHit> hits;

    int windowMs = 0;
    bool wasEligible = false;
    bool wasInImbalanceRegime = false;
    const char* ineligibleReason = nullptr;

    bool HasPatterns() const { return !patterns.empty(); }
};

// ============================================================================
// MAIN DETECTION FUNCTION
// ============================================================================

inline ImbalanceDeltaPatternResult DetectImbalanceDeltaPatterns(
    const DomHistoryBuffer& buffer,
    const DomEventFeatures& baseFeatures,
    const TrendProgressTracker& trendTracker,
    const std::vector<TPOMechanics>& tpoMechanics,
    const std::vector<DOMEvent>& domEvents,
    const std::vector<DOMControlPattern>& domControlPatterns,
    int windowMs)
{
    ImbalanceDeltaPatternResult result;
    result.windowMs = windowMs;

    // Compute features (includes regime gating)
    const ImbalanceDeltaFeatures f = ComputeImbalanceDeltaFeatures(
        buffer, baseFeatures, trendTracker, tpoMechanics, domEvents, domControlPatterns);

    result.wasEligible = f.isEligible;
    result.wasInImbalanceRegime = f.inImbalanceRegime;
    result.ineligibleReason = f.ineligibleReason;

    if (!f.isEligible) return result;

    // === Detect all patterns ===

    // STRONG_CONVERGENCE
    if (auto hit = DetectStrongConvergence(f, windowMs))
    {
        result.patterns.push_back(hit->type);
        result.hits.push_back(*hit);
    }

    // WEAK_PULLBACK
    if (auto hit = DetectWeakPullback(f, windowMs))
    {
        result.patterns.push_back(hit->type);
        result.hits.push_back(*hit);
    }

    // EFFORT_NO_RESULT
    if (auto hit = DetectEffortNoResult(f, windowMs))
    {
        result.patterns.push_back(hit->type);
        result.hits.push_back(*hit);
    }

    // CLIMAX_EXHAUSTION
    if (auto hit = DetectClimaxExhaustion(f, windowMs))
    {
        result.patterns.push_back(hit->type);
        result.hits.push_back(*hit);
    }

    // Sort hits by strength
    std::sort(result.hits.begin(), result.hits.end());

    return result;
}

// ============================================================================
// LOGGING SUPPORT
// ============================================================================

struct ImbalanceDeltaLogState
{
    int lastLogBar = -1;
    std::vector<ImbalanceDeltaPattern> lastPatterns;

    bool ShouldLog(const ImbalanceDeltaPatternResult& result, int currentBar)
    {
        if (!result.HasPatterns()) return false;

        // De-duplicate: only log if patterns changed or new bar
        if (currentBar != lastLogBar || result.patterns != lastPatterns)
        {
            lastLogBar = currentBar;
            lastPatterns = result.patterns;
            return true;
        }
        return false;
    }

    void Reset()
    {
        lastLogBar = -1;
        lastPatterns.clear();
    }
};

inline std::string BuildImbalanceDeltaLogMessage(
    const ImbalanceDeltaPatternResult& result,
    int timestampMs)
{
    std::string msg = "[IMB-DELTA] t=";
    msg += std::to_string(timestampMs);
    msg += "ms | ";

    for (size_t i = 0; i < result.patterns.size(); ++i)
    {
        if (i > 0) msg += ", ";
        msg += to_string(result.patterns[i]);

        // Add strength if we have hits
        if (i < result.hits.size())
        {
            msg += "(";
            msg += std::to_string(static_cast<int>(result.hits[i].strength01 * 100));
            msg += "%)";

            // Add confirmation info for CLIMAX_EXHAUSTION
            if (result.patterns[i] == ImbalanceDeltaPattern::CLIMAX_EXHAUSTION)
            {
                msg += "[";
                if (result.hits[i].confirmationType & 1) msg += "FR";
                if (result.hits[i].confirmationType & 2) msg += "+SL";
                if (result.hits[i].confirmationType & 4) msg += "+ED";
                msg += "]";
            }
        }
    }

    return msg;
}

}  // namespace AMT
