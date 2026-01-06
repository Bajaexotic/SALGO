// ============================================================================
// AMT_DomEvents.h
// DOM Event Detection Module - Pure detection with no Sierra dependencies
// Detects: DOMControlPattern and DOMEvent from DOM observation samples
// ============================================================================

#ifndef AMT_DOM_EVENTS_H
#define AMT_DOM_EVENTS_H

#include "AMT_Patterns.h"
#include "AMT_ValueLocation.h"  // For ValueZone (SSOT for value location)
#include <vector>
#include <deque>
#include <optional>
#include <algorithm>
#include <array>
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
// HIT STRUCTS - Return types for pattern detectors
// ============================================================================

struct DOMControlHit
{
    DOMControlPattern type = DOMControlPattern::NONE;
    float strength01 = 0.0f;    // [0, 1] strength of the pattern
    int windowMs = 0;           // Detection window used

    // Sort by strength descending (strongest patterns first)
    bool operator<(const DOMControlHit& other) const {
        return strength01 > other.strength01;
    }
};

struct DOMEventHit
{
    DOMEvent type = DOMEvent::NONE;
    float strength01 = 0.0f;    // [0, 1] strength of the event
    int windowMs = 0;           // Detection window used

    // Sort by strength descending (strongest events first)
    bool operator<(const DOMEventHit& other) const {
        return strength01 > other.strength01;
    }
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

// ============================================================================
// SPATIAL DOM TIME-SERIES TRACKING
// Per-price-level DOM snapshots for order flow pattern detection
// ============================================================================

// Configuration for spatial DOM tracking
struct SpatialDomConfig
{
    // Buffer sizing
    static constexpr int LEVELS_PER_SIDE = 10;      // +/- 10 levels from reference
    static constexpr int TOTAL_LEVELS = 20;          // 10 bid + 10 ask
    static constexpr int HISTORY_SIZE = 32;          // Samples in ring buffer
    static constexpr int MIN_SAMPLES = 5;            // Minimum for pattern detection
    static constexpr int DEFAULT_WINDOW_MS = 3000;   // 3 second detection window

    // Spoofing detection thresholds
    static constexpr double SPOOF_DISAPPEAR_RATIO = 0.20;   // <20% of original = disappeared
    static constexpr double SPOOF_MIN_SIZE_PCTILE = 80.0;   // Must be above P80 to be "large"
    static constexpr int SPOOF_MIN_APPEAR_MS = 500;         // Visible for at least 500ms
    static constexpr int SPOOF_MAX_DISAPPEAR_MS = 2000;     // Vanishes within 2 seconds

    // Iceberg detection thresholds
    static constexpr double ICEBERG_REFILL_RATIO = 0.70;    // Maintains >70% of peak
    static constexpr int ICEBERG_MIN_REFILLS = 3;           // At least 3 refills observed
    static constexpr double ICEBERG_DEPLETE_RATIO = 0.50;   // Falls below 50% before refill

    // Wall break detection thresholds
    static constexpr double WALL_BREAK_RATIO = 0.30;        // <30% remaining = broken
    static constexpr int WALL_BREAK_MIN_BARS = 2;           // Over at least 2 bars
    static constexpr double WALL_MIN_SIZE_PCTILE = 90.0;    // Must be above P90 to be "wall"

    // Flip detection thresholds
    static constexpr double FLIP_MIN_RATIO = 2.0;           // Must be >2x imbalance to flip
    static constexpr int FLIP_TOLERANCE_TICKS = 2;          // Same level within 2 ticks
    static constexpr double FLIP_MIN_QUANTITY = 50.0;       // Minimum quantity for flip

    // Observability
    static constexpr int LOG_THROTTLE_BARS = 5;             // Min bars between duplicate logs
};

// ============================================================================
// SPATIAL DOM LEVEL - Single price level in DOM
// ============================================================================

struct SpatialDomLevel
{
    int tickOffset = 0;           // Offset from reference price in ticks (-10 to +10)
    double quantity = 0.0;        // Resting quantity at this level
    bool isBid = false;           // True = bid side, False = ask side
    bool isValid = false;         // True if level exists in DOM

    // Clear to default state
    void Clear()
    {
        tickOffset = 0;
        quantity = 0.0;
        isBid = false;
        isValid = false;
    }
};

// ============================================================================
// SPATIAL DOM SNAPSHOT - Full DOM capture at a point in time
// ============================================================================

struct SpatialDomSnapshot
{
    int64_t timestampMs = 0;         // Epoch milliseconds
    int barIndex = -1;               // Bar when captured
    double referencePrice = 0.0;     // Reference price for offset calculation
    double tickSize = 0.0;           // Tick size for conversions

    // Per-level data: index 0-9 = bid side (closest to farthest from ref)
    //                 index 10-19 = ask side (closest to farthest from ref)
    std::array<SpatialDomLevel, SpatialDomConfig::TOTAL_LEVELS> levels;

    // Summary metrics (for quick filtering)
    double totalBidQuantity = 0.0;
    double totalAskQuantity = 0.0;
    double maxBidQuantity = 0.0;     // Largest single bid level
    double maxAskQuantity = 0.0;     // Largest single ask level
    int maxBidOffset = 0;            // Offset of largest bid
    int maxAskOffset = 0;            // Offset of largest ask

    // Validity check
    bool isValid() const { return timestampMs > 0 && barIndex >= 0 && tickSize > 0.0; }

    // Accessors for bid/ask by index (0 = closest to ref, 9 = farthest)
    const SpatialDomLevel& GetBidByIndex(int idx) const
    {
        return levels[(std::min)(idx, SpatialDomConfig::LEVELS_PER_SIDE - 1)];
    }

    const SpatialDomLevel& GetAskByIndex(int idx) const
    {
        return levels[SpatialDomConfig::LEVELS_PER_SIDE +
                      (std::min)(idx, SpatialDomConfig::LEVELS_PER_SIDE - 1)];
    }

    // Get level by tick offset from reference (-10 to +10)
    const SpatialDomLevel* GetLevelAtOffset(int tickOff) const
    {
        if (tickOff < 0)
        {
            // Bid side: -1 = index 0, -2 = index 1, etc.
            const int idx = -tickOff - 1;
            if (idx >= 0 && idx < SpatialDomConfig::LEVELS_PER_SIDE)
                return &levels[idx];
        }
        else if (tickOff > 0)
        {
            // Ask side: +1 = index 10, +2 = index 11, etc.
            const int idx = SpatialDomConfig::LEVELS_PER_SIDE + tickOff - 1;
            if (idx < SpatialDomConfig::TOTAL_LEVELS)
                return &levels[idx];
        }
        return nullptr;
    }

