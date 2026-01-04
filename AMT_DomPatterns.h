// ============================================================================
// AMT_DomPatterns.h
// Static DOM Pattern Detection Module - Reuses features from AMT_DomEvents.h
// Detects: BalanceDOMPattern and ImbalanceDOMPattern
// ============================================================================

#ifndef AMT_DOM_PATTERNS_H
#define AMT_DOM_PATTERNS_H

#include "AMT_DomEvents.h"  // Reuse DomEventFeatures, DomHistoryBuffer, etc.

namespace AMT {

// ============================================================================
// CONFIGURATION - Static DOM pattern thresholds
// ============================================================================

struct DomPatternConfig
{
    // Balance patterns (depth-based)
    static constexpr double STACKED_DEPTH_RATIO = 2.5;       // bid/ask ratio for STACKED_BIDS
    static constexpr double STACKED_INV_RATIO = 0.4;         // 1/ratio for STACKED_ASKS (1/2.5)
    static constexpr double EXTREME_RATIO = 3.0;             // Threshold for BID_ASK_RATIO_EXTREME

    // Order reloading (iceberg detection)
    static constexpr int RELOADING_MIN_SAMPLES = 4;          // Min samples showing consistent restacking
    static constexpr double RELOADING_STACK_THRESHOLD = 0.5; // Min stack dominance after pull

    // Spoof detection (rapid side-switching)
    static constexpr int SPOOF_FLIP_MIN_COUNT = 2;           // Min flips in window for spoof
    static constexpr double SPOOF_FLIP_MAGNITUDE = 0.3;      // Min change magnitude for flip

    // Chasing orders (best price movement)
    static constexpr int CHASING_MIN_TICKS = 2;              // Min bid/ask price change
    static constexpr int CHASING_MIN_SAMPLES = 3;            // Sustained samples in direction

    // Observability
    static constexpr int LOG_THROTTLE_BARS = 10;             // Min bars between duplicate logs
};

// ============================================================================
// EXTENDED FEATURES - Additional metrics for static pattern detection
// ============================================================================
// Extracted from DomHistoryBuffer window, extends DomEventFeatures

struct DomPatternFeatures
{
    // Window eligibility (inherited check)
    bool isEligible = false;
    const char* ineligibleReason = nullptr;

    // Depth ratio (from DomEventFeatures - repeated for clarity)
    double bidAskDepthRatio = 1.0;

    // Stack/Pull time-series analysis
    int stackDominantSampleCount = 0;   // Samples where stack > pull
    int pullDominantSampleCount = 0;    // Samples where pull > stack
    bool consistentRestack = false;     // Pull followed by stack pattern

    // Depth imbalance flip detection (for spoof)
    int depthImbalanceFlipCount = 0;    // Count of bid/ask dominance flips
    bool rapidImbalanceFlip = false;    // Multiple flips in short window

    // Bid/Ask price persistence (for chasing)
    int bidAdvanceSamples = 0;          // Consecutive samples with bid advancing
    int bidRetreatSamples = 0;          // Consecutive samples with bid retreating
    int askAdvanceSamples = 0;          // Consecutive samples with ask advancing (lower)
    int askRetreatSamples = 0;          // Consecutive samples with ask retreating (higher)

