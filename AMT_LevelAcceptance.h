// ============================================================================
// AMT_LevelAcceptance.h
// Unified Acceptance/Rejection Framework for All Significant Price Levels
// ============================================================================
//
// CORE PRINCIPLE (Auction Market Theory):
// Every significant price level is a hypothesis that price tests.
// - When price finds responsive activity → REJECTION
// - When price finds no resistance → ACCEPTANCE (and continues)
//
// LEVEL BEHAVIOR EXPECTATIONS:
// - HVN: Should attract and hold (acceptance expected)
//        Unexpected rejection = momentum through, significant
// - LVN: Should repel (rejection expected)
//        Unexpected acceptance = TREND SIGNAL (building value in "unfair" area)
// - VAH/VAL: Boundary tests (either outcome is significant)
// - POC: Ultimate fair value (should attract)
// - Session extremes: Probes (rejection more common)
// - IB levels: Range definition (break = range extension day)
//
// ACTIONABLE SIGNALS:
// 1. LVN acceptance = strongest trend signal (price building value where it shouldn't)
// 2. HVN rejection = momentum, unusual, significant
// 3. VAH/VAL resolution = direction of next move
// 4. IB break with acceptance = range extension day
//
// ============================================================================
#pragma once

#include "amt_core.h"
#include <vector>
#include <unordered_map>
#include <cmath>
#include <algorithm>

namespace AMT {

// ============================================================================
// LEVEL TEST (Individual Level State)
// ============================================================================

struct LevelTest {
    LevelType type = LevelType::UNKNOWN;
    double price = 0.0;
    LevelTestOutcome outcome = LevelTestOutcome::UNTESTED;

    // ========================================================================
    // ACCEPTANCE SIGNALS (Common for all level types)
    // ========================================================================
    int barsAtLevel = 0;            // Time (TPO count) at this level
    double volumeAtLevel = 0.0;     // Cumulative volume traded at level
    double deltaAtLevel = 0.0;      // Cumulative delta at level
    double avgCloseStrength = 0.0;  // Average close strength (0=weak tail, 1=strong close)

    // Test tracking
    int testCount = 0;              // Number of times level has been tested
    int testBar = 0;                // Bar when current test started
    int resolutionBar = 0;          // Bar when outcome was determined
    double entryPrice = 0.0;        // Price when test began
    double maxExcursion = 0.0;      // Max distance from level during test

    // Retest tracking
    int retestCount = 0;            // Number of retests after initial test
    bool lastRetestHeld = false;    // Did last retest hold?

    // ========================================================================
    // COMPUTED SCORES
    // ========================================================================
    double acceptanceScore = 0.0;   // -1.0 (strong rejection) to +1.0 (strong acceptance)
    bool isActionable = false;      // Is this an actionable trading signal?

    // ========================================================================
    // METHODS
    // ========================================================================

    void Reset() {
        outcome = LevelTestOutcome::UNTESTED;
        barsAtLevel = 0;
        volumeAtLevel = 0.0;
        deltaAtLevel = 0.0;
        avgCloseStrength = 0.0;
        testCount = 0;
        testBar = 0;
        resolutionBar = 0;
        entryPrice = 0.0;
        maxExcursion = 0.0;
        retestCount = 0;
        lastRetestHeld = false;
        acceptanceScore = 0.0;
        isActionable = false;
    }

    void StartTest(int bar, double currentPrice) {
        if (outcome != LevelTestOutcome::TESTING) {
            testCount++;
            testBar = bar;
            entryPrice = currentPrice;
            maxExcursion = 0.0;
        }
        outcome = LevelTestOutcome::TESTING;
    }

    void UpdateTest(double currentPrice, double barVolume, double barDelta, double closeStrength) {
        if (outcome != LevelTestOutcome::TESTING) return;

        barsAtLevel++;
        volumeAtLevel += barVolume;
        deltaAtLevel += barDelta;

        // Running average of close strength
        const double weight = 1.0 / barsAtLevel;
        avgCloseStrength = avgCloseStrength * (1.0 - weight) + closeStrength * weight;

        // Track max excursion from level
        const double excursion = std::abs(currentPrice - price);
        maxExcursion = (std::max)(maxExcursion, excursion);
    }

    void ResolveTest(int bar, LevelTestOutcome result) {
        outcome = result;
        resolutionBar = bar;
        isActionable = AMT::IsActionableSignal(type, outcome);
    }

    bool IsExpected() const {
        return AMT::IsExpectedOutcome(type, outcome);
    }