    // Clear all levels
    void Clear()
    {
        timestampMs = 0;
        barIndex = -1;
        referencePrice = 0.0;
        tickSize = 0.0;
        for (auto& level : levels)
            level.Clear();
        totalBidQuantity = 0.0;
        totalAskQuantity = 0.0;
        maxBidQuantity = 0.0;
        maxAskQuantity = 0.0;
        maxBidOffset = 0;
        maxAskOffset = 0;
    }
};

// ============================================================================
// SPATIAL DOM HISTORY BUFFER - Time-series of DOM snapshots
// ============================================================================

struct SpatialDomHistoryBuffer
{
    std::deque<SpatialDomSnapshot> samples;
    static constexpr size_t MAX_SIZE = SpatialDomConfig::HISTORY_SIZE;

    void Push(const SpatialDomSnapshot& snapshot)
    {
        if (!snapshot.isValid()) return;
        samples.push_back(snapshot);
        while (samples.size() > MAX_SIZE)
            samples.pop_front();
    }

    void Reset()
    {
        samples.clear();
    }

    size_t Size() const { return samples.size(); }

    bool HasMinSamples() const
    {
        return samples.size() >= static_cast<size_t>(SpatialDomConfig::MIN_SAMPLES);
    }

    // Get samples within time window (most recent windowMs)
    std::vector<SpatialDomSnapshot> GetWindow(int windowMs) const
    {
        std::vector<SpatialDomSnapshot> result;
        if (samples.empty()) return result;

        const int64_t cutoff = samples.back().timestampMs - windowMs;
        for (const auto& s : samples)
        {
            if (s.timestampMs >= cutoff)
                result.push_back(s);
        }
        return result;
    }

    // Get most recent N samples
    std::vector<SpatialDomSnapshot> GetLastN(size_t n) const
    {
        std::vector<SpatialDomSnapshot> result;
        const size_t start = (samples.size() > n) ? (samples.size() - n) : 0;
        for (size_t i = start; i < samples.size(); ++i)
            result.push_back(samples[i]);
        return result;
    }

    // Get first and last sample for change detection
    bool GetFirstLast(SpatialDomSnapshot& first, SpatialDomSnapshot& last) const
    {
        if (samples.size() < 2) return false;
        first = samples.front();
        last = samples.back();
        return true;
    }

    // Get the most recent sample
    const SpatialDomSnapshot* GetLatest() const
    {
        return samples.empty() ? nullptr : &samples.back();
    }
};

// ============================================================================
// DOM PATTERN CONTEXT - Auction context for pattern interpretation
// ============================================================================
// Per AMT: The same DOM pattern means different things depending on WHERE
// in the auction it occurs. This context is used to adjust significance.
// ============================================================================

// NOTE: Value location uses ValueZone from AMT_ValueLocation.h (SSOT)
// The mapping from ValueZone (9 states) to DOM significance:
//   - AT_POC: Patterns are often noise (rotation expected)
//   - AT_VAH, AT_VAL: Patterns are highly significant (defense/attack)
//   - UPPER_VALUE, LOWER_VALUE: Inside value, moderate significance
//   - NEAR_ABOVE_VALUE, NEAR_BELOW_VALUE: Outside but testing, significant
//   - FAR_ABOVE_VALUE, FAR_BELOW_VALUE: Discovery, very significant

// Simplified market state for DOM context
enum class DomMarketState : int
{
    UNKNOWN = 0,
    BALANCE,          // 2TF - rotation, both sides active
    IMBALANCE         // 1TF - one side in control, trending
};

// Pattern interpretation hint based on context
enum class PatternInterpretation : int
{
    NOISE = 0,        // Low significance, likely noise
    DEFENSIVE,        // Defending a level (responsive)
    AGGRESSIVE,       // Attacking a level (initiative)
    EXHAUSTION,       // Trend exhaustion signal
    ACCUMULATION,     // Hidden accumulation/distribution
    BREAKOUT_SIGNAL,  // Potential breakout confirmation
    REJECTION_SIGNAL, // Rejection of price level
    TRAPPED_TRADERS   // Trapped longs/shorts
};

// Full context for DOM pattern interpretation
struct DomPatternContext
{
    // Location context - uses ValueZone from ValueLocationEngine (SSOT)
    ValueZone valueZone = ValueZone::UNKNOWN;
    double distanceFromPOCTicks = 0.0;
    double distanceFromVAHTicks = 0.0;
    double distanceFromVALTicks = 0.0;
    double distanceFromSessionHighTicks = 0.0;
    double distanceFromSessionLowTicks = 0.0;

    // Market state context
    DomMarketState marketState = DomMarketState::UNKNOWN;
    bool is1TF = false;  // One-time framing (imbalance)
    bool is2TF = false;  // Two-time framing (balance)

    // Value migration context
    bool valueMigratingHigher = false;
    bool valueMigratingLower = false;
    bool pocMovingTowardPrice = false;

    // Session context
    bool isInitialBalance = false;  // In IB window
    bool isNearIBExtreme = false;   // Near IB high/low
    bool isNearSessionExtreme = false;  // Near session high/low

    // Price direction context
    bool priceRising = false;
    bool priceFalling = false;

    // Validity
    bool isValid = false;

    // Helpers - map ValueZone to semantic queries
    bool IsAtValueEdge() const
    {
        return valueZone == ValueZone::AT_VAH ||
               valueZone == ValueZone::AT_VAL;
    }

    bool IsOutsideValue() const
    {
        return valueZone == ValueZone::NEAR_ABOVE_VALUE ||
               valueZone == ValueZone::FAR_ABOVE_VALUE ||
               valueZone == ValueZone::NEAR_BELOW_VALUE ||
               valueZone == ValueZone::FAR_BELOW_VALUE;
    }

    bool IsInDiscovery() const
    {
        return valueZone == ValueZone::FAR_ABOVE_VALUE ||
               valueZone == ValueZone::FAR_BELOW_VALUE;
    }

    bool IsAtPOC() const
    {
        return valueZone == ValueZone::AT_POC;
    }

    bool IsInsideValue() const
    {
        return valueZone == ValueZone::UPPER_VALUE ||
               valueZone == ValueZone::LOWER_VALUE ||
               valueZone == ValueZone::AT_POC;
    }

    bool IsInBalance() const
    {
        return marketState == DomMarketState::BALANCE || is2TF;
    }

    bool IsInImbalance() const
    {
        return marketState == DomMarketState::IMBALANCE || is1TF;
    }

