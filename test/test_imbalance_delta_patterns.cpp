// ============================================================================
// test_imbalance_delta_patterns.cpp
// Unit tests for AMT_ImbalanceDeltaPatterns.h - Imbalance Delta Pattern Detection
// Tests: STRONG_CONVERGENCE, WEAK_PULLBACK, EFFORT_NO_RESULT, CLIMAX_EXHAUSTION
// ============================================================================

#include "../AMT_ImbalanceDeltaPatterns.h"
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
#define ASSERT_LT(a, b) do { if (!((a) < (b))) throw std::runtime_error("Assertion failed: " #a " < " #b); } while(0)
#define ASSERT_GE(a, b) do { if (!((a) >= (b))) throw std::runtime_error("Assertion failed: " #a " >= " #b); } while(0)
#define ASSERT_NEAR(a, b, tol) do { if (std::abs((a) - (b)) > (tol)) throw std::runtime_error("Assertion failed: " #a " near " #b); } while(0)

// Helper to create a sample with given properties
DomObservationSample MakeSample(
    int64_t tsMs, int barIdx, int bidTick, int askTick,
    double deltaSec = 0.0, double bidVolSec = 50.0, double askVolSec = 50.0)
{
    DomObservationSample s;
    s.timestampMs = tsMs;
    s.barIndex = barIdx;
    s.bestBidTick = bidTick;
    s.bestAskTick = askTick;
    s.domBidSize = 200.0;
    s.domAskSize = 200.0;
    s.bidStackPull = 0.0;
    s.askStackPull = 0.0;
    s.haloDepthImbalance = 0.0;
    s.haloDepthValid = true;
    s.askVolSec = askVolSec;
    s.bidVolSec = bidVolSec;
    s.deltaSec = deltaSec;
    s.tradesSec = 10.0;
    return s;
}

// Helper to compute DomEventFeatures for testing
DomEventFeatures MakeFeatures(const DomHistoryBuffer& buffer, int windowMs = 5000)
{
    auto window = buffer.GetWindow(windowMs);
    return ExtractFeatures(window, windowMs);
}

// ============================================================================
// REGIME GATING TESTS
// ============================================================================

TEST(RegimeGating_NotImbalance)
{
    // Setup: TPO_OVERLAP regime (balance) - patterns should NOT fire
    DomHistoryBuffer buffer;
    TrendProgressTracker tracker;
    std::vector<TPOMechanics> tpoMech = { TPOMechanics::TPO_OVERLAP };  // Balance
    std::vector<DOMEvent> domEvents;
    std::vector<DOMControlPattern> domControlPatterns;

    // Create samples with delta spike
    for (int i = 0; i < 10; ++i)
    {
        double delta = (i < 7) ? 1.0 : 15.0;
        auto s = MakeSample(1000 + i * 500, i, 1000 + i, 1001 + i, delta);
        buffer.Push(s);
    }

    tracker.EstablishTrend(1, 1005, 5);
    tracker.Update(1010, 10);

    auto features = MakeFeatures(buffer);
    auto result = DetectImbalanceDeltaPatterns(
        buffer, features, tracker, tpoMech, domEvents, domControlPatterns, 5000);

    ASSERT_FALSE(result.wasEligible);
    ASSERT_FALSE(result.wasInImbalanceRegime);
    ASSERT_TRUE(result.patterns.empty());
}

