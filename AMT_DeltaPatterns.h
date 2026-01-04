// ============================================================================
// AMT_DeltaPatterns.h
// Balance Delta Pattern Detection Module
// Detects: BalanceDeltaPattern (ABSORPTION_AT_HIGH/LOW, DELTA_DIVERGENCE_FADE,
//          AGGRESSIVE_INITIATION)
// ============================================================================

#ifndef AMT_DELTA_PATTERNS_H
#define AMT_DELTA_PATTERNS_H

#include "AMT_DomEvents.h"       // Reuse DomEventFeatures, DomHistoryBuffer
#include "AMT_VolumePatterns.h"  // BalanceSnapshot

namespace AMT {

// ============================================================================
// CONFIGURATION - Balance delta pattern thresholds
// ============================================================================

struct DeltaPatternConfig
{
    // Analysis window (use same as DOM patterns for consistency)
    static constexpr int DEFAULT_WINDOW_MS = DomEventConfig::DEFAULT_WINDOW_MS;
    static constexpr int MIN_SAMPLES = DomEventConfig::MIN_SAMPLES;

    // Proximity to balance edges (in ticks)
    static constexpr int EDGE_PROXIMITY_TICKS = 4;       // Within 4 ticks of VAH/VAL

    // Delta impulse thresholds (MAD-based z-scores)
    static constexpr double DELTA_IMPULSE_K = 2.0;       // K-factor for "strong" delta
    static constexpr double DELTA_WEAK_K = 1.0;          // K-factor for "weak" delta (divergence)

    // Price stall for absorption (max net movement in window)
    static constexpr int ABSORPTION_MAX_MOVE_TICKS = 2;  // Stalled if |move| <= 2 ticks

    // Aggressive initiation (min directional movement)
    static constexpr int INITIATION_MIN_MOVE_TICKS = 3;  // Need at least 3 ticks away from edge

    // Divergence fade ratio (current vs prior push)
    static constexpr double DIVERGENCE_FADE_RATIO = 0.6; // Current < 60% of prior = fade

    // Prior push memory
    static constexpr int PRIOR_PUSH_EXPIRY_BARS = 50;    // Prior push expires after N bars

    // Observability
    static constexpr int LOG_THROTTLE_BARS = 10;         // Min bars between duplicate logs
};

// ============================================================================
// PRIOR PUSH TRACKER - Session-scoped memory for divergence fade detection
// ============================================================================
// Tracks the strongest delta impulse at each edge (VAH/VAL) for comparison

struct PriorPushRecord
{
    bool valid = false;
    int capturedAtBar = -1;
    double deltaImpulse = 0.0;   // Absolute delta z-score magnitude
    int priceTick = 0;           // Price tick when captured

    void Reset()
    {
        valid = false;
        capturedAtBar = -1;
        deltaImpulse = 0.0;
        priceTick = 0;
    }

    bool IsExpired(int currentBar, int expiryBars) const
    {
        if (!valid) return true;
        return (currentBar - capturedAtBar) > expiryBars;
    }
};

struct PriorPushTracker
{
    PriorPushRecord highEdge;  // Prior push at VAH
    PriorPushRecord lowEdge;   // Prior push at VAL

    void Reset()
    {
        highEdge.Reset();
        lowEdge.Reset();
    }

    void ExpireStale(int currentBar, int expiryBars = DeltaPatternConfig::PRIOR_PUSH_EXPIRY_BARS)
    {
        if (highEdge.IsExpired(currentBar, expiryBars))
            highEdge.Reset();
        if (lowEdge.IsExpired(currentBar, expiryBars))
            lowEdge.Reset();
    }

    void RecordHighEdgePush(double deltaImpulse, int priceTick, int bar)
    {
        // Only record if stronger than existing (or existing is invalid/expired)
        if (!highEdge.valid || deltaImpulse > highEdge.deltaImpulse)
        {
            highEdge.valid = true;
            highEdge.deltaImpulse = deltaImpulse;
            highEdge.priceTick = priceTick;
            highEdge.capturedAtBar = bar;
        }
    }

