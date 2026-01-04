// ============================================================================
// AMT_DomEvents.h
// DOM Event Detection Module - Pure detection with no Sierra dependencies
// Detects: DOMControlPattern and DOMEvent from DOM observation samples
// ============================================================================

#ifndef AMT_DOM_EVENTS_H
#define AMT_DOM_EVENTS_H

#include "AMT_Patterns.h"
#include <vector>
#include <deque>
#include <optional>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace AMT {

// ============================================================================
// CONFIGURATION - Centralized thresholds for determinism and tuning
// ============================================================================

struct DomEventConfig
{
    // Buffer sizing
    static constexpr int HISTORY_BUFFER_SIZE = 64;        // Max samples in ring buffer
    static constexpr int MIN_SAMPLES = 6;                 // Minimum for feature extraction
    static constexpr int MIN_WINDOW_MS = 1000;            // Minimum window for detection (1 sec)
    static constexpr int DEFAULT_WINDOW_MS = 5000;        // Default detection window (5 sec)

    // MAD-based thresholds (k-factors for outlier detection)
    static constexpr double MAD_K_FACTOR = 2.5;           // Standard outlier threshold
    static constexpr double MAD_SCALE = 1.4826;           // Scale MAD to sigma equivalent

    // Liquidity patterns
    static constexpr double STACK_PULL_DOMINANCE_RATIO = 1.5;  // Stack/Pull must exceed other by 50%
    static constexpr double HALO_DEPTH_CHANGE_K = 2.0;         // K-factor for halo depth change

    // Aggressor patterns (lifting asks / hitting bids)
    static constexpr double AGGRESSOR_RATIO_THRESHOLD = 1.8;   // askVol/bidVol ratio for lifting
    static constexpr int BEST_PRICE_MOVE_TICKS = 2;            // Min ticks for directional move

    // Exhaustion divergence
    static constexpr double EXHAUSTION_DELTA_K = 2.0;          // Delta spike threshold
    static constexpr int EXHAUSTION_PRICE_MAX_TICKS = 2;       // Max price movement for "stall"

    // Event thresholds
    static constexpr double DISAPPEARANCE_K = 2.5;             // Halo depth drop threshold
    static constexpr int REVERSAL_MIN_SAMPLES = 4;             // Min sustained samples for reversal
    static constexpr double REVERSAL_MAGNITUDE_MIN = 0.3;      // Min delta magnitude for reversal
    static constexpr int SWEEP_MIN_TICKS = 3;                  // Min ticks for sweep detection
    static constexpr double SWEEP_DEPTH_DROP_K = 2.0;          // Depth collapse threshold

    // Observability
    static constexpr int LOG_THROTTLE_BARS = 10;               // Min bars between duplicate logs
};

// ============================================================================
// DOM OBSERVATION SAMPLE - Single snapshot of DOM state
// ============================================================================
// No Sierra types - uses primitive values only
// Timestamp stored as epoch milliseconds for portability

struct DomObservationSample
{
    int64_t timestampMs = 0;       // Epoch milliseconds (or relative session ms)
    int barIndex = -1;             // Bar index when captured

    // Best bid/ask in ticks (converted from price via PriceToTicks)
    int bestBidTick = 0;
    int bestAskTick = 0;

    // DOM depth totals (from LiquiditySnapshot)
    double domBidSize = 0.0;
    double domAskSize = 0.0;

    // Stack/Pull metrics (from LiquiditySnapshot)
    double bidStackPull = 0.0;
    double askStackPull = 0.0;

    // Halo depth mass (SSOT from ComputeDepthMassHalo - imbalance [-1, +1])
    double haloDepthImbalance = 0.0;
    bool haloDepthValid = false;

    // Effort metrics (from EffortSnapshot)
    double askVolSec = 0.0;        // At-ask volume per second
    double bidVolSec = 0.0;        // At-bid volume per second
    double deltaSec = 0.0;         // Delta per second
    double tradesSec = 0.0;        // Trades per second

    bool isValid() const { return timestampMs > 0 && barIndex >= 0; }
};

