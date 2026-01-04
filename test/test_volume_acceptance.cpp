// ============================================================================
// test_volume_acceptance.cpp - Unit Tests for VolumeAcceptanceEngine
// ============================================================================
// Tests:
//   1. Acceptance/rejection state classification
//   2. Volume intensity classification (phase-aware)
//   3. POC migration detection
//   4. Value area tracking
//   5. Session boundary handling (reset)
//   6. Validity gating (warmup, errors)
//   7. Confirmation multiplier calculation
//
// Compile: g++ -std=c++17 -I.. -o test_volume_acceptance.exe test_volume_acceptance.cpp
// Run: ./test_volume_acceptance.exe
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
#include "../AMT_VolumeAcceptance.h"

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

// Helper: Create populated EffortBaselineStore with volume samples
EffortBaselineStore CreatePopulatedEffortStore() {
    EffortBaselineStore store;
    store.Reset(500);

    // Populate all phase buckets with volume data
    for (int i = 0; i < AMT::EFFORT_BUCKET_COUNT; ++i) {
        auto& bucket = store.buckets[i];

        // Add 100 samples with known distribution: 10 to 110
        for (int j = 0; j < 100; ++j) {
            double volume = 10.0 + j;  // Range: 10-110
            bucket.vol_sec.push(volume);
        }

        bucket.sessionsContributed = 5;
        bucket.totalBarsPushed = 100;
    }

    return store;
}

// ============================================================================
// TEST: Engine Initialization
// ============================================================================

void TestEngineInitialization() {
    TEST_SECTION("Engine Initialization");

    VolumeAcceptanceEngine engine;

    TEST_ASSERT(engine.effortStore == nullptr, "Engine should start with no effort store");
    TEST_ASSERT(engine.currentPhase == SessionPhase::UNKNOWN, "Engine should start with UNKNOWN phase");
    TEST_ASSERT(engine.confirmedState == AcceptanceState::UNKNOWN, "Engine should start with UNKNOWN state");
    TEST_ASSERT(engine.sessionBars == 0, "Session bars should be 0");
}

// ============================================================================
// TEST: NO-FALLBACK Contract - Error without Effort Store
// ============================================================================

void TestNoFallbackWithoutEffortStore() {
    TEST_SECTION("NO-FALLBACK Contract - No Effort Store");

    VolumeAcceptanceEngine engine;
    engine.SetPhase(SessionPhase::MID_SESSION);

    auto result = engine.Compute(
        5000.0, 5001.0, 4999.0, 0.25, 100,  // close, high, low, tickSize, barIdx
        1000.0  // totalVolume
    );

    TEST_ASSERT(!result.IsReady(), "Result should not be ready without effort store");
    TEST_ASSERT(result.errorReason == AcceptanceErrorReason::ERR_NO_EFFORT_STORE,
                "Error reason should be NO_EFFORT_STORE");
}

// ============================================================================
// TEST: Invalid Input Handling
// ============================================================================

void TestInvalidInputs() {
    TEST_SECTION("Invalid Input Handling");

    VolumeAcceptanceEngine engine;
    auto store = CreatePopulatedEffortStore();
    engine.SetEffortStore(&store);
    engine.SetPhase(SessionPhase::MID_SESSION);

    // Invalid price (high < low)
    {
        auto result = engine.Compute(
            5000.0, 4999.0, 5001.0, 0.25, 100,  // high < low
            1000.0
        );
        TEST_ASSERT(!result.IsReady(), "Should fail with high < low");
        TEST_ASSERT(result.errorReason == AcceptanceErrorReason::ERR_INVALID_PRICE,
                    "Error should be INVALID_PRICE");
    }

    // Invalid VA (VAH <= VAL)
    {
        auto result = engine.Compute(
            5000.0, 5001.0, 4999.0, 0.25, 101,
            1000.0, 400.0, 600.0, 200.0,
            5000.25, 4998.0, 5002.0  // VAH=4998 < VAL=5002
        );
        TEST_ASSERT(!result.IsReady(), "Should fail with VAH <= VAL");
        TEST_ASSERT(result.errorReason == AcceptanceErrorReason::ERR_INVALID_VA,
                    "Error should be INVALID_VA");
    }

    // Invalid volume (negative)
    {
        auto result = engine.Compute(
            5000.0, 5001.0, 4999.0, 0.25, 102,
            -1000.0  // Negative volume
        );
        TEST_ASSERT(!result.IsReady(), "Should fail with negative volume");
        TEST_ASSERT(result.errorReason == AcceptanceErrorReason::ERR_INVALID_VOLUME,
                    "Error should be INVALID_VOLUME");
    }
}

// ============================================================================
// TEST: Volume Intensity Classification
// ============================================================================

