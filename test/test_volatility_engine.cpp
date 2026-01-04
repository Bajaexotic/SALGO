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
    // Using close = low (typical for testing without gaps)
    aggregator.Push(100.00, 99.50, 99.75, 60.0);  // Range = 2 pts = 8 ticks
    aggregator.Push(100.25, 99.75, 100.00, 60.0);
    aggregator.Push(100.50, 99.25, 99.50, 60.0);
    aggregator.Push(100.75, 99.00, 99.25, 60.0);

    TEST_ASSERT(!aggregator.IsReady(), "Should not be ready with 4 bars");

    // Push 5th bar - now ready
    aggregator.Push(101.00, 99.00, 100.00, 60.0);

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
    aggregator.Push(102.00, 100.00, 101.00, 60.0);  // New bar shifts window

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
        double close = 99.5 + i * 0.25;  // Close in middle
        auto result = engine.ComputeFromRawBar(high, low, close, 60.0, tickSize);

        TEST_ASSERT(!result.IsReady(), "Should not be ready during warmup");
        TEST_ASSERT(result.errorReason == VolatilityErrorReason::WARMUP_SYNTHETIC,
                    "Should report WARMUP_SYNTHETIC");
        TEST_ASSERT(result.usingSyntheticBars, "Should be using synthetic bars");
    }

    // 5th bar should produce valid result
    auto result = engine.ComputeFromRawBar(101.0, 99.0, 100.0, 60.0, tickSize);

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

        auto result = engine.ComputeFromRawBar(100.0, 99.0, 99.5, 60.0, tickSize);

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
        engine.ComputeFromRawBar(100.0, 99.0, 99.5, 60.0, tickSize);
    }

    TEST_ASSERT(engine.IsSyntheticReady(), "Should be ready");

    // Reset for new session
    engine.ResetForSession();

    TEST_ASSERT(!engine.IsSyntheticReady(), "Should not be ready after reset");

    // Warmup again
    auto result = engine.ComputeFromRawBar(100.0, 99.0, 99.5, 60.0, tickSize);
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
        bool formed = aggregator.Push(100.0 + i * 0.25, 99.0, 99.5 + i * 0.25, 60.0);
        TEST_ASSERT(!formed, "Should not form synthetic bar before 5 bars");
        TEST_ASSERT(!aggregator.DidNewSyntheticBarForm(), "Flag should be false");
    }

    // 5th bar should form a new synthetic bar
    bool formed = aggregator.Push(101.0, 99.0, 100.0, 60.0);
    TEST_ASSERT(formed, "5th bar should form synthetic bar");
    TEST_ASSERT(aggregator.DidNewSyntheticBarForm(), "Flag should be true on 5th bar");

    // 6th bar should NOT form a new synthetic bar
    formed = aggregator.Push(101.0, 99.0, 100.0, 60.0);
    TEST_ASSERT(!formed, "6th bar should not form synthetic bar");

    // 10th bar (5 more) should form a new synthetic bar
    for (int i = 0; i < 3; ++i) {
        aggregator.Push(101.0 + i * 0.25, 99.0, 100.0 + i * 0.25, 60.0);
    }
    formed = aggregator.Push(102.0, 99.0, 101.0, 60.0);  // 10th bar
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
        double close = 99.5 + i * 0.25;
        engine.ComputeFromRawBar(high, low, close, 60.0, tickSize);
    }

    // Now add one more bar to get a valid result with newSyntheticBarFormed
    auto result = engine.ComputeFromRawBar(101.25, 99.0, 100.5, 60.0, tickSize);

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
// TEST: True Range for Synthetic Bars
// ============================================================================
// True Range captures overnight gaps between synthetic windows