TEST(RegimeGating_ImbalanceAllowed)
{
    // Setup: TPO_SEPARATION regime (imbalance) - patterns CAN fire
    DomHistoryBuffer buffer;
    TrendProgressTracker tracker;
    std::vector<TPOMechanics> tpoMech = { TPOMechanics::TPO_SEPARATION };  // Imbalance
    std::vector<DOMEvent> domEvents;
    std::vector<DOMControlPattern> domControlPatterns;

    // Create samples with delta spike and price progress
    for (int i = 0; i < 7; ++i)
    {
        double delta = 2.0;
        auto s = MakeSample(1000 + i * 500, i, 1000 + i, 1001 + i, delta);
        buffer.Push(s);
    }
    for (int i = 7; i < 10; ++i)
    {
        double delta = 15.0;
        auto s = MakeSample(1000 + i * 500, i, 1000 + i, 1001 + i, delta);
        buffer.Push(s);
    }

    tracker.EstablishTrend(1, 1005, 5);
    tracker.Update(1010, 10);

    auto features = MakeFeatures(buffer);
    auto result = DetectImbalanceDeltaPatterns(
        buffer, features, tracker, tpoMech, domEvents, domControlPatterns, 5000);

    ASSERT_TRUE(result.wasEligible);
    ASSERT_TRUE(result.wasInImbalanceRegime);
}

// ============================================================================
// TREND PROGRESS TRACKER TESTS
// ============================================================================

TEST(TrendTracker_ResetClearsState)
{
    TrendProgressTracker tracker;
    tracker.EstablishTrend(1, 1000, 10);
    tracker.Update(1005, 15);

    ASSERT_EQ(tracker.trendDirection, 1);
    ASSERT_GT(tracker.trendDurationBars, 0);

    tracker.Reset();

    ASSERT_EQ(tracker.trendDirection, 0);
    ASSERT_EQ(tracker.trendDurationBars, 0);
    ASSERT_EQ(tracker.highWaterTick, 0);
}

TEST(TrendTracker_UptrendProgress)
{
    TrendProgressTracker tracker;
    tracker.EstablishTrend(1, 1000, 10);  // Start uptrend at 1000

    // Price rises
    tracker.Update(1005, 11);  // +5 ticks
    tracker.Update(1008, 12);  // +3 more
    tracker.Update(1010, 13);  // +2 more

    ASSERT_EQ(tracker.highWaterTick, 1010);
    ASSERT_EQ(tracker.lowWaterTick, 1000);
    ASSERT_EQ(tracker.trendDurationBars, 4);  // 13 - 10 + 1
    ASSERT_EQ(tracker.GetRetraceTicks(), 0);  // No retrace yet
}

TEST(TrendTracker_UptrendPullback)
{
    TrendProgressTracker tracker;
    tracker.EstablishTrend(1, 1000, 10);
    tracker.Update(1005, 11);  // Peak
    tracker.Update(1003, 12);  // Pullback -2 ticks

    ASSERT_EQ(tracker.peakTick, 1005);
    ASSERT_EQ(tracker.GetRetraceTicks(), 2);  // 1005 - 1003
    ASSERT_TRUE(tracker.IsInPullback(1, 4));  // Within 1-4 tick retrace
}

TEST(TrendTracker_DowntrendProgress)
{
    TrendProgressTracker tracker;
    tracker.EstablishTrend(-1, 1000, 10);  // Start downtrend at 1000

    // Price falls
    tracker.Update(995, 11);  // -5 ticks
    tracker.Update(992, 12);  // -3 more

    ASSERT_EQ(tracker.lowWaterTick, 992);
    ASSERT_EQ(tracker.highWaterTick, 1000);
    ASSERT_EQ(tracker.troughTick, 992);
}

// ============================================================================
// STRONG_CONVERGENCE TESTS
// ============================================================================