    void RecordLowEdgePush(double deltaImpulse, int priceTick, int bar)
    {
        if (!lowEdge.valid || deltaImpulse > lowEdge.deltaImpulse)
        {
            lowEdge.valid = true;
            lowEdge.deltaImpulse = deltaImpulse;
            lowEdge.priceTick = priceTick;
            lowEdge.capturedAtBar = bar;
        }
    }
};

// ============================================================================
// DELTA PATTERN FEATURES - Extended from base DomEventFeatures
// ============================================================================

struct DeltaPatternFeatures
{
    // Eligibility
    bool isEligible = false;
    const char* ineligibleReason = nullptr;

    // Delta statistics (from DomEventFeatures)
    double deltaSecMedian = 0.0;
    double deltaSecMad = 0.0;
    double deltaSecCurrent = 0.0;
    double deltaSecZScore = 0.0;
    bool deltaStatsValid = false;

    // Absolute delta impulse (magnitude, always positive)
    double deltaImpulse = 0.0;  // |deltaSecZScore|

    // Price movement over window
    int netPriceMoveTicks = 0;      // bestBid end - bestBid start
    int priceDirection = 0;         // +1 = up, -1 = down, 0 = flat

    // Current price position (mid-tick)
    int currentMidTick = 0;

    // Delta sign (current)
    int deltaSign = 0;              // +1 = buy aggression, -1 = sell aggression

    // Balance boundary reference
    int vahTick = 0;
    int valTick = 0;
    bool boundaryValid = false;

    // Distance to edges (in ticks, signed)
    int distToVAH = 0;              // Positive if below VAH
    int distToVAL = 0;              // Positive if above VAL