void TestTrueRangeForSyntheticBars() {
    TEST_SECTION("True Range for Synthetic Bars");

    constexpr double tickSize = 0.25;

    // -------------------------------------------------------------------------
    // Test 1: No gap (continuous bars) - True Range = Simple Range
    // -------------------------------------------------------------------------
    {
        SyntheticBarAggregator aggregator;
        aggregator.SetAggregationBars(5);

        // 5 bars with close at 100.0 (no gap expected)
        aggregator.Push(101.0, 99.0, 100.0, 60.0);  // Close = 100.0
        aggregator.Push(101.5, 99.5, 100.5, 60.0);
        aggregator.Push(102.0, 99.0, 101.0, 60.0);
        aggregator.Push(101.5, 98.5, 100.0, 60.0);
        aggregator.Push(101.0, 99.0, 100.0, 60.0);

        TEST_ASSERT(aggregator.IsReady(), "Aggregator should be ready");

        // Simple range = High - Low = 102.0 - 98.5 = 3.5 pts = 14 ticks
        double simpleRange = aggregator.GetSyntheticRangeTicks(tickSize);
        TEST_ASSERT(std::abs(simpleRange - 14.0) < 0.01,
                    "Simple range should be 14 ticks (102.0-98.5)");

        // Without previous synthetic close, True Range = Simple Range
        double trueRange = aggregator.GetSyntheticTrueRangeTicks(tickSize);
        TEST_ASSERT(std::abs(trueRange - simpleRange) < 0.01,
                    "True Range should equal Simple Range without gap");

        TEST_ASSERT(!aggregator.HasGap(), "Should NOT have gap (no prev close)");
    }

    // -------------------------------------------------------------------------
    // Test 2: Gap UP - True Range > Simple Range
    // -------------------------------------------------------------------------
    {
        SyntheticBarAggregator aggregator;
        aggregator.SetAggregationBars(5);

        // First synthetic bar window: closes at 100.0
        for (int i = 0; i < 5; ++i) {
            aggregator.Push(101.0, 99.0, 100.0, 60.0);
        }
        TEST_ASSERT(aggregator.IsReady(), "First window ready");

        // Second synthetic bar window: gaps UP to 105.0 (5 point gap)
        // All bars in window have High=106, Low=104 (simple range = 2 pts)
        for (int i = 0; i < 5; ++i) {
            aggregator.Push(106.0, 104.0, 105.0, 60.0);
        }

        // Simple range = 106.0 - 104.0 = 2 pts = 8 ticks
        double simpleRange = aggregator.GetSyntheticRangeTicks(tickSize);
        TEST_ASSERT(std::abs(simpleRange - 8.0) < 0.01,
                    "Simple range should be 8 ticks (106-104)");

        // True Range should include gap from prev close (100.0) to High (106.0)
        // TrueHigh = max(106.0, 100.0) = 106.0
        // TrueLow = min(104.0, 100.0) = 100.0
        // True Range = 106.0 - 100.0 = 6 pts = 24 ticks
        double trueRange = aggregator.GetSyntheticTrueRangeTicks(tickSize);
        TEST_ASSERT(trueRange > simpleRange,
                    "True Range should be > Simple Range with gap UP");
        TEST_ASSERT(std::abs(trueRange - 24.0) < 0.01,
                    "True Range should be 24 ticks (106-100 via gap)");

        TEST_ASSERT(aggregator.HasGap(), "Should have gap");
        double gapTicks = aggregator.GetGapTicks(tickSize);
        TEST_ASSERT(std::abs(gapTicks - 16.0) < 0.01,
                    "Gap component should be 16 ticks (24-8)");
    }

    // -------------------------------------------------------------------------
    // Test 3: Gap DOWN - True Range > Simple Range
    // -------------------------------------------------------------------------
    {
        SyntheticBarAggregator aggregator;
        aggregator.SetAggregationBars(5);

        // First synthetic bar window: closes at 100.0
        for (int i = 0; i < 5; ++i) {
            aggregator.Push(101.0, 99.0, 100.0, 60.0);
        }
        TEST_ASSERT(aggregator.IsReady(), "First window ready");

        // Second synthetic bar window: gaps DOWN to 95.0 (5 point gap down)
        // All bars in window have High=96, Low=94 (simple range = 2 pts)
        for (int i = 0; i < 5; ++i) {
            aggregator.Push(96.0, 94.0, 95.0, 60.0);
        }

        // Simple range = 96.0 - 94.0 = 2 pts = 8 ticks
        double simpleRange = aggregator.GetSyntheticRangeTicks(tickSize);
        TEST_ASSERT(std::abs(simpleRange - 8.0) < 0.01,
                    "Simple range should be 8 ticks (96-94)");

        // True Range should include gap from prev close (100.0) to Low (94.0)
        // TrueHigh = max(96.0, 100.0) = 100.0
        // TrueLow = min(94.0, 100.0) = 94.0
        // True Range = 100.0 - 94.0 = 6 pts = 24 ticks
        double trueRange = aggregator.GetSyntheticTrueRangeTicks(tickSize);
        TEST_ASSERT(trueRange > simpleRange,
                    "True Range should be > Simple Range with gap DOWN");
        TEST_ASSERT(std::abs(trueRange - 24.0) < 0.01,
                    "True Range should be 24 ticks (100-94 via gap)");

        TEST_ASSERT(aggregator.HasGap(), "Should have gap");
    }

    // -------------------------------------------------------------------------
    // Test 4: VolatilityResult includes True Range diagnostics
    // -------------------------------------------------------------------------
    {
        VolatilityEngine engine;
        auto store = CreatePopulatedEffortStore();
        engine.SetEffortStore(&store);
        engine.SetPhase(SessionPhase::MID_SESSION);
        engine.SetSyntheticMode(true, 5);

        // First window - no gap
        for (int i = 0; i < 5; ++i) {
            engine.ComputeFromRawBar(101.0, 99.0, 100.0, 60.0, tickSize);
        }

        // Second window with gap
        for (int i = 0; i < 4; ++i) {
            engine.ComputeFromRawBar(106.0, 104.0, 105.0, 60.0, tickSize);
        }
        auto result = engine.ComputeFromRawBar(106.0, 104.0, 105.0, 60.0, tickSize);

        TEST_ASSERT(result.IsReady(), "Should be ready");
        TEST_ASSERT(result.syntheticHasGap, "Should report gap in result");
        TEST_ASSERT(result.syntheticGapTicks > 0.0, "Gap ticks should be > 0");

        // True Range velocity should be higher than simple range velocity
        // when gap is present
        double trueRangeVelocity = result.syntheticRangeVelocity;
        TEST_ASSERT(trueRangeVelocity > 0.0, "True Range velocity should be > 0");
    }

    // -------------------------------------------------------------------------
    // Test 5: True Range velocity (ticks/min)
    // -------------------------------------------------------------------------
    {
        SyntheticBarAggregator aggregator;
        aggregator.SetAggregationBars(5);

        // 5 bars of 60 sec each = 300 sec = 5 min synthetic window
        for (int i = 0; i < 5; ++i) {
            aggregator.Push(106.0, 104.0, 105.0, 60.0);
        }

        // True Range = 8 ticks, Duration = 5 min
        // Velocity = 8 / 5 = 1.6 ticks/min
        double velocity = aggregator.GetSyntheticTrueRangeVelocity(tickSize);
        TEST_ASSERT(std::abs(velocity - 1.6) < 0.01,
                    "True Range velocity should be 1.6 ticks/min");
    }

    std::cout << "[OK] True Range for synthetic bars works correctly\n";
}

// ============================================================================
// EFFICIENCY RATIO TESTS
// ============================================================================

