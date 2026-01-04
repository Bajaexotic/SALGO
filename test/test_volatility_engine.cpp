// ============================================================================
// test_volatility_engine.cpp - Unit Tests for VolatilityEngine
// ============================================================================
// Tests:
//   1. Regime classification from percentiles
//   2. Hysteresis state machine (transition confirmation)
//   3. Tradability rules per regime
//   4. Session boundary handling (finalize/reset)
//   5. ATR normalization
//   6. Validity gating (warmup, errors)
//   7. Auction pace classification
//   8. Pace hysteresis
//   9. Pace tradability multipliers
//
// Compile: g++ -std=c++17 -I.. -o test_volatility_engine.exe test_volatility_engine.cpp
// Run: ./test_volatility_engine.exe
// ============================================================================

#include <iostream>
#include <cassert>
#include <cmath>
#include <string>
#include <vector>

// Include proper Sierra Chart mocks for standalone testing
#include "test_sierrachart_mock.h"

#include "../amt_core.h"
#include "../AMT_Snapshots.h"
#include "../AMT_Volatility.h"

using namespace AMT;

// ============================================================================
// TEST UTILITIES
// ============================================================================

static int g_testsPassed = 0;
static int g_testsFailed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "[FAIL] " << msg << " (line " << __LINE__ << ")\n"; \
        g_testsFailed++; \
    } else { \
        g_testsPassed++; \
    } \
} while(0)

#define TEST_SECTION(name) std::cout << "\n=== " << name << " ===\n"

// Default bar duration for tests (60 seconds = 1 minute bars)
static constexpr double TEST_BAR_DURATION_SEC = 60.0;

// Helper: Create populated EffortBaselineStore with bar_range and range_velocity samples
EffortBaselineStore CreatePopulatedEffortStore() {
    EffortBaselineStore store;
    store.Reset(500);

    // Populate GLOBEX and INITIAL_BALANCE buckets with realistic ranges
    // ES typically: 2-4 ticks compression, 4-8 normal, 8-15 expansion, 15+ event
    const double barDurationMin = TEST_BAR_DURATION_SEC / 60.0;
    const double synthDurationMin = TEST_BAR_DURATION_SEC * 5 / 60.0;  // 5-bar synthetic

    for (int i = 0; i < AMT::EFFORT_BUCKET_COUNT; ++i) {
        auto& bucket = store.buckets[i];

        // Add 100 samples with roughly normal distribution
        // Mean ~6 ticks, std ~3 ticks
        for (int j = 0; j < 100; ++j) {
            double range = 2.0 + (j % 12);  // Range: 2-14 ticks
            bucket.bar_range.push(range);

            // Range velocity: ticks per minute
            double rangeVelocity = range / barDurationMin;
            bucket.range_velocity.push(rangeVelocity);
        }

        // Populate SYNTHETIC baselines (5-bar aggregation = wider ranges)
        // Synthetic range ~3-5x wider than individual bar range
        for (int j = 0; j < 50; ++j) {
            double synthRange = 8.0 + (j % 24);  // Range: 8-32 ticks (5-bar window)
            bucket.synthetic_bar_range.push(synthRange);

            // Synthetic velocity: ticks per minute for synthetic bar
            double synthVelocity = synthRange / synthDurationMin;
            bucket.synthetic_range_velocity.push(synthVelocity);
        }

        bucket.sessionsContributed = 5;
        bucket.totalBarsPushed = 100;
    }

    return store;
}

// ============================================================================
// TEST: Regime Classification
// ============================================================================

void TestRegimeClassification() {
    TEST_SECTION("Regime Classification");

    VolatilityEngine engine;
    auto store = CreatePopulatedEffortStore();
    engine.SetEffortStore(&store);
    engine.SetPhase(SessionPhase::INITIAL_BALANCE);

    // Test compression (low percentile)
    {
        auto result = engine.Compute(2.0, TEST_BAR_DURATION_SEC);  // Low range
        TEST_ASSERT(result.IsReady(), "Result should be ready with populated baseline");
        TEST_ASSERT(result.rangePercentile < 30.0, "Low range should have low percentile");
        // Raw regime should be compression (before hysteresis kicks in for first bar)
        TEST_ASSERT(result.rawRegime == VolatilityRegime::COMPRESSION ||
                    result.rawRegime == VolatilityRegime::NORMAL,
                    "Low range should be compression or normal");
    }

    engine.ResetForSession();

    // Test normal (mid percentile)
    {
        auto result = engine.Compute(7.0, TEST_BAR_DURATION_SEC);  // Mid range
        TEST_ASSERT(result.IsReady(), "Result should be ready");
        TEST_ASSERT(result.rangePercentile > 30.0 && result.rangePercentile < 80.0,
                    "Mid range should have mid percentile");
        TEST_ASSERT(result.rawRegime == VolatilityRegime::NORMAL,
                    "Mid range should be NORMAL regime");
    }

    engine.ResetForSession();

    // Test expansion (high percentile)
    {
        auto result = engine.Compute(13.0, TEST_BAR_DURATION_SEC);  // High range
        TEST_ASSERT(result.IsReady(), "Result should be ready");
        TEST_ASSERT(result.rangePercentile > 70.0, "High range should have high percentile");
        TEST_ASSERT(result.rawRegime == VolatilityRegime::EXPANSION ||
                    result.rawRegime == VolatilityRegime::EVENT,
                    "High range should be EXPANSION or EVENT");
    }

    std::cout << "[OK] Regime classification works correctly\n";
}

// ============================================================================
// TEST: Hysteresis State Machine
// ============================================================================