TEST(StrongConvergence_UptrendWithPositiveDelta)
{
    // Uptrend with delta supporting price movement
    DomHistoryBuffer buffer;
    TrendProgressTracker tracker;
    std::vector<TPOMechanics> tpoMech = { TPOMechanics::TPO_SEPARATION };
    std::vector<DOMEvent> domEvents;
    std::vector<DOMControlPattern> domControlPatterns;

    // First 7 samples: low baseline delta (to establish low median/MAD)
    for (int i = 0; i < 7; ++i)
    {
        double delta = 1.0 + (i % 3) * 0.3;  // Low baseline: ~1.0-1.6
        auto s = MakeSample(1000 + i * 500, i, 1000, 1001, delta);
        buffer.Push(s);
    }
    // Last 3 samples: strong positive delta spike with price progress
    for (int i = 7; i < 10; ++i)
    {
        double delta = 8.0 + (i % 2) * 1.0;  // High spike: 8-9 (z-score > 1.0)
        auto s = MakeSample(1000 + i * 500, i, 1000 + i - 4, 1001 + i - 4, delta);
        buffer.Push(s);
    }

    // Establish uptrend with sufficient duration (>= 3 bars)
    tracker.EstablishTrend(1, 1001, 3);
    tracker.Update(1006, 10);

    auto features = MakeFeatures(buffer);
    auto result = DetectImbalanceDeltaPatterns(
        buffer, features, tracker, tpoMech, domEvents, domControlPatterns, 5000);

    // Should detect STRONG_CONVERGENCE
    bool foundConvergence = false;
    for (const auto& p : result.patterns)
    {
        if (p == ImbalanceDeltaPattern::STRONG_CONVERGENCE) foundConvergence = true;
    }
    ASSERT_TRUE(foundConvergence);
}

TEST(StrongConvergence_DeltaOpposesPrice_NoHit)
{
    // Price rising but delta negative - no convergence
    DomHistoryBuffer buffer;
    TrendProgressTracker tracker;
    std::vector<TPOMechanics> tpoMech = { TPOMechanics::TPO_SEPARATION };
    std::vector<DOMEvent> domEvents;
    std::vector<DOMControlPattern> domControlPatterns;

    for (int i = 0; i < 10; ++i)
    {
        double delta = (i < 5) ? -1.0 : -10.0;  // Negative delta
        auto s = MakeSample(1000 + i * 500, i, 1000 + i, 1001 + i, delta);
        buffer.Push(s);
    }

    tracker.EstablishTrend(1, 1001, 3);  // Uptrend
    tracker.Update(1010, 10);

    auto features = MakeFeatures(buffer);
    auto result = DetectImbalanceDeltaPatterns(
        buffer, features, tracker, tpoMech, domEvents, domControlPatterns, 5000);

    bool foundConvergence = false;
    for (const auto& p : result.patterns)
    {
        if (p == ImbalanceDeltaPattern::STRONG_CONVERGENCE) foundConvergence = true;
    }
    ASSERT_FALSE(foundConvergence);
}

TEST(StrongConvergence_InsufficientTrendDuration_NoHit)
{
    // Trend too short (< 3 bars)
    DomHistoryBuffer buffer;
    TrendProgressTracker tracker;
    std::vector<TPOMechanics> tpoMech = { TPOMechanics::TPO_SEPARATION };
    std::vector<DOMEvent> domEvents;
    std::vector<DOMControlPattern> domControlPatterns;

    for (int i = 0; i < 10; ++i)
    {
        double delta = (i < 5) ? 2.0 : 15.0;
        auto s = MakeSample(1000 + i * 500, i, 1000 + i, 1001 + i, delta);
        buffer.Push(s);
    }

    tracker.EstablishTrend(1, 1008, 9);  // Very short trend (2 bars)
    tracker.Update(1010, 10);

    auto features = MakeFeatures(buffer);
    auto result = DetectImbalanceDeltaPatterns(
        buffer, features, tracker, tpoMech, domEvents, domControlPatterns, 5000);

    bool foundConvergence = false;
    for (const auto& p : result.patterns)
    {
        if (p == ImbalanceDeltaPattern::STRONG_CONVERGENCE) foundConvergence = true;
    }
    ASSERT_FALSE(foundConvergence);
}

// ============================================================================
// WEAK_PULLBACK TESTS
// ============================================================================