    // Edge proximity flags
    bool nearHighEdge = false;      // Within EDGE_PROXIMITY_TICKS of VAH
    bool nearLowEdge = false;       // Within EDGE_PROXIMITY_TICKS of VAL
};

// ============================================================================
// FEATURE EXTRACTION
// ============================================================================

inline DeltaPatternFeatures ExtractDeltaFeatures(
    const std::vector<DomObservationSample>& window,
    const DomEventFeatures& baseFeatures,
    const BalanceSnapshot& boundary,
    int windowMs)
{
    DeltaPatternFeatures f;

    // Inherit eligibility from base features
    if (!baseFeatures.isEligible)
    {
        f.isEligible = false;
        f.ineligibleReason = baseFeatures.ineligibleReason;
        return f;
    }

    if (window.size() < DeltaPatternConfig::MIN_SAMPLES)
    {
        f.isEligible = false;
        f.ineligibleReason = "INSUFFICIENT_SAMPLES";
        return f;
    }

    // Check balance boundary validity
    if (!boundary.IsCoherent())
    {
        f.isEligible = false;
        f.ineligibleReason = "BOUNDARY_INVALID";
        return f;
    }

    f.isEligible = true;
    f.boundaryValid = true;
    f.vahTick = boundary.vahTick;
    f.valTick = boundary.valTick;

    // Copy delta statistics from base features
    f.deltaSecMedian = baseFeatures.deltaSecMedian;
    f.deltaSecMad = baseFeatures.deltaSecMad;
    f.deltaSecCurrent = baseFeatures.deltaSecCurrent;
    f.deltaSecZScore = baseFeatures.deltaSecZScore;
    f.deltaStatsValid = baseFeatures.deltaSecStatsValid;

    // Compute delta impulse (absolute magnitude of z-score)
    f.deltaImpulse = std::abs(f.deltaSecZScore);

    // Delta sign
    f.deltaSign = (f.deltaSecCurrent > 0.01) ? 1 : ((f.deltaSecCurrent < -0.01) ? -1 : 0);

    // Price movement
    const auto& oldest = window.front();
    const auto& current = window.back();

    f.netPriceMoveTicks = current.bestBidTick - oldest.bestBidTick;
    f.priceDirection = (f.netPriceMoveTicks > 0) ? 1 : ((f.netPriceMoveTicks < 0) ? -1 : 0);

    // Current mid-tick (average of bid/ask)
    f.currentMidTick = (current.bestBidTick + current.bestAskTick) / 2;

    // Distance to edges
    f.distToVAH = f.vahTick - f.currentMidTick;  // Positive if below VAH
    f.distToVAL = f.currentMidTick - f.valTick;  // Positive if above VAL

    // Edge proximity
    f.nearHighEdge = (std::abs(f.distToVAH) <= DeltaPatternConfig::EDGE_PROXIMITY_TICKS);
    f.nearLowEdge = (std::abs(f.distToVAL) <= DeltaPatternConfig::EDGE_PROXIMITY_TICKS);

    return f;
}

// ============================================================================
// PATTERN DETECTORS
// ============================================================================

// --- ABSORPTION_AT_HIGH ---
// Price near VAH, strong buy delta impulse, but price stalls (no upward progress)
inline std::optional<BalanceDeltaHit> DetectAbsorptionAtHigh(
    const DeltaPatternFeatures& f)
{
    if (!f.isEligible || !f.deltaStatsValid) return std::nullopt;

    // Must be near high edge
    if (!f.nearHighEdge) return std::nullopt;

    // Must have strong positive delta (buy aggression)
    const bool strongBuyDelta = (f.deltaSign > 0) &&
        (f.deltaImpulse >= DeltaPatternConfig::DELTA_IMPULSE_K);

    // Price must be stalled (not breaking through)
    const bool priceStalled = (std::abs(f.netPriceMoveTicks) <=
        DeltaPatternConfig::ABSORPTION_MAX_MOVE_TICKS);

    if (strongBuyDelta && priceStalled)
    {
        BalanceDeltaHit hit;
        hit.type = BalanceDeltaPattern::ABSORPTION_AT_HIGH;
        // Strength based on delta impulse magnitude
        hit.strength01 = static_cast<float>(
            (std::min)(1.0, f.deltaImpulse / 4.0));
        hit.anchorTick = f.vahTick;
        hit.priceMoveTicks = f.netPriceMoveTicks;
        return hit;
    }
    return std::nullopt;
}

// --- ABSORPTION_AT_LOW ---
// Price near VAL, strong sell delta impulse, but price stalls (no downward progress)
inline std::optional<BalanceDeltaHit> DetectAbsorptionAtLow(
    const DeltaPatternFeatures& f)
{
    if (!f.isEligible || !f.deltaStatsValid) return std::nullopt;

    // Must be near low edge
    if (!f.nearLowEdge) return std::nullopt;

    // Must have strong negative delta (sell aggression)
    const bool strongSellDelta = (f.deltaSign < 0) &&
        (f.deltaImpulse >= DeltaPatternConfig::DELTA_IMPULSE_K);

    // Price must be stalled
    const bool priceStalled = (std::abs(f.netPriceMoveTicks) <=
        DeltaPatternConfig::ABSORPTION_MAX_MOVE_TICKS);

    if (strongSellDelta && priceStalled)
    {
        BalanceDeltaHit hit;
        hit.type = BalanceDeltaPattern::ABSORPTION_AT_LOW;
        hit.strength01 = static_cast<float>(
            (std::min)(1.0, f.deltaImpulse / 4.0));
        hit.anchorTick = f.valTick;
        hit.priceMoveTicks = f.netPriceMoveTicks;
        return hit;
    }
    return std::nullopt;
}

// --- DELTA_DIVERGENCE_FADE ---
// Price reaches edge again, but delta impulse is materially weaker than prior push
// STRICTLY REQUIRES a valid prior push record - no fallback
inline std::optional<BalanceDeltaHit> DetectDeltaDivergenceFade(
    const DeltaPatternFeatures& f,
    const PriorPushTracker& priorPushes,
    int currentBar)
{
    if (!f.isEligible || !f.deltaStatsValid) return std::nullopt;

    // Check high edge divergence (buy attempts fading)
    if (f.nearHighEdge && f.deltaSign > 0)
    {
        // STRICTLY require valid prior push at high edge
        if (!priorPushes.highEdge.valid) return std::nullopt;
        if (priorPushes.highEdge.IsExpired(currentBar,
            DeltaPatternConfig::PRIOR_PUSH_EXPIRY_BARS)) return std::nullopt;

        // Current impulse must be materially weaker
        const double fadeRatio = f.deltaImpulse /
            (std::max)(priorPushes.highEdge.deltaImpulse, 0.01);

        if (fadeRatio < DeltaPatternConfig::DIVERGENCE_FADE_RATIO)
        {
            BalanceDeltaHit hit;
            hit.type = BalanceDeltaPattern::DELTA_DIVERGENCE_FADE;
            // Strength based on how much weaker (lower ratio = stronger signal)
            hit.strength01 = static_cast<float>(
                (std::min)(1.0, (1.0 - fadeRatio)));
            hit.anchorTick = f.vahTick;
            hit.priceMoveTicks = f.netPriceMoveTicks;
            return hit;
        }
    }

    // Check low edge divergence (sell attempts fading)
    if (f.nearLowEdge && f.deltaSign < 0)
    {
        // STRICTLY require valid prior push at low edge
        if (!priorPushes.lowEdge.valid) return std::nullopt;
        if (priorPushes.lowEdge.IsExpired(currentBar,
            DeltaPatternConfig::PRIOR_PUSH_EXPIRY_BARS)) return std::nullopt;

        const double fadeRatio = f.deltaImpulse /
            (std::max)(priorPushes.lowEdge.deltaImpulse, 0.01);

        if (fadeRatio < DeltaPatternConfig::DIVERGENCE_FADE_RATIO)
        {
            BalanceDeltaHit hit;
            hit.type = BalanceDeltaPattern::DELTA_DIVERGENCE_FADE;
            hit.strength01 = static_cast<float>(
                (std::min)(1.0, (1.0 - fadeRatio)));
            hit.anchorTick = f.valTick;
            hit.priceMoveTicks = f.netPriceMoveTicks;
            return hit;
        }
    }

    return std::nullopt;
}

// --- AGGRESSIVE_INITIATION ---
// Strong delta impulse aligned with directional movement AWAY from edge
inline std::optional<BalanceDeltaHit> DetectAggressiveInitiation(
    const DeltaPatternFeatures& f)
{
    if (!f.isEligible || !f.deltaStatsValid) return std::nullopt;

    // Need minimum directional movement
    const int absMove = std::abs(f.netPriceMoveTicks);
    if (absMove < DeltaPatternConfig::INITIATION_MIN_MOVE_TICKS) return std::nullopt;

    // Check bullish initiation: near VAL, positive delta, moving UP (away from VAL)
    if (f.nearLowEdge && f.deltaSign > 0 && f.priceDirection > 0)
    {
        // Need strong delta impulse
        if (f.deltaImpulse >= DeltaPatternConfig::DELTA_IMPULSE_K)
        {
            BalanceDeltaHit hit;
            hit.type = BalanceDeltaPattern::AGGRESSIVE_INITIATION;
            // Strength based on both delta and movement
            const double moveScore = (std::min)(1.0, static_cast<double>(absMove) / 6.0);
            const double deltaScore = (std::min)(1.0, f.deltaImpulse / 4.0);
            hit.strength01 = static_cast<float>((moveScore + deltaScore) / 2.0);
            hit.anchorTick = f.valTick;
            hit.priceMoveTicks = f.netPriceMoveTicks;
            return hit;
        }
    }

    // Check bearish initiation: near VAH, negative delta, moving DOWN (away from VAH)
    if (f.nearHighEdge && f.deltaSign < 0 && f.priceDirection < 0)
    {
        if (f.deltaImpulse >= DeltaPatternConfig::DELTA_IMPULSE_K)
        {
            BalanceDeltaHit hit;
            hit.type = BalanceDeltaPattern::AGGRESSIVE_INITIATION;
            const double moveScore = (std::min)(1.0, static_cast<double>(absMove) / 6.0);
            const double deltaScore = (std::min)(1.0, f.deltaImpulse / 4.0);
            hit.strength01 = static_cast<float>((moveScore + deltaScore) / 2.0);
            hit.anchorTick = f.vahTick;
            hit.priceMoveTicks = f.netPriceMoveTicks;
            return hit;
        }
    }

    return std::nullopt;
}

// ============================================================================
// DETECTION RESULT
// ============================================================================

struct DeltaPatternResult
{
    std::vector<BalanceDeltaPattern> patterns;
    std::vector<BalanceDeltaHit> hits;