    // PREFERRED: Build context from ValueLocationEngine output (SSOT-compliant)
    static DomPatternContext BuildFromValueLocation(
        const ValueLocationResult& valLocResult,
        bool is1TFState,
        bool valueMigHigh,
        bool valueMigLow,
        bool priceUp,
        bool priceDown)
    {
        DomPatternContext ctx;
        if (!valLocResult.IsReady()) return ctx;

        // Use SSOT values directly
        ctx.valueZone = valLocResult.confirmedZone;  // Hysteresis-confirmed zone
        ctx.distanceFromPOCTicks = valLocResult.distFromPOCTicks;
        ctx.distanceFromVAHTicks = valLocResult.distFromVAHTicks;
        ctx.distanceFromVALTicks = valLocResult.distFromVALTicks;
        ctx.distanceFromSessionHighTicks = valLocResult.distToSessionHighTicks;
        ctx.distanceFromSessionLowTicks = valLocResult.distToSessionLowTicks;

        // Market state
        ctx.is1TF = is1TFState;
        ctx.is2TF = !is1TFState;
        ctx.marketState = is1TFState ? DomMarketState::IMBALANCE : DomMarketState::BALANCE;

        // Value migration
        ctx.valueMigratingHigher = valueMigHigh;
        ctx.valueMigratingLower = valueMigLow;

        // Price direction
        ctx.priceRising = priceUp;
        ctx.priceFalling = priceDown;

        // Session extremes (use SSOT distances)
        const double edgeTolerance = 3.0;
        ctx.isNearSessionExtreme = (std::abs(valLocResult.distToSessionHighTicks) <= edgeTolerance ||
                                    std::abs(valLocResult.distToSessionLowTicks) <= edgeTolerance);

        // IB context
        ctx.isNearIBExtreme = (std::abs(valLocResult.distToIBHighTicks) <= edgeTolerance ||
                               std::abs(valLocResult.distToIBLowTicks) <= edgeTolerance);
        ctx.isInitialBalance = !valLocResult.isIBComplete;

        ctx.isValid = true;
        return ctx;
    }

    // DEPRECATED: Build from raw values (computes location internally - duplicates ValueLocationEngine)
    // Use BuildFromValueLocation() with ValueLocationEngine output instead.
    static DomPatternContext Build(
        double currentPrice,
        double poc,
        double vah,
        double val,
        double sessionHigh,
        double sessionLow,
        double tickSize,
        bool is1TFState,
        bool valueMigHigh,
        bool valueMigLow,
        bool priceUp,
        bool priceDown,
        double edgeToleranceTicks = 2.0,
        double discoveryThresholdTicks = 10.0)
    {
        DomPatternContext ctx;
        if (tickSize <= 0.0) return ctx;

        ctx.distanceFromPOCTicks = (currentPrice - poc) / tickSize;
        ctx.distanceFromVAHTicks = (currentPrice - vah) / tickSize;
        ctx.distanceFromVALTicks = (currentPrice - val) / tickSize;
        ctx.distanceFromSessionHighTicks = (sessionHigh - currentPrice) / tickSize;
        ctx.distanceFromSessionLowTicks = (currentPrice - sessionLow) / tickSize;

        // Determine value zone (mirrors ValueLocationEngine logic)
        const double absDistPOC = std::abs(ctx.distanceFromPOCTicks);
        const double absDistVAH = std::abs(ctx.distanceFromVAHTicks);
        const double absDistVAL = std::abs(ctx.distanceFromVALTicks);

        if (absDistPOC <= edgeToleranceTicks)
        {
            ctx.valueZone = ValueZone::AT_POC;
        }
        else if (absDistVAH <= edgeToleranceTicks)
        {
            ctx.valueZone = ValueZone::AT_VAH;
        }
        else if (absDistVAL <= edgeToleranceTicks)
        {
            ctx.valueZone = ValueZone::AT_VAL;
        }
        else if (currentPrice > vah)
        {
            ctx.valueZone = (ctx.distanceFromVAHTicks > discoveryThresholdTicks)
                ? ValueZone::FAR_ABOVE_VALUE
                : ValueZone::NEAR_ABOVE_VALUE;
        }
        else if (currentPrice < val)
        {
            ctx.valueZone = (std::abs(ctx.distanceFromVALTicks) > discoveryThresholdTicks)
                ? ValueZone::FAR_BELOW_VALUE
                : ValueZone::NEAR_BELOW_VALUE;
        }
        else if (currentPrice > poc)
        {
            ctx.valueZone = ValueZone::UPPER_VALUE;
        }
        else
        {
            ctx.valueZone = ValueZone::LOWER_VALUE;
        }

        // Market state
        ctx.is1TF = is1TFState;
        ctx.is2TF = !is1TFState;
        ctx.marketState = is1TFState ? DomMarketState::IMBALANCE : DomMarketState::BALANCE;

        // Value migration
        ctx.valueMigratingHigher = valueMigHigh;
        ctx.valueMigratingLower = valueMigLow;

        // Price direction
        ctx.priceRising = priceUp;
        ctx.priceFalling = priceDown;

        // Session extremes
        ctx.isNearSessionExtreme = (ctx.distanceFromSessionHighTicks <= edgeToleranceTicks ||
                                    ctx.distanceFromSessionLowTicks <= edgeToleranceTicks);

        ctx.isValid = true;
        return ctx;
    }
};

// ============================================================================
// CONTEXT SIGNIFICANCE MULTIPLIERS
// ============================================================================
// Per AMT, patterns have different significance based on location:
//   - At POC: Patterns are often noise (rotation expected)
//   - At VAH/VAL: Patterns are highly significant (defense/attack)
//   - Outside Value: Patterns indicate acceptance/rejection
//   - In Discovery: Patterns are very significant (trend confirmation)
// ============================================================================

struct ContextSignificanceConfig
{
    // Location multipliers (applied to base strength)
    static constexpr float AT_POC_MULT = 0.5f;           // Lower significance at POC
    static constexpr float INSIDE_VALUE_MULT = 0.7f;     // Moderate inside value
    static constexpr float AT_EDGE_MULT = 1.5f;          // High at VAH/VAL
    static constexpr float OUTSIDE_VALUE_MULT = 1.3f;    // Elevated outside value
    static constexpr float IN_DISCOVERY_MULT = 1.5f;     // High in discovery

    // Market state multipliers
    static constexpr float BALANCE_MULT = 0.8f;          // Patterns less actionable in balance
    static constexpr float IMBALANCE_MULT = 1.2f;        // Patterns more actionable in trend

    // Pattern-specific location adjustments
    // Spoofing significance by location
    static constexpr float SPOOF_AT_POC = 0.3f;          // Very low - noise
    static constexpr float SPOOF_AT_EDGE = 1.8f;         // Very high - manipulation
    static constexpr float SPOOF_IN_DISCOVERY = 1.5f;    // High - trend protection

    // Iceberg significance by location
    static constexpr float ICE_AT_POC = 0.6f;            // Moderate - accumulation
    static constexpr float ICE_AT_EDGE = 1.6f;           // High - defense
    static constexpr float ICE_OUTSIDE = 1.4f;           // High - hidden buying/selling

    // Wall break significance by location
    static constexpr float WALL_AT_POC = 0.5f;           // Low - normal rotation
    static constexpr float WALL_AT_EDGE = 2.0f;          // Very high - breakout signal
    static constexpr float WALL_OUTSIDE = 1.3f;          // Elevated - continuation