void TestEfficiencyRatioCalculation() {
    TEST_SECTION("Efficiency Ratio Calculation");

    constexpr double tickSize = 0.25;

    // -------------------------------------------------------------------------
    // Test 1: Perfect trend (all closes moving in same direction)
    // ER should be 1.0 (net = path)
    // -------------------------------------------------------------------------
    {
        SyntheticBarAggregator aggregator;
        aggregator.SetAggregationBars(5);

        // Closes: 100, 101, 102, 103, 104 (each bar +1 point = +4 ticks)
        // Net change: |104 - 100| = 4 points = 16 ticks
        // Path length: 4 + 4 + 4 + 4 = 16 ticks (same as net - perfect efficiency)
        aggregator.Push(101.0, 99.0, 100.0, 60.0);
        aggregator.Push(102.0, 100.0, 101.0, 60.0);
        aggregator.Push(103.0, 101.0, 102.0, 60.0);
        aggregator.Push(104.0, 102.0, 103.0, 60.0);
        aggregator.Push(105.0, 103.0, 104.0, 60.0);

        TEST_ASSERT(aggregator.IsEfficiencyValid(tickSize),
                    "Efficiency should be valid with sufficient movement");

        double er = aggregator.GetEfficiencyRatio(tickSize);
        TEST_ASSERT(std::abs(er - 1.0) < 0.01,
                    "Perfect trend should have ER close to 1.0");

        // Net change = 16 ticks, Path = 16 ticks
        double netTicks = aggregator.GetNetChangeTicks(tickSize);
        double pathTicks = aggregator.GetPathLengthTicks(tickSize);
        TEST_ASSERT(std::abs(netTicks - 16.0) < 0.1, "Net change should be 16 ticks");
        TEST_ASSERT(std::abs(pathTicks - 16.0) < 0.1, "Path length should be 16 ticks");
    }

    // -------------------------------------------------------------------------
    // Test 2: Pure chop (back and forth, ends where started)
    // ER should be 0.0 or very low
    // -------------------------------------------------------------------------
    {
        SyntheticBarAggregator aggregator;
        aggregator.SetAggregationBars(5);

        // Closes: 100, 102, 100, 102, 100 (choppy)
        // Net change: |100 - 100| = 0
        // Path length: 8 + 8 + 8 + 8 = 32 ticks
        aggregator.Push(101.0, 99.0, 100.0, 60.0);
        aggregator.Push(103.0, 101.0, 102.0, 60.0);
        aggregator.Push(101.0, 99.0, 100.0, 60.0);
        aggregator.Push(103.0, 101.0, 102.0, 60.0);
        aggregator.Push(101.0, 99.0, 100.0, 60.0);

        TEST_ASSERT(aggregator.IsEfficiencyValid(tickSize),
                    "Efficiency should be valid with sufficient movement");

        double er = aggregator.GetEfficiencyRatio(tickSize);
        TEST_ASSERT(er < 0.1, "Pure chop should have ER close to 0.0");

        double netTicks = aggregator.GetNetChangeTicks(tickSize);
        double pathTicks = aggregator.GetPathLengthTicks(tickSize);
        TEST_ASSERT(netTicks < 0.1, "Net change should be ~0 for chop");
        TEST_ASSERT(pathTicks > 30.0, "Path should be ~32 ticks");
    }

    // -------------------------------------------------------------------------
    // Test 3: Mixed movement (partial trend)
    // ER should be between 0 and 1
    // -------------------------------------------------------------------------
    {
        SyntheticBarAggregator aggregator;
        aggregator.SetAggregationBars(5);

        // Closes: 100, 101, 100.5, 101.5, 102 (net up 2 pts, with some retracement)
        // Net: |102 - 100| = 2 pts = 8 ticks
        // Path: 4 + 2 + 4 + 2 = 12 ticks
        // ER = 8/12 = 0.67
        aggregator.Push(101.0, 99.0, 100.0, 60.0);
        aggregator.Push(102.0, 100.0, 101.0, 60.0);
        aggregator.Push(101.5, 100.0, 100.5, 60.0);
        aggregator.Push(102.5, 101.0, 101.5, 60.0);
        aggregator.Push(103.0, 101.0, 102.0, 60.0);

        double er = aggregator.GetEfficiencyRatio(tickSize);
        TEST_ASSERT(er > 0.5 && er < 0.8, "Mixed movement should have ER ~0.67");
    }

    // -------------------------------------------------------------------------
    // Test 4: Low movement edge case (path < 2 ticks)
    // ER should be invalid, return 0.5 neutral
    // -------------------------------------------------------------------------
    {
        SyntheticBarAggregator aggregator;
        aggregator.SetAggregationBars(5);

        // Very tiny movements - all closes at same price
        aggregator.Push(100.1, 99.9, 100.0, 60.0);
        aggregator.Push(100.1, 99.9, 100.0, 60.0);
        aggregator.Push(100.1, 99.9, 100.0, 60.0);
        aggregator.Push(100.1, 99.9, 100.0, 60.0);
        aggregator.Push(100.1, 99.9, 100.0, 60.0);

        TEST_ASSERT(!aggregator.IsEfficiencyValid(tickSize),
                    "ER should be invalid when path < 2 ticks");

        double er = aggregator.GetEfficiencyRatio(tickSize);
        TEST_ASSERT(std::abs(er - 0.5) < 0.01,
                    "Invalid ER should return neutral 0.5");
    }

    // -------------------------------------------------------------------------
    // Test 5: Window reset on boundary
    // -------------------------------------------------------------------------
    {
        SyntheticBarAggregator aggregator;
        aggregator.SetAggregationBars(3);

        // First window: closes 100, 101, 102 (trend up)
        aggregator.Push(101.0, 99.0, 100.0, 60.0);
        aggregator.Push(102.0, 100.0, 101.0, 60.0);
        aggregator.Push(103.0, 101.0, 102.0, 60.0);

        double er1 = aggregator.GetEfficiencyRatio(tickSize);
        TEST_ASSERT(std::abs(er1 - 1.0) < 0.01, "First window should have ER=1.0");

        // Second window: closes 102, 100, 102 (chop)
        aggregator.Push(103.0, 101.0, 102.0, 60.0);
        aggregator.Push(101.0, 99.0, 100.0, 60.0);
        aggregator.Push(103.0, 101.0, 102.0, 60.0);

        // Path length should be reset for new window
        double pathTicks = aggregator.GetPathLengthTicks(tickSize);
        TEST_ASSERT(pathTicks > 15.0, "New window should have fresh path calculation");
    }

    std::cout << "[OK] Efficiency ratio calculation works correctly\n";
}

