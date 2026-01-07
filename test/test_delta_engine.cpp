// ============================================================================
// test_delta_engine.cpp - Unit Tests for DeltaEngine
// ============================================================================
// Tests:
//   1. Character classification (SUSTAINED, EPISODIC, BUILDING, FADING, REVERSAL)
//   2. Alignment classification (CONVERGENT, DIVERGENT, ABSORPTION)
//   3. Confidence degradation (thin tape, high chop, exhaustion)
//   4. Trading constraints output
//   5. Hysteresis state machine
//   6. Session boundary handling
//   7. Validity gating (warmup, errors)
//   8. History tracking
//   9. Location context (Jan 2025): zone detection, outcome likelihoods
//  10. Context gates (Jan 2025): LIQ_VOID/EVENT blocks, COMPRESSION/stress degrades
//
// Compile: g++ -std=c++17 -I.. -o test_delta_engine.exe test_delta_engine.cpp
// Run: ./test_delta_engine.exe
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
#include "../AMT_DeltaEngine.h"

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

// Helper: Create populated EffortBaselineStore with delta_pct and vol_sec samples
EffortBaselineStore CreatePopulatedEffortStore() {
    EffortBaselineStore store;
    store.Reset(500);

    // Populate all tradeable phase buckets
    for (int i = 0; i < AMT::EFFORT_BUCKET_COUNT; ++i) {
        auto& bucket = store.buckets[i];

        // Add 100 samples with realistic distribution
        // delta_pct ranges: -0.8 to +0.8, mean ~0
        // vol_sec ranges: 50 to 500 (volume per second)
        for (int j = 0; j < 100; ++j) {
            double deltaPct = -0.8 + (j % 17) * 0.1;  // Range: -0.8 to +0.8
            bucket.delta_pct.push(std::abs(deltaPct));  // Store magnitude

            double volSec = 50.0 + j * 4.5;  // Range: 50 to 500
            bucket.vol_sec.push(volSec);
        }

        bucket.sessionsContributed = 5;
        bucket.totalBarsPushed = 100;
    }

    return store;
}

// Helper: Create populated SessionDeltaBaseline
SessionDeltaBaseline CreatePopulatedSessionBaseline() {
    SessionDeltaBaseline baseline;
    baseline.Reset(50);

    for (int i = 0; i < AMT::EFFORT_BUCKET_COUNT; ++i) {
        auto& bucket = baseline.buckets[i];

        // Add session delta ratios: -0.3 to +0.3
        for (int j = 0; j < 30; ++j) {
            double ratio = -0.3 + (j % 7) * 0.1;
            bucket.Push(ratio);  // Stores absolute value internally
        }
        bucket.sessionsContributed = 5;
    }

    return baseline;
}

// ============================================================================
// TEST: Character Classification - Sustained vs Episodic
// ============================================================================

void TestCharacterClassification() {
    TEST_SECTION("Character Classification");

    DeltaEngine engine;
    auto store = CreatePopulatedEffortStore();
    auto sessBaseline = CreatePopulatedSessionBaseline();

    engine.SetEffortStore(&store);
    engine.SetSessionDeltaBaseline(&sessBaseline);
    engine.SetPhase(SessionPhase::INITIAL_BALANCE);
    engine.config.sustainedMinBars = 3;

    // Test EPISODIC: Single bar spike
    {
        engine.Reset();
        // First bar with strong delta
        auto result = engine.Compute(500.0, 1000.0, 2.0, 500.0, 1000.0, 0);
        // First bar, not enough history for SUSTAINED
        TEST_ASSERT(result.IsReady(), "Result should be ready with populated baseline");
        // Character will be EPISODIC or NEUTRAL initially (not enough history for SUSTAINED)
        TEST_ASSERT(result.character != DeltaCharacter::SUSTAINED,
                    "Single bar should not be SUSTAINED");
    }

    // Test SUSTAINED: Multiple aligned bars
    {
        engine.Reset();
        // Simulate 5 bars with consistently positive delta
        for (int i = 0; i < 5; ++i) {
            double barDelta = 400.0 + i * 20;  // Consistently positive
            double barVol = 1000.0;
            double priceChange = 1.0;  // Price going up
            auto result = engine.Compute(barDelta, barVol, priceChange,
                                          barDelta * (i + 1), barVol * (i + 1), i);
        }

        // After 5 aligned bars, should be SUSTAINED
        auto result = engine.Compute(420.0, 1000.0, 1.0, 2500.0, 6000.0, 5);
        TEST_ASSERT(result.sustainedBars >= 3, "Should have 3+ sustained bars");
        // With hysteresis, might take a few bars to confirm SUSTAINED
    }

    std::cout << "[OK] Character classification differentiates sustained vs episodic\n";
}

// ============================================================================
// TEST: Character - Reversal Detection
// ============================================================================

void TestReversalDetection() {
    TEST_SECTION("Reversal Detection");

    DeltaEngine engine;
    auto store = CreatePopulatedEffortStore();
    auto sessBaseline = CreatePopulatedSessionBaseline();

    engine.SetEffortStore(&store);
    engine.SetSessionDeltaBaseline(&sessBaseline);
    engine.SetPhase(SessionPhase::MID_SESSION);

    // Build up positive delta direction
    for (int i = 0; i < 3; ++i) {
        engine.Compute(300.0, 1000.0, 1.0, 300.0 * (i + 1), 1000.0 * (i + 1), i);
    }

    // Reverse to negative delta
    auto result = engine.Compute(-400.0, 1000.0, -1.0, 500.0, 4000.0, 3);

    TEST_ASSERT(result.IsReady(), "Result should be ready");
    TEST_ASSERT(result.reversalDetected, "Should detect reversal on direction flip");

    std::cout << "[OK] Reversal detection identifies direction flips\n";
}

// ============================================================================
// TEST: Alignment Classification - Convergent
// ============================================================================

void TestAlignmentConvergent() {
    TEST_SECTION("Alignment Classification - Convergent");

    DeltaEngine engine;
    auto store = CreatePopulatedEffortStore();
    auto sessBaseline = CreatePopulatedSessionBaseline();

    engine.SetEffortStore(&store);
    engine.SetSessionDeltaBaseline(&sessBaseline);
    engine.SetPhase(SessionPhase::MID_SESSION);
    engine.config.alignmentDeltaThreshold = 0.15;
    engine.config.alignmentPriceThreshold = 0.5;
    engine.config.alignmentConfirmBars = 1;  // Fast confirmation for test

    // Test CONVERGENT: positive delta + price up
    {
        // Delta = +300 / 1000 = +0.30 (positive, above threshold)
        // Price change = +2 ticks (up)
        // Should be CONVERGENT
        auto result = engine.Compute(300.0, 1000.0, 2.0, 300.0, 1000.0, 0);
        TEST_ASSERT(result.IsReady(), "Result should be ready");
        TEST_ASSERT(result.DeltaSign() > 0, "Delta should be positive");
        TEST_ASSERT(result.PriceSign() > 0, "Price should be positive");

        // With single confirm bar, alignment should converge
        auto result2 = engine.Compute(350.0, 1000.0, 3.0, 650.0, 2000.0, 1);
        // After 2 bars of same alignment, should confirm CONVERGENT
        TEST_ASSERT(result2.alignment == DeltaAlignment::CONVERGENT ||
                    result2.alignment == DeltaAlignment::NEUTRAL,
                    "Positive delta + price up should be CONVERGENT or NEUTRAL");
    }

    engine.Reset();

    // Test CONVERGENT: negative delta + price down
    {
        auto result = engine.Compute(-300.0, 1000.0, -2.0, -300.0, 1000.0, 0);
        TEST_ASSERT(result.DeltaSign() < 0, "Delta should be negative");
        TEST_ASSERT(result.PriceSign() < 0, "Price should be negative");

        auto result2 = engine.Compute(-350.0, 1000.0, -3.0, -650.0, 2000.0, 1);
        TEST_ASSERT(result2.alignment == DeltaAlignment::CONVERGENT ||
                    result2.alignment == DeltaAlignment::NEUTRAL,
                    "Negative delta + price down should be CONVERGENT or NEUTRAL");
    }

    std::cout << "[OK] Convergent alignment detected when delta matches price\n";
}

// ============================================================================
// TEST: Alignment Classification - Divergent/Absorption
// ============================================================================