    bool IsResolved() const {
        return outcome == LevelTestOutcome::ACCEPTED ||
               outcome == LevelTestOutcome::REJECTED ||
               outcome == LevelTestOutcome::BROKEN_THROUGH;
    }

    /**
     * Get trading implication based on level type and outcome.
     * Returns: 1 = bullish, -1 = bearish, 0 = neutral
     */
    int GetDirectionalImplication() const {
        if (!IsResolved()) return 0;

        switch (type) {
            case LevelType::VAH:
            case LevelType::PRIOR_VAH:
            case LevelType::DEVELOPING_VAH:
            case LevelType::IB_HIGH:
            case LevelType::SESSION_HIGH:
                // Upper levels: acceptance = bullish, rejection = bearish
                if (outcome == LevelTestOutcome::ACCEPTED ||
                    outcome == LevelTestOutcome::BROKEN_THROUGH) {
                    return 1;  // Bullish - breaking higher
                }
                return -1;  // Bearish - rejected at high

            case LevelType::VAL:
            case LevelType::PRIOR_VAL:
            case LevelType::DEVELOPING_VAL:
            case LevelType::IB_LOW:
            case LevelType::SESSION_LOW:
                // Lower levels: acceptance = bearish, rejection = bullish
                if (outcome == LevelTestOutcome::ACCEPTED ||
                    outcome == LevelTestOutcome::BROKEN_THROUGH) {
                    return -1;  // Bearish - breaking lower
                }
                return 1;  // Bullish - rejected at low

            case LevelType::LVN:
                // LVN acceptance is the STRONGEST trend signal
                if (outcome == LevelTestOutcome::ACCEPTED) {
                    // Determine direction from delta
                    return (deltaAtLevel > 0) ? 1 : -1;
                }
                return 0;  // Rejection at LVN is expected, neutral

            case LevelType::HVN:
                // HVN rejection is unusual - momentum signal
                if (outcome == LevelTestOutcome::REJECTED ||
                    outcome == LevelTestOutcome::BROKEN_THROUGH) {
                    return (deltaAtLevel > 0) ? 1 : -1;
                }
                return 0;  // Acceptance at HVN is expected, neutral

            default:
                return 0;
        }
    }
};

// ============================================================================
// LEVEL ACCEPTANCE CONFIG
// ============================================================================

struct LevelAcceptanceConfig {
    // Proximity thresholds (in ticks)
    int proximityTicks = 4;           // How close to level to be "at" it
    int departureTicks = 8;           // How far to depart to resolve test

    // Time thresholds (in bars)
    int minBarsForAcceptance = 3;     // Minimum TPOs to consider accepted
    int minBarsForRejection = 1;      // Minimum TPOs before rejection possible

    // Volume thresholds
    double volumeAcceptanceRatio = 1.5;  // Volume vs baseline for acceptance
    double volumeRejectionRatio = 0.5;   // Volume vs baseline for rejection

    // Close strength thresholds
    double strongCloseThreshold = 0.7;   // Close strength for acceptance
    double weakCloseThreshold = 0.3;     // Close strength for rejection

    // Delta thresholds
    double deltaConfirmThreshold = 0.6;  // Delta consistency for direction

    // Retest configuration
    int retestDepartureBars = 2;         // Bars away before return = retest
};

// ============================================================================
// LEVEL ACCEPTANCE ENGINE
// ============================================================================

class LevelAcceptanceEngine {
public:
    // ========================================================================
    // CONFIGURATION
    // ========================================================================
    LevelAcceptanceConfig config;

private:
    // Level state storage (keyed by LevelType)
    std::unordered_map<int, LevelTest> levels_;

    // Baseline for volume comparison
    double volumeBaseline_ = 0.0;
    int volumeBaselineSamples_ = 0;

    // Current bar tracking
    int currentBar_ = 0;
    double currentPrice_ = 0.0;
    double tickSize_ = 0.25;

public:
    // ========================================================================
    // INITIALIZATION
    // ========================================================================

    void Reset() {
        levels_.clear();
        volumeBaseline_ = 0.0;
        volumeBaselineSamples_ = 0;
        currentBar_ = 0;
        currentPrice_ = 0.0;
    }

    void SetTickSize(double ts) {
        tickSize_ = ts;
    }

    // ========================================================================
    // LEVEL REGISTRATION
    // ========================================================================

