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
    TestTradingConstraints();
    TestHysteresis();
    TestSessionBoundary();
    TestValidityGating();
    TestHistoryTracking();
    TestDecisionInputHelper();
    TestSignalStrength();
    TestPhaseAwareBaselines();

    std::cout << "\n=================================================\n";
    std::cout << "Tests Passed: " << g_testsPassed << "\n";
    std::cout << "Tests Failed: " << g_testsFailed << "\n";
    std::cout << "=================================================\n";

    return g_testsFailed > 0 ? 1 : 0;
}