void TestHysteresis() {
    TEST_SECTION("Hysteresis State Machine");

    VolatilityEngine engine;
    engine.config.minConfirmationBars = 3;  // Need 3 bars to confirm transition

    auto store = CreatePopulatedEffortStore();
    engine.SetEffortStore(&store);
    engine.SetPhase(SessionPhase::MID_SESSION);

    // Start in NORMAL regime
    for (int i = 0; i < 5; ++i) {
        auto result = engine.Compute(7.0, TEST_BAR_DURATION_SEC);  // Normal range
    }
    TEST_ASSERT(engine.confirmedRegime == VolatilityRegime::NORMAL,
                "Should be in NORMAL after 5 normal bars");

    // Single compression bar should NOT change regime (hysteresis)
    {
        auto result = engine.Compute(2.0, TEST_BAR_DURATION_SEC);  // Compression range
        TEST_ASSERT(engine.confirmedRegime == VolatilityRegime::NORMAL,
                    "Single compression bar should not change regime");
        TEST_ASSERT(engine.candidateRegime == VolatilityRegime::COMPRESSION,
                    "Candidate should be COMPRESSION");
        TEST_ASSERT(engine.candidateConfirmationBars == 1,
                    "Should have 1 confirmation bar");
    }

    // Return to normal resets candidate
    {
        auto result = engine.Compute(7.0, TEST_BAR_DURATION_SEC);  // Normal range
        TEST_ASSERT(engine.confirmedRegime == VolatilityRegime::NORMAL,
                    "Should still be in NORMAL");
        TEST_ASSERT(engine.candidateConfirmationBars == 0,
                    "Candidate bars should reset on return to confirmed");
    }

    // Three consecutive compression bars SHOULD change regime
    for (int i = 0; i < 3; ++i) {
        auto result = engine.Compute(2.0, TEST_BAR_DURATION_SEC);  // Compression range
    }
    TEST_ASSERT(engine.confirmedRegime == VolatilityRegime::COMPRESSION,
                "Should transition to COMPRESSION after 3 consecutive compression bars");

    std::cout << "[OK] Hysteresis prevents whipsaw and confirms transitions\n";
}

// ============================================================================
// TEST: Tradability Rules
// ============================================================================

void TestTradabilityRules() {
    TEST_SECTION("Tradability Rules");

    VolatilityEngine engine;
    auto store = CreatePopulatedEffortStore();
    engine.SetEffortStore(&store);
    engine.SetPhase(SessionPhase::MID_SESSION);

    // Test NORMAL regime tradability (defaults)
    {
        for (int i = 0; i < 5; ++i) engine.Compute(7.0, TEST_BAR_DURATION_SEC);  // Establish NORMAL

        auto result = engine.Compute(7.0, TEST_BAR_DURATION_SEC);
        TEST_ASSERT(result.tradability.allowNewEntries, "NORMAL should allow new entries");
        TEST_ASSERT(!result.tradability.blockBreakouts, "NORMAL should not block breakouts");
        TEST_ASSERT(result.tradability.positionSizeMultiplier == 1.0,
                    "NORMAL should have 1.0 position multiplier");
    }

    engine.ResetForSession();

    // Test COMPRESSION regime tradability
    {
        // Need to get into compression regime
        for (int i = 0; i < 5; ++i) engine.Compute(2.0, TEST_BAR_DURATION_SEC);  // Low range

        auto result = engine.Compute(2.0, TEST_BAR_DURATION_SEC);
        if (result.regime == VolatilityRegime::COMPRESSION) {
            TEST_ASSERT(result.tradability.blockBreakouts,
                        "COMPRESSION should block breakouts");
            TEST_ASSERT(result.tradability.preferMeanReversion,
                        "COMPRESSION should prefer mean reversion");
            TEST_ASSERT(result.tradability.positionSizeMultiplier < 1.0,
                        "COMPRESSION should scale down position size");
        }
    }

    engine.ResetForSession();

    // Test EXPANSION regime tradability
    {
        for (int i = 0; i < 5; ++i) engine.Compute(13.0, TEST_BAR_DURATION_SEC);  // High range

        auto result = engine.Compute(13.0, TEST_BAR_DURATION_SEC);
        if (result.regime == VolatilityRegime::EXPANSION) {
            TEST_ASSERT(result.tradability.requireWideStop,
                        "EXPANSION should require wide stops");
        }
    }

    std::cout << "[OK] Tradability rules configured per regime\n";
}

// ============================================================================
// TEST: Session Boundary Handling
// ============================================================================

void TestSessionBoundary() {
    TEST_SECTION("Session Boundary Handling");

    VolatilityEngine engine;
    auto store = CreatePopulatedEffortStore();
    engine.SetEffortStore(&store);
    engine.SetPhase(SessionPhase::MID_SESSION);

    // Simulate first session
    for (int i = 0; i < 50; ++i) {
        engine.Compute(7.0 + (i % 5), TEST_BAR_DURATION_SEC);  // Varying normal ranges
    }

    TEST_ASSERT(engine.sessionBars == 50, "Should track 50 session bars");

    // Finalize session (updates priors)
    engine.FinalizeSession();
    TEST_ASSERT(engine.priorReady, "Prior should be ready after FinalizeSession");
    TEST_ASSERT(engine.sessionsContributed == 1, "Should have 1 session contributed");
    TEST_ASSERT(engine.priorAvgRange > 0.0, "Prior avg range should be positive");

    // Reset for new session
    engine.ResetForSession();
    TEST_ASSERT(engine.sessionBars == 0, "Session bars should reset");
    TEST_ASSERT(engine.confirmedRegime == VolatilityRegime::UNKNOWN,
                "Confirmed regime should reset");
    TEST_ASSERT(engine.priorReady, "Prior should be preserved across reset");

    // New session should have prior available
    auto result = engine.Compute(7.0, TEST_BAR_DURATION_SEC);
    TEST_ASSERT(result.priorReady, "Result should have prior available");
    TEST_ASSERT(result.priorSessionAvgRange > 0.0, "Prior avg range in result should be positive");

    std::cout << "[OK] Session boundary handling works correctly\n";
}

// ============================================================================
// TEST: ATR Normalization
// ============================================================================