    // Flip significance by location
    static constexpr float FLIP_AT_POC = 0.7f;           // Moderate - rotation
    static constexpr float FLIP_AT_EDGE = 1.8f;          // Very high - trapped traders
    static constexpr float FLIP_OUTSIDE = 1.5f;          // High - trend exhaustion
};

// Compute context-adjusted significance for each pattern type
inline float ComputeSpoofingSignificance(float baseStrength, const DomPatternContext& ctx)
{
    if (!ctx.isValid) return baseStrength;

    float mult = 1.0f;

    // Location adjustment
    if (ctx.IsAtPOC())
        mult *= ContextSignificanceConfig::SPOOF_AT_POC;
    else if (ctx.IsAtValueEdge())
        mult *= ContextSignificanceConfig::SPOOF_AT_EDGE;
    else if (ctx.IsInDiscovery())
        mult *= ContextSignificanceConfig::SPOOF_IN_DISCOVERY;
    else if (ctx.IsOutsideValue())
        mult *= ContextSignificanceConfig::OUTSIDE_VALUE_MULT;
    else
        mult *= ContextSignificanceConfig::INSIDE_VALUE_MULT;

    // Market state adjustment
    if (ctx.IsInImbalance())
        mult *= ContextSignificanceConfig::IMBALANCE_MULT;
    else
        mult *= ContextSignificanceConfig::BALANCE_MULT;

    // Near session extreme boost
    if (ctx.isNearSessionExtreme)
        mult *= 1.3f;

    return (std::min)(baseStrength * mult, 1.0f);
}

inline float ComputeIcebergSignificance(float baseStrength, const DomPatternContext& ctx)
{
    if (!ctx.isValid) return baseStrength;

    float mult = 1.0f;

    // Location adjustment
    if (ctx.IsAtPOC())
        mult *= ContextSignificanceConfig::ICE_AT_POC;
    else if (ctx.IsAtValueEdge())
        mult *= ContextSignificanceConfig::ICE_AT_EDGE;
    else if (ctx.IsOutsideValue())
        mult *= ContextSignificanceConfig::ICE_OUTSIDE;
    else
        mult *= ContextSignificanceConfig::INSIDE_VALUE_MULT;

    // Iceberg in balance is accumulation (interesting)
    if (ctx.IsInBalance())
        mult *= 1.1f;

    return (std::min)(baseStrength * mult, 1.0f);
}

inline float ComputeWallBreakSignificance(float baseStrength, const DomPatternContext& ctx)
{
    if (!ctx.isValid) return baseStrength;

    float mult = 1.0f;

    // Location adjustment - wall break at edge is KEY signal
    if (ctx.IsAtPOC())
        mult *= ContextSignificanceConfig::WALL_AT_POC;
    else if (ctx.IsAtValueEdge())
        mult *= ContextSignificanceConfig::WALL_AT_EDGE;
    else if (ctx.IsOutsideValue())
        mult *= ContextSignificanceConfig::WALL_OUTSIDE;
    else
        mult *= ContextSignificanceConfig::INSIDE_VALUE_MULT;

    // In imbalance, wall break confirms trend
    if (ctx.IsInImbalance())
        mult *= 1.3f;

    // Near session extreme is very significant
    if (ctx.isNearSessionExtreme)
        mult *= 1.4f;

    return (std::min)(baseStrength * mult, 1.0f);
}

inline float ComputeFlipSignificance(float baseStrength, const DomPatternContext& ctx)
{
    if (!ctx.isValid) return baseStrength;

    float mult = 1.0f;

    // Location adjustment
    if (ctx.IsAtPOC())
        mult *= ContextSignificanceConfig::FLIP_AT_POC;
    else if (ctx.IsAtValueEdge())
        mult *= ContextSignificanceConfig::FLIP_AT_EDGE;
    else if (ctx.IsOutsideValue())
        mult *= ContextSignificanceConfig::FLIP_OUTSIDE;
    else
        mult *= ContextSignificanceConfig::INSIDE_VALUE_MULT;

    // Flip during imbalance = potential exhaustion
    if (ctx.IsInImbalance())
        mult *= 1.2f;

    return (std::min)(baseStrength * mult, 1.0f);
}

// Derive interpretation based on pattern type and context
inline PatternInterpretation InterpretSpoofing(const DomPatternContext& ctx, bool isBidSide)
{
    if (!ctx.isValid) return PatternInterpretation::NOISE;

    if (ctx.IsAtValueEdge())
    {
        // Spoofing at edge - someone manipulating to prevent breakout
        return isBidSide ? PatternInterpretation::DEFENSIVE : PatternInterpretation::AGGRESSIVE;
    }
    if (ctx.IsOutsideValue())
    {
        // Spoofing outside value - manipulation during discovery
        return PatternInterpretation::AGGRESSIVE;
    }
    if (ctx.IsAtPOC())
    {
        return PatternInterpretation::NOISE;  // Normal rotation noise
    }
    return PatternInterpretation::NOISE;
}

inline PatternInterpretation InterpretIceberg(const DomPatternContext& ctx, bool isBidSide)
{
    if (!ctx.isValid) return PatternInterpretation::ACCUMULATION;

    if (ctx.IsAtValueEdge())
    {
        // Iceberg at edge = strong defense
        return PatternInterpretation::DEFENSIVE;
    }
    if (ctx.IsOutsideValue())
    {
        // Hidden buying/selling in discovery
        return PatternInterpretation::ACCUMULATION;
    }
    // Inside value - passive accumulation
    return PatternInterpretation::ACCUMULATION;
}

inline PatternInterpretation InterpretWallBreak(const DomPatternContext& ctx, bool isBidSide)
{
    if (!ctx.isValid) return PatternInterpretation::BREAKOUT_SIGNAL;

    if (ctx.IsAtValueEdge())
    {
        // Wall break at edge = breakout confirmation
        return PatternInterpretation::BREAKOUT_SIGNAL;
    }
    if (ctx.IsOutsideValue())
    {
        // Wall break outside value = trend continuation
        return PatternInterpretation::AGGRESSIVE;
    }
    if (ctx.IsAtPOC())
    {
        // Wall break at POC = rotation, less significant
        return PatternInterpretation::NOISE;
    }
    return PatternInterpretation::BREAKOUT_SIGNAL;
}

inline PatternInterpretation InterpretFlip(const DomPatternContext& ctx, bool bidToAsk)
{
    if (!ctx.isValid) return PatternInterpretation::TRAPPED_TRADERS;

    if (ctx.IsAtValueEdge())
    {
        // Flip at edge = trapped traders from failed breakout
        return PatternInterpretation::TRAPPED_TRADERS;
    }
    if (ctx.IsOutsideValue())
    {
        // Flip outside value = exhaustion
        return PatternInterpretation::EXHAUSTION;
    }
    if (ctx.IsAtPOC())
    {
        // Flip at POC = rotation, trapped rotators
        return PatternInterpretation::TRAPPED_TRADERS;
    }
    return PatternInterpretation::TRAPPED_TRADERS;
}

// ============================================================================
// PATTERN HIT STRUCTS - Detection results for each pattern type
// ============================================================================

// Spoofing: Large order appears then vanishes
struct SpoofingHit
{
    int tickOffset = 0;           // Which level showed spoof
    bool isBidSide = false;       // Bid or ask spoof
    double peakQuantity = 0.0;    // Maximum quantity observed
    double endQuantity = 0.0;     // Final quantity (near zero)
    int64_t durationMs = 0;       // How long it was visible
    float strength01 = 0.0f;      // Base confidence [0, 1]
    bool valid = false;