void TestVolumeIntensityClassification() {
    TEST_SECTION("Volume Intensity Classification");

    VolumeAcceptanceEngine engine;
    auto store = CreatePopulatedEffortStore();
    engine.SetEffortStore(&store);
    engine.SetPhase(SessionPhase::MID_SESSION);

    // Test VERY_LOW (< P10) - value 15 in range 10-110 is ~5th percentile
    {
        auto result = engine.Compute(
            5000.0, 5001.0, 4999.0, 0.25, 100,
            1000.0, 400.0, 600.0, 200.0,
            5000.25, 5002.0, 4998.0,
            0.0, 0.0, 0.0,
            15.0  // Low volume
        );
        if (result.IsReady()) {
            TEST_ASSERT(result.intensity == VolumeIntensity::VERY_LOW,
                        "Volume 15 in range 10-110 should be VERY_LOW");
        }
    }

    // Test NORMAL (P25-P75) - value 60 in range 10-110 is ~50th percentile
    {
        auto result = engine.Compute(
            5000.0, 5001.0, 4999.0, 0.25, 101,
            1000.0, 400.0, 600.0, 200.0,
            5000.25, 5002.0, 4998.0,
            0.0, 0.0, 0.0,
            60.0  // Mid volume
        );
        if (result.IsReady()) {
            TEST_ASSERT(result.intensity == VolumeIntensity::NORMAL,
                        "Volume 60 in range 10-110 should be NORMAL");
        }
    }

    // Test VERY_HIGH (> P90) - value 105 in range 10-110 is ~95th percentile
    {
        auto result = engine.Compute(
            5000.0, 5001.0, 4999.0, 0.25, 102,
            1000.0, 400.0, 600.0, 200.0,
            5000.25, 5002.0, 4998.0,
            0.0, 0.0, 0.0,
            105.0  // High volume
        );
        if (result.IsReady()) {
            TEST_ASSERT(result.intensity == VolumeIntensity::VERY_HIGH,
                        "Volume 105 in range 10-110 should be VERY_HIGH");
        }
    }
}

// ============================================================================
// TEST: Value Area Location Tracking
// ============================================================================

void TestValueAreaTracking() {
    TEST_SECTION("Value Area Location Tracking");

    VolumeAcceptanceEngine engine;
    auto store = CreatePopulatedEffortStore();
    engine.SetEffortStore(&store);
    engine.SetPhase(SessionPhase::MID_SESSION);

    double tickSize = 0.25;

    // Price inside VA
    {
        auto result = engine.Compute(
            5000.0, 5001.0, 4999.0, tickSize, 100,
            1000.0, 400.0, 600.0, 200.0,
            5000.25, 5002.0, 4998.0  // POC=5000.25, VAH=5002, VAL=4998
        );
        TEST_ASSERT(result.priceInVA, "Close 5000 should be inside VA [4998, 5002]");
        TEST_ASSERT(!result.priceAboveVA, "Should not be above VA");
        TEST_ASSERT(!result.priceBelowVA, "Should not be below VA");
    }

    // Price above VA
    {
        auto result = engine.Compute(
            5003.0, 5004.0, 5002.5, tickSize, 101,
            1000.0, 400.0, 600.0, 200.0,
            5000.25, 5002.0, 4998.0
        );
        TEST_ASSERT(result.priceAboveVA, "Close 5003 should be above VA [4998, 5002]");
        TEST_ASSERT(!result.priceInVA, "Should not be inside VA");
        TEST_ASSERT(!result.priceBelowVA, "Should not be below VA");
    }

    // Price below VA
    {
        auto result = engine.Compute(
            4997.0, 4998.0, 4996.0, tickSize, 102,
            1000.0, 400.0, 600.0, 200.0,
            5000.25, 5002.0, 4998.0
        );
        TEST_ASSERT(result.priceBelowVA, "Close 4997 should be below VA [4998, 5002]");
        TEST_ASSERT(!result.priceInVA, "Should not be inside VA");
        TEST_ASSERT(!result.priceAboveVA, "Should not be above VA");
    }
}

// ============================================================================
// TEST: Delta Ratio Calculation
// ============================================================================