// ============================================================================
// DOM HISTORY BUFFER - Session-scoped circular buffer
// ============================================================================

struct DomHistoryBuffer
{
    std::deque<DomObservationSample> samples;
    static constexpr size_t MAX_SIZE = DomEventConfig::HISTORY_BUFFER_SIZE;

    void Push(const DomObservationSample& sample)
    {
        if (!sample.isValid()) return;
        samples.push_back(sample);
        while (samples.size() > MAX_SIZE)
            samples.pop_front();
    }

    void Reset()
    {
        samples.clear();
    }

    size_t Size() const { return samples.size(); }
    bool HasMinSamples() const { return samples.size() >= DomEventConfig::MIN_SAMPLES; }

    // Get samples within a time window (most recent windowMs)
    std::vector<DomObservationSample> GetWindow(int windowMs) const
    {
        std::vector<DomObservationSample> result;
        if (samples.empty()) return result;

        const int64_t cutoff = samples.back().timestampMs - windowMs;
        for (const auto& s : samples)
        {
            if (s.timestampMs >= cutoff)
                result.push_back(s);
        }
        return result;
    }

    // Get last N samples
    std::vector<DomObservationSample> GetLastN(size_t n) const
    {
        std::vector<DomObservationSample> result;
        const size_t start = (samples.size() > n) ? (samples.size() - n) : 0;
        for (size_t i = start; i < samples.size(); ++i)
            result.push_back(samples[i]);
        return result;
    }
};

// ============================================================================
// DOM EVENT FEATURES - Extracted from rolling window for detection
// ============================================================================

struct DomEventFeatures
{
    // Window info
    int windowMs = 0;
    int sampleCount = 0;
    bool isEligible = false;      // True if enough samples and valid window

    // Halo depth statistics (SSOT for near-touch liquidity)
    double haloDepthMedian = 0.0;
    double haloDepthMad = 0.0;
    double haloDepthCurrent = 0.0;
    double haloDepthZScore = 0.0;  // (current - median) / (MAD * 1.4826)
    bool haloDepthStatsValid = false;

    // Delta statistics
    double deltaSecMedian = 0.0;
    double deltaSecMad = 0.0;
    double deltaSecCurrent = 0.0;
    double deltaSecZScore = 0.0;
    bool deltaSecStatsValid = false;

    // Trades statistics
    double tradesSecMedian = 0.0;
    double tradesSecMad = 0.0;
    double tradesSecCurrent = 0.0;

    // Stack/Pull dominance (current snapshot)
    double stackDominance = 0.0;   // max(bidStackPull, 0) + max(askStackPull, 0)
    double pullDominance = 0.0;    // -min(bidStackPull, 0) - min(askStackPull, 0)

    // Depth ratio
    double bidAskDepthRatio = 1.0; // domBidSize / max(domAskSize, eps)

    // Aggressor ratio (current)
    double askVolSecCurrent = 0.0;
    double bidVolSecCurrent = 0.0;
    double aggressorRatio = 1.0;   // askVolSec / max(bidVolSec, eps)

    // Best price movement over window (in ticks)
    int bestBidMoveTicks = 0;      // current - oldest
    int bestAskMoveTicks = 0;

    // Delta sign persistence (for reversal detection)
    int consecutivePositiveDelta = 0;
    int consecutiveNegativeDelta = 0;
    bool deltaSignFlipped = false; // True if sign changed within window

    // Ineligibility reason (for debugging)
    const char* ineligibleReason = nullptr;
};

// ============================================================================
// FEATURE EXTRACTION - Pure function, no Sierra dependencies
// ============================================================================