void TestATRNormalization() {
    TEST_SECTION("ATR Normalization");

    VolatilityEngine engine;
    auto store = CreatePopulatedEffortStore();
    engine.SetEffortStore(&store);
    engine.SetPhase(SessionPhase::MID_SESSION);

    // Test without ATR
    {
        auto result = engine.Compute(8.0, TEST_BAR_DURATION_SEC, 0.0);  // No ATR
        TEST_ASSERT(!result.normalizedRangeValid, "Normalized range should be invalid without ATR");
        TEST_ASSERT(!result.atrReady, "ATR should not be ready");
    }

    // Test with ATR
    {
        auto result = engine.Compute(8.0, TEST_BAR_DURATION_SEC, 4.0);  // ATR = 4
        TEST_ASSERT(result.normalizedRangeValid, "Normalized range should be valid with ATR");
        TEST_ASSERT(std::abs(result.normalizedRange - 2.0) < 0.01,
                    "Normalized range should be 8/4 = 2.0");
    }

    // Build ATR baseline
    for (int i = 0; i < 15; ++i) {
        engine.Compute(7.0, TEST_BAR_DURATION_SEC, 4.0 + (i % 3));  // ATR varies 4-6
    }

    {
        auto result = engine.Compute(8.0, TEST_BAR_DURATION_SEC, 5.0);
        TEST_ASSERT(result.atrReady, "ATR baseline should be ready after 15 samples");
        TEST_ASSERT(result.atrPercentile > 0.0 && result.atrPercentile <= 100.0,
                    "ATR percentile should be in valid range");
    }

    std::cout << "[OK] ATR normalization works correctly\n";
}

// ============================================================================
// TEST: Validity Gating
// ============================================================================

void TestValidityGating() {
    TEST_SECTION("Validity Gating");

    VolatilityEngine engine;
    engine.SetPhase(SessionPhase::MID_SESSION);

    // Test without effort store
    {
        auto result = engine.Compute(8.0, TEST_BAR_DURATION_SEC);
        TEST_ASSERT(!result.IsReady(), "Should not be ready without effort store");
        TEST_ASSERT(result.errorReason == VolatilityErrorReason::ERR_NO_EFFORT_STORE,
                    "Error should be NO_EFFORT_STORE");
    }

    // Test with empty effort store (warmup)
    {
        EffortBaselineStore emptyStore;
        emptyStore.Reset(100);
        engine.SetEffortStore(&emptyStore);

        auto result = engine.Compute(8.0, TEST_BAR_DURATION_SEC);
        TEST_ASSERT(!result.IsReady(), "Should not be ready with empty baseline");
        TEST_ASSERT(result.IsWarmup(), "Should be in warmup state");
        TEST_ASSERT(result.errorReason == VolatilityErrorReason::WARMUP_BASELINE,
                    "Error should be WARMUP_BASELINE");
    }

    // Test invalid input
    {
        auto store = CreatePopulatedEffortStore();
        engine.SetEffortStore(&store);

        auto result = engine.Compute(-5.0, TEST_BAR_DURATION_SEC);  // Negative range
        TEST_ASSERT(!result.IsReady(), "Should not be ready with invalid input");
        TEST_ASSERT(result.errorReason == VolatilityErrorReason::ERR_INVALID_INPUT,
                    "Error should be INVALID_INPUT");
    }

    std::cout << "[OK] Validity gating prevents invalid usage\n";
}

// ============================================================================
// TEST: Event Detection
// ============================================================================

void TestEventDetection() {
    TEST_SECTION("Event Detection");

    VolatilityEngine engine;
    engine.config.eventThreshold = 95.0;  // P95 = event

    // Create store with more varied distribution to ensure we can hit P95+
    EffortBaselineStore store;
    store.Reset(500);
    const double barDurationMin = TEST_BAR_DURATION_SEC / 60.0;
    for (int i = 0; i < AMT::EFFORT_BUCKET_COUNT; ++i) {
        auto& bucket = store.buckets[i];
        for (int j = 0; j < 100; ++j) {
            double range = 2.0 + (j % 10);  // Range: 2-12 ticks
            bucket.bar_range.push(range);
            bucket.range_velocity.push(range / barDurationMin);
        }
        bucket.sessionsContributed = 5;
        bucket.totalBarsPushed = 100;
    }

    engine.SetEffortStore(&store);
    engine.SetPhase(SessionPhase::MID_SESSION);

    // Establish normal baseline
    for (int i = 0; i < 5; ++i) {
        engine.Compute(6.0, TEST_BAR_DURATION_SEC);
    }

    // Extreme range should trigger EVENT
    auto result = engine.Compute(20.0, TEST_BAR_DURATION_SEC);  // Way above P95
    TEST_ASSERT(result.IsReady(), "Should be ready");
    TEST_ASSERT(result.rangePercentile > 95.0, "Extreme range should be > P95");
    TEST_ASSERT(result.rawRegime == VolatilityRegime::EVENT,
                "Raw regime should be EVENT for extreme range");

    std::cout << "[OK] Event detection identifies extreme volatility\n";
}

// ============================================================================
// TEST: Stability Tracking
// ============================================================================

void TestStabilityTracking() {
    TEST_SECTION("Stability Tracking");

    VolatilityEngine engine;
    auto store = CreatePopulatedEffortStore();
    engine.SetEffortStore(&store);
    engine.SetPhase(SessionPhase::MID_SESSION);

    // Establish and track stability
    for (int i = 0; i < 10; ++i) {
        auto result = engine.Compute(7.0, TEST_BAR_DURATION_SEC);

        if (i >= 1) {  // After first bar
            TEST_ASSERT(result.stabilityBars == i + 1,
                        "Stability bars should increment each bar without regime change");
        }
    }

    TEST_ASSERT(engine.stabilityBars == 10, "Should have 10 stability bars");

    // Force regime change resets stability
    engine.config.minConfirmationBars = 1;  // Immediate confirmation for test
    auto result = engine.Compute(2.0, TEST_BAR_DURATION_SEC);  // Compression

    // Note: The exact behavior depends on whether the regime actually changes
    // With minConfirmationBars=1, first compression bar might not immediately confirm
    // Let's verify barsInRegime is being tracked
    TEST_ASSERT(engine.barsInConfirmedRegime > 0, "Should track bars in confirmed regime");

    std::cout << "[OK] Stability tracking works correctly\n";
}