    /**
     * Register or update a level for tracking.
     */
    void RegisterLevel(LevelType type, double price) {
        const int key = static_cast<int>(type);
        auto it = levels_.find(key);

        if (it == levels_.end()) {
            // New level
            LevelTest test;
            test.type = type;
            test.price = price;
            levels_[key] = test;
        } else {
            // Update price if significantly different
            const double drift = std::abs(price - it->second.price);
            if (drift > tickSize_ * config.proximityTicks) {
                // Price moved significantly - reset the test
                it->second.Reset();
                it->second.type = type;
                it->second.price = price;
            } else {
                // Minor drift - just update price
                it->second.price = price;
            }
        }
    }

    /**
     * Register multiple HVN/LVN levels.
     */
    void RegisterHVNs(const std::vector<double>& prices) {
        // For now, track the nearest HVN only (could expand to track multiple)
        if (!prices.empty()) {
            double nearest = prices[0];
            double nearestDist = std::abs(prices[0] - currentPrice_);
            for (size_t i = 1; i < prices.size(); ++i) {
                const double dist = std::abs(prices[i] - currentPrice_);
                if (dist < nearestDist) {
                    nearest = prices[i];
                    nearestDist = dist;
                }
            }
            RegisterLevel(LevelType::HVN, nearest);
        }
    }

    void RegisterLVNs(const std::vector<double>& prices) {
        if (!prices.empty()) {
            double nearest = prices[0];
            double nearestDist = std::abs(prices[0] - currentPrice_);
            for (size_t i = 1; i < prices.size(); ++i) {
                const double dist = std::abs(prices[i] - currentPrice_);
                if (dist < nearestDist) {
                    nearest = prices[i];
                    nearestDist = dist;
                }
            }
            RegisterLevel(LevelType::LVN, nearest);
        }
    }

    // ========================================================================
    // BAR PROCESSING
    // ========================================================================

    /**
     * Process a new bar and update all level tests.
     */
    void ProcessBar(int bar, double high, double low, double close,
                    double volume, double delta, double closeStrength) {
        currentBar_ = bar;
        currentPrice_ = close;

        // Update volume baseline
        UpdateVolumeBaseline(volume);

        // Process each tracked level
        for (auto& pair : levels_) {
            LevelTest& test = pair.second;
            ProcessLevelTest(test, high, low, close, volume, delta, closeStrength);
        }
    }

    // ========================================================================
    // QUERIES
    // ========================================================================

    /**
     * Get the test state for a specific level type.
     */
    const LevelTest* GetLevel(LevelType type) const {
        const int key = static_cast<int>(type);
        auto it = levels_.find(key);
        return (it != levels_.end()) ? &it->second : nullptr;
    }

    LevelTest* GetLevelMutable(LevelType type) {
        const int key = static_cast<int>(type);
        auto it = levels_.find(key);
        return (it != levels_.end()) ? &it->second : nullptr;
    }

    /**
     * Get outcome for a level type.
     */
    LevelTestOutcome GetOutcome(LevelType type) const {
        const LevelTest* test = GetLevel(type);
        return test ? test->outcome : LevelTestOutcome::UNTESTED;
    }

    /**
     * Get all actionable signals (resolved tests with trading implications).
     */
    std::vector<const LevelTest*> GetActionableSignals() const {
        std::vector<const LevelTest*> signals;
        for (const auto& pair : levels_) {
            if (pair.second.isActionable) {
                signals.push_back(&pair.second);
            }
        }
        return signals;
    }

    /**
     * Get the strongest directional signal.
     * Returns: 1 = bullish, -1 = bearish, 0 = no signal
     */
    int GetNetDirectionalSignal() const {
        int bullish = 0;
        int bearish = 0;

        for (const auto& pair : levels_) {
            const LevelTest& test = pair.second;
            if (!test.isActionable) continue;

            const int dir = test.GetDirectionalImplication();
            if (dir > 0) bullish++;
            else if (dir < 0) bearish++;
        }

        if (bullish > bearish) return 1;
        if (bearish > bullish) return -1;
        return 0;
    }

    /**
     * Check if any level is currently being tested.
     */
    bool IsTestingAnyLevel() const {
        for (const auto& pair : levels_) {
            if (pair.second.outcome == LevelTestOutcome::TESTING) {
                return true;
            }
        }
        return false;
    }

    /**
     * Get the level currently being tested (if any).
     */
    const LevelTest* GetActiveTest() const {
        for (const auto& pair : levels_) {
            if (pair.second.outcome == LevelTestOutcome::TESTING) {
                return &pair.second;
            }
        }
        return nullptr;
    }