    int windowMs = 0;
    bool wasEligible = false;
    const char* ineligibleReason = nullptr;

    bool HasPatterns() const { return !patterns.empty(); }
};

// ============================================================================
// MAIN DETECTION FUNCTION
// ============================================================================

inline DeltaPatternResult DetectBalanceDeltaPatterns(
    const DomHistoryBuffer& buffer,
    const DomEventFeatures& baseFeatures,
    const BalanceSnapshot& boundary,
    PriorPushTracker& priorPushes,
    int currentBar,
    int windowMs = DeltaPatternConfig::DEFAULT_WINDOW_MS)
{
    DeltaPatternResult result;
    result.windowMs = windowMs;

    // Get window samples
    auto window = buffer.GetWindow(windowMs);

    // Expire stale prior pushes
    priorPushes.ExpireStale(currentBar);

    // Extract delta-specific features
    DeltaPatternFeatures f = ExtractDeltaFeatures(window, baseFeatures, boundary, windowMs);

    result.wasEligible = f.isEligible;
    result.ineligibleReason = f.ineligibleReason;

    if (!f.isEligible) return result;

    // Run detectors
    if (auto hit = DetectAbsorptionAtHigh(f))
    {
        hit->windowMs = windowMs;
        result.patterns.push_back(hit->type);
        result.hits.push_back(*hit);

        // Record this as a potential prior push for divergence detection
        priorPushes.RecordHighEdgePush(f.deltaImpulse, f.currentMidTick, currentBar);
    }

    if (auto hit = DetectAbsorptionAtLow(f))
    {
        hit->windowMs = windowMs;
        result.patterns.push_back(hit->type);
        result.hits.push_back(*hit);

        // Record as prior push
        priorPushes.RecordLowEdgePush(f.deltaImpulse, f.currentMidTick, currentBar);
    }

    if (auto hit = DetectDeltaDivergenceFade(f, priorPushes, currentBar))
    {
        hit->windowMs = windowMs;
        result.patterns.push_back(hit->type);
        result.hits.push_back(*hit);
    }

    if (auto hit = DetectAggressiveInitiation(f))
    {
        hit->windowMs = windowMs;
        result.patterns.push_back(hit->type);
        result.hits.push_back(*hit);
    }

    // Sort hits for deterministic ordering
    std::sort(result.hits.begin(), result.hits.end());

    return result;
}

// ============================================================================
// OBSERVABILITY - Log state tracker for de-duplication
// ============================================================================

struct DeltaPatternLogState
{
    int lastLogBar = -1;
    std::vector<BalanceDeltaPattern> lastPatterns;
    bool firstEmissionDone = false;