TEST(WeakPullback_UptrendWithShallowRetrace)
{
    DomHistoryBuffer buffer;
    TrendProgressTracker tracker;
    std::vector<TPOMechanics> tpoMech = { TPOMechanics::TPO_SEPARATION };
    std::vector<DOMEvent> domEvents;
    std::vector<DOMControlPattern> domControlPatterns;

    // Baseline samples at lower price
    for (int i = 0; i < 5; ++i)
    {
        auto s = MakeSample(1000 + i * 500, i, 1000, 1001, 2.0);
        buffer.Push(s);
    }
    // Samples showing pullback from peak (1010 -> 1008)
    for (int i = 5; i < 10; ++i)
    {
        int bid = 1008;  // Pulled back from peak
        auto s = MakeSample(1000 + i * 500, i, bid, bid + 1, 3.0);  // Neutral delta (not reversing)
        buffer.Push(s);
    }

    // Establish uptrend, peak was at 1010, now at 1008 (2 tick pullback)
    tracker.EstablishTrend(1, 1000, 1);
    tracker.Update(1010, 5);  // Peak
    tracker.peakTick = 1010;
    tracker.lastMidTick = 1008;  // Pulled back

    auto features = MakeFeatures(buffer);
    auto result = DetectImbalanceDeltaPatterns(
        buffer, features, tracker, tpoMech, domEvents, domControlPatterns, 5000);

    bool foundPullback = false;
    for (const auto& p : result.patterns)
    {
        if (p == ImbalanceDeltaPattern::WEAK_PULLBACK) foundPullback = true;
    }
    ASSERT_TRUE(foundPullback);
}

TEST(WeakPullback_DeltaReversesStrongly_NoHit)
{
    DomHistoryBuffer buffer;
    TrendProgressTracker tracker;
    std::vector<TPOMechanics> tpoMech = { TPOMechanics::TPO_SEPARATION };
    std::vector<DOMEvent> domEvents;
    std::vector<DOMControlPattern> domControlPatterns;

    // Samples with strongly negative delta (reversal, not continuation)
    for (int i = 0; i < 5; ++i)
    {
        auto s = MakeSample(1000 + i * 500, i, 1000, 1001, 2.0);
        buffer.Push(s);
    }
    for (int i = 5; i < 10; ++i)
    {
        auto s = MakeSample(1000 + i * 500, i, 1008, 1009, -12.0);  // Strong reversal delta
        buffer.Push(s);
    }

    tracker.EstablishTrend(1, 1000, 1);
    tracker.Update(1010, 5);
    tracker.peakTick = 1010;
    tracker.lastMidTick = 1008;

    auto features = MakeFeatures(buffer);
    auto result = DetectImbalanceDeltaPatterns(
        buffer, features, tracker, tpoMech, domEvents, domControlPatterns, 5000);

    bool foundPullback = false;
    for (const auto& p : result.patterns)
    {
        if (p == ImbalanceDeltaPattern::WEAK_PULLBACK) foundPullback = true;
    }
    ASSERT_FALSE(foundPullback);
}

// ============================================================================
// EFFORT_NO_RESULT TESTS
// ============================================================================

TEST(EffortNoResult_HighEffortNoProgress)
{
    DomHistoryBuffer buffer;
    TrendProgressTracker tracker;
    std::vector<TPOMechanics> tpoMech = { TPOMechanics::TPO_SEPARATION };
    std::vector<DOMEvent> domEvents;
    std::vector<DOMControlPattern> domControlPatterns;

    // First 7 samples: low baseline delta
    for (int i = 0; i < 7; ++i)
    {
        double delta = 1.0 + (i % 3) * 0.2;  // Low baseline: 1.0-1.4
        auto s = MakeSample(1000 + i * 500, i, 1000, 1001, delta);
        buffer.Push(s);
    }
    // Last 3 samples: extreme delta spike but price stalled (z-score > 2.0)
    for (int i = 7; i < 10; ++i)
    {
        double delta = 15.0 + (i % 2) * 2.0;  // High spike: 15-17
        auto s = MakeSample(1000 + i * 500, i, 1000, 1001, delta);  // Same price!
        buffer.Push(s);
    }

    auto features = MakeFeatures(buffer);
    auto result = DetectImbalanceDeltaPatterns(
        buffer, features, tracker, tpoMech, domEvents, domControlPatterns, 5000);

    bool foundEffort = false;
    for (const auto& p : result.patterns)
    {
        if (p == ImbalanceDeltaPattern::EFFORT_NO_RESULT) foundEffort = true;
    }
    ASSERT_TRUE(foundEffort);
}

