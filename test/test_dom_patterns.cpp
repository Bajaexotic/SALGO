// ============================================================================
// test_dom_patterns.cpp
// Unit tests for AMT_DomPatterns.h - Static DOM Pattern Detection
// Tests: BalanceDOMPattern, ImbalanceDOMPattern
// ============================================================================

#include "../AMT_DomPatterns.h"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace AMT;

// Test utilities
static int g_testsPassed = 0;
static int g_testsFailed = 0;

#define TEST(name) void test_##name()
#define RUN_TEST(name) do { \
    std::cout << "Running " #name "... "; \
    try { test_##name(); g_testsPassed++; std::cout << "PASSED\n"; } \
    catch (const std::exception& e) { g_testsFailed++; std::cout << "FAILED: " << e.what() << "\n"; } \
    catch (...) { g_testsFailed++; std::cout << "FAILED: Unknown exception\n"; } \
} while(0)

#define ASSERT_TRUE(cond) do { if (!(cond)) throw std::runtime_error("Assertion failed: " #cond); } while(0)
#define ASSERT_FALSE(cond) do { if (cond) throw std::runtime_error("Assertion failed: NOT " #cond); } while(0)
#define ASSERT_EQ(a, b) do { if ((a) != (b)) throw std::runtime_error("Assertion failed: " #a " == " #b); } while(0)
#define ASSERT_GT(a, b) do { if (!((a) > (b))) throw std::runtime_error("Assertion failed: " #a " > " #b); } while(0)
#define ASSERT_GE(a, b) do { if (!((a) >= (b))) throw std::runtime_error("Assertion failed: " #a " >= " #b); } while(0)
#define ASSERT_NEAR(a, b, tol) do { if (std::abs((a) - (b)) > (tol)) throw std::runtime_error("Assertion failed: " #a " near " #b); } while(0)

// Helper to create a sample with given properties
DomObservationSample MakeSample(
    int64_t tsMs, int barIdx, int bidTick, int askTick,
    double bidSize, double askSize,
    double bidStackPull = 0.0, double askStackPull = 0.0,
    double haloImbal = 0.0, bool haloValid = true,
    double askVolSec = 0.0, double bidVolSec = 0.0, double deltaSec = 0.0)
{
    DomObservationSample s;
    s.timestampMs = tsMs;
    s.barIndex = barIdx;
    s.bestBidTick = bidTick;
    s.bestAskTick = askTick;
    s.domBidSize = bidSize;
    s.domAskSize = askSize;
    s.bidStackPull = bidStackPull;
    s.askStackPull = askStackPull;
    s.haloDepthImbalance = haloImbal;
    s.haloDepthValid = haloValid;
    s.askVolSec = askVolSec;
    s.bidVolSec = bidVolSec;
    s.deltaSec = deltaSec;
    s.tradesSec = 10.0;
    return s;
}

// ============================================================================
// BALANCE DOM PATTERN TESTS
// ============================================================================

TEST(StackedBids_HighBidRatio)
{
    // Setup: High bid/ask ratio (3:1)
    DomHistoryBuffer buffer;
    for (int i = 0; i < 10; ++i)
    {
        // Bid size 300, Ask size 100 = 3.0 ratio
        auto s = MakeSample(1000 + i * 500, i, 1000, 1001, 300.0, 100.0);
        s.haloDepthImbalance = 0.5 + (i % 3) * 0.05;  // Add variance for MAD
        buffer.Push(s);
    }

    auto window = buffer.GetWindow(DomEventConfig::DEFAULT_WINDOW_MS);
    auto features = ExtractFeatures(window, DomEventConfig::DEFAULT_WINDOW_MS);
    DomPatternFeatures pf = ExtractPatternFeatures(window, features);

    auto hit = DetectStackedBids(features, pf);
    ASSERT_TRUE(hit.has_value());
    ASSERT_EQ(hit->type, BalanceDOMPattern::STACKED_BIDS);
    ASSERT_GT(hit->strength01, 0.0f);
}