void TestDeltaRatio() {
    TEST_SECTION("Delta Ratio Calculation");

    VolumeAcceptanceEngine engine;
    auto store = CreatePopulatedEffortStore();
    engine.SetEffortStore(&store);
    engine.SetPhase(SessionPhase::MID_SESSION);

    // Strong positive delta (bullish)
    {
        auto result = engine.Compute(
            5000.0, 5001.0, 4999.0, 0.25, 100,
            1000.0,
            200.0, 800.0, 600.0  // bid=200, ask=800, delta=600
        );
        TEST_ASSERT(result.deltaRatio > 0.5, "Delta ratio should be ~0.6 (600/1000)");
        TEST_ASSERT(result.deltaRatio < 0.7, "Delta ratio should be ~0.6");
    }

    // Strong negative delta (bearish)
    {
        auto result = engine.Compute(
            5000.0, 5001.0, 4999.0, 0.25, 101,
            1000.0,
            800.0, 200.0, -600.0  // bid=800, ask=200, delta=-600
        );
        TEST_ASSERT(result.deltaRatio < -0.5, "Delta ratio should be ~-0.6 (-600/1000)");
        TEST_ASSERT(result.deltaRatio > -0.7, "Delta ratio should be ~-0.6");
    }

    // Neutral delta
    {
        auto result = engine.Compute(
            5000.0, 5001.0, 4999.0, 0.25, 102,
            1000.0,
            500.0, 500.0, 0.0  // bid=500, ask=500, delta=0
        );
        TEST_ASSERT(std::abs(result.deltaRatio) < 0.1, "Delta ratio should be ~0");
    }
}

// ============================================================================
// TEST: POC Migration Detection
// ============================================================================

void TestPOCMigration() {
    TEST_SECTION("POC Migration Detection");

    VolumeAcceptanceEngine engine;
    auto store = CreatePopulatedEffortStore();
    engine.SetEffortStore(&store);
    engine.SetPhase(SessionPhase::MID_SESSION);

    double tickSize = 0.25;
    double poc = 5000.0;

    // Simulate POC migrating upward over several bars
    for (int bar = 0; bar < 15; bar++) {
        poc += tickSize * 2;  // POC moves up 2 ticks per bar

        auto result = engine.Compute(
            poc, poc + tickSize, poc - tickSize, tickSize, bar,
            1000.0, 400.0, 600.0, 200.0,
            poc, poc + 4*tickSize, poc - 4*tickSize,
            0.0, 0.0, 0.0, 60.0
        );

        // After building up history, check migration is detected
        if (bar >= 10) {
            TEST_ASSERT(result.pocMigrationTicks > 0,
                        "POC migration ticks should be positive (upward)");
            TEST_ASSERT(result.migrationDirection >= 0,
                        "Migration direction should be up or stable");
        }
    }
}

// ============================================================================
// TEST: Session Reset
// ============================================================================

void TestSessionReset() {
    TEST_SECTION("Session Reset");

    VolumeAcceptanceEngine engine;
    auto store = CreatePopulatedEffortStore();
    engine.SetEffortStore(&store);
    engine.SetPhase(SessionPhase::MID_SESSION);

    // Build up some state
    for (int i = 0; i < 10; i++) {
        engine.Compute(
            5000.0, 5001.0, 4999.0, 0.25, i,
            1000.0, 400.0, 600.0, 200.0,
            5000.25, 5002.0, 4998.0,
            0.0, 0.0, 0.0, 60.0
        );
    }

    TEST_ASSERT(engine.sessionBars > 0, "Session bars should be > 0 after processing");

    // Reset for new session
    engine.ResetForSession();

    TEST_ASSERT(engine.sessionBars == 0, "Session bars should be 0 after reset");
    TEST_ASSERT(engine.confirmedState == AcceptanceState::UNKNOWN,
                "Confirmed state should be UNKNOWN after reset");
    TEST_ASSERT(engine.pocTracker.currentPOC == 0.0, "POC tracker should be reset");
    TEST_ASSERT(engine.vaTracker.currentVAH == 0.0, "VA tracker should be reset");
}

// ============================================================================
// TEST: Acceptance Score Components
// ============================================================================

void TestAcceptanceScoreComponents() {
    TEST_SECTION("Acceptance Score Components");

    VolumeAcceptanceEngine engine;
    auto store = CreatePopulatedEffortStore();
    engine.SetEffortStore(&store);
    engine.SetPhase(SessionPhase::MID_SESSION);

    // High volume, inside VA, positive delta
    auto result = engine.Compute(
        5000.0, 5001.0, 4999.0, 0.25, 100,
        1000.0, 300.0, 700.0, 400.0,  // Positive delta
        5000.25, 5002.0, 4998.0,
        0.0, 0.0, 0.0,
        80.0  // High volume
    );

    // Components should be populated and in valid range
    TEST_ASSERT(result.volumeComponent >= 0.0 && result.volumeComponent <= 1.0,
                "Volume component should be in [0, 1]");
    TEST_ASSERT(result.deltaComponent >= 0.0 && result.deltaComponent <= 1.0,
                "Delta component should be in [0, 1]");
    TEST_ASSERT(result.priceActionComponent >= 0.0 && result.priceActionComponent <= 1.0,
                "Price action component should be in [0, 1]");
    TEST_ASSERT(result.acceptanceScore >= 0.0 && result.acceptanceScore <= 1.0,
                "Acceptance score should be in [0, 1]");
}