    // Price movement magnitude
    int totalBidMoveTicks = 0;          // Net bid price movement
    int totalAskMoveTicks = 0;          // Net ask price movement
};

// ============================================================================
// EXTENDED FEATURE EXTRACTION
// ============================================================================

inline DomPatternFeatures ExtractPatternFeatures(
    const std::vector<DomObservationSample>& window,
    const DomEventFeatures& baseFeatures)
{
    DomPatternFeatures pf;

    // Inherit eligibility from base features
    pf.isEligible = baseFeatures.isEligible;
    pf.ineligibleReason = baseFeatures.ineligibleReason;
    pf.bidAskDepthRatio = baseFeatures.bidAskDepthRatio;

    if (!pf.isEligible || window.size() < 2)
        return pf;

    // Analyze time-series patterns
    int prevBidTick = window.front().bestBidTick;
    int prevAskTick = window.front().bestAskTick;
    double prevDepthRatio = window.front().domBidSize /
        (std::max)(window.front().domAskSize, 1.0);
    bool prevBidDominant = (prevDepthRatio > 1.0);

    int bidAdvanceRun = 0, bidRetreatRun = 0;
    int askAdvanceRun = 0, askRetreatRun = 0;
    bool hadPull = false;

    for (size_t i = 1; i < window.size(); ++i)
    {
        const auto& s = window[i];

        // Stack/Pull dominance
        const double stack = (std::max)(s.bidStackPull, 0.0) + (std::max)(s.askStackPull, 0.0);
        const double pull = -(std::min)(s.bidStackPull, 0.0) - (std::min)(s.askStackPull, 0.0);

        if (stack > pull * 1.2)
        {
            pf.stackDominantSampleCount++;
            if (hadPull)
            {
                pf.consistentRestack = true;
            }
        }
        else if (pull > stack * 1.2)
        {
            pf.pullDominantSampleCount++;
            hadPull = true;
        }

        // Depth imbalance flip detection
        const double curDepthRatio = s.domBidSize / (std::max)(s.domAskSize, 1.0);
        const bool curBidDominant = (curDepthRatio > 1.0);
        if (curBidDominant != prevBidDominant)
        {
            const double flipMag = std::abs(curDepthRatio - prevDepthRatio);
            if (flipMag > DomPatternConfig::SPOOF_FLIP_MAGNITUDE)
            {
                pf.depthImbalanceFlipCount++;
            }
        }
        prevBidDominant = curBidDominant;
        prevDepthRatio = curDepthRatio;

        // Bid price movement tracking
        const int bidDelta = s.bestBidTick - prevBidTick;
        if (bidDelta > 0)
        {
            bidAdvanceRun++;
            bidRetreatRun = 0;
        }
        else if (bidDelta < 0)
        {
            bidRetreatRun++;
            bidAdvanceRun = 0;
        }
        pf.bidAdvanceSamples = (std::max)(pf.bidAdvanceSamples, bidAdvanceRun);
        pf.bidRetreatSamples = (std::max)(pf.bidRetreatSamples, bidRetreatRun);

        // Ask price movement tracking
        const int askDelta = s.bestAskTick - prevAskTick;
        if (askDelta < 0)  // Ask moving down = buyers chasing
        {
            askAdvanceRun++;
            askRetreatRun = 0;
        }
        else if (askDelta > 0)  // Ask moving up = sellers retreating
        {
            askRetreatRun++;
            askAdvanceRun = 0;
        }
        pf.askAdvanceSamples = (std::max)(pf.askAdvanceSamples, askAdvanceRun);
        pf.askRetreatSamples = (std::max)(pf.askRetreatSamples, askRetreatRun);

        prevBidTick = s.bestBidTick;
        prevAskTick = s.bestAskTick;
    }

    // Calculate total price movement
    pf.totalBidMoveTicks = window.back().bestBidTick - window.front().bestBidTick;
    pf.totalAskMoveTicks = window.back().bestAskTick - window.front().bestAskTick;

    // Rapid flip detection
    pf.rapidImbalanceFlip = (pf.depthImbalanceFlipCount >= DomPatternConfig::SPOOF_FLIP_MIN_COUNT);

    return pf;
}

// ============================================================================
// BALANCE DOM PATTERN DETECTORS
// ============================================================================

inline std::optional<BalanceDOMHit> DetectStackedBids(
    const DomEventFeatures& f,
    const DomPatternFeatures& pf)
{
    if (!pf.isEligible) return std::nullopt;

    // STACKED_BIDS: Significant bid-side depth dominance
    if (pf.bidAskDepthRatio >= DomPatternConfig::STACKED_DEPTH_RATIO)
    {
        BalanceDOMHit hit;
        hit.type = BalanceDOMPattern::STACKED_BIDS;
        // Strength scales with ratio (2.5 = 0.5, 5.0 = 1.0)
        hit.strength01 = static_cast<float>(
            (std::min)(1.0, (pf.bidAskDepthRatio - 1.5) / 3.5));
        hit.windowMs = f.windowMs;
        return hit;
    }
    return std::nullopt;
}

inline std::optional<BalanceDOMHit> DetectStackedAsks(
    const DomEventFeatures& f,
    const DomPatternFeatures& pf)
{
    if (!pf.isEligible) return std::nullopt;

    // STACKED_ASKS: Significant ask-side depth dominance (inverse ratio)
    if (pf.bidAskDepthRatio <= DomPatternConfig::STACKED_INV_RATIO)
    {
        BalanceDOMHit hit;
        hit.type = BalanceDOMPattern::STACKED_ASKS;
        // Strength scales inversely (0.4 = 0.5, 0.2 = 1.0)
        const double invRatio = 1.0 / (std::max)(pf.bidAskDepthRatio, 0.1);
        hit.strength01 = static_cast<float>(
            (std::min)(1.0, (invRatio - 1.5) / 3.5));
        hit.windowMs = f.windowMs;
        return hit;
    }
    return std::nullopt;
}

inline std::optional<BalanceDOMHit> DetectOrderReloading(
    const DomEventFeatures& f,
    const DomPatternFeatures& pf)
{
    if (!pf.isEligible) return std::nullopt;

    // ORDER_RELOADING: Iceberg-like behavior - pull followed by consistent restacking
    // Requires: evidence of pulls AND consistent restacking pattern
    const bool hasPulls = (pf.pullDominantSampleCount >= 2);
    const bool hasRestacks = (pf.stackDominantSampleCount >= DomPatternConfig::RELOADING_MIN_SAMPLES);
    const bool patternMatch = pf.consistentRestack;

    if (hasPulls && hasRestacks && patternMatch)
    {
        BalanceDOMHit hit;
        hit.type = BalanceDOMPattern::ORDER_RELOADING;
        // Strength based on restack consistency
        const int totalDominant = pf.stackDominantSampleCount + pf.pullDominantSampleCount;
        hit.strength01 = static_cast<float>(
            (std::min)(1.0, static_cast<double>(pf.stackDominantSampleCount) / totalDominant));
        hit.windowMs = f.windowMs;
        return hit;
    }
    return std::nullopt;
}

inline std::optional<BalanceDOMHit> DetectSpoofOrderFlip(
    const DomEventFeatures& /* f */,
    const DomPatternFeatures& pf)
{
    if (!pf.isEligible) return std::nullopt;

    // SPOOF_ORDER_FLIP: Rapid side-switching of depth dominance
    // Indicates potential manipulation (showing then pulling)
    if (pf.rapidImbalanceFlip)
    {
        BalanceDOMHit hit;
        hit.type = BalanceDOMPattern::SPOOF_ORDER_FLIP;
        // Strength based on flip count
        hit.strength01 = static_cast<float>(
            (std::min)(1.0, static_cast<double>(pf.depthImbalanceFlipCount) / 4.0));
        hit.windowMs = 0;  // Will be set by caller
        return hit;
    }
    return std::nullopt;
}

// ============================================================================
// IMBALANCE DOM PATTERN DETECTORS
// ============================================================================

inline std::optional<ImbalanceDOMHit> DetectChasingOrdersBuy(
    const DomEventFeatures& f,
    const DomPatternFeatures& pf)
{
    if (!pf.isEligible) return std::nullopt;

    // CHASING_ORDERS_BUY: Best bid persistently advancing (buyers stepping up)
    const bool bidAdvancing = (pf.totalBidMoveTicks >= DomPatternConfig::CHASING_MIN_TICKS);
    const bool persistent = (pf.bidAdvanceSamples >= DomPatternConfig::CHASING_MIN_SAMPLES);

    if (bidAdvancing && persistent)
    {
        ImbalanceDOMHit hit;
        hit.type = ImbalanceDOMPattern::CHASING_ORDERS_BUY;
        // Strength based on tick movement
        hit.strength01 = static_cast<float>(
            (std::min)(1.0, static_cast<double>(pf.totalBidMoveTicks) / 6.0));
        hit.windowMs = f.windowMs;
        return hit;
    }
    return std::nullopt;
}

inline std::optional<ImbalanceDOMHit> DetectChasingOrdersSell(
    const DomEventFeatures& f,
    const DomPatternFeatures& pf)
{
    if (!pf.isEligible) return std::nullopt;

    // CHASING_ORDERS_SELL: Best ask persistently declining (sellers stepping down)
    // Note: negative askMoveTicks means ask moving down
    const bool askDeclining = (pf.totalAskMoveTicks <= -DomPatternConfig::CHASING_MIN_TICKS);
    const bool persistent = (pf.askAdvanceSamples >= DomPatternConfig::CHASING_MIN_SAMPLES);

    if (askDeclining && persistent)
    {
        ImbalanceDOMHit hit;
        hit.type = ImbalanceDOMPattern::CHASING_ORDERS_SELL;
        // Strength based on tick movement magnitude
        hit.strength01 = static_cast<float>(
            (std::min)(1.0, static_cast<double>(-pf.totalAskMoveTicks) / 6.0));
        hit.windowMs = f.windowMs;
        return hit;
    }
    return std::nullopt;
}

inline std::optional<ImbalanceDOMHit> DetectBidAskRatioExtreme(
    const DomEventFeatures& f,
    const DomPatternFeatures& pf)
{
    if (!pf.isEligible) return std::nullopt;

    // BID_ASK_RATIO_EXTREME: >3:1 or <1:3 ratio
    // More extreme than STACKED - indicates strong directional conviction
    const bool bidExtreme = (pf.bidAskDepthRatio >= DomPatternConfig::EXTREME_RATIO);
    const bool askExtreme = (pf.bidAskDepthRatio <= 1.0 / DomPatternConfig::EXTREME_RATIO);

    if (bidExtreme || askExtreme)
    {
        ImbalanceDOMHit hit;
        hit.type = ImbalanceDOMPattern::BID_ASK_RATIO_EXTREME;
        // Strength based on ratio magnitude
        const double ratio = bidExtreme ? pf.bidAskDepthRatio : (1.0 / pf.bidAskDepthRatio);
        hit.strength01 = static_cast<float>(
            (std::min)(1.0, (ratio - 2.0) / 4.0));
        hit.windowMs = f.windowMs;
        // Store which side is extreme via anchorTick (positive = bid extreme, negative = ask)
        hit.anchorTick = bidExtreme ? 1 : -1;
        return hit;
    }
    return std::nullopt;
}

inline std::optional<ImbalanceDOMHit> DetectAbsorptionFailure(
    const DomDetectionResult& group1Result)
{
    // ABSORPTION_FAILURE: Strictly composite pattern
    // Requires BOTH EXHAUSTION_DIVERGENCE AND SWEEP_LIQUIDATION detected in Group 1
    // This indicates: large effort (exhaustion) + rapid move (sweep) = absorption broke down

    bool hasExhaustion = false;
    bool hasSweep = false;
    float exhaustionStrength = 0.0f;
    float sweepStrength = 0.0f;

    for (const auto& hit : group1Result.controlHits)
    {
        if (hit.type == DOMControlPattern::EXHAUSTION_DIVERGENCE)
        {
            hasExhaustion = true;
            exhaustionStrength = hit.strength01;
        }
    }

    for (const auto& hit : group1Result.eventHits)
    {
        if (hit.type == DOMEvent::SWEEP_LIQUIDATION)
        {
            hasSweep = true;
            sweepStrength = hit.strength01;
        }
    }

    if (hasExhaustion && hasSweep)
    {
        ImbalanceDOMHit hit;
        hit.type = ImbalanceDOMPattern::ABSORPTION_FAILURE;
        // Composite strength = geometric mean of components
        hit.strength01 = std::sqrt(exhaustionStrength * sweepStrength);
        hit.windowMs = group1Result.windowMs;
        hit.isComposite = true;
        return hit;
    }
    return std::nullopt;
}

// ============================================================================
// DETECTION RESULT - Aggregated output from static pattern detectors
// ============================================================================

struct DomPatternResult
{
    std::vector<BalanceDOMPattern> balancePatterns;
    std::vector<ImbalanceDOMPattern> imbalancePatterns;
    std::vector<BalanceDOMHit> balanceHits;
    std::vector<ImbalanceDOMHit> imbalanceHits;