TEST(StackedBids_LowRatioNoDetect)
{
    // Setup: Low bid/ask ratio (1.5:1) - below threshold
    DomHistoryBuffer buffer;
    for (int i = 0; i < 10; ++i)
    {
        // Bid size 150, Ask size 100 = 1.5 ratio
        auto s = MakeSample(1000 + i * 500, i, 1000, 1001, 150.0, 100.0);
        s.haloDepthImbalance = 0.3 + (i % 3) * 0.05;
        buffer.Push(s);
    }

    auto window = buffer.GetWindow(DomEventConfig::DEFAULT_WINDOW_MS);
    auto features = ExtractFeatures(window, DomEventConfig::DEFAULT_WINDOW_MS);
    DomPatternFeatures pf = ExtractPatternFeatures(window, features);

    auto hit = DetectStackedBids(features, pf);
    ASSERT_FALSE(hit.has_value());
}

TEST(StackedAsks_HighAskRatio)
{
    // Setup: High ask/bid ratio (3:1 inverse = 0.33)
    DomHistoryBuffer buffer;
    for (int i = 0; i < 10; ++i)
    {
        // Bid size 100, Ask size 300 = 0.33 ratio
        auto s = MakeSample(1000 + i * 500, i, 1000, 1001, 100.0, 300.0);
        s.haloDepthImbalance = -0.5 + (i % 3) * 0.05;
        buffer.Push(s);
    }

    auto window = buffer.GetWindow(DomEventConfig::DEFAULT_WINDOW_MS);
    auto features = ExtractFeatures(window, DomEventConfig::DEFAULT_WINDOW_MS);
    DomPatternFeatures pf = ExtractPatternFeatures(window, features);

    auto hit = DetectStackedAsks(features, pf);
    ASSERT_TRUE(hit.has_value());
    ASSERT_EQ(hit->type, BalanceDOMPattern::STACKED_ASKS);
    ASSERT_GT(hit->strength01, 0.0f);
}

TEST(OrderReloading_PullThenStack)
{
    // Setup: Initial pulls followed by consistent restacking
    DomHistoryBuffer buffer;

    // First 3 samples: Pull dominant (negative stackPull)
    for (int i = 0; i < 3; ++i)
    {
        auto s = MakeSample(1000 + i * 500, i, 1000, 1001, 200.0, 200.0,
                           -10.0, -10.0);  // Pull dominant
        s.haloDepthImbalance = 0.1 + (i % 3) * 0.05;
        buffer.Push(s);
    }

    // Next 5 samples: Stack dominant (positive stackPull)
    for (int i = 3; i < 8; ++i)
    {
        auto s = MakeSample(1000 + i * 500, i, 1000, 1001, 200.0, 200.0,
                           15.0, 15.0);  // Stack dominant
        s.haloDepthImbalance = 0.1 + (i % 3) * 0.05;
        buffer.Push(s);
    }

    auto window = buffer.GetWindow(DomEventConfig::DEFAULT_WINDOW_MS);
    auto features = ExtractFeatures(window, DomEventConfig::DEFAULT_WINDOW_MS);
    DomPatternFeatures pf = ExtractPatternFeatures(window, features);

    auto hit = DetectOrderReloading(features, pf);
    ASSERT_TRUE(hit.has_value());
    ASSERT_EQ(hit->type, BalanceDOMPattern::ORDER_RELOADING);
}

TEST(SpoofOrderFlip_RapidFlips)
{
    // Setup: Rapid depth imbalance flips
    DomHistoryBuffer buffer;

    for (int i = 0; i < 10; ++i)
    {
        // Alternate between bid-heavy and ask-heavy depth
        double bidSize = (i % 2 == 0) ? 300.0 : 100.0;
        double askSize = (i % 2 == 0) ? 100.0 : 300.0;
        auto s = MakeSample(1000 + i * 500, i, 1000, 1001, bidSize, askSize);
        s.haloDepthImbalance = 0.0 + (i % 3) * 0.1;
        buffer.Push(s);
    }

    auto window = buffer.GetWindow(DomEventConfig::DEFAULT_WINDOW_MS);
    auto features = ExtractFeatures(window, DomEventConfig::DEFAULT_WINDOW_MS);
    DomPatternFeatures pf = ExtractPatternFeatures(window, features);

    auto hit = DetectSpoofOrderFlip(features, pf);
    ASSERT_TRUE(hit.has_value());
    ASSERT_EQ(hit->type, BalanceDOMPattern::SPOOF_ORDER_FLIP);
}