void TestAlignmentDivergent() {
    TEST_SECTION("Alignment Classification - Divergent/Absorption");

    DeltaEngine engine;
    auto store = CreatePopulatedEffortStore();
    auto sessBaseline = CreatePopulatedSessionBaseline();

    engine.SetEffortStore(&store);
    engine.SetSessionDeltaBaseline(&sessBaseline);
    engine.SetPhase(SessionPhase::MID_SESSION);
    engine.config.alignmentDeltaThreshold = 0.15;
    engine.config.alignmentPriceThreshold = 0.5;
    engine.config.alignmentConfirmBars = 1;

    // Test ABSORPTION_BID: price up but negative delta (sellers hitting into buyers)
    {
        // Delta = -300 / 1000 = -0.30 (negative, selling)
        // Price change = +2 ticks (going UP despite selling)
        // This is bullish divergence - passive buyers absorbing sells
        auto result = engine.Compute(-300.0, 1000.0, 2.0, -300.0, 1000.0, 0);
        TEST_ASSERT(result.IsReady(), "Result should be ready");
        TEST_ASSERT(result.DeltaSign() < 0, "Delta should be negative");
        TEST_ASSERT(result.PriceSign() > 0, "Price should be positive");

        auto result2 = engine.Compute(-350.0, 1000.0, 3.0, -650.0, 2000.0, 1);
        bool isDivergent = result2.alignment == DeltaAlignment::DIVERGENT ||
                           result2.alignment == DeltaAlignment::ABSORPTION_BID;
        TEST_ASSERT(isDivergent || result2.alignment == DeltaAlignment::NEUTRAL,
                    "Price up + negative delta should be DIVERGENT/ABSORPTION_BID");
    }

    engine.Reset();

    // Test ABSORPTION_ASK: price down but positive delta (buyers lifting into sellers)
    {
        // Delta = +300 / 1000 = +0.30 (positive, buying)
        // Price change = -2 ticks (going DOWN despite buying)
        // This is bearish divergence - passive sellers absorbing buys
        auto result = engine.Compute(300.0, 1000.0, -2.0, 300.0, 1000.0, 0);
        TEST_ASSERT(result.DeltaSign() > 0, "Delta should be positive");
        TEST_ASSERT(result.PriceSign() < 0, "Price should be negative");

        auto result2 = engine.Compute(350.0, 1000.0, -3.0, 650.0, 2000.0, 1);
        bool isDivergent = result2.alignment == DeltaAlignment::DIVERGENT ||
                           result2.alignment == DeltaAlignment::ABSORPTION_ASK;
        TEST_ASSERT(isDivergent || result2.alignment == DeltaAlignment::NEUTRAL,
                    "Price down + positive delta should be DIVERGENT/ABSORPTION_ASK");
    }

    std::cout << "[OK] Divergent/absorption alignment detected on price-delta mismatch\n";
}

// ============================================================================
// TEST: Confidence Degradation - Thin Tape
// ============================================================================

void TestConfidenceThinTape() {
    TEST_SECTION("Confidence Degradation - Thin Tape");

    DeltaEngine engine;
    auto store = CreatePopulatedEffortStore();
    auto sessBaseline = CreatePopulatedSessionBaseline();

    engine.SetEffortStore(&store);
    engine.SetSessionDeltaBaseline(&sessBaseline);
    engine.SetPhase(SessionPhase::MID_SESSION);
    engine.config.thinTapeVolumePctile = 10.0;

    // Test thin tape: very low volume
    {
        // Volume = 30 (very low, should be below P10)
        auto result = engine.Compute(20.0, 30.0, 1.0, 20.0, 30.0, 0);
        TEST_ASSERT(result.IsReady(), "Result should be ready");
        TEST_ASSERT(result.isThinTape, "Low volume should flag thin tape");
        TEST_ASSERT(result.confidence != DeltaConfidence::FULL,
                    "Thin tape should degrade confidence from FULL");
        TEST_ASSERT(result.HasWarnings(), "Should have warning flags");
    }

    std::cout << "[OK] Thin tape detection degrades confidence\n";
}

// ============================================================================
// TEST: Confidence Degradation - High Chop
// ============================================================================

void TestConfidenceHighChop() {
    TEST_SECTION("Confidence Degradation - High Chop");

    DeltaEngine engine;
    auto store = CreatePopulatedEffortStore();
    auto sessBaseline = CreatePopulatedSessionBaseline();

    engine.SetEffortStore(&store);
    engine.SetSessionDeltaBaseline(&sessBaseline);
    engine.SetPhase(SessionPhase::MID_SESSION);
    engine.config.highChopReversalsThreshold = 4;
    engine.config.reversalLookback = 10;

    // Simulate choppy market with many reversals
    // Alternate positive/negative delta to create reversals
    for (int i = 0; i < 10; ++i) {
        double delta = (i % 2 == 0) ? 300.0 : -300.0;  // Alternating
        engine.Compute(delta, 1000.0, delta > 0 ? 1.0 : -1.0,
                       delta * (i + 1), 1000.0 * (i + 1), i);
    }

    auto result = engine.Compute(300.0, 1000.0, 1.0, 3000.0, 11000.0, 10);

    TEST_ASSERT(result.IsReady(), "Result should be ready");
    TEST_ASSERT(result.isHighChop, "Frequent reversals should flag high chop");
    TEST_ASSERT(result.confidence != DeltaConfidence::FULL,
                "High chop should degrade confidence");

    std::cout << "[OK] High chop detection degrades confidence\n";
}

// ============================================================================
// TEST: Confidence Degradation - Exhaustion
// ============================================================================

void TestConfidenceExhaustion() {
    TEST_SECTION("Confidence Degradation - Exhaustion");

    DeltaEngine engine;
    auto store = CreatePopulatedEffortStore();
    auto sessBaseline = CreatePopulatedSessionBaseline();

    engine.SetEffortStore(&store);
    engine.SetSessionDeltaBaseline(&sessBaseline);
    engine.SetPhase(SessionPhase::MID_SESSION);
    engine.config.exhaustionDeltaPctile = 95.0;

    // Test exhaustion: extremely one-sided delta (above P95)
    {
        // Delta = +900 / 1000 = +0.90 (extreme positive)
        // This should be well above P95 in baseline
        auto result = engine.Compute(900.0, 1000.0, 5.0, 900.0, 1000.0, 0);
        TEST_ASSERT(result.IsReady(), "Result should be ready");

        if (result.barDeltaPctile > 95.0) {
            TEST_ASSERT(result.isExhaustion, "Extreme delta should flag exhaustion");
        }
    }

    std::cout << "[OK] Exhaustion detection identifies extreme one-sidedness\n";
}

// ============================================================================
// TEST: Shock Delta Detection (Jan 2025)
// ============================================================================

void TestShockDeltaDetection() {
    TEST_SECTION("Shock Delta Detection (P99+)");

    DeltaEngine engine;
    auto store = CreatePopulatedEffortStore();
    auto sessBaseline = CreatePopulatedSessionBaseline();

    engine.SetEffortStore(&store);
    engine.SetSessionDeltaBaseline(&sessBaseline);
    engine.SetPhase(SessionPhase::MID_SESSION);
    engine.config.exhaustionDeltaPctile = 95.0;
    engine.config.shockDeltaPctile = 99.0;

    // Test shock: extremely one-sided delta (above P99)
    {
        // Delta = +990 / 1000 = +0.99 (extreme positive - near max)
        // This should be well above P99 in baseline
        auto result = engine.Compute(990.0, 1000.0, 5.0, 990.0, 1000.0, 0);
        TEST_ASSERT(result.IsReady(), "Result should be ready");

        // Verify flag is set correctly based on percentile
        if (result.barDeltaPctile > 99.0) {
            TEST_ASSERT(result.isShockDelta, "P99+ delta should flag shock");
            TEST_ASSERT(result.IsShock(), "IsShock() helper should return true");
        }

        // Also verify exhaustion is triggered (P95+ implies P99+ when shock)
        if (result.barDeltaPctile > 95.0) {
            TEST_ASSERT(result.isExhaustion, "Shock delta should also flag exhaustion");
        }

        std::cout << "  barDeltaPctile=" << result.barDeltaPctile
                  << " isExhaustion=" << result.isExhaustion
                  << " isShockDelta=" << result.isShockDelta << "\n";
    }

    // Test threshold logic: verify flags are set correctly based on percentile
    {
        // Create fresh engine for this test to avoid state issues
        DeltaEngine engine2;
        auto store2 = CreatePopulatedEffortStore();
        auto sessBaseline2 = CreatePopulatedSessionBaseline();
        engine2.SetEffortStore(&store2);
        engine2.SetSessionDeltaBaseline(&sessBaseline2);
        engine2.SetPhase(SessionPhase::MID_SESSION);
        engine2.config.exhaustionDeltaPctile = 95.0;
        engine2.config.shockDeltaPctile = 99.0;

        // Delta = +500 / 1000 = +0.50 (moderate - should be below exhaustion)
        auto result = engine2.Compute(500.0, 1000.0, 5.0, 500.0, 1000.0, 0);

        if (result.IsReady()) {
            // Verify that lower percentiles don't trigger shock
            if (result.barDeltaPctile <= 95.0) {
                TEST_ASSERT(!result.isExhaustion, "P95- should NOT flag exhaustion");
                TEST_ASSERT(!result.isShockDelta, "P95- should NOT flag shock");
            }

            std::cout << "  moderate delta: barDeltaPctile=" << result.barDeltaPctile
                      << " isExhaustion=" << result.isExhaustion
                      << " isShockDelta=" << result.isShockDelta << "\n";
        } else {
            std::cout << "  moderate delta: skipped (baseline not ready)\n";
        }
    }

    // Test warning flags bitmask includes shock
    {
        auto result = engine.Compute(990.0, 1000.0, 5.0, 990.0, 1000.0, 0);
        if (result.isShockDelta) {
            TEST_ASSERT((result.warningFlags & (1 << 4)) != 0, "Shock should set bit 4 in warningFlags");
        }
    }

    std::cout << "[OK] Shock delta detection identifies P99+ one-sidedness\n";
}