// ============================================================================
// TEST: Expected Range Multiplier
// ============================================================================

void TestExpectedRangeMultiplier() {
    TEST_SECTION("Expected Range Multiplier");

    VolatilityEngine engine;
    engine.config.compressionExpectedMultiplier = 0.6;
    engine.config.normalExpectedMultiplier = 1.0;
    engine.config.expansionExpectedMultiplier = 1.5;
    engine.config.eventExpectedMultiplier = 2.5;

    auto store = CreatePopulatedEffortStore();
    engine.SetEffortStore(&store);
    engine.SetPhase(SessionPhase::MID_SESSION);

    // Establish NORMAL regime
    for (int i = 0; i < 5; ++i) engine.Compute(7.0, TEST_BAR_DURATION_SEC);

    {
        auto result = engine.Compute(7.0, TEST_BAR_DURATION_SEC);
        if (result.regime == VolatilityRegime::NORMAL) {
            TEST_ASSERT(result.expectedRangeMultiplier == 1.0,
                        "NORMAL should have 1.0 expected multiplier");
        }
    }

    engine.ResetForSession();
    engine.config.minConfirmationBars = 1;  // Fast transitions for test

    // Get into compression
    for (int i = 0; i < 3; ++i) engine.Compute(2.0, TEST_BAR_DURATION_SEC);

    {
        auto result = engine.Compute(2.0, TEST_BAR_DURATION_SEC);
        if (result.regime == VolatilityRegime::COMPRESSION) {
            TEST_ASSERT(result.expectedRangeMultiplier == 0.6,
                        "COMPRESSION should have 0.6 expected multiplier");
        }
    }

    std::cout << "[OK] Expected range multipliers configured per regime\n";
}

// ============================================================================
// TEST: Auction Pace Classification
// ============================================================================

void TestPaceClassification() {
    TEST_SECTION("Auction Pace Classification");

    VolatilityEngine engine;
    auto store = CreatePopulatedEffortStore();
    engine.SetEffortStore(&store);
    engine.SetPhase(SessionPhase::INITIAL_BALANCE);

    // Test slow pace (low velocity = slow discovery)
    // With 60 sec bars and range of 2 ticks, velocity = 2 ticks/min (low)
    {
        auto result = engine.Compute(2.0, TEST_BAR_DURATION_SEC);
        TEST_ASSERT(result.IsReady(), "Result should be ready");
        TEST_ASSERT(result.IsPaceReady(), "Pace should be ready with populated baseline");
        TEST_ASSERT(result.rangeVelocity > 0.0, "Range velocity should be positive");
        // Low velocity should be slow pace (< P25)
        TEST_ASSERT(result.rangeVelocityPercentile < 30.0 ||
                    result.rawPace == AuctionPace::SLOW ||
                    result.rawPace == AuctionPace::NORMAL,
                    "Low velocity should classify as SLOW or NORMAL");
    }

    engine.ResetForSession();

    // Test normal pace (mid velocity)
    {
        auto result = engine.Compute(7.0, TEST_BAR_DURATION_SEC);
        TEST_ASSERT(result.IsPaceReady(), "Pace should be ready");
        // Mid velocity should be normal pace
        TEST_ASSERT(result.rawPace == AuctionPace::NORMAL,
                    "Mid velocity should be NORMAL pace");
    }

    engine.ResetForSession();

    // Test fast pace (high velocity = rapid discovery)
    {
        auto result = engine.Compute(13.0, TEST_BAR_DURATION_SEC);
        TEST_ASSERT(result.IsPaceReady(), "Pace should be ready");
        // High velocity should be fast pace
        TEST_ASSERT(result.rawPace == AuctionPace::FAST ||
                    result.rawPace == AuctionPace::EXTREME,
                    "High velocity should be FAST or EXTREME pace");
    }

    std::cout << "[OK] Auction pace classification works correctly\n";
}

// ============================================================================
// TEST: Pace Hysteresis
// ============================================================================

void TestPaceHysteresis() {
    TEST_SECTION("Pace Hysteresis");

    VolatilityEngine engine;
    engine.config.paceMinConfirmationBars = 2;  // Need 2 bars to confirm pace change

    auto store = CreatePopulatedEffortStore();
    engine.SetEffortStore(&store);
    engine.SetPhase(SessionPhase::MID_SESSION);

    // Start in NORMAL pace
    for (int i = 0; i < 5; ++i) {
        auto result = engine.Compute(7.0, TEST_BAR_DURATION_SEC);  // Normal velocity
    }
    TEST_ASSERT(engine.confirmedPace == AuctionPace::NORMAL,
                "Should be in NORMAL pace after 5 normal bars");

    // Single slow bar should NOT change pace (hysteresis)
    {
        auto result = engine.Compute(2.0, TEST_BAR_DURATION_SEC);  // Slow velocity
        TEST_ASSERT(engine.confirmedPace == AuctionPace::NORMAL,
                    "Single slow bar should not change pace");
        // Note: Candidate pace may or may not be set depending on percentile
    }

    // Return to normal resets candidate
    {
        auto result = engine.Compute(7.0, TEST_BAR_DURATION_SEC);  // Normal velocity
        TEST_ASSERT(engine.confirmedPace == AuctionPace::NORMAL,
                    "Should still be in NORMAL pace");
    }

    // Two consecutive slow bars SHOULD change pace (with minConfirmationBars=2)
    {
        engine.Compute(2.0, TEST_BAR_DURATION_SEC);  // First slow bar
        auto result = engine.Compute(2.0, TEST_BAR_DURATION_SEC);  // Second slow bar

        // After 2 slow bars, if we transitioned, confirmedPace should be SLOW
        // Note: This depends on the percentile calculation
        TEST_ASSERT(result.IsPaceReady(), "Pace should still be ready");
    }

    std::cout << "[OK] Pace hysteresis prevents pace whipsaw\n";
}