TEST(SpoofOrderFlip_StableNoFlip)
{
    // Setup: Stable depth (no flips)
    DomHistoryBuffer buffer;

    for (int i = 0; i < 10; ++i)
    {
        // Consistent bid-heavy depth
        auto s = MakeSample(1000 + i * 500, i, 1000, 1001, 200.0, 100.0);
        s.haloDepthImbalance = 0.4 + (i % 3) * 0.03;
        buffer.Push(s);
    }

    auto window = buffer.GetWindow(DomEventConfig::DEFAULT_WINDOW_MS);
    auto features = ExtractFeatures(window, DomEventConfig::DEFAULT_WINDOW_MS);
    DomPatternFeatures pf = ExtractPatternFeatures(window, features);

    auto hit = DetectSpoofOrderFlip(features, pf);
    ASSERT_FALSE(hit.has_value());
}

// ============================================================================
// IMBALANCE DOM PATTERN TESTS
// ============================================================================

TEST(ChasingOrdersBuy_BidAdvancing)
{
    // Setup: Best bid advancing persistently
    DomHistoryBuffer buffer;

    for (int i = 0; i < 10; ++i)
    {
        // Bid price advancing 1 tick per sample
        int bidTick = 1000 + i;  // 1000, 1001, 1002, ...
        auto s = MakeSample(1000 + i * 500, i, bidTick, bidTick + 1, 200.0, 200.0);
        s.haloDepthImbalance = 0.3 + (i % 3) * 0.05;
        buffer.Push(s);
    }

    auto window = buffer.GetWindow(DomEventConfig::DEFAULT_WINDOW_MS);
    auto features = ExtractFeatures(window, DomEventConfig::DEFAULT_WINDOW_MS);
    DomPatternFeatures pf = ExtractPatternFeatures(window, features);

    auto hit = DetectChasingOrdersBuy(features, pf);
    ASSERT_TRUE(hit.has_value());
    ASSERT_EQ(hit->type, ImbalanceDOMPattern::CHASING_ORDERS_BUY);
    ASSERT_GT(hit->strength01, 0.0f);
}

TEST(ChasingOrdersBuy_NoAdvance)
{
    // Setup: Bid price not advancing
    DomHistoryBuffer buffer;

    for (int i = 0; i < 10; ++i)
    {
        // Bid price stable
        auto s = MakeSample(1000 + i * 500, i, 1000, 1001, 200.0, 200.0);
        s.haloDepthImbalance = 0.3 + (i % 3) * 0.05;
        buffer.Push(s);
    }

    auto window = buffer.GetWindow(DomEventConfig::DEFAULT_WINDOW_MS);
    auto features = ExtractFeatures(window, DomEventConfig::DEFAULT_WINDOW_MS);
    DomPatternFeatures pf = ExtractPatternFeatures(window, features);

    auto hit = DetectChasingOrdersBuy(features, pf);
    ASSERT_FALSE(hit.has_value());
}

TEST(ChasingOrdersSell_AskDeclining)
{
    // Setup: Best ask declining persistently
    DomHistoryBuffer buffer;

    for (int i = 0; i < 10; ++i)
    {
        // Ask price declining 1 tick per sample
        int askTick = 1010 - i;  // 1010, 1009, 1008, ...
        auto s = MakeSample(1000 + i * 500, i, askTick - 1, askTick, 200.0, 200.0);
        s.haloDepthImbalance = -0.3 + (i % 3) * 0.05;
        buffer.Push(s);
    }

    auto window = buffer.GetWindow(DomEventConfig::DEFAULT_WINDOW_MS);
    auto features = ExtractFeatures(window, DomEventConfig::DEFAULT_WINDOW_MS);
    DomPatternFeatures pf = ExtractPatternFeatures(window, features);

    auto hit = DetectChasingOrdersSell(features, pf);
    ASSERT_TRUE(hit.has_value());
    ASSERT_EQ(hit->type, ImbalanceDOMPattern::CHASING_ORDERS_SELL);
}