    // Context-aware fields (populated when context is provided)
    float contextSignificance = 0.0f;  // Adjusted strength based on location/state
    PatternInterpretation interpretation = PatternInterpretation::NOISE;
    ValueZone valueZone = ValueZone::UNKNOWN;  // SSOT from ValueLocationEngine
    bool hasContext = false;           // True if context fields are populated

    // Apply context to compute significance and interpretation
    void ApplyContext(const DomPatternContext& ctx)
    {
        if (!ctx.isValid) return;
        contextSignificance = ComputeSpoofingSignificance(strength01, ctx);
        interpretation = InterpretSpoofing(ctx, isBidSide);
        valueZone = ctx.valueZone;
        hasContext = true;
    }

    // Get effective strength (context-adjusted if available)
    float GetEffectiveStrength() const
    {
        return hasContext ? contextSignificance : strength01;
    }

    bool operator<(const SpoofingHit& other) const
    {
        return GetEffectiveStrength() > other.GetEffectiveStrength();
    }
};

// Iceberg: Level keeps refilling (hidden liquidity)
struct IcebergHit
{
    int tickOffset = 0;           // Which level shows iceberg
    bool isBidSide = false;
    double avgQuantity = 0.0;     // Average maintained quantity
    int refillCount = 0;          // Number of refills observed
    double depletionDepth = 0.0;  // How low before refill
    float strength01 = 0.0f;      // Base confidence [0, 1]
    bool valid = false;

    // Context-aware fields
    float contextSignificance = 0.0f;
    PatternInterpretation interpretation = PatternInterpretation::ACCUMULATION;
    ValueZone valueZone = ValueZone::UNKNOWN;  // SSOT from ValueLocationEngine
    bool hasContext = false;

    void ApplyContext(const DomPatternContext& ctx)
    {
        if (!ctx.isValid) return;
        contextSignificance = ComputeIcebergSignificance(strength01, ctx);
        interpretation = InterpretIceberg(ctx, isBidSide);
        valueZone = ctx.valueZone;
        hasContext = true;
    }

    float GetEffectiveStrength() const
    {
        return hasContext ? contextSignificance : strength01;
    }

    bool operator<(const IcebergHit& other) const
    {
        return GetEffectiveStrength() > other.GetEffectiveStrength();
    }
};

// Wall Break: Large resting order gets absorbed
struct WallBreakHit
{
    int tickOffset = 0;
    bool isBidSide = false;
    double startQuantity = 0.0;   // Quantity at window start
    double endQuantity = 0.0;     // Quantity at window end
    double absorptionRate = 0.0;  // Quantity consumed per bar
    float strength01 = 0.0f;      // Base confidence [0, 1]
    bool valid = false;

    // Context-aware fields
    float contextSignificance = 0.0f;
    PatternInterpretation interpretation = PatternInterpretation::BREAKOUT_SIGNAL;
    ValueZone valueZone = ValueZone::UNKNOWN;  // SSOT from ValueLocationEngine
    bool hasContext = false;

    void ApplyContext(const DomPatternContext& ctx)
    {
        if (!ctx.isValid) return;
        contextSignificance = ComputeWallBreakSignificance(strength01, ctx);
        interpretation = InterpretWallBreak(ctx, isBidSide);
        valueZone = ctx.valueZone;
        hasContext = true;
    }

    float GetEffectiveStrength() const
    {
        return hasContext ? contextSignificance : strength01;
    }

    bool operator<(const WallBreakHit& other) const
    {
        return GetEffectiveStrength() > other.GetEffectiveStrength();
    }
};

// Flip: Bid wall becomes ask wall (or vice versa)
struct FlipHit
{
    double priceLevel = 0.0;      // Price where flip occurred
    int tickOffset = 0;           // Offset from current reference
    double bidQuantityBefore = 0.0;
    double askQuantityAfter = 0.0;
    bool bidToAsk = true;         // True = was bid wall, now ask wall
    float strength01 = 0.0f;      // Base confidence [0, 1]
    bool valid = false;

    // Context-aware fields
    float contextSignificance = 0.0f;
    PatternInterpretation interpretation = PatternInterpretation::TRAPPED_TRADERS;
    ValueZone valueZone = ValueZone::UNKNOWN;  // SSOT from ValueLocationEngine
    bool hasContext = false;

    void ApplyContext(const DomPatternContext& ctx)
    {
        if (!ctx.isValid) return;
        contextSignificance = ComputeFlipSignificance(strength01, ctx);
        interpretation = InterpretFlip(ctx, bidToAsk);
        valueZone = ctx.valueZone;
        hasContext = true;
    }

    float GetEffectiveStrength() const
    {
        return hasContext ? contextSignificance : strength01;
    }

    bool operator<(const FlipHit& other) const
    {
        return GetEffectiveStrength() > other.GetEffectiveStrength();
    }
};

// ============================================================================
// SPATIAL DOM PATTERN RESULT - Combined detection results
// ============================================================================

struct SpatialDomPatternResult
{
    std::vector<SpoofingHit> spoofingHits;
    std::vector<IcebergHit> icebergHits;
    std::vector<WallBreakHit> wallBreakHits;
    std::vector<FlipHit> flipHits;

    int windowMs = 0;
    bool wasEligible = false;
    const char* ineligibleReason = nullptr;

    // Context that was applied (for logging/diagnostics)
    DomPatternContext appliedContext;
    bool hasContext = false;

    bool HasPatterns() const
    {
        return !spoofingHits.empty() || !icebergHits.empty() ||
               !wallBreakHits.empty() || !flipHits.empty();
    }

    bool HasSpoofing() const { return !spoofingHits.empty(); }
    bool HasIceberg() const { return !icebergHits.empty(); }
    bool HasWallBreak() const { return !wallBreakHits.empty(); }
    bool HasFlip() const { return !flipHits.empty(); }

    int TotalPatternCount() const
    {
        return static_cast<int>(spoofingHits.size() + icebergHits.size() +
                                wallBreakHits.size() + flipHits.size());
    }

    // Apply context to all detected patterns
    void ApplyContext(const DomPatternContext& ctx)
    {
        if (!ctx.isValid) return;

        for (auto& hit : spoofingHits)
            hit.ApplyContext(ctx);
        for (auto& hit : icebergHits)
            hit.ApplyContext(ctx);
        for (auto& hit : wallBreakHits)
            hit.ApplyContext(ctx);
        for (auto& hit : flipHits)
            hit.ApplyContext(ctx);

        appliedContext = ctx;
        hasContext = true;

        // Re-sort by effective strength (context-adjusted)
        std::sort(spoofingHits.begin(), spoofingHits.end());
        std::sort(icebergHits.begin(), icebergHits.end());
        std::sort(wallBreakHits.begin(), wallBreakHits.end());
        std::sort(flipHits.begin(), flipHits.end());
    }