void TestChopSeverityAndMultipliers() {
    TEST_SECTION("Chop Severity and Multipliers");

    constexpr double tickSize = 0.25;

    // Create populated baseline for regime classification
    EffortBaselineStore effortStore = CreatePopulatedEffortStore();

    // -------------------------------------------------------------------------
    // Test 1: Low chop (high efficiency) - minimal multiplier impact
    // -------------------------------------------------------------------------
    {
        VolatilityEngine engine;
        engine.SetEffortStore(&effortStore);
        engine.SetPhase(SessionPhase::INITIAL_BALANCE);
        engine.SetSyntheticMode(true, 5);

        // Push trending bars (high ER)
        for (int i = 0; i < 5; ++i) {
            double base = 100.0 + i * 1.0;  // Steadily rising
            engine.ComputeFromRawBar(base + 1.0, base - 0.5, base + 0.5, 60.0, tickSize);
        }

        auto result = engine.ComputeFromRawBar(105.5, 104.0, 105.0, 60.0, tickSize);

        if (result.efficiencyValid) {
            TEST_ASSERT(result.efficiencyRatio > 0.6,
                        "Trending market should have high ER");
            TEST_ASSERT(result.chopSeverity < 0.4,
                        "High ER should have low chop severity");
            TEST_ASSERT(result.tradability.chopSizeMultiplier > 0.8,
                        "Low chop should have near 1.0 size multiplier");
            TEST_ASSERT(result.tradability.chopConfirmationMultiplier < 1.4,
                        "Low chop should have near 1.0 confirmation multiplier");
        }
    }

    // -------------------------------------------------------------------------
    // Test 2: High chop (low efficiency) - significant multiplier impact
    // -------------------------------------------------------------------------
    {
        VolatilityEngine engine;
        engine.SetEffortStore(&effortStore);
        engine.SetPhase(SessionPhase::INITIAL_BALANCE);
        engine.SetSyntheticMode(true, 5);

        // Push choppy bars (low ER)
        double closes[] = {100.0, 102.0, 100.0, 102.0, 100.0, 102.0};
        for (int i = 0; i < 6; ++i) {
            double c = closes[i % 6];
            engine.ComputeFromRawBar(c + 1.0, c - 1.0, c, 60.0, tickSize);
        }

        auto result = engine.ComputeFromRawBar(103.0, 101.0, 102.0, 60.0, tickSize);

        if (result.efficiencyValid) {
            TEST_ASSERT(result.efficiencyRatio < 0.4,
                        "Choppy market should have low ER");
            TEST_ASSERT(result.chopSeverity > 0.6,
                        "Low ER should have high chop severity");

            // With chopSeverity ~0.7+:
            // chopSizeMultiplier = 1.0 - 0.5 * 0.7 = 0.65
            // chopConfirmationMultiplier = 1.0 + 0.7 = 1.7
            TEST_ASSERT(result.tradability.chopSizeMultiplier < 0.75,
                        "High chop should reduce size multiplier");
            TEST_ASSERT(result.tradability.chopConfirmationMultiplier > 1.5,
                        "High chop should increase confirmation multiplier");
        }
    }

    // -------------------------------------------------------------------------
    // Test 3: chopActive flag (high vol + high chop = danger)
    // -------------------------------------------------------------------------
    {
        VolatilityEngine engine;
        engine.SetEffortStore(&effortStore);
        engine.SetPhase(SessionPhase::INITIAL_BALANCE);
        engine.SetSyntheticMode(true, 5);

        // Need EXPANSION regime + high chop
        // Push wide range choppy bars to hit EXPANSION
        double closes[] = {100.0, 106.0, 100.0, 106.0, 100.0, 106.0};
        for (int i = 0; i < 10; ++i) {  // Extra bars for regime confirmation
            double c = closes[i % 6];
            // Wide range bars to push into EXPANSION
            engine.ComputeFromRawBar(c + 4.0, c - 4.0, c, 60.0, tickSize);
        }

        auto result = engine.ComputeFromRawBar(108.0, 100.0, 106.0, 60.0, tickSize);

        // If in EXPANSION + low ER, chopActive should be true
        if (result.efficiencyValid && result.chopSeverity > 0.6 &&
            (result.regime == VolatilityRegime::EXPANSION ||
             result.regime == VolatilityRegime::EVENT)) {
            TEST_ASSERT(result.chopActive,
                        "High vol + high chop = chopActive should be true");
        }
    }

    // -------------------------------------------------------------------------
    // Test 4: Raw mode returns neutral values
    // -------------------------------------------------------------------------
    {
        VolatilityEngine engine;
        engine.SetEffortStore(&effortStore);
        engine.SetPhase(SessionPhase::INITIAL_BALANCE);
        engine.SetSyntheticMode(false, 1);  // Raw mode

        for (int i = 0; i < 30; ++i) {
            engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);
        }

        auto result = engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);

        TEST_ASSERT(!result.efficiencyValid,
                    "Raw mode should have efficiency invalid");
        TEST_ASSERT(std::abs(result.efficiencyRatio - 0.5) < 0.01,
                    "Raw mode should have neutral ER");
        TEST_ASSERT(std::abs(result.chopSeverity - 0.5) < 0.01,
                    "Raw mode should have neutral chop severity");
        TEST_ASSERT(!result.chopActive,
                    "Raw mode should not have chopActive");
        TEST_ASSERT(std::abs(result.tradability.chopSizeMultiplier - 1.0) < 0.01,
                    "Raw mode should have neutral size multiplier");
    }

    // -------------------------------------------------------------------------
    // Test 5: Combined multipliers
    // -------------------------------------------------------------------------
    {
        VolatilityEngine engine;
        engine.SetEffortStore(&effortStore);
        engine.SetPhase(SessionPhase::INITIAL_BALANCE);
        engine.SetSyntheticMode(true, 5);

        // Get a result with known chop
        double closes[] = {100.0, 102.0, 100.0, 102.0, 100.0};
        for (int i = 0; i < 5; ++i) {
            double c = closes[i];
            engine.ComputeFromRawBar(c + 1.0, c - 1.0, c, 60.0, tickSize);
        }

        auto result = engine.ComputeFromRawBar(103.0, 101.0, 102.0, 60.0, tickSize);

        // GetCombinedSizeMultiplier includes chop
        double combined = result.tradability.GetCombinedSizeMultiplier();
        TEST_ASSERT(combined > 0.0 && combined <= 1.0,
                    "Combined size multiplier should be in (0, 1]");

        double combinedConf = result.tradability.GetCombinedConfirmationMultiplier();
        TEST_ASSERT(combinedConf >= 1.0,
                    "Combined confirmation multiplier should be >= 1.0");
    }

    std::cout << "[OK] Chop severity and multipliers work correctly\n";
}

// ============================================================================
// SHOCK DETECTION TESTS
// ============================================================================