TEST(BidAskRatioExtreme_BidDominant)
{
    // Setup: Extreme bid/ask ratio (4:1)
    DomHistoryBuffer buffer;

    for (int i = 0; i < 10; ++i)
    {
        // Bid size 400, Ask size 100 = 4.0 ratio
        auto s = MakeSample(1000 + i * 500, i, 1000, 1001, 400.0, 100.0);
        s.haloDepthImbalance = 0.6 + (i % 3) * 0.05;
        buffer.Push(s);
    }

    auto window = buffer.GetWindow(DomEventConfig::DEFAULT_WINDOW_MS);
    auto features = ExtractFeatures(window, DomEventConfig::DEFAULT_WINDOW_MS);
    DomPatternFeatures pf = ExtractPatternFeatures(window, features);

    auto hit = DetectBidAskRatioExtreme(features, pf);
    ASSERT_TRUE(hit.has_value());
    ASSERT_EQ(hit->type, ImbalanceDOMPattern::BID_ASK_RATIO_EXTREME);
    ASSERT_EQ(hit->anchorTick, 1);  // Positive = bid extreme
}

TEST(BidAskRatioExtreme_AskDominant)
{
    // Setup: Extreme ask/bid ratio (1:4)
    DomHistoryBuffer buffer;

    for (int i = 0; i < 10; ++i)
    {
        // Bid size 100, Ask size 400 = 0.25 ratio
        auto s = MakeSample(1000 + i * 500, i, 1000, 1001, 100.0, 400.0);
        s.haloDepthImbalance = -0.6 + (i % 3) * 0.05;
        buffer.Push(s);
    }

    auto window = buffer.GetWindow(DomEventConfig::DEFAULT_WINDOW_MS);
    auto features = ExtractFeatures(window, DomEventConfig::DEFAULT_WINDOW_MS);
    DomPatternFeatures pf = ExtractPatternFeatures(window, features);

    auto hit = DetectBidAskRatioExtreme(features, pf);
    ASSERT_TRUE(hit.has_value());
    ASSERT_EQ(hit->type, ImbalanceDOMPattern::BID_ASK_RATIO_EXTREME);
    ASSERT_EQ(hit->anchorTick, -1);  // Negative = ask extreme
}

TEST(BidAskRatioExtreme_NoExtreme)
{
    // Setup: Normal bid/ask ratio (2:1) - below extreme threshold
    DomHistoryBuffer buffer;

    for (int i = 0; i < 10; ++i)
    {
        // Bid size 200, Ask size 100 = 2.0 ratio
        auto s = MakeSample(1000 + i * 500, i, 1000, 1001, 200.0, 100.0);
        s.haloDepthImbalance = 0.3 + (i % 3) * 0.05;
        buffer.Push(s);
    }

    auto window = buffer.GetWindow(DomEventConfig::DEFAULT_WINDOW_MS);
    auto features = ExtractFeatures(window, DomEventConfig::DEFAULT_WINDOW_MS);
    DomPatternFeatures pf = ExtractPatternFeatures(window, features);

    auto hit = DetectBidAskRatioExtreme(features, pf);
    ASSERT_FALSE(hit.has_value());
}

// ============================================================================
// ABSORPTION FAILURE TESTS (Composite)
// ============================================================================