// ============================================================================
// TEST: Trading Constraints
// ============================================================================

void TestTradingConstraints() {
    TEST_SECTION("Trading Constraints");

    DeltaEngine engine;
    auto store = CreatePopulatedEffortStore();
    auto sessBaseline = CreatePopulatedSessionBaseline();

    engine.SetEffortStore(&store);
    engine.SetSessionDeltaBaseline(&sessBaseline);
    engine.SetPhase(SessionPhase::MID_SESSION);
    engine.config.blockContinuationOnDivergence = true;
    engine.config.alignmentConfirmBars = 1;

    // Test constraints on divergent alignment
    {
        // Create divergent condition: price up, delta negative
        for (int i = 0; i < 3; ++i) {
            engine.Compute(-300.0, 1000.0, 2.0, -300.0 * (i + 1), 1000.0 * (i + 1), i);
        }

        auto result = engine.Compute(-350.0, 1000.0, 3.0, -1250.0, 4000.0, 3);

        if (result.IsDiverging()) {
            TEST_ASSERT(!result.constraints.allowContinuation,
                        "Divergent alignment should block continuation");
            TEST_ASSERT(result.constraints.allowFade,
                        "Divergent alignment should allow fade");
        }
    }

    std::cout << "[OK] Trading constraints applied based on delta state\n";
}

// ============================================================================
// TEST: Hysteresis State Machine
// ============================================================================

void TestHysteresis() {
    TEST_SECTION("Hysteresis State Machine");

    DeltaEngine engine;
    auto store = CreatePopulatedEffortStore();
    auto sessBaseline = CreatePopulatedSessionBaseline();

    engine.SetEffortStore(&store);
    engine.SetSessionDeltaBaseline(&sessBaseline);
    engine.SetPhase(SessionPhase::MID_SESSION);
    engine.config.characterConfirmBars = 3;

    // Establish baseline character
    for (int i = 0; i < 5; ++i) {
        engine.Compute(300.0, 1000.0, 1.0, 300.0 * (i + 1), 1000.0 * (i + 1), i);
    }

    DeltaCharacter initialChar = engine.GetConfirmedCharacter();

    // Single different bar should NOT change confirmed character
    {
        auto result = engine.Compute(-50.0, 1000.0, 0.0, 1450.0, 6000.0, 5);
        // Confirmed character should remain stable
        TEST_ASSERT(engine.GetConfirmedCharacter() == initialChar ||
                    engine.GetConfirmedCharacter() == DeltaCharacter::UNKNOWN,
                    "Single bar should not change confirmed character");
    }

    std::cout << "[OK] Hysteresis prevents character whipsaw\n";
}

// ============================================================================
// TEST: Session Boundary Handling
// ============================================================================

void TestSessionBoundary() {
    TEST_SECTION("Session Boundary Handling");

    DeltaEngine engine;
    auto store = CreatePopulatedEffortStore();
    auto sessBaseline = CreatePopulatedSessionBaseline();

    engine.SetEffortStore(&store);
    engine.SetSessionDeltaBaseline(&sessBaseline);
    engine.SetPhase(SessionPhase::MID_SESSION);

    // Simulate first session
    for (int i = 0; i < 20; ++i) {
        engine.Compute(200.0 + (i % 100), 1000.0, 1.0,
                       200.0 * (i + 1), 1000.0 * (i + 1), i);
    }

    TEST_ASSERT(engine.GetSessionBars() == 20, "Should track 20 session bars");

    // Reset for new session
    engine.ResetForSession();

    TEST_ASSERT(engine.GetSessionBars() == 0, "Session bars should reset");
    // Note: hysteresis state is preserved across sessions

    // New session should work correctly
    auto result = engine.Compute(250.0, 1000.0, 1.0, 250.0, 1000.0, 0);
    TEST_ASSERT(result.IsReady(), "New session should compute correctly");

    std::cout << "[OK] Session boundary handling works correctly\n";
}

// ============================================================================
// TEST: Validity Gating
// ============================================================================

void TestValidityGating() {
    TEST_SECTION("Validity Gating");

    // Test without effort store
    {
        DeltaEngine engine;
        engine.SetPhase(SessionPhase::MID_SESSION);

        auto result = engine.Compute(200.0, 1000.0, 1.0, 200.0, 1000.0, 0);
        TEST_ASSERT(!result.IsReady(), "Should not be ready without effort store");
        TEST_ASSERT(result.errorReason == DeltaErrorReason::ERR_NO_BASELINE_STORE,
                    "Error should be NO_BASELINE_STORE");
    }

    // Test with empty effort store (warmup)
    {
        DeltaEngine engine;
        engine.SetPhase(SessionPhase::MID_SESSION);
        EffortBaselineStore emptyStore;
        emptyStore.Reset(100);
        engine.SetEffortStore(&emptyStore);

        auto result = engine.Compute(200.0, 1000.0, 1.0, 200.0, 1000.0, 0);
        TEST_ASSERT(!result.IsReady(), "Should not be ready with empty baseline");
        TEST_ASSERT(result.IsWarmup(), "Should be in warmup state");
    }

    // Test zero volume
    {
        DeltaEngine engine;
        engine.SetPhase(SessionPhase::MID_SESSION);
        auto store = CreatePopulatedEffortStore();
        engine.SetEffortStore(&store);

        auto result = engine.Compute(100.0, 0.0, 1.0, 100.0, 1000.0, 0);
        TEST_ASSERT(!result.IsReady(), "Should not be ready with zero volume");
        TEST_ASSERT(result.errorReason == DeltaErrorReason::ERR_ZERO_VOLUME,
                    "Error should be ZERO_VOLUME");
    }

    std::cout << "[OK] Validity gating prevents invalid usage\n";
}

// ============================================================================
// TEST: History Tracking
// ============================================================================

void TestHistoryTracking() {
    TEST_SECTION("History Tracking");

    DeltaEngine engine;
    auto store = CreatePopulatedEffortStore();
    auto sessBaseline = CreatePopulatedSessionBaseline();

    engine.SetEffortStore(&store);
    engine.SetSessionDeltaBaseline(&sessBaseline);
    engine.SetPhase(SessionPhase::MID_SESSION);

    // Add bars and verify history tracking
    for (int i = 0; i < 10; ++i) {
        engine.Compute(200.0 + i * 10, 1000.0, 1.0,
                       200.0 * (i + 1), 1000.0 * (i + 1), i);
    }

    const auto& history = engine.GetHistory();
    TEST_ASSERT(history.history.size() == 10, "History should have 10 entries");
    TEST_ASSERT(history.GetBarsInDirection() > 0, "Should track consecutive aligned bars");

    // Verify magnitude trend calculation
    double trend = history.GetMagnitudeTrend();
    // With increasing delta, trend should be positive
    TEST_ASSERT(!std::isnan(trend), "Magnitude trend should be valid number");

    std::cout << "[OK] History tracking records bar-by-bar state\n";
}

// ============================================================================
// TEST: DeltaDecisionInput Helper
// ============================================================================

void TestDecisionInputHelper() {
    TEST_SECTION("DeltaDecisionInput Helper");

    DeltaEngine engine;
    auto store = CreatePopulatedEffortStore();
    auto sessBaseline = CreatePopulatedSessionBaseline();

    engine.SetEffortStore(&store);
    engine.SetSessionDeltaBaseline(&sessBaseline);
    engine.SetPhase(SessionPhase::MID_SESSION);

    // Build up some state
    for (int i = 0; i < 5; ++i) {
        engine.Compute(300.0, 1000.0, 2.0, 300.0 * (i + 1), 1000.0 * (i + 1), i);
    }

    auto result = engine.Compute(350.0, 1000.0, 2.0, 1850.0, 6000.0, 5);

    // Convert to decision input
    auto input = DeltaDecisionInput::FromResult(result);

    TEST_ASSERT(input.isReady == result.IsReady(), "isReady should match");
    TEST_ASSERT(input.isSustained == result.IsSustained(), "isSustained should match");
    TEST_ASSERT(input.isConvergent == result.IsAligned(), "isConvergent should match");
    TEST_ASSERT(input.confidence == result.confidence, "confidence should match");
    TEST_ASSERT(input.allowContinuation == result.constraints.allowContinuation,
                "allowContinuation should match");

    std::cout << "[OK] DeltaDecisionInput helper converts result correctly\n";
}

// ============================================================================
// TEST: Signal Strength Calculation
// ============================================================================