TEST(EffortNoResult_PriceProgresses_NoHit)
{
    DomHistoryBuffer buffer;
    TrendProgressTracker tracker;
    std::vector<TPOMechanics> tpoMech = { TPOMechanics::TPO_SEPARATION };
    std::vector<DOMEvent> domEvents;
    std::vector<DOMControlPattern> domControlPatterns;

    // Baseline
    for (int i = 0; i < 5; ++i)
    {
        auto s = MakeSample(1000 + i * 500, i, 1000, 1001, 2.0);
        buffer.Push(s);
    }
    // High delta AND price progress (normal trend, not effort-no-result)
    for (int i = 5; i < 10; ++i)
    {
        auto s = MakeSample(1000 + i * 500, i, 1000 + i, 1001 + i, 20.0);  // Price moves
        buffer.Push(s);
    }

    auto features = MakeFeatures(buffer);
    auto result = DetectImbalanceDeltaPatterns(
        buffer, features, tracker, tpoMech, domEvents, domControlPatterns, 5000);

    bool foundEffort = false;
    for (const auto& p : result.patterns)
    {
        if (p == ImbalanceDeltaPattern::EFFORT_NO_RESULT) foundEffort = true;
    }
    ASSERT_FALSE(foundEffort);
}

// ============================================================================
// CLIMAX_EXHAUSTION TESTS
// ============================================================================

TEST(ClimaxExhaustion_ExtremeEffortWithConfirmer)
{
    DomHistoryBuffer buffer;
    TrendProgressTracker tracker;
    std::vector<TPOMechanics> tpoMech = { TPOMechanics::TPO_SEPARATION };
    std::vector<DOMEvent> domEvents = { DOMEvent::ORDER_FLOW_REVERSAL };  // Confirmer
    std::vector<DOMControlPattern> domControlPatterns;

    // First 7 samples: low baseline delta
    for (int i = 0; i < 7; ++i)
    {
        double delta = 1.0 + (i % 3) * 0.2;  // Low baseline: 1.0-1.4
        auto s = MakeSample(1000 + i * 500, i, 1000, 1001, delta);
        buffer.Push(s);
    }
    // Last 3 samples: extreme delta spike (z-score > 2.5)
    for (int i = 7; i < 10; ++i)
    {
        double delta = 25.0 + (i % 2) * 3.0;  // Extreme spike: 25-28
        auto s = MakeSample(1000 + i * 500, i, 1000, 1001, delta);
        buffer.Push(s);
    }

    auto features = MakeFeatures(buffer);
    auto result = DetectImbalanceDeltaPatterns(
        buffer, features, tracker, tpoMech, domEvents, domControlPatterns, 5000);

    bool foundClimax = false;
    for (size_t i = 0; i < result.patterns.size(); ++i)
    {
        if (result.patterns[i] == ImbalanceDeltaPattern::CLIMAX_EXHAUSTION)
        {
            foundClimax = true;
            ASSERT_TRUE(result.hits[i].hasConfirmation);
            ASSERT_EQ(result.hits[i].confirmationType & 1, 1);  // FLOW_REVERSAL
        }
    }
    ASSERT_TRUE(foundClimax);
}