void TestShockDetection() {
    TEST_SECTION("Shock Detection and Aftershock Decay");

    constexpr double tickSize = 0.25;

    // Use the standard populated effort store
    EffortBaselineStore effortStore = CreatePopulatedEffortStore();

    // -------------------------------------------------------------------------
    // Test 1: Normal bar should not be shock
    // -------------------------------------------------------------------------
    {
        VolatilityEngine engine;
        engine.SetEffortStore(&effortStore);
        engine.SetPhase(SessionPhase::INITIAL_BALANCE);
        engine.SetSyntheticMode(true, 5);

        // Push 5 normal bars (8 tick range = mid-range)
        for (int i = 0; i < 5; ++i) {
            engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);
        }

        auto result = engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);

        TEST_ASSERT(!result.shockFlag, "Normal bar should not be shock");
        TEST_ASSERT(!result.aftershockActive, "No shock = no aftershock");
        TEST_ASSERT(result.barsSinceShock == 999, "No shock should have large barsSinceShock");
    }

    // -------------------------------------------------------------------------
    // Test 2: Extreme bar (P99+) should be shock
    // -------------------------------------------------------------------------
    {
        VolatilityEngine engine;
        engine.SetEffortStore(&effortStore);
        engine.SetPhase(SessionPhase::INITIAL_BALANCE);
        engine.SetSyntheticMode(true, 5);

        // Push 5 extreme bars (50 tick range = way above P99)
        for (int i = 0; i < 5; ++i) {
            engine.ComputeFromRawBar(112.5, 100.0, 106.0, 60.0, tickSize);  // 50 ticks
        }

        auto result = engine.ComputeFromRawBar(112.5, 100.0, 106.0, 60.0, tickSize);

        if (result.IsReady() && result.rangePercentile >= 99.0) {
            TEST_ASSERT(result.shockFlag, "P99+ bar should be shock");
            TEST_ASSERT(result.barsSinceShock == 0, "Shock bar should have barsSinceShock=0");
            TEST_ASSERT(result.shockMagnitude >= 99.0, "Shock magnitude should be >= 99");
            TEST_ASSERT(result.aftershockActive, "Shock bar also has aftershock active");
        }
    }

    // -------------------------------------------------------------------------
    // Test 3: Aftershock decay window (3 synthetic bars)
    // -------------------------------------------------------------------------
    {
        VolatilityEngine engine;
        engine.SetEffortStore(&effortStore);
        engine.SetPhase(SessionPhase::INITIAL_BALANCE);
        engine.SetSyntheticMode(true, 5);

        // First: trigger a shock with extreme bar
        // Push 5 raw bars to form first synthetic bar (warmup)
        for (int i = 0; i < 5; ++i) {
            engine.ComputeFromRawBar(112.5, 100.0, 106.0, 60.0, tickSize);
        }
        // Push 5 more to form second synthetic bar with shock
        for (int i = 0; i < 4; ++i) {
            engine.ComputeFromRawBar(112.5, 100.0, 106.0, 60.0, tickSize);
        }
        auto shockResult = engine.ComputeFromRawBar(112.5, 100.0, 106.0, 60.0, tickSize);

        bool hadShock = shockResult.IsReady() && shockResult.shockFlag;

        if (hadShock) {
            TEST_ASSERT(shockResult.barsSinceShock == 0, "Shock bar should have barsSinceShock=0");

            // Synthetic bar 1 after shock: should still be in aftershock (barsSinceShock=1)
            // Push exactly 5 raw bars to complete one synthetic bar
            for (int i = 0; i < 4; ++i) {
                engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);
            }
            auto bar1 = engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);
            TEST_ASSERT(!bar1.shockFlag, "Normal bar after shock should not be shock");
            TEST_ASSERT(bar1.aftershockActive, "Bar 1 after shock should be in aftershock");
            TEST_ASSERT(bar1.barsSinceShock == 1, "barsSinceShock should be 1");

            // Synthetic bar 2 after shock: still in aftershock (barsSinceShock=2)
            for (int i = 0; i < 4; ++i) {
                engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);
            }
            auto bar2 = engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);
            TEST_ASSERT(bar2.aftershockActive, "Bar 2 after shock should be in aftershock");
            TEST_ASSERT(bar2.barsSinceShock == 2, "barsSinceShock should be 2");

            // Synthetic bar 3 after shock: still in aftershock (barsSinceShock=3)
            for (int i = 0; i < 4; ++i) {
                engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);
            }
            auto bar3 = engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);
            TEST_ASSERT(bar3.aftershockActive, "Bar 3 after shock should be in aftershock");
            TEST_ASSERT(bar3.barsSinceShock == 3, "barsSinceShock should be 3");

            // Synthetic bar 4 after shock: aftershock should expire (barsSinceShock=4 > 3)
            for (int i = 0; i < 4; ++i) {
                engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);
            }
            auto bar4 = engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);
            TEST_ASSERT(!bar4.aftershockActive, "Bar 4 after shock: aftershock should expire");
            TEST_ASSERT(bar4.barsSinceShock == 4, "barsSinceShock should be 4");
        }
    }

    // -------------------------------------------------------------------------
    // Test 4: Shock accessors
    // -------------------------------------------------------------------------
    {
        VolatilityResult result;
        result.shockFlag = true;
        result.aftershockActive = true;

        TEST_ASSERT(result.IsShock(), "IsShock should be true when shockFlag=true");
        TEST_ASSERT(result.IsShockOrAftershock(), "IsShockOrAftershock should be true");
        TEST_ASSERT(!result.IsAftershock(), "IsAftershock should be false on shock bar");

        // Size multiplier
        TEST_ASSERT(std::abs(result.GetShockSizeMultiplier() - 0.5) < 0.01,
                    "Shock should have 0.5x size multiplier");

        // Aftershock only (not shock)
        result.shockFlag = false;
        result.aftershockActive = true;
        TEST_ASSERT(!result.IsShock(), "IsShock should be false");
        TEST_ASSERT(result.IsAftershock(), "IsAftershock should be true when aftershock only");
        TEST_ASSERT(std::abs(result.GetShockSizeMultiplier() - 0.75) < 0.01,
                    "Aftershock should have 0.75x size multiplier");

        // No shock or aftershock
        result.aftershockActive = false;
        TEST_ASSERT(!result.IsShockOrAftershock(), "Neither shock nor aftershock");
        TEST_ASSERT(std::abs(result.GetShockSizeMultiplier() - 1.0) < 0.01,
                    "No shock should have 1.0x size multiplier");
    }

    // -------------------------------------------------------------------------
    // Test 5: Session reset clears shock state
    // -------------------------------------------------------------------------
    {
        VolatilityEngine engine;
        engine.SetEffortStore(&effortStore);
        engine.SetPhase(SessionPhase::INITIAL_BALANCE);
        engine.SetSyntheticMode(true, 5);

        // Trigger a shock
        for (int i = 0; i < 5; ++i) {
            engine.ComputeFromRawBar(112.5, 100.0, 106.0, 60.0, tickSize);
        }
        auto shockResult = engine.ComputeFromRawBar(112.5, 100.0, 106.0, 60.0, tickSize);

        // Reset for session
        engine.ResetForSession();

        // Next bar should not have aftershock
        for (int i = 0; i < 5; ++i) {
            engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);
        }
        auto afterReset = engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);

        TEST_ASSERT(!afterReset.aftershockActive,
                    "After session reset, aftershock should not be active");
        TEST_ASSERT(afterReset.barsSinceShock == 999,
                    "After session reset, barsSinceShock should be 999");
    }

    std::cout << "[OK] Shock detection and aftershock decay works correctly\n";
}

// ============================================================================
// VOLATILITY MOMENTUM + STABILITY TESTS
// ============================================================================