TEST(AbsorptionFailure_BothConditions)
{
    // Setup: Group 1 result with both EXHAUSTION_DIVERGENCE and SWEEP_LIQUIDATION
    DomDetectionResult group1;
    group1.windowMs = 5000;
    group1.wasEligible = true;

    // Add EXHAUSTION_DIVERGENCE hit
    DOMControlHit exhaustHit;
    exhaustHit.type = DOMControlPattern::EXHAUSTION_DIVERGENCE;
    exhaustHit.strength01 = 0.7f;
    group1.controlPatterns.push_back(exhaustHit.type);
    group1.controlHits.push_back(exhaustHit);

    // Add SWEEP_LIQUIDATION hit
    DOMEventHit sweepHit;
    sweepHit.type = DOMEvent::SWEEP_LIQUIDATION;
    sweepHit.strength01 = 0.8f;
    group1.events.push_back(sweepHit.type);
    group1.eventHits.push_back(sweepHit);

    auto hit = DetectAbsorptionFailure(group1);
    ASSERT_TRUE(hit.has_value());
    ASSERT_EQ(hit->type, ImbalanceDOMPattern::ABSORPTION_FAILURE);
    ASSERT_TRUE(hit->isComposite);
    // Geometric mean of 0.7 and 0.8 = sqrt(0.56) ≈ 0.748
    ASSERT_NEAR(hit->strength01, std::sqrt(0.7f * 0.8f), 0.01f);
}

TEST(AbsorptionFailure_OnlyExhaustion)
{
    // Setup: Only EXHAUSTION_DIVERGENCE (no SWEEP_LIQUIDATION)
    DomDetectionResult group1;
    group1.windowMs = 5000;
    group1.wasEligible = true;

    DOMControlHit exhaustHit;
    exhaustHit.type = DOMControlPattern::EXHAUSTION_DIVERGENCE;
    exhaustHit.strength01 = 0.7f;
    group1.controlPatterns.push_back(exhaustHit.type);
    group1.controlHits.push_back(exhaustHit);

    auto hit = DetectAbsorptionFailure(group1);
    ASSERT_FALSE(hit.has_value());  // Must have BOTH conditions
}

TEST(AbsorptionFailure_OnlySweep)
{
    // Setup: Only SWEEP_LIQUIDATION (no EXHAUSTION_DIVERGENCE)
    DomDetectionResult group1;
    group1.windowMs = 5000;
    group1.wasEligible = true;

    DOMEventHit sweepHit;
    sweepHit.type = DOMEvent::SWEEP_LIQUIDATION;
    sweepHit.strength01 = 0.8f;
    group1.events.push_back(sweepHit.type);
    group1.eventHits.push_back(sweepHit);

    auto hit = DetectAbsorptionFailure(group1);
    ASSERT_FALSE(hit.has_value());  // Must have BOTH conditions
}

TEST(AbsorptionFailure_NoConditions)
{
    // Setup: Neither condition present
    DomDetectionResult group1;
    group1.windowMs = 5000;
    group1.wasEligible = true;

    auto hit = DetectAbsorptionFailure(group1);
    ASSERT_FALSE(hit.has_value());
}

// ============================================================================
// MAIN DETECTION FUNCTION TESTS
// ============================================================================

TEST(DetectDomPatterns_IntegrationFlow)
{
    // Setup: Buffer with stacked bids pattern
    DomHistoryBuffer buffer;
    for (int i = 0; i < 10; ++i)
    {
        auto s = MakeSample(1000 + i * 500, i, 1000, 1001, 300.0, 100.0);
        s.haloDepthImbalance = 0.5 + (i % 3) * 0.05;
        buffer.Push(s);
    }

    // Extract base features
    auto window = buffer.GetWindow(DomEventConfig::DEFAULT_WINDOW_MS);
    auto features = ExtractFeatures(window, DomEventConfig::DEFAULT_WINDOW_MS);

    // Create empty Group 1 result
    DomDetectionResult group1;
    group1.windowMs = DomEventConfig::DEFAULT_WINDOW_MS;
    group1.wasEligible = true;

    // Run detection
    auto result = DetectDomPatterns(buffer, features, group1, DomEventConfig::DEFAULT_WINDOW_MS);

    ASSERT_TRUE(result.wasEligible);
    ASSERT_TRUE(result.HasPatterns());
    ASSERT_FALSE(result.balancePatterns.empty());
    ASSERT_EQ(result.balancePatterns[0], BalanceDOMPattern::STACKED_BIDS);
}