    // Check if any pattern has high significance (> threshold) after context
    bool HasHighSignificancePatterns(float threshold = 0.7f) const
    {
        for (const auto& h : spoofingHits)
            if (h.GetEffectiveStrength() >= threshold) return true;
        for (const auto& h : icebergHits)
            if (h.GetEffectiveStrength() >= threshold) return true;
        for (const auto& h : wallBreakHits)
            if (h.GetEffectiveStrength() >= threshold) return true;
        for (const auto& h : flipHits)
            if (h.GetEffectiveStrength() >= threshold) return true;
        return false;
    }

    // Get the most significant pattern across all types
    float GetMaxSignificance() const
    {
        float maxSig = 0.0f;
        for (const auto& h : spoofingHits)
            maxSig = (std::max)(maxSig, h.GetEffectiveStrength());
        for (const auto& h : icebergHits)
            maxSig = (std::max)(maxSig, h.GetEffectiveStrength());
        for (const auto& h : wallBreakHits)
            maxSig = (std::max)(maxSig, h.GetEffectiveStrength());
        for (const auto& h : flipHits)
            maxSig = (std::max)(maxSig, h.GetEffectiveStrength());
        return maxSig;
    }

    // Get dominant interpretation (most common or most significant)
    PatternInterpretation GetDominantInterpretation() const
    {
        if (!hasContext || !HasPatterns())
            return PatternInterpretation::NOISE;

        // Find highest significance pattern
        float maxSig = 0.0f;
        PatternInterpretation dom = PatternInterpretation::NOISE;

        for (const auto& h : spoofingHits)
            if (h.GetEffectiveStrength() > maxSig) { maxSig = h.GetEffectiveStrength(); dom = h.interpretation; }
        for (const auto& h : icebergHits)
            if (h.GetEffectiveStrength() > maxSig) { maxSig = h.GetEffectiveStrength(); dom = h.interpretation; }
        for (const auto& h : wallBreakHits)
            if (h.GetEffectiveStrength() > maxSig) { maxSig = h.GetEffectiveStrength(); dom = h.interpretation; }
        for (const auto& h : flipHits)
            if (h.GetEffectiveStrength() > maxSig) { maxSig = h.GetEffectiveStrength(); dom = h.interpretation; }

        return dom;
    }
};

// ============================================================================
// PATTERN DETECTION FUNCTIONS
// ============================================================================

// Spoofing Detection: Large order appears then vanishes before price reaches it
inline std::vector<SpoofingHit> DetectSpoofing(
    const std::vector<SpatialDomSnapshot>& window,
    double quantityP80)  // P80 threshold for "large"
{
    std::vector<SpoofingHit> hits;
    if (window.size() < static_cast<size_t>(SpatialDomConfig::MIN_SAMPLES))
        return hits;

    // Track peak quantity and timing for each level
    std::array<double, SpatialDomConfig::TOTAL_LEVELS> peakQty = {};
    std::array<int64_t, SpatialDomConfig::TOTAL_LEVELS> peakTime = {};
    std::array<int64_t, SpatialDomConfig::TOTAL_LEVELS> appearTime = {};

    // First pass: find peaks and appearance times
    for (const auto& snap : window)
    {
        for (int i = 0; i < SpatialDomConfig::TOTAL_LEVELS; ++i)
        {
            const auto& level = snap.levels[i];
            if (!level.isValid) continue;

            // Track first appearance
            if (level.quantity > 0 && appearTime[i] == 0)
                appearTime[i] = snap.timestampMs;

            // Track peak
            if (level.quantity > peakQty[i])
            {
                peakQty[i] = level.quantity;
                peakTime[i] = snap.timestampMs;
            }
        }
    }

    // Check last snapshot for vanishing
    const auto& last = window.back();
    const int64_t lastTime = last.timestampMs;

    for (int i = 0; i < SpatialDomConfig::TOTAL_LEVELS; ++i)
    {
        const double peak = peakQty[i];
        const double current = last.levels[i].isValid ? last.levels[i].quantity : 0.0;
        const int64_t duration = (peakTime[i] > appearTime[i]) ? (peakTime[i] - appearTime[i]) : 0;
        const int64_t sinceDisappear = lastTime - peakTime[i];

        // Check spoofing criteria
        const bool wasLarge = (peak >= quantityP80 && peak > 0);
        const bool disappeared = (current < peak * SpatialDomConfig::SPOOF_DISAPPEAR_RATIO);
        const bool visibleLongEnough = (duration >= SpatialDomConfig::SPOOF_MIN_APPEAR_MS);
        const bool vanishedQuickly = (sinceDisappear <= SpatialDomConfig::SPOOF_MAX_DISAPPEAR_MS);

        if (wasLarge && disappeared && visibleLongEnough && vanishedQuickly)
        {
            SpoofingHit hit;
            hit.tickOffset = last.levels[i].tickOffset;
            hit.isBidSide = last.levels[i].isBid;
            hit.peakQuantity = peak;
            hit.endQuantity = current;
            hit.durationMs = duration;
            hit.strength01 = (peak > 0) ? static_cast<float>((peak - current) / peak) : 0.0f;
            hit.valid = true;
            hits.push_back(hit);
        }
    }

    // Sort by strength
    std::sort(hits.begin(), hits.end());
    return hits;
}

// Iceberg Detection: Level keeps refilling (hidden liquidity)
inline std::vector<IcebergHit> DetectIceberg(
    const std::vector<SpatialDomSnapshot>& window)
{
    std::vector<IcebergHit> hits;
    if (window.size() < static_cast<size_t>(SpatialDomConfig::MIN_SAMPLES))
        return hits;

    // For each level, track refill cycles
    for (int levelIdx = 0; levelIdx < SpatialDomConfig::TOTAL_LEVELS; ++levelIdx)
    {
        double sumQty = 0.0;
        double peakQty = 0.0;
        int refillCount = 0;
        bool inDepletion = false;
        int sampleCount = 0;

        for (const auto& snap : window)
        {
            const auto& level = snap.levels[levelIdx];
            if (!level.isValid) continue;

            sumQty += level.quantity;
            sampleCount++;
            peakQty = (std::max)(peakQty, level.quantity);

            if (peakQty > 0)
            {
                const double depleteThresh = peakQty * SpatialDomConfig::ICEBERG_DEPLETE_RATIO;
                const double refillThresh = peakQty * SpatialDomConfig::ICEBERG_REFILL_RATIO;

                if (!inDepletion && level.quantity < depleteThresh)
                {
                    inDepletion = true;
                }
                else if (inDepletion && level.quantity > refillThresh)
                {
                    inDepletion = false;
                    refillCount++;
                }
            }
        }

        if (refillCount >= SpatialDomConfig::ICEBERG_MIN_REFILLS && peakQty > 0 && sampleCount > 0)
        {
            const auto& lastLevel = window.back().levels[levelIdx];
            IcebergHit hit;
            hit.tickOffset = lastLevel.tickOffset;
            hit.isBidSide = lastLevel.isBid;
            hit.avgQuantity = sumQty / sampleCount;
            hit.refillCount = refillCount;
            hit.depletionDepth = peakQty * SpatialDomConfig::ICEBERG_DEPLETE_RATIO;
            hit.strength01 = (std::min)(1.0f, static_cast<float>(refillCount) / 5.0f);
            hit.valid = true;
            hits.push_back(hit);
        }
    }

    std::sort(hits.begin(), hits.end());
    return hits;
}

// Wall Break Detection: Large resting order gets progressively consumed
inline std::vector<WallBreakHit> DetectWallBreaking(
    const std::vector<SpatialDomSnapshot>& window,
    double quantityP90)  // P90 threshold for "wall"
{
    std::vector<WallBreakHit> hits;
    if (window.size() < 2) return hits;

    const auto& first = window.front();
    const auto& last = window.back();

    // Check sufficient bar span
    if (last.barIndex - first.barIndex < SpatialDomConfig::WALL_BREAK_MIN_BARS)
        return hits;

    for (int i = 0; i < SpatialDomConfig::TOTAL_LEVELS; ++i)
    {
        const double startQty = first.levels[i].isValid ? first.levels[i].quantity : 0.0;
        const double endQty = last.levels[i].isValid ? last.levels[i].quantity : 0.0;

        // Was it a wall at start?
        const bool wasWall = (startQty >= quantityP90 && startQty > 0);
        // Is it broken now?
        const bool isBroken = (endQty < startQty * SpatialDomConfig::WALL_BREAK_RATIO);

        if (wasWall && isBroken)
        {
            WallBreakHit hit;
            hit.tickOffset = last.levels[i].tickOffset;
            hit.isBidSide = last.levels[i].isBid;
            hit.startQuantity = startQty;
            hit.endQuantity = endQty;
            const int barSpan = last.barIndex - first.barIndex;
            hit.absorptionRate = (barSpan > 0) ? (startQty - endQty) / barSpan : 0.0;
            hit.strength01 = (startQty > 0) ? static_cast<float>((startQty - endQty) / startQty) : 0.0f;
            hit.valid = true;
            hits.push_back(hit);
        }
    }

    std::sort(hits.begin(), hits.end());
    return hits;
}

// Flip Detection: Bid wall becomes ask wall at same price (or vice versa)
inline std::vector<FlipHit> DetectFlip(
    const std::vector<SpatialDomSnapshot>& window,
    double currentPrice,
    double tickSize)
{
    std::vector<FlipHit> hits;
    if (window.size() < 3 || tickSize <= 0) return hits;

    const auto& first = window.front();
    const auto& last = window.back();

    // Check if price moved (required for flip)
    const double priceMoveTicks = (last.referencePrice - first.referencePrice) / tickSize;
    if (std::abs(priceMoveTicks) < 1.0) return hits;

    // Look for significant bid levels in first snapshot
    for (int i = 0; i < SpatialDomConfig::LEVELS_PER_SIDE; ++i)
    {
        const auto& bidLevel = first.levels[i];
        if (!bidLevel.isValid || bidLevel.quantity < SpatialDomConfig::FLIP_MIN_QUANTITY)
            continue;

        // Calculate where this price is now relative to current reference
        const double levelPrice = first.referencePrice + bidLevel.tickOffset * tickSize;
        const int newOffset = static_cast<int>((levelPrice - last.referencePrice) / tickSize);

        // If price crossed this level (bid level is now on ask side)
        if (newOffset > 0 && newOffset <= SpatialDomConfig::LEVELS_PER_SIDE)
        {
            const auto& askNow = last.GetAskByIndex(newOffset - 1);
            if (askNow.isValid && askNow.quantity >= SpatialDomConfig::FLIP_MIN_QUANTITY)
            {
                // Significant quantity now on ask side at same level
                FlipHit hit;
                hit.priceLevel = levelPrice;
                hit.tickOffset = newOffset;
                hit.bidQuantityBefore = bidLevel.quantity;
                hit.askQuantityAfter = askNow.quantity;
                hit.bidToAsk = true;
                const double minQty = (std::min)(hit.askQuantityAfter, hit.bidQuantityBefore);
                const double maxQty = (std::max)(hit.askQuantityAfter, hit.bidQuantityBefore);
                hit.strength01 = (maxQty > 0) ? static_cast<float>(minQty / maxQty) : 0.0f;
                hit.valid = true;
                hits.push_back(hit);
            }
        }
    }

    // Also check ask-to-bid flips (price moved down)
    for (int i = 0; i < SpatialDomConfig::LEVELS_PER_SIDE; ++i)
    {
        const auto& askLevel = first.GetAskByIndex(i);
        if (!askLevel.isValid || askLevel.quantity < SpatialDomConfig::FLIP_MIN_QUANTITY)
            continue;

        const double levelPrice = first.referencePrice + askLevel.tickOffset * tickSize;
        const int newOffset = static_cast<int>((levelPrice - last.referencePrice) / tickSize);

        // If price crossed this level (ask level is now on bid side)
        if (newOffset < 0 && -newOffset <= SpatialDomConfig::LEVELS_PER_SIDE)
        {
            const auto& bidNow = last.GetBidByIndex(-newOffset - 1);
            if (bidNow.isValid && bidNow.quantity >= SpatialDomConfig::FLIP_MIN_QUANTITY)
            {
                FlipHit hit;
                hit.priceLevel = levelPrice;
                hit.tickOffset = newOffset;
                hit.bidQuantityBefore = bidNow.quantity;
                hit.askQuantityAfter = askLevel.quantity;
                hit.bidToAsk = false;  // Ask to bid flip
                const double minQty = (std::min)(hit.askQuantityAfter, hit.bidQuantityBefore);
                const double maxQty = (std::max)(hit.askQuantityAfter, hit.bidQuantityBefore);
                hit.strength01 = (maxQty > 0) ? static_cast<float>(minQty / maxQty) : 0.0f;
                hit.valid = true;
                hits.push_back(hit);
            }
        }
    }

    std::sort(hits.begin(), hits.end());
    return hits;
}

// ============================================================================
// COMBINED SPATIAL DOM PATTERN DETECTION
// ============================================================================

inline SpatialDomPatternResult DetectSpatialDomPatterns(
    const SpatialDomHistoryBuffer& buffer,
    double quantityP80,           // For spoofing: "large" threshold
    double quantityP90,           // For wall break: "wall" threshold
    double currentPrice,
    double tickSize,
    int windowMs = SpatialDomConfig::DEFAULT_WINDOW_MS)
{
    SpatialDomPatternResult result;
    result.windowMs = windowMs;

    auto window = buffer.GetWindow(windowMs);
    if (window.size() < static_cast<size_t>(SpatialDomConfig::MIN_SAMPLES))
    {
        result.wasEligible = false;
        result.ineligibleReason = "INSUFFICIENT_SPATIAL_SAMPLES";
        return result;
    }
    result.wasEligible = true;

    result.spoofingHits = DetectSpoofing(window, quantityP80);
    result.icebergHits = DetectIceberg(window);
    result.wallBreakHits = DetectWallBreaking(window, quantityP90);
    result.flipHits = DetectFlip(window, currentPrice, tickSize);

    return result;
}

// Context-aware overload - applies auction context to detected patterns
// This changes interpretation and significance based on WHERE in the auction the pattern occurs
inline SpatialDomPatternResult DetectSpatialDomPatterns(
    const SpatialDomHistoryBuffer& buffer,
    double quantityP80,
    double quantityP90,
    double currentPrice,
    double tickSize,
    const DomPatternContext& ctx,    // Auction context for significance adjustment
    int windowMs = SpatialDomConfig::DEFAULT_WINDOW_MS)
{
    // Call base detection
    SpatialDomPatternResult result = DetectSpatialDomPatterns(
        buffer, quantityP80, quantityP90, currentPrice, tickSize, windowMs);

    // Apply auction context to all hits if context is valid
    if (ctx.isValid)
    {
        result.ApplyContext(ctx);
    }

    return result;
}

// ============================================================================
// SPATIAL DOM PATTERN LOG STATE - Throttling for log output
// ============================================================================

struct SpatialDomPatternLogState
{
    int lastSpoofLogBar = -1;
    int lastIcebergLogBar = -1;
    int lastWallBreakLogBar = -1;
    int lastFlipLogBar = -1;