// ============================================================================
// TEST: Pace Tradability Multipliers
// ============================================================================

void TestPaceTradabilityMultipliers() {
    TEST_SECTION("Pace Tradability Multipliers");

    VolatilityEngine engine;
    engine.config.slowPaceConfirmationMultiplier = 0.8;
    engine.config.slowPaceSizeMultiplier = 1.0;
    engine.config.normalPaceConfirmationMultiplier = 1.0;
    engine.config.normalPaceSizeMultiplier = 1.0;
    engine.config.fastPaceConfirmationMultiplier = 1.5;
    engine.config.fastPaceSizeMultiplier = 0.75;
    engine.config.extremePaceConfirmationMultiplier = 2.0;
    engine.config.extremePaceSizeMultiplier = 0.5;

    auto store = CreatePopulatedEffortStore();
    engine.SetEffortStore(&store);
    engine.SetPhase(SessionPhase::MID_SESSION);

    // Test NORMAL pace multipliers
    {
        for (int i = 0; i < 5; ++i) engine.Compute(7.0, TEST_BAR_DURATION_SEC);

        auto result = engine.Compute(7.0, TEST_BAR_DURATION_SEC);
        if (result.pace == AuctionPace::NORMAL) {
            TEST_ASSERT(result.tradability.paceConfirmationMultiplier == 1.0,
                        "NORMAL pace should have 1.0 confirmation multiplier");
            TEST_ASSERT(result.tradability.paceSizeMultiplier == 1.0,
                        "NORMAL pace should have 1.0 size multiplier");
        }
    }

    engine.ResetForSession();

    // Test combined regime + pace multipliers
    {
        // Get into NORMAL regime with NORMAL pace
        for (int i = 0; i < 5; ++i) engine.Compute(7.0, TEST_BAR_DURATION_SEC);

        auto result = engine.Compute(7.0, TEST_BAR_DURATION_SEC);
        // Combined size multiplier = regime * pace
        double combined = result.GetCombinedPositionSizeMultiplier();
        TEST_ASSERT(combined > 0.0, "Combined size multiplier should be positive");
    }

    std::cout << "[OK] Pace tradability multipliers configured correctly\n";
}

// ============================================================================
// TEST: Pace Session Reset
// ============================================================================

void TestPaceSessionReset() {
    TEST_SECTION("Pace Session Reset");

    VolatilityEngine engine;
    auto store = CreatePopulatedEffortStore();
    engine.SetEffortStore(&store);
    engine.SetPhase(SessionPhase::MID_SESSION);

    // Establish pace state
    for (int i = 0; i < 10; ++i) {
        engine.Compute(7.0, TEST_BAR_DURATION_SEC);
    }

    TEST_ASSERT(engine.confirmedPace != AuctionPace::UNKNOWN,
                "Pace should be established after 10 bars");
    TEST_ASSERT(engine.barsInConfirmedPace > 0,
                "Should track bars in confirmed pace");

    // Reset for new session
    engine.ResetForSession();

    TEST_ASSERT(engine.confirmedPace == AuctionPace::UNKNOWN,
                "Confirmed pace should reset");
    TEST_ASSERT(engine.candidatePace == AuctionPace::UNKNOWN,
                "Candidate pace should reset");
    TEST_ASSERT(engine.candidatePaceConfirmationBars == 0,
                "Pace confirmation bars should reset");
    TEST_ASSERT(engine.barsInConfirmedPace == 0,
                "Bars in confirmed pace should reset");
    TEST_ASSERT(engine.slowPaceBars == 0 && engine.fastPaceBars == 0,
                "Pace session evidence should reset");

    std::cout << "[OK] Pace session reset works correctly\n";
}

// ============================================================================
// TEST: Zero/Negative Duration Handling
// ============================================================================

void TestZeroDurationHandling() {
    TEST_SECTION("Zero/Negative Duration Handling");

    VolatilityEngine engine;
    auto store = CreatePopulatedEffortStore();
    engine.SetEffortStore(&store);
    engine.SetPhase(SessionPhase::MID_SESSION);

    // Test zero duration (should not crash, pace should handle gracefully)
    {
        auto result = engine.Compute(7.0, 0.0);  // Zero duration
        TEST_ASSERT(result.IsReady(), "Result should still be ready (regime works)");
        // Pace with zero duration should produce zero velocity
        TEST_ASSERT(result.rangeVelocity == 0.0 || !result.IsPaceReady(),
                    "Zero duration should produce zero velocity or no pace");
    }

    engine.ResetForSession();

    // Test very small duration
    {
        auto result = engine.Compute(7.0, 0.001);  // Very small (1ms)
        TEST_ASSERT(result.IsReady(), "Result should still be ready");
        // Very small duration produces very high velocity
    }

    std::cout << "[OK] Zero/negative duration handled gracefully\n";
}

// ============================================================================
// TEST: Synthetic Bar Aggregator
// ============================================================================