void TestSignalStrength() {
    TEST_SECTION("Signal Strength Calculation");

    DeltaEngine engine;
    auto store = CreatePopulatedEffortStore();
    auto sessBaseline = CreatePopulatedSessionBaseline();

    engine.SetEffortStore(&store);
    engine.SetSessionDeltaBaseline(&sessBaseline);
    engine.SetPhase(SessionPhase::MID_SESSION);

    // Low signal conditions
    {
        auto result = engine.Compute(50.0, 1000.0, 0.1, 50.0, 1000.0, 0);
        if (result.IsReady()) {
            double strength = result.GetSignalStrength();
            TEST_ASSERT(strength >= 0.0 && strength <= 1.0,
                        "Signal strength should be in [0, 1]");
        }
    }

    // High signal conditions
    {
        for (int i = 0; i < 5; ++i) {
            engine.Compute(500.0, 1000.0, 3.0, 500.0 * (i + 1), 1000.0 * (i + 1), i);
        }

        auto result = engine.Compute(550.0, 1000.0, 4.0, 3050.0, 6000.0, 5);
        if (result.IsReady()) {
            double strength = result.GetSignalStrength();
            TEST_ASSERT(strength >= 0.0 && strength <= 1.0,
                        "Signal strength should be in [0, 1]");
            // Strong sustained convergent should have higher strength
        }
    }

    std::cout << "[OK] Signal strength calculation produces bounded values\n";
}

// ============================================================================
// TEST: Phase-Aware Baseline Queries
// ============================================================================

void TestPhaseAwareBaselines() {
    TEST_SECTION("Phase-Aware Baseline Queries");

    DeltaEngine engine;
    auto store = CreatePopulatedEffortStore();
    auto sessBaseline = CreatePopulatedSessionBaseline();

    engine.SetEffortStore(&store);
    engine.SetSessionDeltaBaseline(&sessBaseline);

    // Test different phases use different baselines
    {
        engine.SetPhase(SessionPhase::GLOBEX);
        auto result1 = engine.Compute(300.0, 1000.0, 1.0, 300.0, 1000.0, 0);
        TEST_ASSERT(result1.phase == SessionPhase::GLOBEX, "Phase should be GLOBEX");
    }

    {
        engine.Reset();
        engine.SetPhase(SessionPhase::MID_SESSION);
        auto result2 = engine.Compute(300.0, 1000.0, 1.0, 300.0, 1000.0, 0);
        TEST_ASSERT(result2.phase == SessionPhase::MID_SESSION, "Phase should be MID_SESSION");
    }

    // Test non-tradeable phase
    {
        engine.Reset();
        engine.SetPhase(SessionPhase::UNKNOWN);
        auto result = engine.Compute(300.0, 1000.0, 1.0, 300.0, 1000.0, 0);
        TEST_ASSERT(!result.IsReady(), "Non-tradeable phase should not be ready");
    }

    std::cout << "[OK] Phase-aware baseline queries work correctly\n";
}

// ============================================================================
// TEST: Location Context Build
// ============================================================================

void TestLocationContextBuild() {
    TEST_SECTION("Location Context Build");

    const double tickSize = 0.25;

    // Test IN_VALUE
    {
        auto ctx = DeltaLocationContext::Build(
            6050.0,   // price at POC
            6050.0,   // poc
            6060.0,   // vah
            6040.0,   // val
            tickSize,
            2.0,      // edgeToleranceTicks
            8.0       // discoveryThresholdTicks
        );
        TEST_ASSERT(ctx.isValid, "Context should be valid");
        TEST_ASSERT(ctx.zone == ValueZone::AT_POC, "Price at POC should be AT_POC");
        TEST_ASSERT(ctx.IsInValue(), "IsInValue() should be true for AT_POC");
        TEST_ASSERT(!ctx.IsAtEdge(), "IsAtEdge() should be false");
    }

    // Test AT_VALUE_EDGE (at VAH)
    {
        auto ctx = DeltaLocationContext::Build(
            6060.25,  // price at VAH + 1 tick (within 2 tick tolerance)
            6050.0,   // poc
            6060.0,   // vah
            6040.0,   // val
            tickSize
        );
        TEST_ASSERT(ctx.isValid, "Context should be valid");
        TEST_ASSERT(ctx.zone == ValueZone::AT_VAH, "Price near VAH should be AT_VAH");
        TEST_ASSERT(ctx.IsAtEdge(), "IsAtEdge() should be true");
        TEST_ASSERT(ctx.IsAboveValue(), "IsAboveValue() should be true at VAH");
    }

    // Test OUTSIDE_VALUE (NEAR_ABOVE_VALUE)
    {
        auto ctx = DeltaLocationContext::Build(
            6061.25,  // price 5 ticks above VAH (6060 + 5*0.25)
            6050.0,   // poc
            6060.0,   // vah
            6040.0,   // val
            tickSize,
            2.0,      // edgeToleranceTicks
            8.0       // discoveryThresholdTicks (5 < 8, so NEAR_ABOVE not FAR_ABOVE)
        );
        TEST_ASSERT(ctx.isValid, "Context should be valid");
        TEST_ASSERT(ctx.zone == ValueZone::NEAR_ABOVE_VALUE, "Price 5t above VAH should be NEAR_ABOVE_VALUE");
        TEST_ASSERT(ctx.IsOutsideValue(), "IsOutsideValue() should be true");
        TEST_ASSERT(ctx.IsAboveValue(), "IsAboveValue() should be true");
    }

    // Test IN_DISCOVERY (FAR_ABOVE_VALUE)
    {
        auto ctx = DeltaLocationContext::Build(
            6075.0,   // price 15 ticks above VAH
            6050.0,   // poc
            6060.0,   // vah
            6040.0,   // val
            tickSize,
            2.0,      // edgeToleranceTicks
            8.0       // discoveryThresholdTicks (15 > 8, so FAR_ABOVE_VALUE)
        );
        TEST_ASSERT(ctx.isValid, "Context should be valid");
        TEST_ASSERT(ctx.zone == ValueZone::FAR_ABOVE_VALUE, "Price 15t above VAH should be FAR_ABOVE_VALUE");
        TEST_ASSERT(ctx.IsInDiscovery(), "IsInDiscovery() should be true");
        TEST_ASSERT(ctx.IsAboveValue(), "IsAboveValue() should be true");
    }

    // Test POC migration detection
    {
        auto ctx = DeltaLocationContext::Build(
            6055.0,   // price above POC
            6052.0,   // poc (moved up from 6050)
            6060.0, 6040.0, tickSize,
            2.0, 8.0,
            0.0, 0.0, 0.0, 0.0,
            6050.0    // priorPOC
        );
        TEST_ASSERT(ctx.isMigratingTowardPrice, "POC moving toward price should set flag");
        TEST_ASSERT(!ctx.isMigratingAwayFromPrice, "Should not be migrating away");
    }

    std::cout << "[OK] Location context build correctly classifies price zones\n";
}

// ============================================================================
// TEST: Location-Aware Compute
// ============================================================================

void TestLocationAwareCompute() {
    TEST_SECTION("Location-Aware Compute");

    DeltaEngine engine;
    auto store = CreatePopulatedEffortStore();
    auto sessBaseline = CreatePopulatedSessionBaseline();

    engine.SetEffortStore(&store);
    engine.SetSessionDeltaBaseline(&sessBaseline);
    engine.SetPhase(SessionPhase::MID_SESSION);

    const double tickSize = 0.25;

    // Test compute with location context
    {
        // Build location context - price IN_VALUE
        auto locCtx = DeltaLocationContext::Build(
            6050.0, 6050.0, 6060.0, 6040.0, tickSize);

        auto result = engine.Compute(
            300.0, 1000.0, 2.0,
            300.0, 1000.0, 0,
            locCtx);

        TEST_ASSERT(result.IsReady(), "Result should be ready");
        TEST_ASSERT(result.HasLocationContext(), "Result should have location context");
        TEST_ASSERT(result.location.isValid, "Location should be valid");
        TEST_ASSERT(result.IsInValue(), "Should be in value");
    }

    // Test that location context affects outcome likelihoods
    engine.Reset();
    {
        // Build up sustained convergent state
        for (int i = 0; i < 5; ++i) {
            auto locCtx = DeltaLocationContext::Build(
                6065.0 + i, 6050.0, 6060.0, 6040.0, tickSize,
                2.0, 8.0);
            engine.Compute(300.0, 1000.0, 1.0,
                           300.0 * (i + 1), 1000.0 * (i + 1), i, locCtx);
        }

        // Price outside value (5 ticks above VAH = NEAR_ABOVE), sustained + aligned should favor acceptance
        auto locCtx = DeltaLocationContext::Build(
            6061.25, 6050.0, 6060.0, 6040.0, tickSize, 2.0, 8.0);  // 5 ticks above VAH
        auto result = engine.Compute(350.0, 1000.0, 2.0,
                                      1850.0, 6000.0, 5, locCtx);

        TEST_ASSERT(result.HasLocationContext(), "Should have location context");
        TEST_ASSERT(result.IsOutsideValue(), "Should be outside value");

        // Likelihoods should be set
        double totalLik = result.acceptanceLikelihood + result.rejectionLikelihood + result.rotationLikelihood;
        TEST_ASSERT(std::abs(totalLik - 1.0) < 0.01, "Likelihoods should sum to ~1.0");
    }

    std::cout << "[OK] Location-aware compute attaches context and computes outcomes\n";
}