    /**
     * Check for LVN acceptance (strongest trend signal).
     */
    bool HasLVNAcceptance() const {
        const LevelTest* lvn = GetLevel(LevelType::LVN);
        return lvn && lvn->outcome == LevelTestOutcome::ACCEPTED;
    }

    /**
     * Check for HVN rejection (unusual momentum signal).
     */
    bool HasHVNRejection() const {
        const LevelTest* hvn = GetLevel(LevelType::HVN);
        return hvn && (hvn->outcome == LevelTestOutcome::REJECTED ||
                       hvn->outcome == LevelTestOutcome::BROKEN_THROUGH);
    }

    /**
     * Check for IB break (range extension signal).
     */
    bool HasIBBreak(bool* isBreakUp = nullptr) const {
        const LevelTest* ibHigh = GetLevel(LevelType::IB_HIGH);
        const LevelTest* ibLow = GetLevel(LevelType::IB_LOW);

        const bool breakUp = ibHigh &&
            (ibHigh->outcome == LevelTestOutcome::ACCEPTED ||
             ibHigh->outcome == LevelTestOutcome::BROKEN_THROUGH);

        const bool breakDown = ibLow &&
            (ibLow->outcome == LevelTestOutcome::ACCEPTED ||
             ibLow->outcome == LevelTestOutcome::BROKEN_THROUGH);

        if (isBreakUp) {
            *isBreakUp = breakUp && !breakDown;
        }

        return breakUp || breakDown;
    }

    /**
     * Get VAH/VAL resolution for trading bias.
     */
    int GetVAResolution() const {
        const LevelTest* vah = GetLevel(LevelType::VAH);
        const LevelTest* val = GetLevel(LevelType::VAL);

        int signal = 0;

        if (vah && vah->IsResolved()) {
            signal += vah->GetDirectionalImplication();
        }
        if (val && val->IsResolved()) {
            signal += val->GetDirectionalImplication();
        }

        // Clamp to -1, 0, 1
        return (signal > 0) ? 1 : ((signal < 0) ? -1 : 0);
    }

private:
    // ========================================================================
    // INTERNAL PROCESSING
    // ========================================================================

    void UpdateVolumeBaseline(double volume) {
        // Simple exponential moving average
        if (volumeBaselineSamples_ == 0) {
            volumeBaseline_ = volume;
        } else {
            const double alpha = 0.1;  // Smoothing factor
            volumeBaseline_ = volumeBaseline_ * (1.0 - alpha) + volume * alpha;
        }
        volumeBaselineSamples_++;
    }

    void ProcessLevelTest(LevelTest& test, double high, double low, double close,
                          double volume, double delta, double closeStrength) {
        const double proximityThreshold = config.proximityTicks * tickSize_;
        const double departureThreshold = config.departureTicks * tickSize_;

        const double distToLevel = std::abs(close - test.price);
        const bool priceAtLevel = distToLevel <= proximityThreshold;
        const bool priceTouchedLevel = (low <= test.price + proximityThreshold &&
                                        high >= test.price - proximityThreshold);

        switch (test.outcome) {
            case LevelTestOutcome::UNTESTED:
                // Check if we've reached the level
                if (priceAtLevel || priceTouchedLevel) {
                    test.StartTest(currentBar_, close);
                    test.UpdateTest(close, volume, delta, closeStrength);
                }
                break;

            case LevelTestOutcome::TESTING:
                if (priceAtLevel) {
                    // Still at level - accumulate signals
                    test.UpdateTest(close, volume, delta, closeStrength);

                    // Check for acceptance conditions
                    if (ShouldAccept(test)) {
                        test.ResolveTest(currentBar_, LevelTestOutcome::ACCEPTED);
                        ComputeAcceptanceScore(test);
                    }
                } else if (distToLevel > departureThreshold) {
                    // Departed from level - determine outcome
                    if (ShouldReject(test, close)) {
                        test.ResolveTest(currentBar_, LevelTestOutcome::REJECTED);
                    } else {
                        // Broke through
                        test.ResolveTest(currentBar_, LevelTestOutcome::BROKEN_THROUGH);
                    }
                    ComputeAcceptanceScore(test);
                }
                break;

            case LevelTestOutcome::ACCEPTED:
            case LevelTestOutcome::REJECTED:
            case LevelTestOutcome::BROKEN_THROUGH:
                // Check for retest
                if (priceAtLevel && !test.lastRetestHeld) {
                    test.retestCount++;
                    // Track retest behavior (simplified)
                    test.lastRetestHeld = true;  // Will be updated on departure
                } else if (!priceAtLevel) {
                    test.lastRetestHeld = false;
                }
                break;
        }
    }