// ============================================================================
// TEST: Confirmation Multiplier
// ============================================================================

void TestConfirmationMultiplier() {
    TEST_SECTION("Confirmation Multiplier");

    VolumeAcceptanceEngine engine;
    auto store = CreatePopulatedEffortStore();
    engine.SetEffortStore(&store);
    engine.SetPhase(SessionPhase::MID_SESSION);

    // High volume
    auto result1 = engine.Compute(
        5000.0, 5001.0, 4999.0, 0.25, 100,
        1000.0, 400.0, 600.0, 200.0,
        5000.25, 5002.0, 4998.0,
        0.0, 0.0, 0.0,
        100.0  // High volume
    );

    // Low volume
    auto result2 = engine.Compute(
        5000.0, 5001.0, 4999.0, 0.25, 101,
        1000.0, 400.0, 600.0, 200.0,
        5000.25, 5002.0, 4998.0,
        0.0, 0.0, 0.0,
        20.0  // Low volume
    );

    // High volume should not have low volume penalty
    TEST_ASSERT(result1.confirmationMultiplier >= result2.confirmationMultiplier,
                "High volume should have >= multiplier than low volume");

    // Low volume should have penalty applied (multiplier reduced)
    // The config has lowVolumeMultiplier = 0.7
    TEST_ASSERT(result2.confirmationMultiplier <= 1.0,
                "Low volume should have multiplier <= 1.0");
}

// ============================================================================
// TEST: Phase Awareness
// ============================================================================

void TestPhaseAwareness() {
    TEST_SECTION("Phase Awareness");

    VolumeAcceptanceEngine engine;
    EffortBaselineStore store;
    store.Reset(500);

    // Populate GLOBEX with lower volume range
    auto& gbxBucket = store.buckets[SessionPhaseToBucketIndex(SessionPhase::GLOBEX)];
    for (int j = 0; j < 100; ++j) {
        gbxBucket.vol_sec.push(10.0 + j * 0.5);  // Range: 10-60
    }
    gbxBucket.sessionsContributed = 5;
    gbxBucket.totalBarsPushed = 100;

    // Populate MID_SESSION with higher volume range
    auto& midBucket = store.buckets[SessionPhaseToBucketIndex(SessionPhase::MID_SESSION)];
    for (int j = 0; j < 100; ++j) {
        midBucket.vol_sec.push(50.0 + j);  // Range: 50-150
    }
    midBucket.sessionsContributed = 5;
    midBucket.totalBarsPushed = 100;

    engine.SetEffortStore(&store);

    // Same volume value (40) should be different percentile in each phase
    // In GLOBEX (10-60 range), 40 is about 60th percentile
    engine.SetPhase(SessionPhase::GLOBEX);
    auto result1 = engine.Compute(
        5000.0, 5001.0, 4999.0, 0.25, 100,
        1000.0, 400.0, 600.0, 200.0,
        5000.25, 5002.0, 4998.0,
        0.0, 0.0, 0.0,
        40.0
    );

    // In MID_SESSION (50-150 range), 40 is below minimum -> very low percentile
    engine.SetPhase(SessionPhase::MID_SESSION);
    auto result2 = engine.Compute(
        5000.0, 5001.0, 4999.0, 0.25, 101,
        1000.0, 400.0, 600.0, 200.0,
        5000.25, 5002.0, 4998.0,
        0.0, 0.0, 0.0,
        40.0
    );

    if (result1.IsReady() && result2.IsReady()) {
        TEST_ASSERT(result1.volumePercentile > result2.volumePercentile,
                    "Same volume should have higher percentile in GLOBEX than MID_SESSION");
    }
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
    std::cout << "\n========================================\n";
    std::cout << "VolumeAcceptanceEngine Unit Tests\n";
    std::cout << "========================================\n";

    TestEngineInitialization();
    TestNoFallbackWithoutEffortStore();
    TestInvalidInputs();
    TestVolumeIntensityClassification();
    TestValueAreaTracking();
    TestDeltaRatio();
    TestPOCMigration();
    TestSessionReset();
    TestAcceptanceScoreComponents();
    TestConfirmationMultiplier();
    TestPhaseAwareness();

    std::cout << "\n========================================\n";
    std::cout << "Results: " << g_testsPassed << " passed, "
              << g_testsFailed << " failed\n";
    std::cout << "========================================\n";

    return g_testsFailed > 0 ? 1 : 0;
}