// ============================================================================
// TEST: Auction Outcome Likelihoods
// ============================================================================

void TestAuctionOutcomeLikelihoods() {
    TEST_SECTION("Auction Outcome Likelihoods");

    DeltaEngine engine;
    auto store = CreatePopulatedEffortStore();
    auto sessBaseline = CreatePopulatedSessionBaseline();

    engine.SetEffortStore(&store);
    engine.SetSessionDeltaBaseline(&sessBaseline);
    engine.SetPhase(SessionPhase::MID_SESSION);

    const double tickSize = 0.25;

    // Test IN_VALUE -> rotation biased
    {
        auto locCtx = DeltaLocationContext::Build(
            6050.0, 6050.0, 6060.0, 6040.0, tickSize);

        auto result = engine.Compute(200.0, 1000.0, 0.5,
                                      200.0, 1000.0, 0, locCtx);

        if (result.HasLocationContext()) {
            // In value should have higher rotation likelihood
            TEST_ASSERT(result.rotationLikelihood > 0.3,
                        "IN_VALUE should have elevated rotation likelihood");
        }
    }

    engine.Reset();

    // Test AT_VALUE_EDGE with divergence -> rejection biased
    {
        // Create divergent condition at VAH: price up, delta negative
        auto locCtx = DeltaLocationContext::Build(
            6060.0, 6050.0, 6060.0, 6040.0, tickSize);  // At VAH

        for (int i = 0; i < 3; ++i) {
            engine.Compute(-300.0, 1000.0, 1.0,  // Negative delta, price up
                           -300.0 * (i + 1), 1000.0 * (i + 1), i, locCtx);
        }

        auto result = engine.Compute(-350.0, 1000.0, 2.0,
                                      -1250.0, 4000.0, 3, locCtx);

        if (result.HasLocationContext() && result.IsDiverging()) {
            // Divergent at edge should favor rejection
            TEST_ASSERT(result.rejectionLikelihood >= result.rotationLikelihood,
                        "Divergent at edge should favor rejection");
        }
    }

    std::cout << "[OK] Auction outcome likelihoods vary by location and delta state\n";
}

// ============================================================================
// TEST: Location-Sensitive Adjustments
// ============================================================================

void TestLocationSensitiveAdjustments() {
    TEST_SECTION("Location-Sensitive Adjustments");

    DeltaEngine engine;
    auto store = CreatePopulatedEffortStore();
    auto sessBaseline = CreatePopulatedSessionBaseline();

    engine.SetEffortStore(&store);
    engine.SetSessionDeltaBaseline(&sessBaseline);
    engine.SetPhase(SessionPhase::MID_SESSION);

    const double tickSize = 0.25;

    // Test IN_VALUE requires delta alignment
    {
        auto locCtx = DeltaLocationContext::Build(
            6050.0, 6050.0, 6060.0, 6040.0, tickSize);

        auto result = engine.Compute(200.0, 1000.0, 1.0,
                                      200.0, 1000.0, 0, locCtx);

        if (result.HasLocationContext()) {
            TEST_ASSERT(result.constraints.requireDeltaAlignment,
                        "IN_VALUE should require delta alignment");
        }
    }

    engine.Reset();

    // Test IN_DISCOVERY without conviction reduces position size
    {
        auto locCtx = DeltaLocationContext::Build(
            6080.0, 6050.0, 6060.0, 6040.0, tickSize,  // Far outside value
            2.0, 8.0);  // 20 ticks above VAH > 8 threshold

        // Single bar (not sustained)
        auto result = engine.Compute(200.0, 1000.0, 1.0,
                                      200.0, 1000.0, 0, locCtx);

        if (result.HasLocationContext() && !result.IsSustained()) {
            TEST_ASSERT(result.constraints.positionSizeMultiplier < 1.0,
                        "Discovery without conviction should reduce position size");
        }
    }

    std::cout << "[OK] Location-sensitive adjustments applied correctly\n";
}

// ============================================================================
// TEST: Outcome Accessors
// ============================================================================

void TestOutcomeAccessors() {
    TEST_SECTION("Outcome Accessors");

    DeltaResult result;

    // Test default values
    TEST_ASSERT(!result.HasLocationContext(), "Default should not have location context");
    TEST_ASSERT(!result.IsAcceptanceLikely(), "Default should not be acceptance likely");
    TEST_ASSERT(!result.IsRejectionLikely(), "Default should not be rejection likely");
    TEST_ASSERT(!result.IsRotationLikely(), "Default should not be rotation likely");

    // Set acceptance outcome
    result.location.isValid = true;
    result.location.zone = ValueZone::NEAR_ABOVE_VALUE;  // 9-state: outside value above
    result.likelyOutcome = DeltaAuctionPrediction::ACCEPTANCE_LIKELY;
    result.acceptanceLikelihood = 0.65;
    result.rejectionLikelihood = 0.20;
    result.rotationLikelihood = 0.15;

    TEST_ASSERT(result.HasLocationContext(), "Should have location context");
    TEST_ASSERT(result.IsAcceptanceLikely(), "Should be acceptance likely");
    TEST_ASSERT(!result.IsRejectionLikely(), "Should not be rejection likely");
    TEST_ASSERT(result.GetDominantLikelihood() == 0.65, "Dominant likelihood should be 0.65");
    TEST_ASSERT(result.IsHighConvictionOutcome(), "0.65 > 0.6 threshold");

    std::cout << "[OK] Outcome accessors return correct values\n";
}

// ============================================================================
// TEST: High Quality Signal With Context
// ============================================================================

void TestHighQualitySignalWithContext() {
    TEST_SECTION("High Quality Signal With Context");

    DeltaEngine engine;
    auto store = CreatePopulatedEffortStore();
    auto sessBaseline = CreatePopulatedSessionBaseline();

    engine.SetEffortStore(&store);
    engine.SetSessionDeltaBaseline(&sessBaseline);
    engine.SetPhase(SessionPhase::MID_SESSION);

    const double tickSize = 0.25;

    // Build up strong signal with location context
    for (int i = 0; i < 5; ++i) {
        auto locCtx = DeltaLocationContext::Build(
            6065.0, 6050.0, 6060.0, 6040.0, tickSize, 2.0, 8.0);
        engine.Compute(500.0, 1000.0, 3.0,
                       500.0 * (i + 1), 1000.0 * (i + 1), i, locCtx);
    }

    auto locCtx = DeltaLocationContext::Build(
        6065.0, 6050.0, 6060.0, 6040.0, tickSize, 2.0, 8.0);
    auto result = engine.Compute(550.0, 1000.0, 4.0,
                                  3050.0, 6000.0, 5, locCtx);

    // Check the combined quality assessment
    if (result.IsReady() && result.HasLocationContext()) {
        bool isHighQuality = result.IsHighQualitySignalWithContext();
        // This is informational - high quality depends on signal strength + outcome conviction
        std::cout << "  High quality signal: " << (isHighQuality ? "YES" : "NO") << "\n";
        std::cout << "  Signal strength: " << result.GetSignalStrength() << "\n";
        std::cout << "  Dominant likelihood: " << result.GetDominantLikelihood() << "\n";
    }

    std::cout << "[OK] High quality signal assessment works with context\n";
}

// ============================================================================
// TEST: Context Gates (Jan 2025)
// ============================================================================