    bool ShouldAccept(const LevelTest& test) const {
        // Time requirement
        if (test.barsAtLevel < config.minBarsForAcceptance) {
            return false;
        }

        // Volume requirement (relative to baseline)
        if (volumeBaseline_ > 0) {
            const double volumeRatio = test.volumeAtLevel / (volumeBaseline_ * test.barsAtLevel);
            if (volumeRatio < config.volumeAcceptanceRatio) {
                return false;
            }
        }

        // Close strength requirement
        if (test.avgCloseStrength < config.strongCloseThreshold) {
            return false;
        }

        return true;
    }

    bool ShouldReject(const LevelTest& test, double currentPrice) const {
        // Rejection = returned toward origin (before level)
        // For upper levels: rejection means we went down
        // For lower levels: rejection means we went up

        switch (test.type) {
            case LevelType::VAH:
            case LevelType::PRIOR_VAH:
            case LevelType::DEVELOPING_VAH:
            case LevelType::IB_HIGH:
            case LevelType::SESSION_HIGH:
            case LevelType::PRIOR_HIGH:
                // Upper level - rejection if we're now below
                return currentPrice < test.price;

            case LevelType::VAL:
            case LevelType::PRIOR_VAL:
            case LevelType::DEVELOPING_VAL:
            case LevelType::IB_LOW:
            case LevelType::SESSION_LOW:
            case LevelType::PRIOR_LOW:
                // Lower level - rejection if we're now above
                return currentPrice > test.price;

            case LevelType::HVN:
            case LevelType::LVN:
            case LevelType::POC:
            case LevelType::PRIOR_POC:
            case LevelType::DEVELOPING_POC:
                // Symmetric levels - rejection based on entry direction
                if (test.entryPrice > test.price) {
                    // Entered from above - rejection if we returned above
                    return currentPrice > test.price;
                } else {
                    // Entered from below - rejection if we returned below
                    return currentPrice < test.price;
                }

            default:
                return true;  // Default to rejection
        }
    }

    void ComputeAcceptanceScore(LevelTest& test) {
        // Score from -1 (strong rejection) to +1 (strong acceptance)
        double score = 0.0;

        // Time component (more time = more acceptance)
        const double timeScore = (std::min)(1.0, static_cast<double>(test.barsAtLevel) / 10.0);

        // Volume component
        double volumeScore = 0.0;
        if (volumeBaseline_ > 0 && test.barsAtLevel > 0) {
            const double volumeRatio = test.volumeAtLevel / (volumeBaseline_ * test.barsAtLevel);
            volumeScore = (std::min)(1.0, volumeRatio / 2.0);  // Normalize to 0-1
        }

        // Close strength component
        const double closeScore = test.avgCloseStrength;

        // Combine scores
        score = (timeScore * 0.3 + volumeScore * 0.4 + closeScore * 0.3);

        // Flip sign for rejection
        if (test.outcome == LevelTestOutcome::REJECTED) {
            score = -score;
        }

        test.acceptanceScore = score;
    }
};

// ============================================================================
// HELPER: Get Trading Bias from Level Acceptance
// ============================================================================

/**
 * Derive trading bias from level acceptance engine state.
 * This is a helper that can be integrated into TradingBias derivation.
 */
inline TradingBias GetBiasFromLevelAcceptance(const LevelAcceptanceEngine& engine) {
    // Priority 1: LVN acceptance = strongest trend signal
    if (engine.HasLVNAcceptance()) {
        return TradingBias::FOLLOW;
    }

    // Priority 2: IB break with acceptance
    bool isBreakUp = false;
    if (engine.HasIBBreak(&isBreakUp)) {
        return TradingBias::FOLLOW;
    }

    // Priority 3: VA boundary resolution
    const int vaSignal = engine.GetVAResolution();
    if (vaSignal != 0) {
        // VAH accepted or VAL rejected = bullish (follow up)
        // VAL accepted or VAH rejected = bearish (follow down)
        return TradingBias::FOLLOW;
    }

    // Priority 4: Check for active test (wait for resolution)
    if (engine.IsTestingAnyLevel()) {
        return TradingBias::WAIT;
    }

    // Priority 5: Net directional signal from all levels
    const int netSignal = engine.GetNetDirectionalSignal();
    if (netSignal != 0) {
        return TradingBias::FOLLOW;
    }

    // Default: wait for setup
    return TradingBias::WAIT;
}

}  // namespace AMT