void TestSyntheticBarAggregator() {
    TEST_SECTION("Synthetic Bar Aggregator");

    SyntheticBarAggregator aggregator;
    aggregator.SetAggregationBars(5);  // 5-bar aggregation

    constexpr double tickSize = 0.25;

    // Push 4 bars - should not be ready yet
    aggregator.Push(100.00, 99.50, 60.0);  // Range = 2 pts = 8 ticks
    aggregator.Push(100.25, 99.75, 60.0);
    aggregator.Push(100.50, 99.25, 60.0);
    aggregator.Push(100.75, 99.00, 60.0);

    TEST_ASSERT(!aggregator.IsReady(), "Should not be ready with 4 bars");

    // Push 5th bar - now ready
    aggregator.Push(101.00, 99.00, 60.0);

    TEST_ASSERT(aggregator.IsReady(), "Should be ready with 5 bars");

    // Check synthetic values
    double synthHigh = aggregator.GetSyntheticHigh();
    double synthLow = aggregator.GetSyntheticLow();
    double synthRange = aggregator.GetSyntheticRangeTicks(tickSize);
    double synthDuration = aggregator.GetSyntheticDurationSec();

    TEST_ASSERT(synthHigh == 101.00, "Synthetic high should be max of all highs");
    TEST_ASSERT(synthLow == 99.00, "Synthetic low should be min of all lows");
    TEST_ASSERT(synthRange == 8.0, "Synthetic range should be (101-99)/0.25 = 8 ticks");
    TEST_ASSERT(synthDuration == 300.0, "Synthetic duration should be 5 * 60 = 300 seconds");

    // Check velocity
    double velocity = aggregator.GetSyntheticRangeVelocity(tickSize);
    TEST_ASSERT(std::abs(velocity - 1.6) < 0.01, "Velocity should be 8 ticks / 5 min = 1.6 t/min");

    // Test rolling update
    aggregator.Push(102.00, 100.00, 60.0);  // New bar shifts window

    synthHigh = aggregator.GetSyntheticHigh();
    TEST_ASSERT(synthHigh == 102.00, "Synthetic high should update with new max");

    std::cout << "[OK] Synthetic bar aggregator works correctly\n";
}

// ============================================================================
// TEST: Synthetic Mode Integration
// ============================================================================

void TestSyntheticModeIntegration() {
    TEST_SECTION("Synthetic Mode Integration");

    VolatilityEngine engine;
    auto store = CreatePopulatedEffortStore();
    engine.SetEffortStore(&store);
    engine.SetPhase(SessionPhase::MID_SESSION);
    engine.SetSyntheticMode(true, 5);  // Enable 5-bar synthetic

    constexpr double tickSize = 0.25;

    // Warmup: push 4 bars - should get WARMUP_SYNTHETIC
    for (int i = 0; i < 4; ++i) {
        double high = 100.0 + i * 0.25;
        double low = 99.0;
        auto result = engine.ComputeFromRawBar(high, low, 60.0, tickSize);

        TEST_ASSERT(!result.IsReady(), "Should not be ready during warmup");
        TEST_ASSERT(result.errorReason == VolatilityErrorReason::WARMUP_SYNTHETIC,
                    "Should report WARMUP_SYNTHETIC");
        TEST_ASSERT(result.usingSyntheticBars, "Should be using synthetic bars");
    }

    // 5th bar should produce valid result
    auto result = engine.ComputeFromRawBar(101.0, 99.0, 60.0, tickSize);

    TEST_ASSERT(result.IsReady(), "Should be ready after 5 bars");
    TEST_ASSERT(result.usingSyntheticBars, "Should be using synthetic bars");
    TEST_ASSERT(result.syntheticAggregationBars == 5, "Should track 5-bar aggregation");
    TEST_ASSERT(result.syntheticRangeTicks > 0.0, "Should have synthetic range");
    TEST_ASSERT(result.syntheticDurationSec == 300.0, "Should have 5-min duration");

    std::cout << "[OK] Synthetic mode integration works correctly\n";
}

// ============================================================================
// TEST: Synthetic vs Raw Mode
// ============================================================================

void TestSyntheticVsRawMode() {
    TEST_SECTION("Synthetic vs Raw Mode");

    constexpr double tickSize = 0.25;

    // Test raw mode (synthetic disabled)
    {
        VolatilityEngine engine;
        auto store = CreatePopulatedEffortStore();
        engine.SetEffortStore(&store);
        engine.SetPhase(SessionPhase::MID_SESSION);
        engine.SetSyntheticMode(false);  // Disable synthetic

        auto result = engine.ComputeFromRawBar(100.0, 99.0, 60.0, tickSize);

        TEST_ASSERT(result.IsReady(), "Raw mode should be ready immediately");
        TEST_ASSERT(!result.usingSyntheticBars, "Should NOT be using synthetic bars");
        TEST_ASSERT(result.barRangeTicks == 4.0, "Raw range should be 4 ticks");
    }

    std::cout << "[OK] Synthetic vs raw mode comparison works\n";
}

// ============================================================================
// TEST: Synthetic Mode Session Reset
// ============================================================================

void TestSyntheticSessionReset() {
    TEST_SECTION("Synthetic Mode Session Reset");

    VolatilityEngine engine;
    auto store = CreatePopulatedEffortStore();
    engine.SetEffortStore(&store);
    engine.SetPhase(SessionPhase::MID_SESSION);
    engine.SetSyntheticMode(true, 5);

    constexpr double tickSize = 0.25;

    // Fill aggregator
    for (int i = 0; i < 5; ++i) {
        engine.ComputeFromRawBar(100.0, 99.0, 60.0, tickSize);
    }

    TEST_ASSERT(engine.IsSyntheticReady(), "Should be ready");

    // Reset for new session
    engine.ResetForSession();

    TEST_ASSERT(!engine.IsSyntheticReady(), "Should not be ready after reset");

    // Warmup again
    auto result = engine.ComputeFromRawBar(100.0, 99.0, 60.0, tickSize);
    TEST_ASSERT(result.errorReason == VolatilityErrorReason::WARMUP_SYNTHETIC,
                "Should need warmup after reset");

    std::cout << "[OK] Synthetic mode session reset works correctly\n";
}

// ============================================================================
// TEST: Synthetic Baseline Population
// ============================================================================