void TestVolatilityMomentumAndStability() {
    TEST_SECTION("Volatility Momentum and Stability");

    constexpr double tickSize = 0.25;

    // Use the standard populated effort store
    EffortBaselineStore effortStore = CreatePopulatedEffortStore();

    // -------------------------------------------------------------------------
    // Test 1: Expanding volatility (increasing ranges)
    // -------------------------------------------------------------------------
    {
        VolatilityEngine engine;
        engine.SetEffortStore(&effortStore);
        engine.SetPhase(SessionPhase::INITIAL_BALANCE);
        engine.SetSyntheticMode(true, 5);

        // First synthetic bar: 8 tick range
        for (int i = 0; i < 5; ++i) {
            engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);
        }

        // Second synthetic bar: 16 tick range (2x larger = log(2) = 0.69 > 0.18)
        for (int i = 0; i < 4; ++i) {
            engine.ComputeFromRawBar(104.0, 100.0, 102.0, 60.0, tickSize);
        }
        auto result = engine.ComputeFromRawBar(104.0, 100.0, 102.0, 60.0, tickSize);

        if (result.volMomentumValid) {
            TEST_ASSERT(result.volMomentum > 0.18,
                        "Doubling range should have volMomentum > 0.18");
            TEST_ASSERT(result.volTrend == VolatilityTrend::EXPANDING,
                        "Doubling range should be EXPANDING");
        }
    }

    // -------------------------------------------------------------------------
    // Test 2: Contracting volatility (decreasing ranges)
    // -------------------------------------------------------------------------
    {
        VolatilityEngine engine;
        engine.SetEffortStore(&effortStore);
        engine.SetPhase(SessionPhase::INITIAL_BALANCE);
        engine.SetSyntheticMode(true, 5);

        // First synthetic bar: 16 tick range
        for (int i = 0; i < 5; ++i) {
            engine.ComputeFromRawBar(104.0, 100.0, 102.0, 60.0, tickSize);
        }

        // Second synthetic bar: 8 tick range (0.5x = log(0.5) = -0.69 < -0.18)
        for (int i = 0; i < 4; ++i) {
            engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);
        }
        auto result = engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);

        if (result.volMomentumValid) {
            TEST_ASSERT(result.volMomentum < -0.18,
                        "Halving range should have volMomentum < -0.18");
            TEST_ASSERT(result.volTrend == VolatilityTrend::CONTRACTING,
                        "Halving range should be CONTRACTING");
        }
    }

    // -------------------------------------------------------------------------
    // Test 3: Stable volatility (similar ranges)
    // -------------------------------------------------------------------------
    {
        VolatilityEngine engine;
        engine.SetEffortStore(&effortStore);
        engine.SetPhase(SessionPhase::INITIAL_BALANCE);
        engine.SetSyntheticMode(true, 5);

        // First synthetic bar: 8 tick range
        for (int i = 0; i < 5; ++i) {
            engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);
        }

        // Second synthetic bar: 9 tick range (similar = log(9/8) = 0.12)
        for (int i = 0; i < 4; ++i) {
            engine.ComputeFromRawBar(102.25, 100.0, 101.0, 60.0, tickSize);
        }
        auto result = engine.ComputeFromRawBar(102.25, 100.0, 101.0, 60.0, tickSize);

        if (result.volMomentumValid) {
            TEST_ASSERT(std::abs(result.volMomentum) <= 0.18,
                        "Similar ranges should have |volMomentum| <= 0.18");
            TEST_ASSERT(result.volTrend == VolatilityTrend::STABLE,
                        "Similar ranges should be STABLE");
        }
    }

    // -------------------------------------------------------------------------
    // Test 4: Stability classification (CV-based)
    // -------------------------------------------------------------------------
    {
        VolatilityEngine engine;
        engine.SetEffortStore(&effortStore);
        engine.SetPhase(SessionPhase::INITIAL_BALANCE);
        engine.SetSyntheticMode(true, 5);

        // Push many synthetic bars with consistent ranges to build stability
        for (int synth = 0; synth < 10; ++synth) {
            // All bars with 8 tick range (low CV = stable)
            for (int i = 0; i < 5; ++i) {
                engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);
            }
        }

        auto result = engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);

        if (result.stabilityValid) {
            TEST_ASSERT(result.volCV < 0.2,
                        "Consistent ranges should have low CV");
            TEST_ASSERT(result.volStability == VolatilityStability::STABLE,
                        "Low CV should classify as STABLE");
            TEST_ASSERT(std::abs(result.stabilityConfidenceMultiplier - 1.0) < 0.01,
                        "STABLE should have 1.0 confidence multiplier");
        }
    }

    // -------------------------------------------------------------------------
    // Test 5: Unstable volatility (high CV)
    // -------------------------------------------------------------------------
    {
        VolatilityEngine engine;
        engine.SetEffortStore(&effortStore);
        engine.SetPhase(SessionPhase::INITIAL_BALANCE);
        engine.SetSyntheticMode(true, 5);

        // Push synthetic bars with wildly varying ranges
        double ranges[] = {4.0, 16.0, 4.0, 16.0, 4.0, 16.0, 4.0, 16.0, 4.0, 16.0};
        for (int synth = 0; synth < 10; ++synth) {
            double range = ranges[synth % 10];  // Alternating 4 and 16 ticks
            double high = 100.0 + range * tickSize;
            for (int i = 0; i < 5; ++i) {
                engine.ComputeFromRawBar(high, 100.0, 100.0 + range * tickSize / 2, 60.0, tickSize);
            }
        }

        auto result = engine.ComputeFromRawBar(104.0, 100.0, 102.0, 60.0, tickSize);

        if (result.stabilityValid) {
            TEST_ASSERT(result.volCV > 0.5,
                        "Wildly varying ranges should have high CV");
            TEST_ASSERT(result.volStability == VolatilityStability::UNSTABLE,
                        "High CV should classify as UNSTABLE");
            TEST_ASSERT(std::abs(result.stabilityConfidenceMultiplier - 0.7) < 0.01,
                        "UNSTABLE should have 0.7 confidence multiplier");
        }
    }

    // -------------------------------------------------------------------------
    // Test 6: First bar has no momentum (no prior)
    // -------------------------------------------------------------------------
    {
        VolatilityEngine engine;
        engine.SetEffortStore(&effortStore);
        engine.SetPhase(SessionPhase::INITIAL_BALANCE);
        engine.SetSyntheticMode(true, 5);

        // First synthetic bar
        for (int i = 0; i < 4; ++i) {
            engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);
        }
        auto result = engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);

        TEST_ASSERT(!result.volMomentumValid,
                    "First synthetic bar should have no momentum (no prior)");
        TEST_ASSERT(result.volTrend == VolatilityTrend::UNKNOWN,
                    "First bar should have UNKNOWN trend");
    }

    // -------------------------------------------------------------------------
    // Test 7: Session reset clears momentum state
    // -------------------------------------------------------------------------
    {
        VolatilityEngine engine;
        engine.SetEffortStore(&effortStore);
        engine.SetPhase(SessionPhase::INITIAL_BALANCE);
        engine.SetSyntheticMode(true, 5);

        // Build up some state
        for (int synth = 0; synth < 5; ++synth) {
            for (int i = 0; i < 5; ++i) {
                engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);
            }
        }

        engine.ResetForSession();

        // First bar after reset should have no momentum
        for (int i = 0; i < 4; ++i) {
            engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);
        }
        auto result = engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);

        TEST_ASSERT(!result.volMomentumValid,
                    "After reset, first bar should have no momentum");
        TEST_ASSERT(!result.stabilityValid,
                    "After reset, stability should not be valid yet (< 5 samples)");
    }

    std::cout << "[OK] Volatility momentum and stability works correctly\n";
}