TEST(DetectDomPatterns_IneligibleFeatures)
{
    // Setup: Insufficient samples
    DomHistoryBuffer buffer;
    for (int i = 0; i < 3; ++i)  // Only 3 samples (below MIN_SAMPLES)
    {
        auto s = MakeSample(1000 + i * 500, i, 1000, 1001, 300.0, 100.0);
        buffer.Push(s);
    }

    auto window = buffer.GetWindow(DomEventConfig::DEFAULT_WINDOW_MS);
    auto features = ExtractFeatures(window, DomEventConfig::DEFAULT_WINDOW_MS);

    DomDetectionResult group1;
    group1.windowMs = DomEventConfig::DEFAULT_WINDOW_MS;
    group1.wasEligible = false;
    group1.ineligibleReason = "INSUFFICIENT_SAMPLES";

    auto result = DetectDomPatterns(buffer, features, group1, DomEventConfig::DEFAULT_WINDOW_MS);

    ASSERT_FALSE(result.wasEligible);
    ASSERT_FALSE(result.HasPatterns());
}

// ============================================================================
// LOGGING AND OBSERVABILITY TESTS
// ============================================================================

TEST(DomPatternLogState_ThrottleAndChange)
{
    DomPatternLogState logState;

    // First result with patterns
    DomPatternResult result1;
    result1.balancePatterns.push_back(BalanceDOMPattern::STACKED_BIDS);

    // First emission should succeed
    ASSERT_TRUE(logState.ShouldLog(result1, 0));

    // Same patterns, within throttle window - should NOT log
    ASSERT_FALSE(logState.ShouldLog(result1, 5));

    // Same patterns, outside throttle window - should still not log (no change)
    ASSERT_FALSE(logState.ShouldLog(result1, 15));

    // Different patterns - should log
    DomPatternResult result2;
    result2.imbalancePatterns.push_back(ImbalanceDOMPattern::CHASING_ORDERS_BUY);
    ASSERT_TRUE(logState.ShouldLog(result2, 16));
}

TEST(BuildDomPatternLogMessage_Format)
{
    DomPatternResult result;
    result.balancePatterns.push_back(BalanceDOMPattern::STACKED_BIDS);
    result.imbalancePatterns.push_back(ImbalanceDOMPattern::BID_ASK_RATIO_EXTREME);

    BalanceDOMHit balHit;
    balHit.type = BalanceDOMPattern::STACKED_BIDS;
    balHit.strength01 = 0.75f;
    result.balanceHits.push_back(balHit);

    ImbalanceDOMHit imbHit;
    imbHit.type = ImbalanceDOMPattern::BID_ASK_RATIO_EXTREME;
    imbHit.strength01 = 0.60f;
    result.imbalanceHits.push_back(imbHit);

    std::string msg = BuildDomPatternLogMessage(result, 12345678);

    // Check message contains expected elements
    ASSERT_TRUE(msg.find("[DOM-PAT]") != std::string::npos);
    ASSERT_TRUE(msg.find("ts=12345678") != std::string::npos);
    ASSERT_TRUE(msg.find("BAL=") != std::string::npos);
    ASSERT_TRUE(msg.find("STACKED_BIDS") != std::string::npos);
    ASSERT_TRUE(msg.find("IMB=") != std::string::npos);
    ASSERT_TRUE(msg.find("BA_RATIO_EXT") != std::string::npos);
}

TEST(BuildDomPatternLogMessage_EmptyResult)
{
    DomPatternResult result;  // Empty

    std::string msg = BuildDomPatternLogMessage(result, 0);

    ASSERT_TRUE(msg.find("NONE") != std::string::npos);
}

// ============================================================================
// PATTERN FEATURE EXTRACTION TESTS
// ============================================================================