    void Reset()
    {
        lastSpoofLogBar = -1;
        lastIcebergLogBar = -1;
        lastWallBreakLogBar = -1;
        lastFlipLogBar = -1;
    }

    bool ShouldLogSpoofing(int currentBar) const
    {
        return (currentBar - lastSpoofLogBar >= SpatialDomConfig::LOG_THROTTLE_BARS);
    }

    bool ShouldLogIceberg(int currentBar) const
    {
        return (currentBar - lastIcebergLogBar >= SpatialDomConfig::LOG_THROTTLE_BARS);
    }

    bool ShouldLogWallBreak(int currentBar) const
    {
        return (currentBar - lastWallBreakLogBar >= SpatialDomConfig::LOG_THROTTLE_BARS);
    }

    bool ShouldLogFlip(int currentBar) const
    {
        return (currentBar - lastFlipLogBar >= SpatialDomConfig::LOG_THROTTLE_BARS);
    }
};

// ============================================================================
// STRING CONVERSION FOR LOGGING
// ============================================================================

// NOTE: For ValueZone logging, use ValueZoneToString() from AMT_ValueLocation.h

// Convert DomMarketState to short string for logging
inline const char* DomMarketStateToString(DomMarketState state)
{
    switch (state)
    {
        case DomMarketState::UNKNOWN:   return "UNK";
        case DomMarketState::BALANCE:   return "BAL";
        case DomMarketState::IMBALANCE: return "IMB";
        default:                        return "?";
    }
}

// Convert PatternInterpretation to short string for logging
inline const char* PatternInterpretationToString(PatternInterpretation interp)
{
    switch (interp)
    {
        case PatternInterpretation::NOISE:           return "NOISE";
        case PatternInterpretation::DEFENSIVE:       return "DEFENSIVE";
        case PatternInterpretation::AGGRESSIVE:      return "AGGRESSIVE";
        case PatternInterpretation::EXHAUSTION:      return "EXHAUSTION";
        case PatternInterpretation::ACCUMULATION:    return "ACCUMULATION";
        case PatternInterpretation::BREAKOUT_SIGNAL: return "BREAKOUT";
        case PatternInterpretation::REJECTION_SIGNAL:return "REJECTION";
        case PatternInterpretation::TRAPPED_TRADERS: return "TRAPPED";
        default:                                     return "?";
    }
}

inline std::string FormatSpatialPatternResult(const SpatialDomPatternResult& result, int barIndex)
{
    std::string msg = "[SPATIAL-DOM] Bar ";
    msg += std::to_string(barIndex);
    msg += " | SPOOF=";
    msg += std::to_string(result.spoofingHits.size());
    msg += " ICE=";
    msg += std::to_string(result.icebergHits.size());
    msg += " WALL=";
    msg += std::to_string(result.wallBreakHits.size());
    msg += " FLIP=";
    msg += std::to_string(result.flipHits.size());
    msg += " | eligible=";
    msg += result.wasEligible ? "true" : "false";
    return msg;
}

inline std::string FormatSpoofingHit(const SpoofingHit& hit, int barIndex)
{
    std::string msg = "[SPATIAL-SPOOF] Bar ";
    msg += std::to_string(barIndex);
    msg += " | level=";
    msg += std::to_string(hit.tickOffset);
    msg += "t qty=";
    msg += std::to_string(static_cast<int>(hit.peakQuantity));
    msg += "->";
    msg += std::to_string(static_cast<int>(hit.endQuantity));
    msg += " dur=";
    msg += std::to_string(hit.durationMs);
    msg += "ms side=";
    msg += hit.isBidSide ? "BID" : "ASK";
    msg += " str=";
    msg += std::to_string(static_cast<int>(hit.strength01 * 100));
    msg += "%";
    return msg;
}

inline std::string FormatIcebergHit(const IcebergHit& hit, int barIndex)
{
    std::string msg = "[SPATIAL-ICE] Bar ";
    msg += std::to_string(barIndex);
    msg += " | level=";
    msg += std::to_string(hit.tickOffset);
    msg += "t avg=";
    msg += std::to_string(static_cast<int>(hit.avgQuantity));
    msg += " refills=";
    msg += std::to_string(hit.refillCount);
    msg += " side=";
    msg += hit.isBidSide ? "BID" : "ASK";
    msg += " str=";
    msg += std::to_string(static_cast<int>(hit.strength01 * 100));
    msg += "%";
    return msg;
}

inline std::string FormatWallBreakHit(const WallBreakHit& hit, int barIndex)
{
    std::string msg = "[SPATIAL-WALL] Bar ";
    msg += std::to_string(barIndex);
    msg += " | level=";
    msg += std::to_string(hit.tickOffset);
    msg += "t qty=";
    msg += std::to_string(static_cast<int>(hit.startQuantity));
    msg += "->";
    msg += std::to_string(static_cast<int>(hit.endQuantity));
    msg += " rate=";
    msg += std::to_string(static_cast<int>(hit.absorptionRate));
    msg += "/bar side=";
    msg += hit.isBidSide ? "BID" : "ASK";
    msg += " str=";
    msg += std::to_string(static_cast<int>(hit.strength01 * 100));
    msg += "%";
    return msg;
}

inline std::string FormatFlipHit(const FlipHit& hit, int barIndex)
{
    std::string msg = "[SPATIAL-FLIP] Bar ";
    msg += std::to_string(barIndex);
    msg += " | price=";
    msg += std::to_string(hit.priceLevel);
    msg += " ";
    msg += hit.bidToAsk ? "BID->ASK" : "ASK->BID";
    msg += " qty=";
    msg += std::to_string(static_cast<int>(hit.bidQuantityBefore));
    msg += "->";
    msg += std::to_string(static_cast<int>(hit.askQuantityAfter));
    msg += " str=";
    msg += std::to_string(static_cast<int>(hit.strength01 * 100));
    msg += "%";
    return msg;
}

} // namespace AMT

#endif // AMT_DOM_EVENTS_H