    int windowMs = 0;
    bool wasEligible = false;
    const char* ineligibleReason = nullptr;

    bool HasPatterns() const { return !balancePatterns.empty() || !imbalancePatterns.empty(); }
};

// ============================================================================
// MAIN DETECTION FUNCTION - Called after Group 1 detection
// ============================================================================
// Reuses DomEventFeatures from Group 1, extracts additional pattern features

inline DomPatternResult DetectDomPatterns(
    const DomHistoryBuffer& buffer,
    const DomEventFeatures& baseFeatures,
    const DomDetectionResult& group1Result,
    int windowMs = DomEventConfig::DEFAULT_WINDOW_MS)
{
    DomPatternResult result;
    result.windowMs = windowMs;

    // Check base eligibility
    if (!baseFeatures.isEligible)
    {
        result.wasEligible = false;
        result.ineligibleReason = baseFeatures.ineligibleReason;
        return result;
    }
    result.wasEligible = true;

    // Get window samples for extended analysis
    auto window = buffer.GetWindow(windowMs);

    // Extract pattern-specific features (reuses base features)
    DomPatternFeatures pf = ExtractPatternFeatures(window, baseFeatures);

    // Run Balance DOM Pattern detectors
    if (auto hit = DetectStackedBids(baseFeatures, pf))
    {
        result.balancePatterns.push_back(hit->type);
        result.balanceHits.push_back(*hit);
    }
    if (auto hit = DetectStackedAsks(baseFeatures, pf))
    {
        result.balancePatterns.push_back(hit->type);
        result.balanceHits.push_back(*hit);
    }
    if (auto hit = DetectOrderReloading(baseFeatures, pf))
    {
        result.balancePatterns.push_back(hit->type);
        result.balanceHits.push_back(*hit);
    }
    if (auto hit = DetectSpoofOrderFlip(baseFeatures, pf))
    {
        hit->windowMs = windowMs;
        result.balancePatterns.push_back(hit->type);
        result.balanceHits.push_back(*hit);
    }

    // Run Imbalance DOM Pattern detectors
    if (auto hit = DetectChasingOrdersBuy(baseFeatures, pf))
    {
        result.imbalancePatterns.push_back(hit->type);
        result.imbalanceHits.push_back(*hit);
    }
    if (auto hit = DetectChasingOrdersSell(baseFeatures, pf))
    {
        result.imbalancePatterns.push_back(hit->type);
        result.imbalanceHits.push_back(*hit);
    }
    if (auto hit = DetectBidAskRatioExtreme(baseFeatures, pf))
    {
        result.imbalancePatterns.push_back(hit->type);
        result.imbalanceHits.push_back(*hit);
    }

    // ABSORPTION_FAILURE: Composite detector (requires Group 1 results)
    if (auto hit = DetectAbsorptionFailure(group1Result))
    {
        result.imbalancePatterns.push_back(hit->type);
        result.imbalanceHits.push_back(*hit);
    }

    // Sort hits for deterministic ordering
    std::sort(result.balanceHits.begin(), result.balanceHits.end());
    std::sort(result.imbalanceHits.begin(), result.imbalanceHits.end());

    return result;
}

// ============================================================================
// OBSERVABILITY - Log state tracker for de-duplication
// ============================================================================

struct DomPatternLogState
{
    int lastLogBar = -1;
    std::vector<BalanceDOMPattern> lastBalancePatterns;
    std::vector<ImbalanceDOMPattern> lastImbalancePatterns;
    bool firstEmissionDone = false;