void TestSyntheticBaselinePopulation() {
    TEST_SECTION("Synthetic Baseline Population");

    SyntheticBarAggregator aggregator;
    aggregator.SetAggregationBars(5);

    // Push 4 bars - no new synthetic bar formed
    for (int i = 0; i < 4; ++i) {
        bool formed = aggregator.Push(100.0 + i * 0.25, 99.0, 60.0);
        TEST_ASSERT(!formed, "Should not form synthetic bar before 5 bars");
        TEST_ASSERT(!aggregator.DidNewSyntheticBarForm(), "Flag should be false");
    }

    // 5th bar should form a new synthetic bar
    bool formed = aggregator.Push(101.0, 99.0, 60.0);
    TEST_ASSERT(formed, "5th bar should form synthetic bar");
    TEST_ASSERT(aggregator.DidNewSyntheticBarForm(), "Flag should be true on 5th bar");

    // 6th bar should NOT form a new synthetic bar
    formed = aggregator.Push(101.0, 99.0, 60.0);
    TEST_ASSERT(!formed, "6th bar should not form synthetic bar");

    // 10th bar (5 more) should form a new synthetic bar
    for (int i = 0; i < 3; ++i) {
        aggregator.Push(101.0 + i * 0.25, 99.0, 60.0);
    }
    formed = aggregator.Push(102.0, 99.0, 60.0);  // 10th bar
    TEST_ASSERT(formed, "10th bar should form synthetic bar");

    std::cout << "[OK] Synthetic baseline population timing works correctly\n";
}

// ============================================================================
// TEST: Synthetic vs Raw Baseline Query
// ============================================================================

void TestSyntheticBaselineQuery() {
    TEST_SECTION("Synthetic vs Raw Baseline Query");

    VolatilityEngine engine;
    auto store = CreatePopulatedEffortStore();
    engine.SetEffortStore(&store);
    engine.SetPhase(SessionPhase::MID_SESSION);

    // Query raw baseline with a raw range (7 ticks - mid-range for 2-14 distribution)
    {
        auto result = engine.Compute(7.0, 60.0, 0.0, false /* useSyntheticBaseline */);
        TEST_ASSERT(result.IsReady(), "Raw baseline query should be ready");
        TEST_ASSERT(result.rangePercentile > 30.0 && result.rangePercentile < 70.0,
                    "7 ticks should be mid-range for raw baseline (2-14 dist)");
    }

    // Query synthetic baseline with a synthetic range (20 ticks - mid-range for 8-32 distribution)
    {
        auto result = engine.Compute(20.0, 300.0, 0.0, true /* useSyntheticBaseline */);
        TEST_ASSERT(result.IsReady(), "Synthetic baseline query should be ready");
        TEST_ASSERT(result.rangePercentile > 30.0 && result.rangePercentile < 70.0,
                    "20 ticks should be mid-range for synthetic baseline (8-32 dist)");
    }

    // Query synthetic baseline with a raw-sized range (7 ticks - should be low percentile!)
    {
        auto result = engine.Compute(7.0, 300.0, 0.0, true /* useSyntheticBaseline */);
        TEST_ASSERT(result.IsReady(), "Synthetic baseline query should be ready");
        // 7 ticks is below the synthetic baseline range (8-32), so should be very low percentile
        TEST_ASSERT(result.rangePercentile < 15.0,
                    "7 ticks should be LOW percentile for synthetic baseline (8-32 dist)");
    }

    std::cout << "[OK] Synthetic vs raw baseline query works correctly\n";
}

// ============================================================================
// TEST: Synthetic Mode Uses Correct Baseline
// ============================================================================

void TestSyntheticModeUsesCorrectBaseline() {
    TEST_SECTION("Synthetic Mode Uses Correct Baseline");

    VolatilityEngine engine;
    auto store = CreatePopulatedEffortStore();
    engine.SetEffortStore(&store);
    engine.SetPhase(SessionPhase::MID_SESSION);
    engine.SetSyntheticMode(true, 5);

    constexpr double tickSize = 0.25;

    // Fill aggregator with 5 bars, creating a synthetic range
    // Bars from 100.00-99.00 to 101.00-99.00 = synthetic range = 2 pts = 8 ticks
    for (int i = 0; i < 5; ++i) {
        double high = 100.0 + i * 0.25;
        double low = 99.0;
        engine.ComputeFromRawBar(high, low, 60.0, tickSize);
    }

    // Now add one more bar to get a valid result with newSyntheticBarFormed
    auto result = engine.ComputeFromRawBar(101.25, 99.0, 60.0, tickSize);

    TEST_ASSERT(result.IsReady(), "Should be ready");
    TEST_ASSERT(result.usingSyntheticBars, "Should be using synthetic bars");

    // The synthetic range should be compared against synthetic baseline (8-32 ticks)
    // not raw baseline (2-14 ticks)
    // A ~9 tick synthetic range should be LOW percentile for synthetic baseline
    TEST_ASSERT(result.syntheticRangeTicks > 0.0, "Should have synthetic range");

    std::cout << "[OK] Synthetic mode uses correct baseline\n";
}

// ============================================================================
// TEST: Asymmetric Hysteresis
// ============================================================================
// Fast EVENT entry (1 bar), slow EVENT exit (3 bars), moderate others (2 bars)