TEST(ExtractPatternFeatures_DepthRatio)
{
    DomHistoryBuffer buffer;
    for (int i = 0; i < 10; ++i)
    {
        // Bid size 250, Ask size 100 = 2.5 ratio
        auto s = MakeSample(1000 + i * 500, i, 1000, 1001, 250.0, 100.0);
        s.haloDepthImbalance = 0.4 + (i % 3) * 0.05;
        buffer.Push(s);
    }

    auto window = buffer.GetWindow(DomEventConfig::DEFAULT_WINDOW_MS);
    auto features = ExtractFeatures(window, DomEventConfig::DEFAULT_WINDOW_MS);
    DomPatternFeatures pf = ExtractPatternFeatures(window, features);

    ASSERT_TRUE(pf.isEligible);
    ASSERT_NEAR(pf.bidAskDepthRatio, 2.5, 0.01);
}

TEST(ExtractPatternFeatures_PriceMovement)
{
    DomHistoryBuffer buffer;
    for (int i = 0; i < 10; ++i)
    {
        // Bid advancing 1 tick per sample
        auto s = MakeSample(1000 + i * 500, i, 1000 + i, 1001 + i, 200.0, 200.0);
        s.haloDepthImbalance = 0.3 + (i % 3) * 0.05;
        buffer.Push(s);
    }

    auto window = buffer.GetWindow(DomEventConfig::DEFAULT_WINDOW_MS);
    auto features = ExtractFeatures(window, DomEventConfig::DEFAULT_WINDOW_MS);
    DomPatternFeatures pf = ExtractPatternFeatures(window, features);

    ASSERT_EQ(pf.totalBidMoveTicks, 9);  // 1009 - 1000
    ASSERT_EQ(pf.totalAskMoveTicks, 9);  // 1010 - 1001
    ASSERT_GT(pf.bidAdvanceSamples, 0);
}

// ============================================================================
// MAIN
// ============================================================================

int main()
{
    std::cout << "========================================\n";
    std::cout << "DOM Pattern Detection Tests\n";
    std::cout << "========================================\n\n";

    // Balance DOM Pattern tests
    std::cout << "--- Balance DOM Patterns ---\n";
    RUN_TEST(StackedBids_HighBidRatio);
    RUN_TEST(StackedBids_LowRatioNoDetect);
    RUN_TEST(StackedAsks_HighAskRatio);
    RUN_TEST(OrderReloading_PullThenStack);
    RUN_TEST(SpoofOrderFlip_RapidFlips);
    RUN_TEST(SpoofOrderFlip_StableNoFlip);

    // Imbalance DOM Pattern tests
    std::cout << "\n--- Imbalance DOM Patterns ---\n";
    RUN_TEST(ChasingOrdersBuy_BidAdvancing);
    RUN_TEST(ChasingOrdersBuy_NoAdvance);
    RUN_TEST(ChasingOrdersSell_AskDeclining);
    RUN_TEST(BidAskRatioExtreme_BidDominant);
    RUN_TEST(BidAskRatioExtreme_AskDominant);
    RUN_TEST(BidAskRatioExtreme_NoExtreme);

    // Absorption Failure (Composite) tests
    std::cout << "\n--- Absorption Failure (Composite) ---\n";
    RUN_TEST(AbsorptionFailure_BothConditions);
    RUN_TEST(AbsorptionFailure_OnlyExhaustion);
    RUN_TEST(AbsorptionFailure_OnlySweep);
    RUN_TEST(AbsorptionFailure_NoConditions);

    // Main detection function tests
    std::cout << "\n--- Detection Function ---\n";
    RUN_TEST(DetectDomPatterns_IntegrationFlow);
    RUN_TEST(DetectDomPatterns_IneligibleFeatures);

    // Observability tests
    std::cout << "\n--- Observability ---\n";
    RUN_TEST(DomPatternLogState_ThrottleAndChange);
    RUN_TEST(BuildDomPatternLogMessage_Format);
    RUN_TEST(BuildDomPatternLogMessage_EmptyResult);

    // Feature extraction tests
    std::cout << "\n--- Feature Extraction ---\n";
    RUN_TEST(ExtractPatternFeatures_DepthRatio);
    RUN_TEST(ExtractPatternFeatures_PriceMovement);

    std::cout << "\n========================================\n";
    std::cout << "Results: " << g_testsPassed << " passed, " << g_testsFailed << " failed\n";
    std::cout << "========================================\n";

    return g_testsFailed > 0 ? 1 : 0;
}