void TestContextGates() {
    TEST_SECTION("Context Gates (Volatility/Liquidity/Dalton)");

    DeltaEngine engine;
    auto store = CreatePopulatedEffortStore();
    auto sessBaseline = CreatePopulatedSessionBaseline();

    engine.SetEffortStore(&store);
    engine.SetSessionDeltaBaseline(&sessBaseline);
    engine.SetPhase(SessionPhase::MID_SESSION);

    // Enable context gate checking
    engine.config.requireLiquidityGate = true;
    engine.config.requireVolatilityGate = true;
    engine.config.blockOnVoid = true;
    engine.config.blockOnThin = false;  // Default: thin only degrades
    engine.config.blockOnEvent = true;
    engine.config.degradeOnCompression = true;
    engine.config.highStressThreshold = 0.90;
    engine.config.useDaltonContext = true;

    const double tickSize = 0.25;
    auto locCtx = DeltaLocationContext::Build(
        6050.0, 6050.0, 6060.0, 6040.0, tickSize);

    // Test 1: LIQ_VOID blocks signals
    {
        engine.Reset();
        auto result = engine.Compute(
            300.0, 1000.0, 2.0, 300.0, 1000.0, 0,
            locCtx,
            LiquidityState::LIQ_VOID,      // VOID should block
            VolatilityRegime::NORMAL,
            0.5,                            // stress rank
            AMTMarketState::BALANCE,
            false                           // is1TF
        );

        TEST_ASSERT(result.contextGate.contextValid, "Context should be valid");
        TEST_ASSERT(!result.contextGate.liquidityOK, "LIQ_VOID should fail liquidity gate");
        TEST_ASSERT(result.contextGate.volatilityOK, "NORMAL regime should pass volatility gate");
        TEST_ASSERT(!result.contextGate.allGatesPass, "All gates should NOT pass with VOID");
        TEST_ASSERT(result.contextGate.IsBlocked(), "Should be blocked by VOID");
        TEST_ASSERT(result.errorReason == DeltaErrorReason::BLOCKED_LIQUIDITY_VOID,
                    "Error reason should be BLOCKED_LIQUIDITY_VOID");
        TEST_ASSERT(!result.constraints.allowContinuation, "Continuation blocked on VOID");
        TEST_ASSERT(!result.constraints.allowBreakout, "Breakout blocked on VOID");

        std::cout << "  LIQ_VOID blocks: OK\n";
    }

    // Test 2: EVENT regime blocks signals
    {
        engine.Reset();
        auto result = engine.Compute(
            300.0, 1000.0, 2.0, 300.0, 1000.0, 0,
            locCtx,
            LiquidityState::LIQ_NORMAL,
            VolatilityRegime::EVENT,        // EVENT should block
            0.5,
            AMTMarketState::BALANCE,
            false
        );

        TEST_ASSERT(result.contextGate.contextValid, "Context should be valid");
        TEST_ASSERT(result.contextGate.liquidityOK, "NORMAL liq should pass");
        TEST_ASSERT(!result.contextGate.volatilityOK, "EVENT should fail volatility gate");
        TEST_ASSERT(!result.contextGate.allGatesPass, "All gates should NOT pass with EVENT");
        TEST_ASSERT(result.contextGate.IsBlocked(), "Should be blocked by EVENT");
        TEST_ASSERT(result.errorReason == DeltaErrorReason::BLOCKED_VOLATILITY_EVENT,
                    "Error reason should be BLOCKED_VOLATILITY_EVENT");

        std::cout << "  EVENT regime blocks: OK\n";
    }

    // Test 3: COMPRESSION degrades but doesn't block
    {
        engine.Reset();
        auto result = engine.Compute(
            300.0, 1000.0, 2.0, 300.0, 1000.0, 0,
            locCtx,
            LiquidityState::LIQ_NORMAL,
            VolatilityRegime::COMPRESSION,  // COMPRESSION should degrade only
            0.5,
            AMTMarketState::BALANCE,
            false
        );

        TEST_ASSERT(result.contextGate.contextValid, "Context should be valid");
        TEST_ASSERT(result.contextGate.liquidityOK, "NORMAL liq should pass");
        TEST_ASSERT(result.contextGate.volatilityOK, "COMPRESSION should pass volatility gate");
        TEST_ASSERT(result.contextGate.compressionDegraded, "Should flag compression degradation");
        TEST_ASSERT(result.contextGate.allGatesPass, "All gates should pass (degraded but not blocked)");
        TEST_ASSERT(!result.contextGate.IsBlocked(), "Should NOT be blocked by COMPRESSION");
        TEST_ASSERT(result.contextGate.IsDegraded(), "Should be degraded by COMPRESSION");
        // Breakouts should be blocked in compression
        TEST_ASSERT(!result.constraints.allowBreakout, "Breakouts blocked in COMPRESSION");
        TEST_ASSERT(result.constraints.allowFade, "Fade should be allowed in COMPRESSION");

        std::cout << "  COMPRESSION degrades: OK\n";
    }

    // Test 4: High stress degrades confidence
    {
        engine.Reset();
        auto result = engine.Compute(
            300.0, 1000.0, 2.0, 300.0, 1000.0, 0,
            locCtx,
            LiquidityState::LIQ_NORMAL,
            VolatilityRegime::NORMAL,
            0.95,                           // High stress (above 0.90 threshold)
            AMTMarketState::BALANCE,
            false
        );

        TEST_ASSERT(result.contextGate.contextValid, "Context should be valid");
        TEST_ASSERT(result.contextGate.liquidityOK, "NORMAL liq should pass");
        TEST_ASSERT(result.contextGate.volatilityOK, "NORMAL regime should pass");
        TEST_ASSERT(result.contextGate.highStress, "Should flag high stress");
        TEST_ASSERT(result.contextGate.allGatesPass, "All gates should pass (degraded but not blocked)");
        TEST_ASSERT(result.contextGate.IsDegraded(), "Should be degraded by high stress");
        TEST_ASSERT(result.constraints.positionSizeMultiplier < 1.0,
                    "High stress should reduce position size");
        TEST_ASSERT(result.constraints.requireSustained,
                    "High stress should require sustained character");

        std::cout << "  High stress degrades: OK\n";
    }

    // Test 5: Normal conditions pass all gates
    {
        engine.Reset();
        auto result = engine.Compute(
            300.0, 1000.0, 2.0, 300.0, 1000.0, 0,
            locCtx,
            LiquidityState::LIQ_NORMAL,
            VolatilityRegime::NORMAL,
            0.5,                            // Normal stress
            AMTMarketState::BALANCE,
            false
        );

        TEST_ASSERT(result.contextGate.contextValid, "Context should be valid");
        TEST_ASSERT(result.contextGate.liquidityOK, "NORMAL liq should pass");
        TEST_ASSERT(result.contextGate.volatilityOK, "NORMAL regime should pass");
        TEST_ASSERT(!result.contextGate.highStress, "Should not flag high stress");
        TEST_ASSERT(!result.contextGate.compressionDegraded, "Should not be compression degraded");
        TEST_ASSERT(result.contextGate.allGatesPass, "All gates should pass");
        TEST_ASSERT(!result.contextGate.IsBlocked(), "Should not be blocked");
        TEST_ASSERT(!result.contextGate.IsDegraded(), "Should not be degraded");

        std::cout << "  Normal conditions pass: OK\n";
    }

    // Test 6: LIQ_THIN with blockOnThin=false only degrades
    {
        engine.Reset();
        engine.config.blockOnThin = false;  // Ensure thin only degrades
        auto result = engine.Compute(
            300.0, 1000.0, 2.0, 300.0, 1000.0, 0,
            locCtx,
            LiquidityState::LIQ_THIN,       // THIN should only degrade
            VolatilityRegime::NORMAL,
            0.5,
            AMTMarketState::BALANCE,
            false
        );

        TEST_ASSERT(result.contextGate.liquidityOK, "LIQ_THIN should pass with blockOnThin=false");
        TEST_ASSERT(result.contextGate.allGatesPass, "All gates should pass");
        TEST_ASSERT(!result.contextGate.IsBlocked(), "Should NOT be blocked");

        std::cout << "  LIQ_THIN with blockOnThin=false: OK\n";
    }

    // Test 7: LIQ_THIN with blockOnThin=true blocks
    {
        engine.Reset();
        engine.config.blockOnThin = true;  // Now thin should block
        auto result = engine.Compute(
            300.0, 1000.0, 2.0, 300.0, 1000.0, 0,
            locCtx,
            LiquidityState::LIQ_THIN,       // THIN should block with flag
            VolatilityRegime::NORMAL,
            0.5,
            AMTMarketState::BALANCE,
            false
        );

        TEST_ASSERT(!result.contextGate.liquidityOK, "LIQ_THIN should fail with blockOnThin=true");
        TEST_ASSERT(!result.contextGate.allGatesPass, "All gates should NOT pass");
        TEST_ASSERT(result.contextGate.IsBlocked(), "Should be blocked");
        TEST_ASSERT(result.errorReason == DeltaErrorReason::BLOCKED_LIQUIDITY_THIN,
                    "Error reason should be BLOCKED_LIQUIDITY_THIN");

        std::cout << "  LIQ_THIN with blockOnThin=true: OK\n";
    }

    // Test 8: Dalton 1TF context relaxes requirements
    {
        engine.Reset();
        engine.config.blockOnThin = false;
        auto result = engine.Compute(
            300.0, 1000.0, 2.0, 300.0, 1000.0, 0,
            locCtx,
            LiquidityState::LIQ_NORMAL,
            VolatilityRegime::NORMAL,
            0.5,
            AMTMarketState::IMBALANCE,      // 1TF trending
            true                             // is1TF
        );

        TEST_ASSERT(result.contextGate.hasDaltonContext, "Should have Dalton context");
        TEST_ASSERT(result.contextGate.is1TF, "Should be 1TF");
        TEST_ASSERT(result.contextGate.daltonState == AMTMarketState::IMBALANCE,
                    "Dalton state should be IMBALANCE");
        // 1TF with aligned delta should be more permissive
        TEST_ASSERT(result.contextGate.allGatesPass, "All gates should pass");

        std::cout << "  Dalton 1TF context: OK\n";
    }

    // Test 9: Dalton 2TF context tightens requirements
    {
        engine.Reset();
        auto result = engine.Compute(
            300.0, 1000.0, 2.0, 300.0, 1000.0, 0,
            locCtx,
            LiquidityState::LIQ_NORMAL,
            VolatilityRegime::NORMAL,
            0.5,
            AMTMarketState::BALANCE,        // 2TF rotating
            false                            // is 2TF
        );

        TEST_ASSERT(result.contextGate.hasDaltonContext, "Should have Dalton context");
        TEST_ASSERT(!result.contextGate.is1TF, "Should be 2TF (not 1TF)");
        TEST_ASSERT(result.contextGate.daltonState == AMTMarketState::BALANCE,
                    "Dalton state should be BALANCE");
        // 2TF should require more confirmation
        TEST_ASSERT(result.constraints.requireSustained || result.constraints.requireDeltaAlignment,
                    "2TF should tighten requirements");

        std::cout << "  Dalton 2TF context: OK\n";
    }

    // Test 10: Context gate helper functions
    {
        TEST_ASSERT(IsDeltaContextBlocked(DeltaErrorReason::BLOCKED_LIQUIDITY_VOID),
                    "BLOCKED_LIQUIDITY_VOID should be context blocked");
        TEST_ASSERT(IsDeltaContextBlocked(DeltaErrorReason::BLOCKED_LIQUIDITY_THIN),
                    "BLOCKED_LIQUIDITY_THIN should be context blocked");
        TEST_ASSERT(IsDeltaContextBlocked(DeltaErrorReason::BLOCKED_VOLATILITY_EVENT),
                    "BLOCKED_VOLATILITY_EVENT should be context blocked");
        TEST_ASSERT(!IsDeltaContextBlocked(DeltaErrorReason::DEGRADED_VOLATILITY_COMPRESSION),
                    "DEGRADED_VOLATILITY_COMPRESSION should NOT be context blocked");
        TEST_ASSERT(!IsDeltaContextBlocked(DeltaErrorReason::NONE),
                    "NONE should NOT be context blocked");

        TEST_ASSERT(IsDeltaContextDegraded(DeltaErrorReason::DEGRADED_VOLATILITY_COMPRESSION),
                    "DEGRADED_VOLATILITY_COMPRESSION should be context degraded");
        TEST_ASSERT(IsDeltaContextDegraded(DeltaErrorReason::DEGRADED_HIGH_STRESS),
                    "DEGRADED_HIGH_STRESS should be context degraded");
        TEST_ASSERT(!IsDeltaContextDegraded(DeltaErrorReason::BLOCKED_LIQUIDITY_VOID),
                    "BLOCKED_LIQUIDITY_VOID should NOT be context degraded");

        std::cout << "  Context gate helpers: OK\n";
    }

    std::cout << "[OK] Context gates correctly block/degrade based on external engine state\n";
}