TEST(ClimaxExhaustion_NoConfirmer_NoHit)
{
    DomHistoryBuffer buffer;
    TrendProgressTracker tracker;
    std::vector<TPOMechanics> tpoMech = { TPOMechanics::TPO_SEPARATION };
    std::vector<DOMEvent> domEvents;  // NO confirmer
    std::vector<DOMControlPattern> domControlPatterns;

    for (int i = 0; i < 5; ++i)
    {
        auto s = MakeSample(1000 + i * 500, i, 1000, 1001, 2.0);
        buffer.Push(s);
    }
    for (int i = 5; i < 10; ++i)
    {
        auto s = MakeSample(1000 + i * 500, i, 1000, 1001, 30.0);
        buffer.Push(s);
    }

    auto features = MakeFeatures(buffer);
    auto result = DetectImbalanceDeltaPatterns(
        buffer, features, tracker, tpoMech, domEvents, domControlPatterns, 5000);

    bool foundClimax = false;
    for (const auto& p : result.patterns)
    {
        if (p == ImbalanceDeltaPattern::CLIMAX_EXHAUSTION) foundClimax = true;
    }
    ASSERT_FALSE(foundClimax);  // No confirmer = no hit
}

TEST(ClimaxExhaustion_MultipleConfirmers)
{
    DomHistoryBuffer buffer;
    TrendProgressTracker tracker;
    std::vector<TPOMechanics> tpoMech = { TPOMechanics::TPO_SEPARATION };
    std::vector<DOMEvent> domEvents = { DOMEvent::ORDER_FLOW_REVERSAL, DOMEvent::SWEEP_LIQUIDATION };
    std::vector<DOMControlPattern> domControlPatterns = { DOMControlPattern::EXHAUSTION_DIVERGENCE };

    // First 7 samples: low baseline delta
    for (int i = 0; i < 7; ++i)
    {
        double delta = 1.0 + (i % 3) * 0.2;  // Low baseline: 1.0-1.4
        auto s = MakeSample(1000 + i * 500, i, 1000, 1001, delta);
        buffer.Push(s);
    }
    // Last 3 samples: extreme delta spike (z-score > 2.5)
    for (int i = 7; i < 10; ++i)
    {
        double delta = 25.0 + (i % 2) * 3.0;  // Extreme spike: 25-28
        auto s = MakeSample(1000 + i * 500, i, 1000, 1001, delta);
        buffer.Push(s);
    }

    auto features = MakeFeatures(buffer);
    auto result = DetectImbalanceDeltaPatterns(
        buffer, features, tracker, tpoMech, domEvents, domControlPatterns, 5000);

    bool foundClimax = false;
    for (size_t i = 0; i < result.patterns.size(); ++i)
    {
        if (result.patterns[i] == ImbalanceDeltaPattern::CLIMAX_EXHAUSTION)
        {
            foundClimax = true;
            ASSERT_EQ(result.hits[i].confirmationType, 7);  // All 3 confirmers: 1+2+4
        }
    }
    ASSERT_TRUE(foundClimax);
}

// ============================================================================
// LOGGING TESTS
// ============================================================================

TEST(Logging_BuildLogMessage)
{
    ImbalanceDeltaPatternResult result;
    result.patterns.push_back(ImbalanceDeltaPattern::STRONG_CONVERGENCE);
    result.patterns.push_back(ImbalanceDeltaPattern::CLIMAX_EXHAUSTION);

    ImbalanceDeltaHit hit1;
    hit1.type = ImbalanceDeltaPattern::STRONG_CONVERGENCE;
    hit1.strength01 = 0.8f;
    result.hits.push_back(hit1);

    ImbalanceDeltaHit hit2;
    hit2.type = ImbalanceDeltaPattern::CLIMAX_EXHAUSTION;
    hit2.strength01 = 0.9f;
    hit2.hasConfirmation = true;
    hit2.confirmationType = 5;  // FR + ED
    result.hits.push_back(hit2);

    std::string msg = BuildImbalanceDeltaLogMessage(result, 12345);

    ASSERT_TRUE(msg.find("IMB-DELTA") != std::string::npos);
    ASSERT_TRUE(msg.find("STRONG_CONV") != std::string::npos);
    ASSERT_TRUE(msg.find("CLIMAX_EXH") != std::string::npos);
    ASSERT_TRUE(msg.find("FR") != std::string::npos);
    ASSERT_TRUE(msg.find("ED") != std::string::npos);
}