    void Reset()
    {
        lastLogBar = -1;
        lastBalancePatterns.clear();
        lastImbalancePatterns.clear();
        firstEmissionDone = false;
    }

    bool ShouldLog(const DomPatternResult& result, int currentBar)
    {
        // Throttle: don't log too frequently
        if (currentBar - lastLogBar < DomPatternConfig::LOG_THROTTLE_BARS &&
            firstEmissionDone)
        {
            return false;
        }

        // Check if patterns changed
        bool changed = false;

        if (result.balancePatterns.size() != lastBalancePatterns.size() ||
            result.imbalancePatterns.size() != lastImbalancePatterns.size())
        {
            changed = true;
        }
        else
        {
            for (size_t i = 0; i < result.balancePatterns.size(); ++i)
            {
                if (result.balancePatterns[i] != lastBalancePatterns[i])
                {
                    changed = true;
                    break;
                }
            }
            if (!changed)
            {
                for (size_t i = 0; i < result.imbalancePatterns.size(); ++i)
                {
                    if (result.imbalancePatterns[i] != lastImbalancePatterns[i])
                    {
                        changed = true;
                        break;
                    }
                }
            }
        }

        if (!firstEmissionDone || changed)
        {
            lastLogBar = currentBar;
            lastBalancePatterns = result.balancePatterns;
            lastImbalancePatterns = result.imbalancePatterns;
            firstEmissionDone = true;
            return true;
        }

        return false;
    }
};

// ============================================================================
// LOG MESSAGE BUILDER - For observability
// ============================================================================

inline std::string BuildDomPatternLogMessage(
    const DomPatternResult& result,
    int64_t timestampMs)
{
    std::string msg = "[DOM-PAT] ts=";
    msg += std::to_string(timestampMs);
    msg += " ";

    // Balance patterns
    if (!result.balancePatterns.empty())
    {
        msg += "BAL=[";
        for (size_t i = 0; i < result.balancePatterns.size(); ++i)
        {
            if (i > 0) msg += ",";
            msg += to_string(result.balancePatterns[i]);
        }
        msg += "] ";
    }

    // Imbalance patterns
    if (!result.imbalancePatterns.empty())
    {
        msg += "IMB=[";
        for (size_t i = 0; i < result.imbalancePatterns.size(); ++i)
        {
            if (i > 0) msg += ",";
            msg += to_string(result.imbalancePatterns[i]);
        }
        msg += "] ";
    }

    // Hit strengths (top hits only)
    if (!result.balanceHits.empty())
    {
        const auto& top = result.balanceHits.front();
        msg += "balStr=";
        int strengthPct = static_cast<int>(top.strength01 * 100);
        msg += "0.";
        if (strengthPct < 10) msg += "0";
        msg += std::to_string(strengthPct);
        msg += " ";
    }
    if (!result.imbalanceHits.empty())
    {
        const auto& top = result.imbalanceHits.front();
        msg += "imbStr=";
        int strengthPct = static_cast<int>(top.strength01 * 100);
        msg += "0.";
        if (strengthPct < 10) msg += "0";
        msg += std::to_string(strengthPct);
        if (top.isComposite) msg += "(C)";
        msg += " ";
    }

    if (result.balancePatterns.empty() && result.imbalancePatterns.empty())
    {
        msg += "NONE";
    }

    return msg;
}

} // namespace AMT

#endif // AMT_DOM_PATTERNS_H