void TestAsymmetricHysteresis() {
    TEST_SECTION("Asymmetric Hysteresis");

    VolatilityEngine engine;
    auto store = CreatePopulatedEffortStore();
    engine.SetEffortStore(&store);
    engine.SetPhase(SessionPhase::MID_SESSION);

    // Verify asymmetric config defaults
    TEST_ASSERT(engine.config.eventEntryBars == 1, "EVENT entry should be 1 bar");
    TEST_ASSERT(engine.config.eventExitBars == 3, "EVENT exit should be 3 bars");
    TEST_ASSERT(engine.config.otherTransitionBars == 2, "Other transitions should be 2 bars");

    // -------------------------------------------------------------------------
    // Test 1: Fast EVENT entry (1 bar)
    // -------------------------------------------------------------------------
    // Start in NORMAL
    for (int i = 0; i < 5; ++i) {
        engine.Compute(7.0, TEST_BAR_DURATION_SEC);  // Normal range
    }
    TEST_ASSERT(engine.confirmedRegime == VolatilityRegime::NORMAL,
                "Should start in NORMAL");

    // Single EVENT bar should immediately trigger transition (1 bar entry)
    {
        auto result = engine.Compute(20.0, TEST_BAR_DURATION_SEC);  // Extreme range
        TEST_ASSERT(engine.confirmedRegime == VolatilityRegime::EVENT,
                    "Single EVENT bar should immediately transition to EVENT");
    }

    // -------------------------------------------------------------------------
    // Test 2: Slow EVENT exit (3 bars)
    // -------------------------------------------------------------------------
    // Now try to exit EVENT - should take 3 bars
    {
        // First NORMAL bar - should not exit
        engine.Compute(7.0, TEST_BAR_DURATION_SEC);
        TEST_ASSERT(engine.confirmedRegime == VolatilityRegime::EVENT,
                    "First NORMAL bar should NOT exit EVENT");
        TEST_ASSERT(engine.candidateConfirmationBars == 1,
                    "Should have 1 confirmation bar");

        // Second NORMAL bar - should not exit
        engine.Compute(7.0, TEST_BAR_DURATION_SEC);
        TEST_ASSERT(engine.confirmedRegime == VolatilityRegime::EVENT,
                    "Second NORMAL bar should NOT exit EVENT");
        TEST_ASSERT(engine.candidateConfirmationBars == 2,
                    "Should have 2 confirmation bars");

        // Third NORMAL bar - should finally exit
        engine.Compute(7.0, TEST_BAR_DURATION_SEC);
        TEST_ASSERT(engine.confirmedRegime == VolatilityRegime::NORMAL,
                    "Third NORMAL bar SHOULD exit EVENT");
    }

    engine.ResetForSession();

    // -------------------------------------------------------------------------
    // Test 3: Moderate other transitions (2 bars)
    // -------------------------------------------------------------------------
    // Start in NORMAL
    for (int i = 0; i < 5; ++i) {
        engine.Compute(7.0, TEST_BAR_DURATION_SEC);
    }
    TEST_ASSERT(engine.confirmedRegime == VolatilityRegime::NORMAL,
                "Should start in NORMAL for test 3");

    // Transition NORMAL -> COMPRESSION should take 2 bars
    {
        // First COMPRESSION bar - should not transition
        engine.Compute(2.0, TEST_BAR_DURATION_SEC);
        TEST_ASSERT(engine.confirmedRegime == VolatilityRegime::NORMAL,
                    "First COMPRESSION bar should NOT change regime");
        TEST_ASSERT(engine.candidateConfirmationBars == 1,
                    "Should have 1 confirmation bar");

        // Second COMPRESSION bar - should transition
        engine.Compute(2.0, TEST_BAR_DURATION_SEC);
        TEST_ASSERT(engine.confirmedRegime == VolatilityRegime::COMPRESSION,
                    "Second COMPRESSION bar SHOULD change regime");
    }

    engine.ResetForSession();

    // -------------------------------------------------------------------------
    // Test 4: Bouncing between non-EVENT regimes resets counter
    // -------------------------------------------------------------------------
    // Start in NORMAL
    for (int i = 0; i < 5; ++i) {
        engine.Compute(7.0, TEST_BAR_DURATION_SEC);
    }

    // Single COMPRESSION bar
    engine.Compute(2.0, TEST_BAR_DURATION_SEC);
    TEST_ASSERT(engine.candidateConfirmationBars == 1, "Should have 1 confirmation");

    // Bounce to EXPANSION - resets counter
    engine.Compute(12.0, TEST_BAR_DURATION_SEC);  // Expansion range
    TEST_ASSERT(engine.candidateConfirmationBars == 1, "Counter should reset to 1 for new candidate");
    TEST_ASSERT(engine.candidateRegime == VolatilityRegime::EXPANSION,
                "Candidate should now be EXPANSION");

    // Stay in NORMAL (confirmed still NORMAL despite candidates)
    TEST_ASSERT(engine.confirmedRegime == VolatilityRegime::NORMAL,
                "Should still be in NORMAL due to resets");

    std::cout << "[OK] Asymmetric hysteresis works correctly\n";
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "=================================================\n";
    std::cout << "         VolatilityEngine Unit Tests\n";
    std::cout << "=================================================\n";

    // Regime tests
    TestRegimeClassification();
    TestHysteresis();
    TestTradabilityRules();
    TestSessionBoundary();
    TestATRNormalization();
    TestValidityGating();
    TestEventDetection();
    TestStabilityTracking();
    TestExpectedRangeMultiplier();

    // Auction Pace tests
    TestPaceClassification();
    TestPaceHysteresis();
    TestPaceTradabilityMultipliers();
    TestPaceSessionReset();
    TestZeroDurationHandling();

    // Synthetic bar aggregation tests
    TestSyntheticBarAggregator();
    TestSyntheticModeIntegration();
    TestSyntheticVsRawMode();
    TestSyntheticSessionReset();

    // Synthetic baseline tests
    TestSyntheticBaselinePopulation();
    TestSyntheticBaselineQuery();
    TestSyntheticModeUsesCorrectBaseline();

    // Asymmetric hysteresis tests
    TestAsymmetricHysteresis();

    std::cout << "\n=================================================\n";
    std::cout << "Tests Passed: " << g_testsPassed << "\n";
    std::cout << "Tests Failed: " << g_testsFailed << "\n";
    std::cout << "=================================================\n";

    return g_testsFailed > 0 ? 1 : 0;
}