TEST(LogState_DedupSameBar)
{
    ImbalanceDeltaLogState state;
    ImbalanceDeltaPatternResult result;
    result.patterns.push_back(ImbalanceDeltaPattern::STRONG_CONVERGENCE);

    ASSERT_TRUE(state.ShouldLog(result, 100));  // First time
    ASSERT_FALSE(state.ShouldLog(result, 100)); // Same bar, same patterns

    result.patterns.push_back(ImbalanceDeltaPattern::WEAK_PULLBACK);
    ASSERT_TRUE(state.ShouldLog(result, 100));  // Patterns changed
}

TEST(LogState_ResetClearsState)
{
    ImbalanceDeltaLogState state;
    ImbalanceDeltaPatternResult result;
    result.patterns.push_back(ImbalanceDeltaPattern::STRONG_CONVERGENCE);

    state.ShouldLog(result, 100);
    state.Reset();

    ASSERT_TRUE(state.ShouldLog(result, 100));  // Should log again after reset
}

// ============================================================================
// TO_STRING TESTS
// ============================================================================

TEST(ToString_AllPatterns)
{
    ASSERT_EQ(std::string(to_string(ImbalanceDeltaPattern::STRONG_CONVERGENCE)), "STRONG_CONV");
    ASSERT_EQ(std::string(to_string(ImbalanceDeltaPattern::WEAK_PULLBACK)), "WEAK_PB");
    ASSERT_EQ(std::string(to_string(ImbalanceDeltaPattern::EFFORT_NO_RESULT)), "EFFORT_NO_RES");
    ASSERT_EQ(std::string(to_string(ImbalanceDeltaPattern::CLIMAX_EXHAUSTION)), "CLIMAX_EXH");
}

// ============================================================================
// MAIN
// ============================================================================

int main()
{
    std::cout << "=== Imbalance Delta Pattern Tests ===\n\n";

    // Regime gating
    RUN_TEST(RegimeGating_NotImbalance);
    RUN_TEST(RegimeGating_ImbalanceAllowed);

    // Trend tracker
    RUN_TEST(TrendTracker_ResetClearsState);
    RUN_TEST(TrendTracker_UptrendProgress);
    RUN_TEST(TrendTracker_UptrendPullback);
    RUN_TEST(TrendTracker_DowntrendProgress);

    // STRONG_CONVERGENCE
    RUN_TEST(StrongConvergence_UptrendWithPositiveDelta);
    RUN_TEST(StrongConvergence_DeltaOpposesPrice_NoHit);
    RUN_TEST(StrongConvergence_InsufficientTrendDuration_NoHit);

    // WEAK_PULLBACK
    RUN_TEST(WeakPullback_UptrendWithShallowRetrace);
    RUN_TEST(WeakPullback_DeltaReversesStrongly_NoHit);

    // EFFORT_NO_RESULT
    RUN_TEST(EffortNoResult_HighEffortNoProgress);
    RUN_TEST(EffortNoResult_PriceProgresses_NoHit);

    // CLIMAX_EXHAUSTION
    RUN_TEST(ClimaxExhaustion_ExtremeEffortWithConfirmer);
    RUN_TEST(ClimaxExhaustion_NoConfirmer_NoHit);
    RUN_TEST(ClimaxExhaustion_MultipleConfirmers);

    // Logging
    RUN_TEST(Logging_BuildLogMessage);
    RUN_TEST(LogState_DedupSameBar);
    RUN_TEST(LogState_ResetClearsState);

    // to_string
    RUN_TEST(ToString_AllPatterns);

    std::cout << "\n=== Summary ===\n";
    std::cout << "Passed: " << g_testsPassed << "\n";
    std::cout << "Failed: " << g_testsFailed << "\n";

    return g_testsFailed > 0 ? 1 : 0;
}