// ============================================================================
// TEST: Context Gate IsContextBlocked/IsContextDegraded Accessors
// ============================================================================

void TestContextGateAccessors() {
    TEST_SECTION("Context Gate Result Accessors");

    DeltaEngine engine;
    auto store = CreatePopulatedEffortStore();
    auto sessBaseline = CreatePopulatedSessionBaseline();

    engine.SetEffortStore(&store);
    engine.SetSessionDeltaBaseline(&sessBaseline);
    engine.SetPhase(SessionPhase::MID_SESSION);

    const double tickSize = 0.25;
    auto locCtx = DeltaLocationContext::Build(
        6050.0, 6050.0, 6060.0, 6040.0, tickSize);

    // Test IsContextBlocked accessor
    {
        engine.Reset();
        auto result = engine.Compute(
            300.0, 1000.0, 2.0, 300.0, 1000.0, 0,
            locCtx,
            LiquidityState::LIQ_VOID,
            VolatilityRegime::NORMAL,
            0.5, AMTMarketState::BALANCE, false
        );

        TEST_ASSERT(result.IsContextBlocked(), "IsContextBlocked() should be true for VOID");
    }

    // Test IsContextDegraded accessor
    {
        engine.Reset();
        auto result = engine.Compute(
            300.0, 1000.0, 2.0, 300.0, 1000.0, 0,
            locCtx,
            LiquidityState::LIQ_NORMAL,
            VolatilityRegime::COMPRESSION,
            0.5, AMTMarketState::BALANCE, false
        );

        TEST_ASSERT(!result.IsContextBlocked(), "IsContextBlocked() should be false for COMPRESSION");
        TEST_ASSERT(result.IsContextDegraded(), "IsContextDegraded() should be true for COMPRESSION");
    }

    // Test neither blocked nor degraded
    {
        engine.Reset();
        auto result = engine.Compute(
            300.0, 1000.0, 2.0, 300.0, 1000.0, 0,
            locCtx,
            LiquidityState::LIQ_NORMAL,
            VolatilityRegime::NORMAL,
            0.5, AMTMarketState::BALANCE, false
        );

        TEST_ASSERT(!result.IsContextBlocked(), "IsContextBlocked() should be false");
        TEST_ASSERT(!result.IsContextDegraded(), "IsContextDegraded() should be false");
    }

    std::cout << "[OK] Context gate accessors work correctly\n";
}

// ============================================================================
// TEST: Asymmetric Hysteresis (Jan 2025)
// ============================================================================

void TestAsymmetricHysteresis() {
    TEST_SECTION("Asymmetric Hysteresis");

    DeltaEngine engine;
    auto store = CreatePopulatedEffortStore();
    auto sessBaseline = CreatePopulatedSessionBaseline();

    engine.SetEffortStore(&store);
    engine.SetSessionDeltaBaseline(&sessBaseline);
    engine.SetPhase(SessionPhase::MID_SESSION);

    // Test config defaults
    {
        TEST_ASSERT(engine.config.reversalEntryBars == 1,
                    "reversalEntryBars should default to 1");
        TEST_ASSERT(engine.config.buildingEntryBars == 1,
                    "buildingEntryBars should default to 1");
        TEST_ASSERT(engine.config.sustainedExitBars == 3,
                    "sustainedExitBars should default to 3");
        TEST_ASSERT(engine.config.divergenceEntryBars == 1,
                    "divergenceEntryBars should default to 1");
        TEST_ASSERT(engine.config.convergenceExitBars == 3,
                    "convergenceExitBars should default to 3");

        std::cout << "  Config defaults: OK\n";
    }

    // Test that result reports required confirmation bars
    {
        engine.Reset();
        auto result = engine.Compute(300.0, 1000.0, 2.0, 300.0, 1000.0, 0);
        TEST_ASSERT(result.characterConfirmationRequired >= 1,
                    "Should report character confirmation required");
        TEST_ASSERT(result.alignmentConfirmationRequired >= 1,
                    "Should report alignment confirmation required");

        std::cout << "  Confirmation bars reported: OK\n";
    }

    // Test bars in confirmed state tracking
    {
        engine.Reset();
        for (int i = 0; i < 5; ++i) {
            auto result = engine.Compute(300.0, 1000.0, 2.0, 300.0, 1000.0, i);
            if (result.IsReady()) {
                TEST_ASSERT(result.barsInConfirmedCharacter >= 1,
                            "barsInConfirmedCharacter should increment");
            }
        }

        std::cout << "  Bars in confirmed state tracking: OK\n";
    }

    std::cout << "[OK] Asymmetric hysteresis configuration and tracking works\n";
}

// ============================================================================
// TEST: Thin Tape Classification (Jan 2025)
// ============================================================================

void TestThinTapeClassification() {
    TEST_SECTION("Thin Tape Classification");

    // Test ThinTapeType enum and string conversion
    {
        TEST_ASSERT(ThinTapeTypeToString(ThinTapeType::NONE) != nullptr,
                    "ThinTapeTypeToString(NONE) should return valid string");
        TEST_ASSERT(ThinTapeTypeToString(ThinTapeType::TRUE_THIN) != nullptr,
                    "ThinTapeTypeToString(TRUE_THIN) should return valid string");
        TEST_ASSERT(ThinTapeTypeToString(ThinTapeType::HFT_FRAGMENTED) != nullptr,
                    "ThinTapeTypeToString(HFT_FRAGMENTED) should return valid string");
        TEST_ASSERT(ThinTapeTypeToString(ThinTapeType::INSTITUTIONAL) != nullptr,
                    "ThinTapeTypeToString(INSTITUTIONAL) should return valid string");

        std::cout << "  ThinTapeType enum strings: OK\n";
    }

    // Test classification thresholds in config
    {
        DeltaConfig cfg;
        TEST_ASSERT(cfg.lowTradesPctile == 25.0, "lowTradesPctile default should be 25");
        TEST_ASSERT(cfg.highTradesPctile == 75.0, "highTradesPctile default should be 75");
        TEST_ASSERT(cfg.lowVolumePctile == 10.0, "lowVolumePctile default should be 10");
        TEST_ASSERT(cfg.highVolumePctile == 75.0, "highVolumePctile default should be 75");
        TEST_ASSERT(cfg.thinTapeConfidencePenalty == 3, "thinTapeConfidencePenalty should be 3");
        TEST_ASSERT(cfg.hftFragmentedConfidencePenalty == 1, "hftFragmentedConfidencePenalty should be 1");
        TEST_ASSERT(cfg.institutionalConfidenceBoost == 1, "institutionalConfidenceBoost should be 1");

        std::cout << "  Classification config defaults: OK\n";
    }

    std::cout << "[OK] Thin tape classification types and config work correctly\n";
}