    void Reset()
    {
        lastLogBar = -1;
        lastPatterns.clear();
        firstEmissionDone = false;
    }

    bool ShouldLog(const DeltaPatternResult& result, int currentBar)
    {
        // Throttle: don't log too frequently
        if (currentBar - lastLogBar < DeltaPatternConfig::LOG_THROTTLE_BARS &&
            firstEmissionDone)
        {
            return false;
        }

        // Check if patterns changed
        bool changed = false;

        if (result.patterns.size() != lastPatterns.size())
        {
            changed = true;
        }
        else
        {
            for (size_t i = 0; i < result.patterns.size(); ++i)
            {
                if (result.patterns[i] != lastPatterns[i])
                {
                    changed = true;
                    break;
                }
            }
        }

        if (!firstEmissionDone || changed)
        {
            lastLogBar = currentBar;
            lastPatterns = result.patterns;
            firstEmissionDone = true;
            return true;
        }

        return false;
    }
};

// ============================================================================
// LOG MESSAGE BUILDER
// ============================================================================

inline std::string BuildDeltaPatternLogMessage(
    const DeltaPatternResult& result,
    int64_t timestampMs)
{
    std::string msg = "[DELTA-PAT] ts=";
    msg += std::to_string(timestampMs);
    msg += " ";

    if (!result.patterns.empty())
    {
        msg += "PAT=[";
        for (size_t i = 0; i < result.patterns.size(); ++i)
        {
            if (i > 0) msg += ",";
            msg += to_string(result.patterns[i]);
        }
        msg += "] ";

        // Add top hit details
        if (!result.hits.empty())
        {
            const auto& top = result.hits.front();
            msg += "str=";
            int strengthPct = static_cast<int>(top.strength01 * 100);
            msg += "0.";
            if (strengthPct < 10) msg += "0";
            msg += std::to_string(strengthPct);
            msg += " anchor=";
            msg += std::to_string(top.anchorTick);
            msg += " move=";
            msg += std::to_string(top.priceMoveTicks);
            msg += "t ";
        }
    }
    else
    {
        msg += "NONE";
    }

    return msg;
}

} // namespace AMT

#endif // AMT_DELTA_PATTERNS_H