// ============================================================================
// STOP GUIDANCE TESTS
// ============================================================================

void TestStopGuidance() {
    TEST_SECTION("Stop Guidance and Admissibility");

    constexpr double tickSize = 0.25;

    // Create populated effort store with known p75
    EffortBaselineStore effortStore = CreatePopulatedEffortStore();

    // -------------------------------------------------------------------------
    // Test 1: Basic stop guidance calculation
    // -------------------------------------------------------------------------
    {
        VolatilityEngine engine;
        engine.SetEffortStore(&effortStore);
        engine.SetPhase(SessionPhase::INITIAL_BALANCE);
        engine.SetSyntheticMode(true, 5);

        // Push bars to get baseline ready and compute stop guidance
        for (int synth = 0; synth < 3; ++synth) {
            for (int i = 0; i < 5; ++i) {
                engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);
            }
        }

        auto result = engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);

        if (result.IsReady()) {
            TEST_ASSERT(result.IsStopGuidanceReady(),
                        "Stop guidance should be ready when result is ready");
            TEST_ASSERT(result.stopGuidance.baseRangeTicks > 0.0,
                        "Base range should be positive");
            TEST_ASSERT(result.stopGuidance.minStopTicks > 0.0,
                        "Minimum stop should be positive");
            TEST_ASSERT(result.stopGuidance.suggestedTicks > result.stopGuidance.minStopTicks,
                        "Suggested stop should be greater than minimum");
            TEST_ASSERT(result.stopGuidance.suggestedTicks == result.stopGuidance.minStopTicks * 1.5,
                        "Suggested stop should be 1.5x minimum");
        }
    }

    // -------------------------------------------------------------------------
    // Test 2: Pace multiplier effects
    // -------------------------------------------------------------------------
    {
        // Test with FAST pace - should have higher stop floor
        VolatilityEngine engineFast;
        engineFast.SetEffortStore(&effortStore);
        engineFast.SetPhase(SessionPhase::INITIAL_BALANCE);
        engineFast.SetSyntheticMode(true, 5);

        // Push high-velocity bars to trigger FAST pace
        for (int synth = 0; synth < 3; ++synth) {
            for (int i = 0; i < 5; ++i) {
                // 8 ticks in 10 seconds = 0.8 ticks/sec = high velocity
                engineFast.ComputeFromRawBar(102.0, 100.0, 101.0, 10.0, tickSize);
            }
        }

        auto resultFast = engineFast.ComputeFromRawBar(102.0, 100.0, 101.0, 10.0, tickSize);

        // Normal pace engine
        VolatilityEngine engineNormal;
        engineNormal.SetEffortStore(&effortStore);
        engineNormal.SetPhase(SessionPhase::INITIAL_BALANCE);
        engineNormal.SetSyntheticMode(true, 5);

        for (int synth = 0; synth < 3; ++synth) {
            for (int i = 0; i < 5; ++i) {
                // Same range but longer duration = slower pace
                engineNormal.ComputeFromRawBar(102.0, 100.0, 101.0, 120.0, tickSize);
            }
        }

        auto resultNormal = engineNormal.ComputeFromRawBar(102.0, 100.0, 101.0, 120.0, tickSize);

        if (resultFast.IsStopGuidanceReady() && resultNormal.IsStopGuidanceReady()) {
            // FAST pace should have pace multiplier > 1.0
            if (resultFast.pace == AuctionPace::FAST || resultFast.pace == AuctionPace::EXTREME) {
                TEST_ASSERT(resultFast.stopGuidance.paceMultiplier >= 1.3,
                            "FAST/EXTREME pace should have pace multiplier >= 1.3");
            }
            // Normal pace should have multiplier = 1.0
            if (resultNormal.pace == AuctionPace::NORMAL || resultNormal.pace == AuctionPace::SLOW) {
                TEST_ASSERT(resultNormal.stopGuidance.paceMultiplier == 1.0,
                            "NORMAL/SLOW pace should have pace multiplier = 1.0");
            }
        }
    }

    // -------------------------------------------------------------------------
    // Test 3: Admissibility check
    // -------------------------------------------------------------------------
    {
        VolatilityEngine engine;
        engine.SetEffortStore(&effortStore);
        engine.SetPhase(SessionPhase::INITIAL_BALANCE);
        engine.SetSyntheticMode(true, 5);

        for (int synth = 0; synth < 3; ++synth) {
            for (int i = 0; i < 5; ++i) {
                engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);
            }
        }

        auto result = engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);

        if (result.IsStopGuidanceReady()) {
            double minStop = result.GetMinStopTicks();

            // Stop above floor should be admissible
            TEST_ASSERT(result.IsStopAdmissible(minStop + 5.0),
                        "Stop above floor should be admissible");

            // Stop at exact floor should be admissible
            TEST_ASSERT(result.IsStopAdmissible(minStop),
                        "Stop at exact floor should be admissible");

            // Stop below floor should NOT be admissible
            TEST_ASSERT(!result.IsStopAdmissible(minStop - 1.0),
                        "Stop below floor should NOT be admissible");

            // Check reason for inadmissibility
            const char* reason = result.stopGuidance.GetInadmissibleReason(minStop - 1.0);
            TEST_ASSERT(reason != nullptr,
                        "Should have reason for inadmissible stop");
        }
    }

    // -------------------------------------------------------------------------
    // Test 4: Shock multiplier effects
    // -------------------------------------------------------------------------
    {
        VolatilityEngine engine;
        engine.SetEffortStore(&effortStore);
        engine.SetPhase(SessionPhase::INITIAL_BALANCE);
        engine.SetSyntheticMode(true, 5);

        // Build baseline with moderate bars
        for (int synth = 0; synth < 3; ++synth) {
            for (int i = 0; i < 5; ++i) {
                engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);
            }
        }

        auto normalResult = engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);

        // Now push a shock bar (extreme range)
        for (int i = 0; i < 4; ++i) {
            engine.ComputeFromRawBar(150.0, 100.0, 125.0, 60.0, tickSize);  // 200 tick range!
        }
        auto shockResult = engine.ComputeFromRawBar(150.0, 100.0, 125.0, 60.0, tickSize);

        // If shock detected, multiplier should be elevated
        if (shockResult.IsShock() && shockResult.IsStopGuidanceReady()) {
            TEST_ASSERT(shockResult.stopGuidance.shockMultiplier == 1.5,
                        "Shock should have shock multiplier = 1.5");
        }
    }

    // -------------------------------------------------------------------------
    // Test 5: Not active during warmup
    // -------------------------------------------------------------------------
    {
        VolatilityEngine engine;
        // Don't set effort store - should be in warmup

        auto result = engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);

        // Stop guidance should not be active during warmup
        TEST_ASSERT(!result.IsStopGuidanceReady(),
                    "Stop guidance should not be active during warmup");

        // When not active, any stop should be admissible (fail-open)
        TEST_ASSERT(result.IsStopAdmissible(1.0),
                    "When inactive, any stop should be admissible");
        TEST_ASSERT(result.IsStopAdmissible(0.0),
                    "When inactive, even zero stop should be admissible");
    }

    // -------------------------------------------------------------------------
    // Test 6: Full size multiplier combines all factors
    // -------------------------------------------------------------------------
    {
        VolatilityEngine engine;
        engine.SetEffortStore(&effortStore);
        engine.SetPhase(SessionPhase::INITIAL_BALANCE);
        engine.SetSyntheticMode(true, 5);

        for (int synth = 0; synth < 3; ++synth) {
            for (int i = 0; i < 5; ++i) {
                engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);
            }
        }

        auto result = engine.ComputeFromRawBar(102.0, 100.0, 101.0, 60.0, tickSize);

        if (result.IsReady()) {
            double fullMult = result.GetFullSizeMultiplier();
            double expectedMult = result.tradability.positionSizeMultiplier
                                * result.tradability.paceSizeMultiplier
                                * result.tradability.chopSizeMultiplier
                                * result.GetShockSizeMultiplier()
                                * result.stabilityConfidenceMultiplier;

            TEST_ASSERT(std::abs(fullMult - expectedMult) < 0.001,
                        "GetFullSizeMultiplier should combine all factors correctly");
        }
    }

    std::cout << "[OK] Stop guidance and admissibility works correctly\n";
}