inline double ComputeMedian(const std::vector<double>& vals)
{
    if (vals.empty()) return 0.0;
    std::vector<double> sorted = vals;
    std::sort(sorted.begin(), sorted.end());
    const size_t n = sorted.size();
    if (n % 2 == 0)
        return (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
    return sorted[n / 2];
}

inline double ComputeMAD(const std::vector<double>& vals, double median)
{
    if (vals.size() < 2) return 0.0;
    std::vector<double> absDevs;
    absDevs.reserve(vals.size());
    for (double v : vals)
        absDevs.push_back(std::abs(v - median));
    return ComputeMedian(absDevs);
}

inline DomEventFeatures ExtractFeatures(
    const std::vector<DomObservationSample>& window,
    int windowMs)
{
    DomEventFeatures f;
    f.windowMs = windowMs;
    f.sampleCount = static_cast<int>(window.size());

    // Eligibility check
    if (window.size() < DomEventConfig::MIN_SAMPLES)
    {
        f.isEligible = false;
        f.ineligibleReason = "INSUFFICIENT_SAMPLES";
        return f;
    }
    if (windowMs < DomEventConfig::MIN_WINDOW_MS)
    {
        f.isEligible = false;
        f.ineligibleReason = "WINDOW_TOO_SHORT";
        return f;
    }
    f.isEligible = true;

    const auto& current = window.back();
    const auto& oldest = window.front();

    // Collect values for statistics
    std::vector<double> haloVals, deltaSecVals, tradesSecVals;
    int positiveDeltaRun = 0, negativeDeltaRun = 0;
    double prevDeltaSign = 0.0;
    bool signFlipped = false;

    for (const auto& s : window)
    {
        if (s.haloDepthValid)
            haloVals.push_back(s.haloDepthImbalance);
        deltaSecVals.push_back(s.deltaSec);
        tradesSecVals.push_back(s.tradesSec);

        // Track delta sign persistence
        const double curSign = (s.deltaSec > 0.01) ? 1.0 : ((s.deltaSec < -0.01) ? -1.0 : 0.0);
        if (curSign != 0.0)
        {
            if (prevDeltaSign != 0.0 && curSign != prevDeltaSign)
            {
                signFlipped = true;
                positiveDeltaRun = (curSign > 0) ? 1 : 0;
                negativeDeltaRun = (curSign < 0) ? 1 : 0;
            }
            else
            {
                if (curSign > 0) positiveDeltaRun++;
                else if (curSign < 0) negativeDeltaRun++;
            }
            prevDeltaSign = curSign;
        }
    }

    // Halo depth statistics
    if (haloVals.size() >= DomEventConfig::MIN_SAMPLES)
    {
        f.haloDepthMedian = ComputeMedian(haloVals);
        f.haloDepthMad = ComputeMAD(haloVals, f.haloDepthMedian);
        f.haloDepthCurrent = current.haloDepthImbalance;
        if (f.haloDepthMad > 1e-9)
        {
            f.haloDepthZScore = (f.haloDepthCurrent - f.haloDepthMedian) /
                                (f.haloDepthMad * DomEventConfig::MAD_SCALE);
        }
        f.haloDepthStatsValid = true;
    }

    // Delta statistics
    if (deltaSecVals.size() >= DomEventConfig::MIN_SAMPLES)
    {
        f.deltaSecMedian = ComputeMedian(deltaSecVals);
        f.deltaSecMad = ComputeMAD(deltaSecVals, f.deltaSecMedian);
        f.deltaSecCurrent = current.deltaSec;
        if (f.deltaSecMad > 1e-9)
        {
            f.deltaSecZScore = (f.deltaSecCurrent - f.deltaSecMedian) /
                               (f.deltaSecMad * DomEventConfig::MAD_SCALE);
        }
        f.deltaSecStatsValid = true;
    }

    // Trades statistics
    if (!tradesSecVals.empty())
    {
        f.tradesSecMedian = ComputeMedian(tradesSecVals);
        f.tradesSecMad = ComputeMAD(tradesSecVals, f.tradesSecMedian);
        f.tradesSecCurrent = current.tradesSec;
    }

    // Stack/Pull dominance (current sample)
    f.stackDominance = (std::max)(current.bidStackPull, 0.0) +
                       (std::max)(current.askStackPull, 0.0);
    f.pullDominance = -(std::min)(current.bidStackPull, 0.0) -
                       (std::min)(current.askStackPull, 0.0);

    // Depth ratio
    constexpr double EPS = 1.0;
    f.bidAskDepthRatio = current.domBidSize / (std::max)(current.domAskSize, EPS);

    // Aggressor ratio
    f.askVolSecCurrent = current.askVolSec;
    f.bidVolSecCurrent = current.bidVolSec;
    f.aggressorRatio = current.askVolSec / (std::max)(current.bidVolSec, EPS);

    // Best price movement
    f.bestBidMoveTicks = current.bestBidTick - oldest.bestBidTick;
    f.bestAskMoveTicks = current.bestAskTick - oldest.bestAskTick;

    // Delta sign persistence
    f.consecutivePositiveDelta = positiveDeltaRun;
    f.consecutiveNegativeDelta = negativeDeltaRun;
    f.deltaSignFlipped = signFlipped;

    return f;
}

// ============================================================================
// DETECTORS - Pure functions returning optional hits
// ============================================================================

// --- DOMControlPattern Detectors ---

inline std::optional<DOMControlHit> DetectLiquidityPulling(
    const DomEventFeatures& f)
{
    if (!f.isEligible) return std::nullopt;

    // Pulling: pullDominance exceeds stackDominance by ratio threshold
    // AND haloDepth is decreasing (negative z-score)
    const bool pullDominant = (f.pullDominance > f.stackDominance * DomEventConfig::STACK_PULL_DOMINANCE_RATIO);
    const bool haloDecreasing = f.haloDepthStatsValid &&
                                (f.haloDepthZScore < -DomEventConfig::HALO_DEPTH_CHANGE_K);

    if (pullDominant && haloDecreasing)
    {
        DOMControlHit hit;
        hit.type = DOMControlPattern::LIQUIDITY_PULLING;
        // Strength based on z-score magnitude (clamped to [0,1])
        hit.strength01 = static_cast<float>((std::min)(1.0, std::abs(f.haloDepthZScore) / 4.0));
        hit.windowMs = f.windowMs;
        return hit;
    }
    return std::nullopt;
}

inline std::optional<DOMControlHit> DetectLiquidityStacking(
    const DomEventFeatures& f)
{
    if (!f.isEligible) return std::nullopt;

    // Stacking: stackDominance exceeds pullDominance by ratio threshold
    // AND haloDepth is increasing (positive z-score)
    const bool stackDominant = (f.stackDominance > f.pullDominance * DomEventConfig::STACK_PULL_DOMINANCE_RATIO);
    const bool haloIncreasing = f.haloDepthStatsValid &&
                                (f.haloDepthZScore > DomEventConfig::HALO_DEPTH_CHANGE_K);

    if (stackDominant && haloIncreasing)
    {
        DOMControlHit hit;
        hit.type = DOMControlPattern::LIQUIDITY_STACKING;
        hit.strength01 = static_cast<float>((std::min)(1.0, std::abs(f.haloDepthZScore) / 4.0));
        hit.windowMs = f.windowMs;
        return hit;
    }
    return std::nullopt;
}

inline std::optional<DOMControlHit> DetectBuyersLiftingAsks(
    const DomEventFeatures& f)
{
    if (!f.isEligible) return std::nullopt;

    // Buyers lifting asks: askVolSec dominates bidVolSec
    // AND bestAsk increases (or at least doesn't decrease)
    const bool askDominant = (f.aggressorRatio >= DomEventConfig::AGGRESSOR_RATIO_THRESHOLD);
    const bool priceAdvancing = (f.bestAskMoveTicks >= DomEventConfig::BEST_PRICE_MOVE_TICKS);

    if (askDominant && priceAdvancing)
    {
        DOMControlHit hit;
        hit.type = DOMControlPattern::BUYERS_LIFTING_ASKS;
        // Strength based on aggressor ratio (clamped)
        hit.strength01 = static_cast<float>((std::min)(1.0, f.aggressorRatio / 3.0));
        hit.windowMs = f.windowMs;
        return hit;
    }
    return std::nullopt;
}

inline std::optional<DOMControlHit> DetectSellersHittingBids(
    const DomEventFeatures& f)
{
    if (!f.isEligible) return std::nullopt;

    // Sellers hitting bids: bidVolSec dominates askVolSec (inverse ratio)
    // AND bestBid decreases
    const double inverseRatio = f.bidVolSecCurrent / (std::max)(f.askVolSecCurrent, 1.0);
    const bool bidDominant = (inverseRatio >= DomEventConfig::AGGRESSOR_RATIO_THRESHOLD);
    const bool priceDropping = (f.bestBidMoveTicks <= -DomEventConfig::BEST_PRICE_MOVE_TICKS);

    if (bidDominant && priceDropping)
    {
        DOMControlHit hit;
        hit.type = DOMControlPattern::SELLERS_HITTING_BIDS;
        hit.strength01 = static_cast<float>((std::min)(1.0, inverseRatio / 3.0));
        hit.windowMs = f.windowMs;
        return hit;
    }
    return std::nullopt;
}

inline std::optional<DOMControlHit> DetectExhaustionDivergence(
    const DomEventFeatures& f)
{
    if (!f.isEligible || !f.deltaSecStatsValid) return std::nullopt;

    // Exhaustion divergence: large delta spike (effort) with minimal price movement (no result)
    // Symmetric: applies to both buying and selling exhaustion
    const bool deltaSpike = (std::abs(f.deltaSecZScore) >= DomEventConfig::EXHAUSTION_DELTA_K);
    const bool priceStalled = (std::abs(f.bestBidMoveTicks) <= DomEventConfig::EXHAUSTION_PRICE_MAX_TICKS) &&
                              (std::abs(f.bestAskMoveTicks) <= DomEventConfig::EXHAUSTION_PRICE_MAX_TICKS);

    if (deltaSpike && priceStalled)
    {
        DOMControlHit hit;
        hit.type = DOMControlPattern::EXHAUSTION_DIVERGENCE;
        hit.strength01 = static_cast<float>((std::min)(1.0, std::abs(f.deltaSecZScore) / 4.0));
        hit.windowMs = f.windowMs;
        return hit;
    }
    return std::nullopt;
}

// --- DOMEvent Detectors ---

inline std::optional<DOMEventHit> DetectLiquidityDisappearance(
    const DomEventFeatures& f)
{
    if (!f.isEligible || !f.haloDepthStatsValid) return std::nullopt;

    // Halo depth drops sharply below median
    const bool disappeared = (f.haloDepthZScore < -DomEventConfig::DISAPPEARANCE_K);

    if (disappeared)
    {
        DOMEventHit hit;
        hit.type = DOMEvent::LIQUIDITY_DISAPPEARANCE;
        hit.strength01 = static_cast<float>((std::min)(1.0, std::abs(f.haloDepthZScore) / 4.0));
        hit.windowMs = f.windowMs;
        return hit;
    }
    return std::nullopt;
}

inline std::optional<DOMEventHit> DetectOrderFlowReversal(
    const DomEventFeatures& f)
{
    if (!f.isEligible || !f.deltaSecStatsValid) return std::nullopt;

    // Sign flip detected and sustained for minimum samples
    // AND current delta magnitude exceeds threshold
    const bool signFlipped = f.deltaSignFlipped;
    const int sustainedSamples = (std::max)(f.consecutivePositiveDelta, f.consecutiveNegativeDelta);
    const bool sustained = (sustainedSamples >= DomEventConfig::REVERSAL_MIN_SAMPLES);
    const bool significantMagnitude = (std::abs(f.deltaSecCurrent) >= DomEventConfig::REVERSAL_MAGNITUDE_MIN);

    if (signFlipped && sustained && significantMagnitude)
    {
        DOMEventHit hit;
        hit.type = DOMEvent::ORDER_FLOW_REVERSAL;
        // Strength based on sustained samples and magnitude
        const double sustainScore = (std::min)(1.0, static_cast<double>(sustainedSamples) / 8.0);
        const double magScore = (std::min)(1.0, std::abs(f.deltaSecCurrent));
        hit.strength01 = static_cast<float>(sustainScore * 0.5 + magScore * 0.5);
        hit.windowMs = f.windowMs;
        return hit;
    }
    return std::nullopt;
}

inline std::optional<DOMEventHit> DetectSweepLiquidation(
    const DomEventFeatures& f)
{
    if (!f.isEligible) return std::nullopt;

    // Best price moves rapidly across multiple ticks
    // AND halo depth collapses (if available) OR trades spike
    const int priceMove = (std::max)(std::abs(f.bestBidMoveTicks), std::abs(f.bestAskMoveTicks));
    const bool rapidMove = (priceMove >= DomEventConfig::SWEEP_MIN_TICKS);

    bool depthCollapse = false;
    if (f.haloDepthStatsValid)
    {
        depthCollapse = (f.haloDepthZScore < -DomEventConfig::SWEEP_DEPTH_DROP_K);
    }

    // Fallback: trades spike if no depth stats
    bool tradeSpike = false;
    if (f.tradesSecMad > 1e-9)
    {
        const double tradesZ = (f.tradesSecCurrent - f.tradesSecMedian) /
                               (f.tradesSecMad * DomEventConfig::MAD_SCALE);
        tradeSpike = (tradesZ > DomEventConfig::MAD_K_FACTOR);
    }

    if (rapidMove && (depthCollapse || tradeSpike))
    {
        DOMEventHit hit;
        hit.type = DOMEvent::SWEEP_LIQUIDATION;
        hit.strength01 = static_cast<float>((std::min)(1.0, static_cast<double>(priceMove) / 6.0));
        hit.windowMs = f.windowMs;
        return hit;
    }
    return std::nullopt;
}

inline std::optional<DOMEventHit> DetectLargeLotExecution(
    const DomEventFeatures& /* f */)
{
    // DEFERRED: No trade size primitive available
    // This detector always returns nullopt in v1
    // Future: requires per-trade size data or max trade size tracking
    return std::nullopt;
}

// ============================================================================
// DETECTION RESULT - Aggregated output from all detectors
// ============================================================================

struct DomDetectionResult
{
    std::vector<DOMControlPattern> controlPatterns;
    std::vector<DOMEvent> events;
    std::vector<DOMControlHit> controlHits;
    std::vector<DOMEventHit> eventHits;

    int windowMs = 0;
    bool wasEligible = false;
    const char* ineligibleReason = nullptr;

    bool HasPatterns() const { return !controlPatterns.empty() || !events.empty(); }
};

// ============================================================================
// MAIN DETECTION FUNCTION - Called from integration point
// ============================================================================

inline DomDetectionResult DetectDomEventsAndControl(
    const DomHistoryBuffer& buffer,
    int windowMs = DomEventConfig::DEFAULT_WINDOW_MS)
{
    DomDetectionResult result;
    result.windowMs = windowMs;

    // Get window samples
    auto window = buffer.GetWindow(windowMs);

    // Extract features
    DomEventFeatures f = ExtractFeatures(window, windowMs);
    result.wasEligible = f.isEligible;
    result.ineligibleReason = f.ineligibleReason;

    if (!f.isEligible) return result;

    // Run all DOMControlPattern detectors
    if (auto hit = DetectLiquidityPulling(f))
    {
        result.controlPatterns.push_back(hit->type);
        result.controlHits.push_back(*hit);
    }
    if (auto hit = DetectLiquidityStacking(f))
    {
        result.controlPatterns.push_back(hit->type);
        result.controlHits.push_back(*hit);
    }
    if (auto hit = DetectBuyersLiftingAsks(f))
    {
        result.controlPatterns.push_back(hit->type);
        result.controlHits.push_back(*hit);
    }
    if (auto hit = DetectSellersHittingBids(f))
    {
        result.controlPatterns.push_back(hit->type);
        result.controlHits.push_back(*hit);
    }
    if (auto hit = DetectExhaustionDivergence(f))
    {
        result.controlPatterns.push_back(hit->type);
        result.controlHits.push_back(*hit);
    }

    // Run all DOMEvent detectors
    if (auto hit = DetectLiquidityDisappearance(f))
    {
        result.events.push_back(hit->type);
        result.eventHits.push_back(*hit);
    }
    if (auto hit = DetectOrderFlowReversal(f))
    {
        result.events.push_back(hit->type);
        result.eventHits.push_back(*hit);
    }
    if (auto hit = DetectSweepLiquidation(f))
    {
        result.events.push_back(hit->type);
        result.eventHits.push_back(*hit);
    }
    // Note: DetectLargeLotExecution always returns nullopt (deferred)

    // Sort hits for deterministic ordering
    std::sort(result.controlHits.begin(), result.controlHits.end());
    std::sort(result.eventHits.begin(), result.eventHits.end());

    return result;
}

// ============================================================================
// OBSERVABILITY - Log state tracker for de-duplication
// ============================================================================

struct DomEventLogState
{
    int lastLogBar = -1;
    std::vector<DOMControlPattern> lastControlPatterns;
    std::vector<DOMEvent> lastEvents;
    bool firstEmissionDone = false;

    void Reset()
    {
        lastLogBar = -1;
        lastControlPatterns.clear();
        lastEvents.clear();
        firstEmissionDone = false;
    }

    // Returns true if the emitted set changed (should log)
    bool ShouldLog(const DomDetectionResult& result, int currentBar)
    {
        // Throttle: don't log too frequently
        if (currentBar - lastLogBar < DomEventConfig::LOG_THROTTLE_BARS &&
            firstEmissionDone)
        {
            return false;
        }

        // Check if patterns changed
        bool changed = false;

        if (result.controlPatterns.size() != lastControlPatterns.size() ||
            result.events.size() != lastEvents.size())
        {
            changed = true;
        }
        else
        {
            // Check actual content
            for (size_t i = 0; i < result.controlPatterns.size(); ++i)
            {
                if (result.controlPatterns[i] != lastControlPatterns[i])
                {
                    changed = true;
                    break;
                }
            }
            if (!changed)
            {
                for (size_t i = 0; i < result.events.size(); ++i)
                {
                    if (result.events[i] != lastEvents[i])
                    {
                        changed = true;
                        break;
                    }
                }
            }
        }

        // First emission OR changed
        if (!firstEmissionDone || changed)
        {
            lastLogBar = currentBar;
            lastControlPatterns = result.controlPatterns;
            lastEvents = result.events;
            firstEmissionDone = true;
            return true;
        }

        return false;
    }
};

// ============================================================================
// LOG MESSAGE BUILDER - For observability
// ============================================================================

inline std::string BuildDomEventLogMessage(
    const DomDetectionResult& result,
    int64_t timestampMs)
{
    std::string msg = "[DOM-EVENT] t=" + std::to_string(timestampMs) + "ms";
    msg += " | window=" + std::to_string(result.windowMs) + "ms";
    msg += " | control=" + std::to_string(result.controlPatterns.size());
    msg += " events=" + std::to_string(result.events.size());

    if (!result.controlHits.empty())
    {
        msg += " |";
        for (const auto& hit : result.controlHits)
        {
            msg += " ";
            msg += to_string(hit.type);
            msg += "(";
            // Format strength to 2 decimal places without <iomanip>
            int strengthPct = static_cast<int>(hit.strength01 * 100);
            msg += "0.";
            if (strengthPct < 10) msg += "0";
            msg += std::to_string(strengthPct);
            msg += ")";
        }
    }

    if (!result.eventHits.empty())
    {
        msg += " |";
        for (const auto& hit : result.eventHits)
        {
            msg += " ";
            msg += to_string(hit.type);
            msg += "(";
            int strengthPct = static_cast<int>(hit.strength01 * 100);
            msg += "0.";
            if (strengthPct < 10) msg += "0";
            msg += std::to_string(strengthPct);
            msg += ")";
        }
    }

    return msg;
}

} // namespace AMT

#endif // AMT_DOM_EVENTS_H