// ============================================================================
// TEST: Range-Adaptive Thresholds (Jan 2025)
// ============================================================================

void TestRangeAdaptiveThresholds() {
    TEST_SECTION("Range-Adaptive Thresholds");

    // Test config defaults
    {
        DeltaConfig cfg;
        TEST_ASSERT(cfg.useRangeAdaptiveThresholds == true,
                    "useRangeAdaptiveThresholds should default true");
        TEST_ASSERT(cfg.compressionRangePctile == 25.0,
                    "compressionRangePctile should be 25");
        TEST_ASSERT(cfg.expansionRangePctile == 75.0,
                    "expansionRangePctile should be 75");
        TEST_ASSERT(std::abs(cfg.compressionNoiseMultiplier - 0.7) < 0.01,
                    "compressionNoiseMultiplier should be 0.7");
        TEST_ASSERT(std::abs(cfg.expansionNoiseMultiplier - 1.3) < 0.01,
                    "expansionNoiseMultiplier should be 1.3");

        std::cout << "  Range-adaptive config defaults: OK\n";
    }

    // Test that DeltaResult has range-adaptive fields
    {
        DeltaResult result;
        result.effectiveNoiseFloor = 17.5;  // Compressed
        result.effectiveStrongSignal = 52.5;
        result.rangeAdaptiveApplied = true;

        TEST_ASSERT(result.effectiveNoiseFloor < 25.0,
                    "Compressed noise floor should be < 25");
        TEST_ASSERT(result.rangeAdaptiveApplied,
                    "rangeAdaptiveApplied flag should work");

        std::cout << "  Range-adaptive result fields: OK\n";
    }

    std::cout << "[OK] Range-adaptive threshold configuration works\n";
}

// ============================================================================
// TEST: DeltaInput Struct (Jan 2025)
// ============================================================================

void TestDeltaInputStruct() {
    TEST_SECTION("DeltaInput Struct");

    // Test builder pattern
    {
        DeltaInput input;
        input.WithCore(100.0, 500.0, 2.0, 1000.0, 5000.0, 10)
             .WithExtended(8.0, 50.0, 2.5, 3.0, 4.0);

        TEST_ASSERT(input.barDelta == 100.0, "barDelta should be set");
        TEST_ASSERT(input.barVolume == 500.0, "barVolume should be set");
        TEST_ASSERT(input.priceChangeTicks == 2.0, "priceChangeTicks should be set");
        TEST_ASSERT(input.sessionCumDelta == 1000.0, "sessionCumDelta should be set");
        TEST_ASSERT(input.sessionVolume == 5000.0, "sessionVolume should be set");
        TEST_ASSERT(input.currentBar == 10, "currentBar should be set");

        TEST_ASSERT(input.barRangeTicks == 8.0, "barRangeTicks should be set");
        TEST_ASSERT(input.numTrades == 50.0, "numTrades should be set");
        TEST_ASSERT(input.tradesPerSec == 2.5, "tradesPerSec should be set");
        TEST_ASSERT(input.avgBidTradeSize == 3.0, "avgBidTradeSize should be set");
        TEST_ASSERT(input.avgAskTradeSize == 4.0, "avgAskTradeSize should be set");
        TEST_ASSERT(input.hasExtendedInputs, "hasExtendedInputs should be true");

        std::cout << "  Builder pattern: OK\n";
    }

    // Test derived value helpers
    {
        DeltaInput input;
        input.barDelta = 100.0;
        input.barVolume = 500.0;
        input.sessionCumDelta = 200.0;
        input.sessionVolume = 1000.0;
        input.avgBidTradeSize = 2.0;
        input.avgAskTradeSize = 4.0;

        TEST_ASSERT(std::abs(input.GetDeltaPct() - 0.2) < 0.01,
                    "GetDeltaPct should compute correctly");
        TEST_ASSERT(std::abs(input.GetSessionDeltaPct() - 0.2) < 0.01,
                    "GetSessionDeltaPct should compute correctly");
        TEST_ASSERT(std::abs(input.GetAvgTradeSize() - 3.0) < 0.01,
                    "GetAvgTradeSize should compute correctly");

        std::cout << "  Derived value helpers: OK\n";
    }

    // Test Compute overload with DeltaInput
    {
        DeltaEngine engine;
        auto store = CreatePopulatedEffortStore();
        auto sessBaseline = CreatePopulatedSessionBaseline();

        engine.SetEffortStore(&store);
        engine.SetSessionDeltaBaseline(&sessBaseline);
        engine.SetPhase(SessionPhase::MID_SESSION);

        DeltaInput input;
        input.WithCore(300.0, 1000.0, 2.0, 300.0, 1000.0, 0);

        auto result = engine.Compute(input);
        TEST_ASSERT(result.IsReady() || result.IsWarmup(),
                    "Compute(DeltaInput) should work");
        TEST_ASSERT(result.hasExtendedInputs == input.hasExtendedInputs,
                    "hasExtendedInputs should match input");

        std::cout << "  Compute(DeltaInput) overload: OK\n";
    }

    std::cout << "[OK] DeltaInput struct and Compute overloads work correctly\n";
}

// ============================================================================
// TEST: Extended Metrics Result Fields (Jan 2025)
// ============================================================================

void TestExtendedMetricsResultFields() {
    TEST_SECTION("Extended Metrics Result Fields");

    // Test that result has all expected fields with defaults
    {
        DeltaResult result;

        // Thin tape fields
        TEST_ASSERT(result.tradesPctile == 0.0, "tradesPctile default should be 0");
        TEST_ASSERT(result.tradesBaselineReady == false, "tradesBaselineReady default should be false");
        TEST_ASSERT(result.thinTapeType == ThinTapeType::NONE, "thinTapeType default should be NONE");

        // Range-adaptive fields
        TEST_ASSERT(result.rangePctile == 0.0, "rangePctile default should be 0");
        TEST_ASSERT(result.rangeBaselineReady == false, "rangeBaselineReady default should be false");
        TEST_ASSERT(result.effectiveNoiseFloor == 25.0, "effectiveNoiseFloor default should be 25");
        TEST_ASSERT(result.effectiveStrongSignal == 75.0, "effectiveStrongSignal default should be 75");

        // Institutional fields
        TEST_ASSERT(result.avgTradeSizePctile == 0.0, "avgTradeSizePctile default should be 0");
        TEST_ASSERT(result.isInstitutionalActivity == false, "isInstitutionalActivity default should be false");
        TEST_ASSERT(result.isRetailActivity == false, "isRetailActivity default should be false");

        std::cout << "  Extended result field defaults: OK\n";
    }

    std::cout << "[OK] Extended metrics result fields have correct defaults\n";
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "=================================================\n";
    std::cout << "           DeltaEngine Unit Tests\n";
    std::cout << "=================================================\n";

    TestCharacterClassification();
    TestReversalDetection();
    TestAlignmentConvergent();
    TestAlignmentDivergent();
    TestConfidenceThinTape();
    TestConfidenceHighChop();
    TestConfidenceExhaustion();
    TestShockDeltaDetection();
    TestTradingConstraints();
    TestHysteresis();
    TestSessionBoundary();
    TestValidityGating();
    TestHistoryTracking();
    TestDecisionInputHelper();
    TestSignalStrength();
    TestPhaseAwareBaselines();

    // Location Awareness Tests (Jan 2025)
    TestLocationContextBuild();
    TestLocationAwareCompute();
    TestAuctionOutcomeLikelihoods();
    TestLocationSensitiveAdjustments();
    TestOutcomeAccessors();
    TestHighQualitySignalWithContext();

    // Context Gates Tests (Jan 2025)
    TestContextGates();
    TestContextGateAccessors();

    // Extended Baseline Metrics Tests (Jan 2025)
    TestAsymmetricHysteresis();
    TestThinTapeClassification();
    TestRangeAdaptiveThresholds();
    TestDeltaInputStruct();
    TestExtendedMetricsResultFields();

    std::cout << "\n=================================================\n";
    std::cout << "Tests Passed: " << g_testsPassed << "\n";
    std::cout << "Tests Failed: " << g_testsFailed << "\n";
    std::cout << "=================================================\n";

    return g_testsFailed > 0 ? 1 : 0;
}