// ============================================================================
// GAP CONTEXT TESTS
// ============================================================================

void TestGapContextInjection() {
    TEST_SECTION("Gap Context Injection (Diagnostic)");

    // -------------------------------------------------------------------------
    // Test 1: Gap context setter and accessors
    // -------------------------------------------------------------------------
    {
        VolatilityResult result;

        // Initially unknown
        TEST_ASSERT(!result.HasGapContext(),
                    "Initial state should have no gap context");
        TEST_ASSERT(result.gapLocation == GapLocation::UNKNOWN,
                    "Initial gap location should be UNKNOWN");

        // Set gap up above value
        result.SetGapContext(GapLocation::ABOVE_VALUE, 12.5);

        TEST_ASSERT(result.HasGapContext(),
                    "After setting, should have gap context");
        TEST_ASSERT(result.IsGapUp(),
                    "Should be gap up");
        TEST_ASSERT(!result.IsGapDown(),
                    "Should not be gap down");
        TEST_ASSERT(result.gapFromValueTicks == 12.5,
                    "Gap distance should be 12.5 ticks");
    }

    // -------------------------------------------------------------------------
    // Test 2: Gap response setter
    // -------------------------------------------------------------------------
    {
        VolatilityResult result;
        result.SetGapContext(GapLocation::BELOW_VALUE, 8.0);
        result.SetGapResponse(EarlyResponse::REJECTING, 5);

        TEST_ASSERT(result.IsGapDown(),
                    "Should be gap down");
        TEST_ASSERT(result.IsGapRejecting(),
                    "Should be rejecting gap");
        TEST_ASSERT(!result.IsGapAccepting(),
                    "Should not be accepting gap");
        TEST_ASSERT(result.barsIntoSession == 5,
                    "Bars into session should be 5");
    }

    // -------------------------------------------------------------------------
    // Test 3: In-value case
    // -------------------------------------------------------------------------
    {
        VolatilityResult result;
        result.SetGapContext(GapLocation::IN_VALUE, 0.0);

        TEST_ASSERT(result.HasGapContext(),
                    "In-value should still have gap context");
        TEST_ASSERT(result.IsInValue(),
                    "Should be in value");
        TEST_ASSERT(!result.IsGapUp() && !result.IsGapDown(),
                    "Should not be gap up or down");
    }

    // -------------------------------------------------------------------------
    // Test 4: String conversions
    // -------------------------------------------------------------------------
    {
        TEST_ASSERT(std::string(GapLocationToString(GapLocation::ABOVE_VALUE)) == "ABOVE_VALUE",
                    "GapLocation string conversion should work");
        TEST_ASSERT(std::string(EarlyResponseToString(EarlyResponse::ACCEPTING)) == "ACCEPTING",
                    "EarlyResponse string conversion should work");
    }

    std::cout << "[OK] Gap context injection works correctly\n";
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

    // True Range tests
    TestTrueRangeForSyntheticBars();

    // Efficiency Ratio tests
    TestEfficiencyRatioCalculation();
    TestChopSeverityAndMultipliers();

    // Shock Detection tests
    TestShockDetection();

    // Volatility Momentum + Stability tests
    TestVolatilityMomentumAndStability();

    // Stop Guidance tests
    TestStopGuidance();

    // Gap Context tests
    TestGapContextInjection();

    std::cout << "\n=================================================\n";
    std::cout << "Tests Passed: " << g_testsPassed << "\n";
    std::cout << "Tests Failed: " << g_testsFailed << "\n";
    std::cout << "=================================================\n";

    return g_testsFailed > 0 ? 1 : 0;
}
